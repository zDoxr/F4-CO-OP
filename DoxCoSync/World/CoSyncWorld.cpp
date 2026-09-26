#include "CoSyncWorld.h"

#include "GameReferences.h"
#include "Relocation.h"
#include "CoSyncRequirementsLog.h"


extern RelocPtr<PlayerCharacter*> g_player;
extern PlayerCharacter* CoSyncGetPlayer();

namespace CoSyncWorld
{
    bool IsWorldReady()
    {
        LOG_REQUIREMENT("CoSyncWorld::IsWorldReady", "Safe to call any time. It checks for a valid player and cell on its own before saying the world is ready.");
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc)
            return false;

        TESObjectCELL* cell = pc->parentCell;
        if (!cell)
            return false;


        if (cell->formID == 0)
            return false;

        return true;
    }
}
