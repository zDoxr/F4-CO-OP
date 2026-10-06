#include "CoSyncVault111.h"
#include "CoSyncSharedIntro.h"
#include "CoSyncPlayer.h"
#include "CoSyncPuppetAuthority.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncEntityRegistry.h"
#include "ConsoleLogger.h"
#include "GameObjects.h"
#include "GameForms.h"
#include "GameReferences.h"
#include "CoSyncGameAPI.h"
#include "CoSyncPlayerMarker.h"
#include "CoSyncAnimVarSystem.h"
#include "CoSyncAnimationUtils.h"
#include "../Animations/CoSyncPlayerAnimation.h"
#include "CoSyncCreatureAnimSync.h"
#include "CoSyncPapyrusHelper.h"
#include "CoSyncProxyWeaponAuthority.h"
#include "CoSyncEquipHook.h"
#include "CoSyncThrowableSync.h"
#include "CoSyncOverlay.h"
#include "CoSyncNpcWorldSync.h"
#include "../Hooks/TickHook.h"
#include "CoSyncNet.h"
#include "PapyrusVM.h"
#include "PapyrusEvents.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

extern RelocPtr<PlayerCharacter*> g_player;

static double NowSeconds()
{
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

static constexpr double kInterpDelay = 0.12;

// How far below the world a not-yet-loaded player proxy is parked while it
// waits for its peer's first real position. Deep enough to be outside any
// gameplay proximity check, shallow enough to stay in sane float range.
static constexpr float kParkedProxyDepth = 30000.0f;

static bool PSC_SendAnimEvent(Actor* actor, const char* eventName)
{

    return CoSyncAnimUtils::SendEvent(actor, eventName);
}

static NiPoint3 Lerp(const NiPoint3& a, const NiPoint3& b, double t)
{
    return NiPoint3{
        a.x + float((b.x - a.x) * t),
        a.y + float((b.y - a.y) * t),
        a.z + float((b.z - a.z) * t)
    };
}

CoSyncPlayer::CoSyncPlayer(const std::string& name)
    : username(name)
{
}

bool CoSyncPlayer::SpawnInWorld(TESObjectREFR* anchor, TESNPC* npcBase)
{
    if (hasSpawned)
        return true;

    if (!anchor || !npcBase)
        return false;

    Actor* actor = CoSyncGameAPI::SpawnRemoteActor(
        npcBase,
        createType == CoSyncEntityType::Player);
    if (!actor)
        return false;

    actorRef = actor;
    g_CoSyncEntities.Register(entityID, actorRef);


    m_tfBuffer.clear();
    m_lastHostTime = 0.0;
    m_lastRecvLocalTime = NowSeconds();
    m_hasAny = false;

    hasSpawned = true;
    m_spawnTime = NowSeconds();

    // Show the proxy actor as a map marker on the local player's Pip-Boy map
    // so each side can always see where the other player is. PLAYER ONLY —
    // this used to fire for every spawned proxy (including NPC/companion
    // clones), so the single marker got stolen by the next NPC spawn instead
    // of tracking the remote player.
    // Marker + overlay registration is deferred to RevealOnFirstTransform ONLY
    // when this proxy actually took the spawn-disabled path (registering a
    // marker against a disabled actor parked at our feet is what crashed the
    // host inside PackHandle).
    //
    // 2026-08-04 FIX: when the peer's real position is ALREADY known at spawn
    // time, no reveal ever happens -- and the previous version registered
    // nothing at all in that case. Live proof: host log had spawnedDISABLED=0,
    // REVEALED=0, 0 markers resolved and no overlay card, while the client
    // (which did take the disabled path) had 4/4 markers and a working card.
    // The registration therefore happens below, AFTER the disable decision --
    // it must not run before m_awaitingFirstTransform has been decided.

    if (hasPendingTransform)
        ApplyPendingTransformIfAny();

    // A player proxy is created the moment the peer's HELLO arrives, but
    // CreateReferenceAtLocation can only place it at the LOCAL player (it
    // needs a loaded cell), so until that peer's first real position arrives
    // it would otherwise stand on top of us doing nothing. Hide it until we
    // actually know where it belongs.
    //
    // NPC proxies are excluded: they are created from a host CREATE that
    // already carries a real position, so there is nothing to wait for.
    // Gate on m_hasRealTransform, NOT m_hasAny: the ApplyPendingTransformIfAny
    // call just above seeds m_hasAny from this proxy's own PlaceAtMe position,
    // which would make us think we already know where the peer is.
    if (createType == CoSyncEntityType::Player && !m_hasRealTransform)
    {
        // Park it far BELOW the world as well as disabling it.
        //
        // CreateReferenceAtLocation can only place a new ref at the local
        // player, so a waiting proxy is born standing on top of us. Disabling
        // hides it, but it is still an ACTOR sitting at our exact position,
        // and several systems here pick targets by proximity -- the NPC
        // cell-scan takes the nearest matching actor, and
        // FindClosestRegisteredActor picks the closest registered one. A
        // parked proxy at the player's feet is therefore a plausible
        // contaminant for the "NPCs follow the proxy" behaviour.
        //
        // Sinking it well under the terrain costs nothing (it is disabled and
        // about to be repositioned by RevealOnFirstTransform anyway) and takes
        // it out of every proximity comparison while it waits.
        {
            NiPoint3 parked{ actorRef->pos.x, actorRef->pos.y, actorRef->pos.z - kParkedProxyDepth };
            NiPoint3 noRot{ 0.f, 0.f, 0.f };
            ApplyProxyTransform(parked, noRot);
        }

        CoSyncGameAPI::DisableWorldActor(static_cast<TESObjectREFR*>(actorRef));
        m_awaitingFirstTransform = true;
        LOG_INFO("[CoSyncPlayer] entity=%u spawned DISABLED and parked %.0f units below "
                 "— waiting for first position",
            entityID, kParkedProxyDepth);
    }

    // Now that the defer decision is made: if we are NOT waiting for a first
    // position, this proxy is already live and positioned, so give it its map
    // marker and overlay card immediately. If we ARE waiting,
    // RevealOnFirstTransform does it once the peer's real position lands.
    if (!m_awaitingFirstTransform)
        RegisterProxyVisuals();

    LOG_INFO("[CoSyncPlayer] Spawn END entity=%u actor=%p", entityID, actorRef);
    if (createType == CoSyncEntityType::Player)
        LOG_INFO("[CoSyncProxy] SPAWN SteamID=%llu EntityID=%u Username='%s' ProxyFormID=0x%08X ProxyRef=%p disabledUntilFirstPos=%d",
            (unsigned long long)CoSyncNet::GetSteamIDForEntity(entityID), entityID, username.c_str(),
            actorRef->formID, static_cast<void*>(actorRef), m_awaitingFirstTransform ? 1 : 0);
    return true;
}

// Map marker + overlay card for a proxy that is live, positioned and enabled.
// Called from exactly two places -- SpawnInWorld (when the peer's position was
// already known) and RevealOnFirstTransform (when it wasn't) -- so a proxy gets
// its visuals down EITHER path. Having this only on the reveal path is what
// left the host with no marker and no overlay card.
void CoSyncPlayer::RegisterProxyVisuals()
{
    if (!hasSpawned || !actorRef)
        return;
    if (createType != CoSyncEntityType::Player)
        return;          // NPC/companion clones must not consume marker slots
    if (m_visualsRegistered)
        return;
    m_visualsRegistered = true;

    CoSyncPlayerMarker::ShowForProxy(actorRef, entityID);

    // Seed the lobby card now. It used to be created ONLY by CoSyncHealthSync's
    // apply, so a peer had no overlay entry until a health/AP packet arrived --
    // which is why they appeared only once they ran (sprinting drains AP) or
    // took damage.
    CoSyncOverlay_UpdateLobbyEntry(
        entityID,
        username.c_str(),
        m_uiHealth,
        m_uiMaxHealth,
        static_cast<int>(m_uiLevel),
        false,
        m_uiRads);

    LOG_INFO("[CoSyncPlayer] entity=%u proxy visuals registered (map marker + overlay card)",
        entityID);
}

void CoSyncPlayer::ApplyUpdate(const EntityUpdatePacket& u)
{
    lastPacketTime = u.timestamp;


    const double   prevLocalTime  = m_lastRecvLocalTime;
    const NiPoint3 prevRecvPos    = m_prevRecvPos;
    const bool     hadPrevRecvPos = m_prevRecvHasPos;

    m_lastRecvLocalTime = NowSeconds();


    m_prevRecvPos    = u.pos;
    m_prevRecvHasPos = true;

    pendingPos = u.pos;
    pendingRot = u.rot;
    pendingVel = u.vel;
    hasPendingTransform = true;


    m_lastVel   = u.vel;
    m_lastPitch = u.rot.x;

    if (u.hasAnimVars && u.animVars.isValid)
    {


        const uint32_t prevLocoState = m_lastRecvAnimVars.integers[15];

        const float prevFPSpeed = m_lastRecvAnimVars.floats[4];

        m_lastRecvAnimVars = u.animVars;

        float hSpeed = sqrtf(u.vel.x * u.vel.x + u.vel.y * u.vel.y);

        if (hSpeed < 1.0f && hadPrevRecvPos)
        {
            const float dx = u.pos.x - prevRecvPos.x;
            const float dy = u.pos.y - prevRecvPos.y;
            const double dt = m_lastRecvLocalTime - prevLocalTime;
            if (dt > 0.005 && dt < 2.0)
            {
                const float derived = sqrtf(dx * dx + dy * dy) / static_cast<float>(dt);
                if (derived > 1.0f)
                {
                    hSpeed = derived;
                    LOG_EVERY_MS(2000, LOG_INFO("[CoSyncPlayer] entity=%u hSpeed DERIVED from pos-delta: %.1f (dx=%.1f dy=%.1f dt=%.3f)",
                        entityID, hSpeed, dx, dy, (float)dt));
                }
            }
        }

        if (m_lastRecvAnimVars.floats[4] < 1.0f)
        {
            m_lastRecvAnimVars.floats[4]  = hSpeed;
            m_lastRecvAnimVars.floats[10] = hSpeed;
            m_lastRecvAnimVars.floats[11] = hSpeed;
        }
        else
        {

            m_lastRecvAnimVars.floats[10] = m_lastRecvAnimVars.floats[4];
        }
        m_lastRecvAnimVars.floats[25] = u.vel.z;

        const float netVelMag = sqrtf(u.vel.x * u.vel.x + u.vel.y * u.vel.y);
        const float moveDirX  = (netVelMag > 1.0f) ? u.vel.x : (u.pos.x - prevRecvPos.x);
        const float moveDirY  = (netVelMag > 1.0f) ? u.vel.y : (u.pos.y - prevRecvPos.y);

        static constexpr float kPi = 3.14159265f;
        if (hSpeed > 5.0f)
        {
            const float velAngle = atan2f(moveDirY, moveDirX);
            float relDir = velAngle - u.rot.z;
            while (relDir >  kPi) relDir -= 2.0f * kPi;
            while (relDir < -kPi) relDir += 2.0f * kPi;
            m_lastRecvAnimVars.floats[0]  = relDir;
            m_lastRecvAnimVars.floats[8]  = relDir;
            m_lastRecvAnimVars.floats[16] = relDir * 57.2957795f;

            const uint32_t isBackward = (cosf(relDir) < 0.0f) ? 1u : 0u;
            m_lastRecvAnimVars.integers[25] = isBackward;
            m_lastRecvAnimVars.integers[24] = isBackward;

            uint32_t syncDir = 0;
            const float absDir = fabsf(relDir);
            if (absDir > 2.356f)                         syncDir = 2;
            else if (relDir >  0.785f && relDir <  2.356f) syncDir = 1;
            else if (relDir < -0.785f && relDir > -2.356f) syncDir = 3;
            m_lastRecvAnimVars.integers[23] = syncDir;
        }
        else
        {
            m_lastRecvAnimVars.floats[0]  = 0.0f;
            m_lastRecvAnimVars.floats[8]  = 0.0f;
            m_lastRecvAnimVars.floats[16] = 0.0f;
            m_lastRecvAnimVars.integers[23] = 0u;
            m_lastRecvAnimVars.integers[24] = 0u;
            m_lastRecvAnimVars.integers[25] = 0u;
        }

        const float effectiveSpeed = m_lastRecvAnimVars.floats[4];

        const bool inPA = m_isInPowerArmor;
        uint32_t locoState = 0;

        if (inPA)
        {
            if      (effectiveSpeed > 270.0f)                              locoState = 3;
            else if (prevLocoState == 3 && effectiveSpeed > 230.0f)       locoState = 3;
            else if (effectiveSpeed > 140.0f)                              locoState = 2;
            else if (prevLocoState == 2 && effectiveSpeed > 110.0f)       locoState = 2;
            else if (effectiveSpeed >  20.0f)                              locoState = 1;
            else if (prevLocoState == 1 && effectiveSpeed > 15.0f)        locoState = 1;
        }
        else
        {
            if      (effectiveSpeed > 370.0f)                              locoState = 3;
            else if (prevLocoState == 3 && effectiveSpeed > 310.0f)       locoState = 3;
            else if (effectiveSpeed > 200.0f)                              locoState = 2;
            else if (prevLocoState == 2 && effectiveSpeed > 165.0f)       locoState = 2;
            // Walk tier engages from ~10 u/s so slow walking (analog
            // partial-stick / walk key, ~15-70 u/s) animates instead of
            // sliding in the idle pose; the Speed float drives the blend
            // weight inside the tier so very slow speeds still look right.
            else if (effectiveSpeed >  10.0f)                              locoState = 1;
            else if (prevLocoState == 1 && effectiveSpeed >  7.0f)        locoState = 1;
        }
        m_lastRecvAnimVars.integers[1]  = (locoState >= 3) ? 1u : 0u;
        m_lastRecvAnimVars.integers[13] = (locoState >= 2) ? 1u : 0u;
        m_lastRecvAnimVars.integers[15] = locoState;
        m_lastRecvAnimVars.integers[20] = locoState;

        m_lastRecvAnimVars.integers[26] = (effectiveSpeed >= 5.0f) ? 1u : 0u;

        m_lastRecvAnimVars.integers[11] = (effectiveSpeed >= 5.0f) ? 0u : 13u;

        m_lastRecvAnimVars.integers[18] = 1u;


        if (effectiveSpeed > 320.0f)
            m_lastRecvAnimVars.booleans |=  (1ULL << 1);
        else
            m_lastRecvAnimVars.booleans &= ~(1ULL << 1);

        {
            // Round 138: weapon state per the SDK's own ActorState accessor —
            // (flags >> 1) & 7, drawn when >= 3 (kWeaponState_Drawn). The old
            // (flags >> 29) & 7 heuristic read undocumented bits.
            const uint32_t wsRaw = (m_lastRecvAnimVars.actorStateFlags2 >> 1) & 0x7u;
            const bool wActive = (wsRaw >= 3u);

            m_lastRecvAnimVars.integers[30] = wActive ? 0u : 1u;


            m_lastRecvAnimVars.integers[28] = wActive ? 1u : 0u;
        }


        // Round 141: bits 7 (isJumping) and 9 (bInJumpState) are indices 7
        // and 9 of kBooleanLookupTable in CoSyncAnimVarSystem.cpp — i.e. this
        // is the SENDER's REAL behavior-graph capture, ground truth exactly
        // like the sneak/attack fixes earlier this session. The code below
        // used to unconditionally OVERWRITE both bits from a crude
        // vel.z-threshold guess (>50 or <-150), discarding the real graph
        // state on every single packet — a jump that started ascending at
        // moderate vertical speed, or was already mid-air when threshold
        // wasn't crossed on that particular network sample, got silently
        // cleared back to "not jumping" here. Now OR'd: either signal being
        // true means in-air, so the real capture is never discarded, and the
        // velocity heuristic still catches falls where the graph var might
        // lag (physics-driven, not player-input-driven descents).
        const bool velInAir   = (u.vel.z > 50.0f || u.vel.z < -150.0f);
        const bool graphInAir = (m_lastRecvAnimVars.booleans & ((1ULL << 7) | (1ULL << 9))) != 0;
        const bool inAir = velInAir || graphInAir;

        if (inAir)
        {
            m_lastRecvAnimVars.booleans |=  (1ULL << 7);
            m_lastRecvAnimVars.booleans |=  (1ULL << 9);
            m_lastRecvAnimVars.integers[17] = 1u;
            m_lastRecvAnimVars.integers[27] = 1u;
        }
        else
        {
            m_lastRecvAnimVars.booleans &= ~(1ULL << 7);
            m_lastRecvAnimVars.booleans &= ~(1ULL << 9);
            m_lastRecvAnimVars.integers[17] = 0u;
            m_lastRecvAnimVars.integers[27] = 0u;
        }

        static int s_animRecvDiag = 0;
        if (++s_animRecvDiag % 300 == 1 || (inAir && s_animRecvDiag % 30 == 0))
            LOG_INFO("[CoSyncPlayer] entity=%u: AnimVars hSpeed=%.1f dir=%.2f locoState=%u sprint=%d jump=%d (vel=%d graph=%d) vel.z=%.1f pitch=%.2f diag#=%d",
                entityID, hSpeed,
                m_lastRecvAnimVars.floats[0],
                locoState,
                (m_lastRecvAnimVars.booleans & (1ULL << 1)) ? 1 : 0,
                inAir ? 1 : 0,
                velInAir ? 1 : 0, graphInAir ? 1 : 0,
                u.vel.z,
                u.rot.x,
                s_animRecvDiag);
    }
    else
    {
        static int s_noAnimDiag = 0;
        if (++s_noAnimDiag % 300 == 1)
            LOG_WARN("[CoSyncPlayer] entity=%u: no AnimVars in packet (hasAnimVars=%d) diag#=%d",
                entityID, u.hasAnimVars ? 1 : 0, s_noAnimDiag);
    }

    if (u.equippedWeaponFormID != m_lastQueuedWeaponFormID)
    {
        m_pendingWeaponFormID       = u.equippedWeaponFormID;
        m_pendingWeaponChange       = true;
        m_lastQueuedWeaponFormID    = u.equippedWeaponFormID;
        m_weaponConfirmedEquipped   = false;
        m_lastWeaponRetryTime       = NowSeconds();
    }
    // Round 143: periodic retry when the wire value hasn't changed but the
    // last equip attempt was never confirmed landed (WEAPON VERIFY
    // present=0/equipped=0 in the log) — without this, a silent AddItem/
    // EquipItem failure was permanent until the real player switched
    // weapons again, since the edge-trigger above only fires on a VALUE
    // change. Same self-healing property OutfitSync already had via its
    // periodic 10s re-broadcast.
    // 0.3.3 (Stage 3): every 10 s, compare the wire weapon with what the
    // ENGINE has equipped on the proxy, not with m_weaponConfirmedEquipped.
    // That flag stays true after a successful verify even if something later
    // takes the weapon off the proxy (a 3D rebuild, a slot conflict), and the
    // proxy then stayed unarmed until the real player switched weapons.
    else if (u.equippedWeaponFormID != 0 &&
             (NowSeconds() - m_lastWeaponRetryTime) > 10.0 &&
             !(actorRef && CoSyncGameAPI::IsDeadOrDying(actorRef)))   // a corpse is never re-armed
    {
        m_lastWeaponRetryTime = NowSeconds();
        bool present = false, equipped = false;
        const bool readOk = hasSpawned && actorRef &&
            CoSyncGameAPI::GetInventoryEquipState(actorRef, u.equippedWeaponFormID, present, equipped);
        if (!m_weaponConfirmedEquipped || (readOk && !equipped))
        {
            if (m_weaponConfirmedEquipped)
                LOG_UPDATED("[CoSyncPlayer] entity=%u weapon 0x%08X no longer equipped on the proxy -- re-equipping",
                    entityID, u.equippedWeaponFormID);
            m_pendingWeaponFormID     = u.equippedWeaponFormID;
            m_pendingWeaponChange     = true;
            m_weaponConfirmedEquipped = false;
        }
    }


    // Latch, never clear: the sender's field drops back to 0 the moment the
    // grenade leaves their hand, and TickAnimVars still needs to know what it
    // was when it picks the throw clip. Overwritten by the next real
    // throwable, which is the only time the answer actually changes.
    if (u.equippedThrowableFormID != 0)
        m_lastThrowableFormID = u.equippedThrowableFormID;

    // NOTE: u.isThrowing / u.isReloading / u.isFiring are intentionally NOT
    // consumed here any more. Fire, reload and throw are discrete events and
    // are delivered as AEV| one-shots edge-detected at frame rate on the
    // sender; level-sampling them at packet rate provably dropped nearly
    // every shot. The packet fields remain only so a mixed-build session
    // still parses.

    m_pendingCombatActive = u.isCombatActive;
    m_pendingCombatTarget = u.combatTargetEntityID;
    m_pendingWeaponDrawn  = u.weaponDrawnState;
    m_pendingInDialog     = u.isInDialog;
    if (u.hasMoveChannels)
    {
        m_hasMoveChannels = true;
        m_moveSpeed       = u.moveSpeed;
        m_moveDirection   = u.moveDirection;
        m_moveRecvTime    = NowSeconds();
    }

    if (!hasSpawned || !actorRef)
        return;

    // NPC-type proxies used to hard-snap here (one PositionRemoteActor per
    // 20Hz packet) while players got the interpolation buffer below. That
    // 20Hz stepping was most of the visible "NPC world not synced" jank on
    // the client — world NPCs now ride the same buffered interpolation path.

    if (m_hasAny && u.timestamp < m_lastHostTime)
        return;

    // 0.3.3 (Stage 1): place each sample on the local timeline by the time
    // the SENDER captured it, not by when it happened to arrive.
    //
    // The buffer used to stamp samples with their arrival time. EU travels
    // unreliable and the relay batches, so two samples taken 66 ms apart can
    // land 5 ms apart and the next one 130 ms later; interpolating on arrival
    // time turns that delivery jitter straight into speed changes of the
    // model (surging and stalling at a constant real walking speed).
    //
    // offset = arrival - senderStamp includes the (varying) network delay; the
    // minimum over the last ~2 s is the least-delayed sample, so
    // senderStamp + minOffset is the ideal arrival time of each sample.
    // kInterpDelay then absorbs the jitter above that minimum. The offset
    // follows the windowed minimum gradually so clock drift or a route change
    // never makes the timeline jump.
    const double arrival = NowSeconds();
    double sampleOffset = arrival - u.timestamp;
    if (u.timestamp <= 0.0)
        sampleOffset = 0.0;                           // unstamped: fall back to arrival time
    m_clockSamples[m_clockSampleNext] = sampleOffset;
    m_clockSampleNext = (m_clockSampleNext + 1) % kClockSamples;
    if (m_clockSampleCount < kClockSamples)
        ++m_clockSampleCount;
    double windowMin = m_clockSamples[0];
    for (int i = 1; i < m_clockSampleCount; ++i)
        if (m_clockSamples[i] < windowMin)
            windowMin = m_clockSamples[i];
    if (!m_clockOffsetValid || std::fabs(windowMin - m_clockOffset) > 1.0)
    {
        m_clockOffset      = windowMin;               // first sample, or a real clock jump
        m_clockOffsetValid = true;
    }
    else
    {
        m_clockOffset += (windowMin - m_clockOffset) * 0.1;
    }

    NetTransform tf{};
    tf.pos       = u.pos;
    tf.rot       = u.rot;
    tf.vel       = u.vel;
    tf.scale     = 1.0f;
    tf.hostTime  = u.timestamp;
    tf.localTime = (u.timestamp > 0.0) ? (u.timestamp + m_clockOffset) : arrival;
    // Never earlier than the previous sample on the timeline.
    if (!m_tfBuffer.empty() && tf.localTime < m_tfBuffer.back().localTime)
        tf.localTime = m_tfBuffer.back().localTime;

    m_tfBuffer.push_back(tf);
    m_lastHostTime      = tf.hostTime;
    m_hasAny = true;

    // Distinct from m_hasAny: ApplyPendingTransformIfAny SEEDS m_hasAny from
    // the proxy's own PlaceAtMe position (i.e. right on top of the local
    // player) when no real packet has arrived yet. Revealing on that seed
    // would put the peer at our feet -- the exact thing being fixed. Only this
    // path, a genuine peer transform, sets the flag.
    m_hasRealTransform = true;

    while (m_tfBuffer.size() > 12)
        m_tfBuffer.pop_front();

    // We now know where this peer actually is, so a proxy parked disabled at
    // our feet can be placed and shown.
    RevealOnFirstTransform();
}

void CoSyncPlayer::RevealOnFirstTransform()
{
    if (!m_awaitingFirstTransform || !hasSpawned || !actorRef)
        return;
    if (m_tfBuffer.empty())
        return;

    // A proxy can be freed between spawn and the first packet (cell
    // transitions free proxies, not just reloads) -- never touch a cached
    // Actor* without re-validating.
    if (!CoSyncGameAPI::IsFormStillLive(static_cast<TESForm*>(actorRef)))
    {
        LOG_WARN("[CoSyncPlayer] entity=%u reveal skipped — actor no longer live", entityID);
        m_awaitingFirstTransform = false;
        return;
    }

    m_awaitingFirstTransform = false;

    // Position FIRST, then enable, so it never renders for even one frame at
    // the local player's feet before snapping away.
    const NetTransform& tf = m_tfBuffer.back();
    ApplyProxyTransform(tf.pos, tf.rot);

    CoSyncGameAPI::EnableProxyActor(actorRef);

    // Enable() restores armor through the biped system but NOT individually
    // equipped weapons (round 143). Re-fire the equip a few frames later, once
    // the enable has settled, or the peer reappears unarmed.
    if (m_lastQueuedWeaponFormID != 0)
    {
        m_postRefreshWeaponFormID = m_lastQueuedWeaponFormID;
        m_postRefreshWeaponDelay  = 8;
        m_weaponConfirmedEquipped = false;
    }

    LOG_INFO("[CoSyncPlayer] entity=%u REVEALED at (%.0f,%.0f,%.0f) — first position received "
             "(weaponReEquip=0x%08X)",
        entityID, tf.pos.x, tf.pos.y, tf.pos.z, m_postRefreshWeaponFormID);

    // Live, positioned and enabled now -- safe to give it its marker and
    // overlay card. Doing this at spawn time instead aimed the marker at a
    // disabled actor and crashed the host inside PackHandle.
    RegisterProxyVisuals();

    // This is the moment the peer's character is actually in the world -- the
    // proxy was parked disabled until now, so it is the earliest point where
    // "they have loaded" is true rather than merely "they connected".
    {
        char note[160];
        std::snprintf(note, sizeof(note), "%s loaded in",
            username.empty() ? "Player" : username.c_str());
        CoSyncGameAPI::QueueNotification(note);
    }
}

void CoSyncPlayer::ApplyPendingTransformIfAny()
{
    if (!hasSpawned || !actorRef || !hasPendingTransform)
        return;

    // Player-type: ApplyUpdate() already pushes every packet into m_tfBuffer
    // and TickSmoothing() owns all position application via interpolation/
    // extrapolation between buffered samples. This function exists only for
    // the one-shot catch-up case (a transform arrived before SpawnInWorld
    // finished, m_hasAny still false). Once the buffer has real samples,
    // PlayerManager::Tick() calls this EVERY tick (hasPendingTransform is
    // set true on every incoming packet) -- if we kept hard-snapping +
    // clearing the buffer here every time, TickSmoothing never sees more
    // than 1 sample and never interpolates: position only ever hard-snaps
    // at the ~20Hz network rate. That was the visible jitter/stutter.
    if (m_hasAny)
    {
        hasPendingTransform = false;
        return;
    }

    // pendingPos here is from the HELLO packet, captured before P2's save
    // finished loading — always stale. PlaceAtMe already put the actor near
    // the local player in a loaded cell. Moving it to pendingPos sends it to
    // an unloaded cell and 3D never loads. Seed the buffer with the actor's
    // actual PlaceAtMe spawn position so it stays visible while EU packets
    // arrive with P2's real position.
    NiPoint3 seedPos{}, seedRot{};
    if (!CoSyncGameAPI::GetActorWorldTransform(actorRef, seedPos, seedRot))
    {
        seedPos = pendingPos;
        seedRot = pendingRot;
        LOG_WARN("[CoSyncPlayer] entity=%u GetActorWorldTransform failed at seed — falling back to HELLO pos (%.1f,%.1f,%.1f)",
            entityID, seedPos.x, seedPos.y, seedPos.z);
    }
    else
    {
        LOG_INFO("[CoSyncPlayer] entity=%u seeding tfBuffer from PlaceAtMe pos (%.1f,%.1f,%.1f) — NOT applying stale HELLO pos (%.1f,%.1f,%.1f)",
            entityID, seedPos.x, seedPos.y, seedPos.z, pendingPos.x, pendingPos.y, pendingPos.z);
    }

    m_tfBuffer.clear();

    const double seedNow = NowSeconds();
    NetTransform tf{};
    tf.pos       = seedPos;
    tf.rot       = seedRot;
    tf.scale     = 1.0f;
    tf.hostTime  = lastPacketTime;
    tf.localTime = seedNow;

    m_tfBuffer.push_back(tf);
    m_lastHostTime      = tf.hostTime;
    m_lastRecvLocalTime = seedNow;
    m_hasAny = true;

    hasPendingTransform = false;
}

// Farther than any walk/run between two network updates: a teleport.
static constexpr float kPuppetSnapDist = 2000.f;

void CoSyncPlayer::ApplyProxyTransform(const NiPoint3& pos, const NiPoint3& rot)
{
    if (!actorRef)
        return;
    // AI-alive enemy puppets: native SetPosition steer (cell-aware, doesn't
    // fight the engine's own animation). Players: raw NiNode write.
    if (CoSyncGameAPI::PuppetModeEnabled() && createType == CoSyncEntityType::NPC)
    {
        // A big jump (its owner fast-travelled / went through a door with it,
        // e.g. the host's companion) can't be steered: SetPosition never
        // changes cell, so the puppet stayed where it was (user test
        // 2026-09-26). Relocate it properly once, then keep steering.
        const float dx = actorRef->pos.x - pos.x;
        const float dy = actorRef->pos.y - pos.y;
        const float dz = actorRef->pos.z - pos.z;
        static std::unordered_map<uint32_t, double> s_lastSnap;   // entityID -> time (once a second at most)
        double& lastSnap = s_lastSnap[entityID];
        if (dx * dx + dy * dy + dz * dz > kPuppetSnapDist * kPuppetSnapDist && NowSeconds() - lastSnap > 1.0)
        {
            lastSnap = NowSeconds();
            CoSyncGameAPI::SnapActor(actorRef, pos, rot);
            LOG_UPDATED("[CoSyncPlayer] entity=%u jumped %.0f units -- relocated", entityID,
                sqrtf(dx * dx + dy * dy + dz * dz));
        }
        // 0.3.6: seated/leaning on the owner's furniture -> the engine holds
        // the actor on the furniture marker; steering it to the network
        // position every frame pulled it off again (the reported "standing
        // instead of sitting", "fighting the synchronized movement").
        if (m_npcFurnitureRef != 0)
        {
            // 0.4.3: with the combat-entry gate live the puppet cannot enter a
            // fight its owner is not in, so there is nothing to stop here.
            if (!CoSyncPuppetAuthority::CombatGateActive())
                CoSyncGameAPI::StopActorCombat(actorRef);
            CoSyncPuppetAuthority::SetPuppetMovement(actorRef, 0.f, 0.f, false);
            m_lastAnimApplyTime = NowSeconds();
            return;
        }
        CoSyncGameAPI::SteerPuppetPosition(actorRef, pos, rot.z);
        // 0.3.3: SetPosition zeroed the controller velocity (verified); hand
        // the engine the owner's velocity instead, so the puppet is moving,
        // not being slid while "standing", between network updates.
        // Only while updates are fresh: a held position with a stale
        // velocity would make the physics step push it forward every frame.
        {
            const bool fresh = (NowSeconds() - m_lastRecvLocalTime) < 0.25;
            CoSyncGameAPI::SetActorControllerVelocity(actorRef, fresh ? m_lastVel : NiPoint3(0.f, 0.f, 0.f));
        }

        // 0.4.3: the engine's locomotion channels carry the OWNER's values
        // (the puppet's own movement controller would write what its own AI
        // wants). Released when the owner's stream goes stale.
        {
            const bool freshMove = m_hasMoveChannels && (NowSeconds() - m_moveRecvTime) < 0.5;
            CoSyncPuppetAuthority::SetPuppetMovement(actorRef, m_moveSpeed, m_moveDirection, freshMove);
        }

        // Re-neutralize every tick, not just at spawn: the puppet's local AI
        // (still fully alive by design) can decide to enter combat against
        // the local client's own player at any point in a long session, and
        // once it does it keeps running its own local movement/attack logic
        // every tick, fighting the SteerPuppetPosition call above -- IsInCombat
        // gates this internally so the repeated call is a cheap no-op the rest
        // of the time.
        // 0.4.3: only without the combat-entry gate. With it, local combat
        // cannot start at all, and the owner's own fight (started on the
        // puppet with the same target) must not be stopped every frame -- that
        // flapping is what made the HUD enemy meter flicker (Bug #3).
        if (!CoSyncPuppetAuthority::CombatGateActive())
            CoSyncGameAPI::StopActorCombat(actorRef);

        // Drag the client's own hidden original along with the proxy. Quest
        // aliases and quest target markers still reference that original ref,
        // so if it stays where it was disabled the objective marker is pinned
        // there while the NPC visibly walks away -- the reported "marker did
        // not follow the quest npc for client". Throttled to 1Hz inside.
        CoSyncNpcWorldSync::SyncDisabledOriginalToProxy(entityID, pos);
        // Puppets skip TickAnimVars (engine animates them natively), so update
        // the apply timestamp here to keep NpcSyncDiag's OK/STUCK verdict
        // meaningful — otherwise every steered puppet reads STUCK forever.
        m_lastAnimApplyTime = NowSeconds();
    }
    else
    {
        // 0.5.3/0.5.6: in the Vault 111 cryo rooms every player stands in
        // "the" player pod on their own machine during the vanilla sequence;
        // here each other player is shown in their own pod (CoSyncVault111).
        NiPoint3 shownPos = pos, shownRot = rot;
        CoSyncVault111::RemapProxyIntoPod(entityID, shownPos, shownRot);
        CoSyncGameAPI::PositionRemoteActor(actorRef, shownPos, shownRot);

        // 0.3.4: PositionRemoteActor moves the character controller with
        // Actor::SetPosition(pos, true) (vtable 0xCA, sub_140C60B50), which
        // also zeroes the controller's linear velocity every frame (IDA: the
        // controller's vfunc +0x1D0, slot 0x3A, is called with the zero
        // vector). The puppet branch above already hands the owner's velocity
        // back; player proxies never did, so from 0.3.3 on the engine saw a
        // controller that never moves. A first-person sender's pose is applied
        // to the upper body only, and the legs are left to the proxy's own
        // locomotion graph, which is fed from the engine's movement state --
        // so the proxy slid with standing legs. INFERRED link (the exact
        // consumer that turns controller velocity into the Speed graph input
        // is still unverified, see docs/SYNC_REWORK_REPORT.md); same
        // freshness rule as the puppet path.
        const bool fresh = (NowSeconds() - m_lastRecvLocalTime) < 0.25;
        CoSyncGameAPI::SetActorControllerVelocity(actorRef, fresh ? m_lastVel : NiPoint3(0.f, 0.f, 0.f));
    }
}

void CoSyncPlayer::TickSmoothing(double now)
{
    if (!hasSpawned || !actorRef)
        return;

    // NPC-type proxies interpolate too now (see ApplyUpdate). On the HOST the
    // buffer for its own authoritative world NPCs is never fed (no EU packets
    // arrive for them), so the empty-buffer check below keeps the host from
    // ever touching its real NPCs from here.
    if (m_tfBuffer.empty())
        return;

    const double renderTime = now - kInterpDelay;


    if (m_tfBuffer.size() < 2 || renderTime < m_tfBuffer.front().localTime)
    {
        ApplyProxyTransform(m_tfBuffer.front().pos, m_tfBuffer.front().rot);
        return;
    }


    const NetTransform& last = m_tfBuffer.back();
    if (renderTime >= last.localTime)
    {
        const double dt = renderTime - last.localTime;

        if (dt < 0.20)
        {
            NiPoint3 extPos{
                last.pos.x + last.vel.x * float(dt),
                last.pos.y + last.vel.y * float(dt),
                last.pos.z + last.vel.z * float(dt)
            };
            ApplyProxyTransform(extPos, last.rot);
        }
        else
        {
            ApplyProxyTransform(last.pos, last.rot);
        }
        return;
    }


    NetTransform a = m_tfBuffer.front();
    NetTransform b = m_tfBuffer.back();

    for (size_t i = 1; i < m_tfBuffer.size(); ++i)
    {
        if (m_tfBuffer[i].localTime >= renderTime)
        {
            a = m_tfBuffer[i - 1];
            b = m_tfBuffer[i];
            break;
        }
    }

    const double denom = b.localTime - a.localTime;
    double t = (denom > 0.0001) ? (renderTime - a.localTime) / denom : 0.0;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    const NiPoint3 pos = Lerp(a.pos, b.pos, t);

    // ── Short-arc rotation blend (list item #7) ─────────────────────────────
    // These are EULER ANGLES, not a linear quantity. A plain Lerp between,
    // say, 3.10 and -3.10 rad -- an 0.08 rad step across the +/-pi wrap --
    // sweeps the long way round instead, spinning the proxy through nearly a
    // full turn every time the player's heading crosses that boundary. The
    // anim-var path already solved this with ShortArcLerp; the transform path
    // was still doing the naive thing.
    auto ShortArcAngle = [](float from, float to, float f) -> float
    {
        float diff = to - from;
        while (diff >  3.14159265f) diff -= 6.28318531f;
        while (diff < -3.14159265f) diff += 6.28318531f;
        return from + diff * f;
    };

    const float tf = static_cast<float>(t);
    NiPoint3 rot{
        ShortArcAngle(a.rot.x, b.rot.x, tf),
        ShortArcAngle(a.rot.y, b.rot.y, tf),
        ShortArcAngle(a.rot.z, b.rot.z, tf)
    };

    ApplyProxyTransform(pos, rot);
}

// Replays the owner's discrete animation events (AEV|: fire, reload, throw),
// one per queued event. Players call it from TickAnimVars; NPC puppets (which
// skip TickAnimVars so the engine animates their locomotion) call it directly
// from PlayerManager -- before 0.3.1 puppets never played these at all, so an
// NPC fighting on its owner's machine stood still everywhere else (K3).
void CoSyncPlayer::ReplayDiscreteEvents()
{
    if (!actorRef)
        return;

    // Discrete anim events, one replay per queued event. Sender-side
    // edge detection at frame rate (CoSyncNet::SendAnimEdges) is what
    // makes rapid fire work: N shots on the shooter become N counts
    // here. Draining with exchange(0) keeps a burst that arrived
    // between ticks intact instead of collapsing it to one.
    //
    // Capped per tick so a flood (or a stuck sender) cannot spend the
    // whole frame dispatching anim events.
    constexpr int kMaxEventsPerTick = 8;

    int fires = m_evFire.exchange(0);
    if (fires > kMaxEventsPerTick) fires = kMaxEventsPerTick;
    for (int i = 0; i < fires; ++i)
    {
        // Vanilla ordering: enter the attack state, then the shot,
        // then the muzzle effect. attackStart on its own never plays
        // a shot -- that was the "melee animates, shooting doesn't"
        // bug.
        PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::AttackStart);
        PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::WeaponFire);
        PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::WeaponFireEffect);
    }
    if (fires > 0)
        LOG_UPDATED("[TickAnimVars] entity=%u weaponFire x%d", entityID, fires);

    if (m_evReloadStart.exchange(0) > 0)
    {
        PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::ReloadStart);
        LOG_UPDATED("[TickAnimVars] entity=%u reloadStart", entityID);
    }
    if (m_evReloadEnd.exchange(0) > 0)
    {
        PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::ReloadEnd);
        LOG_UPDATED("[TickAnimVars] entity=%u reloadEnd", entityID);
    }

    if (m_evThrowGrenade.exchange(0) > 0)
    {
        PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::GrenadeThrowStart);
        LOG_UPDATED("[TickAnimVars] entity=%u grenadeThrowStart", entityID);
    }
    if (m_evThrowMine.exchange(0) > 0)
    {
        PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::MineThrowStart);
        LOG_UPDATED("[TickAnimVars] entity=%u mineThrowStart", entityID);
    }
    if (m_evThrowEnd.exchange(0) > 0)
    {
        PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::ThrowEnd);
        LOG_UPDATED("[TickAnimVars] entity=%u throwEnd", entityID);
    }
}

void CoSyncPlayer::TickAnimVars()
{
    if (!hasSpawned || !actorRef)
        return;

    // round 97 diagnostic: confirms the fix removing the duplicate
    // TickHook.cpp call sites (RemoteActorUpdate_Hook/ActorUpdate_Hook used
    // to also call this once per frame, doubling the exponential-smoothing
    // rate) actually held. CoSyncPlayerManager::Tick() is now the only
    // caller, once per frame, so callsPerEntity/sec should settle near the
    // game's frame rate, NOT roughly double it.
    {
        static std::unordered_map<uint32_t, uint32_t> s_callsThisSecond;
        static std::unordered_map<uint32_t, double>   s_lastLogTime;
        const double now = NowSeconds();
        uint32_t& count = s_callsThisSecond[entityID];
        ++count;
        double& lastLog = s_lastLogTime[entityID];
        if (now - lastLog >= 5.0)
        {
            LOG_INFO("[TickAnimVars] entity=%u calls/5s=%u (should be about 5x the frame rate, not roughly double that)",
                entityID, count);
            lastLog = now;
            count = 0;
        }
    }

    // UPDATE 2026-06-21 (round 74): round 73 fixed the engine's missing
    // animGraphHolder(+0x48) null-check at Fallout4.exe+0x4C4E00 (a
    // background-worker-thread crash), but the very next live test crashed
    // again a few frames later -- this time inside the engine's own
    // BSAnimationGraphManager variable/string lookup chain reached from
    // GetVariableSetDirect's vtable[4] call below (RCX held a tagged/invalid
    // handle-looking value 0xFF000000000000A8, not a real pointer). That
    // call IS wrapped in __try/__except here, but the crash still reached
    // CoSync_VEH and killed the process -- the engine's own call chain
    // passes through at least one true leaf function with no .pdata/unwind
    // info (confirmed: our VEH's stack walker hit "no unwind info, stopping
    // walk" at the exact crash RIP), which can make x64 SEH unable to walk
    // back out to our handler at all. Same root family as round 73 (engine
    // assumes a fully-Load3D'd actor; Load3D is async per
    // [[project_fo4wrld_reference]]'s dossier and ours hasn't finished) but
    // this time inside structures we can't safely null-check from outside
    // (graph-internal string/variable hash tables, not a single pointer
    // field). Rather than chase individual missing checks deeper into
    // engine internals one crash at a time, give every freshly-spawned
    // proxy's animation graph a flat grace period to finish settling before
    // we touch it at all.
    constexpr double kAnimGraphSettleDelay = 0.5;
    if (NowSeconds() - m_spawnTime < kAnimGraphSettleDelay)
        return;

    // UPDATE 2026-06-17: re-enabled for player-type proxies. Was disabled
    // entirely after ruling out timing, bad values, and write mechanism (see
    // git history / project_cosync_crash_fixes.md memory for the full
    // investigation) -- left with "likely cause: EnableActorAI/DisableActorAI
    // never actually ran" as the remaining theory, pending IDA root-cause.
    // That's now done: CoSyncGameAPI::EnableActorAI/DisableActorAI were
    // calling Actor.EnableAI (registered as
    // NativeFunction2<Actor,void,bool,bool>, i.e. two required bool args)
    // with zero and one argument respectively -- a real, concretely-verified
    // bug, fixed. If these proxies were sitting in a bad AI/process state
    // because that call never properly took effect, this should resolve it.
    // One-time log below so the next session's log makes it obvious whether
    // this path is reached and whether it survives first contact.
    static bool s_loggedFirstPlayerProxyAnimTouch = false;
    if (createType == CoSyncEntityType::Player && !s_loggedFirstPlayerProxyAnimTouch)
    {
        s_loggedFirstPlayerProxyAnimTouch = true;
        LOG_INFO("[CoSyncPlayer] First time touching this player proxy's animation graph "
                 "since the AI-enable fix — it should not crash here anymore. "
                 "If a [CRASH] line shows up right after this, that fix did not hold.");
    }

    // Weapon draw delay: run BEFORE IsGraphReady because DrawWeaponEngine
    // (vtable slot 201 / DrawWeaponMagicHands) is a native engine call that
    // does NOT read animation graph variables. Placing this after IsGraphReady
    // caused the countdown to stall whenever PlaceAtMe_Native's failed PA
    // chassis spawn left IsGraphReady() returning false — the proxy kept its
    // human graph but the Is3DLoaded path returned spuriously false.
    if (m_weaponDrawDelay > 0)
    {
        --m_weaponDrawDelay;
        if (m_weaponDrawDelay == 0)
        {
            const bool engineOk = CoSyncEquipHook::DrawWeaponEngine(actorRef, true);
            if (!engineOk)
                PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::WeaponDraw);
            LOG_UPDATED("[TickAnimVars] entity=%u weaponDraw DELAYED fire (%s)%s", entityID,
                engineOk ? "engine DrawWeaponMagicHands" : "graph-event fallback",
                m_isInPowerArmor ? " [PA]" : "");
        }
    }

    // NPC world-actor puppets: same "run before IsGraphReady" reasoning as
    // the player block just above -- DrawWeaponEngine is a native vtable call
    // that doesn't touch the animation graph, so it shouldn't wait on graph
    // readiness (which can take 3.5s+ after spawn, per kAnimGraphSettleDelay's
    // own history). Originally placed after IsGraphReady/LoadAnimationVariables
    // below; moved here after a live test where an NPC was killed well under
    // a second post-spawn and never got a weapon drawn -- graph readiness
    // hadn't happened yet, so the old placement never even ran. Uses
    // m_lastRecvAnimVars directly (pre-processing) rather than the "smoothed"
    // local further down, which doesn't exist yet at this point in the
    // function; actorStateFlags2 itself isn't touched by the smoothing steps
    // in between, so the bits read identically either way. isCreatureSnapshot
    // isn't computed yet either at this point, so it's inlined here instead.
    if (createType == CoSyncEntityType::NPC && !m_isInPowerArmor &&
        !(m_lastRecvAnimVars.isValid && (m_lastRecvAnimVars.booleans & kCreatureSnapshotFlag)))
    {
        const uint32_t npcWsRaw = (m_lastRecvAnimVars.actorStateFlags2 >> 1) & 0x7u;
        const bool npcWeaponDrawn = (npcWsRaw >= 3u);
        if (m_lastRecvAnimVars.isValid)
            CoSyncProxyWeaponAuthority::SetReplicatedDrawn(actorRef, npcWeaponDrawn);
        if (npcWeaponDrawn != m_prevNpcWeaponDrawn)
        {
            if (!CoSyncEquipHook::DrawWeaponEngine(actorRef, npcWeaponDrawn))
                PSC_SendAnimEvent(actorRef, npcWeaponDrawn
                    ? CoSyncAnimUtils::Events::WeaponDraw
                    : CoSyncAnimUtils::Events::WeaponSheathe);
            LOG_INFO("[TickAnimVars] entity=%u NPC weapon%s (wsRaw=%u)",
                entityID, npcWeaponDrawn ? "Draw" : "Sheathe", npcWsRaw);
            m_prevNpcWeaponDrawn = npcWeaponDrawn;
        }
    }

    if (!CoSyncAnimVarSystem::IsGraphReady(actorRef))
    {
        // See CoSyncPlayer.h m_3dNotLoadedSince/m_3dLoadRetried comment:
        // IsGraphReady checking Is3DLoaded() first with no retry meant a
        // proxy whose 3D got stuck mid-stream (much likelier with 30-40+
        // simultaneous spawns in a dense area) simply never animated again.
        // Give it one kick: if still not ready kAnimGraphSettleDelay*6
        // seconds after first observed not-ready, run the same
        // Disable()+Enable() cycle a fresh spawn gets, once.
        // 0.3.6: a proxy WorldSync hid for a location mismatch is disabled on
        // purpose, so its graph is never "ready" -- the retries below then
        // re-ENABLED it (RefreshActor3D = Disable+Enable) 2 ms after the
        // hide, at coordinates from another worldspace. 0.3.5 host log
        // 11680 21:02:13.783-.839: HIDDEN, "PA graph still not ready 0.0s",
        // then the engine freed the re-enabled actor 56 ms later and the
        // client's proxy was lost. WorldSync's SHOW path rebuilds it.
        if (m_proxyHiddenForCell)
        {
            m_3dNotLoadedSince = 0.0;
            return;
        }
        const double now = NowSeconds();
        if (m_3dNotLoadedSince == 0.0)
            m_3dNotLoadedSince = now;
        else if (!m_3dLoadRetried && (now - m_3dNotLoadedSince) >= (kAnimGraphSettleDelay * 6.0))
        {
            m_3dLoadRetried = true;
            m_idleEventSent = false;
            LOG_WARN("[TickAnimVars] entity=%u 3D/graph still not ready %.1fs after spawn -- retrying RefreshActor3D",
                entityID, now - m_spawnTime);
            CoSyncGameAPI::RefreshActor3D(actorRef);
        }
        // PA entry swaps the whole behavior graph -- a bigger transition than
        // a fresh spawn's one-time load, which the single retry above was
        // tuned for. Keep periodically retrying (instead of giving up after
        // one attempt) while in PA, so a slow graph swap eventually recovers
        // rather than leaving the proxy permanently unsynced/un-animated.
        // 0.3.6: the not-ready state must itself have lasted the settle
        // window. Only the time since the LAST retry was checked, so after a
        // loading screen (stale m_lastPaGraphRetryTime) the first not-ready
        // frame fired a Disable+Enable at once ("0.0s after PA entry", every
        // load in the 0.3.5 logs), stacking on WorldSync's own rebuild.
        if (m_isInPowerArmor &&
            (now - m_3dNotLoadedSince) >= (kAnimGraphSettleDelay * 6.0) &&
            (now - m_lastPaGraphRetryTime) >= (kAnimGraphSettleDelay * 6.0))
        {
            m_lastPaGraphRetryTime = now;
            LOG_WARN("[TickAnimVars] entity=%u PA graph still not ready %.1fs after PA entry -- retrying RefreshActor3D",
                entityID, now - m_3dNotLoadedSince);
            CoSyncGameAPI::RefreshActor3D(actorRef);
        }
        return;
    }
    m_3dNotLoadedSince = 0.0;
    m_lastAnimApplyTime = NowSeconds();

    // NPC proxies: fire IdleForceDefaultState natively on the first successful
    // IsGraphReady() pass. InitNpcProxy queues this via Papyrus at 120-tick
    // defer (~0.9s after spawn) but the behavior graph isn't ready until ~3.5s,
    // so that Papyrus event lands in an unready graph and is dropped. Here the
    // graph is guaranteed live, so the event is guaranteed to land.
    if (createType == CoSyncEntityType::NPC && !m_idleEventSent)
    {
        m_idleEventSent = true;
        CoSyncAnimUtils::SendEvent(actorRef, "IdleForceDefaultState");
        LOG_UPDATED("[TickAnimVars] entity=%u NPC first-ready: IdleForceDefaultState fired natively", entityID);
    }

    // Non-humanoid creature action-sync: apply the safe probe-then-named-set
    // writer for locomotion vars, then FALL THROUGH (no early return) so
    // creatures get the same shared event-dispatch blocks below (sneak,
    // weapon, attack, move/sprint, sitting) that humanoid proxies already
    // get -- previously this hard-returned here, meaning creature proxies
    // never fired a single attack/idle/move event, only mirrored position.
    // The raw indexed writer further below (LoadAnimationVariables /
    // LoadPowerArmorLocomotion) must still be hard-skipped for creatures --
    // see the isCreatureSnapshot guard at that call site -- it corrupts a
    // creature's much smaller variable table (round-28 STATUS_ILLEGAL_
    // INSTRUCTION history in CoSyncCreatureAnimSync.cpp).
    const bool isCreatureSnapshot = m_lastRecvAnimVars.isValid &&
        (m_lastRecvAnimVars.booleans & kCreatureSnapshotFlag);
    if (isCreatureSnapshot)
    {
        CoSyncCreatureAnimSync::LoadCreatureVars(actorRef, m_lastRecvAnimVars);
    }

    // PA state transition: fire PowerArmorEnter/Exit when m_isInPowerArmor
    // changes. The AE| packet path already forwards these events from the
    // sender's behavior graph, but SwitchToPowerArmor fires asynchronously
    // from the VM queue — this ensures the proxy's own behavior graph gets
    // the event even if SwitchToPowerArmor fires it internally before the
    // AE| packet arrives.
    if (m_isInPowerArmor != m_prevInPA)
    {
        if (m_isInPowerArmor)
        {
            bool acc = PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::PowerArmorEnter);
            LOG_UPDATED("[TickAnimVars] entity=%u PowerArmorEnter, graph accepted=%d", entityID, acc ? 1 : 0);
            // PA graph-index diagnostic: confirm/rule out a graph-object
            // mismatch between our Get(0) diagnostics and SendEvent's real
            // index-aware dispatch as the cause of PA events being rejected.
            // See CoSyncAnimVarSystem::DumpGraphIndexDiag for full rationale.
            CoSyncAnimVarSystem::DumpGraphIndexDiag(actorRef, "PowerArmorEnter");
            // 0.3.4: the delayed DumpGraphVariableNames that followed here is
            // gone. It walked the PA graph's variable hash buckets and faulted
            // inside them (0.3.3 host log 23:52:22, READ at 0x9,
            // DumpGraphVariableNames+0x290 via TickAnimVars) -- a diagnostic
            // that crashes is worse than no diagnostic.
        }
        else
        {
            PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::PowerArmorExit);
            LOG_UPDATED("[TickAnimVars] entity=%u PowerArmorExit", entityID);
        }
        m_prevInPA = m_isInPowerArmor;
    }

    if (m_hasSwimPending)
    {
        m_hasSwimPending = false;
        if (m_pendingSwimming != m_prevSwimming)
        {
            m_prevSwimming = m_pendingSwimming;
            if (m_pendingSwimming)
                PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::SwimStart);
            else
                PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::SwimStop);
            LOG_UPDATED("[TickAnimVars] entity=%u swim state -> %s",
                entityID, m_pendingSwimming ? "SwimStart" : "SwimStop");
        }
    }

    if (m_lastRecvAnimVars.isValid)
    {
        static int s_fullSnapDiag = 0;
        if (++s_fullSnapDiag % 300 == 1)
            LOG_INFO("[TickAnimVars] entity=%u FULL SNAP: speed=%.2f iState=%u sprint=%d jump=%d sneak=%d weap=0x%08X diag#=%d",
                entityID,
                m_lastRecvAnimVars.floats[4],
                m_lastRecvAnimVars.integers[0],
                (m_lastRecvAnimVars.booleans & (1ULL << 1)) ? 1 : 0,
                (m_lastRecvAnimVars.booleans & (1ULL << 7)) ? 1 : 0,
                (m_lastRecvAnimVars.booleans & ((1ULL << 11) | (1ULL << 6))) ? 1 : 0,
                m_lastEquippedWeaponFormID,
                s_fullSnapDiag);

        if (m_isSitting)
        {
            m_lastRecvAnimVars.floats[4]  = 0.0f;
            m_lastRecvAnimVars.floats[10] = 0.0f;
            m_lastRecvAnimVars.floats[11] = 0.0f;
        }

        // Frame-rate-independent smoothing. The old constant 0.25/frame was
        // tuned at 60fps — at 144fps it converged ~2.4x faster (visible
        // stepping between anim states), and below 60fps transitions dragged.
        // Equivalent time-constant form: 0.25 @60fps => rate = -ln(0.75)*60
        // ≈ 17.3/s, so behavior at 60fps is unchanged.
        const double animNow = NowSeconds();
        float animDt = (m_lastAnimTickTime > 0.0)
            ? float(animNow - m_lastAnimTickTime)
            : (1.0f / 60.0f);
        m_lastAnimTickTime = animNow;
        if (animDt < 0.001f) animDt = 0.001f;
        if (animDt > 0.100f) animDt = 0.100f;
        const float kAlpha = 1.0f - expf(-17.3f * animDt);

        m_smoothSpeed     += (m_lastRecvAnimVars.floats[4]  - m_smoothSpeed)     * kAlpha;
        m_smoothSpeedSamp += (m_lastRecvAnimVars.floats[10] - m_smoothSpeedSamp) * kAlpha;
        m_smoothSpeedSmth += (m_lastRecvAnimVars.floats[11] - m_smoothSpeedSmth) * kAlpha;

        auto ShortArcLerp = [](float& smooth, float target, float alpha)
        {
            float diff = target - smooth;
            while (diff >  3.14159f) diff -= 6.28318f;
            while (diff < -3.14159f) diff += 6.28318f;
            smooth += diff * alpha;
        };

        ShortArcLerp(m_smoothDirection, m_lastRecvAnimVars.floats[0],  kAlpha);

        // ── Pitch guard (list item #9) ──────────────────────────────────────
        // Symptom: on weapon equip the proxy snaps to looking STRAIGHT UP and
        // then eases back down over ~a second. Both halves are explained by a
        // single bad sample: an extreme value enters m_smoothPitch, and
        // ShortArcLerp's slow kAlpha is what produces the lazy recovery.
        // Equip is exactly when the proxy's graph is torn down and rebuilt
        // (EquipItemDirect / RefreshActor3D), so a default/garbage Pitch var
        // read during that window is the likely source.
        //
        // This is a GUARD, not a root-cause fix: it makes an implausible pitch
        // unable to reach the skeleton, and removes the slow tail by treating a
        // large jump as a discontinuity to snap through rather than glide.
        // Finding the true source needs a live trace of floats[6] across an
        // equip -- logged below when it trips, so that trace comes for free.
        {
            // A head/spine look angle beyond ~70 degrees is not something a
            // real player produces; anything past this is corrupt.
            constexpr float kMaxPitch = 1.2f;
            float rawPitch = m_lastRecvAnimVars.floats[6];

            // 2026-09-26: the 0.2.4 test logged this ~25x/s (10,469 lines in
            // 7 min on the client) with values that sit steadily at -5..-21
            // for minutes -- not a one-off rebuild glitch. Those numbers are
            // plausible look angles IN DEGREES, so the radian premise of
            // kMaxPitch is UNCONFIRMED. PitchDiag logs the full raw range
            // (in-range samples too) every 10 s so a look-up/look-down test
            // settles the unit; the warning is rate-limited meanwhile.
            if (std::isfinite(rawPitch))
            {
                m_pitchDiagMin = (std::min)(m_pitchDiagMin, rawPitch);
                m_pitchDiagMax = (std::max)(m_pitchDiagMax, rawPitch);
                ++m_pitchDiagSamples;
            }
            if (animNow - m_pitchDiagStart >= 10.0)
            {
                if (m_pitchDiagSamples > 0)
                    LOG_INFO("[PitchDiag] entity=%u raw Pitch over %.0fs: min=%.3f max=%.3f (%u samples, %u clamped)",
                        entityID, animNow - m_pitchDiagStart, m_pitchDiagMin, m_pitchDiagMax,
                        m_pitchDiagSamples, m_pitchDiagClamped);
                m_pitchDiagStart = animNow;
                m_pitchDiagMin = 1e30f;
                m_pitchDiagMax = -1e30f;
                m_pitchDiagSamples = 0;
                m_pitchDiagClamped = 0;
            }

            if (!std::isfinite(rawPitch) || fabsf(rawPitch) > kMaxPitch)
            {
                if ((m_pitchDiagClamped++ % 600) == 0)
                    LOG_UPDATED_WARN("[TickAnimVars] entity=%u implausible pitch %.3f rad "
                                     "(clamped) — likely a graph rebuild during equip (1 in 600 logged)",
                        entityID, rawPitch);
                rawPitch = (rawPitch > 0.f) ? kMaxPitch : -kMaxPitch;
                if (!std::isfinite(m_lastRecvAnimVars.floats[6]))
                    rawPitch = 0.0f;
            }

            // Snap through a big discontinuity instead of easing, so a bad
            // sample cannot leave a visible second-long recovery.
            constexpr float kPitchSnapThreshold = 0.8f;
            if (fabsf(rawPitch - m_smoothPitch) > kPitchSnapThreshold)
                m_smoothPitch = rawPitch;
            else
                ShortArcLerp(m_smoothPitch, rawPitch, kAlpha);
        }

        ShortArcLerp(m_smoothHeading, m_lastRecvAnimVars.floats[26], kAlpha);

        AnimVarSnapshot smoothed = m_lastRecvAnimVars;
        smoothed.floats[0]  = m_smoothDirection;
        smoothed.floats[4]  = m_smoothSpeed;
        smoothed.floats[8]  = m_smoothDirection;
        smoothed.floats[10] = m_smoothSpeedSamp;
        smoothed.floats[11] = m_smoothSpeedSmth;
        smoothed.floats[16] = m_smoothDirection * 57.2957795f;
        smoothed.floats[30] = m_smoothDirection;

        // Round 138: pitch sign INVERTED on apply — live report: proxy head
        // looked down when the real player looked up and vice versa. The
        // sender's captured Pitch var and the proxy graph's consumption of it
        // use opposite sign conventions.
        const float pipBoyPitch = m_lastPipboyOpen ? -0.65f : -m_smoothPitch;
        smoothed.floats[6]  = pipBoyPitch;
        smoothed.floats[27] = pipBoyPitch;
        smoothed.floats[26] = m_smoothHeading;

        // NPC world-actor clones don't have native AI driving their behavior
        // graph through state transitions — raw-writing the host NPC's iState
        // override (e.g. 7 = alert idle, reached on the host via AI events)
        // causes the proxy graph to land in an intermediate/undefined state =
        // A-pose. Force iState=0 when idle so the graph stays in standard idle.
        if (createType == CoSyncEntityType::NPC && m_smoothSpeed < 1.0f)
            smoothed.integers[0] = 0;

        // PA's active behavior graph has its own variable index table, so the
        // raw varSet.data[TP::idx] writes LoadAnimationVariables does for the
        // human graph would corrupt it. LoadPowerArmorLocomotion writes the
        // same locomotion data exclusively through the by-name engine setter
        // instead, which re-resolves the index per-graph on every call.
        if (isCreatureSnapshot)
        {
            // Already applied via LoadCreatureVars above. Never run the
            // humanoid raw indexed writer against a creature's smaller
            // variable table -- see round-28 history in
            // CoSyncCreatureAnimSync.cpp (STATUS_ILLEGAL_INSTRUCTION) for
            // why this must stay a hard skip, not an optimization.
            static std::unordered_set<uint32_t> s_creatureRawWriteSkipLogged;
            if (s_creatureRawWriteSkipLogged.insert(entityID).second)
                LOG_INFO("[TickAnimVars] entity=%u creature snapshot -- raw graph writer skipped, event-dispatch only", entityID);
        }
        else if (createType == CoSyncEntityType::Player)
        {
            // 2026-08-26: moved ahead of the m_isInPowerArmor branch below --
            // that check used to run FIRST, which meant a player proxy current-
            // ly in Power Armor never reached bone-rep at all and silently fell
            // back to LoadPowerArmorLocomotion (the old graph-var PA path the
            // entire [0.0.69]-[0.0.76] changelog saga never got working,
            // "graph accepted=0"). The clean swap's "pure bone replication for
            // players" contract was never supposed to have a PA exception --
            // verified this session (skeleton .nif comparison, human vs PA)
            // that PA doesn't rename the core body bones bone-rep needs; PA
            // just parents <Bone>_Armor plate nodes onto the same humanoid
            // bone names, so the plates ride along for free via normal scene-
            // graph parenting once CoSyncPowerArmorSync attaches them. No PA-
            // specific handling needed here at all -- PA players are just
            // players now.
            //
            // 2026-08-21: REVERTED to the unconditional direct call --
            // TickHook.cpp's RemoteActorUpdate_Hook post-orig apply (the
            // FO4_Wrld write-last port from 2026-08-20) was live-tested and
            // confirmed via a dedicated diagnostic to NEVER FIRE for these
            // proxies on this build (1.11.240.0): "RemoteActorUpdate_Hook
            // FIRED" logged ZERO times on either host or client across a
            // full session, despite the vtable[21] PATCH itself succeeding
            // (InstallRemoteActorHook's "hook installed" log DID fire).
            // Install succeeding only proves the vtable write landed, not
            // that the engine ever calls through that slot for a spawned
            // Character/NPC-class proxy -- it demonstrably does for the
            // local PlayerCharacter (InstallTickHook/ActorUpdate_Hook fires
            // every frame, proven by everything else that depends on it),
            // but NOT for this actor class/slot combination. Gating this
            // branch on IsRemoteActorHookInstalled() (install-succeeded)
            // was therefore wrong -- it silently killed the only call path
            // that actually runs, which is exactly the regression the user
            // reported ("still working normally" i.e. no bone application
            // at all). This is the RELIABLE, proven-since-2026-08-18 call
            // site (CoSyncPlayerManager::Tick()'s loop, unconditional once
            // per frame). Real per-bone replication of the REMOTE sender's
            // own skeleton (see CoSyncPlayerAnimation's network functions) --
            // not the local-mirror IsEnabled() path below, which has no
            // network component and would just copy whoever is at THIS
            // keyboard. Falls back to the normal graph-var path until this
            // entity's first BONES packet arrives (e.g. right after spawn).
            //
            // The write-last hook itself is INTENTIONALLY LEFT IN PLACE in
            // TickHook.cpp, not removed -- since it's confirmed to never
            // fire on this build, it cannot double-apply against this call,
            // and finding the REAL per-proxy Update_PerFrame slot (vtable
            // slot 21 is apparently PlayerCharacter-specific, not shared
            // with the Character/NPC class layout) is a real open IDA task
            // for write-last ordering to ever actually land. See
            // project-cosync-bone-replication-fix memory.
            //
            // 2026-08-21: the TrySeedCanonicalFromActor() call that used to
            // sit here is gone along with the canonical joint list itself.
            // It existed to break a decode deadlock that the canonical-INDEX
            // wire format created (a machine whose local player never entered
            // third person could never build a list, and so could never
            // decode any peer's packet). Bone identity is now a name hash
            // carried with each bone, so there is no list to seed and no
            // deadlock to break -- a packet decodes the moment it arrives.
            // 2026-08-22: the hybrid apply (bones for the upper body, the
            // graph-variable path for the legs, when the sender was in first
            // person) was built here and REVERTED at the user's call. It
            // worked, but it animates the legs locally instead of replicating
            // them, and it merges mode 2 with the locomotion system -- which
            // this feature was explicitly scoped to stay out of.
            //
            // Back to the original contract: pure bone replication, and the
            // graph-variable path only as a fallback when there is no pose to
            // apply at all.
            CoSyncPlayerAnimation::SkeletonCapture recvCap{};
            if (CoSyncPlayerAnimation::GetReceivedSkeleton(entityID, recvCap))
                CoSyncPlayerAnimation::ApplySkeleton(actorRef, recvCap);
        }
        else if (m_isInPowerArmor)
            CoSyncAnimVarSystem::LoadPowerArmorLocomotion(actorRef, smoothed);
        else
            CoSyncAnimVarSystem::LoadAnimationVariables(actorRef, smoothed);

        // ── Movement Direction, recomputed on the proxy ─────────────────────
        // The captured "Direction" graph var arrives ~0: a first-person source
        // graph doesn't expose lateral movement, and the networked
        // iSyncDirection enum is never populated by a normally-controlled
        // source player -- so the proxy blended forward walk/run no matter
        // which way it actually moved (reported: strafe-left/right and
        // backpedal never animated, "always walking or running forward").
        // Recompute it from what the proxy already has: its synced world
        // velocity (m_lastVel) vs. its synced facing yaw (rot.z). Direction is
        // body-relative -- 0 = moving where it faces (forward), +/-pi/2 =
        // strafe, +/-pi = backpedal -- and it is the value the locomotion
        // blend tree keys off, so this is what makes the directional clips
        // play. Runs after LoadAnimationVariables so it overrides the dead
        // captured value.
        //
        // 2026-08-26 -- NPC/creature path only. Players are fully bone-replicated
        // (their legs come from the captured pose, whose Direction is set on the
        // SENDER's parked graph via WriteParkedGraphLocomotion), so this
        // receiver-side locomotion driver is the animation system for NON-player
        // proxies. It does not run for players at all.
        if (createType != CoSyncEntityType::Player)
        {
            const float horzSpeed = sqrtf(m_lastVel.x * m_lastVel.x +
                                          m_lastVel.y * m_lastVel.y);
            if (horzSpeed > 8.0f)
            {
                // Bethesda yaw convention: heading measured clockwise from +Y,
                // i.e. atan2(x, y); facing (rot.z) uses the same convention, so
                // their difference is the body-relative movement angle. NOTE:
                // if a live test shows strafe-left/right swapped, negate `rel`;
                // if forward/back are swapped, add pi. Kept isolated for that.
                const float kPi = 3.14159265358979f;
                const float velHeading = atan2f(m_lastVel.x, m_lastVel.y);
                const float facing     = actorRef->rot.z;
                float rel = velHeading - facing;
                while (rel >  kPi) rel -= 2.0f * kPi;
                while (rel < -kPi) rel += 2.0f * kPi;

                // Shortest-arc smoothing so interpolation jitter doesn't twitch
                // the blend and the +/-pi seam isn't crossed the long way.
                if (!m_dirValid)
                {
                    m_dirSmoothed = rel;
                    m_dirValid = true;
                }
                else
                {
                    float d = rel - m_dirSmoothed;
                    while (d >  kPi) d -= 2.0f * kPi;
                    while (d < -kPi) d += 2.0f * kPi;
                    m_dirSmoothed += d * 0.35f;
                    while (m_dirSmoothed >  kPi) m_dirSmoothed -= 2.0f * kPi;
                    while (m_dirSmoothed < -kPi) m_dirSmoothed += 2.0f * kPi;
                }

                const int backward = (fabsf(m_dirSmoothed) > kPi * 0.5f) ? 1 : 0;
                CoSyncAnimUtils::SetFloat(actorRef, "Direction",            m_dirSmoothed);
                CoSyncAnimUtils::SetFloat(actorRef, "DirectionSmoothed",    m_dirSmoothed);
                CoSyncAnimUtils::SetFloat(actorRef, "DirectionDamped",      m_dirSmoothed);
                CoSyncAnimUtils::SetFloat(actorRef, "DirectionDegrees",     m_dirSmoothed * 57.2957795f);

                // Real movement Speed. The captured "Speed" var arrives ~0 (dead
                // FP-graph read -- FULL SNAP logged speed=0.00 while the player
                // sprinted), so the blend tree never left near-idle and
                // walk/run/sprint never selected. m_lastVel magnitude IS the
                // proxy's real speed; clamp interpolation catch-up spikes (seen
                // to 1752; real sprint tops ~450-500) and smooth it.
                float spd = (horzSpeed > 600.0f) ? 600.0f : horzSpeed;
                m_speedSmoothed += (spd - m_speedSmoothed) * 0.30f;
                CoSyncAnimUtils::SetFloat(actorRef, "Speed",         m_speedSmoothed);
                CoSyncAnimUtils::SetFloat(actorRef, "SpeedSmoothed", m_speedSmoothed);
                CoSyncAnimUtils::SetFloat(actorRef, "SampledSpeed",  m_speedSmoothed);

                CoSyncAnimUtils::SetInt(actorRef,   "iSyncForwardBackward",   backward);
                CoSyncAnimUtils::SetInt(actorRef,   "iSyncForwardBackward00", backward);

                static uint32_t s_dirLog = 0;
                if ((s_dirLog++ % 60) == 0)
                    LOG_INFO("[TickAnimVars] entity=%u computed Direction=%.2f (%.0fdeg) spd=%.0f velH=%.2f facing=%.2f back=%d",
                        entityID, m_dirSmoothed, m_dirSmoothed * 57.2957795f,
                        horzSpeed, velHeading, facing, backward);
            }
            else
            {
                // Stopped (or nearly): decay the smoothed speed toward 0 so the
                // locomotion tier below can fall back to idle instead of holding
                // a stale run/sprint value.
                m_dirValid = false;
                m_speedSmoothed *= 0.5f;
                if (m_speedSmoothed < 1.0f)
                    m_speedSmoothed = 0.0f;
            }
        }

        {
            // Round 138 (IDA-verified, Fallout4 console IsSneaking handler,
            // sub_1405A1FD0): the engine's own sneak test is
            //   (actor+0x134 & 0x3800) == 0x800
            // and actor+0x134 IS actorState.flags — which the snapshot already
            // carries as actorStateFlags2. This replaces the graph-var bits
            // (11/6) that flickered per-frame in live tests (round-2 logs:
            // ENTER/EXIT pairs 300ms apart) and never fired at all in round 3.
            // Graph bits stay as fallback for snapshots with no state flags.
            // Raw sneak target from the source's real engine actorState
            // ((actor+0x134 & 0x3800)==0x800, IDA-verified). Graph bits are the
            // fallback for snapshots with no state dword.
            bool rawSneak;
            if (m_lastRecvAnimVars.actorStateFlags2 != 0)
                rawSneak = (m_lastRecvAnimVars.actorStateFlags2 & 0x3800u) == 0x800u;
            else
                rawSneak =
                    (m_lastRecvAnimVars.booleans & (1ULL << 11)) != 0 ||
                    (m_lastRecvAnimVars.booleans & (1ULL << 6))  != 0;

            // DEBOUNCE. The host's captured sneak bit was seen ALTERNATING
            // rapidly (recvFlags2 0xFF100906/0xFF100106, 8 flips in 6s -- a
            // first-person capture artifact), which would flicker the proxy in
            // and out of sneak. Only COMMIT a change after the raw value holds
            // for a few consecutive ticks; everything below is driven from the
            // committed state (m_prevSneaking), not the raw one.
            if (rawSneak != m_prevSneaking)
            {
                if (++m_sneakDebounceTicks >= 4)
                {
                    m_sneakDebounceTicks = 0;
                    m_prevSneaking = rawSneak;
                    // Send the COMMAND events (sneakStart/sneakStop) that drive
                    // the graph transition into/out of the sneak sub-machine --
                    // NOT sneakStateEnter/Exit, which are the graph's own
                    // state-entered notifications. A first-person source never
                    // emits these on its dormant 3rd-person graph, so the proxy
                    // must be driven directly. State events sent too as a
                    // fallback for graphs that gate on them.
                    PSC_SendAnimEvent(actorRef, rawSneak ? "sneakStart" : "sneakStop");
                    PSC_SendAnimEvent(actorRef,
                        rawSneak ? CoSyncAnimUtils::Events::SneakEnter
                                 : CoSyncAnimUtils::Events::SneakExit);
                    LOG_INFO("[TickAnimVars] entity=%u sneak %s COMMIT (+sneakStart/Stop)",
                        entityID, rawSneak ? "ENTER" : "EXIT");
                }
            }
            else
            {
                m_sneakDebounceTicks = 0;
            }

            const bool committedSneak = m_prevSneaking;

            // DIRECT actorState write -- Papyrus StartSneaking() is a no-op on an
            // NPC-type proxy (proven: proxyFlags2 never changed). The engine
            // reads sneak straight from actorState.flags, so set the 0x3800
            // field ourselves (read-modify-write preserves every other bit).
            // Written every tick from the committed state so it holds even if
            // the engine rewrites the dword.
            const uint32_t curFlags = actorRef->actorState.flags;
            const uint32_t newFlags = (curFlags & ~0x3800u) | (committedSneak ? 0x800u : 0x000u);
            if (newFlags != curFlags)
                actorRef->actorState.flags = newFlags;

            // Reinforce the sneak graph booleans BY NAME (indexed data[] path
            // goes dead on this runtime).
            CoSyncAnimUtils::SetBool(actorRef, CoSyncAnimUtils::Vars::IsSneaking,  committedSneak);
            CoSyncAnimUtils::SetBool(actorRef, CoSyncAnimUtils::Vars::bIsSneaking, committedSneak);
            CoSyncAnimUtils::SetBool(actorRef, "iIsInSneak",                       committedSneak);
        }

        {
            const uint32_t weapState = m_lastRecvAnimVars.integers[28];
            // 0.3.4: the owner's drawn state is authoritative for this proxy;
            // its own AI may no longer holster/draw against it (see
            // CoSyncProxyWeaponAuthority).
            if (!isCreatureSnapshot)
                CoSyncProxyWeaponAuthority::SetReplicatedDrawn(actorRef, weapState > 0);
            if (weapState != m_prevWeaponState)
            {
                if (m_prevWeaponState == 0 && weapState > 0)
                {
                    if (!CoSyncEquipHook::DrawWeaponEngine(actorRef, true))
                        PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::WeaponDraw);
                    LOG_INFO("[TickAnimVars] entity=%u weaponDraw (state %u->%u)",
                        entityID, m_prevWeaponState, weapState);
                }
                else if (m_prevWeaponState > 0 && weapState == 0)
                {
                    if (!CoSyncEquipHook::DrawWeaponEngine(actorRef, false))
                        PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::WeaponSheathe);
                    LOG_INFO("[TickAnimVars] entity=%u weaponSheathe (state %u->%u)",
                        entityID, m_prevWeaponState, weapState);
                }
                m_prevWeaponState = weapState;
            }

            // 0.3.4: converge on the STATE, not only on edges. If the owner
            // has the weapon out and the engine has this proxy's weapon fully
            // holstered for over a second (anything the authority hook did
            // not cover, a 3D rebuild, a missed edge), draw it again -- once
            // per episode, through the engine's own draw. Skipped while a
            // scheduled draw is pending or the proxy is in Power Armor, whose
            // enter/exit sequence owns the weapon.
            if (weapState > 0 && !isCreatureSnapshot && m_weaponDrawDelay == 0 && !m_isInPowerArmor &&
                CoSyncProxyWeaponAuthority::IsEngineHolstered(actorRef))
            {
                const double tNow = NowSeconds();
                if (m_weapHolsteredSince == 0.0)
                    m_weapHolsteredSince = tNow;
                else if (tNow - m_weapHolsteredSince > 1.0)
                {
                    m_weapHolsteredSince = 0.0;
                    const bool ok = CoSyncEquipHook::DrawWeaponEngine(actorRef, true);
                    LOG_UPDATED("[TickAnimVars] entity=%u owner has the weapon drawn but the proxy's is holstered -- redrawn (%s)",
                        entityID, ok ? "engine" : "failed");
                }
            }
            else
            {
                m_weapHolsteredSince = 0.0;
            }
        }


        // Reload is handled by the AEV| event counters above. It used to be
        // level-sampled here (first off dead snapshot bit 3, then off a
        // packet bool + latch); both failed. Deliberately NOT left in as a
        // "fallback" -- two uncoordinated systems driving one animation is
        // its own bug, and the counters are strictly better informed.


        {
            const bool isBlocking = (m_lastRecvAnimVars.booleans & (1ULL << 23)) != 0;
            if (isBlocking != m_prevBlocking)
            {
                PSC_SendAnimEvent(actorRef,
                    isBlocking ? CoSyncAnimUtils::Events::BlockStart
                               : CoSyncAnimUtils::Events::BlockStop);
                LOG_INFO("[TickAnimVars] entity=%u block %s", entityID,
                    isBlocking ? "START" : "STOP");
                m_prevBlocking = isBlocking;
            }
        }


        {
            const bool isStaggering = (m_lastRecvAnimVars.booleans & (1ULL << 10)) != 0;
            if (isStaggering != m_prevStaggering)
            {
                // Round 139: recovery event is staggerStop — "getUp" does not
                // exist in the human behavior graph (HKX string-table check).
                PSC_SendAnimEvent(actorRef,
                    isStaggering ? CoSyncAnimUtils::Events::StaggerStart
                                 : CoSyncAnimUtils::Events::StaggerStop);
                LOG_INFO("[TickAnimVars] entity=%u stagger %s", entityID,
                    isStaggering ? "START" : "STOP");
                m_prevStaggering = isStaggering;
            }
        }

        const bool isInAir   = (m_lastRecvAnimVars.booleans & (1ULL << 9))  != 0;
        const bool isLanding = (m_lastRecvAnimVars.booleans & (1ULL << 33)) != 0;

        // 2026-08-26 -- locomotion tier + MoveStart/SprintStart events for
        // NON-player proxies only (see the note above). Players are bone-
        // replicated and never run this.
        if (createType != CoSyncEntityType::Player)
        {

            // Locomotion tier from the STABLE computed speed, not the captured
            // iLocomotionSpeedState (integers[15]) -- that var is dead/unstable
            // for a first-person player (its dormant 3rd-person graph doesn't
            // maintain it), and it was flipping 3<->2 every ~250ms, cycling
            // sprintStart/sprintStop = the "mini jump while sprinting". Stepwise
            // hysteresis (separate enter/exit thresholds with a gap) so a speed
            // hovering on a boundary can't chatter. Units are game u/s:
            // walk ~130, run ~300, sprint ~450.
            int currTier = m_prevLocoTier;
            const float sp = m_speedSmoothed;
            if      (currTier <= 0 && sp > 20.f)   currTier = 1;   // idle -> walk
            else if (currTier >= 1 && sp < 12.f)   currTier = 0;   // -> idle
            if      (currTier == 1 && sp > 195.f)  currTier = 2;   // walk -> run
            else if (currTier == 2 && sp < 150.f)  currTier = 1;   // run -> walk
            if      (currTier == 2 && sp > 410.f)  currTier = 3;   // run -> sprint
            else if (currTier == 3 && sp < 345.f)  currTier = 2;   // sprint -> run

            if (!isInAir && !isLanding) {
            if (m_prevLocoTier != currTier)
            {
                if (m_prevLocoTier <= 0 && currTier > 0)
                {

                    // 2026-07-31: logging acceptance here too (not just the
                    // PA-specific events below) to check whether SendEvent's
                    // return value is a trustworthy signal at all -- this exact
                    // moveStart call is already known-working visually for
                    // non-PA proxies, so if IT also comes back accepted=0, the
                    // return value itself can't be trusted as evidence of a PA-
                    // specific problem.
                    bool moveStartAcc = PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::MoveStart);
                    LOG_INFO("[TickAnimVars] entity=%u moveStart fired (tier %d->%d) accepted=%d",
                        entityID, m_prevLocoTier, currTier, moveStartAcc ? 1 : 0);
                    // 2026-07-31: PA reuses the same graph/variables (confirmed
                    // via a live DumpGraphVariableNames dump) but gates its own
                    // locomotion branch behind PA-prefixed events instead of the
                    // generic ones above -- this is the actual missing piece for
                    // PA gliding/not-animating, not different variable indices.
                    if (m_isInPowerArmor)
                    {
                        bool acc = PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::PAMoveStart);
                        LOG_UPDATED("[TickAnimVars] entity=%u PAMoveStart sent, graph accepted=%d", entityID, acc ? 1 : 0);

                        // PA graph-index diagnostic, one-shot per entity (this
                        // moveStart transition can repeat many times per PA
                        // session -- avoid log spam). Compared against the
                        // PowerArmorEnter-time reading above to see whether
                        // SwitchToPowerArmor's async graph swap is still
                        // settling at entry vs. by the time movement starts.
                        static std::unordered_set<uint32_t> s_paMoveStartDiagLogged;
                        if (s_paMoveStartDiagLogged.insert(entityID).second)
                            CoSyncAnimVarSystem::DumpGraphIndexDiag(actorRef, "PAMoveStart");
                    }
                }
                else if (m_prevLocoTier > 0 && currTier <= 0)
                {

                    PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::MoveStop);
                    LOG_INFO("[TickAnimVars] entity=%u moveStop fired (tier %d->%d)",
                        entityID, m_prevLocoTier, currTier);
                }

                if (m_prevLocoTier < 3 && currTier >= 3)
                {
                    bool sprintStartAcc = PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::SprintStart);
                    LOG_UPDATED("[TickAnimVars] entity=%u sprintStart (tier %d->%d) accepted=%d",
                        entityID, m_prevLocoTier, currTier, sprintStartAcc ? 1 : 0);
                    if (m_isInPowerArmor)
                    {
                        bool acc = PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::PASprintStart);
                        LOG_UPDATED("[TickAnimVars] entity=%u PASprintStart sent, graph accepted=%d", entityID, acc ? 1 : 0);
                    }
                }
                else if (m_prevLocoTier >= 3 && currTier < 3)
                {
                    PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::SprintStop);
                    LOG_UPDATED("[TickAnimVars] entity=%u sprintStop (tier %d->%d)",
                        entityID, m_prevLocoTier, currTier);
                    if (m_isInPowerArmor)
                    {
                        bool acc = PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::PASprintStop);
                        LOG_UPDATED("[TickAnimVars] entity=%u PASprintStop sent, graph accepted=%d", entityID, acc ? 1 : 0);
                    }
                }


                // Round 139: the old "LocoStateChange" event fired here (and
                // every 2 ticks below) does NOT exist in any human behavior
                // graph — it was silently dropped by the engine the whole
                // time. Removed; moveStart/Stop + sprintStart/Stop above are
                // the real transitions.
                m_prevLocoTier = currTier;
            }
            }
        }

        {
            if (m_prevJumping != isInAir)
            {
                if (isInAir)
                {
                    PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::JumpStart);
                    LOG_INFO("[TickAnimVars] entity=%u jumpStart fired", entityID);
                    if (m_isInPowerArmor)
                    {
                        bool acc = PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::PAJumpFall);
                        LOG_UPDATED("[TickAnimVars] entity=%u PAJumpFall sent, graph accepted=%d", entityID, acc ? 1 : 0);
                    }
                }
                else
                {

                    // Round 139: "JumpLandHard" doesn't exist in the graph —
                    // jumpLand is the only real landing event.
                    PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::JumpLand);
                    LOG_INFO("[TickAnimVars] entity=%u jumpLand fired (landing=%d)", entityID, isLanding ? 1 : 0);
                    if (m_isInPowerArmor)
                    {
                        bool acc = PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::PAJumpEnd);
                        LOG_UPDATED("[TickAnimVars] entity=%u PAJumpEnd sent, graph accepted=%d", entityID, acc ? 1 : 0);
                    }
                }
                m_prevJumping = isInAir;
            }
        }

        {
            const int currTier = (int)m_lastRecvAnimVars.integers[15];
            if (m_prevLanding && !isLanding)
            {
                if (currTier != m_prevLocoTier)
                {
                    if (m_prevLocoTier <= 0 && currTier > 0)
                        PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::MoveStart);
                    else if (m_prevLocoTier > 0 && currTier <= 0)
                        PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::MoveStop);


                    if (m_prevLocoTier < 3 && currTier >= 3)
                        PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::SprintStart);
                    else if (m_prevLocoTier >= 3 && currTier < 3)
                        PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::SprintStop);

                    m_prevLocoTier = currTier;
                }
                m_locoStateChangeTick = 0;
                LOG_INFO("[TickAnimVars] entity=%u landing complete, re-sync tier=%d",
                    entityID, currTier);
            }
            m_prevLanding = isLanding;
        }

        {
            // Round 138: NO event on bAnimationDriven anymore. Melee power
            // attacks/lunges are animation-driven, and the old code fired a
            // furniture-style "Activate" at the proxy mid-swing — live logs
            // (02:31:28, p2) show "Activate fired" at the exact moment of the
            // host's melee attack, which is what snapped the proxy into an
            // A-pose. Log-only until a real use case needs an event here.
            const bool isAnimDriven = (m_lastRecvAnimVars.booleans & (1ULL << 48)) != 0;
            if (isAnimDriven != m_prevAnimDriven)
                LOG_INFO("[TickAnimVars] entity=%u bAnimDriven -> %d (no event sent)",
                    entityID, isAnimDriven ? 1 : 0);
            m_prevAnimDriven = isAnimDriven;
        }

        {
            // 2026-08-04: THROW. Bit 21 of the snapshot bitfield is
            // kBooleanLookupTable[21] == TP::kbIsThrowing (graph variable
            // index 208) — already captured and shipped for players AND
            // host-owned NPCs since the anim-var system was written; nothing
            // here ever read it, which is why a peer's grenade appeared as a
            // bare explosion with no wind-up.
            //
            // grenadeThrowStart / mineThrowStart / throwEnd were validated
            // against the string tables of all 44 human behavior graphs the
            // same way the round-139 set was — see the note on
            // CoSyncAnimUtils::Events. The mine/grenade split is data-driven
            // off the thrown weapon's PROJ record (explosionProximity), so
            // DLC and modded throwables classify without a hardcoded list.
            // Throw is handled by the AEV| event counters above, for the same
            // reason as reload and fire. The sender now also decides grenade
            // vs mine, since it is the only side that can see which throwable
            // is equipped.
        }

        ReplayDiscreteEvents();

        {
            // Round 138 (IDA-verified, GetAttackState fallback in
            // sub_140CD1770): the engine's raw attack state is
            //   (actor+0x130 >> 25) & 7   ==  (actorStateFlags1 >> 25) & 7.
            // CK enum: 0=none 1=draw 2=swing/fire 3=hit 4=next 5=follow 6=bash.
            // The old graph-var meleeState (integers[16]) never fired once in
            // four live logs — this replaces it for BOTH melee and ranged.
            const uint32_t atkState = (m_lastRecvAnimVars.actorStateFlags1 >> 25) & 0x7u;
            if (atkState != m_prevMeleeState)
            {
                const bool wasIdle = (m_prevMeleeState == 0u || m_prevMeleeState >= 4u);
                if (wasIdle && (atkState == 1u || atkState == 2u || atkState == 6u))
                {
                    // Round 139 (behavior-graph string tables): the graph's
                    // real melee event is "attackStartMelee"; fists count as
                    // melee too (no H2H-specific start exists). Gun bash
                    // (atkState 6) has its own event. Ranged fire keeps
                    // "attackStart". Melee vs ranged: a WEAP with no ammo
                    // form is a melee weapon.
                    bool isMeleeWeapon = true;
                    if (m_lastEquippedWeaponFormID != 0)
                    {
                        TESForm* wf = LookupFormByID(m_lastEquippedWeaponFormID);
                        if (wf && wf->formType == kFormType_WEAP)
                            isMeleeWeapon = (static_cast<TESObjectWEAP*>(wf)->weapData.ammo == nullptr);
                    }

                    // 2026-08-04: atkState is the MELEE/BASH state machine —
                    // it is NOT a fire signal. Live log proof: the only
                    // transitions ever seen were 0->1 and 3->0, 8 of each, and
                    // they were the player BASHING; actually firing the gun
                    // never touched atkState at all (and atkState 6 never
                    // appeared, so the old ==6 bash test was dead code).
                    //
                    // So with a RANGED weapon equipped, any atkState activity
                    // is a gun bash — never a shot. Sending attackStart here
                    // is what made proxies play a shooting animation when the
                    // real player pressed melee. Ranged fire is owned entirely
                    // by the isFiring block above.
                    const char* atkEvent = isMeleeWeapon
                        ? CoSyncAnimUtils::Events::AttackStartMelee
                        : CoSyncAnimUtils::Events::AttackBashStart;

                    PSC_SendAnimEvent(actorRef, atkEvent);
                    LOG_UPDATED("[TickAnimVars] entity=%u %s (atkState %u->%u, weapon=0x%08X melee=%d)",
                        entityID, atkEvent, m_prevMeleeState, atkState,
                        m_lastEquippedWeaponFormID, isMeleeWeapon ? 1 : 0);
                }
                else if (atkState == 0u && m_prevMeleeState != 0u)
                {
                    PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::AttackStop);
                    LOG_UPDATED("[TickAnimVars] entity=%u attackStop (atkState %u->0)",
                        entityID, m_prevMeleeState);
                }
                m_prevMeleeState = atkState;
            }
        }

        {
            // Round 139: "weaponDrawn"/"weaponHolstered" do not exist in the
            // human behavior graph (HKX string-table check) — those sends
            // were silent no-ops. The real transitions are weaponDraw/
            // weaponSheathe, already fired from the weapState block above.
            const bool gunDown = (m_lastRecvAnimVars.integers[30] != 0);
            if (gunDown != m_prevGunDown)
                LOG_INFO("[TickAnimVars] entity=%u gunDown -> %d (no event; weapState block owns draw/sheathe)",
                    entityID, gunDown ? 1 : 0);
            m_prevGunDown = gunDown;
        }

        {
            if (m_isSitting != m_prevSitting)
            {
                if (!m_isSitting)
                {
                    PSC_SendAnimEvent(actorRef, "sitStop");
                    PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::FurnitureExit);
                    LOG_UPDATED("[TickAnimVars] entity=%u sit STOP (sitStop + furnitureExit)", entityID);
                }
                else
                {
                    // Drive the sit directly instead of suppressing: a
                    // first-person source never emits furnitureEnter on its
                    // dormant 3rd-person graph, and m_isSitting is the synced
                    // truth. No furniture REF on the proxy, so this is a
                    // free-standing sit pose (the graph's furniture/sit idle),
                    // not snapped to a specific chair -- exact-chair alignment
                    // needs the furniture REF formID synced (GetFurnitureReference
                    // exists) as a follow-up.
                    PSC_SendAnimEvent(actorRef, "sitStartFromStand");
                    PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::FurnitureEnter);
                    PSC_SendAnimEvent(actorRef, "g_IdleSitting");
                    LOG_UPDATED("[TickAnimVars] entity=%u sit START (sitStartFromStand + furnitureEnter + g_IdleSitting)", entityID);
                }
                m_prevSitting = m_isSitting;
            }
        }

        return;
    }

    static int s_fallbackDiag = 0;
    if (++s_fallbackDiag % 300 == 1)
        LOG_WARN("[TickAnimVars] entity=%u FALLBACK (no recv snapshot) diag#=%d",
            entityID, s_fallbackDiag);

    const float horzSpeed = sqrtf(
        m_lastVel.x * m_lastVel.x +
        m_lastVel.y * m_lastVel.y);
    const float speed = (horzSpeed > 400.0f) ? 400.0f : horzSpeed;
    const float pitch = m_lastPitch;

    static constexpr int kHysteresis = 4;
    const bool wantsMove = (speed > 5.0f);
    if (wantsMove != m_locoActive)
    {
        m_locoHysteresis++;
        if (m_locoHysteresis >= kHysteresis)
        {
            m_locoActive     = wantsMove;
            m_locoHysteresis = 0;
        }
    }
    else
    {
        m_locoHysteresis = 0;
    }

    AnimVarSnapshot snap;
    if (!CoSyncAnimVarSystem::SaveAnimationVariables(actorRef, snap))
        return;

    snap.floats[4]  = speed;
    snap.floats[11] = speed;
    snap.floats[0]  = 0.0f;
    snap.floats[6]  = pitch;
    snap.floats[27] = pitch;
    snap.integers[0] = m_locoActive ? 190u : 0u;

    if (CoSyncPlayerAnimation::IsEnabled())
        CoSyncPlayerAnimation::ApplySkeleton(actorRef);
    else
        CoSyncAnimVarSystem::LoadAnimationVariables(actorRef, snap);


    {
        const int currTier = m_locoActive ? 1 : 0;
        if (m_prevLocoTier != currTier)
        {
            if (m_prevLocoTier <= 0 && currTier > 0)
                PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::MoveStart);
            else if (m_prevLocoTier > 0 && currTier <= 0)
                PSC_SendAnimEvent(actorRef, CoSyncAnimUtils::Events::MoveStop);
            m_prevLocoTier = currTier;
        }
    }
}

std::string CoSyncPlayer::GetAnimationState() const
{
    if (!m_locoActive)
        return "idle";

    const float speed = sqrtf(
        m_lastVel.x * m_lastVel.x +
        m_lastVel.y * m_lastVel.y);

    if (speed > 150.0f) return "sprinting";
    if (speed > 300.0f) return "running";
    return "walking";
}
