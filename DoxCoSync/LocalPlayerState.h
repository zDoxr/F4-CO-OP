#pragma once

#include <cstdint>
#include "f4se/NiTypes.h"        // NiPoint3
#include "f4se/GameReferences.h" // PlayerCharacter

// ========================================================
// Full replicated state used by TickHook, CoSyncNet, and
// RemotePlayers. Must match PlayerStatePacket serialize!
// ========================================================
struct LocalPlayerState
{
    // Identity
    uint32_t formID = 0;
    uint32_t cellFormID = 0;
    uint64_t steamID = 0; // optional but used later

    // Transform
    NiPoint3 position;
    NiPoint3 rotation;
    NiPoint3 velocity;

    // Base stats
    float health = 0.0f;
    float maxHealth = 0.0f;

    float ap = 0.0f;
    float maxActionPoints = 0.0f;

    // Movement flags
    bool isMoving = false;
    bool isSprinting = false;
    bool isCrouching = false;
    bool isJumping = false;

    // Weapon
    uint32_t equippedWeaponFormID = 0;
};

// Global instance written every tick by TickHook
extern LocalPlayerState g_localPlayerState;

// Capture local player into LocalPlayerState (optional helper)
void CaptureLocalPlayerState(PlayerCharacter* pc);
