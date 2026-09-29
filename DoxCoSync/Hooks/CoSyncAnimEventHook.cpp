#include "CoSyncAnimEventHook.h"
#include "CoSyncAnimationUtils.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "ConsoleLogger.h"
#include "GameReferences.h"
#include "GameObjects.h"
#include "GameTypes.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncNpcWorldSync.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncEntityState.h"
#include "CoSyncGameAPI.h"

#include "MinHook.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <string>
#include <atomic>
#include <mutex>
#include <vector>
#include "CoSyncThreadDiag.h"

// Was extern RelocPtr<PlayerCharacter*> g_player -- switched to CoSyncGetPlayer()
// (see GameGlobals.cpp), the established, confirmed-working replacement for
// raw g_player dereferences used throughout the rest of the codebase.
extern PlayerCharacter* CoSyncGetPlayer();
extern CoSyncPlayerManager g_CoSyncPlayerManager;

// LAYOUT PROVEN 2026-08-02 against Fallout4_disk.i64 by decompiling the engine's
// own static event constructors (sub_140806510 / sub_140806690 / sub_1408065D0),
// which write the three fields literally:
//     +0x00 = 0                                 (holder)
//     +0x08 = BSFixedString("StartMotionDriven") (tag)
//     +0x10 = BSFixedString()                    (payload)
// Corroborated inside ReceiveEvent 0xCAB610 itself, which hands `event+8` to the
// BSFixedString helpers (0x167C300 length / 0x167BEF0 copy-ctor) and `event+16`
// to the response-handler walk 0xCC8360.
//
// This struct previously declared `tag` at +0x00 -- off by 8 bytes. c_str() was
// therefore reading the HOLDER pointer as an interned-string entry: null holder
// -> empty tag -> event silently dropped; non-null holder -> a TESObjectREFR*
// walked as a StringCache::Entry, which is the source of both the garbage
// '@'/''/null tags AND the hard host crash logged 2026-07-15. That crash is what
// got this hook branded "the WRONG function on NG" and disabled -- the hook
// target and the actor = sink - 0x38 math were correct all along.
class BSAnimationGraphEvent
{
public:
    TESObjectREFR* holder;   // 00
    BSFixedString  tag;      // 08
    BSFixedString  payload;  // 10
};

static const std::unordered_set<std::string> s_allowlist =
{

    // Round 139: list pruned to events that actually exist in the human
    // behavior graphs (HKXExtract string-table validation) — the removed
    // names (LocoStateChange, JumpLandHard, weaponDrawn/Holstered, getUp,
    // H2H/power/recoil variants) were never real events.
    CoSyncAnimUtils::Events::MoveStart,
    CoSyncAnimUtils::Events::MoveStop,


    CoSyncAnimUtils::Events::JumpStart,
    CoSyncAnimUtils::Events::JumpLand,


    CoSyncAnimUtils::Events::SneakEnter,
    CoSyncAnimUtils::Events::SneakExit,


    CoSyncAnimUtils::Events::WeaponDraw,
    CoSyncAnimUtils::Events::WeaponSheathe,


    CoSyncAnimUtils::Events::ReloadStart,
    CoSyncAnimUtils::Events::ReloadEnd,


    CoSyncAnimUtils::Events::BlockStart,
    CoSyncAnimUtils::Events::BlockStop,


    CoSyncAnimUtils::Events::StaggerStart,
    CoSyncAnimUtils::Events::StaggerStop,


    CoSyncAnimUtils::Events::AttackStart,
    CoSyncAnimUtils::Events::AttackStop,
    CoSyncAnimUtils::Events::AttackStartMelee,
    CoSyncAnimUtils::Events::AttackBashStart,
    CoSyncAnimUtils::Events::AttackStartAuto,


    CoSyncAnimUtils::Events::SprintStart,
    CoSyncAnimUtils::Events::SprintStop,


    CoSyncAnimUtils::Events::Activate,
    CoSyncAnimUtils::Events::IdleStop,


    CoSyncAnimUtils::Events::PowerArmorEnter,
    CoSyncAnimUtils::Events::PowerArmorExit,


    CoSyncAnimUtils::Events::SwimStart,
    CoSyncAnimUtils::Events::SwimStop,


    CoSyncAnimUtils::Events::DeathStart,


    CoSyncAnimUtils::Events::KillMoveStart,
    CoSyncAnimUtils::Events::KillMoveEnd,


    CoSyncAnimUtils::Events::FurnitureEnter,
    CoSyncAnimUtils::Events::FurnitureExit,


    CoSyncAnimUtils::Events::VATSStart,
    CoSyncAnimUtils::Events::VATSEnd,
};

// LOCAL-PLAYER event forwarding set. Every name here was verified present as a
// real graph-received event (lowercase-first) in the union of all 44 human
// behavior graphs under HKXExtract/Meshes/Actors/Character/Behaviors via
// `strings`. These are the events the source player's OWN graph emits that
// TickAnimVars does NOT already dispatch on the proxy -- so forwarding them and
// replaying on the proxy (which the AE| receive path already does by SendEvent)
// makes the proxy mirror the full animation vocabulary: aim/sighted, ready vs
// gun-down posture, hit reactions and flinches, jump sub-states, walk-vs-run
// starts, reload state machine, melee variants, stumble, combat state, etc.
// Kept disjoint from the TickAnimVars-driven events (moveStart, sprintStart,
// jumpStart/jumpLand, attackStart, weaponFire, reloadStart/End, block/stagger
// Start/Stop, sneak, throw) so nothing double-fires.
static const std::unordered_set<std::string> s_playerForwardEvents =
{
    // Sneak transitions -- the REAL events the source graph fires to enter/exit
    // the sneak sub-graph (sneakStateEnter alone, which TickAnimVars sent, was
    // not moving the graph; forward what the source actually emits).
    "sneakStart", "sneakStop",
    // Aim / posture
    "sightedStateEnter", "sightedStateExit",
    "readyStateEnter",   "readyStateExit",
    "gunDownStateEnter", "gunDownStateExit",
    "combatStateStart",  "combatStateStop",
    // Hit reactions + flinches (the directional react clips)
    "hitReactionStart",  "hitReactionStop",
    "hitFlinchStart",    "hitFlinchStop",
    // Attack state machine + charge/release + melee variants
    "attackStateEnter",  "attackStateExit",
    "attackRelease",     "chargeRelease",
    "meleeattackStart",  "meleeattackPowerStart", "meleeattackSneakStart",
    "boltChargeStart",
    // Reload state machine
    "reloadStateEnter",  "reloadStateExit", "reloadComplete",
    // NOTE: jump sub-states (jumpFall/jumpLandSoft/jumpImpactLand/jumpEnd/
    // jumpStateEnter/Exit) are deliberately NOT forwarded -- they fire on tiny
    // terrain drops while running/sprinting and replayed as mini-jumps on the
    // proxy (reported "mini jump while sprinting"). Real jumps are driven by
    // TickAnimVars' jumpStart/jumpLand off the actual in-air actorState.
    // Locomotion refinements + transitions
    "walkStart", "runStart", "walkStartRear", "runStartRear",
    "transMoveStart", "transMoveStop", "standStart", "sprintToSneak",
    // Stagger/stumble tail
    "stumbleStart", "stumbleStop", "staggerExit",
    "recoilStop", "recoilExit",
    // Sit / furniture -- forwarded because TickAnimVars can't fire furnitureEnter
    // on the proxy (it has no furniture REF); the source's graph DOES emit these
    // when it sits/stands, so replaying them puts the proxy into the sit pose.
    "furnitureEnter", "furnitureExit",
    "sitStartFromStand", "sitStartFromStandCombat", "sitStartFromStandInstant",
    // Sit / equip / idle-loop / panic
    "sitStop", "weapEquip", "unEquip",
    "idleLoopingStart", "panicStart", "cowerStart", "cowerStop",
};

// (0.3.5) The old NPC OUTPUT-event set that lived here is gone: its names are
// what a graph emits, and a puppet's graph accepted none of them (0 of 828,
// 2026-09-29). NPC events are captured on the input side now
// (Detour_ActorNotifyGraph, IsForwardableNpcInput).

using ReceiveEventFn = int(__fastcall*)(void* thisPtr,
                                        BSAnimationGraphEvent* evnt,
                                        void*                  dispatcher);

static ReceiveEventFn s_origReceiveEvent = nullptr;
static bool           s_hookInstalled    = false;
static void*          s_hookTarget       = nullptr;


static std::atomic<uint32_t> s_fwdCount{ 0 };
static std::atomic<uint32_t> s_seenCount{ 0 };

// Echo guard, same shape as CoSyncEquipHook's tls_cs_equip_applying: set while
// the receive side replays a remote event onto a proxy, so any event the graph
// synchronously emits back into the sink is not re-broadcast.
static thread_local bool tls_cs_anim_applying = false;

void CoSyncAnimEventHook::SetApplyingRemote(bool applying)
{
    tls_cs_anim_applying = applying;
}

// SEH-isolated tag copy. Split into its own noinline function because the
// caller builds std::string/char buffers that need C++ unwinding, which cannot
// share a frame with __try (C2712). noinline is load-bearing under /GL+/LTCG.
__declspec(noinline) static bool SafeCopyTag(const BSAnimationGraphEvent* evnt,
                                             char* out, size_t outSize)
{
    __try
    {
        if (!evnt)
            return false;
        const char* tag = evnt->tag.c_str();
        if (!tag || tag[0] == '\0')
            return false;
        strncpy_s(out, outSize, tag, _TRUNCATE);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

__declspec(noinline) static bool SafeCopyTagString(const BSFixedString* ev, char* out, size_t outSize)
{
    __try
    {
        if (!ev)
            return false;
        const char* tag = ev->c_str();
        if (!tag || tag[0] == '\0')
            return false;
        strncpy_s(out, outSize, tag, _TRUNCATE);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Only the machine that actually DRIVES an entity may broadcast its events --
// same ownership test HostSendNpcUpdates_Impl uses for position/anim-var
// updates. This is what keeps proxies from echoing events straight back, and
// what implements "mirror what the host sees, host itself changes nothing".
static bool LocallyDrivesEntity(uint32_t entityID)
{
    const auto& states = g_CoSyncPlayerManager.GetStates();
    auto it = states.find(entityID);
    if (it == states.end() || !it->second.hasCreate)
        return false;

    return CoSyncNpcWorldSync::IsWorldActorEntityID(entityID)
        ? it->second.isLocallyOwned
        : (CoSyncNet::IsHost() && it->second.hostAuthoritative);
}

static void TryForwardEvent(Actor* actor, const char* tag)
{
    LOG_REQUIREMENT("TryForwardEvent", "Needs the actor to be a registered NPC this machine drives, and the event name to already be on the approved list.");

    const uint32_t eid = CoSyncNpcWorldSync::GetEntityIDForActor(actor);
    if (eid == 0)
        return;

    if (!LocallyDrivesEntity(eid))
        return;

    char buf[128];
    std::snprintf(buf, sizeof(buf), "AE|%u|%s", eid, tag);
    CoSyncTransport::Send(std::string(buf));

    const uint32_t n = ++s_fwdCount;
    if (n <= 30 || n % 300 == 0)
        LOG_UPDATED("[AnimEventHook] TX '%s' entity=%u fwd#%u (seen#%u)",
            tag, eid, n, s_seenCount.load());
}

// THREAD SAFETY: ReceiveEvent's calling thread is unverified and may be an
// animation worker thread, not the game thread. The old hook did unordered_map
// lookups + CoSyncTransport::Send inline in the hook body, which raced
// game-thread writes and could stall the anim update (the A-pose this hook was
// disabled for). So the hook body now does the absolute minimum -- push the tag
// string into a mutex-guarded queue -- and the actual Send happens on the game
// thread in DrainLocalPlayerAnimEvents(), drained once per frame.
static std::mutex               s_playerTagMutex;
static std::vector<std::string> s_pendingPlayerTags;

struct NpcTag
{
    uint32_t formID;
    char     tag[40];
};
static std::mutex          s_npcTagMutex;
static std::vector<NpcTag> s_pendingNpcTags;

__declspec(noinline) static uint32_t SafeActorFormID(Actor* actor)
{
    __try { return actor ? actor->formID : 0u; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
}

static void EnqueuePlayerTag(const char* tag)
{
    std::lock_guard<std::mutex> lk(s_playerTagMutex);
    // Bound the queue so a flood (or a stalled drain) can never grow unbounded.
    if (s_pendingPlayerTags.size() < 64)
        s_pendingPlayerTags.emplace_back(tag);
}

// GAME THREAD ONLY. Drains queued local-player events and forwards each under my
// entityID. The AE| receive path (CoSyncNet) resolves the entity and SendEvent's
// the tag onto the proxy -- a player proxy is looked up by GetPlayerByEntityID
// exactly like an NPC proxy, so no extra receive-side wiring is needed.
void DrainLocalPlayerAnimEvents()
{
    std::vector<std::string> tags;
    {
        std::lock_guard<std::mutex> lk(s_playerTagMutex);
        if (s_pendingPlayerTags.empty())
            return;
        tags.swap(s_pendingPlayerTags);
    }

    if (!CoSyncNet::IsConnected())
        return;
    const uint32_t myEid = CoSyncNet::GetMyEntityID();
    if (myEid == 0)
        return;

    for (const std::string& tag : tags)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "AE|%u|%s", myEid, tag.c_str());
        CoSyncTransport::Send(std::string(buf));

        const uint32_t n = ++s_fwdCount;
        if (n <= 30 || n % 300 == 0)
            LOG_UPDATED("[AnimEventHook] PLAYER TX '%s' myEntity=%u fwd#%u", tag.c_str(), myEid, n);
    }
}

void DrainNpcAnimEvents()
{
    std::vector<NpcTag> tags;
    {
        std::lock_guard<std::mutex> lk(s_npcTagMutex);
        if (s_pendingNpcTags.empty())
            return;
        tags.swap(s_pendingNpcTags);
    }
    if (!CoSyncNet::IsConnected())
        return;

    // Per-entity budget: 20 events per second at most.
    static std::unordered_map<uint32_t, std::pair<double, int>> s_budget;
    const double now = static_cast<double>(GetTickCount64()) / 1000.0;

    for (const NpcTag& t : tags)
    {
        TESForm* f = LookupFormByID(t.formID);
        if (!f || f->formType != kFormType_ACHR)
            continue;
        Actor* actor = static_cast<Actor*>(f);
        const uint32_t eid = CoSyncNpcWorldSync::GetEntityIDForActor(actor);
        if (eid == 0 || !LocallyDrivesEntity(eid))
            continue;

        // A dead NPC's graph still receives engine events (death, ragdoll
        // cleanup); nothing it does after death is replicated as an action.
        if (CoSyncGameAPI::IsDeadOrDying(actor))
            continue;

        if (std::strcmp(t.tag, "attackStart") == 0)
        {
            // Guns: the SHOT replay (CoSyncCombatAuthority) sends attackStart
            // with the engine shot. Only melee here.
            TESForm* w = LookupFormByID(CoSyncPlayerManager::EquippedWeaponFormID(actor));
            const bool melee = !w || w->formType != kFormType_WEAP ||
                               static_cast<TESObjectWEAP*>(w)->weapData.ammo == nullptr;
            if (!melee)
                continue;
        }

        auto& b = s_budget[eid];
        if (now - b.first >= 1.0) { b.first = now; b.second = 0; }
        if (++b.second > 20)
            continue;

        char buf[96];
        std::snprintf(buf, sizeof(buf), "AE|%u|%s", eid, t.tag);
        CoSyncTransport::Send(std::string(buf));
        const uint32_t n = ++s_fwdCount;
        if (n <= 30 || n % 300 == 0)
            LOG_UPDATED("[AnimEventHook] NPC TX '%s' entity=%u fwd#%u", t.tag, eid, n);
    }
}

// ── NPC graph INPUT capture (0.3.5) ─────────────────────────────────────────
// IDA 1.11.240 (2026-09-29): an actor's animation events enter its graph
// through IAnimationGraphManagerHolder::NotifyAnimationGraphImpl, slot 1 of the
// holder subobject vtable at actor+0x48 (Actor: 0x2568DB0, a thunk to
// sub_140819E60, which calls BSAnimationGraphManager::NotifyAnimationGraph
// sub_14130EED0 and returns whether the graph took the event).
// PlayerCharacter has its own holder vtable (0x256D358), so swapping this slot
// touches non-player actors only, with no code patched. Every event the
// engine (AI, combat, actions, hits, staggers) sends an NPC's graph passes
// here; what the OWNER's graph accepted is exactly what a puppet elsewhere
// needs to receive to play the same action. CoSync's own sends onto proxies
// (CoSyncAnimUtils::SendEvent, same slot) are excluded by the echo guard.
namespace
{
    constexpr uintptr_t kRVA_ActorHolderVtbl   = 0x02568DB0;
    constexpr uintptr_t kRVA_HolderNotifyThunk = 0x00CB2D90;
    using HolderNotify_t = bool(__fastcall*)(void* holder, BSFixedString* ev);
    HolderNotify_t        s_origHolderNotify = nullptr;
    std::atomic<bool>     s_inputCaptureOn{ false };
    std::atomic<uint32_t> s_inputSeen{ 0 };
    std::atomic<uint32_t> s_inputQueued{ 0 };

    // Gameplay actions a remote copy must show. Matched on the lower-cased
    // name: first a deny list (states a puppet reaches through its own
    // replicated state -- movement, weapon draw, death, furniture, dialogue --
    // or must never enter from a bare event), then the categories.
    bool IsForwardableNpcInput(const char* name)
    {
        char lc[64];
        size_t n = 0;
        for (; name[n] && n < sizeof(lc) - 1; ++n)
            lc[n] = static_cast<char>((name[n] >= 'A' && name[n] <= 'Z') ? name[n] + 32 : name[n]);
        lc[n] = '\0';
        static const char* const kDeny[] = {
            "death", "ragdoll", "bleedout", "killmove", "paired", "furniture", "sit", "getup",
            "weapdraw", "weapondraw", "weaponsheathe", "weapequip", "unequip", "equip",
            "dialogue", "initiate", "powerarmor", "graphdeleting", "idleforcedefault",
            "movestart", "movestop", "turn", "sprint", "walk", "footleft", "footright",
        };
        for (const char* d : kDeny)
            if (std::strstr(lc, d)) return false;
        static const char* const kAllow[] = {
            "attack", "bash", "block", "reload", "stagger", "recoil", "hitreact", "flinch",
            "sighted", "aim", "ready", "gundown", "throw", "grenade", "mine", "jump", "sneak",
            "dodge", "cower", "panic", "stumble", "melee", "charge", "surrender", "combat",
        };
        for (const char* a : kAllow)
            if (std::strstr(lc, a)) return true;
        return false;
    }

    bool __fastcall Detour_ActorNotifyGraph(void* holder, BSFixedString* ev)
    {
        const bool accepted = s_origHolderNotify ? s_origHolderNotify(holder, ev) : false;
        if (!accepted || tls_cs_anim_applying || !s_inputCaptureOn.load(std::memory_order_relaxed))
            return accepted;
        ++s_inputSeen;
        char tag[40];
        if (!SafeCopyTagString(ev, tag, sizeof(tag)) || !IsForwardableNpcInput(tag))
            return accepted;
        Actor* actor = reinterpret_cast<Actor*>(reinterpret_cast<uintptr_t>(holder) - 0x48);
        const uint32_t fid = SafeActorFormID(actor);
        if (fid == 0)
            return accepted;
        std::lock_guard<std::mutex> lk(s_npcTagMutex);
        if (s_pendingNpcTags.size() < 256)
        {
            NpcTag t{};
            t.formID = fid;
            strncpy_s(t.tag, sizeof(t.tag), tag, _TRUNCATE);
            s_pendingNpcTags.push_back(t);
            ++s_inputQueued;
        }
        return accepted;
    }
}

void InstallNpcGraphInputCapture()
{
    LOG_REQUIREMENT("InstallNpcGraphInputCapture", "Plugin load or later. Swaps one verified vtable slot (Actor's animation-graph holder, slot 1) after checking it still points at the 1.11.240 thunk.");
    if (s_origHolderNotify)
        return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void** slot = reinterpret_cast<void**>(base + kRVA_ActorHolderVtbl + 1 * sizeof(void*));
    if (reinterpret_cast<uintptr_t>(*slot) != base + kRVA_HolderNotifyThunk)
    {
        LOG_ERROR("[AnimEventHook] Actor holder slot 1 = %p, expected base+0x%llX -- NPC input capture NOT installed",
            *slot, (unsigned long long)kRVA_HolderNotifyThunk);
        return;
    }
    DWORD oldProt = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt))
    {
        LOG_ERROR("[AnimEventHook] VirtualProtect on the Actor holder vtable failed (%lu)", GetLastError());
        return;
    }
    s_origHolderNotify = reinterpret_cast<HolderNotify_t>(*slot);
    *slot = reinterpret_cast<void*>(&Detour_ActorNotifyGraph);
    VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
    s_inputCaptureOn.store(true);
    LOG_INFO("[AnimEventHook] NPC graph INPUT capture installed (Actor holder vtable base+0x%llX slot 1)",
        (unsigned long long)kRVA_ActorHolderVtbl);
}

void LogNpcGraphInputCaptureStats()
{
    const uint32_t seen = s_inputSeen.exchange(0), queued = s_inputQueued.exchange(0);
    if (seen || queued)
        LOG_UPDATED("[AnimEventHook] NPC graph inputs last 10 s: accepted by graphs here=%u, forwardable queued=%u", seen, queued);
}

static int __fastcall HookedReceiveEvent(
    void*                  thisPtr,
    BSAnimationGraphEvent* evnt,
    void*                  dispatcher)
{
    LOG_REQUIREMENT("HookedReceiveEvent", "Needs this to be the real animation event listener so it can find the owning actor, and the original game function must stay valid so every event still gets processed normally.");
    CoSyncThreadDiag::Note(CoSyncThreadDiag::kAnimEvent);

    // Deliberately NOT forwarding the local player here: player animation sync
    // already works through TickAnimVars' state-based dispatch, and firing both
    // would double-dispatch the one path currently confirmed good in-game.
    // World NPCs have no equivalent coverage at all -- that is the gap this
    // hook exists to close.
    if (!tls_cs_anim_applying && CoSyncNet::IsConnected())
    {
        ++s_seenCount;

        // Cheapest possible gate first: is this the LOCAL player's own sink?
        // (pointer compare, no string work) -- so a non-player event pays almost
        // nothing. Only the local player's events are handled; the old NPC path
        // (TryForwardEvent) did map lookups + Send inline on this unverified
        // thread and is intentionally left off until NPC sync is revisited as
        // an enqueue-only body too.
        Actor* actor = reinterpret_cast<Actor*>(
            reinterpret_cast<uintptr_t>(thisPtr) - 0x38);
        if (actor == reinterpret_cast<Actor*>(CoSyncGetPlayer()))
        {
            char tag[64];
            if (SafeCopyTag(evnt, tag, sizeof(tag)) &&
                s_playerForwardEvents.count(tag) != 0)
            {
                // Enqueue only -- the Send happens on the game thread.
                EnqueuePlayerTag(tag);
            }
        }
        // World NPCs are no longer captured here. This sink sees what a graph
        // EMITS (attackStateEnter, sightedStateExit, ...); replayed as input on
        // a puppet, the graph has no transition for them: 0 of 828 NPC events
        // were accepted on the client (log 2026-09-29). NPCs are now captured on
        // the INPUT side, where the engine drives the graph (see
        // Detour_ActorNotifyGraph).
    }

    return s_origReceiveEvent
        ? s_origReceiveEvent(thisPtr, evnt, dispatcher)
        : 0;
}

void InstallAnimEventHook()
{
    LOG_REQUIREMENT("InstallAnimEventHook", "Must not already be installed. Needs a valid player with a working animation event listener to hook into.");
    if (s_hookInstalled)
        return;

    // Primary: read slot 1 out of the live player's +0x38 sink vtable, so the
    // hook self-adapts if the game is ever patched. Fallback: the disk-IDB
    // verified address, used when the player isn't constructed yet -- without
    // it a deferred install would simply never happen, and this hook only gets
    // one install attempt. Both are expected to resolve to the same function
    // (ReceiveEvent, shared by Actor and PlayerCharacter); a mismatch is logged
    // rather than silently trusted.
    static constexpr uintptr_t kReceiveEvent_RVA = 0x00CAB9A0;
    void* rvaTarget = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(GetModuleHandle(NULL)) + kReceiveEvent_RVA);

    void* target = nullptr;
    PlayerCharacter* pc = CoSyncGetPlayer();
    if (pc)
    {
        void*** sinkVtblPtr = reinterpret_cast<void***>(
            reinterpret_cast<uintptr_t>(pc) + 0x38);
        if (sinkVtblPtr && *sinkVtblPtr)
            target = (*sinkVtblPtr)[1];
    }

    if (target && target != rvaTarget)
        LOG_WARN("[AnimEventHook] sink slot1 %p != verified RVA %p — using the live vtable value",
            target, rvaTarget);

    if (!target)
    {
        target = rvaTarget;
        LOG_INFO("[AnimEventHook] player not ready — using verified ReceiveEvent RVA 0x%llX",
            (unsigned long long)kReceiveEvent_RVA);
    }

    s_hookTarget = target;


    MH_STATUS st = MH_CreateHook(
        target,
        reinterpret_cast<LPVOID>(&HookedReceiveEvent),
        reinterpret_cast<LPVOID*>(&s_origReceiveEvent)
    );

    if (st != MH_OK)
    {
        LOG_ERROR("[AnimEventHook] MH_CreateHook failed: status=%d  target=%p",
            (int)st, target);
        return;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK)
    {
        LOG_ERROR("[AnimEventHook] MH_EnableHook failed: status=%d", (int)st);
        MH_RemoveHook(target);
        return;
    }

    s_hookInstalled = true;
    LOG_INFO("[AnimEventHook] INSTALLED"
        "\n  animGraphEventSink @ player+0x38"
        "\n  ReceiveEvent @ %p"
        "\n  Allowlist: %zu events"
        "\n  NOTE: supplements TickAnimVars state-based dispatch (both active until verified)",
        target, s_allowlist.size());
}

void UninstallAnimEventHook()
{
    LOG_REQUIREMENT("UninstallAnimEventHook", "The hook must already be installed for there to be anything to remove.");
    if (!s_hookInstalled || !s_hookTarget)
        return;

    MH_DisableHook(s_hookTarget);
    MH_RemoveHook(s_hookTarget);

    s_hookInstalled    = false;
    s_origReceiveEvent = nullptr;
    s_hookTarget       = nullptr;

    LOG_INFO("[AnimEventHook] Uninstalled (forwarded %u events total)", s_fwdCount.load());
}
