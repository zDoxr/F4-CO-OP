#pragma once

#include <cstdint>
#include "NiTypes.h"
#include "PluginAPI.h"

namespace CoSyncSpawnTasks
{
    // Must be called once during plugin init
    void Init(const F4SETaskInterface* taskInterface);

    // Safe to call from ANY thread
    void EnqueueSpawn(
        uint32_t entityID,
        uint32_t baseFormID,
        const NiPoint3& spawnPos,
        const NiPoint3& spawnRot
    );
}
