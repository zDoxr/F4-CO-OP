#pragma once

#include <cstdint>
#include <unordered_map>
#include <deque>
#include <mutex>

#include "Packets_EntityCreate.h"
#include "Packets_EntityUpdate.h"
#include "CoSyncPlayer.h"

// -----------------------------------------------------------------------------
// InboxItem
// Network → game-thread message envelope
// -----------------------------------------------------------------------------
struct InboxItem
{
    enum class Type : uint8_t
    {
        Create,
        Update
    };

    Type type = Type::Update;
    EntityCreatePacket create{};
    EntityUpdatePacket update{};
};

// -----------------------------------------------------------------------------
// CoSyncPlayerManager
//
// F4MP-aligned responsibilities:
//   - Owns remote entity proxies (CoSyncPlayer)
//   - Buffers network messages (Create / Update)
//   - Applies updates on game thread
//   - Defers spawning until world is ready
//
// RULES:
//   - UPDATE packets NEVER cause spawning
//   - CREATE packets are the ONLY spawn trigger
//   - Spawning is rate-limited (≤1 per tick)
// -----------------------------------------------------------------------------
class CoSyncPlayerManager
{
public:
    // -------------------------------------------------------------------------
    // Network entry points (thread-safe)
    // -------------------------------------------------------------------------
    void EnqueueEntityCreate(const EntityCreatePacket& p);
    void EnqueueEntityUpdate(const EntityUpdatePacket& p);

    // -------------------------------------------------------------------------
    // Game-thread processing
    // -------------------------------------------------------------------------
    // Drains inbox → routes to ProcessEntity*
    void ProcessInbox();

    // Per-frame tick:
    //   - pumps deferred spawns
    //   - handles timeouts / cleanup
    void Tick();

    // -------------------------------------------------------------------------
    // Identity
    // -------------------------------------------------------------------------
    // Prevents spawning/updating a proxy for the local player
    // MUST be set once local entity ID is known
    void SetLocalEntityID(uint32_t id) { m_localEntityID = id; }

    // Lookup or create a remote proxy slot
    CoSyncPlayer& GetOrCreateByEntityID(uint32_t entityID);

private:
    // -------------------------------------------------------------------------
    // Internal processing (game thread only)
    // -------------------------------------------------------------------------
    void ProcessEntityCreate(const EntityCreatePacket& p);
    void ProcessEntityUpdate(const EntityUpdatePacket& u);

    // Spawn at most ONE queued CREATE per tick (F4MP rule)
    void PumpDeferredSpawns();

private:
    // -------------------------------------------------------------------------
    // Network inbox (can be written from networking layer)
    // -------------------------------------------------------------------------
    std::mutex m_inboxMutex;
    std::deque<InboxItem> m_inbox;

    // -------------------------------------------------------------------------
    // Deferred CREATE queue (spawn-only)
    // -------------------------------------------------------------------------
    std::mutex m_spawnMutex;
    std::deque<EntityCreatePacket> m_pendingCreates;

    // -------------------------------------------------------------------------
    // Remote entity registry
    // -------------------------------------------------------------------------
    std::unordered_map<uint32_t, CoSyncPlayer> m_playersByEntityID;

    // Local authoritative entity ID (never proxied)
    uint32_t m_localEntityID = 0; // 0 = unknown/unset
};

// Global instance (mirrors F4MP singleton-style manager)
extern CoSyncPlayerManager g_CoSyncPlayerManager;
