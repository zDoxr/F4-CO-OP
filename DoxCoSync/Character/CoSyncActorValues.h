#pragma once

#include "Relocation.h"

class Actor;
class ActorValueInfo;

static constexpr uint32_t kAVI_Strength      = 0x000002C2;
static constexpr uint32_t kAVI_Perception    = 0x000002C3;
static constexpr uint32_t kAVI_Endurance     = 0x000002C4;
static constexpr uint32_t kAVI_Charisma      = 0x000002C5;
static constexpr uint32_t kAVI_Intelligence  = 0x000002C6;
static constexpr uint32_t kAVI_Agility       = 0x000002C7;
static constexpr uint32_t kAVI_Luck          = 0x000002C8;


static constexpr uint32_t kAVI_Health        = 0x000002D4;
static constexpr uint32_t kAVI_ActionPoints  = 0x000002D5;
static constexpr uint32_t kAVI_SpeedMult     = 0x000002DA;
static constexpr uint32_t kAVI_CarryWeight   = 0x000002DC;
static constexpr uint32_t kAVI_Rads          = 0x000002E1;

typedef void (*_Actor_SetActorValue)(
    Actor* actor,
    ActorValueInfo* av,
    float value
    );

extern RelocAddr<_Actor_SetActorValue> Actor_SetActorValue;
