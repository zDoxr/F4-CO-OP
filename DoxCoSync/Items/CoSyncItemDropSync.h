#pragma once
#include <cstdint>
#include <string>

// Dropped-item sync: when a player drops something from their inventory, the
// other side sees a matching item appear in their world and can pick it up
// (and vice versa). Uses the same host-arbitrated ownership architecture
// already proven for NPC world sync in this session ("Stage 2"): the
// reporting machine (whoever's local scan found the new drop) sends
// DROPSEEN, the host allocates an entity ID and confirms with DROPCREATE,
// the owner just tracks its own real ref while every other peer spawns a
// visual clone via CoSyncGameAPI::SpawnItemAtPosition. Picking up either the
// real item or a clone (detected via the existing, already-fixed pickup
// hook) broadcasts DROPGONE so every peer removes their copy.
//
// Detection is polling-based (tight radius around the local player, once a
// second) rather than a new engine hook — no vanilla Papyrus event exists
// for "player dropped this item," and reusing the already-proven cell-scan
// pattern (same technique as NpcWorldSync/ContainerSync) avoids introducing
// another unverified RVA in the same session as the door-hook fix.
namespace CoSyncItemDropSync
{
    constexpr uint32_t kDropEntityBase = 0x00030000u;
    constexpr uint32_t kDropEntityMax  = 0x0003FFFFu;
    inline bool IsDropEntityID(uint32_t id) { return id >= kDropEntityBase && id <= kDropEntityMax; }

    // Game thread, both roles. Scans the local player's cell for newly
    // appeared, untracked, lootable-type dynamic refs within a tight radius
    // and reports them; also drains the pending spawn/despawn queue.
    void Tick(double now);

    // Wire dispatch (network thread, queues only).
    bool IsDropSeen(const std::string& msg);
    void OnReceiveDropSeen(const std::string& msg);      // host only
    bool IsDropCreate(const std::string& msg);
    void OnReceiveDropCreate(const std::string& msg);    // all roles
    bool IsDropGone(const std::string& msg);
    void OnReceiveDropGone(const std::string& msg);      // all roles

    // Called from CoSyncPickupHook's detour when a picked-up refFormID
    // matches a tracked drop (either the reporter's real original or a
    // locally-spawned clone). No-op if refFormID isn't tracked.
    void NotifyLocalPickup(uint32_t refFormID);

    void Reset();
}
