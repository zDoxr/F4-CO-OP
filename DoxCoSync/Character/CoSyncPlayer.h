#pragma once

#include <string>
#include <cstdint>
#include <deque>
#include <atomic>

#include "NiTypes.h"
#include "GameReferences.h"
#include "LocalPlayerState.h"
#include "Packets_EntityUpdate.h"
#include "CoSyncEntityTypes.h"
#include "CoSyncAnimVarSystem.h"
#include "CoSyncRequirementsLog.h"

class CoSyncPlayer
{
public:
    explicit CoSyncPlayer(const std::string& name);

    bool SpawnInWorld(TESObjectREFR* anchor, TESNPC* npcBase);

    void ApplyUpdate(const EntityUpdatePacket& u);


    void ApplyPendingTransformIfAny();


    void TickSmoothing(double now);

    // Routes an interpolated transform to the engine: native SetPosition steer
    // for AI-alive NPC puppets (createType==NPC && PuppetModeEnabled), else the
    // legacy raw-NiNode PositionRemoteActor path (players, or puppet mode off).
    void ApplyProxyTransform(const NiPoint3& pos, const NiPoint3& rot);


    void TickAnimVars();
    // AEV| replay (fire/reload/throw). Players via TickAnimVars; NPC puppets directly.
    void ReplayDiscreteEvents();

    std::string GetAnimationState() const;

public:

    uint32_t    entityID = 0;
    uint32_t    ownerEntityID = 0;
    std::string username;

    CoSyncEntityType createType = CoSyncEntityType::Player;
    bool isRemoteControlled = false;

    bool isLocalPlayer = false;

    bool IsOwner(uint32_t localEntityID) const
    {
        LOG_REQUIREMENT("CoSyncPlayer::IsOwner", "Just compares against this proxy's already-set owner ID.");
        return ownerEntityID != 0 && ownerEntityID == localEntityID;
    }

    Actor* actorRef = nullptr;
    bool   hasSpawned = false;
    double m_spawnTime = 0.0;

    // 2026-07-21: user reported NPCs stuck A-posed in dense areas (30-40+
    // proxies spawning at once), suspecting the discovery/spawn radius was
    // "too slow" to keep up. Traced instead to CoSyncAnimUtils::IsGraphReady
    // checking Is3DLoaded() (GetObjectRootNode() != nullptr) FIRST, with no
    // retry -- if an actor's 3D never finishes streaming in (exactly what a
    // burst of many simultaneous spawns can cause), TickAnimVars returns
    // immediately every call, forever, and the proxy never gets animated.
    // These two fields let TickAnimVars retry once (a fresh RefreshActor3D
    // kick, the same "redo the sequence" fix already proven for the ext/int
    // proxy-hide case) if 3D is still not loaded after a grace window,
    // instead of giving up on the actor for the rest of the session.
    double m_3dNotLoadedSince = 0.0;
    bool   m_3dLoadRetried    = false;
    // 2026-07-31: entering PA swaps the actor's whole behavior graph, a much
    // bigger transition than a fresh spawn's one-time 3D load -- the single
    // retry above (tuned for that smaller case) can leave a PA proxy stuck
    // "not ready" (and therefore completely unsynced/un-animated) for the
    // rest of the session if the graph swap is slow to settle. Tracks the
    // last periodic retry specifically for the PA case, which keeps retrying
    // instead of giving up after one attempt.
    double m_lastPaGraphRetryTime = 0.0;

    // 2026-07-21: set every time TickAnimVars gets past the IsGraphReady
    // gate (i.e. actually processes/applies animation state), never reset
    // to 0 after the first success. A world-actor entity with hasSpawned
    // true but this still 0.0 after several seconds has NEVER once gotten
    // past IsGraphReady -- the exact "stuck" case [0.0.56]'s retry targets.
    // Lets a periodic diagnostic report per-entity health (spawned /
    // graph-ready / actively-animating) in one place instead of manually
    // cross-referencing RefreshActor3D and AnimVarSystem log lines by hand,
    // which is how every round of this investigation so far had to work.
    double m_lastAnimApplyTime = 0.0;

    NiPoint3 authoritativePos{ 0.f, 0.f, 0.f };
    NiPoint3 authoritativeRot{ 0.f, 0.f, 0.f };

    // 2026-07-21: user reported exterior NPC clones teleporting to the door
    // when a player transitions ext->int. Root cause: SpawnWatchdog's respawn
    // range-gate compares authoritativePos (set ONCE at CREATE time, never
    // refreshed) against the local player's CURRENT raw position with no
    // worldspace check -- the same missing-worldspace-context bug already
    // found and fixed three times this session elsewhere (NPC departure,
    // proxy cell-visibility). Interior/exterior coordinates are numerically
    // incomparable; when the engine frees a clone across a cell transition
    // and the watchdog's distance check spuriously "passes" by coincidence,
    // it re-enqueues a spawn that lands at the player's OWN current position
    // (CreateReferenceAtLocation always spawns at the local player to
    // guarantee a loaded cell) -- i.e. right at the door. This captures the
    // local player's worldspace formID (0 = interior, matching the existing
    // lastRemoteWorldspaceID convention) at the same moment authoritativePos
    // is set, so SpawnWatchdog can refuse to respawn across a context change
    // regardless of what the raw distance number says.
    uint32_t authoritativeWorldspaceID = 0xFFFFFFFFu; // sentinel: not yet known

    // True between "spawned disabled at the local player" and the peer's first
    // real position arriving. See SpawnInWorld and RevealOnFirstTransform.
    bool     m_awaitingFirstTransform = false;

    // Re-equip countdown after the reveal Enable(). Your notes (round 143,
    // m_postRefreshWeaponDelay) record that Enable() re-attaches ARMOR via the
    // biped system but NOT individually-equipped weapons -- so a proxy
    // revealed from disabled comes back unarmed unless the equip is re-fired.
    int      m_revealReEquipDelay = 0;

    // Called once the first authoritative transform lands: positions the proxy
    // where it actually belongs, then Enables it. Safe to call repeatedly --
    // no-ops unless m_awaitingFirstTransform is set.
    void RevealOnFirstTransform();

    // Map marker + overlay card for a live, positioned proxy. Called from
    // SpawnInWorld when the peer's position was already known, and from
    // RevealOnFirstTransform when it wasn't -- so visuals land on EITHER path.
    // Idempotent via m_visualsRegistered.
    void RegisterProxyVisuals();
    bool m_visualsRegistered = false;

    bool     hasPendingTransform = false;
    NiPoint3 pendingPos{ 0.f, 0.f, 0.f };
    NiPoint3 pendingRot{ 0.f, 0.f, 0.f };
    NiPoint3 pendingVel{ 0.f, 0.f, 0.f };

    struct NetTransform
    {
        NiPoint3 pos{ 0.f, 0.f, 0.f };
        NiPoint3 rot{ 0.f, 0.f, 0.f };
        float    scale = 1.0f;
        double   hostTime    = 0.0;
        double   localTime   = 0.0;
        NiPoint3 vel{ 0.f, 0.f, 0.f };
    };

    std::deque<NetTransform> m_tfBuffer;

    double m_lastHostTime = 0.0;
    double m_lastRecvLocalTime = 0.0;
    bool   m_hasAny = false;

    // 0.3.3: sender-clock -> local-clock mapping for the interpolation
    // timeline (see ApplyUpdate). offset = (local arrival - sender stamp);
    // the smallest recent sample is the one with the least network delay.
    static constexpr int kClockSamples = 32;
    double m_clockSamples[kClockSamples] = {};
    int    m_clockSampleCount = 0;
    int    m_clockSampleNext  = 0;
    double m_clockOffset      = 0.0;
    bool   m_clockOffsetValid = false;

    // True only once a GENUINE peer transform has arrived. m_hasAny is ALSO
    // set by ApplyPendingTransformIfAny's seed (the proxy's own PlaceAtMe
    // position, i.e. right on top of the local player), so it cannot be used
    // to decide whether we know where the peer really is.
    bool   m_hasRealTransform = false;

    double lastPacketTime = 0.0;
    LocalPlayerState lastState{};

    NiPoint3  m_lastVel{ 0.f, 0.f, 0.f };
    float     m_lastPitch = 0.0f;
    // Body-relative movement Direction (radians) computed on the proxy from
    // its own synced velocity vs. facing, smoothed frame-to-frame. Drives the
    // strafe/backward locomotion blend the captured graph var never carried.
    float     m_dirSmoothed = 0.0f;
    bool      m_dirValid    = false;
    // Body movement speed computed on the proxy from its synced velocity,
    // smoothed. The captured "Speed" graph var arrives ~0 (dead FP read), so
    // this drives the walk/run/sprint blend selection instead.
    float     m_speedSmoothed = 0.0f;

    int       m_locoHysteresis = 0;
    bool      m_locoActive     = false;

    int       m_prevLocoTier   = -1;
    bool      m_prevJumping    = false;
    bool      m_prevLanding    = false;
    int       m_weaponDrawDelay = 0;
    bool      m_headTrackSent  = false;

    bool     m_prevSneaking    = false;
    int      m_sneakDebounceTicks = 0;
    // Ticks to wait after issuing a StartSneaking() toggle before issuing
    // another. StartSneaking runs async (CallFunctionNoWait) so its result
    // does not appear in actorState.flags for a frame or two; without a
    // cooldown the per-tick reconcile would double-toggle straight past the
    // target while the first call is still in flight.
    int      m_sneakToggleCooldown = 0;
    bool     m_prevReloading   = false;
    bool     m_prevBlocking    = false;
    bool     m_prevStaggering  = false;
    bool     m_prevAnimDriven  = false;
    bool     m_prevFiring      = false;
    bool     m_prevSprinting      = false;
    int      m_sprintFrames       = 0;
    int      m_notSprintFrames    = 0;
    bool     m_prevSwimming    = false;
    bool     m_pendingSwimming = false;
    bool     m_hasSwimPending  = false;

    bool     m_prevSitting     = false;
    bool     m_prevInPA        = false;
    // 2026-07-31 diagnostic: ticks remaining until DumpGraphVariableNames
    // fires once after a PA enter, giving SwitchToPowerArmor's async VM-queued
    // graph swap time to settle before we dump (0 = not pending).
    int      m_paVarDumpDelay  = 0;
    bool     m_prevGunDown     = true;
    uint32_t m_prevMeleeState  = 0;
    uint32_t m_prevWeaponState = 0;
    // NPC world-actor puppets only: mirrors m_prevWeaponState's edge-detect
    // pattern but keyed off actorStateFlags2's real weapon-state bits (the
    // host captures these from the actual AI-driven NPC), since NPC snapshots
    // don't populate integers[28] the way player snapshots do.
    bool     m_prevNpcWeaponDrawn = false;
    int      m_locoStateChangeTick = 0;

    AnimVarSnapshot m_lastRecvAnimVars;

    float m_smoothSpeed     = 0.0f;
    float m_smoothSpeedSamp = 0.0f;
    float m_smoothSpeedSmth = 0.0f;
    float m_smoothDirection = 0.0f;

    double m_lastAnimTickTime = 0.0;

    float m_smoothPitch   = 0.0f;
    // PitchDiag (see TickAnimVars): raw Pitch range per 10 s window.
    double   m_pitchDiagStart   = 0.0;
    float    m_pitchDiagMin     = 1e30f;
    float    m_pitchDiagMax     = -1e30f;
    uint32_t m_pitchDiagSamples = 0;
    uint32_t m_pitchDiagClamped = 0;
    float m_smoothHeading = 0.0f;

    NiPoint3 m_prevRecvPos{ 0.f, 0.f, 0.f };
    bool     m_prevRecvHasPos = false;

    bool     m_isSitting = false;

    uint32_t m_lastEquippedWeaponFormID = 0;

    // Last throwable this entity was reported holding. Latched rather than
    // read live at the moment of the throw: the engine unequips the grenade
    // the instant it leaves the hand, so by the time bIsThrowing falls the
    // field is already back to 0 and there would be nothing left to classify.
    uint32_t m_lastThrowableFormID = 0;
    bool     m_prevThrowing        = false;

    // (superseded by the AEV| event counters below)
    // ── Discrete animation events (AEV| messages) ───────────────────────────
    // COUNTERS, not booleans, and edge-detected by the SENDER at frame rate.
    //
    // 2026-08-04: these replace a level-sampled bool + "latch" scheme that
    // failed three different ways in one live test, for two reasons worth
    // remembering:
    //
    //   1. The latch re-armed itself. It was set on EVERY packet reporting
    //      true but only cleared on the rising edge, so after the first shot
    //      it stayed stuck true, the falling edge never arrived, and no
    //      further rising edge was ever possible. Symptom: reload fired
    //      exactly ONCE per session with no reloadEnd.
    //   2. isFiring is true for roughly a SINGLE FRAME per shot. Sampling a
    //      level at packet rate (and the local-player send rate drops to a
    //      third while standing still) misses it almost every time. Symptom:
    //      no shot animation at all.
    //
    // A counter also gives correct rapid fire for free: N rising edges on the
    // sender become N queued shots here, instead of one held-true level.
    // Incremented off the network thread, drained on the game thread in
    // TickAnimVars -- atomics rather than a mutex, same as m_pendingPipboyOpen.
    std::atomic<int> m_evFire        { 0 };
    std::atomic<int> m_evReloadStart { 0 };
    std::atomic<int> m_evReloadEnd   { 0 };
    std::atomic<int> m_evThrowGrenade{ 0 };
    std::atomic<int> m_evThrowMine   { 0 };
    std::atomic<int> m_evThrowEnd    { 0 };

    bool     m_pendingWeaponChange = false;
    uint32_t m_pendingWeaponFormID = 0;

    // Two-phase weapon equip: AddItem fires immediately on weapon change,
    // EquipItem only after this frame countdown so the async AddItem has
    // landed in the proxy's inventory (same-tick EquipItem silently no-ops
    // on an item that isn't there yet).
    uint32_t m_weaponEquipFormID  = 0;
    int      m_weaponEquipDelay   = 0;
    bool     m_weaponEquipRetried = false;

    // After RefreshActor3D (Disable+Enable), Enable() re-attaches armor via
    // the biped system but does NOT re-attach individually-equipped weapons.
    // Set by ProcessWeaponVerifyQueue when it fires RefreshActor3D; counts down
    // in Tick; at 0, re-calls EquipItemDirect + arms m_weaponDrawDelay=8.
    int      m_postRefreshWeaponDelay  = 0;
    uint32_t m_postRefreshWeaponFormID = 0;

    uint32_t m_lastQueuedWeaponFormID = 0;

    // Round 143: weapon equip has NO periodic retry unlike outfit sync (which
    // force-resends every ~10s regardless of change) — the network-side
    // trigger below is purely edge-detected on m_lastQueuedWeaponFormID
    // changing, so a silent AddItem/EquipItem failure (confirmed live via
    // WEAPON VERIFY present=0) was permanent until the real player switched
    // weapons again. m_weaponConfirmedEquipped is set true ONLY by
    // CoSyncPlayerManager::ProcessWeaponVerifyQueue on a successful
    // present+equipped readback; ApplyUpdate uses it to periodically
    // re-trigger the equip even when the wire value hasn't changed.
    bool     m_weaponConfirmedEquipped = false;
    double   m_lastWeaponRetryTime     = 0.0;


    bool     m_lastCombatActive     = false;
    bool     m_pendingCombatActive  = false;
    uint32_t m_pendingCombatTarget  = 0;
    uint32_t m_lastCombatTarget     = 0;


    bool m_lastInDialog    = false;
    bool m_pendingInDialog = false;

    bool                m_lastPipboyOpen    = false;
    std::atomic<bool>   m_pendingPipboyOpen { false };

    // InitNpcProxy deferred init: SpawnTask sets this to 120 instead of
    // calling InitNpcProxy directly. The Papyrus VM calls (SetEssential,
    // SetProtected, IdleForceDefaultState) must NOT fire during SpawnTask::Run
    // because the actor's Papyrus VM script object isn't ready yet — immediate
    // Papyrus dispatch causes an EXEC AV that the VEH "fixes" by simulating a
    // return to the F4SE task-dispatch site, silently dropping the NEXT queued
    // task (typically the host player proxy or an NPC proxy). CoSyncPlayerManager
    // ticks this down and calls InitNpcProxy when it reaches 0.
    int      m_pendingNpcInitDelay = 0;

    // NPC proxies: IdleForceDefaultState must be sent AFTER the behavior graph
    // is ready. InitNpcProxy queues it via Papyrus at 120 ticks (~0.9s) but
    // the graph isn't ready until ~3.5s — the Papyrus event fires into an
    // unready graph and is silently dropped. TickAnimVars detects the first
    // successful IsGraphReady() pass and fires the event natively exactly once.
    // Reset to false when a 3D-load retry fires so the event re-sends after the
    // new Enable cycle settles.
    bool     m_idleEventSent = false;
    int      m_pendingFactionDelay = 0;

    bool     m_isInPowerArmor      = false;
    uint32_t m_paChassisRefFormID  = 0;
    bool     m_paBehaviorEntered   = false;
    // Ref of the PA chassis we PlaceAtMe-spawned for this proxy (0 = none).
    // The receiving client almost never has the host's chassis, so we spawn a
    // fresh frame at the proxy and delete it again on PA exit.
    uint32_t m_paSpawnedChassisRefFormID = 0;

    // 2026-08-26: this client's OWN local ref for the REAL chassis the sender
    // entered/exited (0 = not found on this client, e.g. too far away to be
    // loaded) -- separate from m_paSpawnedChassisRefFormID above, which is
    // the fake clone used only for the proxy attach. Found by scanning the
    // local cell for a ref of the sent chassisBaseFormID near the sent
    // position (see CoSyncPowerArmorSync.cpp), since the real frame's own
    // formID is almost never directly resolvable cross-machine. Disabled on
    // enter (so it disappears for every observer, matching what naturally
    // happens on the wearer's own screen) and re-enabled + repositioned to
    // the exit location on exit.
    uint32_t m_paRealFrameRefFormID = 0;

    // Lobby-bar UI mirror of this player's last network-reported vitals.
    // Written on the game thread (HealthSync flush), read by the overlay.
    float    m_uiHealth    = 0.f;
    float    m_uiMaxHealth = 0.f;
    uint32_t m_uiLevel     = 0;
    float    m_uiRads      = 0.f;   // current radiation, drives red health-bar chunk

    // Per-entity worldspace tracking for proxy cell-visibility.
    // lastRemoteWorldspaceID = 0 (interior), non-zero (exterior worldspace formID),
    // 0xFFFFFFFF (no WS packet received yet — never hide based on this).
    uint32_t lastRemoteWorldspaceID = 0xFFFFFFFF;
    uint32_t lastRemoteCellFormID   = 0;
    bool     m_proxyHiddenForCell   = false;

    // Networked bleedout/revive (CoSyncReviveSync). Presentation-only for a
    // remote proxy -- their real health/life-state lives on whichever machine
    // owns the actor; this just mirrors "should this proxy look/behave down"
    // for everyone else.
    bool     isDowned = false;
    double   m_downedSince = 0.0;
    uint32_t m_reviverEntityID = 0;   // 0 = nobody currently reviving them
};
