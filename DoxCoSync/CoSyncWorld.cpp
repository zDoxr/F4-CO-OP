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

        // IMPORTANT:
        // Interiors often have no worldspace, so do NOT fail readiness just because GetWorldspace() is null.
        // We only require worldspace if the cell is exterior.
        //if (cell->IsInterior() == false)
        //{
            // Exterior: must have a worldspace
          //  if (!CALL_MEMBER_FN(pc, GetWorldspace)())
            //    return false;
        //}

        return true;
    }
}
