#pragma once
#include "GameReferences.h"

extern RelocPtr<PlayerCharacter*> g_player;

namespace CoSyncWorld
{
    inline bool IsWorldReady()
    {
        auto* p = *g_player;
        return p && p->parentCell && p->parentCell->formID != 0;
    }
}
