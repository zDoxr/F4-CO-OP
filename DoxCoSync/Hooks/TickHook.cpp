#include "ITypes.h"
#include "Relocation.h"
#include "NiTypes.h"
#include "NiNodes.h"
#include "GameReferences.h"
#include "GameObjects.h"
#include "GameTypes.h"

#include "ConsoleLogger.h"
#include "LocalPlayerStateGlobals.h"
#include "LocalPlayerState.h"
#include "CoSyncAnimVarSystem.h"
#include "CoSyncPlayerAnimation.h"
#include "CoSyncPlayer.h"
#include "CoSyncAnimEventHook.h"

#include "CoSyncNet.h"
#include "CoSyncTimeSync.h"
#include "TickHook.h"
#include "CoSyncWorld.h"
#include "CoSyncLocalPlayer.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <unordered_map>
#include <mutex>
#include <vector>
#include "MinHook.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncThreadDiag.h"


extern RelocPtr<PlayerCharacter*> g_player;
extern PlayerCharacter* CoSyncGetPlayer();
extern LocalPlayerState g_localPlayerState;

// Round 117: g_lastTickStep (CoSyncNet.cpp) covers CoSyncNet::Tick()'s and
// CoSyncRuntime_TickGameThread()'s own explicit call chains in fine detail,
// but this file's hook callbacks (ActorUpdate_Hook, NpcAnimSync_Hook,
// SafeCallNpcAnimSyncOriginal, RemoteActorUpdate_Hook) are MinHook detours
// fired directly by the ENGINE's own per-actor update dispatch -- they never
// run as part of CoSync's own Tick chain, so g_lastTickStep never reflected
// them. That's why every NpcAnimSync crash log so far showed a generic
// RTGT/Tick step instead of anything from this file. Wiring it in here too,
// function by function, so the next crash (wherever it lands) says exactly
// which of these engine-driven callbacks -- and which call boundary inside
// it -- was actually live.
extern volatile const char* g_lastTickStep;

using ActorUpdateFn = __int64(__fastcall*)(PlayerCharacter* actor, void* arg2, void* arg3);
static ActorUpdateFn g_ActorUpdate_Original = nullptr;

static std::mutex                               s_remoteActorMutex;
static std::unordered_map<Actor*, CoSyncPlayer*> s_remoteActors;


static CoSyncPlayer* LookupRemotePlayer(Actor* actor)
{
    LOG_REQUIREMENT("LookupRemotePlayer", "Doesn't touch the game. The actor pointer is only used as a lookup key, never read.");
    std::lock_guard<std::mutex> lk(s_remoteActorMutex);
    auto it = s_remoteActors.find(actor);
    return (it != s_remoteActors.end()) ? it->second : nullptr;
}

static bool   g_hookInstalled   = false;
static double g_installTime     = 0.0;
static double g_lastFireTime    = 0.0;

AnimVarSnapshot g_latestLocalAnimVars{};

static bool   g_hasPrevPos = false;
static double g_prevTime = 0.0;
static double g_lastNetSendTime = 0.0;

static bool     g_localEntityBound = false;
static uint32_t g_localEntityID = 0;

static NiPoint3 g_prevPos{ 0.f, 0.f, 0.f };

static double GetNowSeconds()
{
    LOG_REQUIREMENT("GetNowSeconds", "Doesn't touch the game. Just reads the system clock, which is always available on Windows.");
    static double s_freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
        }();

    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / s_freq;
}

namespace
{
    // 2026-07-26: ActorUpdate_Hook had no death-check of its own -- it only
    // ever checked "is this actor a registered remote proxy" before either
    // forwarding to the real engine update or calling SaveAnimationVariables.
    // This used to run its OWN independent HP poll (SafeReadActorHP + a
    // local -1.f-as-dead check), separate from CoSynclocalplayer.cpp's own
    // poll of the exact same actor. That was the actual bug: this hook is
    // driven by the engine's own per-actor vtable dispatch while
    // CoSynclocalplayer.cpp's poll runs off a separate AddTaskPermanent main-
    // thread task -- two different call cadences sampling the same HP value
    // can and did disagree. Log evidence: this hook's independent poll caught
    // a single-tick positive HP reading 133ms before a crash, letting
    // ForceOverrideOnGameThread() fire (direct writes to the GameHour global
    // and Sky weather object) during what CoSynclocalplayer.cpp's own poll
    // still correctly saw as a fully-dead, non-respawned window (it never
    // logged RESPAWN detected that session) -- proof the engine's own
    // reload/respawn processing can flicker HP positive for a single frame
    // without the transition actually completing. Now delegates to
    // CoSyncLocalPlayer::IsLocallyConfirmedDead(), the single, debounced
    // source of truth (requires several consecutive positive readings before
    // trusting a respawn), instead of trusting its own one-off sample.
    bool IsLocalPlayerConfirmedDead(Actor*)
    {
        return CoSyncLocalPlayer::IsLocallyConfirmedDead();
    }
}

static __int64 __fastcall ActorUpdate_Hook(
    PlayerCharacter* actor,
    void* arg2,
    void* arg3)
{
    LOG_REQUIREMENT("ActorUpdate_Hook", "This runs once per frame for the player, once the hook is installed. The actor passed in could be the real local player or a remote player's stand-in -- both are handled here.");
    CoSyncThreadDiag::Note(CoSyncThreadDiag::kLocalActorUpdate);

    // UPDATE 2026-06-21 (round 79): per FO4_Wrld's independently-developed
    // "GHOST-BAIL" mitigation for this exact crash family (studied for
    // architecture/concept only, reimplemented independently here -- see
    // [[project_fo4wrld_reference]]) -- a PlaceAtMe-spawned remote-proxy
    // actor gets visited by the engine's own Update_PerFrame walker (this
    // function, reached via vtable[21]) every tick, which routes into the
    // anim-graph/locomotion evaluators that rounds 73/75/76/77 found
    // crashing on this actor's not-yet-fully-initialized sub-objects.
    // Rounds 76/77's SEH guards stop the crash but the engine just
    // re-enters the same broken path again on literally every subsequent
    // tick -- round 78 capped the resulting diagnostic-logging cost, but
    // the underlying retry-forever loop itself was still live. We don't
    // need or want the engine's own AI/locomotion update running on a
    // remote-controlled proxy at all -- its position is driven directly by
    // PositionRemoteActor and its animation variables are written directly
    // by TickAnimVars/CoSyncAnimVarSystem, never by anything Update_PerFrame
    // would otherwise decide. Skip calling the real engine update entirely
    // for any actor that resolves to a registered remote proxy; only an
    // actor that is NOT a remote proxy (i.e. the genuine local player, on
    // this shared-vtable path) still gets the real original call.
    g_lastTickStep = "TickHook:ActorUpdate_Hook:entry";

    if (actor)
    {
        CoSyncPlayer* remotePlayer = LookupRemotePlayer(reinterpret_cast<Actor*>(actor));
        if (remotePlayer)
        {
            g_lastTickStep = "TickHook:ActorUpdate_Hook:remote-proxy-skip";
            // FIXED round 97: this used to call remotePlayer->TickAnimVars()
            // here too. CoSyncPlayerManager::Tick() ALSO calls
            // ent.TickAnimVars() unconditionally once per frame for every
            // spawned entity (CoSyncPlayerManager.cpp, the m_playersByEntityID
            // loop) -- this hook fires independently whenever the engine's
            // own per-actor Update dispatch reaches this proxy, which is the
            // same "once per frame" cadence for a live actor. That's the
            // exact double-fire defect class CoSyncRuntime.cpp's own comment
            // already documents and fixed once (PlayerManager::Tick() being
            // called from two competing places), just via a different pair
            // of call sites that fix never touched: doubling
            // TickAnimVars()'s exponential-smoothing rate every frame
            // (m_smoothSpeed += (target - m_smoothSpeed) * kAlpha run twice
            // against the same unchanged target) and redundantly
            // re-applying LoadAnimationVariables. PlayerManager::Tick()'s
            // loop is the established single authoritative driver (see
            // CoSyncRuntime.cpp); this hook's job is purely to skip the real
            // engine Update for remote proxies (the GHOST-BAIL mitigation),
            // not to also drive animation ticking.
            return 0;
        }
    }


    g_lastTickStep = "TickHook:ActorUpdate_Hook:pre-engine-call";
    // 2026-07-26: this was the one engine-call site in this file with no SEH
    // around it (every sibling hook -- RemoteActorUpdate_Hook's original
    // call, SafeCallNpcAnimSyncOriginal, etc. -- already guards its original-
    // function call). A client crash landed 16+ frames deep inside pure
    // Fallout4.exe code with this hook's OWN last marker already at ":exit"
    // (i.e. after this call had already returned once), so this guard isn't
    // proven to be the actual fault site -- but it closes the one gap found
    // auditing this function, on the same principle as every other original-
    // call site in this codebase.
    __int64 result = 0;
    if (g_ActorUpdate_Original)
    {
        __try
        {
            result = g_ActorUpdate_Original(actor, arg2, arg3);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[TickHook] ActorUpdate_Hook: g_ActorUpdate_Original crashed -- caught");
            result = 0;
        }
    }
    g_lastTickStep = "TickHook:ActorUpdate_Hook:post-engine-call";

    if (!actor)
        return result;

    PlayerCharacter* localPlayer = CoSyncGetPlayer();
    if (!localPlayer || actor != localPlayer)
        return result;

    // 2026-07-26: mirrors CoSynclocalplayer.cpp's dead-player gate (same
    // curHP<=0.f check, -1.f fault sentinel excluded). That file's own gate
    // only covers its own Tick() -- this hook fires independently off the
    // engine's own per-actor Update dispatch and was still unconditionally
    // touching the behavior graph (SaveAnimationVariables) and capturing
    // position/anim state for a confirmed-dead local player every frame.
    // Does NOT skip the forwarded g_ActorUpdate_Original call above -- the
    // real engine update on the genuine local player is left alone even
    // while dead, since that's the game's own death/ragdoll/respawn
    // processing, not something safe to withhold.
    if (IsLocalPlayerConfirmedDead(actor))
    {
        g_lastTickStep = "TickHook:ActorUpdate_Hook:dead-skip";
        return result;
    }

    const double now = GetNowSeconds();
    g_lastFireTime = now;

    g_lastTickStep = "TickHook:ActorUpdate_Hook:SaveAnimationVariables";
    // Round 120 bisection: ruled out (re-enabled round 125, NpcAnimSync_Hook
    // confirmed as the sole cause -- this was never it).
    CoSyncAnimVarSystem::SaveAnimationVariables(actor, g_latestLocalAnimVars);

    // SAFE gun-state mapping diagnostic (2026-07-15): the behavior-graph
    // variables are index-shifted on next-gen so kisFiring etc. read 0, but
    // the engine's GunState nibble is ground-truth at actorState.flags2 bits
    // (>>14)&0xF (IDA: console GunState sub_1405AB810, reads *(actor+0x134)).
    // This only reads an integer already captured in the snapshot — no engine
    // dereference, cannot crash. Logs on change so an aim/fire/holster test
    // reveals which nibble value == aiming and which == firing, to drive the
    // real ranged aim/fire animation sync next.
    {
        static uint32_t s_prevGunState = 0xFFFFFFFFu;
        const uint32_t gunState = (g_latestLocalAnimVars.actorStateFlags2 >> 14) & 0xFu;
        if (gunState != s_prevGunState)
        {
            LOG_UPDATED("[GunStateDiag] LOCAL gunState %u -> %u  (flags2=0x%08X)  <-- note what you were doing (holster/draw/aim/fire)",
                s_prevGunState, gunState, g_latestLocalAnimVars.actorStateFlags2);
            s_prevGunState = gunState;
        }
    }

    if (!g_localEntityBound && CoSyncNet::IsConnected())
    {
        g_localEntityID = CoSyncNet::GetMyEntityID();
        g_localEntityBound = true;

        LOG_INFO(
            "[TickHook] Local entity bound: entityID=%u",
            g_localEntityID
        );
    }

    g_lastTickStep = "TickHook:ActorUpdate_Hook:parentCell-check";
    if (!actor->parentCell || actor->parentCell->formID == 0)
        return result;

    const NiPoint3 pos = actor->pos;
    const NiPoint3 rot = actor->rot;

    if (pos.x == 0.f && pos.y == 0.f && pos.z == 0.f)
        return result;

    NiPoint3 vel{ 0.f, 0.f, 0.f };
    if (g_hasPrevPos)
    {
        const double dt = now - g_prevTime;
        if (dt > 0.0001)
        {
            vel.x = (pos.x - g_prevPos.x) / float(dt);
            vel.y = (pos.y - g_prevPos.y) / float(dt);
            vel.z = (pos.z - g_prevPos.z) / float(dt);
        }
    }

    g_prevPos = pos;
    g_prevTime = now;
    g_hasPrevPos = true;

    g_localPlayerState.timestamp = now;
    g_localPlayerState.position = pos;
    g_localPlayerState.rotation = rot;
    g_localPlayerState.velocity = vel;

    g_localPlayerState.formID = actor->formID;
    g_localPlayerState.cellFormID = actor->parentCell->formID;

    g_lastTickStep = "TickHook:ActorUpdate_Hook:ForceOverrideOnGameThread";
    CoSyncTimeSync::ForceOverrideOnGameThread();

    g_lastTickStep = "TickHook:ActorUpdate_Hook:exit";
    return result;
}

void TickHook_RetryInstallIfNeeded()
{
    LOG_REQUIREMENT("TickHook_RetryInstallIfNeeded", "Needs to be called once per frame until the hook installs successfully. Waits for the game world to actually be ready, not just for a player object to exist, since one exists even at the main menu.");
    if (g_hookInstalled)
        return;

    static int s_retryCount = 0;
    ++s_retryCount;

    // Round 110: CoSyncGetPlayer() returning non-null only means a
    // PlayerCharacter object exists -- this is also true at the bare main
    // menu before any save is loaded, where the object has no parentCell
    // and isn't a real in-world actor. Every other Tick-pipeline gate in
    // this codebase (CoSyncNet::Init, CoSyncPlayerManager::Tick, etc.) keys
    // off CoSyncWorld::IsWorldReady() (parentCell+formID check) for exactly
    // this reason. TickHook didn't, so a full multiplayer host/connect test
    // run from the main menu (no save loaded) installed the vtable[21]
    // hook + AnimEventHook against that degenerate menu-context actor, which
    // then crashed fatally once the connection state changed and the
    // engine's own NpcAnimSync dispatch (Fallout4.exe+0xCA9520, already
    // flagged as crash-prone for not-fully-initialized actors -- see
    // SafeCallNpcAnimSyncOriginal's comment below) touched it. Confirmed via
    // g_lastTickStep staying frozen at its initial value through the whole
    // session: CoSyncNet::Init() never ran because IsWorldReady() was never
    // true, because the player never actually loaded into a world.
    if (!CoSyncGetPlayer() || !CoSyncWorld::IsWorldReady())
    {
        if (s_retryCount % 600 == 1)   // ~once every 10s at 60fps, avoid log spam
            LOG_INFO("[TickHook] Still waiting for player to exist in-world (attempt #%d)", s_retryCount);
        return;
    }

    LOG_INFO("[TickHook] Player now exists after %d frame(s) of waiting — installing TickHook", s_retryCount);
    // Round 123 bisection: ruled out (re-enabled round 125, NpcAnimSync_Hook
    // confirmed as the sole cause -- this was never it).
    InstallTickHook();
}

bool   TickHook_IsInstalled()        { LOG_REQUIREMENT("TickHook_IsInstalled", "Doesn't touch the game. Just reports whether the hook is installed."); return g_hookInstalled; }
bool   TickHook_HasFired()           { LOG_REQUIREMENT("TickHook_HasFired", "Doesn't touch the game. Just reports whether the hook has ever run."); return g_lastFireTime > 0.0; }
double TickHook_SecondsSinceInstall(){ LOG_REQUIREMENT("TickHook_SecondsSinceInstall", "Doesn't touch the game. Just does simple time math."); return g_hookInstalled ? (GetNowSeconds() - g_installTime) : 0.0; }
double TickHook_LastFireSeconds()    { LOG_REQUIREMENT("TickHook_LastFireSeconds", "Doesn't touch the game. Just reports a stored timestamp."); return g_lastFireTime; }

static ActorUpdateFn          g_RemoteActorUpdate_Original = nullptr;
static bool                   g_remoteActorHookInstalled   = false;

static __int64 __fastcall RemoteActorUpdate_Hook(
    PlayerCharacter* actor,
    void*            arg2,
    void*            arg3)
{
    LOG_REQUIREMENT("RemoteActorUpdate_Hook", "This is installed separately for each remote player's stand-in actor, and only runs for that specific actor.");
    CoSyncThreadDiag::Note(CoSyncThreadDiag::kRemoteActorUpdate);
    g_lastTickStep = "TickHook:RemoteActorUpdate_Hook:entry";

    CoSyncPlayer* player = nullptr;
    {
        std::lock_guard<std::mutex> lk(s_remoteActorMutex);
        auto it = s_remoteActors.find(reinterpret_cast<Actor*>(actor));
        if (it != s_remoteActors.end())
            player = it->second;
    }

    // 2026-08-21 DIAGNOSTIC -- 2026-08-20's bone-replication write-last apply
    // (below) never fired in the first live test (zero "post-orig bone
    // apply", zero "ApplySkeleton #" log lines on either host or client,
    // despite the hook install + remote-actor registration both succeeding).
    // This throttled entry-log settles the open question from that test:
    // does the engine's own per-actor Update dispatch actually reach vtable
    // slot 21 for a spawned proxy at all on this build (1.11.240.0, NOT the
    // 1.11.221.0 most of this codebase's comments assume -- confirmed live
    // 2026-08-20), the same way it reliably does for the local PlayerCharacter
    // via InstallTickHook/ActorUpdate_Hook? If this line never appears next
    // test, slot 21 firing for a Character-class proxy on THIS build is the
    // real open question, not anything downstream. ~3s per actor.
    if (player)
    {
        static std::unordered_map<Actor*, uint64_t> s_lastEntryLogMs;
        const uint64_t nowMs = GetTickCount64();
        Actor* key = reinterpret_cast<Actor*>(actor);
        auto& last = s_lastEntryLogMs[key];
        if (nowMs - last > 3000)
        {
            last = nowMs;
            LOG_INFO("[TickHook] RemoteActorUpdate_Hook FIRED for entity=%u actor=%p",
                player->entityID, static_cast<void*>(key));
        }
    }

    // UPDATE 2026-06-21 (round 79): see ActorUpdate_Hook's comment above for
    // the full rationale (FO4_Wrld's independently-developed "GHOST-BAIL"
    // mitigation, studied for concept only). This hook is installed
    // per-actor only on confirmed registered remote proxies (see
    // InstallRemoteActorHook), so `player` being non-null here is the
    // common/expected case -- skip the real engine Update_PerFrame entirely
    // instead of letting it retry the same broken anim-graph/locomotion
    // path on every tick. The old "middleProcess null trick" (nulling
    // actor+0x300 around the original call) is now moot for this case since
    // the original is never called at all; left only for the player==null
    // fallback below, which should not normally be reached.
    //
    // FIXED round 97: this used to also call player->TickAnimVars() right
    // here, every time the engine's per-actor Update dispatch reached this
    // proxy (i.e. once per frame). CoSyncPlayerManager::Tick() ALSO calls
    // ent.TickAnimVars() unconditionally once per frame for every spawned
    // entity -- the same double-fire defect class CoSyncRuntime.cpp's
    // comment already documents and fixed once, via a different pair of
    // call sites that fix never touched. Removed; PlayerManager::Tick()'s
    // loop is the established single authoritative driver for animation
    // ticking, this hook's only job is to skip the real engine Update.
    // Round 127: used to `return 0` for registered proxies (round-79
    // "GHOST-BAIL", designed when proxies were fake/garbage actors and the
    // engine Update crashed on them). The per-actor engine Update is what
    // samples the behavior graph and poses the skeleton — skipping it left
    // the now-REAL proxies in a permanent A-pose regardless of the anim vars
    // we write. Proxies are real actors since the spawn fix; run the real
    // update for them like any other NPC.
    if (player)
        g_lastTickStep = "TickHook:RemoteActorUpdate_Hook:remote-proxy-engine-update";
    else
        g_lastTickStep = "TickHook:RemoteActorUpdate_Hook:pre-engine-call";

    __int64 result = g_RemoteActorUpdate_Original
        ? g_RemoteActorUpdate_Original(actor, arg2, arg3)
        : 0;
    g_lastTickStep = "TickHook:RemoteActorUpdate_Hook:post-engine-call";

    // 2026-08-20 -- bone-replication write-last apply (FO4_Wrld port). This
    // runs POST the real engine Update call above, i.e. AFTER the engine's
    // own anim graph has written this frame's bone poses -- our write here
    // is therefore always the LAST thing before the render bake, beating the
    // engine's own graph write instead of racing it. Mirrors FO4_Wrld's
    // detour_actor_update_perframe (npc_ai_suppress.cpp): call original,
    // THEN apply_npc_pose_to_actor. See project-cosync-bone-replication-fix
    // memory for why this write-order gap was suspected as the root cause
    // behind persistent quality issues despite every individual piece of the
    // old pipeline (capture, matching, scale-strip, detach guard) measuring
    // clean in isolation.
    //
    // CoSyncPlayer::TickAnimVars must NOT also call ApplySkeleton for this
    // actor when this hook is installed (see IsRemoteActorHookInstalled) --
    // that would double-apply the pose once per frame (advances the nlerp
    // smoothing state twice, the same double-fire defect class as round 97's
    // TickAnimVars double-call, just for a different pair of call sites).
    // 2026-08-21 DIAGNOSTIC -- logs the FULL gate state once per actor per
    // ~3s regardless of outcome, so a failing condition here is visible
    // instead of silently falling through to nothing (the exact blind spot
    // that made the first live test unreadable -- zero apply logs told us
    // nothing about WHICH stage failed: hook never firing, wrong createType/
    // actorRef, ini gate, or no data ever arriving).
    if (player)
    {
        static std::unordered_map<Actor*, uint64_t> s_lastGateLogMs;
        const uint64_t nowMs = GetTickCount64();
        Actor* key = reinterpret_cast<Actor*>(actor);
        auto& last = s_lastGateLogMs[key];
        if (nowMs - last > 3000)
        {
            last = nowMs;
            const bool typeOk   = (player->createType == CoSyncEntityType::Player);
            const bool refOk    = (player->actorRef == reinterpret_cast<Actor*>(actor));
            const bool replOn   = CoSyncPlayerAnimation::IsBoneReplicationEnabled();
            CoSyncPlayerAnimation::SkeletonCapture probeCap{};
            const bool hasData  = CoSyncPlayerAnimation::GetReceivedSkeleton(player->entityID, probeCap);
            LOG_INFO("[TickHook] bone-apply gate entity=%u typeOk=%d refOk=%d replOn=%d hasData=%d",
                player->entityID, typeOk ? 1 : 0, refOk ? 1 : 0, replOn ? 1 : 0, hasData ? 1 : 0);
        }
    }

    if (player
        && player->createType == CoSyncEntityType::Player
        && player->actorRef == reinterpret_cast<Actor*>(actor)
        && CoSyncPlayerAnimation::IsBoneReplicationEnabled())
    {
        g_lastTickStep = "TickHook:RemoteActorUpdate_Hook:bone-apply";
        CoSyncPlayerAnimation::SkeletonCapture recvCap{};
        if (CoSyncPlayerAnimation::GetReceivedSkeleton(player->entityID, recvCap))
        {
            static std::unordered_map<uint32_t, bool> s_firstFireLogged;
            if (!s_firstFireLogged[player->entityID])
            {
                s_firstFireLogged[player->entityID] = true;
                LOG_INFO("[TickHook] RemoteActorUpdate_Hook: post-orig bone apply first fired for entity=%u",
                    player->entityID);
            }
            CoSyncPlayerAnimation::ApplySkeleton(reinterpret_cast<Actor*>(actor), recvCap);
        }
        g_lastTickStep = "TickHook:RemoteActorUpdate_Hook:post-bone-apply";
    }

    return result;
}

void InstallRemoteActorHook(Actor* actor)
{
    LOG_REQUIREMENT("InstallRemoteActorHook", "Needs a valid, live actor to hook into. Uses the same per-frame update slot as the local player's own hook.");
    // 2026-08-20 RE-ENABLED: round 133 disabled this because, at the time,
    // RemoteActorUpdate_Hook was a pure passthrough that provided nothing
    // (the earlier GHOST-BAIL logic that justified it had already been
    // removed at round 127) -- so it was pure risk for zero benefit. It now
    // has a real job: applying the received bone-replication pose POST the
    // real engine Update call, i.e. write-last, the same architecture
    // FO4_Wrld's detour_actor_update_perframe uses (see the comment at the
    // top of RemoteActorUpdate_Hook). vtable slot 21 is the SAME slot
    // InstallTickHook already uses successfully on the local PlayerCharacter
    // on this exact running build (confirmed live via that hook's own
    // steady fire history) -- this call site was already wired at both its
    // existing callers (CoSyncPlayerManager.cpp companion spawn,
    // CoSyncSpawnTasks.cpp player-proxy spawn) the whole time round 133 was
    // active, they just hit the early return above.
    if (!actor)
        return;


    if (g_remoteActorHookInstalled)
        return;

    uintptr_t* vtbl = *(uintptr_t**)actor;
    if (!vtbl)
    {
        LOG_WARN("[TickHook] InstallRemoteActorHook: actor vtable is NULL");
        return;
    }

    PlayerCharacter* localPlayer = CoSyncGetPlayer();
    if (localPlayer)
    {
        uintptr_t* playerVtbl = *(uintptr_t**)localPlayer;
        if (vtbl == playerVtbl)
        {
            LOG_INFO("[TickHook] Remote actor shares PlayerCharacter vtable — no extra hook needed");
            g_remoteActorHookInstalled = true;
            return;
        }
    }

    constexpr UInt32 kIndex = 21;
    uintptr_t origFn = vtbl[kIndex];
    if (!origFn)
    {
        LOG_WARN("[TickHook] Remote actor vtable[21] is NULL");
        return;
    }


    if (origFn == reinterpret_cast<uintptr_t>(&RemoteActorUpdate_Hook))
    {
        LOG_INFO("[TickHook] Remote actor vtable[21] already points to our hook");
        g_remoteActorHookInstalled = true;
        return;
    }

    g_RemoteActorUpdate_Original = reinterpret_cast<ActorUpdateFn>(origFn);

    DWORD oldProt{};
    VirtualProtect(&vtbl[kIndex], sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProt);
    vtbl[kIndex] = reinterpret_cast<uintptr_t>(&RemoteActorUpdate_Hook);
    VirtualProtect(&vtbl[kIndex], sizeof(uintptr_t), oldProt, &oldProt);

    g_remoteActorHookInstalled = true;
    LOG_INFO("[TickHook] Remote actor vtable hook installed (vtbl=%p origFn=%p vtbl[21]->hook)",
        (void*)vtbl, (void*)origFn);
}

bool IsRemoteActorHookInstalled()
{
    LOG_REQUIREMENT("IsRemoteActorHookInstalled", "Doesn't touch the game. Just reports whether the hook is installed.");
    return g_remoteActorHookInstalled;
}

void RegisterRemoteActor(Actor* actor, CoSyncPlayer* player)
{
    LOG_REQUIREMENT("RegisterRemoteActor", "Needs a valid actor and a valid player. The caller is responsible for calling UnregisterRemoteActor before the player object goes away.");
    if (!actor || !player)
        return;

    std::lock_guard<std::mutex> lk(s_remoteActorMutex);
    s_remoteActors[actor] = player;
    LOG_INFO("[TickHook] Registered remote actor %p entity=%u for game-thread injection",
        (void*)actor, player->entityID);
}

void UnregisterRemoteActor(Actor* actor)
{
    LOG_REQUIREMENT("UnregisterRemoteActor", "Needs a valid actor. It's only used as a lookup key here, never read.");
    if (!actor)
        return;

    std::lock_guard<std::mutex> lk(s_remoteActorMutex);
    size_t removed = s_remoteActors.erase(actor);
    if (removed)
        LOG_INFO("[TickHook] Unregistered remote actor %p", (void*)actor);
}

bool IsRegisteredRemoteActor(Actor* actor)
{
    if (!actor)
        return false;
    std::lock_guard<std::mutex> lk(s_remoteActorMutex);
    return s_remoteActors.find(actor) != s_remoteActors.end();
}

void InstallTickHook()
{
    LOG_REQUIREMENT("InstallTickHook", "Needs a real, loaded-in player character to hook onto.");
    LOG_INFO("[TickHook] Installing PlayerCharacter update hook");

    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc)
    {
        LOG_ERROR("[TickHook] g_player is NULL");
        return;
    }

    uintptr_t* vtbl = *(uintptr_t**)pc;
    if (!vtbl)
    {
        LOG_ERROR("[TickHook] Player vtable is NULL");
        return;
    }

    constexpr UInt32 kIndex = 21;
    uintptr_t origFn = vtbl[kIndex];
    if (!origFn)
    {
        LOG_ERROR("[TickHook] vtbl[%u] is NULL", kIndex);
        return;
    }

    if (origFn == reinterpret_cast<uintptr_t>(&ActorUpdate_Hook))
    {
        LOG_WARN("[TickHook] vtable[21] already patched (double-install detected) — resetting state only");
        g_hasPrevPos = false;
        g_prevTime = 0.0;
        g_lastNetSendTime = 0.0;
        g_prevPos = { 0.f, 0.f, 0.f };
        g_localEntityBound = false;
        g_localEntityID = 0;
        g_latestLocalAnimVars = AnimVarSnapshot{};
        return;
    }

    g_ActorUpdate_Original = reinterpret_cast<ActorUpdateFn>(origFn);

    LOG_INFO("[TickHook] Saving original vtable[21]=%p (pc=%p)", (void*)origFn, (void*)pc);

    DWORD oldProt{};
    VirtualProtect(&vtbl[kIndex], sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProt);
    vtbl[kIndex] = reinterpret_cast<uintptr_t>(&ActorUpdate_Hook);
    VirtualProtect(&vtbl[kIndex], sizeof(uintptr_t), oldProt, &oldProt);


    g_hasPrevPos = false;
    g_prevTime = 0.0;
    g_lastNetSendTime = 0.0;
    g_prevPos = { 0.f, 0.f, 0.f };

    g_localEntityBound = false;
    g_localEntityID = 0;

    g_hookInstalled = true;
    g_installTime   = GetNowSeconds();
    g_lastFireTime  = 0.0;
    g_latestLocalAnimVars = AnimVarSnapshot{};

    LOG_INFO("[TickHook] TickHook installed successfully (vtable index %u)", kIndex);

    // RE-ENABLED 2026-08-02. The 2026-07-15 disable said "(*(player+0x38))[1] is
    // the WRONG function on NG" -- that was a misdiagnosis, disproven against
    // Fallout4_disk.i64. The slot really is ReceiveEvent 0xCAB610, and it is
    // SHARED by PlayerCharacter and plain Actor (.rdata xrefs land in both sink
    // vtables, 0x142560C98 and 0x142565240), so one hook legitimately sees every
    // actor's events. The garbage tags and the host crash both came from
    // BSAnimationGraphEvent being declared with `tag` at +0x00 when the engine
    // puts it at +0x08 -- an 8-byte struct error, now fixed in the hook. The
    // "event this pointers were millions of bytes from the player" observation
    // was correct AND expected: those were other actors' sinks.
    // A-pose was separately root-caused twice since ([0.0.14] RefreshActor3D
    // gate, [0.0.21] tick-loop applying to host-owned actors), so this hook is
    // no longer the suspect for it -- but it was accused of exactly that, so
    // watch for A-pose specifically on this build's first test.
    //
    // 2026-08-02, SAME NIGHT: re-disabled after that first test. Raiders A-posed
    // on BOTH host and client. The struct fix above is correct and stays, but
    // the install goes back off as a clean bisection, because the one mechanism
    // this hook could plausibly break things through was never ruled out:
    // ReceiveEvent's calling thread is unverified, and the capture path did
    // unordered_map lookups (GetEntityIDForActor / GetStates) plus a
    // CoSyncTransport::Send inline. If that runs on an animation worker thread,
    // those race with game-thread writes and could stall the animation update --
    // which looks exactly like A-pose.
    // The logs did clear the hook of the OTHER candidate mechanisms: host RX of
    // AE| was 0 (it never applied a remote event to its own actors), host graphs
    // stayed alive (1241 events observed), and the host never disabled/cloned
    // its own NPCs.
    // RE-ENABLED 2026-08-10. Both conditions the disable required are now met:
    // (a) the hook body is ENQUEUE-ONLY -- it does a single pointer compare and,
    //     for the local player only, pushes the tag into a mutex-guarded queue;
    //     the map lookups and CoSyncTransport::Send were moved off the hook
    //     thread entirely into DrainLocalPlayerAnimEvents(), drained once per
    //     frame from TickHook_CaptureLocalAnimVars (game thread). So the A-pose
    //     race (worker-thread Send/map access) can no longer happen.
    // (b) the forwarded event names were rebuilt from the REAL graph string
    //     tables (strings over all 44 Character/Behaviors/*.hkx), replacing the
    //     guessed names that matched almost nothing. The NPC path stays off
    //     (it needs the same enqueue-only treatment before it comes back).
    InstallAnimEventHook();
}

void TickHook_CaptureLocalAnimVars()
{
    LOG_REQUIREMENT("TickHook_CaptureLocalAnimVars", "Needs a valid player. The actual save only succeeds once the player's animation graph is ready.");
    // Game thread: flush any local-player anim events the ReceiveEvent hook
    // enqueued off-thread since last frame (see CoSyncAnimEventHook).
    DrainLocalPlayerAnimEvents();
    DrainNpcAnimEvents();

    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc)
        return;

    const bool ok = CoSyncAnimVarSystem::SaveAnimationVariables(
        reinterpret_cast<Actor*>(pc), g_latestLocalAnimVars);

    // Round 127: ground-truth capture of the engine's heading->matrix
    // convention, read off the local player's own root node. Drives the
    // proxy-facing fix (proxy renders rotated left relative to the remote
    // player's real heading): compare rot.z against the engine-authored
    // world rotation rows to derive the exact matrix layout to replicate in
    // PositionRemoteActor.
    {
        static int s_rotDiag = 0;
        if (++s_rotDiag % 600 == 1)
        {
            NiNode* prRoot = reinterpret_cast<TESObjectREFR*>(pc)->GetObjectRootNode();
            if (prRoot)
            {
                const NiMatrix43& m = prRoot->m_worldTransform.rot;
                LOG_INFO("[RotDiag] player rotZ=%.4f rootWorldRot=[%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]",
                    pc->rot.z,
                    m.data[0][0], m.data[0][1], m.data[0][2],
                    m.data[1][0], m.data[1][1], m.data[1][2],
                    m.data[2][0], m.data[2][1], m.data[2][2]);
            }
        }
    }

    static int s_captureDiag = 0;
    if (++s_captureDiag % 120 == 1)
        LOG_INFO("[TickHook] CaptureLocalAnimVars #%d: ok=%d isValid=%d speed=%.2f iState=%u bools=0x%llX",
            s_captureDiag, ok ? 1 : 0,
            g_latestLocalAnimVars.isValid ? 1 : 0,
            g_latestLocalAnimVars.floats[4],
            g_latestLocalAnimVars.integers[0],
            (unsigned long long)g_latestLocalAnimVars.booleans);
}

void TickHook_ScheduleBoneTasks()
{
    LOG_REQUIREMENT("TickHook_ScheduleBoneTasks", "Doesn't touch the game yet. Just a placeholder for bone-replication work still to come.");
    static int s_diagCount = 0;
    if (++s_diagCount % 600 == 1)
        LOG_INFO("[TickHook] ScheduleBoneTasks: frame#=%d (bone infra pending)", s_diagCount);
}

// RVA CORRECTED round 93: round 12's "fix" below (changing this from
// 0xCA9520 to 0xCA94F0) was itself the bug -- it broke a value that was
// already correct, the same disassembly-desync defect class found
// project-wide in round 90/93. Settled authoritatively this time via the
// most reliable method available (not caller-decompile pattern matching,
// which has repeatedly misled prior rounds): direct RTTI vtable-slot
// lookup. `??_7Actor@@6B@_4` (Actor::vftable_4) slot 8 and
// `??_7PlayerCharacter@@6B@_4` slot 8 both resolve to 0xCA9520 --
// ida_funcs.get_func() confirms 0xCA9520-0xCA98AE is a clean, valid
// function (entry_match=True), and its decompile is unambiguous: it
// repeatedly calls sub_1413264B0/sub_141326590/sub_141326670 against
// `&qword_1430E19xx` AnimGraph-variable-name globals, using `a1-72` (i.e.
// actor+0x48, the IAnimationGraphManagerHolder offset documented
// elsewhere in this file) as the actor pointer -- textbook per-actor
// AnimGraph variable propagation. PlayerCharacter's own slot 8
// (sub_140D7DB10) literally calls sub_140CA9520(a1, a2) first before
// adding player-only variables, confirming this is the shared
// Actor-base implementation both NPCs and the player dispatch through.
// 0xCA94F0 (round 12's value) and 0xCA9360 (a value briefly considered
// and rejected this round) are both unrelated mid-function/wrong-function
// addresses nearby in .text.
constexpr uintptr_t kNpcAnimSync_RVA = 0xCA98B0;

// UPDATE 2026-06-17: __declspec(noinline) is load-bearing (silent
// SEH-dropping under /GL+/LTCG inlining) and `noexcept` was removed --
// combined with __try/__except, `noexcept` let the optimizer eliminate
// the protected scope entirely (confirmed via disassembly on sibling
// copies of this exact pattern in CoSyncDoorHook.cpp/
// CoSyncPickupHook.cpp/CoSyncLockHook.cpp: just the raw load + jmp
// straight to retn, no protected scope at all).
__declspec(noinline) static void* SafeReadVoidPtr(uintptr_t addr)
{
    LOG_REQUIREMENT("SafeReadVoidPtr", "Safe to call with an address that turns out to be invalid -- wrapped in a crash guard.");
    __try { return *reinterpret_cast<void**>(addr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// 2026-06-17: verified via IDA against Fallout4.exe 1.11.221 -- the real
// function only takes 2 arguments (confirmed from its own decompile: `char
// __fastcall sub_140CA9520(__int64 a1, __int64 *a2)`), not 4. Kept the
// original 4-param shape here since it's harmless (x64 calling convention
// doesn't care about extra unused register args, and the real callee
// already ignores anything past a2), but documenting the real shape for
// accuracy. More importantly: traced where a1 actually comes from. This
// function is reached via virtual dispatch, not a direct call -- its only
// real caller (sub_140D7DB10) is itself referenced only as a vtable entry,
// specifically slot 8 of `Actor::vftable_4` (confirmed: `??_7Actor@@6B@_4`
// at RVA 0x2560CB0, this exact function at slot 8) AND slot 8 of
// `PlayerCharacter::vftable_4` (`??_7PlayerCharacter@@6B@_4` at RVA
// 0x2565258) -- the same function, shared by both, which is why this fires
// for regular NPC actors (what CoSync's proxies are) and not just the
// player. For a virtual call, `a1` (the `this` passed to the callee) is the
// interface subobject pointer the vtable belongs to -- i.e. exactly
// `actor + 0x48`, the same offset confirmed all session for
// IAnimationGraphManagerHolder (slots 1/4/19/20/21, used throughout
// CoSyncAnimationUtils.cpp/CoSyncAnimVarSystem.cpp). That means
// `matchAnimHolder` below (a1 == candidateActor + 0x48) is the check that
// actually matches; `matchActor` and `matchMP` (+0x300) can never be true
// for this specific function (a1 is never the raw actor pointer or
// actor+0x300) but are harmless dead checks, not bugs.
using NpcAnimSyncFn = void(__fastcall*)(void*, void*, void*, void*);
static NpcAnimSyncFn g_NpcAnimSync_Original = nullptr;

// UPDATE 2026-06-17: confirmed via a clean (non-interleaved, post-VEH-lock-fix)
// crash log that calling through to the real, unmodified engine function here
// is NOT safe to do unguarded -- a real crash (READ AV, fault=0x8, near-null)
// happens INSIDE Fallout4.exe's own code (RIP=Fallout4.exe+0xD322E4, called
// from Fallout4.exe+0xCA9676, which is +0x156 past our own hook's target RVA --
// i.e. genuinely the original function's own later code, reached normally via
// this call, not anything we patched). Every other engine-memory touch in this
// codebase is SEH-guarded; this call was the one exception. Root cause
// (whether it's bad args, or this code path simply isn't safe to reach this
// early during world load regardless of args) is not yet confirmed -- this is
// containment, not a fix for whatever upstream state is wrong.
// Round 113: the SEH guard below was already in place and STILL didn't
// catch this -- the same systemic "exception escapes __try/__except in this
// codebase" pattern documented repeatedly elsewhere this campaign (e.g.
// __RTDynamicCast, rounds 107-109). A live crash decoded the exact cause
// this time: the fault instruction is `mov r8,[rcx+8]` (the real engine
// function's own first access to its `a1` argument) and the crash registers
// show RCX=0 -- i.e. the engine is calling this dispatch with a1==nullptr.
// a1 is documented above as always `actor+0x48` (the AnimGraph holder
// offset) for a virtual call through a non-null actor, so a1==null here
// means the underlying actor itself was null at the call site -- an edge
// case the SEH guard was relying on catching after the fact instead of this
// just checking for it up front. Since SEH has now proven unreliable for
// this exact call twice (the guard already existed, this is a live test
// where it still didn't hold), check a1 before ever entering the guarded
// call -- removes the need for SEH to catch anything in this known case at
// all, rather than continuing to trust a guard already shown not to hold.
// Round 115 (diagnostic, not a fix): rounds 113 and 114 both tried
// recovering from the Fallout4.exe+0xD322E4 fault AFTER it happens (a narrow
// register patch, then the safer full real-unwind bailout) and BOTH made
// things worse -- an identical 16-deep cascade of garbage-RIP "bad function
// pointer" recoveries either way, ending in a fatal fault landing inside our
// own DLL's memory. Recovering blind a third time isn't useful without
// knowing what's actually different about the call that's faulting.
// Capturing raw state here instead of guessing again: plain volatile
// assignment, NO printf/formatting at all (round 112 already proved that
// logging from this exact hot MinHook trampoline crashes the CRT on its
// own, independent of this fault). CoSync_VEH's own logging is confirmed
// safe to call from (it logs successfully throughout the round 113/114
// cascades) -- it reads these globals out and reports them, so the next
// crash says exactly which actor/call this was instead of leaving us to
// guess from a raw RIP and a handful of leftover registers again.
volatile void* g_npcAnimSync_lastA1 = nullptr;
volatile uint32_t g_npcAnimSync_lastActorFormID = 0;
volatile uint64_t g_npcAnimSync_callIndex = 0;

__declspec(noinline) static uint32_t SafeReadActorFormID(void* actor)
{
    LOG_REQUIREMENT("SafeReadActorFormID", "Safe to call with a null or invalid actor pointer -- wrapped in a crash guard.");
    __try { return actor ? reinterpret_cast<Actor*>(actor)->formID : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; } // sentinel: the read itself faulted
}

// Round 119: split out from SafeCallNpcAnimSyncOriginal -- a std::unordered_map
// iterator is a non-POD local object, and that function contains __try/__except
// for the engine call below, which MSVC rejects (C2712, "cannot use __try in
// functions that require object unwinding") if any non-trivial-destructor local
// exists anywhere in the same function. Same isolation reason SafeReadActorFormID
// is its own noinline function.
static bool NpcAnimSync_SettleDelayElapsed(void* animGraphHolder)
{
    LOG_REQUIREMENT("NpcAnimSync_SettleDelayElapsed", "Doesn't touch the game. Just tracks timestamps -- the first time it sees a given actor it always reports 'not settled yet'.");
    constexpr double kNpcAnimSyncSettleDelay = 0.5;
    static std::unordered_map<void*, double> s_firstSeenTime;
    const double now = GetNowSeconds();
    auto it = s_firstSeenTime.find(animGraphHolder);
    if (it == s_firstSeenTime.end())
    {
        s_firstSeenTime.emplace(animGraphHolder, now);
        return false;
    }
    return (now - it->second) >= kNpcAnimSyncSettleDelay;
}

__declspec(noinline) static void SafeCallNpcAnimSyncOriginal(void* a1, void* a2, void* a3, void* a4)
{
    LOG_REQUIREMENT("SafeCallNpcAnimSyncOriginal", "Needs the actor's animation graph to actually be ready before calling into the game's own sync function -- calling it too early is a guaranteed, unrecoverable crash, so we check first instead of relying on a crash guard.");
    g_lastTickStep = "TickHook:SafeCallNpcAnimSyncOriginal:entry";

    if (!a1)
        return;

    // a1 is documented above as actor+0x48 (IAnimationGraphManagerHolder) --
    // derive the real actor pointer back out so the captured formID is
    // actually identifiable in a crash log.
    void* derivedActor = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(a1) - 0x48);
    g_npcAnimSync_lastA1 = a1;
    g_npcAnimSync_lastActorFormID = SafeReadActorFormID(derivedActor);
    ++g_npcAnimSync_callIndex;

    // Round 119: round 117/118's IsGraphReady() gate is a structural check
    // only (non-null manager/graph/variable-set of sane size) -- it says
    // nothing about how long ago this AnimGraph was constructed. It passed,
    // SafeCall completed, and this exact crash (same actor formID, same
    // callIndex=1, same 7-frame signature) still happened minutes later:
    // the real engine NpcAnimSync call dispatches further async work on the
    // same AnimGraph (a job-system thread, no DoxCoSync frames in that
    // crash's call stack) which touches a different, still-unpopulated
    // sub-structure (a 32-slot ref-counted pointer table) and faults there.
    // Same root class CoSyncPlayer::TickAnimVars already solved for the
    // player with a flat post-spawn settle delay on top of (not instead of)
    // its own IsGraphReady() check -- this hook just never got that half of
    // the fix, and it fires for every actor, not only spawned proxies. Track
    // first-seen time per AnimGraph holder pointer and require the same
    // settle delay to elapse before ever letting the real engine call
    // through, checked before IsGraphReady() so a too-fresh actor's graph
    // isn't touched at all during the unsafe window.
    if (!NpcAnimSync_SettleDelayElapsed(a1))
    {
        g_lastTickStep = "TickHook:SafeCallNpcAnimSyncOriginal:settle-delay-skip";
        return;
    }

    // Round 118: three independent test runs all crashed on the literal
    // FIRST non-blocked call this hook ever makes (callIndex=1), on the same
    // actor formID, ~1.6s after "Local entity bound" every time -- a fully
    // reproducible bug, not a timing fluke. Live-disassembling the actual
    // installed Fallout4.exe (not the stale IDA database -- see round 117)
    // showed the crash is a weak-handle-table lookup inside the engine's own
    // NpcAnimSync: it walks a hash table off this actor's AnimGraph holder
    // looking for a cached entry, "finds" one, and unconditionally does
    // `lock xadd [rcx+8], eax` (an AddRef) on whatever it got back -- no null
    // or sentinel check. The value it got back was the literal sentinel
    // 0xFFFF0002 every single time, consistent with a not-yet-fully-
    // registered/constructed AnimGraph weak handle (the same "actor's graph
    // isn't warmed up yet" bug class this campaign has already root-caused
    // more than once -- e.g. the kAnimGraphSettleDelay grace period in
    // CoSyncPlayer::TickAnimVars). CoSyncAnimVarSystem::IsGraphReady() is the
    // already-established, already-trusted check for exactly this
    // precondition elsewhere in this codebase; gating this call on it here
    // costs one cheap SEH-guarded read and avoids ever reaching the engine's
    // own unchecked AddRef while the actor's graph isn't ready, instead of
    // gambling on a recovery patch after the fact (already tried twice in
    // rounds 113/114, both made things worse).
    if (!CoSyncAnimVarSystem::IsGraphReady(reinterpret_cast<Actor*>(derivedActor)))
    {
        g_lastTickStep = "TickHook:SafeCallNpcAnimSyncOriginal:graph-not-ready-skip";
        return;
    }

    // Round 117: this exact call boundary is the prime suspect -- mark it
    // distinctly from the generic "entry" step above so a crash log can
    // distinguish "never reached the engine call" from "died inside it".
    g_lastTickStep = "TickHook:SafeCallNpcAnimSyncOriginal:pre-engine-call";
    __try
    {
        if (g_NpcAnimSync_Original)
            g_NpcAnimSync_Original(a1, a2, a3, a4);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[TickHook] NpcAnimSync_Hook: g_NpcAnimSync_Original faulted, "
            "recovered (a1=%p a2=%p a3=%p a4=%p)", a1, a2, a3, a4);
    }
    g_lastTickStep = "TickHook:SafeCallNpcAnimSyncOriginal:post-engine-call";
}

// Round 112: removed this function's call-counted diagnostic logging block
// entirely. PDB-resolved symbol lookup on the actual crash stack (not
// caller-chain guesswork) proved the long-standing, previously "contained"
// CRT vsnprintf crash (this file's 2026-06-17 "crash #6/#7" comments,
// SafeTimestampFormat/SafeVsnprintf/SafeTimestampWrap) is still being
// triggered from exactly here: CoSyncUpdatedLog_Printf, called from this
// diagnostic block, called from NpcAnimSync_Hook -- a MinHook-spliced
// detour on a hot, every-actor-every-frame engine callback. Despite every
// formatting call already being noinline+SEH-guarded, the fault still
// escapes from this specific calling context (consistent with the
// 2026-06-17 notes describing the corruption as durable/un-catchable once
// triggered, not a one-off). This block was pure debug instrumentation --
// it never affected `blocked` below, which takes its own independent
// snapshot -- so the fix is to stop logging from inside this trampoline
// at all, rather than continue chasing the underlying CRT-state bug here.
static void __fastcall NpcAnimSync_Hook(void* a1, void* a2, void* a3, void* a4)
{
    LOG_REQUIREMENT("NpcAnimSync_Hook", "Runs for whichever actor the game is currently updating. Checks whether that actor is one of our remote-player stand-ins first, so we never drive its animation twice.");
    g_lastTickStep = "TickHook:NpcAnimSync_Hook:entry";

    bool blocked = false;
    {
        std::vector<std::pair<Actor*, CoSyncPlayer*>> actorsSnapshot;
        {
            std::lock_guard<std::mutex> lk(s_remoteActorMutex);
            actorsSnapshot.reserve(s_remoteActors.size());
            for (auto& kv : s_remoteActors)
                actorsSnapshot.emplace_back(kv.first, kv.second);
        }

        for (auto& kv : actorsSnapshot)
        {
            Actor* candidateActor = kv.first;

            bool matchActor = (a1 == (void*)candidateActor);

            void* mp = SafeReadVoidPtr(reinterpret_cast<uintptr_t>(candidateActor) + 0x300);
            bool matchMP = (mp != nullptr && a1 == mp);

            bool matchAnimHolder = (a1 == reinterpret_cast<void*>(
                reinterpret_cast<uintptr_t>(candidateActor) + 0x48));

            if (matchActor || matchMP || matchAnimHolder)
            {
                static bool s_diagDone = false;
                if (!s_diagDone)
                {
                    s_diagDone = true;
                    LOG_INFO("[TickHook] NpcAnimSync_Hook: BLOCKED remote proxy "
                        "a1=%p matchActor=%d matchMP=%d matchAnimHolder=%d entity=%u",
                        a1, matchActor ? 1 : 0, matchMP ? 1 : 0, matchAnimHolder ? 1 : 0,
                        kv.second ? kv.second->entityID : 0u);
                }
                blocked = true;
                break;
            }
        }
    }
    if (blocked)
    {
        g_lastTickStep = "TickHook:NpcAnimSync_Hook:remote-proxy-skip";
        return;
    }

    g_lastTickStep = "TickHook:NpcAnimSync_Hook:pre-SafeCall";
    SafeCallNpcAnimSyncOriginal(a1, a2, a3, a4);
    g_lastTickStep = "TickHook:NpcAnimSync_Hook:post-SafeCall";
}

void InstallNpcAnimSyncHook()
{
    LOG_REQUIREMENT("InstallNpcAnimSyncHook", "Only meant to be called once. Needs the game's .exe to be loaded and this game version's animation-sync function to be at the address this build expects.");
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target   = reinterpret_cast<void*>(base + kNpcAnimSync_RVA);


    const uint8_t* b = reinterpret_cast<const uint8_t*>(target);
    LOG_INFO("[TickHook] NpcAnimSync target %p bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
        target, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[TickHook] MH_Initialize failed: %d", (int)st);
        return;
    }

    st = MH_CreateHook(target,
        reinterpret_cast<void*>(&NpcAnimSync_Hook),
        reinterpret_cast<void**>(&g_NpcAnimSync_Original));
    if (st != MH_OK)
    {
        LOG_ERROR("[TickHook] MH_CreateHook(NpcAnimSync) failed: %d (target=%p)", (int)st, target);
        return;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK)
    {
        LOG_ERROR("[TickHook] MH_EnableHook(NpcAnimSync) failed: %d", (int)st);
        return;
    }

    LOG_INFO("[TickHook] NpcAnimSync hook installed at base+0x%zX (%p)", kNpcAnimSync_RVA, target);
}
