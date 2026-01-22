#pragma once

#include "NiTypes.h"
#include "GameReferences.h"   // Actor
#include "GameForms.h"        // TESNPC

// -----------------------------------------------------------------------------
// CoSyncGameAPI
//
// HARD ENGINE BOUNDARY (LOCKED)
//
// RULES:
//   - GAME THREAD ONLY
//   - NEVER write actor->pos / rot directly
//   - ALWAYS use engine / Papyrus calls
//   - NiPoint3 MUST be explicitly initialized
// -----------------------------------------------------------------------------
namespace CoSyncGameAPI
{
    // -----------------------------------------------------------------
    // Spawning (NPC base only)
    // -----------------------------------------------------------------
    Actor* SpawnRemoteActor(TESNPC* npcBase);

    // -----------------------------------------------------------------
    // Transform
    // -----------------------------------------------------------------
    void PositionRemoteActor(
        Actor* actor,
        const NiPoint3& pos,
        const NiPoint3& rot
    );

    bool GetActorWorldTransform(
        Actor* actor,
        NiPoint3& outPos,
        NiPoint3& outRot
    );

    // -----------------------------------------------------------------
    // AI control (NPCs only)
    // -----------------------------------------------------------------
    void DisableActorAI(Actor* actor);
    void EnableActorAI(Actor* actor);
}
