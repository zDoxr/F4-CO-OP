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
//
// IMPORTANT:
//  - Host is authoritative for ALL fields in this packet
//  - Clients MUST NOT reinterpret or mutate CREATE semantics
// -----------------------------------------------------------------------------
struct EntityCreatePacket
{
    // -----------------------------------------------------------------
    // Identity (HOST-AUTHORITATIVE)
    // -----------------------------------------------------------------

    // Unique authoritative entity ID (assigned by host)
    uint32_t entityID = 0;

    // Entity classification (Player / NPC / etc.)
    // MUST be trusted exactly as sent by host
    CoSyncEntityType type = CoSyncEntityType::Player;

    // ActorBase / NPC_ form ID used for spawning
    uint32_t baseFormID = 0;

    // Optional: entity that owns / controls this one
    // 0 = none / world-owned
    uint32_t ownerEntityID = 0;

    // -----------------------------------------------------------------
    // Spawn behavior flags (bitmask, HOST-AUTHORITATIVE)
    // -----------------------------------------------------------------
    enum SpawnFlags : uint32_t
    {
        None = 0,
        RemoteControlled = 1 << 0, // receives UPDATE packets
        HiddenOnSpawn = 1 << 1, // spawn hidden until first UPDATE
        Persistent = 1 << 2, // do not auto-despawn
    };

    // Default semantics:
    //  - Remote-controlled proxy
    //  - Hidden until first authoritative UPDATE
    uint32_t spawnFlags = HiddenOnSpawn | RemoteControlled;

    // Helper (optional but recommended)
    static bool HasFlag(uint32_t flags, SpawnFlags f)
    {
        return (flags & static_cast<uint32_t>(f)) != 0;
    }

    // -----------------------------------------------------------------
    // Initial spawn transform (ENGINE NATIVE)
    //
    // Used ONLY for initial placement.
    // UPDATE packets take over movement after spawn.
    // -----------------------------------------------------------------

    // Position in world space
    NiPoint3 spawnPos{ 0.f, 0.f, 0.f };

    // Rotation in RADIANS
    NiPoint3 spawnRot{ 0.f, 0.f, 0.f };

    // -----------------------------------------------------------------
    // Reserved (future-proofing; must be preserved if serialized)
    // -----------------------------------------------------------------
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
};
