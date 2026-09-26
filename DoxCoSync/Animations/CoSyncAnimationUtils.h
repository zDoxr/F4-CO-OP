#pragma once

class Actor;

namespace CoSyncAnimUtils
{

    namespace Vars
    {

        static constexpr const char* Speed                  = "Speed";
        static constexpr const char* SpeedSmoothed          = "SpeedSmoothed";
        static constexpr const char* SampledSpeed           = "SampledSpeed";
        static constexpr const char* SpeedDamped            = "speedDamped";


        static constexpr const char* Direction              = "Direction";
        static constexpr const char* DirectionSmoothed      = "DirectionSmoothed";
        static constexpr const char* DirectionDamped        = "DirectionDamped";
        static constexpr const char* DirectionDegrees       = "DirectionDegrees";


        static constexpr const char* iState                 = "iState";
        static constexpr const char* iSyncIdleLocomotion    = "iSyncIdleLocomotion";
        static constexpr const char* iSyncDirection         = "iSyncDirection";
        static constexpr const char* iSyncForwardBackward   = "iSyncForwardBackward";
        static constexpr const char* iSyncForwardBackward00 = "iSyncForwardBackward00";
        static constexpr const char* iSyncWalkRun           = "iSyncWalkRun";
        static constexpr const char* iSyncSprintState       = "iSyncSprintState";
        static constexpr const char* iSyncSneakWalkRun      = "iSyncSneakWalkRun";
        static constexpr const char* iSyncTurnState         = "iSyncTurnState";
        static constexpr const char* iSyncReadyAlertRelaxed = "iSyncReadyAlertRelaxed";
        static constexpr const char* iSyncSightedState      = "iSyncSightedState";
        static constexpr const char* iSyncGunDown           = "iSyncGunDown";
        // Confirmed present in RaiderRootBehavior.hkx's string table. The
        // engine's own per-shot fire state for networked actors — used to
        // detect subsequent rounds while isFiring stays high through a burst.
        static constexpr const char* iSyncFireState         = "iSyncFireState";
        static constexpr const char* iSyncLocomotionSpeed   = "iSyncLocomotionSpeed";
        static constexpr const char* iLocomotionSpeedState  = "iLocomotionSpeedState";
        static constexpr const char* iTalkGenerator         = "iTalkGenerator";
        static constexpr const char* staggerDirection       = "staggerDirection";
        static constexpr const char* iIsPlayer              = "iIsPlayer";


        static constexpr const char* IsNPC                  = "IsNPC";
        static constexpr const char* IsPlayer               = "IsPlayer";


        static constexpr const char* bAnimationDriven          = "bAnimationDriven";
        static constexpr const char* bAllowRotation             = "bAllowRotation";
        static constexpr const char* bGraphDriven               = "bGraphDriven";
        static constexpr const char* bGraphDrivenTranslation    = "bGraphDrivenTranslation";
        static constexpr const char* bGraphDrivenRotation       = "bGraphDrivenRotation";
        static constexpr const char* bFreezeSpeedUpdate         = "bFreezeSpeedUpdate";
        static constexpr const char* bFreezeRotationUpdate      = "bFreezeRotationUpdate";
        static constexpr const char* bAnimateWeaponBones        = "bAnimateWeaponBones";
        static constexpr const char* bSyncDirection             = "bSyncDirection";


        static constexpr const char* bNotHeadTrack              = "bNotHeadTrack";
        static constexpr const char* bAllowHeadTracking         = "bAllowHeadTracking";
        static constexpr const char* bDisableSpineTracking      = "bDisableSpineTracking";
        static constexpr const char* m_bEnablePitchTwistMod     = "m_bEnablePitchTwistModifier";


        static constexpr const char* bIsSneaking               = "bIsSneaking";
        static constexpr const char* IsSneaking                = "IsSneaking";


        static constexpr const char* bIsThrowing               = "bIsThrowing";
        // Lowercase 'i' — exact casing from the RaiderRootBehavior.hkx string
        // table, and it matches the existing TP:: table entry name.
        static constexpr const char* isReloading               = "isReloading";
        static constexpr const char* isFiring                  = "isFiring";
        static constexpr const char* bIsSprinting              = "bIsSprinting";
        static constexpr const char* bIsBlocking               = "bIsBlocking";
        static constexpr const char* bAimEnabled               = "bAimEnabled";
        static constexpr const char* bAimCaptureEnabled        = "bAimCaptureEnabled";
        static constexpr const char* bAimActive                = "bAimActive";
        static constexpr const char* bUseRifleReadyDirectAt    = "bUseRifleReadyDirectAt";
        static constexpr const char* bRootRifleEquipOk         = "bRootRifleEquipOk";
        static constexpr const char* bEquipOkIsActiveEnabled   = "bEquipOkIsActiveEnabled";
        static constexpr const char* bGraphWantsFootIK         = "bGraphWantsFootIK";
    }

    namespace Events
    {
        // Round 139: every event below was validated against the string
        // tables of ALL 44 third-person human behavior graphs
        // (HKXExtract/Meshes/Actors/Character/Behaviors). Events that do not
        // exist in any graph were removed or corrected — the engine silently
        // drops unknown events, so sending them was pure no-op noise (and in
        // one case the wrong event snapped the proxy into an A-pose).
        //
        // REMOVED (nonexistent in the human graph): LocoStateChange,
        // JumpLandHard, weaponDrawn, weaponHolstered, getUp,
        // PipBoyMenuOpen/Close, attackStartH2HRight, recoilStart.

        static constexpr const char* MoveStart          = "moveStart";
        static constexpr const char* MoveStop           = "moveStop";


        static constexpr const char* JumpStart          = "jumpStart";
        static constexpr const char* JumpLand           = "jumpLand";


        static constexpr const char* SneakEnter         = "sneakStateEnter";
        static constexpr const char* SneakExit          = "sneakStateExit";


        static constexpr const char* WeaponDraw         = "weaponDraw";
        static constexpr const char* WeaponSheathe      = "weaponSheathe";


        static constexpr const char* ReloadStart        = "reloadStart";
        static constexpr const char* ReloadEnd          = "reloadEnd";


        static constexpr const char* BlockStart         = "blockStart";
        static constexpr const char* BlockStop          = "blockStop";


        static constexpr const char* StaggerStart       = "staggerStart";
        static constexpr const char* StaggerStop        = "staggerStop";


        static constexpr const char* HeadTrackOn        = "HeadTrackingOn";


        static constexpr const char* PowerArmorEnter    = "PowerArmorEnter";
        static constexpr const char* PowerArmorExit     = "PowerArmorExit";


        static constexpr const char* DeathStart         = "DeathStart";


        static constexpr const char* KillMoveStart      = "KillMoveStart";
        static constexpr const char* KillMoveEnd        = "KillMoveEnd";


        // Round 139: graph names are lowercase-f (case-sensitive match).
        static constexpr const char* FurnitureEnter     = "furnitureEnter";
        static constexpr const char* FurnitureExit      = "furnitureExit";


        static constexpr const char* VATSStart          = "VATSStart";
        static constexpr const char* VATSEnd            = "VATSEnd";


        static constexpr const char* SwimStart          = "SwimStart";
        static constexpr const char* SwimStop           = "SwimStop";


        static constexpr const char* Activate           = "Activate";

        static constexpr const char* AttackStart        = "attackStart";
        static constexpr const char* AttackStop         = "attackStop";
        // Round 139: the real melee events per the behavior-graph string
        // tables. Fists are melee-type too — there is no H2H-specific start
        // event in the graph.
        static constexpr const char* AttackStartMelee   = "attackStartMelee";
        static constexpr const char* AttackBashStart    = "meleeAttackBashStart";
        static constexpr const char* AttackStartAuto    = "attackStartAuto";

        // 2026-08-04, verified present in RaiderRootBehavior.hkx's string
        // table: the events that actually play a SHOT. attackStart alone only
        // moves the graph into an attacking state, it never fires the weapon —
        // which is exactly why melee (attackStartMelee) animated on proxies
        // while shooting did not. WeaponFireEffect drives the muzzle flash.
        static constexpr const char* WeaponFire         = "weaponFire";
        static constexpr const char* WeaponFireEffect   = "weaponFireEffect";
        static constexpr const char* AttackRelease      = "attackRelease";


        static constexpr const char* SprintStart        = "sprintStart";
        static constexpr const char* SprintStop         = "sprintStop";

        // 2026-07-31: found via CoSyncAnimVarSystem::DumpGraphVariableNames
        // (a live hash-table dump of an actual PA-entered proxy's graph, NOT
        // guessed) sitting alongside the shared locomotion variables at the
        // SAME indices as the human graph -- confirms PA reuses one graph
        // object rather than swapping to a separate one, and the missing
        // piece for PA locomotion is these event triggers, not different
        // variable indices. Exact casing preserved from the dump (note
        // PAjumpFall/PAjumpEnd keep the base word lowercase like
        // jumpStart/jumpLand, unlike the PascalCase PAMoveStart/PASprint*).
        static constexpr const char* PAMoveStart        = "PAMoveStart";
        static constexpr const char* PASprintStart      = "PASprintStart";
        static constexpr const char* PASprintStop       = "PASprintStop";
        static constexpr const char* PATurn             = "PATurn";
        static constexpr const char* PAJumpFall         = "PAjumpFall";
        static constexpr const char* PAJumpEnd          = "PAjumpEnd";


        static constexpr const char* IdleStop           = "idleStop";

        // 2026-08-04: throw vocabulary, validated the same way as the round-139
        // set -- `strings -n 4` over all 44 human behavior graphs in
        // HKXExtract/Meshes/Actors/Character/Behaviors. All five below appear
        // in RaiderRootBehavior.hkx (the root graph that actually receives
        // events) AND in WeaponBehavior.hkx, alongside the clip families that
        // prove they drive real animation: WPNGrenadeThrow / DefaultGrenadeThrow
        // / SneakGrenadeThrow and WPNMineThrow / DefaultMineThrow /
        // SneakMineThrow.
        //
        // The *Over/*OverLow variants are the cover-shooting throws (the graph
        // gates them on its cover state). We do not drive cover state on
        // proxies, so nothing sends them yet -- they are listed because the
        // graph has them and guessing at the name later is exactly how the
        // pre-round-139 no-op events got written.
        static constexpr const char* GrenadeThrowStart        = "grenadeThrowStart";
        static constexpr const char* GrenadeThrowStartOver    = "grenadeThrowStartOver";
        static constexpr const char* GrenadeThrowStartOverLow = "grenadeThrowStartOverLow";
        static constexpr const char* MineThrowStart           = "mineThrowStart";
        // Ends BOTH the grenade and the mine throw -- the graph has no
        // grenade-specific end event (there is no "grenadeThrowEnd" string in
        // any of the 44 graphs).
        static constexpr const char* ThrowEnd                 = "throwEnd";
    }

    bool Is3DLoaded(Actor* actor);

    // Round 119: Is3DLoaded PLUS a per-actor grace period after 3D first
    // appears. The engine's real anim-var setter spins forever on the graph
    // manager lock (mgr+0xC8/+0xCC, IDA sub_14081AE50) if a loader thread
    // still holds it while the graph is being activated — writing to a
    // proxy's graph 0.5s after spawn froze the main thread twice.
    // 2026-07-27: the local player is NO LONGER exempt — see ResetGraphGrace.
    // A mid-session save reload (death->"Continue", quickload, etc.) rebuilds
    // the local player's graph exactly like a fresh actor spawn does, and the
    // old formID==0x14 shortcut skipped the grace period unconditionally,
    // letting a stale/garbage graph holder get read right after a reload —
    // confirmed as a fatal crash (READ AV in GetBool, bIsUnderwater, ~0.4-1s
    // after "RESPAWN detected").
    bool IsGraphReady(Actor* actor);

    // Call once, right when a reload/respawn is detected (before resuming
    // normal per-tick anim capture), so the local player gets the same fresh
    // grace window a newly-spawned actor gets. No-op for actors IsGraphReady
    // has never seen before (nothing to reset).
    void ResetGraphGrace(Actor* actor);

    bool GetFloat(Actor* actor, const char* name, float& out);
    bool GetInt  (Actor* actor, const char* name, int&   out);
    bool GetBool (Actor* actor, const char* name, bool&  out);

    bool SetFloat(Actor* actor, const char* name, float value);
    bool SetInt  (Actor* actor, const char* name, int   value);
    bool SetBool (Actor* actor, const char* name, bool  value);


    bool SendEvent(Actor* actor, const char* eventName);

}
