#pragma once

#include "GameReferences.h"
#include "GameObjects.h"
#include "NiTypes.h"

class LocalPlayerState;

namespace CoSyncGameAPI
{
    // Functions
    Actor* SpawnRemoteActor(UInt32 baseFormID);

    void PositionRemoteActor(
        Actor* actor,
        const NiPoint3& pos,
        const NiPoint3& rot
    );

    void ApplyRemotePlayerStateToActor(
        Actor* actor,
        const LocalPlayerState& state
    );

    // Globals (DECLARATIONS ONLY)
    extern NiPoint3 rot;
    extern NiPoint3 pos;
}
