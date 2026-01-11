#pragma once

#include <cstdint>

#include "NiTypes.h"            // NiPoint3
#include "CoSyncEntityTypes.h"

// -----------------------------------------------------------------------------
// EntityCreatePacket
//
// Sent HOST → ALL
// Defines the existence of an entity.
// This is the ONLY packet that may cause spawning.
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

    // Optional: entity that owns / controls this one
    // 0 = none / world-owned
    uint32_t ownerEntityID = 0;

    // -----------------------------------------------------------------
    // Spawn behavior flags (F4MP-aligned)
    // -----------------------------------------------------------------

    enum SpawnFlags : uint32_t
    {
        None = 0,
        RemoteControlled = 1 << 0, // receives UPDATE packets
        HiddenOnSpawn = 1 << 1, // spawn hidden until first UPDATE
        Persistent = 1 << 2, // do not auto-despawn
    };

    uint32_t spawnFlags = HiddenOnSpawn | RemoteControlled;

    // -----------------------------------------------------------------
    // Initial spawn transform (ENGINE NATIVE)
    // NOTE:
    //  - Used only for initial placement
    //  - UPDATE packets take over after spawn
    // -----------------------------------------------------------------

    // Position in world space
    NiPoint3 spawnPos{ 0.f, 0.f, 0.f };

    // Rotation in RADIANS
    NiPoint3 spawnRot{ 0.f, 0.f, 0.f };

    // -----------------------------------------------------------------
    // Reserved (future-proofing)
    // -----------------------------------------------------------------
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
};
