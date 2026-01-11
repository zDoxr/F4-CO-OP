#include "CoSyncWorld.h"

#include "GameReferences.h"
#include "Relocation.h"

// Provided by F4SE
extern RelocPtr<PlayerCharacter*> g_player;

namespace CoSyncWorld
{
    bool IsWorldReady()
    {
        PlayerCharacter* pc = *g_player;
        if (!pc)
            return false;

        TESObjectCELL* cell = pc->parentCell;
        if (!cell)
            return false;

        // Must be a real, loaded runtime cell
        if (cell->formID == 0)
            return false;

        // Exterior cells must have a worldspace
        if (!CALL_MEMBER_FN(pc, GetWorldspace)())
            return false;

        return true;
    }
}