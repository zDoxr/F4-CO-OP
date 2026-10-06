#pragma once

#include <string>
#include <cstdint>

// Combat authority (0.3.5): who fires, and who applies the damage.
//
// Engine path this is built on (Fallout4.exe 1.11.240, IDA-verified
// 2026-09-29, full trace in docs/NPC_COMBAT_WORLD_REPORT.md):
//
//   fire clip annotation "weaponFire"
//     -> WeaponFireHandler::Handle (vtable 0x2574788 slot 1, sub_140DFFBE0)
//        (also: task-queue fire sub_140C4D320 case 28, scripted fire sub_140988680)
//     -> TESObjectWEAP::Fire(weaponInstance*, source, equipIndex, ammo, poison)
//        = sub_1404799B0
//          - launch origin: the weapon's fire node (sub_140C82B60 / sub_140D32510)
//          - aim (non-player shooter): the handle at AIProcess+0xBC
//            (sub_140CF1270), fallback: the actor's aim channels
//          - Projectile::Launch per pellet (sub_140E421E0)
//   projectile / melee hit
//     -> Actor::DoHitMe(target, HitData&) = sub_140CD2CA0
//        HitData+0x40 = aggressor ActorHandle (read by DoHitMe itself)
//
// Authority rule, one line: every shot and every hit is decided on the
// machine that SIMULATES the shooter. Everywhere else the shot is
// reproduced for the eyes only:
//
//   * Fire hook: a shooter simulated here (the local player, NPCs this
//     machine owns) -> recorded and replicated as SHOT. A shooter simulated
//     elsewhere (a registered proxy/puppet) -> its own engine fire is refused;
//     only replayed SHOTs fire on it.
//   * SHOT replay: the engine's own TESObjectWEAP::Fire on the proxy, aimed
//     through AIProcess+0xBC at the local copy of the owner's target. Real
//     projectile, muzzle flash, sound, impacts. Shots whose projectile
//     carries an explosion are animation-only here: the flight and the
//     detonation already arrive through CoSyncThrowableSync/ExplosionSync.
//   * DoHitMe hook: a hit whose aggressor is simulated elsewhere is refused
//     (no damage, no hit reaction here). The aggressor's own machine applies
//     it and the result arrives as DMG/HP, as before. This also ends the
//     "phantom damage" of a machine's local AI copies fighting each other.
//
// Wire:  SHOT|shooterEid|seq|weaponFormID|equipIndex|targetEid|flags
//        flags bit0 = explosive projectile (animation only on receivers)
class Actor;

namespace CoSyncCombatAuthority
{
    // Plugin load. MinHook on Fire and DoHitMe, prologue-checked; each hook
    // is independent (one failing leaves the other).
    void Install();

    bool IsShot(const std::string& msg);
    // Game thread (CoSyncNet::OnReceive). Queues; replay happens in Tick.
    void OnReceiveShot(const std::string& msg);

    // Game thread, every CoSync tick: sends this machine's recorded shots,
    // replays queued remote shots, logs a 10 s summary.
    void Tick(double now);

    // 0.5.1 (Bug #8): the actor that last hit this player proxy here (its
    // form ID, recorded by the DoHitMe hook) if that was within maxAgeSec,
    // else 0. Lets the HP-drop relay say who caused the damage.
    uint32_t RecentAggressorFormID(Actor* proxy, double maxAgeSec);

    // Session start/end.
    void Reset();

    // 0.4.4: the shooter entity changed owner in place; its shot sequence
    // starts again from the new owner.
    void OnOwnershipChanged(uint32_t entityID);
}
