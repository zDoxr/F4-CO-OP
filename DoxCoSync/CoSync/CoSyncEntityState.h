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

    // 0.4.3: the last time this entity's owner said ANYTHING about it (CREATE
    // re-broadcast, KILL, ENS, HP, CORPSE, EU). A corpse gets no EU at all
    // (its transform is the ragdoll's), so a timeout on EU alone declared every
    // corpse lost after 15 s and the next CREATE re-broadcast built a brand-new
    // body for it that the re-sent KILL then killed again (Bug #2, client log
    // 34756 21:26:18-21:26:27, entities 65555/65556).
    double lastContactLocalTime = 0.0;
};
