#pragma once

#include "f4se/NiTypes.h"
#include <string>

class LocalPlayerState
{
public:
    std::string username;

    NiPoint3 position;
    NiPoint3 rotation;
    NiPoint3 velocity;

    bool isMoving = false;
    bool isSprinting = false;
    bool isCrouching = false;
    bool isJumping = false;

    float health = 0.0f;
    float maxHealth = 0.0f;
    float ap = 0.0f;
    float maxActionPoints = 0.0f;

    UInt32 formID = 0;
    UInt32 cellFormID = 0;

    // REQUIRED BY PlayerStatePacket
    UInt32 equippedWeaponFormID = 0;
    UInt32 leftHandFormID = 0;
    UInt32 rightHandFormID = 0;

    // REQUIRED BY CoSyncPlayerManager
    double timestamp = 0.0;
};
