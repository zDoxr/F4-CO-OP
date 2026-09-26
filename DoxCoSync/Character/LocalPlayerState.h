#pragma once

#include <string>
#include "NiTypes.h"
#include "ITypes.h"

struct LocalPlayerState
{

    std::string username;

    NiPoint3 position{ 0.f, 0.f, 0.f };


    NiPoint3 rotation{ 0.f, 0.f, 0.f };

    NiPoint3 velocity{ 0.f, 0.f, 0.f };

    bool isMoving = false;
    bool isSprinting = false;
    bool isCrouching = false;
    bool isJumping = false;

    UInt32 formID = 0;
    UInt32 cellFormID = 0;

    float health = 0.f;
    float maxHealth = 0.f;
    float ap = 0.f;
    float maxActionPoints = 0.f;

    double timestamp = 0.0;
};
