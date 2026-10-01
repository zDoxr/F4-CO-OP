#pragma once

#include <string>
#include <cstdint>

class Actor;

// Enemy synchronization (0.4.1). Full research, IDA evidence and the three-pass
// review: docs/ENEMY_SYNC_REPORT.md.
//
// Authority: the machine that SIMULATES an enemy (its "owner": the host
// wherever the host's engine has it loaded -- CoSyncNpcWorldSync's ownership
// rules) decides its hits, health, death, killer, target and scripted casts.
// Every other machine shows a puppet and reproduces those results through the
// engine's own functions. Engine path (Fallout4.exe 1.11.240):
//
//   ModActorValue(actor, 2, Health, -dmg, attacker)   sub_140CC9B70
//     -> CheckForDeath(attacker)  Actor vfunc 0x110   sub_140CD2AE0
//     -> kill core(victim, killer, dmg, silent, ragdollInstant)  sub_140C61800
//          (killer handle -> Actor+0x384, task 8 queued)
//     -> KillImpl  Actor vfunc 0x117                   sub_140CA00C0
//          (ActionDeath on the actor's own graph = the death animation,
//           TESDeathEvent, kill credit when the killer is the player)
//   TESHitEvent (Papyrus OnHit)                        sub_14052D0B0(HitData*)
//   Papyrus Spell.Cast native (latent CastFunctor)     sub_14113F810
//
// Hooks (MinHook, each prologue-checked, each independent):
//   kill core   -- refuses a local death of a guarded puppet (only the owner's
//                  KILL, replayed here, may kill it). Gated BEFORE the engine
//                  marks the actor "kill queued", so the real kill still runs.
//   KillImpl    -- the owner's deaths, with the real killer and ragdoll flag.
//   ModActorValue -- damage a puppet takes from an attacker simulated HERE
//                  (local player / NPC this machine owns) -> EHIT to the owner.
//                  Covers DoHitMe hits and magic damage-over-time alike.
//   Spell.Cast  -- a script self-cast by an enemy this machine owns (the
//                  legendary mutation is one: AbLegendaryScript OnHit < 50 % ->
//                  LegendaryPowerUp.Cast(self, self)) is replicated; the same
//                  self-cast by a puppet's own local script is refused.
//
// Wire (all reliable):
//   EHIT|target|attacker|seq|damage|flags   attacker's machine -> host -> owner
//   ENS|eid|ver|combat|target|class|life|castList   owner -> all (state)
//   KILL|eid|killer|flags                   owner -> all (flags bit0 = ragdoll)
namespace CoSyncEnemySync
{
    // ENS class bits (docs/ENEMY_SYNC_REPORT.md 1.4-1.6).
    enum ClassBits : uint32_t
    {
        kClass_Legendary  = 1u << 0,   // keyword EncTypeLegendary 0x1CF26C
        kClass_Boss       = 1u << 1,   // ref has a LocRefType of BossLocRefTypeList 0xB6EE9
        kClass_Unique     = 1u << 2,   // ACBS Unique
        kClass_Essential  = 1u << 3,   // ACBS Essential
        kClass_Robot      = 1u << 4,   // ActorTypeRobot 0x2CB73
        kClass_Turret     = 1u << 5,   // ActorTypeTurret 0xB2BF3
        kClass_Creature   = 1u << 6,   // ActorTypeCreature 0x13795 / ActorTypeAnimal 0x13798
        kClass_Humanoid   = 1u << 7,   // ActorTypeNPC 0x13794
        kClass_PowerArmor = 1u << 8,   // in power armor (ExtraPowerArmor)
        kClass_Known      = 1u << 31,  // classification was computed
    };

    // Plugin load: installs the four hooks.
    void Install();

    // Session start/end.
    void Reset();

    // Game thread, every CoSync tick (after FlushPendingHealthOps).
    void Tick(double now);

    // EHIT| or ENS| (KILL stays in CoSyncHealthSync).
    bool IsEnemyMessage(const std::string& msg);
    bool IsHit(const std::string& msg);
    // Game thread (CoSyncNet::OnReceive, after the host's sender check).
    // Only parses and queues.
    void OnReceive(const std::string& msg);

    // True while the ModActorValue hook is live: world-NPC damage is then
    // reported per hit with its attacker, and HealthSync's HP-drop polling
    // must not report the same damage again.
    bool IsHitCaptureActive();

    // Owner side: KILL for this entity already went out (from the KillImpl
    // hook or a poller). Pollers call NoteDeathAnnounced when they send one.
    bool WasDeathAnnounced(uint32_t entityID);
    void NoteDeathAnnounced(uint32_t entityID);

    // KILL text for an entity, carrying the killer/flags recorded when it
    // died here (0/0 when unknown). Used by the pollers and the 5 s resend.
    std::string BuildKill(uint32_t entityID);

    // Receiver: KILL|eid|killer|flags for a puppet. Runs the engine kill core
    // (death animation, death event, credit when the killer is our player).
    // Returns false when the engine path is unavailable -- the caller then
    // falls back to CoSyncGameAPI::KillPuppet.
    bool ReplayDeath(uint32_t entityID, Actor* puppet, uint32_t killerEntityID, uint32_t flags);

    // The network entity ID of an actor as this machine knows it (the local
    // player -> our own ID). 0 = not a synced entity.
    uint32_t EntityIdOf(Actor* actor);

    // 0.4.3: thread-safe tests used by engine hooks on any thread.
    // A guarded puppet = this machine's copy of a world NPC another machine
    // simulates. A player proxy = this machine's copy of another player.
    bool IsGuardedPuppet(Actor* actor);
    bool IsPlayerProxy(Actor* actor);

    // 0.4.3: a puppet's Health is its owner's value only. Local engine writes
    // to a guarded puppet's Health (hits, damage over time, regeneration) are
    // refused in the ModActorValue hook -- the hit is still reported to the
    // owner when this machine simulates the attacker -- so the HUD enemy meter
    // (it reads the local actor's Health percent, sub_140A349F0 ->
    // sub_140C63650) never shows a locally predicted value that the owner's
    // HP then pulls back. CoSync's own authoritative writes (HealthSync's HP
    // apply, the engine kill replay) run inside this scope and pass.
    struct ScopedHealthWrite
    {
        ScopedHealthWrite();
        ~ScopedHealthWrite();
        ScopedHealthWrite(const ScopedHealthWrite&) = delete;
        ScopedHealthWrite& operator=(const ScopedHealthWrite&) = delete;
    };
    bool IsAuthoritativeHealthActive();
}
