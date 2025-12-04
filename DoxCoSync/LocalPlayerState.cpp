
#include "LocalPlayerState.h"
#include "ConsoleLogger.h"
#include "f4se/GameReferences.h"

// MUST MATCH TickHook offsets
static constexpr uintptr_t kPosX_Offset = 0xD0;
static constexpr uintptr_t kPosY_Offset = 0xD4;
static constexpr uintptr_t kPosZ_Offset = 0xD8;

static constexpr uintptr_t kRotX_Offset = 0xE0;
static constexpr uintptr_t kRotY_Offset = 0xE4;
static constexpr uintptr_t kRotZ_Offset = 0xE8;

// Global instance
LocalPlayerState g_localPlayerState;

// ------------------------------------------------------
// NO velocity reading in LocalPlayerState
// TickHook computes it and sets g_localPlayerState.velocity
// ------------------------------------------------------
void CaptureLocalPlayerState(PlayerCharacter* pc)
{
    if (!pc)
        return;

    uintptr_t base = reinterpret_cast<uintptr_t>(pc);

    // --- Position ---
    g_localPlayerState.position.x = *reinterpret_cast<float*>(base + kPosX_Offset);
    g_localPlayerState.position.y = *reinterpret_cast<float*>(base + kPosY_Offset);
    g_localPlayerState.position.z = *reinterpret_cast<float*>(base + kPosZ_Offset);

    // --- Rotation ---
    g_localPlayerState.rotation.x = *reinterpret_cast<float*>(base + kRotX_Offset);
    g_localPlayerState.rotation.y = *reinterpret_cast<float*>(base + kRotY_Offset);
    g_localPlayerState.rotation.z = *reinterpret_cast<float*>(base + kRotZ_Offset);

    // HEALTH/AP = zero until Phase 7
    g_localPlayerState.health = 0.f;
    g_localPlayerState.maxHealth = 0.f;
    g_localPlayerState.ap = 0.f;
    g_localPlayerState.maxActionPoints = 0.f;

    LOG_DEBUG("[LocalPlayerState] captured pos=(%.2f %.2f %.2f)",
        g_localPlayerState.position.x,
        g_localPlayerState.position.y,
        g_localPlayerState.position.z);
}
