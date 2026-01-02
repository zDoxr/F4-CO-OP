#pragma once

#include <cstdint>

#include "NiTypes.h"            // NiPoint3
#include "CoSyncEntityTypes.h"

// -----------------------------------------------------------------------------
// EntityCreatePacket
// -----------------------------------------------------------------------------
// PURPOSE:
// - Authoritative spawn instruction from HOST → CLIENTS
// - Mirrors F4MP behavior exactly:
//
//   RULES (CRITICAL):
//   - CREATE defines existence
//   - UPDATE never spawns
//   - EntityCreatePacket is sent ONCE per entity lifetime
//   - Rotation is ALWAYS RADIANS (never degrees)
// -----------------------------------------------------------------------------
struct EntityCreatePacket
{
    // -----------------------------------------------------------------
    // Identity
    // -----------------------------------------------------------------

    // Unique authoritative ID (assigned by host)
    uint32_t entityID = 0;

    // What kind of entity this is (Player / NPC / etc.)
    CoSyncEntityType type = CoSyncEntityType::Player;

    // ActorBase / NPC_ form ID used for spawning
    uint32_t baseFormID = 0;

    // Optional: entity that owns/controls this one
    // 0 = none / world-owned
    uint32_t ownerEntityID = 0;

    // -----------------------------------------------------------------
    // Initial spawn transform (ENGINE NATIVE)
    // -----------------------------------------------------------------

    // Position in world space
    NiPoint3 spawnPos{ 0.f, 0.f, 0.f };

    // Rotation in RADIANS
    NiPoint3 spawnRot{ 0.f, 0.f, 0.f };
};
