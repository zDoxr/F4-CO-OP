#pragma once

#include <cstdint>

#include "ITypes.h"          // UInt32
#include "NiTypes.h"         // NiPoint3
#include "GameReferences.h"  // Actor

// NOTE:
// - SpawnRemoteActor is executed on the game thread (via F4SE task).
// - Uses PlaceAtMe_Native.
// - No engine hooks / internal callees.

namespace CoSyncGameAPI
{
    Actor* SpawnRemoteActor(UInt32 baseFormID);

    void PositionRemoteActor(
        Actor* actor,
        const NiPoint3& pos,
        const NiPoint3& rot
    );
}
