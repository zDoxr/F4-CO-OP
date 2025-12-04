#pragma once

#include <cstdint>
#include "LocalPlayerState.h"
#include "f4se/GameReferences.h"

namespace RemotePlayers
{
    void Init();
    void Shutdown();
    void ApplyRemoteState(uint64_t remoteID, const LocalPlayerState& state);

    Actor* EnsureActor(uint64_t remoteID);
    void Remove(uint64_t remoteID);
}
