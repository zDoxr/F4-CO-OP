#include "CoSyncActorValueSafety.h"

#include "CoSyncRequirementsLog.h"
#include "ConsoleLogger.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameObjects.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// __declspec(noinline) and the absence of `noexcept` are both load-bearing
// under this project's /GL+/LTCG Release config -- see CoSyncDoorHook.cpp's
// SafeReadU32/SafeReadPtr and CoSyncLocomotionCalcGuard.cpp's
// CallOriginalSafe for the same lesson learned repeatedly this project:
// either one missing lets the optimizer silently strip the __try/__except
// scope, letting the fault escape uncaught.
namespace CoSyncAVSafety
{

__declspec(noinline) bool SafeGetValue(Actor* actor, ActorValueInfo* avi, float& out)
{
    LOG_REQUIREMENT("CoSyncAVSafety::SafeGetValue", "Needs a valid actor and stat type. Wrapped in a safety guard, since a proxy player with AI turned off may be missing the piece of data this reads.");
    if (!actor || !avi) return false;
    __try
    {
        out = actor->actorValueOwner.GetValue(avi);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[AVSafety] GetValue crashed actor=%p avi=%p", actor, avi);
        return false;
    }
}

__declspec(noinline) bool SafeGetMaximum(Actor* actor, ActorValueInfo* avi, float& out)
{
    LOG_REQUIREMENT("CoSyncAVSafety::SafeGetMaximum", "Needs a valid actor and stat type. Wrapped in a safety guard, since a proxy player with AI turned off may be missing the piece of data this reads.");
    if (!actor || !avi) return false;
    __try
    {
        out = actor->actorValueOwner.GetMaximum(avi);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[AVSafety] GetMaximum crashed actor=%p avi=%p", actor, avi);
        return false;
    }
}

__declspec(noinline) bool SafeGetBase(Actor* actor, ActorValueInfo* avi, float& out)
{
    LOG_REQUIREMENT("CoSyncAVSafety::SafeGetBase", "Needs a valid actor and stat type. Wrapped in a safety guard, since a proxy player with AI turned off may be missing the piece of data this reads.");
    if (!actor || !avi) return false;
    __try
    {
        out = actor->actorValueOwner.GetBase(avi);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[AVSafety] GetBase crashed actor=%p avi=%p", actor, avi);
        return false;
    }
}

__declspec(noinline) bool SafeSetBase(Actor* actor, ActorValueInfo* avi, float value)
{
    LOG_REQUIREMENT("CoSyncAVSafety::SafeSetBase", "Needs a valid actor and stat type. Wrapped in a safety guard, since a proxy player with AI turned off may be missing the piece of data this writes to.");
    if (!actor || !avi) return false;
    __try
    {
        actor->actorValueOwner.SetBase(avi, value);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[AVSafety] SetBase crashed actor=%p avi=%p value=%.2f", actor, avi, value);
        return false;
    }
}

// ForceCurrent(ForceMode::DAMAGE=2, avi, delta) — ActorValueOwner vtable slot 6,
// IDA-verified 2026-08-02 (Actor AVO vtable rva 0x2560DB8, slot6 sub_140CC97B0:
// `add rcx,-0x58` [AVO@Actor+0x58] then edx=mode, r8=avi, xmm3=delta). This is
// how the engine's own damage system changes a current actor value — it clamps,
// updates the health bar, and trips the death threshold at <=0 — unlike SetBase
// (base value, never kills). Matches TiltedEvolution/FalloutTogether's health
// mirroring (ForceActorValue(DAMAGE)). SDK exposes slot 6 as Mod(unk1,avi,val).
__declspec(noinline) bool SafeForceDamage(Actor* actor, ActorValueInfo* avi, float delta)
{
    LOG_REQUIREMENT("CoSyncAVSafety::SafeForceDamage", "Needs a valid actor and stat type. Wrapped in a safety guard, since a proxy player with AI turned off may be missing the piece of data this changes. Applies the change through the game's own damage system, so health bars and death still trigger normally.");
    if (!actor || !avi) return false;
    __try
    {
        actor->actorValueOwner.Mod(2 /* ForceMode::DAMAGE */, avi, delta);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[AVSafety] ForceDamage crashed actor=%p avi=%p delta=%.2f", actor, avi, delta);
        return false;
    }
}

}
