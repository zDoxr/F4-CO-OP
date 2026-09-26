#pragma once
#include "LocalPlayerState.h"

namespace RemoteActorManager
{
    void Init();
    void Shutdown();


    void OnRemotePlayerState(const LocalPlayerState& st);


    void Tick(float dt);


    void Remove(uint64_t steamID);
}
