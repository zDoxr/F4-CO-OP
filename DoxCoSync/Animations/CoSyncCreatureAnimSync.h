#pragma once

#include "CoSyncAnimVarSystem.h"

class Actor;

static constexpr uint64_t kCreatureSnapshotFlag = (1ULL << 63);

namespace CoSyncCreatureAnimSync
{

    static constexpr uint32_t kHumanoidMinSize = 290;


    bool IsCreatureBehavior(uint32_t varSetSize);

    bool SaveCreatureVars(Actor* actor, AnimVarSnapshot& outSnapshot);

    bool LoadCreatureVars(Actor* actor, const AnimVarSnapshot& snapshot);
}
