#pragma once

#include <cstdint>
#include "NiTypes.h"
#include "PluginAPI.h"
#include "Packets_EntityCreate.h"

namespace CoSyncSpawnTasks
{
    // Must be called once during plugin init
    void Init(const F4SETaskInterface* taskInterface);

    // Safe to call from ANY thread
    void EnqueueSpawn(
        uint32_t entityID,
        uint32_t baseFormID,
        CoSyncEntityType createType,
        uint32_t spawnFlags,
        const NiPoint3& spawnPos,
        const NiPoint3& spawnRot
    );
}
