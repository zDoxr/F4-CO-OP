#pragma once

class Actor;
class ActorValueInfo;

// round 30: Fallout4.exe+0xCC934D (a `call qword ptr [rax+8]` vtable dispatch
// inside the engine's ActorValueOwner internals) crashes whenever ANY direct
// actorValueOwner.* call touches a CoSync-spawned proxy actor -- confirmed
// from THREE independent call sites this session (CoSyncHealthSync's
// GetMaximum, CoSyncSpecialSync's SetBase, and CoSyncPlayerManager's
// GetValue/GetMaximum), each reached via a different nearby Fallout4.exe
// caller (+0xCC9247, +0xCC9101, +0xCC9461) but landing on the exact same
// crash byte sequence every time. Root cause: every CoSync proxy actor has
// AI explicitly disabled synchronously right after spawn (see
// CoSyncGameAPI::DisableActorAI), and the engine only allocates the
// sub-object this vtable slot dispatches through during normal AI
// initialization -- an AI-disabled proxy never gets it, period, so no
// settle-delay (unlike kAnimGraphSettleDelay/kTeleportSettleSeconds
// elsewhere in this codebase) fixes it. SEH can actually catch this one
// (0xC0000005, a real AV -- not a fail-fast/illegal-instruction like round
// 28's creature anim crash), so route every direct actorValueOwner touch on
// a proxy/remote/NPC actor through these wrappers instead of calling
// GetValue/GetMaximum/GetBase/SetBase directly. The genuine local player
// (g_player) is normally AI-initialized and not at risk, but routing those
// calls through here too is harmless and keeps the rule simple: never call
// actorValueOwner directly on any Actor* in this codebase.
namespace CoSyncAVSafety
{
    bool SafeGetValue  (Actor* actor, ActorValueInfo* avi, float& out);
    bool SafeGetMaximum(Actor* actor, ActorValueInfo* avi, float& out);
    bool SafeGetBase   (Actor* actor, ActorValueInfo* avi, float& out);
    bool SafeSetBase   (Actor* actor, ActorValueInfo* avi, float  value);

    // ForceCurrent(DAMAGE, avi, delta) — vtable slot 6 (IDA-verified). Applies a
    // current-value change the engine's damage system's way: clamps, updates the
    // health bar, and kills at <=0. Use for puppet health mirroring instead of
    // SetBase. Pass delta = targetValue - currentValue.
    bool SafeForceDamage(Actor* actor, ActorValueInfo* avi, float  delta);
}
