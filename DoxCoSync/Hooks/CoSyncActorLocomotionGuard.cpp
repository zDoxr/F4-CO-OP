#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncActorLocomotionGuard.h"

#include "ConsoleLogger.h"
#include "MinHook.h"

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
        std::lock_guard<std::mutex> lk(s_faultedActorsMutex);
        for (int i = 0; i < s_faultedActorCount; ++i)
            if (s_faultedActors[i] == actor)
                return true;
        return false;
    }

    void RecordFault(void* actor)
    {
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
        ++s_callCount;
    }
}

// __declspec(noinline) and the absence of `noexcept` are both load-bearing
// under this project's /GL+/LTCG Release config -- see
// CoSyncLocomotionCalcGuard.cpp's CallOriginalSafe for the same lesson.
__declspec(noinline) static float CallOriginalSafe(ActorLocomotionFn fn, void* rcx, void* rdx)
{
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
    LogHeartbeat();

    if (!g_origFn)
        return 0.0f;

    if (HasAlreadyFaulted(rcx))
        return 0.0f;

    return CallOriginalSafe(g_origFn, rcx, rdx);
}

void InstallActorLocomotionGuard()
{
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
    if (!g_hookInstalled.exchange(false))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kActorLocomotionFn_RVA);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    LOG_INFO("[ActorLocomotionGuard] Uninstalled");
}
