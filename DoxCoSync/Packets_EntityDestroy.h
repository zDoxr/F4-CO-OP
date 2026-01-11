#pragma once

#include <cstdint>

// -----------------------------------------------------------------------------
// EntityDestroyPacket
//
// Sent HOST -> ALL
// Removes an existing entity from the session.
// -----------------------------------------------------------------------------
struct EntityDestroyPacket
{
    // Authoritative entity identifier
    uint32_t entityID = 0;

    // Optional reason flags (future-proofing)
    uint32_t reasonFlags = 0;
};