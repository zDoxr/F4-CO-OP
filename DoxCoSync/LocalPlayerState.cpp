#include "LocalPlayerState.h"
#include "ConsoleLogger.h"
#include "LocalPlayerStateGlobals.h"
#include "f4se/GameReferences.h"
#include "f4se_common/Utilities.h"
#include "f4se/GameTypes.h"
#include "f4se_common/Relocation.h"

// ---------------------------------------------------------------------------
// CaptureLocalPlayerState
// Called one-time after world load OR whenever a full static refresh is needed.
// TickHook updates all dynamic fields every frame.
// ---------------------------------------------------------------------------
void CaptureLocalPlayerState(PlayerCharacter* pc)
{
    if (!pc)
        return;

    // ----------------------------------------------------
    // Identity (username is assigned externally)
    // ----------------------------------------------------

    // ----------------------------------------------------
    // Base form and cell
    // ----------------------------------------------------
    g_localPlayerState.formID = pc->formID;
    g_localPlayerState.cellFormID = pc->parentCell ? pc->parentCell->formID : 0;

    // ----------------------------------------------------
    // Transform
    // ----------------------------------------------------
    g_localPlayerState.position = pc->pos;
    g_localPlayerState.rotation = pc->rot;

    // Velocity is handled in TickHook — do not overwrite here.

    // ----------------------------------------------------
    // Stats (TickHook or future systems can fill these)
    // ----------------------------------------------------
    // g_localPlayerState.health
    // g_localPlayerState.maxHealth
    // g_localPlayerState.ap
    // g_localPlayerState.maxActionPoints

    // ----------------------------------------------------
    // Movement Flags (reset; TickHook updates them)
    // ----------------------------------------------------
    g_localPlayerState.isMoving = false;
    g_localPlayerState.isSprinting = false;
    g_localPlayerState.isCrouching = false;
    g_localPlayerState.isJumping = false;

    // ----------------------------------------------------
    // Equipped weapon — TickHook updates this later
    // ----------------------------------------------------
    // g_localPlayerState.equippedWeaponFormID

    LOG_DEBUG("[LocalPlayerState] captured pos=(%.2f %.2f %.2f) rot=(%.2f %.2f %.2f)",
        g_localPlayerState.position.x,
        g_localPlayerState.position.y,
        g_localPlayerState.position.z,
        g_localPlayerState.rotation.x,
        g_localPlayerState.rotation.y,
        g_localPlayerState.rotation.z);
}
