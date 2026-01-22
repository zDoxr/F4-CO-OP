#pragma once

#include <cstdint>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <memory>

#include "Packets_EntityDestroy.h"
#include "Packets_EntityCreate.h"
#include "Packets_EntityUpdate.h"
#include "CoSyncEntityState.h"

// -----------------------------------------------------------------------------
// InboxItem
// Network → game-thread message envelope
// -----------------------------------------------------------------------------
struct InboxItem
{
    enum class Type : uint8_t
    {
        Create,
        Update,
        Destroy
    };

    Type type = Type::Update;

    EntityCreatePacket  create{};
    EntityUpdatePacket  update{};
    EntityDestroyPacket destroy{};
};

class CoSyncPlayer;

// -----------------------------------------------------------------------------
// CoSyncPlayerManager
//
// F4MP-aligned responsibilities:
//
//   ✔ Owns ALL remote entity proxies (CoSyncPlayer)
//   ✔ Owns ALL remote entity state (CoSyncEntityState)
//   ✔ Buffers CREATE / UPDATE / DESTROY packets from networking
//   ✔ Applies everything on the GAME THREAD only
//   ✔ Spawns ≤ 1 entity per tick (CRITICAL F4MP RULE)
//
// HARD RULES (DO NOT BREAK):
//   - UPDATE packets NEVER cause spawning
//   - CREATE packets are the ONLY spawn trigger
//   - Local player entity is NEVER proxied
//   - NPC authority is HOST-ONLY
// -----------------------------------------------------------------------------
class CoSyncPlayerManager
{
public:
    // ---------------------------------------------------------------------
    // Network entry points (THREAD-SAFE)
    // ---------------------------------------------------------------------
    void EnqueueEntityCreate(const EntityCreatePacket& p);
    void EnqueueEntityUpdate(const EntityUpdatePacket& p);
    void EnqueueEntityDestroy(const EntityDestroyPacket& p);

    // Host-only NPC authority path (debug + future AI)
    void HostSendNpcUpdates(double now);

    // ---------------------------------------------------------------------
    // Game-thread processing
    // ---------------------------------------------------------------------
    void ProcessInbox();
    void Tick();

    // ---------------------------------------------------------------------
    // Identity
    // ---------------------------------------------------------------------
    void SetLocalEntityID(uint32_t id) { m_localEntityID = id; }
    uint32_t GetLocalEntityID() const { return m_localEntityID; }

    // ---------------------------------------------------------------------
    // Proxy (world) registry
    // ---------------------------------------------------------------------
    CoSyncPlayer& GetOrCreateByEntityID(uint32_t entityID);

    // ---------------------------------------------------------------------
    // State registry (network-only)
    // ---------------------------------------------------------------------
    CoSyncEntityState& GetOrCreateState(uint32_t entityID);

    // Optional: readonly accessors (overlay / debug / tooling)
    const std::unordered_map<uint32_t, CoSyncEntityState>&
        GetStates() const { return m_statesByEntityID; }

private:
    // ---------------------------------------------------------------------
    // Internal processing (GAME THREAD ONLY)
    // ---------------------------------------------------------------------
    void ProcessEntityCreate(const EntityCreatePacket& p);
    void ProcessEntityUpdate(const EntityUpdatePacket& u);
    void ProcessEntityDestroy(const EntityDestroyPacket& d);

    void DespawnEntity(uint32_t entityID, const char* reason);
    void HandleTimeouts(double now);

    // Deferred spawn pump (≤ 1 per tick)
    void PumpDeferredSpawns();

private:
    // ---------------------------------------------------------------------
    // Network inbox (THREAD-SAFE)
    // ---------------------------------------------------------------------
    std::mutex m_inboxMutex;
    std::deque<InboxItem> m_inbox;

    // ---------------------------------------------------------------------
    // Deferred CREATE queue (spawn-only, GAME THREAD)
    // ---------------------------------------------------------------------
    std::mutex m_spawnMutex;
    std::deque<EntityCreatePacket> m_pendingCreates;

    // ---------------------------------------------------------------------
    // Remote entity registry (world proxies)
    // ---------------------------------------------------------------------
    std::unordered_map<uint32_t, std::unique_ptr<CoSyncPlayer>> m_playersByEntityID;

    // ---------------------------------------------------------------------
    // Remote state registry (network-only)
    // ---------------------------------------------------------------------
    std::unordered_map<uint32_t, CoSyncEntityState> m_statesByEntityID;

    // ---------------------------------------------------------------------
    // Local authoritative entity ID
    // ---------------------------------------------------------------------
    uint32_t m_localEntityID = 0;
};

// Global singleton
extern CoSyncPlayerManager g_CoSyncPlayerManager;
