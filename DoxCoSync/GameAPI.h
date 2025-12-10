#pragma once

#include "f4se/NiTypes.h"
#include "f4se/GameReferences.h"
#include "f4se/GameObjects.h"

// Forward
class LocalPlayerState;

namespace GameAPI
{
    // Spawn remote actor from a baseFormID
    Actor* SpawnRemoteActor(UInt32 baseFormID);

    // Writes Actor position + rotation
    void PositionRemoteActor(Actor* actor, const NiPoint3& pos, const NiPoint3& rot);

    // Applies full LocalPlayerState (pos + rot only for now)
    void ApplyRemotePlayerStateToActor(Actor* actor, const LocalPlayerState& state);

    // Position only helper (pos only)
    void NativeMoveTo(Actor* actor, const NiPoint3& pos);

    // For old naming compatibility
    void SetActorTransform(Actor* actor, const NiPoint3& pos, const NiPoint3& rot);
}

// Global convenience wrapper
inline void NativeMoveTo(Actor* actor, const NiPoint3& pos)
{
    GameAPI::NativeMoveTo(actor, pos);
}
