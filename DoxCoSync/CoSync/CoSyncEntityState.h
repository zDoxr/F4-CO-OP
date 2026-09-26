#pragma once

#include <cstdint>

#include "Packets_EntityCreate.h"
#include "Packets_EntityUpdate.h"

struct CoSyncEntityState
{
    uint32_t entityID = 0;


    bool hasCreate = false;
    bool spawnQueued = false;
    bool isLocallyOwned = false;

    bool isNPC = false;
    bool hostAuthoritative = false;


    EntityCreatePacket lastCreate{};
    EntityUpdatePacket lastUpdate{};


    double lastUpdateLocalTime = 0.0;
    double lastSpawnAttemptTime = 0.0;
};
