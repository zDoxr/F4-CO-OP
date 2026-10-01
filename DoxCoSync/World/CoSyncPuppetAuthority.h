#pragma once

#include <cstdint>

class Actor;

// Puppet authority (0.4.3). Full evidence: docs/NPC_AUTHORITY_REPORT.md.
//
// A "puppet" is this machine's copy of a world NPC another machine simulates
// (CoSyncEnemySync's guard set). Until 0.4.2 a puppet ran its whole AI: it
// detected the local player, entered combat, picked targets, chose packages
// and fed its own movement intention into the animation graph -- and CoSync
// fought that every frame (StopCombat per tick, SetPosition per frame).
// The engine paths involved (Fallout4.exe 1.11.240, IDA):
//
//   combat entry  sub_140D00F80(AIProcess*, Actor*, Actor* target, ...)
//                 every way into combat (detection, Papyrus StartCombat task
//                 97, scripts) goes through it; it starts the combat
//                 controller (Actor+0x328) and the "CombatAnger" bark
//   locomotion    sub_140C66910 (movement update) asks the movement
//                 controller (Actor+0x318) for its DESIRED velocity and writes
//                 it into the graph channels Speed (middle-high process
//                 +0x428, setter sub_140D35520) and Direction (+0xC0, setter
//                 sub_140D355C0)
//
// What this module does instead:
//   - combat entry on a puppet is refused unless the target is the target
//     its OWNER's NPC is fighting (ENS/EU). The owner's combat is then
//     started on the puppet once through the engine's own StartCombat task
//     (sub_140C46B90, the call the Papyrus native makes after validation),
//     so the combat state (and with it the HUD enemy meter, whose visibility
//     test sub_140CD0940 is "the actor's combat group targets the player")
//     is stable and matches the owner.
//   - the puppet's Speed/Direction channel writes are replaced by the
//     owner's raw channel values (EU), so the engine's own locomotion blend
//     plays the owner's movement instead of the puppet's AI intention.
//
// Every hook is prologue-checked; a mismatch disables only that part and the
// 0.4.2 behaviour (StopCombat per tick) stays in charge. INI switches in
// [Puppet] of CoSync.ini: bCombatGate, bMovementChannels (default 1).
namespace CoSyncPuppetAuthority
{
    void Install();
    void Reset();

    bool CombatGateActive();
    bool MovementOverrideActive();

    // Owner side: the actor's raw engine locomotion channels. False when it has
    // no middle-high process (low-process actors have no channels).
    bool ReadMoveChannels(Actor* actor, float& speed, float& direction);

    // Receiver side, game thread, every frame for a steered puppet. `fresh`
    // false releases the override (the engine's own values apply again).
    void SetPuppetMovement(Actor* puppet, float speed, float direction, bool fresh);

    // The owner's combat state for this puppet (game thread). localTarget is
    // the target resolved on this machine (our player, a proxy, another
    // puppet), nullptr when the owner is not fighting or fights something not
    // synced here.
    void ApplyOwnerCombat(uint32_t entityID, Actor* puppet, bool active, Actor* localTarget, double now);

    // A puppet is going away (despawn / unbind / freed).
    void ForgetPuppet(Actor* puppet);

    // Game thread, every CoSync tick (10 s diagnostics line).
    void TickDiagnostics(double now);
}
