#pragma once

#include <cstdint>
#include <string>

#include "Packets_EntityCreate.h"
#include "Packets_EntityUpdate.h"

// -----------------------------------------------------------------------------
// CoSyncEntityState
// F4MP-aligned: authoritative network state for an entity.
// This is NOT a world object and must never touch the engine.
// -----------------------------------------------------------------------------
struct CoSyncEntityState
{
    uint32_t entityID = 0;

    // Identity / debug
    std::string username;

    // Last known CREATE data (if ever received)
    bool hasCreate = false;
    EntityCreatePacket lastCreate{};

    // Last known UPDATE data (may exist before CREATE)
    bool hasUpdate = false;
    EntityUpdatePacket lastUpdate{};

    // Simple timing/telemetry
    double lastUpdateLocalTime = 0.0; // local clock (game thread)
};
