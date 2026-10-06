#include "CoSyncWorld.h"

#include "GameReferences.h"
#include "Relocation.h"


extern RelocPtr<PlayerCharacter*> g_player;
extern PlayerCharacter* CoSyncGetPlayer();

namespace CoSyncWorld
{
    bool IsWorldReady()
    {
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
