#pragma once

#include <string>
#include "NiTypes.h"
#include "ITypes.h"

// NOTE:
// - position, rotation, velocity use engine-native units
// - rotation is ALWAYS radians (never degrees)
// - this struct represents the authoritative local snapshot

struct LocalPlayerState
{
    // -------------------------
    // Identity
    // -------------------------
    std::string username;

    // -------------------------
    // Transform (ENGINE NATIVE)
    // -------------------------
    NiPoint3 position{ 0.f, 0.f, 0.f };

    // rotation is in RADIANS (engine-native)
    NiPoint3 rotation{ 0.f, 0.f, 0.f };

    NiPoint3 velocity{ 0.f, 0.f, 0.f };

    // -------------------------
    // Movement flags
    // -------------------------
    bool isMoving = false;
    bool isSprinting = false;
    bool isCrouching = false;
    bool isJumping = false;

    // -------------------------
    // World identity
    // -------------------------
    UInt32 formID = 0;
    UInt32 cellFormID = 0;

    // -------------------------
    // Stats (optional / future)
    // -------------------------
    float health = 0.f;
    float maxHealth = 0.f;
    float ap = 0.f;
    float maxActionPoints = 0.f;

    // -------------------------
    // Timestamp (seconds)
    // -------------------------
    double timestamp = 0.0;
};
