#pragma once

#include <cstdint>
#include <string>

class NiPoint3;
class TESObjectREFR;

// Vault 111 for every player (0.5.6): the vanilla cryopod sequence plays on
// each machine untouched, CoSync works around it, not against it.
//
// Fallout4.esm, read from the ESM and the compiled scripts (2026-10-06):
//   MQ101 "War Never Changes" 0x1ED86. Stage 600 = the lift reaches
//     PrewarVault111 0xCA260; 7xx = suit, pod, freeze; 900 = the jump to the
//     post-war cryo room; 1000 = set by MQ102's quest script when the player
//     has control again after waking (MQ03QuestScript "CameraOverrideStop").
//   MQ102 "Out of Time" 0x1CC2A in Vault111Cryo 0x16D8:
//     0 wake-up scene, 1 control back, 2 cryo audio, 3 QT00 door unlocked,
//     4 Pip-Boy taken from the skeleton (blocks the skeleton + Pip-Boy ref),
//     5/6 tutorial, 7 Pip-Boy plugged into the gear console (adds the Pip-Boy
//     0x21B3B, removes PipboyDusty 0x6D7B8, puts the player IN the console
//     furniture), 9 console button -> control panel "Stage4" -> gear door
//     Vault111GearDoorREF 0x330C3 "Stage2" + klaxons, 8 VATS back,
//     10 set by the exterior trigger 0x51877 after the ride up -- its fragment
//     DISABLES RespecTriggerREF 0x8C94A (the platform trigger the ride starts
//     from), then 15+ the Commonwealth.
//   The ride: vaultExitElevatorDown trigger 0x20C3A (stage >= 9) lowers
//     V111ElevatorREF 0x244EB to the bridge and disables itself; the platform
//     trigger RespecTriggerREF 0x8C94A (vaultExitElevatorSCRIPT, DOONCE)
//     runs the "final changes" menus, then plays the elevator "stage3" and
//     enables V111EnableRaiseElevatorTriggerEnableMarker 0x1D6CD3; the
//     auto-load door 0x1F180 takes that player outside.
//   Every one of those scripts acts on Game.GetPlayer() only.
//
// Root cause of "the host rides up, the client is stuck" (logs 2026-10-05
// 22:29-22:31, 38320 client / 6532 host): the host's machine exited and three
// things of ITS exit reached the client, which was still inside:
//   - WSE|134202|0: the host's 0x20C3A disabling itself -> the client's
//     elevator could never come down to the bridge.
//   - QS MQ102 10 (22:30:56): its fragment disabled the client's
//     RespecTriggerREF -> no ride for the client at all (+ WSE|575818|0).
//   - DOOR 0x1F180 (22:30:36): the host's auto-load door replayed on the
//     client (CoSyncDoorHook, load doors no longer replayed).
// And the other way round (2026-10-06 14:12:08): the host's stage 7 reached
// the client and pulled the CLIENT into the gear console furniture; the
// client's stage 4 blocked the host's Pip-Boy skeleton.
//
// Now, per machine (the phase is derived from the game, never from timers):
//   PreWar           MQ101 running, before the vault -- the 0.5.3 lockstep
//                    (CoSyncSharedIntro): the host's story, SPECIAL with the rep.
//   VanillaSequence  MQ101 running, in the vault (stage >= 600 or a pod room):
//                    walk, suit, pod, freeze, wake-up. Nothing of it crosses
//                    machines; no NPC in PrewarVault111 is synced; no join is
//                    let in on the host (the lobby holds it).
//   VaultExplore     MQ101 done, MQ102 running, stage 10 not done here.
//                    On entry, once: "reapply" -- the player is stood in front
//                    of their own cryopod, the Pip-Boy state applied.
//   Complete         MQ102 stage 10 done here (this player is outside), or the
//                    game is not in the intro at all: normal CoSync.
//
// Quest stages:
//   personal  never leave the machine, never applied from another: MQ101 > 600,
//             MQ102 <= 10 except 3 and 9, and while not Complete the Tutorial
//             0x160147 and DialogueVault111 0x1ED62.
//   shared    MQ102 3 (doors) and 9 (gear door): host-authoritative world state;
//             a client's goes through the host (QSREQ), the host's reaches all.
//   deferred  MQ102 > 10 on a machine whose player is still inside: applied
//             when that player has exited.
// World state: WSE / WSO for references in the two intro cells, and the
// exit's own references, are not applied on a machine that is not Complete
// (every one of them is reproduced there by its own scripts).
//
// Cryopods: one stable list, written by the host: host first, then each player
// in the order they joined (V111|pods|...). Index 0 = the scripted player pod,
// 1-6 = the six resident pods (nearest first), 7+ = no pod (stand in the
// aisle). Each machine's own player is always in the scripted pod during the
// vanilla sequence (that is the cutscene); the others are shown in their own
// pods (the host, on a client, in that client's pod -- the one swap that keeps
// every pod's owner the same on every machine). Residents of the pods given to
// players leave them (locally); unused pods keep their vanilla occupant.
// After the wake-up each client is stood in front of its own pod, so from then
// on everyone's real positions agree and nothing is remapped.
//
// Pip-Boy: "the Pip-Boy event happened" = MQ102 stage 4 or 7 done on the host,
// the host carries the Pip-Boy after the intro, or a client reported its own
// event (persisted because the host then gets the Pip-Boy too). The host sends
// it (V111|world|<pip>|<gate>); every machine whose player has none gets it
// with CoSyncProfiles::EnsurePipBoy -- the same engine add + equip the
// late-join fallback (ReplaceInventory / GiveStartingItem) has always used.
//
// Wire:  any  -> all  V111|st|<eid>|<phase>|<flags>        (host relays)
//        host -> all  V111|pods|<eid0>,<eid1>,...
//        host -> all  V111|world|<pip 0/1>|<gate 0/1>
namespace CoSyncVault111
{
    enum class Phase : int { Complete = 0, PreWar = 1, VanillaSequence = 2, VaultExplore = 3 };

    // This machine's phase (cached by Tick, any thread).
    Phase GetPhase();
    const char* PhaseName(Phase p);

    // ── Quest stages ────────────────────────────────────────────────────────
    enum class StagePolicy { Normal, Personal, Defer };
    // Any thread. Same answer for an outgoing request, an incoming QS and an
    // incoming QSREQ (Defer only ever for MQ102 > 10 while inside).
    StagePolicy ClassifyStage(uint32_t questFormID, int stage);
    // Client hook: MQ101 during PreWar is the host's story (0.5.3 lockstep).
    bool IsLockstepBlocked(uint32_t questFormID, int stage);
    // Keeps a deferred stage until this player has left the vault.
    void DeferStage(bool isRequest, uint32_t sender, uint32_t questFormID, int stage);
    // Tracked-quest and objective packets for the intro quests while not Complete.
    bool IgnoreTrack(uint32_t questFormID);
    // QS_ST / QS_DONE for MQ101 / MQ102 while this player is inside the vault.
    bool IgnoreQuestLifecycle(uint32_t questFormID);
    bool IgnoreObjective(uint32_t questFormID);

    // ── World ───────────────────────────────────────────────────────────────
    // WSE / WSO receive side (game thread): this ref's remote state is not ours.
    bool IgnoreWorldStateFor(TESObjectREFR* ref);
    // NPC sync: an actor in PrewarVault111 belongs to that machine's cutscene.
    bool IsIntroLocalActor(TESObjectREFR* ref);
    // Host lobby: hold joins while the host is in the vanilla sequence.
    bool HostHoldsJoins();

    // ── Cryopods ────────────────────────────────────────────────────────────
    int  PodIndexOf(uint32_t entityID);   // -1 = not in the list
    // CoSyncPlayer: a remote player in the scripted pod is shown in their own.
    bool RemapProxyIntoPod(uint32_t entityID, NiPoint3& pos, NiPoint3& rot);

    bool IsVaultMessage(const std::string& msg);
    void OnReceive(const std::string& msg, bool isHostRole);
    void Tick(double now);
    void Reset();
}
