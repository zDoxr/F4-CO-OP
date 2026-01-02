#pragma once
#include <cstdint>
#include <string>

#include "NiTypes.h"   // NiPoint3

// -----------------------------------------------------------------------------
// EntityUpdatePacket
// Purpose:
//   Continuous replicated state for an already-existing entity
//   MUST NEVER cause spawning
// -----------------------------------------------------------------------------
struct EntityUpdatePacket
{
    // Authoritative entity identifier
    uint32_t entityID = 0;

    // Optional human-readable label (debug / UI only)
    // NOT used for identity or authority
    std::string username;

    // Transform state
    NiPoint3 pos{ 0.f, 0.f, 0.f };
    NiPoint3 rot{ 0.f, 0.f, 0.f };
    NiPoint3 vel{ 0.f, 0.f, 0.f };

    // Host-authoritative time (seconds)
    double timestamp = 0.0;
};
