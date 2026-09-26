#pragma once

#include <cstdint>
#include <functional>
#include "NiTypes.h"
#include "CoSyncEntityTypes.h"


struct F4SETaskInterface;

namespace CoSyncSpawnTasks
{
    void Init(const F4SETaskInterface* taskInterface);

    void EnqueueSpawn(
        uint32_t entityID,
        uint32_t baseFormID,
        CoSyncEntityType createType,
        uint32_t spawnFlags,
        const NiPoint3& spawnPos,
        const NiPoint3& spawnRot
    );

    // Runs fn on the main thread from F4SE's task queue -- the same point the
    // console runs its commands at. Returns false if the queue isn't ready.
    bool EnqueueMainThread(std::function<void()> fn);
}
