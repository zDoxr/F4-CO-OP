#pragma once

#include <cstdint>
#include "NiTypes.h"
#include "CoSyncEntityTypes.h"

// Provided by F4SE (Tasks.h)
struct F4SETaskInterface;

namespace CoSyncSpawnTasks
{
    void Init(const F4SETaskInterface* taskInterface);

    void EnqueueSpawn(
        uint32_t entityID,
        uint32_t baseFormID,
        CoSyncEntityType createType,
        uint32_t spawnFlags,          // kept for signature compatibility
        const NiPoint3& spawnPos,
        const NiPoint3& spawnRot
    );
}
