#pragma once

#include <string>
#include "GameReferences.h"
#include "LocalPlayerState.h"

class CoSyncPlayer
{
public:
    std::string username;
    LocalPlayerState lastState;

    Actor* actorRef = nullptr;
    bool hasSpawned = false;

    double lastPacketTime = 0.0;

    CoSyncPlayer(const std::string& name);

    void UpdateFromState(const LocalPlayerState& state);
    bool SpawnInWorld(TESObjectREFR* anchor, TESForm* baseForm);
    
    void ApplyStateToActor();
};
