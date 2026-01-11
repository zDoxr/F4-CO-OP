#pragma once

#include "NiTypes.h"
#include "GameReferences.h"   // Actor
#include "GameForms.h"        // TESNPC

namespace CoSyncGameAPI
{
    // Spawns a remote NPC actor using PlaceAtMe (F4MP style)
    Actor* SpawnRemoteActor(TESNPC* npcBase);

    // Applies position + rotation to an existing actor
    void PositionRemoteActor(
        Actor* actor,
        const NiPoint3& pos,
        const NiPoint3& rot
    );


    void DisableActorAI(Actor* actor);
    void EnableActorAI(Actor* actor);


}



