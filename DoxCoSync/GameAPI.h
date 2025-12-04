#pragma once

#include "f4se/NiTypes.h"
#include "f4se/GameReferences.h"
#include "LocalPlayerState.h"

namespace GameAPI
{
    PlayerCharacter* GetPlayer();

    Actor* SpawnActor(UInt32 formID);

    void MoveActorTo(Actor* actor, const NiPoint3& pos, const NiPoint3& rot);

    void SetActorHealth(Actor* actor, float value, float maxValue);

    void ApplyImpulse(Actor* actor, const NiPoint3& velocity);

    
    void ApplyRemotePlayerStateToActor(Actor* actor, const LocalPlayerState& state);

    //wrapper
    void ApplyRemotePlayerState(const LocalPlayerState& state);
}
