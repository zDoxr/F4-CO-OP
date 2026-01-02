#pragma once

#include "GameReferences.h"
#include "Relocation.h"

// -----------------------------------------------------------------------------
// Global player reference (provided by F4SE / common)
// -----------------------------------------------------------------------------
extern RelocPtr<PlayerCharacter*> g_player;

// -----------------------------------------------------------------------------
// CoSyncWorld
// Purpose (F4MP-aligned):
//   Gate *ALL* spawning and world mutations until the game world is fully valid.
//   This MUST be conservative — false negatives are acceptable,
//   false positives are NOT.
// -----------------------------------------------------------------------------
namespace CoSyncWorld
{
    inline bool IsWorldReady()
    {
        PlayerCharacter* pc = *g_player;
        if (!pc)
            return false;

        // Must have a valid loaded cell
        if (!pc->parentCell)
            return false;

        // Cell must be a real runtime cell
        if (pc->parentCell->formID == 0)
            return false;

        // Worldspace check (important for exterior cells)
        // F4MP implicitly relies on this via engine behavior
        if (!CALL_MEMBER_FN(pc, GetWorldspace)())
            return false;

        return true;
    }
}
