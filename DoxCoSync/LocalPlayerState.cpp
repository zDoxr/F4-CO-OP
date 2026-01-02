#include "LocalPlayerState.h"
#include "LocalPlayerStateGlobals.h"
#include "ConsoleLogger.h"
#include "GameReferences.h"
#include "Relocation.h"


// ---------------------------------------------------------------------------
// CaptureLocalPlayerState
//
// PURPOSE:
//   Capture slow-changing / identity data for the local player.
//   This is NOT called every frame.
//
// CALLED:
//   - After world load
//   - On reconnect / full resync
//
// DOES NOT:
//   - Update transforms
//   - Update velocity
//   - Update movement flags
// ---------------------------------------------------------------------------
void CaptureLocalPlayerState(PlayerCharacter* pc)
{
    if (!pc)
    {
        LOG_WARN("[LocalPlayerState] Capture failed: PlayerCharacter is null");
        return;
    }

    // Identity
    g_localPlayerState.formID = pc->formID;

    if (pc->parentCell)
        g_localPlayerState.cellFormID = pc->parentCell->formID;
    else
        g_localPlayerState.cellFormID = 0;

    // Stats intentionally NOT populated yet.
    // These will be added later when replication rules are defined.
    g_localPlayerState.health = 0.f;
    g_localPlayerState.maxHealth = 0.f;
    g_localPlayerState.ap = 0.f;
    g_localPlayerState.maxActionPoints = 0.f;

    LOG_INFO(
        "[LocalPlayerState] Captured static state formID=0x%08X cellID=0x%08X",
        g_localPlayerState.formID,
        g_localPlayerState.cellFormID
    );
}
