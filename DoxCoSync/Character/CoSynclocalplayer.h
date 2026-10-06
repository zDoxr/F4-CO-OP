#pragma once

#include "NiTypes.h"

class Actor;

namespace CoSyncLocalPlayer
{

    void Init();


    void Shutdown();

    void Tick(double now);


    Actor* GetLocalPlayerActor();

    // True from the moment a real (debounced) death is detected until a
    // respawn has been confirmed by several consecutive positive HP
    // readings. Single source of truth for other systems (e.g. TickHook.cpp)
    // that need to know "is it safe to touch this actor's graph/engine
    // state right now" without running their own independent HP poll.
    bool IsLocallyConfirmedDead();

    // 0.5.1: a save finished loading (F4SE PostLoadGame, any thread). The
    // next tick drops any downed/dead state the load replaced, tells peers
    // this player is up, and re-asserts the session's essential flag.
    void OnGameLoaded();


    void ForceUpdate();


    bool GetLocalTransform(NiPoint3& outPos, NiPoint3& outRot);


    void SetUpdateRate(float updatesPerSecond);
    void SetMovementThreshold(float distance);
    void SetRotationThreshold(float radians);
}
