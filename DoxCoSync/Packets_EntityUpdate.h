#pragma once

#include <cstdint>

#include "NiTypes.h"   // NiPoint3

// -----------------------------------------------------------------------------
// EntityUpdatePacket
//
// Sent CLIENT → HOST → ALL
//
// Rules (F4MP-aligned):
//  • UPDATE NEVER causes spawning
//  • UPDATE applies ONLY to existing entities
//  • UPDATE is high-frequency and MUST be lightweight
// -----------------------------------------------------------------------------
struct EntityUpdatePacket
{
    // -----------------------------------------------------------------
    // Identity
    // -----------------------------------------------------------------

    // Authoritative entity identifier
    uint32_t entityID = 0;

    // -----------------------------------------------------------------
    // Update behavior flags
    // -----------------------------------------------------------------
    enum UpdateFlags : uint32_t
    {
        None = 0,
        Teleport = 1 << 0, // snap immediately (no smoothing)
        NoRotation = 1 << 1, // ignore rot
        NoVelocity = 1 << 2, // vel not meaningful
    };

    uint32_t flags = None;

    // -----------------------------------------------------------------
    // Transform state (ENGINE NATIVE)
    // -----------------------------------------------------------------

    NiPoint3 pos{ 0.f, 0.f, 0.f };
    NiPoint3 rot{ 0.f, 0.f, 0.f };
    NiPoint3 vel{ 0.f, 0.f, 0.f };

    // -----------------------------------------------------------------
    // Timing
    // -----------------------------------------------------------------

    // Sender-local monotonic timestamp (seconds)
    // Used ONLY for interpolation ordering, never authority
    double timestamp = 0.0;

    // -----------------------------------------------------------------
    // Reserved (future-proofing)
    // -----------------------------------------------------------------
    uint32_t reserved0 = 0;
};
