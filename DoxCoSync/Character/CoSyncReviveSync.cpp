#include "CoSyncReviveSync.h"

#include "CoSyncRequirementsLog.h"
#include "ConsoleLogger.h"
#include "CoSyncPlayer.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncGameAPI.h"
#include "CoSyncOverlay.h"
#include "CoSyncVMFormArg.h"
#include "MinHook.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "GameData.h"
#include "PapyrusVM.h"
#include "PapyrusEvents.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mutex>
#include <vector>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <unordered_set>

extern PlayerCharacter* CoSyncGetPlayer();

namespace
{
    // Real vanilla Fallout4.esm forms, cross-checked by decoding the
    // reference "Follower Revive System" mod's VMAD property bindings on its
    // scBleedoutManager quest script (objFormat==2 object property = FormID
    // + Alias(int16) + Unused(int16)) and independently confirmed three ways:
    // Stimpak (0x00023736) matches its own ALCH record dump; HealthAV
    // (0x000002D4) matches this project's own kHealthAVI_FormID constant
    // elsewhere; UseStimpakOnTarget (0x000C30F9) matches the IDLE record
    // EDID '3rdPUseStimpakOnTarget' already visible in that mod's plugin.
    // These are master-file (Fallout4.esm) forms, so the FormIDs are stable
    // across any load order -- same basis as kHealthAVI_FormID.
    constexpr uint32_t kStimpakFormID           = 0x00023736u;
    constexpr uint32_t kUseStimpakOnTargetIdleID = 0x000C30F9u; // 3rdPUseStimpakOnTarget
    constexpr uint32_t kIdleStopIdleID          = 0x000E9855u;
    // Fallout4.esm's generic human stand-from-bleedout idle -- found by
    // scanning every IDLE record's EDID for get-up/bleedout-related names
    // (same technique used to decode FRS's own idles), not from FRS itself
    // (its downed actors are real, so vanilla bleedout recovery already
    // stands them up -- it never needed an explicit get-up call). Verified
    // to exist in Fallout4.esm at this FormID; not yet live-verified to look
    // right on a proxy.
    constexpr uint32_t kSharedGetUpIdleID       = 0x0002B089u; // SharedGetUp

    TESForm* ResolveFormChecked(uint32_t formID, UInt32 expectFormType, const char* what)
    {
        TESForm* form = LookupFormByID(formID);
        if (!form)
        {
            LOG_WARN("[ReviveSync] %s (0x%08X) not found", what, formID);
            return nullptr;
        }
        if (form->formType != expectFormType)
        {
            LOG_WARN("[ReviveSync] %s (0x%08X) resolved but formType=%u (expected %u)",
                what, formID, (unsigned)form->formType, (unsigned)expectFormType);
            return nullptr;
        }
        return form;
    }

    TESForm* GetStimpakForm()
    {
        static TESForm* s_form = nullptr;
        static bool      s_tried = false;
        if (!s_tried) { s_tried = true; s_form = ResolveFormChecked(kStimpakFormID, kFormType_ALCH, "Stimpak"); }
        return s_form;
    }

    TESForm* GetUseStimpakOnTargetIdle()
    {
        static TESForm* s_form = nullptr;
        static bool      s_tried = false;
        if (!s_tried) { s_tried = true; s_form = ResolveFormChecked(kUseStimpakOnTargetIdleID, kFormType_IDLE, "UseStimpakOnTarget idle"); }
        return s_form;
    }

    TESForm* GetIdleStopIdle()
    {
        static TESForm* s_form = nullptr;
        static bool      s_tried = false;
        if (!s_tried) { s_tried = true; s_form = ResolveFormChecked(kIdleStopIdleID, kFormType_IDLE, "IdleStop idle"); }
        return s_form;
    }

    TESForm* GetSharedGetUpIdle()
    {
        static TESForm* s_form = nullptr;
        static bool      s_tried = false;
        if (!s_tried) { s_tried = true; s_form = ResolveFormChecked(kSharedGetUpIdleID, kFormType_IDLE, "SharedGetUp idle"); }
        return s_form;
    }

    // Native inventory-count walk, same shape as CoSyncPlayerManager.cpp's
    // ProcessWeaponVerifyQueue read of actor->inventoryList -- SEH-guarded,
    // noinline, tolerates a null/empty list.
    __declspec(noinline) static int CountItemImpl(Actor* actor, uint32_t formID)
    {
        int total = 0;
        BGSInventoryList* invList = actor->inventoryList;
        if (!invList)
            return 0;
        for (UInt32 i = 0; i < invList->items.count; ++i)
        {
            const BGSInventoryItem& item = invList->items[i];
            if (!item.form || item.form->formID != formID)
                continue;
            if (item.stack)
            {
                item.stack->Visit([&total](BGSInventoryItem::Stack* s) {
                    total += s->count;
                    return true;
                });
            }
        }
        return total;
    }

    int CountItem(Actor* actor, uint32_t formID)
    {
        if (!actor)
            return 0;
        __try { return CountItemImpl(actor, formID); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // Actor.PlayIdleWithTarget(Idle akIdle, ObjectReference akTarget) --
    // native VM bridge, same CallFunctionNoWait/VMArray<VMVariable> pattern
    // already proven elsewhere in this codebase (e.g. CoSyncGameAPI::
    // InitNpcProxy's SendAnimationEvent call), using CoSyncVM::SetFormArg so
    // each form packs under its own runtime VM type instead of the generic
    // TESForm type-0 that silently no-ops (see CoSyncVMFormArg.h).
    void NativePlayIdleWithTarget(Actor* actor, TESForm* idle, Actor* target)
    {
        if (!actor || !idle || !target)
        {
            LOG_UPDATED_WARN("[ReviveSync] NativePlayIdleWithTarget skipped: actor=%p idle=%p target=%p",
                static_cast<void*>(actor), static_cast<void*>(idle), static_cast<void*>(target));
            return;
        }
        static const BSFixedString kFn("PlayIdleWithTarget");
        VMArray<VMVariable> args;
        { VMVariable v; CoSyncVM::SetFormArg(v, idle); args.Push(&v); }
        { VMVariable v; CoSyncVM::SetFormArg(v, static_cast<TESForm*>(target)); args.Push(&v); }
        CallFunctionNoWait(actor, kFn, args);
        LOG_UPDATED("[ReviveSync] TX PlayIdleWithTarget actor=%p idle=0x%08X target=%p",
            static_cast<void*>(actor), idle->formID, static_cast<void*>(target));
    }

    void NativePlayIdle(Actor* actor, TESForm* idle)
    {
        if (!actor || !idle)
        {
            LOG_UPDATED_WARN("[ReviveSync] NativePlayIdle skipped: actor=%p idle=%p",
                static_cast<void*>(actor), static_cast<void*>(idle));
            return;
        }
        static const BSFixedString kFn("PlayIdle");
        VMArray<VMVariable> args;
        { VMVariable v; CoSyncVM::SetFormArg(v, idle); args.Push(&v); }
        CallFunctionNoWait(actor, kFn, args);
        LOG_UPDATED("[ReviveSync] TX PlayIdle actor=%p idle=0x%08X", static_cast<void*>(actor), idle->formID);
    }

    // Actor.RemoveItem(Form akItemToRemove, int aiCount = 1, bool abSilent = false,
    // ObjectReference akOtherContainer = None) -- real signature confirmed
    // from the shipped Data\Scripts\Source\Base\ObjectReference.psc. The
    // 4-arg shape (including the destination-container arg CoSyncContainerSync.cpp
    // already documents) is required; a 3-arg call was the bug that made the
    // Stimpak never actually get consumed.
    void NativeRemoveOneStimpak(Actor* actor)
    {
        TESForm* stimpak = GetStimpakForm();
        if (!actor || !stimpak)
        {
            LOG_UPDATED_WARN("[ReviveSync] NativeRemoveOneStimpak skipped: actor=%p stimpak=%p",
                static_cast<void*>(actor), static_cast<void*>(stimpak));
            return;
        }
        static const BSFixedString kFn("RemoveItem");
        VMArray<VMVariable> args;
        { VMVariable v; CoSyncVM::SetFormArg(v, stimpak); args.Push(&v); }
        { SInt32 n = 1; VMVariable v; v.Set(&n); args.Push(&v); }
        { bool b = true; VMVariable v; v.Set(&b); args.Push(&v); }
        { VMVariable v; CoSyncVM::SetFormArg(v, nullptr); args.Push(&v); } // akOtherContainer = None
        CallFunctionNoWait(actor, kFn, args);
        LOG_UPDATED("[ReviveSync] TX RemoveItem actor=%p Stimpak x1", static_cast<void*>(actor));
    }

    double NowSeconds()
    {
        static double s_freq = [] {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return double(f.QuadPart);
        }();
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        return double(c.QuadPart) / s_freq;
    }

    struct ReviveOp
    {
        enum Type { Down, ReviveReq, Revived };
        Type     type = Down;
        uint32_t entityID = 0;
        uint32_t reviverEntityID = 0;
    };

    std::mutex            s_opMutex;
    std::vector<ReviveOp> s_pendingOps;

    // Set by ApplyRevivedToTarget when the local player is the one revived;
    // consumed exactly once by CoSynclocalplayer.cpp at its own AV-safety
    // debounce point. 0 = nothing pending (entityIDs are never 0 elsewhere in
    // this codebase, same convention used throughout).
    std::atomic<uint32_t> s_pendingLocalReviveReviverEID{ 0 };

    struct ReviveChannel
    {
        bool     active = false;
        uint32_t targetEntityID = 0;
        double   startTime = 0.0;
    };
    ReviveChannel s_channel;

    // 0.3.3 (Stage 6): refFormIDs of the proxies that are downed right now,
    // rebuilt every tick from the player manager (so a proxy rebuilt while
    // down keeps its prompt). Read by the activate-text hook, which may run
    // on an engine thread -- hence its own lock.
    std::mutex                   s_downedRefsMutex;
    std::unordered_set<uint32_t> s_downedRefs;

    bool IsDownedProxyRef(uint32_t refFormID)
    {
        std::lock_guard<std::mutex> lk(s_downedRefsMutex);
        return s_downedRefs.count(refFormID) != 0;
    }

    void RefreshDownedRefs()
    {
        std::unordered_set<uint32_t> now;
        for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
        {
            const CoSyncPlayer* p = kv.second.get();
            if (p && p->isDowned && p->actorRef && p->createType == CoSyncEntityType::Player)
                now.insert(p->actorRef->formID);
        }
        std::lock_guard<std::mutex> lk(s_downedRefsMutex);
        s_downedRefs.swap(now);
    }

    constexpr float kReviveMaxRange   = 350.0f; // channel cancels beyond this
    constexpr float kReviveMaxRangeSq = kReviveMaxRange * kReviveMaxRange;

    void CancelChannel(const char* reason)
    {
        if (!s_channel.active)
            return;
        LOG_INFO("[ReviveSync] Revive channel on entity=%u cancelled (%s)",
            s_channel.targetEntityID, reason);
        if (CoSyncPlayer* target = g_CoSyncPlayerManager.GetPlayerByEntityID(s_channel.targetEntityID))
            target->m_reviverEntityID = 0;
        s_channel = ReviveChannel{};
    }

    void ApplyRevivedToTarget(uint32_t entityID, uint32_t reviverEntityID)
    {
        const uint32_t myEntityID = CoSyncNet::GetMyEntityID();
        if (entityID == myEntityID)
        {
            // Never touch the local player's own actor from here -- see
            // ConsumePendingLocalRevive's doc comment.
            s_pendingLocalReviveReviverEID.store(reviverEntityID);
            return;
        }

        CoSyncPlayer* proxy = g_CoSyncPlayerManager.GetPlayerByEntityID(entityID);
        if (!proxy)
            return;

        if (!proxy->isDowned)
            return;   // already up (e.g. the reviver's own REVIVED echoed back)
        proxy->isDowned = false;
        proxy->m_reviverEntityID = 0;
        if (s_channel.active && s_channel.targetEntityID == entityID)
            s_channel = ReviveChannel{};   // someone else got there first

        // 0.3.3 (Stage 6): no stand-up idle any more. Player proxies are
        // bone-replicated, and the downed player now keeps streaming while in
        // bleedout, so the real get-up animation arrives with the pose; a
        // local PlayIdle would be a second, competing animation source.

        char note[160];
        if (reviverEntityID == 0)
            std::snprintf(note, sizeof(note), "%s is back on their feet",
                proxy->username.empty() ? "Player" : proxy->username.c_str());
        else
            std::snprintf(note, sizeof(note), "%s has been revived!",
                proxy->username.empty() ? "Player" : proxy->username.c_str());
        CoSyncGameAPI::QueueNotification(note);

        CoSyncOverlay_UpdateLobbyDowned(entityID, false);

        LOG_UPDATED("[ReviveSync] entity=%u revived by entity=%u", entityID, reviverEntityID);
    }

    std::string BuildIniPath()
    {
        char exePath[MAX_PATH] = {};
        if (::GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0)
            return std::string();

        std::string ini(exePath);
        const size_t slash = ini.find_last_of("\\/");
        ini = (slash == std::string::npos) ? std::string() : ini.substr(0, slash + 1);
        ini += "Data\\F4SE\\Plugins\\CoSync.ini";
        return ini;
    }

    float ReadIniFloat(const char* key, float def)
    {
        const std::string ini = BuildIniPath();
        if (ini.empty())
            return def;

        char defBuf[32];
        std::snprintf(defBuf, sizeof(defBuf), "%.3f", def);
        char buf[32] = {};
        ::GetPrivateProfileStringA("Revive", key, defBuf, buf, sizeof(buf), ini.c_str());
        return static_cast<float>(std::atof(buf));
    }
}

bool CoSyncReviveSync::IsDown(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncReviveSync::IsDown", "Doesn't touch the game. Just checks if a message is a DOWN message.");
    return msg.rfind("DOWN|", 0) == 0;
}

bool CoSyncReviveSync::IsReviveReq(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncReviveSync::IsReviveReq", "Doesn't touch the game. Just checks if a message is a revive request.");
    return msg.rfind("REVIVEREQ|", 0) == 0;
}

bool CoSyncReviveSync::IsRevived(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncReviveSync::IsRevived", "Doesn't touch the game. Just checks if a message means someone got revived.");
    return msg.rfind("REVIVED|", 0) == 0;
}

std::string CoSyncReviveSync::SerializeDown(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncReviveSync::SerializeDown", "Doesn't touch the game. Just builds the DOWN message text.");
    return "DOWN|" + std::to_string(entityID);
}

std::string CoSyncReviveSync::SerializeReviveReq(uint32_t entityID, uint32_t reviverEntityID)
{
    LOG_REQUIREMENT("CoSyncReviveSync::SerializeReviveReq", "Doesn't touch the game. Just builds the revive-request message text.");
    return "REVIVEREQ|" + std::to_string(entityID) + "|" + std::to_string(reviverEntityID);
}

std::string CoSyncReviveSync::SerializeRevived(uint32_t entityID, uint32_t reviverEntityID)
{
    LOG_REQUIREMENT("CoSyncReviveSync::SerializeRevived", "Doesn't touch the game. Just builds the revived message text.");
    return "REVIVED|" + std::to_string(entityID) + "|" + std::to_string(reviverEntityID);
}

void CoSyncReviveSync::OnReceiveDown(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncReviveSync::OnReceiveDown", "Needs a properly formatted DOWN message with a real entity ID.");
    if (!IsDown(msg))
        return;

    const uint32_t entityID = (uint32_t)std::strtoul(msg.c_str() + 5, nullptr, 10);
    if (entityID == 0)
    {
        LOG_WARN("[ReviveSync] Malformed DOWN packet: '%s'", msg.c_str());
        return;
    }

    // Our own DOWN, relayed back by the host: local down-state is owned and
    // already applied directly by CoSynclocalplayer.cpp, not through this
    // queue -- ignore the echo.
    if (entityID == CoSyncNet::GetMyEntityID())
        return;

    std::lock_guard<std::mutex> lk(s_opMutex);
    s_pendingOps.push_back({ ReviveOp::Down, entityID, 0 });
}

void CoSyncReviveSync::OnReceiveReviveReq(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncReviveSync::OnReceiveReviveReq", "Needs a properly formatted revive-request message.");
    if (!IsReviveReq(msg))
        return;

    const char* p = msg.c_str() + 10;
    char* end = nullptr;
    const uint32_t entityID = (uint32_t)std::strtoul(p, &end, 10);
    if (!end || *end != '|' || entityID == 0)
    {
        LOG_WARN("[ReviveSync] Malformed REVIVEREQ packet: '%s'", msg.c_str());
        return;
    }
    const uint32_t reviverEntityID = (uint32_t)std::strtoul(end + 1, nullptr, 10);

    std::lock_guard<std::mutex> lk(s_opMutex);
    s_pendingOps.push_back({ ReviveOp::ReviveReq, entityID, reviverEntityID });
}

void CoSyncReviveSync::OnReceiveRevived(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncReviveSync::OnReceiveRevived", "Needs a properly formatted revived message.");
    if (!IsRevived(msg))
        return;

    const char* p = msg.c_str() + 8;
    char* end = nullptr;
    const uint32_t entityID = (uint32_t)std::strtoul(p, &end, 10);
    if (!end || *end != '|' || entityID == 0)
    {
        LOG_WARN("[ReviveSync] Malformed REVIVED packet: '%s'", msg.c_str());
        return;
    }
    const uint32_t reviverEntityID = (uint32_t)std::strtoul(end + 1, nullptr, 10);

    std::lock_guard<std::mutex> lk(s_opMutex);
    s_pendingOps.push_back({ ReviveOp::Revived, entityID, reviverEntityID });
}

void CoSyncReviveSync::FlushPendingReviveOps()
{
    LOG_REQUIREMENT("CoSyncReviveSync::FlushPendingReviveOps", "Every queued revive action gets looked up by player first. Never writes to the local player's own stats directly.");
    RefreshDownedRefs();

    std::vector<ReviveOp> ops;
    {
        std::lock_guard<std::mutex> lk(s_opMutex);
        if (s_pendingOps.empty())
            return;
        ops.swap(s_pendingOps);
    }

    for (const ReviveOp& op : ops)
    {
        switch (op.type)
        {
        case ReviveOp::Down:
        {
            CoSyncPlayer* proxy = g_CoSyncPlayerManager.GetPlayerByEntityID(op.entityID);
            if (!proxy)
            {
                // Proxy not spawned yet on this machine (e.g. still loading in)
                // -- the one-shot DOWN edge is lost for it. Acceptable for a
                // first pass: the lobby bar/collapse are presentation only, and
                // the peer's own give-up timer still governs their real state.
                LOG_UPDATED_WARN("[ReviveSync] DOWN for unknown entity=%u — dropped", op.entityID);
                continue;
            }

            proxy->isDowned = true;
            proxy->m_downedSince = NowSeconds();

            // No collapse pose forced here: unlike the real local player
            // (whose vanilla bleedout state already puts the actual actor in
            // the correct pose the moment SetNoBleedoutRecovery/Essential
            // collapse engage), a remote proxy is an AI-disabled presentation
            // dummy with no real bleedout state of its own. Follower Revive
            // System never needed to solve this either -- its downed actors
            // are always real, actually-collapsed actors. Left as a known gap
            // (proxy just stops moving + shows the DOWNED tag) rather than
            // guessing at an unverified SendAnimationEvent name.

            char note[160];
            std::snprintf(note, sizeof(note), "%s is down!",
                proxy->username.empty() ? "Player" : proxy->username.c_str());
            CoSyncGameAPI::QueueNotification(note);

            CoSyncOverlay_UpdateLobbyDowned(op.entityID, true);
            LOG_UPDATED("[ReviveSync] entity=%u is down", op.entityID);
            break;
        }

        case ReviveOp::ReviveReq:
        {
            if (op.entityID == CoSyncNet::GetMyEntityID())
            {
                CoSyncPlayer* reviver = g_CoSyncPlayerManager.GetPlayerByEntityID(op.reviverEntityID);
                char note[160];
                std::snprintf(note, sizeof(note), "%s is reviving you!",
                    (reviver && !reviver->username.empty()) ? reviver->username.c_str() : "Someone");
                CoSyncGameAPI::QueueNotification(note);
                break;
            }

            CoSyncPlayer* proxy = g_CoSyncPlayerManager.GetPlayerByEntityID(op.entityID);
            if (proxy)
                proxy->m_reviverEntityID = op.reviverEntityID;
            break;
        }

        case ReviveOp::Revived:
            ApplyRevivedToTarget(op.entityID, op.reviverEntityID);
            break;
        }
    }
}

void CoSyncReviveSync::BeginReviveAttempt(uint32_t targetEntityID)
{
    LOG_REQUIREMENT("CoSyncReviveSync::BeginReviveAttempt", "Needs the target to already be spawned and downed, otherwise does nothing. Must run on the main game thread.");
    if (!SettingEnabled())
        return;

    CoSyncPlayer* target = g_CoSyncPlayerManager.GetPlayerByEntityID(targetEntityID);
    if (!target || !target->isDowned || !target->actorRef)
        return;

    PlayerCharacter* player = CoSyncGetPlayer();
    if (!player)
        return;

    // Follower Revive System's core mechanic: a Stimpak is what actually
    // grants the health back, so reviving costs one -- same as using it on
    // yourself. No Stimpak, no revive.
    const int stimpakCount = CountItem(player, kStimpakFormID);
    LOG_UPDATED("[ReviveSync] BeginReviveAttempt entity=%u stimpakCount=%d", targetEntityID, stimpakCount);
    if (stimpakCount <= 0)
    {
        CoSyncGameAPI::QueueNotification("You need a Stimpak to revive them!");
        return;
    }

    if (s_channel.active)
    {
        if (s_channel.targetEntityID == targetEntityID)
            return; // already reviving this one
        CancelChannel("switched target");
    }

    const uint32_t myEntityID = CoSyncNet::GetMyEntityID();

    s_channel.active = true;
    s_channel.targetEntityID = targetEntityID;
    s_channel.startTime = NowSeconds();
    target->m_reviverEntityID = myEntityID;

    CoSyncTransport::Send(SerializeReviveReq(targetEntityID, myEntityID));
    CoSyncGameAPI::QueueNotification("Reviving...");

    // Actor.PlayIdleWithTarget(UseStimpakOnTarget, target) -- the exact idle
    // Follower Revive System plays on the rescuer (3rdPUseStimpakOnTarget,
    // formID 0x000C30F9, decoded from its own script's VMAD properties).
    NativePlayIdleWithTarget(player, GetUseStimpakOnTargetIdle(), target->actorRef);

    LOG_UPDATED("[ReviveSync] Begin revive attempt on entity=%u", targetEntityID);
}

void CoSyncReviveSync::TickReviveChannel(double now)
{
    LOG_REQUIREMENT("CoSyncReviveSync::TickReviveChannel", "Safe to call every tick -- does nothing if no revive is in progress. Must run on the main game thread.");
    if (!s_channel.active)
        return;

    CoSyncPlayer* target = g_CoSyncPlayerManager.GetPlayerByEntityID(s_channel.targetEntityID);
    if (!target || !target->isDowned || !target->actorRef)
    {
        CancelChannel("target no longer down/available");
        return;
    }

    PlayerCharacter* player = CoSyncGetPlayer();
    if (!player)
    {
        CancelChannel("local player unavailable");
        return;
    }

    const float dx = target->actorRef->pos.x - player->pos.x;
    const float dy = target->actorRef->pos.y - player->pos.y;
    const float dz = target->actorRef->pos.z - player->pos.z;
    if (dx * dx + dy * dy + dz * dz > kReviveMaxRangeSq)
    {
        CancelChannel("moved out of range");
        return;
    }

    if (now - s_channel.startTime < SettingChannelSeconds())
        return;

    const uint32_t targetEntityID = s_channel.targetEntityID;
    const uint32_t myEntityID = CoSyncNet::GetMyEntityID();
    s_channel = ReviveChannel{};

    // Stop the stimpak-use idle (Follower Revive System's IdleStop) and
    // spend the Stimpak that actually earned the heal -- re-checked here
    // rather than trusting the count from BeginReviveAttempt, in case it was
    // used/dropped mid-channel.
    NativePlayIdle(player, GetIdleStopIdle());
    const int stimpakCountAtComplete = CountItem(player, kStimpakFormID);
    LOG_UPDATED("[ReviveSync] TickReviveChannel complete entity=%u stimpakCountAtComplete=%d",
        targetEntityID, stimpakCountAtComplete);
    if (stimpakCountAtComplete > 0)
        NativeRemoveOneStimpak(player);

    CoSyncTransport::Send(SerializeRevived(targetEntityID, myEntityID));
    // Apply locally too -- Send() only puts the packet on the wire, it doesn't
    // loop back to the sender's own OnReceive path.
    ApplyRevivedToTarget(targetEntityID, myEntityID);
}

bool CoSyncReviveSync::ConsumePendingLocalRevive(uint32_t& outReviverEntityID)
{
    LOG_REQUIREMENT("CoSyncReviveSync::ConsumePendingLocalRevive", "Doesn't touch the game. Just reads and clears a pending flag.");
    const uint32_t v = s_pendingLocalReviveReviverEID.exchange(0);
    if (v == 0)
        return false;
    outReviverEntityID = v;
    return true;
}

void CoSyncReviveSync::Reset()
{
    LOG_REQUIREMENT("CoSyncReviveSync::Reset", "Doesn't touch the game. Just clears this module's own state for a new session.");
    {
        std::lock_guard<std::mutex> lk(s_opMutex);
        s_pendingOps.clear();
    }
    s_pendingLocalReviveReviverEID.store(0);
    s_channel = ReviveChannel{};
    {
        std::lock_guard<std::mutex> lk(s_downedRefsMutex);
        s_downedRefs.clear();
    }
    LOG_INFO("[ReviveSync] Reset — state cleared for new session");
}

bool CoSyncReviveSync::SettingEnabled()
{
    static int s_cached = -1;
    if (s_cached < 0)
    {
        const std::string ini = BuildIniPath();
        s_cached = ini.empty() ? 1 : ::GetPrivateProfileIntA("Revive", "bReviveSystemEnabled", 1, ini.c_str());
        LOG_INFO("[ReviveSync] bReviveSystemEnabled=%d", s_cached);
    }
    return s_cached != 0;
}

float CoSyncReviveSync::SettingGiveUpSeconds()
{
    static float s_cached = -1.f;
    if (s_cached < 0.f)
        s_cached = ReadIniFloat("fReviveGiveUpSeconds", 60.f);
    return s_cached;
}

float CoSyncReviveSync::SettingHealthPercent()
{
    static float s_cached = -1.f;
    if (s_cached < 0.f)
        s_cached = ReadIniFloat("fReviveHealthPercent", 50.f);
    return s_cached;
}

float CoSyncReviveSync::SettingChannelSeconds()
{
    static float s_cached = -1.f;
    if (s_cached < 0.f)
        s_cached = ReadIniFloat("fReviveChannelSeconds", 3.f);
    return s_cached;
}

// ── "Revive" prompt (0.3.3, Stage 6) ─────────────────────────────────────────
// IDA 1.11.240: TESNPC vtable 0x24D6400, slot 0x60 (entry 0x24D6700) =
// sub_140656D60(TESNPC* base, TESObjectREFR* ref, BSString* out) builds the
// crosshair text for an actor: "%s\n%s" of the ref's display name
// (sub_140513630(ref)) and a verb such as sTalk (GMST value slot 0x2F22888,
// read at 0x657192), printed with the BSString printf sub_1401F84F0. We let it
// run, then replace the text for a downed player proxy with the same format and
// "Revive". The prompt is recomputed by the engine every time it is shown, so it
// follows the down state live (RefreshDownedRefs runs every tick).
namespace
{
    constexpr uintptr_t kRVA_NpcGetActivateText = 0x00656D60;
    constexpr uintptr_t kRVA_RefDisplayName     = 0x00513630;
    constexpr uintptr_t kRVA_BSStringPrintf     = 0x001F84F0;

    using GetActivateText_t = uint64_t(__fastcall*)(void* base, TESObjectREFR* ref, void* out);
    using RefDisplayName_t  = const char*(__fastcall*)(TESObjectREFR* ref);
    using BSStringPrintf_t  = uint64_t(*)(void* out, const char* fmt, ...);

    GetActivateText_t g_origGetActivateText = nullptr;
    RefDisplayName_t  g_refDisplayName      = nullptr;
    BSStringPrintf_t  g_bsStringPrintf      = nullptr;

    __declspec(noinline) bool WriteReviveText(TESObjectREFR* ref, void* out)
    {
        __try
        {
            const char* name = g_refDisplayName ? g_refDisplayName(ref) : nullptr;
            g_bsStringPrintf(out, "%s\n%s", name ? name : "", "Revive");
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    uint64_t __fastcall Detour_GetActivateText(void* base, TESObjectREFR* ref, void* out)
    {
        const uint64_t r = g_origGetActivateText(base, ref, out);
        if (ref && out && g_bsStringPrintf && IsDownedProxyRef(ref->formID) && WriteReviveText(ref, out))
            return 1;
        return r;
    }
}

void CoSyncReviveSync::InstallActivateTextHook()
{
    static bool s_installed = false;
    if (s_installed)
        return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    g_refDisplayName = reinterpret_cast<RefDisplayName_t>(base + kRVA_RefDisplayName);
    g_bsStringPrintf = reinterpret_cast<BSStringPrintf_t>(base + kRVA_BSStringPrintf);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[ReviveSync] MH_Initialize failed: %d", (int)st);
        return;
    }
    void* target = reinterpret_cast<void*>(base + kRVA_NpcGetActivateText);
    st = MH_CreateHook(target, reinterpret_cast<void*>(&Detour_GetActivateText),
                       reinterpret_cast<void**>(&g_origGetActivateText));
    if (st != MH_OK)
    {
        LOG_ERROR("[ReviveSync] MH_CreateHook(activate text) failed: %d target=%p", (int)st, target);
        return;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK)
    {
        LOG_ERROR("[ReviveSync] MH_EnableHook(activate text) failed: %d", (int)st);
        return;
    }
    s_installed = true;
    LOG_INFO("[ReviveSync] \"Revive\" prompt hook installed at base+0x%zX", (size_t)kRVA_NpcGetActivateText);
}
