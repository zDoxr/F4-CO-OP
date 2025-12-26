#pragma once

#include "Relocation.h"

class Actor;
class ActorValueInfo;

// Engine: lookup ActorValueInfo by editor name
typedef ActorValueInfo* (*_GetActorValueInfoByName)(const char* name);

// Engine: set actor value directly
typedef void (*_Actor_SetActorValue)(
    Actor* actor,
    ActorValueInfo* av,
    float value
    );

extern RelocAddr<_GetActorValueInfoByName> GetActorValueInfoByName;
extern RelocAddr<_Actor_SetActorValue> Actor_SetActorValue;
