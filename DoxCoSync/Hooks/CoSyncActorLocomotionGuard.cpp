#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncActorLocomotionGuard.h"

#include "ConsoleLogger.h"
#include "MinHook.h"
#include "CoSyncRequirementsLog.h"

#include <atomic>
#include <cstdint>
#include <mutex>

// RVA confirmed as a real function entry via pdata ground-truth (round 15):
// [0xCC8AD8, 0xCC8E40), 0x368 bytes. See CoSyncActorLocomotionGuard.h for
// the full rationale and the two distinct live crashes that trace back to
// internal call sites inside this function.
static constexpr uintptr_t kActorLocomotionFn_RVA = 0x00CC8E68;

using ActorLocomotionFn = float(__fastcall*)(void* rcx, void* rdx);

static ActorLocomotionFn  g_origFn = nullptr;
static std::atomic<bool>  g_hookInstalled{ false };

namespace
{
    constexpr int kMaxTrackedFaultedActors = 16;
    std::mutex s_faultedActorsMutex;
    void*      s_faultedActors[kMaxTrackedFaultedActors] = {};
    int        s_faultedActorCount = 0;

    bool HasAlreadyFaulted(void* actor)
    {
        LOG_REQUIREMENT("HasAlreadyFaulted", "Just checks an actor pointer against a list, no need to touch the actor itself.");
        std::lock_guard<std::mutex> lk(s_faultedActorsMutex);
        for (int i = 0; i < s_faultedActorCount; ++i)
            if (s_faultedActors[i] == actor)
                return true;
        return false;
    }

    void RecordFault(void* actor)
    {
        LOG_REQUIREMENT("RecordFault", "Just stores an actor pointer in a list, no need to touch the actor itself.");
        std::lock_guard<std::mutex> lk(s_faultedActorsMutex);
        for (int i = 0; i < s_faultedActorCount; ++i)
            if (s_faultedActors[i] == actor)
                return;
        if (s_faultedActorCount < kMaxTrackedFaultedActors)
            s_faultedActors[s_faultedActorCount++] = actor;
        LOG_WARN("[ActorLocomotionGuard] FAULT actor=%p (now permanently skipped, %d/%d tracked)",
            actor, s_faultedActorCount, kMaxTrackedFaultedActors);
    }

    std::atomic<uint64_t> s_callCount{ 0 };

    // round 18: round 16's SEH-wrapped LOG_INFO call (LogHeartbeatLineSafe)
    // did NOT stop the round-17 crash cascade -- this hook's call context
    // makes even an SEH-guarded vsnprintf unsafe, not just an unguarded one.
    // Logging nothing at all from this specific call site instead of trying
    // to contain it further; s_callCount/s_faultedActorCount remain readable
    // by other guards' diagnostics if ever needed.
    void LogHeartbeat()
    {
        LOG_REQUIREMENT("LogHeartbeat", "Just counts calls, doesn't actually log anything on purpose.");
        ++s_callCount;
    }
}

// __declspec(noinline) and the absence of `noexcept` are both load-bearing
// under this project's /GL+/LTCG Release config -- see
// CoSyncLocomotionCalcGuard.cpp's CallOriginalSafe for the same lesson.
__declspec(noinline) static float CallOriginalSafe(ActorLocomotionFn fn, void* rcx, void* rdx)
{
    LOG_REQUIREMENT("CallOriginalSafe", "Needs a valid pointer to the real game function. The actor passed in may be only partly set up, so this relies on a safety net to catch any crash from touching it.");
    __try
    {
        return fn(rcx, rdx);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        RecordFault(rcx);
        return 0.0f;
    }
}

static float __fastcall Detour_ActorLocomotionFn(void* rcx, void* rdx)
{
    LOG_REQUIREMENT("Detour_ActorLocomotionFn", "Needs the hook to already be installed. Skips any actor that already crashed before, so it doesn't re-trigger a known-bad case.");
    LogHeartbeat();

    if (!g_origFn)
        return 0.0f;

    if (HasAlreadyFaulted(rcx))
        return 0.0f;

    return CallOriginalSafe(g_origFn, rcx, rdx);
}

void InstallActorLocomotionGuard()
{
    LOG_REQUIREMENT("InstallActorLocomotionGuard", "Must not already be installed. Needs the known internal address to be correct for this game build, and the game module to resolve properly.");
    if (g_hookInstalled.exchange(true))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kActorLocomotionFn_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[ActorLocomotionGuard] MH_Initialize failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Detour_ActorLocomotionFn),
        reinterpret_cast<void**>(&g_origFn));
    if (st != MH_OK)
    {
        LOG_ERROR("[ActorLocomotionGuard] MH_CreateHook failed: %d target=%p", (int)st, target);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK)
    {
        LOG_ERROR("[ActorLocomotionGuard] MH_EnableHook failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    LOG_INFO("[ActorLocomotionGuard] Hook installed at %p (RVA 0x%zX)", target, kActorLocomotionFn_RVA);
}

void UninstallActorLocomotionGuard()
{
    LOG_REQUIREMENT("UninstallActorLocomotionGuard", "The hook must already be installed. Needs the game module to resolve to the same address used at install time.");
    if (!g_hookInstalled.exchange(false))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kActorLocomotionFn_RVA);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    LOG_INFO("[ActorLocomotionGuard] Uninstalled");
}
