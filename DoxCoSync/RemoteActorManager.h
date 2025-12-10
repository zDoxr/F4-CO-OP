#pragma once
#include "LocalPlayerState.h"

namespace RemoteActorManager
{
    void Init();
    void Shutdown();

    // Called when network receives a state packet
    void OnRemotePlayerState(const LocalPlayerState& st);

    // Called each tick from TickHook
    void Tick(float dt);

    // Remove player on disconnect
    void Remove(uint64_t steamID);
}
