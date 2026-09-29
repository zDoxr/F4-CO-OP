#include "CoSyncLocalPlayer.h"

#include "../Animations/CoSyncPlayerAnimation.h"
#include "CoSyncFirstPersonGraph.h"
#include "CoSyncRequirementsLog.h"
#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncGameAPI.h"
#include "GameReferences.h"
#include "GameObjects.h"
#include "TickHook.h"
#include "CoSyncSpecialSync.h"
#include "CoSyncVatsSync.h"
#include "CoSyncOutfitSync.h"
#include "CoSyncTransport.h"
#include "CoSyncAnimationUtils.h"
#include "CoSyncHealthSync.h"
#include "CoSyncReviveSync.h"
#include "CoSyncActorValueSafety.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncOverlay.h"
#include "CoSyncPapyrusHelper.h"

#include "GameForms.h"
#include "GameFormComponents.h"
#include "PapyrusVM.h"
#include "PapyrusEvents.h"

#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cmath>


extern RelocPtr<PlayerCharacter*> g_player;
extern PlayerCharacter* CoSyncGetPlayer();
extern CoSyncPlayerManager g_CoSyncPlayerManager;

namespace
{

    bool s_initialized = false;


    NiPoint3 s_lastSentPos{ 0.f, 0.f, 0.f };
    NiPoint3 s_lastSentRot{ 0.f, 0.f, 0.f };
    NiPoint3 s_lastSentVel{ 0.f, 0.f, 0.f };
    uint32_t s_lastSentWeapFormID = 0;
    double   s_lastSendTime = 0.0;

    bool s_prevAnimDriven = false;

    bool s_prevSwimming = false;
    bool s_prevFiring   = false;

    // One-shot flag: send TX EQUIP for our current weapon the first time we
    // detect one while connected. This ensures the remote machine gets an
    // explicit ordered equip packet that arrives AFTER our CREATE, rather than
    // relying solely on the UPDATE path whose first packet can be dropped if it
    // races the CoSyncPlayer entry creation on the remote side.
    bool s_initialWeaponBroadcastSent = false;

    static constexpr uint32_t kHealthAVI_FormID = 0x0002D4u;
    static constexpr uint32_t kAPAVI_FormID     = 0x0002D5u;   // ActionPoints
    float  s_lastSentHP     = -1.f;
    float  s_lastSentRads   = -1.f;
    float  s_lastSentAP     = -1.f;
    float  s_lastSentMaxHP  = -1.f;
    float  s_lastSentMaxAP  = -1.f;
    double s_lastHPSendTime = 0.0;
    double s_lastHPSampleTime = 0.0;

    // Death / revive state
    bool s_deathPacketSent = false;
    int  s_consecutiveAliveTicks = 0;

    // Networked revive (CoSyncReviveSync). s_localDownedSince latches the
    // moment we entered the dead-gate below, for the give-up timer.
    // s_pendingLocalHealPercent/s_localReviverEID are set from INSIDE the
    // gate (revive-consume or give-up) but only ever acted on once the
    // respawn debounce below has already independently proven actorValueOwner
    // is safe to touch again -- this file never writes Health AV during the
    // down window itself, only at that one proven-safe point.
    double s_localDownedSince = 0.0;
    float  s_pendingLocalHealPercent = -1.f;   // -1 = nothing pending
    uint32_t s_localReviverEID = 0;

    static ActorValueInfo* GetHealthAVICached()
    {
        LOG_REQUIREMENT("CoSyncLocalPlayer::GetHealthAVICached", "Needs the game's data to be loaded so it can look up the Health stat definition.");
        static ActorValueInfo* s_avi = nullptr;
        if (!s_avi)
        {
            TESForm* form = LookupFormByID(kHealthAVI_FormID);
            if (form)
                s_avi = static_cast<ActorValueInfo*>(form);
        }
        return s_avi;
    }

    static ActorValueInfo* GetAPAVICached()
    {
        LOG_REQUIREMENT("CoSyncLocalPlayer::GetAPAVICached", "Needs the game's data to be loaded so it can look up the Action Points stat definition. Safe to cache even if the lookup fails.");
        static ActorValueInfo* s_avi = nullptr;
        if (!s_avi)
        {
            TESForm* form = LookupFormByID(kAPAVI_FormID);
            if (form && form->formType == kFormType_AVIF)
                s_avi = static_cast<ActorValueInfo*>(form);
        }
        return s_avi;
    }

    // Player level for the HP packet / lobby bar. Reads the player's TESNPC
    // base actorData via the F4SE GetLevel member fn (handles PCLevelMult).
    // SEH-wrapped like every other raw engine read in this codebase; a fault
    // returns 0 which the UI renders as "Lv --".
    __declspec(noinline) uint32_t GetLocalPlayerLevel(Actor* player)
    {
        __try
        {
            if (!player || !player->baseForm)
                return 0;
            if (player->baseForm->formType != kFormType_NPC_)
                return 0;
            TESNPC* npc = static_cast<TESNPC*>(player->baseForm);
            return CALL_MEMBER_FN(&npc->actorData, GetLevel)();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // Returns HP via SEH-guarded read; -1.f on fault so the caller keeps the
    // "treat as alive" default. Isolated in a noinline function because C2712
    // forbids __try in the same scope as C++ destructible objects (like Tick).
    __declspec(noinline) static float SafeReadPlayerHP(Actor* player, ActorValueInfo* avi)
    {
        __try { return player->actorValueOwner.GetValue(avi); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1.f; }
    }

    NiPoint3 s_prevPos{ 0.f, 0.f, 0.f };
    double   s_prevTime = 0.0;
    bool     s_hasPrevious = false;


    float s_updateRate = 20.0f;
    float s_movementThreshold = 0.01f;
    float s_rotationThreshold = 0.01f;

    // Tether — keep P2 within the loaded exterior cell grid (~2 cells = 8192 units).
    // Beyond this the other player's NPCs and terrain stop loading on the far end.
    static constexpr float  kTetherMaxDist       = 8192.0f;
    static constexpr float  kTetherMaxDistSq     = kTetherMaxDist * kTetherMaxDist;
    // If raw distance exceeds this, assume different worldspace (interior vs exterior) and skip.
    static constexpr float  kTetherWorldspaceSkipSq = 500000.0f * 500000.0f;
    static constexpr double kTetherCheckInterval = 1.0;   // check every 1 s
    static constexpr double kTetherCooldown      = 5.0;   // 5 s pause after each teleport

    static double s_lastTetherCheck   = 0.0;
    static double s_tetherCooldownEnd = 0.0;

    void TickTether(double now, Actor* player, const NiPoint3& myPos)
    {
        LOG_REQUIREMENT("CoSyncLocalPlayer::TickTether", "Needs a valid player actor, an active client connection (not host), and a peer with a known position.");
        if (now < s_tetherCooldownEnd)               return;
        if (now - s_lastTetherCheck < kTetherCheckInterval) return;
        s_lastTetherCheck = now;

        if (!CoSyncNet::IsConnected() || CoSyncNet::IsHost()) return;

        auto peers = CoSyncNet::GetConnectedPeerEntityIDs();
        if (peers.empty()) return;

        CoSyncPlayer* hostProxy = g_CoSyncPlayerManager.GetPlayerByEntityID(peers[0]);
        if (!hostProxy) return;

        const NiPoint3& hostPos = hostProxy->authoritativePos;
        // authoritativePos starts at (0,0,0) before the first packet — skip until we have real data
        if (fabsf(hostPos.x) < 1.0f && fabsf(hostPos.y) < 1.0f) return;

        const float dx = myPos.x - hostPos.x;
        const float dy = myPos.y - hostPos.y;
        const float dz = myPos.z - hostPos.z;
        const float distSq = dx*dx + dy*dy + dz*dz;

        // Skip when clearly in a different worldspace (different interior/exterior)
        if (distSq > kTetherWorldspaceSkipSq) return;
        if (distSq <= kTetherMaxDistSq)        return;

        // Place P2 at host position, 50 units above to clear terrain and hitbox overlap
        NiPoint3 target(hostPos.x, hostPos.y, hostPos.z + 50.0f);
        NiPoint3 rot(player->rot.x, player->rot.y, player->rot.z);

        CoSyncGameAPI::PositionRemoteActor(player, target, rot);

        s_tetherCooldownEnd = now + kTetherCooldown;
        LOG_INFO("[Tether] P2 exceeded range (%.0f units) — pulled to host (%.1f,%.1f,%.1f)",
            sqrtf(distSq), target.x, target.y, target.z);
    }


    double NowSeconds()
    {
        LOG_REQUIREMENT("CoSyncLocalPlayer::NowSeconds", "Just reads the system clock. Needs Windows to report a working timer, which it always does.");
        static double s_freq = [] {
            LOG_REQUIREMENT("CoSyncLocalPlayer::NowSeconds::lambda", "Just reads the system clock. Needs Windows to report a working timer, which it always does.");
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return double(f.QuadPart);
            }();

        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        return double(c.QuadPart) / s_freq;
    }


    float VectorLength(const NiPoint3& v)
    {
        LOG_REQUIREMENT("CoSyncLocalPlayer::VectorLength", "Just needs a valid 3D point to measure.");
        return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    }

    NiPoint3 VectorSubtract(const NiPoint3& a, const NiPoint3& b)
    {
        LOG_REQUIREMENT("CoSyncLocalPlayer::VectorSubtract", "Just needs two valid 3D points.");
        return NiPoint3{ a.x - b.x, a.y - b.y, a.z - b.z };
    }

    NiPoint3 VectorScale(const NiPoint3& v, float scale)
    {
        LOG_REQUIREMENT("CoSyncLocalPlayer::VectorScale", "Just needs a valid 3D point and a number to scale it by.");
        return NiPoint3{ v.x * scale, v.y * scale, v.z * scale };
    }


    NiPoint3 CalculateVelocity(const NiPoint3& currentPos, const NiPoint3& prevPos, double deltaTime)
    {
        LOG_REQUIREMENT("CoSyncLocalPlayer::CalculateVelocity", "Guards against a near-zero time step itself, so it won't divide by almost-zero.");
        if (deltaTime < 0.0001)
            return NiPoint3{ 0.f, 0.f, 0.f };

        NiPoint3 delta = VectorSubtract(currentPos, prevPos);
        float scale = static_cast<float>(1.0 / deltaTime);

        return VectorScale(delta, scale);
    }


    bool HasMovedSignificantly(const NiPoint3& current, const NiPoint3& previous)
    {
        LOG_REQUIREMENT("CoSyncLocalPlayer::HasMovedSignificantly", "Just needs two valid points and the configured movement threshold.");
        NiPoint3 delta = VectorSubtract(current, previous);
        float distance = VectorLength(delta);
        return distance > s_movementThreshold;
    }


    bool HasRotatedSignificantly(const NiPoint3& current, const NiPoint3& previous)
    {
        LOG_REQUIREMENT("CoSyncLocalPlayer::HasRotatedSignificantly", "Just needs two valid rotations and the configured rotation threshold.");

        float deltaX = fabsf(current.x - previous.x);
        float deltaY = fabsf(current.y - previous.y);
        float deltaZ = fabsf(current.z - previous.z);

        return (deltaX > s_rotationThreshold) ||
            (deltaY > s_rotationThreshold) ||
            (deltaZ > s_rotationThreshold);
    }

    // UPDATE 2026-06-18 (round 23 audit, exhaustive pass): split into a noinline
    // helper wrapped in SEH -- raw walk of player->middleProcess->unk08->equipData,
    // an engine-owned array, with zero protection before this fix. Identical
    // unguarded pattern already fixed in CoSyncPlayerManager.cpp::GetEquippedWeaponFormID.
    __declspec(noinline) static uint32_t GetLocalEquippedWeaponFormID_Impl(Actor* player)
    {
        LOG_REQUIREMENT("CoSyncLocalPlayer::GetLocalEquippedWeaponFormID_Impl", "Needs a valid player. Walks raw engine data that's only checked for non-null, not fully validated -- the caller wraps this in a safety net.");
        uint32_t equippedWeapFormID = 0;
        if (player->middleProcess && player->middleProcess->unk08)
        {
            Actor::AIProcess::Data08* data = player->middleProcess->unk08;
            for (UInt32 wi = 0; wi < data->equipData.count; ++wi)
            {
                const Actor::AIProcess::Data08::EquipData& ed = data->equipData[wi];
                if (ed.item && ed.item->formType == kFormType_WEAP)
                {
                    uint32_t fid = ed.item->formID;

                    if ((fid & 0xFF000000u) != 0xFF000000u)
                    {
                        equippedWeapFormID = fid;
                        break;
                    }
                }
            }
        }
        return equippedWeapFormID;
    }

    // Round 138 fallback: walk the inventory for an equipped WEAP. The
    // middleProcess equipData walk above returned 0 for the ENTIRE session on
    // P2's machine (host log: every FULL SNAP for P2 showed weap=0x00000000
    // while P2 was armed) — this is the same BGSInventoryList mechanism
    // OutfitSync uses successfully on both machines.
    __declspec(noinline) static uint32_t GetEquippedWeaponFromInventory_Impl(Actor* player)
    {
        LOG_REQUIREMENT("CoSyncLocalPlayer::GetEquippedWeaponFromInventory_Impl", "Needs a valid player with an inventory list to scan. The caller wraps this in a safety net.");
        BGSInventoryList* invList = player->inventoryList;
        if (!invList)
            return 0;

        for (UInt32 i = 0; i < invList->items.count; ++i)
        {
            const BGSInventoryItem& item = invList->items[i];
            if (!item.form || item.form->formType != kFormType_WEAP)
                continue;
            if ((item.form->formID & 0xFF000000u) == 0xFF000000u)
                continue;
            if (!item.stack)
                continue;

            bool equipped = false;
            item.stack->Visit([&equipped](BGSInventoryItem::Stack* s) {
                if (s->flags & BGSInventoryItem::Stack::kFlagEquipped)
                {
                    equipped = true;
                    return false;
                }
                return true;
            });

            if (equipped)
                return item.form->formID;
        }
        return 0;
    }

    static uint32_t GetEquippedWeaponFromInventory(Actor* player)
    {
        LOG_REQUIREMENT("CoSyncLocalPlayer::GetEquippedWeaponFromInventory", "Needs a valid player. Wraps the inventory scan in a safety net.");
        uint32_t result = 0;
        __try
        {
            result = GetEquippedWeaponFromInventory_Impl(player);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[LocalPlayer] GetEquippedWeaponFromInventory faulted -- caught");
        }
        return result;
    }

    uint32_t GetLocalEquippedWeaponFormID(Actor* player)
    {
        LOG_REQUIREMENT("CoSyncLocalPlayer::GetLocalEquippedWeaponFormID", "Needs a valid player. Wraps the raw engine data walk in a safety net so a fault is caught instead of crashing the game.");
        uint32_t middleProcessResult = 0;
        __try
        {
            middleProcessResult = GetLocalEquippedWeaponFormID_Impl(player);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[LocalPlayer] GetLocalEquippedWeaponFormID faulted -- caught");
        }

        uint32_t result = middleProcessResult;
        uint32_t inventoryResult = 0;
        if (result == 0)
        {
            inventoryResult = GetEquippedWeaponFromInventory(player);
            result = inventoryResult;
        }

        // Round 142: transition-only diagnostic (not throttled/sampled) so a
        // real equip/holster is never missed in the log, and so a future
        // "weapon shows 0x0" report can be traced to WHICH stage failed:
        // middleProcess path (engine equip-slot walk) vs inventory fallback
        // (BGSInventoryList walk, same mechanism OutfitSync uses) vs neither
        // (player genuinely unarmed / fists).
        static uint32_t s_lastLoggedResult = 0xFFFFFFFFu; // sentinel: never logged yet
        if (result != s_lastLoggedResult)
        {
            LOG_UPDATED("[LocalPlayer] equipped weapon -> 0x%08X (middleProcess=0x%08X inventoryFallback=0x%08X)",
                result, middleProcessResult, inventoryResult);
            s_lastLoggedResult = result;
        }

        return result;
    }
}

void CoSyncLocalPlayer::Init()
{
    LOG_REQUIREMENT("CoSyncLocalPlayer::Init", "Just needs this module to not already be started.");
    if (s_initialized)
        return;

    s_initialized = true;


    s_lastSendTime = 0.0;
    s_hasPrevious = false;
    s_initialWeaponBroadcastSent = false;

    LOG_INFO("[LocalPlayer] Initialized (updateRate=%.1f Hz)", s_updateRate);
}

void CoSyncLocalPlayer::Shutdown()
{
    LOG_REQUIREMENT("CoSyncLocalPlayer::Shutdown", "Just needs this module to already be started.");
    if (!s_initialized)
        return;

    s_initialized = false;
    s_hasPrevious = false;

    LOG_INFO("[LocalPlayer] Shutdown");
}

Actor* CoSyncLocalPlayer::GetLocalPlayerActor()
{
    LOG_REQUIREMENT("CoSyncLocalPlayer::GetLocalPlayerActor", "Needs a save to be loaded so the player exists. Callers must still check the result isn't null.");
    return CoSyncGetPlayer();
}

// 2026-07-26: single source of truth for "is the local player currently in
// the declared-dead, debounced-respawn-pending state" -- see Tick()'s dead-
// player gate. Added so TickHook.cpp's ActorUpdate_Hook (a separate,
// engine-driven call cadence from this file's own AddTaskPermanent tick) can
// check the SAME state instead of independently polling HP itself. Two
// independent polls at different cadences is exactly what let a one-frame
// HP flicker from the engine's own reload/respawn processing slip past one
// poll while the other never saw it (confirmed via log correlation).
bool CoSyncLocalPlayer::IsLocallyConfirmedDead()
{
    LOG_REQUIREMENT("CoSyncLocalPlayer::IsLocallyConfirmedDead", "Just reads this file's own death-tracking flag, safe to call from anywhere.");
    return s_deathPacketSent;
}

bool CoSyncLocalPlayer::GetLocalTransform(NiPoint3& outPos, NiPoint3& outRot)
{
    LOG_REQUIREMENT("CoSyncLocalPlayer::GetLocalTransform", "Needs the local player to exist before reading its position and rotation.");
    Actor* player = GetLocalPlayerActor();
    if (!player)
        return false;

    return CoSyncGameAPI::GetActorWorldTransform(player, outPos, outRot);
}

void CoSyncLocalPlayer::ForceUpdate()
{
    LOG_REQUIREMENT("CoSyncLocalPlayer::ForceUpdate", "Needs this module to already be started for the forced resend to happen.");
    if (!s_initialized)
        return;


    s_lastSendTime = 0.0;

    LOG_INFO("[LocalPlayer] Force update requested");
}

void CoSyncLocalPlayer::SetUpdateRate(float updatesPerSecond)
{
    LOG_REQUIREMENT("CoSyncLocalPlayer::SetUpdateRate", "Clamps the update rate to a sane 1-60 Hz range itself, no other requirement.");
    if (updatesPerSecond < 1.0f)
        updatesPerSecond = 1.0f;
    if (updatesPerSecond > 60.0f)
        updatesPerSecond = 60.0f;

    s_updateRate = updatesPerSecond;

    LOG_INFO("[LocalPlayer] Update rate set to %.1f Hz", s_updateRate);
}

void CoSyncLocalPlayer::SetMovementThreshold(float distance)
{
    LOG_REQUIREMENT("CoSyncLocalPlayer::SetMovementThreshold", "Clamps the distance to a non-negative value itself, no other requirement.");
    if (distance < 0.0f)
        distance = 0.0f;

    s_movementThreshold = distance;

    LOG_INFO("[LocalPlayer] Movement threshold set to %.3f", distance);
}

void CoSyncLocalPlayer::SetRotationThreshold(float radians)
{
    LOG_REQUIREMENT("CoSyncLocalPlayer::SetRotationThreshold", "Clamps the angle to a non-negative value itself, no other requirement.");
    if (radians < 0.0f)
        radians = 0.0f;

    s_rotationThreshold = radians;

    LOG_INFO("[LocalPlayer] Rotation threshold set to %.3f rad", radians);
}

void CoSyncLocalPlayer::Tick(double now)
{
    LOG_REQUIREMENT("CoSyncLocalPlayer::Tick", "Needs this module started, the network connected, and the local player to exist -- all checked before touching the actor.");

    if (!s_initialized)
        return;

    if (!CoSyncNet::IsInitialized() || !CoSyncNet::IsConnected())
        return;


    Actor* player = GetLocalPlayerActor();
    if (!player)
        return;

    // Dead-player gate: actorValueOwner sub-objects are torn down on death
    // and cause a crash at the HP-sync read block below. Check here with SEH
    // before any other engine access; gate the entire tick while dead.
    {
        ActorValueInfo* healthAVI = GetHealthAVICached();
        const float curHP = healthAVI ? SafeReadPlayerHP(player, healthAVI) : 1.f;
        // 2026-07-26: this was `curHP >= 0.f && curHP <= 0.f`, which only
        // matches curHP==0.0 exactly. Real overkill damage overshoots below
        // zero almost every time (confirmed in a crash-session log: HP sent
        // as -0.70031 right before this client crashed) -- so the gate this
        // comment describes ("actorValueOwner sub-objects are torn down on
        // death") was silently never engaging on a real death, letting the
        // full tick body (raw actorValueOwner reads/writes below) run against
        // an actor whose value-owner was already being torn down. That's the
        // likely cause of a fatal engine crash (ExtraAliasInstanceArray built
        // from a corrupted pointer, Fallout4.exe+0x2AA94C) seen on both host
        // and client during death/respawn testing. SafeReadPlayerHP's -1.f
        // fault sentinel is excluded explicitly so a transient read fault
        // still falls through to "treat as alive" as originally intended,
        // instead of now also matching curHP<=0.f.
        const bool readFaulted = (curHP == -1.f);
        if (!readFaulted && curHP <= 0.f)
        {
            // Dead-player gate (crash safety only): the actorValueOwner sub-
            // objects are torn down on death, so the HP-sync read block below
            // would fault on them -- skip the rest of the tick while dead.
            //
            // Networked revive lives entirely inside this branch, and never
            // writes an actor value here -- only VM calls (CallFunctionNoWait,
            // same subsystem InitNpcProxy already uses safely on proxies),
            // which don't touch the actorValueOwner sub-object this gate
            // exists to protect. The actual health write happens only once,
            // below, at the respawn-debounce point that already proves AV
            // access is safe again.
            if (!s_deathPacketSent)
            {
                // Edge: just entered bleedout. Block vanilla's own auto-
                // recovery timer so the player stays down until a real revive
                // (or the give-up timeout below) -- the native counterpart to
                // Follower Revive System's SetNoBleedoutRecovery(true).
                static const BSFixedString kSetNoBleedoutRecovery("SetNoBleedoutRecovery");
                VMArray<VMVariable> args;
                bool v = true;
                VMVariable vv; vv.Set(&v); args.Push(&vv);
                CallFunctionNoWait(player, kSetNoBleedoutRecovery, args);

                s_localDownedSince = now;
                CoSyncTransport::Send(CoSyncReviveSync::SerializeDown(CoSyncNet::GetMyEntityID()));
                LOG_UPDATED("[LocalPlayer] Down -- blocked vanilla auto-recovery, broadcast DOWN");
            }

            uint32_t reviverEID = 0;
            const bool revived = CoSyncReviveSync::ConsumePendingLocalRevive(reviverEID);
            const bool gaveUp  = !revived && s_deathPacketSent &&
                (now - s_localDownedSince) >= CoSyncReviveSync::SettingGiveUpSeconds();
            if (revived || gaveUp)
            {
                // Release vanilla's own recovery -- the engine stands the
                // player back up and restores a baseline HP on its own, the
                // exact transition the respawn-debounce below already handles
                // safely. A real revive additionally queues our own
                // percentage heal to apply once that debounce confirms it.
                static const BSFixedString kSetNoBleedoutRecovery("SetNoBleedoutRecovery");
                VMArray<VMVariable> args;
                bool v = false;
                VMVariable vv; vv.Set(&v); args.Push(&vv);
                CallFunctionNoWait(player, kSetNoBleedoutRecovery, args);

                s_pendingLocalHealPercent = revived ? CoSyncReviveSync::SettingHealthPercent() : -1.f;
                s_localReviverEID = revived ? reviverEID : 0;
                LOG_UPDATED("[LocalPlayer] %s -- released vanilla auto-recovery",
                    revived ? "Revived" : "Gave up (revive timeout)");
            }

            s_consecutiveAliveTicks = 0;
            s_deathPacketSent = true;   // latch so the respawn-grace reset below fires once
            return;
        }
        if (s_deathPacketSent)
        {
            // 2026-07-26: was a single-sample check (`if (curHP > 0.f) ...
            // s_deathPacketSent = false`). Log correlation across a crash
            // session proved that's not safe: TickHook.cpp's ActorUpdate_Hook
            // runs its own independent HP poll (different call cadence --
            // engine-driven vtable dispatch vs this file's AddTaskPermanent
            // tick), and it caught a single-tick positive HP reading 133ms
            // before a crash, during what this file's own poll still saw as
            // a fully dead, non-respawned window (this function never logged
            // RESPAWN detected that session). That proves the engine's own
            // reload/respawn processing can flicker HP positive for one
            // frame without the transition actually completing, and trusting
            // any single positive sample was enough to let a fragile-actor
            // window through. Require several consecutive positive readings
            // before believing a real respawn happened.
            constexpr int kRespawnDebounceTicks = 5;
            if (curHP > 0.f && !readFaulted)
            {
                if (++s_consecutiveAliveTicks < kRespawnDebounceTicks)
                    return;

                const uint32_t eid = CoSyncNet::GetMyEntityID();
                LOG_UPDATED("[LocalPlayer][DeathDiag] RESPAWN detected entity=%u actor=%p formID=0x%08X parentCell=%p 3dLoaded=%d hp=%.1f -- resuming full tick this frame",
                    eid, static_cast<void*>(player), player->formID,
                    static_cast<void*>(player->parentCell),
                    (int)CoSyncAnimUtils::Is3DLoaded(player), curHP);
                // 2026-07-27: a death->"Continue" reload rebuilds the local
                // player's behavior graph the same way a fresh actor spawn
                // does, but IsGraphReady used to exempt the local player from
                // its grace-period gate entirely -- letting anim capture read
                // a stale/garbage graph holder right after this exact
                // transition (confirmed fatal crash, READ AV in GetBool
                // bIsUnderwater ~0.4-1s after this log line). Reset here so
                // the local player gets the same fresh 3s grace window a
                // newly-spawned actor gets, instead of being permanently
                // exempt.
                CoSyncAnimUtils::ResetGraphGrace(player);
                s_deathPacketSent = false;
                s_consecutiveAliveTicks = 0;

                // Networked revive heal: this is the one point in the down/
                // respawn cycle the debounce above has already proven safe
                // for actorValueOwner access (5 consecutive positive reads),
                // so it's the only place this file writes Health AV for a
                // revive. No-op on a plain give-up (s_pendingLocalHealPercent
                // stays -1 in that case -- vanilla's own baseline HP is all
                // the player gets back, same as before this feature existed).
                if (s_pendingLocalHealPercent >= 0.f && healthAVI)
                {
                    float maxHP = 0.f;
                    if (CoSyncAVSafety::SafeGetMaximum(player, healthAVI, maxHP) && maxHP > 0.f)
                    {
                        const float target = maxHP * (s_pendingLocalHealPercent / 100.0f);
                        CoSyncAVSafety::SafeSetBase(player, healthAVI, target);
                    }

                    CoSyncPlayer* reviverProxy = s_localReviverEID
                        ? g_CoSyncPlayerManager.GetPlayerByEntityID(s_localReviverEID) : nullptr;
                    char note[160];
                    std::snprintf(note, sizeof(note), "You were revived by %s!",
                        (reviverProxy && !reviverProxy->username.empty())
                            ? reviverProxy->username.c_str() : "a teammate");
                    CoSyncGameAPI::QueueNotification(note);

                    LOG_UPDATED("[LocalPlayer] Revive heal applied — target=%.1f%% of max", s_pendingLocalHealPercent);
                    s_pendingLocalHealPercent = -1.f;
                    s_localReviverEID = 0;
                }
            }
            else
            {
                // Still gated: either a fault, or (shouldn't happen given the
                // branch above) a non-positive read that isn't "confirmed
                // dead" either. Don't let a flicker count toward the debounce.
                s_consecutiveAliveTicks = 0;
                return;
            }
        }
    }

    const uint32_t myEntityID = CoSyncNet::GetMyEntityID();
    CoSyncSpecialSync::TickLocalSpecial(myEntityID, player);


    CoSyncVatsSync::TickLocalVats(myEntityID);


    CoSyncOutfitSync::TickLocalOutfit(myEntityID, player);


    NiPoint3 currentPos{ 0.f, 0.f, 0.f };
    NiPoint3 currentRot{ 0.f, 0.f, 0.f };

    if (!CoSyncGameAPI::GetActorWorldTransform(player, currentPos, currentRot))
        return;


    NiPoint3 velocity{ 0.f, 0.f, 0.f };
    if (s_hasPrevious && s_prevTime > 0.0)
    {
        double deltaTime = now - s_prevTime;
        velocity = CalculateVelocity(currentPos, s_prevPos, deltaTime);
    }


    s_prevPos = currentPos;
    s_prevTime = now;
    s_hasPrevious = true;

    // Vitals stream (health / rads / AP) for teammates' HUD cards. Sampled at
    // 10 Hz and sent on ANY real change (0.1 resolution), plus a 1 s resync.
    // It used to sample at 2 Hz and only resend after AP moved >= 2 / rads
    // >= 1, so regen reached teammates in visible steps and the last partial
    // step (e.g. AP 88.5 -> 90) was never sent at all -- the card then stayed
    // short of the real value. ~60-byte message, so 10 Hz is negligible.
    if (myEntityID != 0 && now - s_lastHPSampleTime >= 0.1)
    {
        s_lastHPSampleTime = now;
        ActorValueInfo* healthAVI = GetHealthAVICached();
        if (healthAVI)
        {
            float curHP = player->actorValueOwner.GetValue(healthAVI);
            float maxHP = player->actorValueOwner.GetMaximum(healthAVI);

            const uint32_t myLevel = GetLocalPlayerLevel(player);

            // Current radiation for the red health-bar chunk. Best-effort:
            // if the Rads AV can't be resolved, rads stays 0 (no red).
            float curRads = 0.f;
            if (ActorValueInfo* radsAVI = CoSyncHealthSync::GetRadsActorValue())
                curRads = player->actorValueOwner.GetValue(radsAVI);
            if (curRads < 0.f) curRads = 0.f;

            float curAP = -1.f, maxAP = 0.f;
            if (ActorValueInfo* apAVI = GetAPAVICached())
            {
                curAP = player->actorValueOwner.GetValue(apAVI);
                maxAP = player->actorValueOwner.GetMaximum(apAVI);
                if (curAP < 0.f) curAP = 0.f;
            }

            // Local player's own card in the lobby bar — refreshed every
            // 0.5s pass so damage shows up immediately regardless of the
            // network send threshold below.
            CoSyncOverlay_UpdateLobbyEntry(
                myEntityID, CoSyncNet::GetMyName(), curHP, maxHP, (int)myLevel, true, curRads);

            constexpr float kEps = 0.1f;
            const bool changed =
                s_lastSentHP < 0.f ||
                fabsf(curHP - s_lastSentHP)     >= kEps ||
                fabsf(maxHP - s_lastSentMaxHP)  >= kEps ||
                fabsf(curRads - s_lastSentRads) >= kEps ||
                (curAP >= 0.f && (fabsf(curAP - s_lastSentAP)   >= kEps ||
                                  fabsf(maxAP - s_lastSentMaxAP) >= kEps));
            const bool resync = now - s_lastHPSendTime >= 1.0;

            if (changed || resync)
            {
                CoSyncTransport::Send(CoSyncHealthSync::SerializeHP(myEntityID, curHP, maxHP, myLevel, curRads, curAP, maxAP));
                s_lastSentHP     = curHP;
                s_lastSentMaxHP  = maxHP;
                s_lastSentRads   = curRads;
                s_lastSentAP     = curAP;
                s_lastSentMaxAP  = maxAP;
                s_lastHPSendTime = now;
            }
        }
    }

    TickTether(now, player, currentPos);

    // Capture local player skeleton every frame for animation testing
    // replication. Two independent gates feed the same capture: IsEnabled()
    // is the Mod Config UI "Testing" toggle (local mirror, no network);
    // IsBoneReplicationEnabled() is bDeltaPositionLocomotion=2 in the ini
    // (the real network path below). Either one needs the capture filled.
    //
    // ShouldCaptureNow() ports FO4_Wrld's own shipped capture gate: while in
    // first person, only capture if the third-person-graph drive is
    // CONFIRMED actually running -- otherwise skip the call entirely and let
    // g_localCapture keep whatever it last held (their own account: sending
    // a not-yet-driven/broken pose is what visibly deformed a remote proxy;
    // a stale-but-valid capture just looks frozen for at most one frame,
    // since driveCount reaches 1 almost immediately after the camera switch
    // in every test this session). No-op (always true) outside first person
    // or outside bone-replication mode, so this changes nothing for any
    // other existing capture scenario.
    // Publish camera state before capturing: it rides along with the packet so
    // the receiver knows whether the lower half of this pose is real. In first
    // person the parked third-person graph does not animate the legs -- six
    // separate attempts to make it do so were measured and failed (see
    // CoSyncFirstPersonGraph.cpp) -- so the honest thing is to tell the
    // receiver rather than ship a pose whose bottom half is a bind stance.
    CoSyncPlayerAnimation::SetLocalFirstPerson(CoSyncFirstPersonGraph::IsSenderInFirstPerson());

    if ((CoSyncPlayerAnimation::IsEnabled() || CoSyncPlayerAnimation::IsBoneReplicationEnabled())
        && CoSyncFirstPersonGraph::ShouldCaptureNow())
        CoSyncPlayerAnimation::CaptureBones(player);

    // [Animations] bDeltaPositionLocomotion=2: transmit this capture so a
    // remote peer's proxy of ME can actually be driven by it -- CaptureBones
    // alone only ever fills the local g_localCapture, there's no implicit
    // network path. Rate-limited well below the 60Hz capture rate: a full
    // skeleton is ~50-70 bones x 13 floats as text, so sending every frame
    // would be several hundred KB/sec for one player alone. 15Hz is a first
    // guess pending a live bandwidth check, same as every other rate in this
    // codebase that started as a guess and got tuned from measured logs.
    if (CoSyncPlayerAnimation::IsBoneReplicationEnabled() && CoSyncNet::IsConnected())
    {
        static double s_lastBonesSend = 0.0;
        if (now - s_lastBonesSend >= (1.0 / 15.0))
        {
            s_lastBonesSend = now;
            std::string bonesMsg = CoSyncPlayerAnimation::SerializeSkeletonCapture(
                myEntityID, CoSyncPlayerAnimation::GetLocalCapture());
            // Unreliable on purpose (2026-08-21). Measured live at 1412-1418
            // bytes per frame; on the shared reliable-ordered stream a lost
            // pose frame was retransmitted AND head-of-line blocked the
            // position, health and quest traffic queued behind it. A pose is
            // the textbook case for drop-don't-retry: the next one is 67ms
            // away and supersedes it completely, and ApplySkeleton's
            // FRESH/HOLD/RELEASE tiering already handles a gap gracefully.
            if (!bonesMsg.empty())
            {
                CoSyncTransport::Send(bonesMsg, /*reliable=*/false);

                // Measurement, not decoration: the walk-depth change
                // (kMaxWalkDepth 12 -> 20) brings the finger chains into
                // range, which is expected to move the captured bone count
                // well above the 48 the previous build managed and take the
                // packet with it. Baseline for comparison: 48 bones / 1418
                // bytes / 15Hz. Logged every 5s so the new figure is one grep
                // away instead of needing another session to establish.
                static double s_lastBonesSizeLog = 0.0;
                if (now - s_lastBonesSizeLog >= 5.0)
                {
                    s_lastBonesSizeLog = now;
                    LOG_INFO("[LocalPlayer] BONES packet: %zu bytes from %u captured bones "
                             "(was 1418 bytes / 48 bones at walk depth 12)",
                             bonesMsg.size(), CoSyncPlayerAnimation::GetLocalCapture().count);
                }
            }
        }
    }

    // Animvar event detection runs every tick (no rate-limit) so single-shot fire isn't missed.
    if (g_latestLocalAnimVars.isValid && CoSyncNet::IsConnected())
    {
        // Round 138: Activate broadcast REMOVED — melee power attacks are
        // animation-driven, so this fired a furniture-style "Activate" at the
        // peer's proxy mid-swing and snapped it into an A-pose (live logs
        // 02:31:28/02:31:29 correlate exactly). Attack animations are now
        // driven by the IDA-verified attack-state bits in actorStateFlags1.
        const bool isAnimDriven = (g_latestLocalAnimVars.booleans & (1ULL << 48)) != 0;
        s_prevAnimDriven = isAnimDriven;

        bool isUnderwater = false;
        CoSyncAnimUtils::GetBool(player, "bIsUnderwater", isUnderwater);
        if (isUnderwater != s_prevSwimming)
        {
            char swBuf[64];
            std::snprintf(swBuf, sizeof(swBuf), "SW|%u|%d", myEntityID, isUnderwater ? 1 : 0);
            CoSyncTransport::Send(std::string(swBuf));
            LOG_UPDATED("[LocalPlayer] SW broadcast: bIsUnderwater=%d entity=%u",
                isUnderwater ? 1 : 0, myEntityID);
            s_prevSwimming = isUnderwater;
        }

        // isFiring (bit 2 = kisFiring) — broadcast every tick so single shots aren't missed
        const bool isFiring = (g_latestLocalAnimVars.booleans & (1ULL << 2)) != 0;
        if (isFiring && !s_prevFiring)
        {
            char fBuf[64];
            std::snprintf(fBuf, sizeof(fBuf), "AE|%u|attackStart", myEntityID);
            CoSyncTransport::Send(std::string(fBuf));
            LOG_UPDATED("[LocalPlayer] AE broadcast: attackStart entity=%u", myEntityID);
        }
        else if (!isFiring && s_prevFiring)
        {
            char fBuf[64];
            std::snprintf(fBuf, sizeof(fBuf), "AE|%u|attackStop", myEntityID);
            CoSyncTransport::Send(std::string(fBuf));
            LOG_UPDATED("[LocalPlayer] AE broadcast: attackStop entity=%u", myEntityID);
        }
        s_prevFiring = isFiring;
    }

    const double updateInterval = 1.0 / static_cast<double>(s_updateRate);
    const double timeSinceLastSend = now - s_lastSendTime;

    if (timeSinceLastSend < updateInterval)
        return;

    uint32_t equippedWeapFormID = GetLocalEquippedWeaponFormID(player);

    // One-shot initial weapon broadcast: send TX EQUIP the first time we
    // detect a weapon while connected so the peer's CoSyncPlayer entry (created
    // from our TX CREATE) gets an explicit equip packet in arrival order,
    // without racing the first UPDATE's equippedWeaponFormID field.
    if (!s_initialWeaponBroadcastSent && equippedWeapFormID != 0)
    {
        s_initialWeaponBroadcastSent = true;
        const uint32_t eid = CoSyncNet::GetMyEntityID();
        char wBuf[64];
        std::snprintf(wBuf, sizeof(wBuf), "EQUIP|%u|%u|1", eid, equippedWeapFormID);
        CoSyncTransport::Send(std::string(wBuf));
        LOG_INFO("[LocalPlayer] TX EQUIP initial: entity=%u formID=0x%08X", eid, equippedWeapFormID);
    }

    const bool weaponChanged = (equippedWeapFormID != s_lastSentWeapFormID);


    const bool hasMoved = HasMovedSignificantly(currentPos, s_lastSentPos);
    const bool hasRotated = HasRotatedSignificantly(currentRot, s_lastSentRot);

    if (!hasMoved && !hasRotated && !weaponChanged)
    {

        if (timeSinceLastSend < (updateInterval * 3.0))
            return;
    }

    const AnimVarSnapshot* pAnimVars = g_latestLocalAnimVars.isValid
        ? &g_latestLocalAnimVars
        : nullptr;

    static int s_sendDiag = 0;
    // Round 141: log EVERY jump-bit transition (bit 7 isJumping / bit 9
    // bInJumpState), not just the 1-in-60 sample — jumps are short, and the
    // earlier round's "jump not working" report needs proof of whether the
    // LOCAL capture ever sets these bits at all before blaming the network
    // or receiver side.
    static bool s_prevSendJumping = false;
    const bool nowJumping = (g_latestLocalAnimVars.booleans & ((1ULL << 7) | (1ULL << 9))) != 0;
    if (nowJumping != s_prevSendJumping)
    {
        LOG_UPDATED("[LocalPlayer] jump bits %d->%d (isJumping=%d bInJumpState=%d) vel.z=%.1f pos.z=%.1f",
            s_prevSendJumping ? 1 : 0, nowJumping ? 1 : 0,
            (g_latestLocalAnimVars.booleans & (1ULL << 7)) ? 1 : 0,
            (g_latestLocalAnimVars.booleans & (1ULL << 9)) ? 1 : 0,
            velocity.z, currentPos.z);
        s_prevSendJumping = nowJumping;
    }

    if (++s_sendDiag % 60 == 1)
        LOG_UPDATED("[LocalPlayer] Send #%d: hasAnimVars=%d speed=%.2f iState=%u weap=0x%08X bools=0x%llX vel.z=%.1f",
            s_sendDiag,
            pAnimVars ? 1 : 0,
            g_latestLocalAnimVars.floats[4],
            g_latestLocalAnimVars.integers[0],
            equippedWeapFormID,
            (unsigned long long)g_latestLocalAnimVars.booleans,
            velocity.z);

    CoSyncNet::SendMyEntityUpdate(
        myEntityID,
        currentPos,
        currentRot,
        velocity,
        now,
        pAnimVars,
        equippedWeapFormID
    );


    s_lastSentPos        = currentPos;
    s_lastSentRot        = currentRot;
    s_lastSentVel        = velocity;
    s_lastSentWeapFormID = equippedWeapFormID;
    s_lastSendTime       = now;


    if (hasMoved || hasRotated)
    {
        LOG_EVERY_MS(2000, LOG_DEBUG("[LocalPlayer] Sent UPDATE entity=%u pos=(%.2f,%.2f,%.2f) vel=(%.2f,%.2f,%.2f)",
            myEntityID,
            currentPos.x, currentPos.y, currentPos.z,
            velocity.x, velocity.y, velocity.z));
    }
}

