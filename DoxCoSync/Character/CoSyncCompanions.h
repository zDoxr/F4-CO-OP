#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Actor;

// Companions in a shared world (Phase 7d, 2026-09-26).
//
//   * The HOST's companions are real actors in the host's world. They sync to
//     clients like any host-owned world actor (CoSyncNpcWorldSync no longer
//     skips the host's teammates), so every client sees them following the
//     host -- and only the host can command them.
//   * A client plays in a copy of the host's save, where the host's followers
//     would follow the CLIENT. Those inherited followers are handed back: the
//     client disables its local copies, and the host's stream puppets them.
//   * A CLIENT's companion is the client's own copy of that NPC: spawned on
//     the client, made its companion through the vanilla Followers quest
//     (FollowersScript.SetCompanion / SetDogmeatCompanion), so it follows
//     and takes commands from that client only. It is registered with the host
//     as client-owned (COMPOWN) and everyone else sees a puppet following
//     that client. Duplicates are fine: the host and every client may all
//     have Piper at the same time.
//   * A client's companions are part of its saved character (Profiles) and
//     come back on rejoin. Dismissing one removes that copy.
//
// Wire: C->H  COMPOWN|<baseFormID>|<refFormID>|<x>,<y>,<z>|<ownerEntityID>
//       C->H  COMP_DISMISS|<entityID>   (existing; the host removes the entity)
//
// Threading: the Papyrus notifications only queue (they run on the VM
// thread); everything else runs on the main thread.
namespace CoSyncCompanions
{
    // Papyrus (CoSyncQuest's follower poll): any thread.
    void NotifyRecruited(Actor* actor);
    void NotifyDismissed(Actor* actor);

    // Client, main thread: its character was applied. Hands the host's
    // inherited followers back and brings the saved companions (base formIDs).
    void ClientOnCharacterApplied(const std::vector<uint32_t>& savedBases);

    // Client: base formIDs of its current companions (saved with the character).
    std::vector<uint32_t> GetMyCompanionBases();

    bool IsCompanionMessage(const std::string& msg);
    void OnReceive(const std::string& msg, bool isHostRole);

    // Main thread, every frame.
    void Tick(double now);

    void Reset();
}
