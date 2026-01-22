// CoSyncLocalPlayer.h
// Tracks and broadcasts the local player's movement to the network
#pragma once

#include "NiTypes.h"

class Actor;

namespace CoSyncLocalPlayer
{
    // Initialize the local player tracking system
    void Init();

    // Shutdown the local player tracking system
    void Shutdown();

    // Per-frame update (sends position updates to network)
    // Should be called from CoSyncRuntime tick
    void Tick(double now);

    // Get the local player actor
    Actor* GetLocalPlayerActor();

    // Force an immediate position update (useful for teleports/loading)
    void ForceUpdate();

    // Get current local player position/rotation
    bool GetLocalTransform(NiPoint3& outPos, NiPoint3& outRot);

    // Configuration
    void SetUpdateRate(float updatesPerSecond);  // Default: 20 Hz
    void SetMovementThreshold(float distance);    // Default: 0.01 units
    void SetRotationThreshold(float radians);     // Default: 0.01 radians
}
