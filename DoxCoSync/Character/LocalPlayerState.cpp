#include "LocalPlayerState.h"
#include "LocalPlayerStateGlobals.h"
#include "CoSyncRequirementsLog.h"
#include "ConsoleLogger.h"
#include "GameReferences.h"
#include "Relocation.h"

void CaptureLocalPlayerState(PlayerCharacter* pc)
{
    LOG_REQUIREMENT("CaptureLocalPlayerState", "Needs a valid player character to read from. It's fine if the player isn't in a cell yet.");
    if (!pc)
    {
        LOG_WARN("[LocalPlayerState] Couldn't save player state -- no player character yet");
        return;
    }


    g_localPlayerState.formID = pc->formID;

    if (pc->parentCell)
        g_localPlayerState.cellFormID = pc->parentCell->formID;
    else
        g_localPlayerState.cellFormID = 0;

    g_localPlayerState.health = 0.f;
    g_localPlayerState.maxHealth = 0.f;
    g_localPlayerState.ap = 0.f;
    g_localPlayerState.maxActionPoints = 0.f;

    LOG_INFO(
        "[LocalPlayerState] Saved player state -- formID=0x%08X cellID=0x%08X",
        g_localPlayerState.formID,
        g_localPlayerState.cellFormID
    );
}
