#pragma once

#include <cstdint>

// -----------------------------------------------------------------------------
// CoSyncEntityType
// -----------------------------------------------------------------------------
// PURPOSE:
// - Describes the authoritative runtime class of a networked entity
// - Mirrors F4MP-style entity separation
//
// RULES (IMPORTANT):
// - Values are STABLE and NETWORK-SERIALIZED
// - DO NOT reorder existing values
// - DO NOT reuse numeric IDs
// -----------------------------------------------------------------------------
enum class CoSyncEntityType : uint8_t
{
    Invalid = 0,   // Reserved / uninitialized (never spawn)

    Player = 1,   // Remote human-controlled player
    NPC = 2,   // AI-controlled actor (host authoritative)

    
};
