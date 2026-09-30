#pragma once

#include <string>
#include <cstdint>

class TESObjectREFR;

// World-object state replication (0.3.5): the engine-level state changes of
// world references, captured where the engine makes them -- not as a list of
// "car exploded / grate opened / door destroyed" cases. Everything below was
// verified in Fallout4.exe 1.11.240 (IDA, 2026-09-29; trace in
// docs/NPC_COMBAT_WORLD_REPORT.md):
//
//  DESTRUCTION -- every destructible (cars, barrels, gas pumps, doors, gates,
//    turret mounts...). A ref's destruction stage is not stored: it is derived
//    from its destructible HEALTH (extra-data 0x55, float +0x18,
//    sub_14028CFD0) against the stage thresholds of the base's
//    BGSDestructibleObjectForm (GetCurrentDestructionStage sub_140300730).
//    Every stage change, whatever caused it (a shot, an explosion, a
//    Deathclaw, a script's DamageObject, ClearDestruction), is announced by
//    one function: TESDestructionStageChangedEvent send, sub_14052C7F0(ref,
//    oldStage, newStage). Hooked there. Receivers damage their copy through
//    the engine's own DamageObject task (TaskQueueInterface task 7,
//    sub_140C461E0) by exactly the health difference, so the engine runs the
//    stage transitions itself (model swap, debris, the car's explosion).
//    Merge is monotonic: a late or duplicate report can never un-destroy
//    anything; only a reported ClearDestruction (stage -> 0) resets
//    (sub_1403014C0).
//
//  OPEN / CLOSE -- doors, gates, manholes, hatches (the Concord manhole the
//    Deathclaw comes out of is DOOR MQ104_RoadManholeDoor01). Every change of
//    open state goes through BGSOpenCloseForm::SetOpenState(ref, open, snap)
//    = sub_140305A90 (21 engine callers: activation, AI door use, the Papyrus
//    SetOpen/Lock natives...). Hooked there; only real changes
//    (GetOpenState sub_140305CE0 before/after) and only those NOT caused by a
//    player activation (CoSyncDoorHook already replays those as DOOR).
//
//  ENABLE / DISABLE by SCRIPT -- quest scripts run on the host only (clients'
//    stages are suppressed by CoSyncQuestHook), so everything a quest enables
//    or disables has to reach the clients. Papyrus Enable/Disable run through
//    GameScript EnableFunctor / DisableFunctor (vtables 0x25CCA58 /
//    0x25CCAF8, slot 1 = run: sub_141150560 -> TESObjectREFR::Enable
//    sub_140515970; sub_1411508C0 -> the ref's virtual Disable, slot 0xB0,
//    sub_140515FC0 for TESObjectREFR). Host only, non-actor refs with a
//    static formID only (actors belong to the NPC sync; CoSync's own
//    representation changes -- hidden originals, frame copies -- are made
//    through direct engine calls and never pass these functors).
//
// Wire (form IDs translated by CoSyncFormIdMap):
//   WSD|ref|base|x|y|z|oldStage|newStage|health   destruction
//   WSO|ref|base|x|y|z|open                        open state (absolute)
//   WSE|ref|enabled                                script enable/disable (host)
namespace CoSyncWorldStateSync
{
    // Plugin load: MinHook + two verified vtable slot swaps, each checked.
    void Install();

    bool IsWorldStateMsg(const std::string& msg);
    // Game thread (CoSyncNet::OnReceive). Queues.
    void OnReceive(const std::string& msg, bool fromHostOrIsHost);

    // Game thread, every CoSync tick. Sends captured changes, applies queued
    // remote ones (retrying refs that are not loaded yet), 10 s summary.
    // Call BEFORE CoSyncExplosionSync so a replicated destruction's own
    // explosion exists before a duplicate EXP is considered.
    void Tick(double now);

    void Reset();
}
