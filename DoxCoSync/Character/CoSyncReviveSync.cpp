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
#include "CoSyncPlayerAnimation.h"
#include "MinHook.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "GameData.h"
#include "PapyrusVM.h"
#include "PapyrusEvents.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern PlayerCharacter* CoSyncGetPlayer();

namespace
{
    // Fallout4.esm forms (master file: stable FormIDs).
    constexpr uint32_t kStimpakFormID          = 0x00023736u;   // ALCH Stimpak
    constexpr uint32_t kPlayerCanStimpakKwd    = 0x000AD52Au;   // KYWD playerCanStimpak

    // Engine activation-action entry (1.11.240, IDA 2026-10-05):
    // sub_140CFAE10(AIProcess*, Actor*, int action, TESForm* base, TESObjectREFR* target)
    // builds a TESActionData for DefaultObject action #action and performs it
    // through the actor's graph (sub_140E13D70); returns true when it played.
    // PlayerCharacter's "use a Stimpak on this bleeding-out actor"
    // (sub_140D777D0, reached from ActivateRef -> sub_140C9DA60) calls it with
    // action 43 and the target -- that is what selects the dyn_Activation
    // *UseStimpakOnTarget idle for the actor's current graph.
    constexpr uintptr_t kRVA_PerformActivationAction = 0x00CFAE10;
    constexpr uint8_t   kPerformActionPrologue[8] = { 0x48, 0x8B, 0xC4, 0x55, 0x56, 0x41, 0x56, 0x41 };
    constexpr int       kActivateAction = 43;
    constexpr size_t    kActorProcessOffset = 0x300;   // Actor+0x300: AIProcess* (a1[96] in the decompiles)

    uintptr_t Base() { static const uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); return b; }

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

    TESForm* FormChecked(uint32_t formID, UInt32 type)
    {
        TESForm* f = LookupFormByID(formID);
        return (f && f->formType == type) ? f : nullptr;
    }

    TESForm* Stimpak()  { static TESForm* f = FormChecked(kStimpakFormID, kFormType_ALCH); return f; }
    TESForm* Keyword()  { static TESForm* f = FormChecked(kPlayerCanStimpakKwd, kFormType_KYWD); return f; }

    // ── Inventory ───────────────────────────────────────────────────────────
    __declspec(noinline) int CountItemImpl(Actor* actor, uint32_t formID)
    {
        __try
        {
            int total = 0;
            BGSInventoryList* inv = actor ? actor->inventoryList : nullptr;
            if (!inv) return 0;
            for (UInt32 i = 0; i < inv->items.count; ++i)
            {
                const BGSInventoryItem& item = inv->items[i];
                if (!item.form || item.form->formID != formID) continue;
                for (BGSInventoryItem::Stack* s = item.stack; s; s = s->next)
                    total += s->count;
            }
            return total;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    int StimpakCount()
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        return pc ? CountItemImpl(pc, kStimpakFormID) : 0;
    }

    // ── Keyword (Papyrus queue; the idle conditions read it on the target) ──
    void SetCanStimpak(TESObjectREFR* ref, bool on)
    {
        TESForm* kw = Keyword();
        if (!ref || !kw) return;
        static const BSFixedString kAdd("AddKeyword");
        static const BSFixedString kRemove("RemoveKeyword");
        VMArray<VMVariable> args;
        VMVariable v; CoSyncVM::SetFormArg(v, kw); args.Push(&v);
        CallFunctionNoWait(ref, on ? kAdd : kRemove, args);
    }

    // ── The animation ───────────────────────────────────────────────────────
    __declspec(noinline) bool PerformActivationImpl(Actor* actor, TESObjectREFR* target)
    {
        __try
        {
            void* process = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(actor) + kActorProcessOffset);
            if (!process || !target || !target->baseForm) return false;
            using Fn = bool(__fastcall*)(void*, Actor*, int, TESForm*, TESObjectREFR*);
            return reinterpret_cast<Fn>(Base() + kRVA_PerformActivationAction)(process, actor, kActivateAction, target->baseForm, target);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool PlayStimpakIdle(Actor* actor, TESObjectREFR* target)
    {
        static const bool ok = std::memcmp(reinterpret_cast<const void*>(Base() + kRVA_PerformActivationAction),
                                           kPerformActionPrologue, sizeof(kPerformActionPrologue)) == 0;
        if (!ok)
        {
            static bool warned = false;
            if (!warned) { warned = true; LOG_ERROR("[ReviveSync] activation-action prologue mismatch -- revive animation disabled"); }
            return false;
        }
        return actor && target && PerformActivationImpl(actor, target);
    }

    // The actor a given entity is on THIS machine (proxy, or our own player).
    Actor* ActorOf(uint32_t eid)
    {
        if (eid == CoSyncNet::GetMyEntityID())
            return reinterpret_cast<Actor*>(CoSyncGetPlayer());
        CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(eid);
        return (p && p->hasSpawned) ? p->actorRef : nullptr;
    }

    std::string NameOf(uint32_t eid)
    {
        if (eid == CoSyncNet::GetMyEntityID()) return CoSyncNet::GetMyName();
        CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(eid);
        return (p && !p->username.empty()) ? p->username : std::string("Player");
    }

    void Note(const std::string& s) { CoSyncGameAPI::QueueNotification(s.c_str()); }

    // ── Wire ────────────────────────────────────────────────────────────────
    bool Parse2(const std::string& msg, size_t skip, uint32_t& a, uint32_t& b, std::string* rest = nullptr)
    {
        const char* p = msg.c_str() + skip;
        char* end = nullptr;
        a = static_cast<uint32_t>(std::strtoul(p, &end, 10));
        b = 0;
        if (end && *end == '|')
        {
            b = static_cast<uint32_t>(std::strtoul(end + 1, &end, 10));
            if (rest && end && *end == '|') *rest = std::string(end + 1);
        }
        return a != 0;
    }

    std::string Msg(const char* tag, uint32_t t, uint32_t r)
    {
        return std::string(tag) + "|" + std::to_string(t) + "|" + std::to_string(r);
    }

    // Host: to everyone, including itself (Send() does not loop back).
    void HostBroadcast(const std::string& msg);
    void HostSendToEntity(uint32_t eid, const std::string& msg);
    void HostCancel(uint32_t t, uint32_t r);

    // ── Queue (network thread -> game thread) ───────────────────────────────
    struct Op { std::string msg; uint32_t conn; bool hostRole; uint32_t sender; };
    std::mutex       s_opMutex;
    std::vector<Op>  s_ops;

    // ── Host authority ──────────────────────────────────────────────────────
    struct DownEntry { double since = 0.0; uint32_t reviver = 0; double claimSince = 0.0; bool done = false; };
    std::unordered_map<uint32_t, DownEntry> s_hostDown;   // target eid ->

    // ── Local reviver channel ───────────────────────────────────────────────
    enum class Ch { None, Claiming, Channeling, Finishing };
    struct Channel { Ch state = Ch::None; uint32_t target = 0; double start = 0.0; double deadline = 0.0; };
    Channel s_ch;

    // ── Local downed player ─────────────────────────────────────────────────
    std::atomic<uint32_t> s_pendingRevive{ 0 };   // reviver eid (REVIVED arrived)
    std::atomic<bool>     s_pendingDeath{ false };
    double   s_localDownSince = 0.0;              // 0 = not down
    double   s_localDeadSince = 0.0;              // 0 = not dead
    uint32_t s_localReviver = 0;                  // being revived by (display)

    // Proxies on this machine that carry the keyword, and the reviver proxies
    // currently animating (bones paused).
    std::unordered_set<uint32_t> s_keyworded;

    // Downed proxies' ref IDs, for the activate-text hook (engine thread).
    std::mutex                   s_downedRefsMutex;
    std::unordered_set<uint32_t> s_downedRefs;

    constexpr float kReviveMaxRange   = 350.0f;
    constexpr float kReviveMaxRangeSq = kReviveMaxRange * kReviveMaxRange;

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

    bool IsDownedProxyRef(uint32_t refFormID)
    {
        std::lock_guard<std::mutex> lk(s_downedRefsMutex);
        return s_downedRefs.count(refFormID) != 0;
    }

    // Everyone: the reviver's animation on this machine.
    void ShowReviveAnimation(uint32_t target, uint32_t reviver)
    {
        if (reviver == CoSyncNet::GetMyEntityID())
            return;   // the reviver's own machine plays it on its player (channel start)
        Actor* rv = ActorOf(reviver);
        Actor* tg = ActorOf(target);
        if (!rv || !tg)
            return;
        const float secs = CoSyncReviveSync::SettingChannelSeconds() + 0.5f;
        CoSyncPlayerAnimation::SuspendPoseApply(rv, static_cast<uint32_t>(secs * 1000.f));
        const bool played = PlayStimpakIdle(rv, reinterpret_cast<TESObjectREFR*>(tg));
        LOG_UPDATED("[ReviveSync] reviver proxy entity=%u: Stimpak animation on entity=%u %s",
            reviver, target, played ? "playing" : "REFUSED by the engine");
        if (!played)
            CoSyncPlayerAnimation::SuspendPoseApply(rv, 0);
    }

    void StopReviveAnimation(uint32_t reviver)
    {
        if (reviver == CoSyncNet::GetMyEntityID()) return;
        if (Actor* rv = ActorOf(reviver))
            CoSyncPlayerAnimation::SuspendPoseApply(rv, 0);   // replicated pose takes over again
    }

    void MarkProxyDowned(uint32_t eid, bool down)
    {
        if (eid == CoSyncNet::GetMyEntityID())
            return;
        CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(eid);
        if (!p) return;
        p->isDowned = down;
        if (!down) p->m_reviverEntityID = 0;
        else p->m_downedSince = NowSeconds();
        if (p->actorRef)
        {
            const bool has = s_keyworded.count(eid) != 0;
            if (down && !has) { SetCanStimpak(p->actorRef, true); s_keyworded.insert(eid); }
            if (!down && has) { SetCanStimpak(p->actorRef, false); s_keyworded.erase(eid); }
        }
        CoSyncOverlay_UpdateLobbyDowned(eid, down);
    }

    void CancelLocalChannel(const char* why, bool tellHost)
    {
        if (s_ch.state == Ch::None)
            return;
        LOG_INFO("[ReviveSync] my revive of entity=%u stopped (%s)", s_ch.target, why);
        const uint32_t target = s_ch.target;
        const bool wasGranted = s_ch.state != Ch::Claiming;
        s_ch = Channel{};
        if (tellHost && wasGranted)
        {
            if (CoSyncTransport::IsHost())
                HostCancel(target, CoSyncNet::GetMyEntityID());
            else
                CoSyncTransport::Send(Msg("RVCANCEL", target, CoSyncNet::GetMyEntityID()));
        }
    }

    // ── Applying one message on this machine (game thread) ──────────────────
    void Apply(const std::string& tag, uint32_t t, uint32_t r, const std::string& rest)
    {
        const uint32_t me = CoSyncNet::GetMyEntityID();

        if (tag == "DOWN")
        {
            if (t == me) return;   // our own state is CoSynclocalplayer's
            MarkProxyDowned(t, true);
            Note(NameOf(t) + " is down!");
            return;
        }
        if (tag == "RVGRANT")
        {
            if (t == me)
            {
                s_localReviver = r;
                Note(NameOf(r) + " is reviving you!");
            }
            else if (CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(t))
                p->m_reviverEntityID = r;
            if (r == me && s_ch.state == Ch::Claiming && s_ch.target == t)
            {
                // Granted: play the engine's own Stimpak activation idle on
                // our player, aimed at the downed player's proxy.
                Actor* tg = ActorOf(t);
                PlayerCharacter* pc = CoSyncGetPlayer();
                const bool played = pc && tg && PlayStimpakIdle(pc, reinterpret_cast<TESObjectREFR*>(tg));
                LOG_UPDATED("[ReviveSync] revive of entity=%u granted -- Stimpak animation %s", t, played ? "playing" : "refused by the engine");
                s_ch.state = Ch::Channeling;
                s_ch.start = NowSeconds();
                Note("Reviving " + NameOf(t) + "...");
            }
            ShowReviveAnimation(t, r);
            return;
        }
        if (tag == "RVDENY")
        {
            if (r == me && s_ch.target == t)
            {
                s_ch = Channel{};
                Note(rest.empty() ? std::string("You can't revive them right now.") : rest);
            }
            return;
        }
        if (tag == "RVCANCEL")
        {
            if (t == me) s_localReviver = 0;
            else if (CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(t))
                if (p->m_reviverEntityID == r) p->m_reviverEntityID = 0;
            if (r == me && s_ch.target == t)
                s_ch = Channel{};
            StopReviveAnimation(r);
            return;
        }
        if (tag == "REVIVED")
        {
            if (r != 0) StopReviveAnimation(r);
            if (t == me)
            {
                // Never touch our own actor from here: CoSynclocalplayer
                // applies it at its own safe point.
                if (s_localDownSince > 0.0 && s_localDeadSince == 0.0)
                    s_pendingRevive.store(r != 0 ? r : 0xFFFFFFFFu);
                s_localReviver = 0;
            }
            else
            {
                CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(t);
                const bool wasDown = p && p->isDowned;
                MarkProxyDowned(t, false);
                if (wasDown)
                    Note(r == 0 ? NameOf(t) + " is back on their feet." : NameOf(t) + " has been revived!");
            }
            // The reviver spends exactly one Stimpak -- only now, when the
            // host has accepted the revive.
            if (r == me && r != 0 && s_ch.target == t && s_ch.state == Ch::Finishing)
            {
                PlayerCharacter* pc = CoSyncGetPlayer();
                TESForm* stim = Stimpak();
                const bool spent = pc && stim && CoSyncGameAPI::NativeRemoveItem(reinterpret_cast<TESObjectREFR*>(pc), stim, 1);
                LOG_UPDATED("[ReviveSync] revive of entity=%u confirmed by the host -- Stimpak %s", t, spent ? "spent" : "NOT spent (none left?)");
                s_ch = Channel{};
                Note("You revived " + NameOf(t) + ".");
            }
            return;
        }
        if (tag == "DIED")
        {
            if (t == me)
            {
                if (s_localDownSince > 0.0)
                    s_pendingDeath.store(true);
                return;
            }
            MarkProxyDowned(t, false);
            if (s_ch.target == t) s_ch = Channel{};
            Note(NameOf(t) + " has died.");
            return;
        }
    }

    // ── Host decisions ──────────────────────────────────────────────────────
    void HostBroadcast(const std::string& msg)
    {
        if (CoSyncTransport::IsInitialized() && CoSyncNet::IsConnected())
            CoSyncTransport::Send(msg);
        uint32_t t = 0, r = 0;
        std::string rest;
        const size_t bar = msg.find('|');
        Parse2(msg, bar + 1, t, r, &rest);
        Apply(msg.substr(0, bar), t, r, rest);
    }

    void HostSendToEntity(uint32_t eid, const std::string& msg)
    {
        if (eid == CoSyncNet::GetMyEntityID())
        {
            uint32_t t = 0, r = 0;
            std::string rest;
            const size_t bar = msg.find('|');
            Parse2(msg, bar + 1, t, r, &rest);
            Apply(msg.substr(0, bar), t, r, rest);
            return;
        }
        for (const auto& p : CoSyncNet::HostGetPeers())
            if (p.entityID == eid)
            {
                CoSyncTransport::SendTo(p.conn, msg);
                return;
            }
    }

    void HostDeny(uint32_t t, uint32_t r, const char* why)
    {
        LOG_INFO("[ReviveSync] host: revive of entity=%u by entity=%u refused (%s)", t, r, why);
        HostSendToEntity(r, Msg("RVDENY", t, r) + "|" + why);
    }

    void HostClaim(uint32_t t, uint32_t r)
    {
        if (t == r) return HostDeny(t, r, "You can't revive yourself.");
        auto it = s_hostDown.find(t);
        if (it == s_hostDown.end() || it->second.done)
            return HostDeny(t, r, "They are not down any more.");
        if (NowSeconds() - it->second.since >= CoSyncReviveSync::SettingDownedSeconds())
            return HostDeny(t, r, "Too late -- they are gone.");
        if (s_hostDown.count(r))
            return HostDeny(t, r, "You are down yourself.");
        if (it->second.reviver != 0 && it->second.reviver != r)
            return HostDeny(t, r, (NameOf(it->second.reviver) + " is already reviving them.").c_str());
        it->second.reviver = r;
        it->second.claimSince = NowSeconds();
        LOG_INFO("[ReviveSync] host: entity=%u may revive entity=%u", r, t);
        HostBroadcast(Msg("RVGRANT", t, r));
    }

    void HostDone(uint32_t t, uint32_t r)
    {
        auto it = s_hostDown.find(t);
        if (it == s_hostDown.end() || it->second.done || it->second.reviver != r)
        {
            LOG_INFO("[ReviveSync] host: completion of entity=%u's revive by entity=%u ignored (no matching claim)", t, r);
            return;
        }
        if (NowSeconds() - it->second.since >= CoSyncReviveSync::SettingDownedSeconds() + 1.0)
            return HostDeny(t, r, "Too late -- they are gone.");
        it->second.done = true;
        s_hostDown.erase(it);
        LOG_INFO("[ReviveSync] host: entity=%u revived entity=%u", r, t);
        HostBroadcast(Msg("REVIVED", t, r));
    }

    void HostCancel(uint32_t t, uint32_t r)
    {
        auto it = s_hostDown.find(t);
        if (it != s_hostDown.end() && it->second.reviver == r)
        {
            it->second.reviver = 0;
            it->second.claimSince = 0.0;
        }
        HostBroadcast(Msg("RVCANCEL", t, r));
    }

    void HostTick()
    {
        const double now = NowSeconds();
        const double limit = CoSyncReviveSync::SettingDownedSeconds();
        std::vector<uint32_t> dead;
        for (auto& kv : s_hostDown)
        {
            if (kv.second.done) continue;
            // A revive in progress gets to finish its channel.
            const double grace = kv.second.reviver ? CoSyncReviveSync::SettingChannelSeconds() + 2.0 : 0.0;
            if (now - kv.second.since >= limit + grace)
                dead.push_back(kv.first);
            else if (kv.second.reviver && now - kv.second.claimSince > CoSyncReviveSync::SettingChannelSeconds() + 6.0)
            {
                // The reviver went silent: free the claim for someone else.
                const uint32_t r = kv.second.reviver;
                kv.second.reviver = 0;
                HostBroadcast(Msg("RVCANCEL", kv.first, r));
            }
        }
        for (uint32_t t : dead)
        {
            s_hostDown.erase(t);
            LOG_INFO("[ReviveSync] host: entity=%u's time ran out -- died", t);
            HostBroadcast("DIED|" + std::to_string(t));
        }
    }

    // One received message on the game thread.
    void Process(const Op& op)
    {
        const size_t bar = op.msg.find('|');
        const std::string tag = op.msg.substr(0, bar);
        uint32_t t = 0, r = 0;
        std::string rest;
        if (!Parse2(op.msg, bar + 1, t, r, &rest))
            return;

        if (op.hostRole)
        {
            const bool fromClient = op.conn != 0;
            const uint32_t s = op.sender;
            // Sender checks: each message only from the side it speaks for.
            if (tag == "DOWN")
            {
                if (fromClient && s != t) return;
                s_hostDown[t] = DownEntry{ NowSeconds() };
                HostBroadcast(op.msg);
                return;
            }
            if (tag == "RVCLAIM")   { if (!fromClient || s == r) HostClaim(t, r);   return; }
            if (tag == "RVDONE")    { if (!fromClient || s == r) HostDone(t, r);    return; }
            if (tag == "RVCANCEL")  { if (!fromClient || s == r) HostCancel(t, r);  return; }
            if (tag == "REVIVED")
            {
                // Only "back on my feet" (reviver 0) from the player itself;
                // a real revive is the host's own REVIVED.
                if (r != 0 || (fromClient && s != t)) return;
                s_hostDown.erase(t);
                HostBroadcast(op.msg);
                return;
            }
            if (tag == "DIED")
            {
                // Fallback death from the player's own machine.
                if (fromClient && s != t) return;
                s_hostDown.erase(t);
                HostBroadcast(op.msg);
                return;
            }
            return;   // RVGRANT / RVDENY never come from a client
        }
        // Client: everything here came from the host.
        Apply(tag, t, r, rest);
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

namespace CoSyncReviveSync
{
    bool IsReviveMessage(const std::string& msg)
    {
        static const char* const kTags[] = { "DOWN|", "RVCLAIM|", "RVGRANT|", "RVDENY|", "RVCANCEL|", "RVDONE|", "REVIVED|", "DIED|" };
        for (const char* t : kTags)
            if (msg.rfind(t, 0) == 0) return true;
        return false;
    }

    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole, uint32_t senderEid)
    {
        LOG_REQUIREMENT("CoSyncReviveSync::OnReceive", "Network side; the game thread applies it. The host checks every message's sender.");
        if (msg.size() > 512) return;
        std::lock_guard<std::mutex> lk(s_opMutex);
        if (s_ops.size() < 256)
            s_ops.push_back({ msg, conn, isHostRole, senderEid });
    }

    std::string SerializeDown(uint32_t entityID) { return "DOWN|" + std::to_string(entityID); }
    std::string SerializeRevived(uint32_t entityID, uint32_t reviverEntityID) { return Msg("REVIVED", entityID, reviverEntityID); }

    void FlushPendingReviveOps()
    {
        RefreshDownedRefs();
        std::vector<Op> ops;
        {
            std::lock_guard<std::mutex> lk(s_opMutex);
            ops.swap(s_ops);
        }
        for (const Op& op : ops)
            Process(op);
        if (CoSyncTransport::IsInitialized() && CoSyncTransport::IsHost())
            HostTick();
    }

    void BeginReviveAttempt(uint32_t target)
    {
        if (!SettingEnabled())
            return;
        CoSyncPlayer* tp = g_CoSyncPlayerManager.GetPlayerByEntityID(target);
        if (!tp || !tp->isDowned || !tp->actorRef)
            return;   // not down (or dead: DIED clears isDowned)
        if (s_localDownSince > 0.0)
            return Note("You can't revive anyone while you are down.");
        if (s_ch.state != Ch::None)
            return;   // already reviving / asking
        if (StimpakCount() <= 0)
            return Note("You need a Stimpak to revive them!");
        const uint32_t me = CoSyncNet::GetMyEntityID();
        s_ch.state = Ch::Claiming;
        s_ch.target = target;
        s_ch.deadline = NowSeconds() + 4.0;
        LOG_UPDATED("[ReviveSync] asking to revive entity=%u", target);
        if (CoSyncTransport::IsHost())
            HostClaim(target, me);
        else
            CoSyncTransport::Send(Msg("RVCLAIM", target, me));
    }

    void TickReviveChannel(double)
    {
        if (s_ch.state == Ch::None)
            return;
        const double now = NowSeconds();
        if (s_ch.state == Ch::Claiming || s_ch.state == Ch::Finishing)
        {
            if (now > s_ch.deadline)
            {
                if (s_ch.state == Ch::Finishing)
                    Note("The revive did not go through.");
                CancelLocalChannel(s_ch.state == Ch::Claiming ? "no answer from the host" : "no confirmation", s_ch.state == Ch::Finishing);
            }
            return;
        }
        // Channeling.
        CoSyncPlayer* tp = g_CoSyncPlayerManager.GetPlayerByEntityID(s_ch.target);
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!tp || !tp->isDowned || !tp->actorRef || !pc)
            return CancelLocalChannel("target no longer down", true);
        const float dx = tp->actorRef->pos.x - pc->pos.x, dy = tp->actorRef->pos.y - pc->pos.y, dz = tp->actorRef->pos.z - pc->pos.z;
        if (dx * dx + dy * dy + dz * dz > kReviveMaxRangeSq)
        {
            Note("Revive interrupted -- you moved away.");
            return CancelLocalChannel("moved out of range", true);
        }
        if (now - s_ch.start < SettingChannelSeconds())
            return;
        if (StimpakCount() <= 0)
        {
            Note("You no longer have a Stimpak.");
            return CancelLocalChannel("no Stimpak left", true);
        }
        // Done: the host accepts it (or not); the Stimpak is spent on its REVIVED.
        s_ch.state = Ch::Finishing;
        s_ch.deadline = now + 5.0;
        const uint32_t me = CoSyncNet::GetMyEntityID();
        if (CoSyncTransport::IsHost())
            HostDone(s_ch.target, me);
        else
            CoSyncTransport::Send(Msg("RVDONE", s_ch.target, me));
    }

    void OnLocalDown()
    {
        s_localDownSince = NowSeconds();
        s_localDeadSince = 0.0;
        s_localReviver = 0;
        s_pendingRevive.store(0);
        s_pendingDeath.store(false);
        if (s_ch.state != Ch::None)
            CancelLocalChannel("I went down", true);
        if (PlayerCharacter* pc = CoSyncGetPlayer())
            SetCanStimpak(reinterpret_cast<TESObjectREFR*>(pc), true);
        const std::string m = SerializeDown(CoSyncNet::GetMyEntityID());
        if (CoSyncTransport::IsHost())
        {
            s_hostDown[CoSyncNet::GetMyEntityID()] = DownEntry{ NowSeconds() };
            if (CoSyncTransport::IsInitialized() && CoSyncNet::IsConnected())
                CoSyncTransport::Send(m);
        }
        else
            CoSyncTransport::Send(m);
    }

    bool ConsumePendingLocalRevive(uint32_t& outReviver)
    {
        const uint32_t v = s_pendingRevive.exchange(0);
        if (v == 0) return false;
        outReviver = v == 0xFFFFFFFFu ? 0 : v;
        return true;
    }

    bool ConsumeLocalDeath(double secondsDown)
    {
        if (s_localDeadSince > 0.0)
            return false;
        bool die = s_pendingDeath.exchange(false);
        if (!die && secondsDown >= SettingDownedSeconds() + 6.0)
        {
            // The host never answered (it left, or this machine is alone):
            // the clock still ends the downed state.
            die = true;
            const std::string m = "DIED|" + std::to_string(CoSyncNet::GetMyEntityID());
            if (CoSyncTransport::IsInitialized() && CoSyncNet::IsConnected())
                CoSyncTransport::Send(m);
            if (CoSyncTransport::IsInitialized() && CoSyncTransport::IsHost())
                s_hostDown.erase(CoSyncNet::GetMyEntityID());
        }
        if (die)
        {
            s_localDeadSince = NowSeconds();
            s_localReviver = 0;
            if (PlayerCharacter* pc = CoSyncGetPlayer())
                SetCanStimpak(reinterpret_cast<TESObjectREFR*>(pc), false);
            Note("You died.");
            LOG_UPDATED("[ReviveSync] I died after %.0f s down", secondsDown);
        }
        return die;
    }

    void OnLocalUp()
    {
        if (s_localDownSince > 0.0)
            if (PlayerCharacter* pc = CoSyncGetPlayer())
                SetCanStimpak(reinterpret_cast<TESObjectREFR*>(pc), false);
        s_localDownSince = 0.0;
        s_localDeadSince = 0.0;
        s_localReviver = 0;
        s_pendingDeath.store(false);
    }

    float RespawnSeconds()
    {
        static float s = -1.f;
        if (s < 0.f) s = (std::max)(1.f, ReadIniFloat("fRespawnSeconds", 6.f));
        return s;
    }

    LocalStatus GetLocalStatus(double)
    {
        LocalStatus st;
        const double now = NowSeconds();
        if (s_localDeadSince > 0.0)
        {
            st.state = 2;
            st.secondsLeft = (std::max)(0, static_cast<int>(RespawnSeconds() - (now - s_localDeadSince) + 0.999));
        }
        else if (s_localDownSince > 0.0)
        {
            st.state = 1;
            st.secondsLeft = (std::max)(0, static_cast<int>(SettingDownedSeconds() - (now - s_localDownSince) + 0.999));
            if (s_localReviver) st.reviver = NameOf(s_localReviver);
        }
        return st;
    }

    void ReleaseLocalDownOnSessionEnd()
    {
        if (s_localDownSince <= 0.0)
            return;
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (pc)
        {
            static const BSFixedString kSetNoBleedoutRecovery("SetNoBleedoutRecovery");
            VMArray<VMVariable> args;
            bool v = false;
            VMVariable vv; vv.Set(&v); args.Push(&vv);
            CallFunctionNoWait(pc, kSetNoBleedoutRecovery, args);
            SetCanStimpak(reinterpret_cast<TESObjectREFR*>(pc), false);
        }
        LOG_INFO("[ReviveSync] session ended while I was down -- vanilla bleedout recovery released");
        s_localDownSince = 0.0;
        s_localDeadSince = 0.0;
    }

    void HostOnPeerLeft(uint32_t eid)
    {
        if (!CoSyncTransport::IsInitialized() || !CoSyncTransport::IsHost() || eid == 0)
            return;
        s_hostDown.erase(eid);   // a downed player who left
        std::vector<std::pair<uint32_t, uint32_t>> cancels;
        for (auto& kv : s_hostDown)
            if (kv.second.reviver == eid)
            {
                kv.second.reviver = 0;
                cancels.push_back({ kv.first, eid });
            }
        for (const auto& c : cancels)
            HostBroadcast(Msg("RVCANCEL", c.first, c.second));
        if (s_ch.target == eid)
            s_ch = Channel{};
    }

    void Reset()
    {
        {
            std::lock_guard<std::mutex> lk(s_opMutex);
            s_ops.clear();
        }
        s_hostDown.clear();
        s_ch = Channel{};
        s_pendingRevive.store(0);
        s_pendingDeath.store(false);
        s_localReviver = 0;
        s_keyworded.clear();
        {
            std::lock_guard<std::mutex> lk(s_downedRefsMutex);
            s_downedRefs.clear();
        }
        LOG_INFO("[ReviveSync] Reset -- state cleared for new session");
    }

    bool SettingEnabled()
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

    float SettingDownedSeconds()
    {
        static float s_cached = -1.f;
        if (s_cached < 0.f)
            s_cached = (std::min)(600.f, (std::max)(5.f, ReadIniFloat("fDownedSeconds", 30.f)));
        return s_cached;
    }

    float SettingHealthPercent()
    {
        static float s_cached = -1.f;
        if (s_cached < 0.f)
            s_cached = ReadIniFloat("fReviveHealthPercent", 50.f);
        return s_cached;
    }

    float SettingChannelSeconds()
    {
        static float s_cached = -1.f;
        if (s_cached < 0.f)
            s_cached = (std::min)(15.f, (std::max)(1.f, ReadIniFloat("fReviveChannelSeconds", 3.f)));
        return s_cached;
    }
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
