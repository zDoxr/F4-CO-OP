#pragma once

#include <cstdint>

#include "Packets_EntityCreate.h"
#include "Packets_EntityUpdate.h"

// -----------------------------------------------------------------------------
// CoSyncEntityState
//
// Network-authoritative state for a remote entity.
// Lives independently of whether a world proxy (Actor) exists yet.
//
// F4MP rules:
//  - CREATE establishes existence
//  - UPDATE refreshes lifetime + motion
//  - Spawning happens ONLY via CREATE
// -----------------------------------------------------------------------------
struct CoSyncEntityState
{
    uint32_t entityID = 0;

    // Lifecycle
    bool hasCreate = false;      // CREATE packet received
    bool spawnQueued = false;    // CREATE queued into spawn system (CRITICAL)
    bool isLocallyOwned = false;

    bool isNPC = false;         
    bool hostAuthoritative = false;

    // Last known packets
    EntityCreatePacket lastCreate{};
    EntityUpdatePacket lastUpdate{};

    // Liveness tracking (local time, seconds)
    double lastUpdateLocalTime = 0.0;
};
