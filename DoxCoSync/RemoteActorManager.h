#pragma once
#include "LocalPlayerState.h"

class Actor;

namespace RemoteActors
{
    // Called whenever we get a fresh remote player state from the network
    void OnRemotePlayerState(const LocalPlayerState& remote);

    // Optional: explicit shutdown / cleanup
    void Shutdown();
}
