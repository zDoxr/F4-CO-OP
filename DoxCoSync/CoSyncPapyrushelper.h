
//
#pragma once

#include <cstdint>

class TESQuest;
class Actor;

namespace CoSyncPapyrusHelper
{
    // Initialize the Papyrus helper (finds the CoSyncQuest)
    void Init();

    // Call Papyrus: CoSyncQuest.SpawnRemotePlayer(entityID)
    void SpawnRemotePlayer(uint32_t entityID);

    // Call Papyrus: CoSyncQuest.DespawnRemotePlayer(entityID)
    void DespawnRemotePlayer(uint32_t entityID);

    // Call Papyrus: CoSyncQuest.GetRemotePlayerActor(entityID) -> Actor
    Actor* GetRemotePlayerActor(uint32_t entityID);

    // Get the CoSyncQuest form
    TESQuest* GetCoSyncQuest();
}