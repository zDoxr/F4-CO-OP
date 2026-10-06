#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncSharedHandleLeafGuard.h"

#include "ConsoleLogger.h"
#include "MinHook.h"

#include <atomic>
#include <cstdint>
#include <mutex>

// RVA confirmed via VEH live crash RIP across rounds 73-94 and round 14 of
// this campaign, and via pdata ground-truth (round 13): no RUNTIME_FUNCTION
// entry covers this address (true leaf, no own stack frame), but the live
// byte dump at the crash RIP decodes cleanly as a complete, aligned function
// body followed by int3 padding -- see CoSyncSharedHandleLeafGuard.h for the
// full disassembly and the 3 known callers.
static constexpr uintptr_t kSharedHandleLeafFn_RVA = 0x00256EA0;

using SharedHandleLeafFn = float(__fastcall*)(void* rcx);

static SharedHandleLeafFn g_origFn = nullptr;
static std::atomic<bool>  g_hookInstalled{ false };

namespace
{
    constexpr int kMaxTrackedFaultedHandles = 16;
    std::mutex s_faultedHandlesMutex;
    void*      s_faultedHandles[kMaxTrackedFaultedHandles] = {};
    int        s_faultedHandleCount = 0;

    bool HasAlreadyFaulted(void* handle)
    {
        std::lock_guard<std::mutex> lk(s_faultedHandlesMutex);
        for (int i = 0; i < s_faultedHandleCount; ++i)
            if (s_faultedHandles[i] == handle)
                return true;
        return false;
    }

    void RecordFault(void* handle)
    {
        std::lock_guard<std::mutex> lk(s_faultedHandlesMutex);
        for (int i = 0; i < s_faultedHandleCount; ++i)
            if (s_faultedHandles[i] == handle)
                return;
        if (s_faultedHandleCount < kMaxTrackedFaultedHandles)
            s_faultedHandles[s_faultedHandleCount++] = handle;
        LOG_WARN("[SharedHandleLeafGuard] FAULT handle=%p (now permanently skipped, %d/%d tracked)",
            handle, s_faultedHandleCount, kMaxTrackedFaultedHandles);
    }

    std::atomic<uint64_t> s_callCount{ 0 };
    std::atomic<uint64_t> s_sentinelGuardCount{ 0 };
    void LogHeartbeat()
    {
        const uint64_t n = ++s_callCount;
        if (n == 1 || n % 20000 == 0)
        {
            int faulted;
            { std::lock_guard<std::mutex> lk(s_faultedHandlesMutex); faulted = s_faultedHandleCount; }
            LOG_INFO("[SharedHandleLeafGuard] heartbeat: calls=%llu sentinelGuardSaves=%llu faultedHandles=%d",
                (unsigned long long)n, (unsigned long long)s_sentinelGuardCount.load(), faulted);
        }
    }
}

// __declspec(noinline) and the absence of `noexcept` are both load-bearing
// under this project's /GL+/LTCG Release config -- see
// CoSyncLocomotionCalcGuard.cpp's CallOriginalSafe for the same lesson.
__declspec(noinline) static float CallOriginalSafe(SharedHandleLeafFn fn, void* rcx)
{
    __try
    {
        return fn(rcx);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        RecordFault(rcx);
        return 0.0f;
    }
}

static float __fastcall Detour_SharedHandleLeafFn(void* rcx)
{
    LogHeartbeat();

    if (!g_origFn)
        return 0.0f;

    if (HasAlreadyFaulted(rcx))
        return 0.0f;

    const uint64_t v = reinterpret_cast<uint64_t>(rcx);
    if (!rcx || (v >> 56) == 0xFF)
    {
        ++s_sentinelGuardCount;
        return 0.0f;
    }

    return CallOriginalSafe(g_origFn, rcx);
}

void InstallSharedHandleLeafGuard()
{
    if (g_hookInstalled.exchange(true))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kSharedHandleLeafFn_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[SharedHandleLeafGuard] MH_Initialize failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Detour_SharedHandleLeafFn),
        reinterpret_cast<void**>(&g_origFn));
    if (st != MH_OK)
    {
        LOG_ERROR("[SharedHandleLeafGuard] MH_CreateHook failed: %d target=%p", (int)st, target);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK)
    {
        LOG_ERROR("[SharedHandleLeafGuard] MH_EnableHook failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    LOG_INFO("[SharedHandleLeafGuard] Hook installed at %p (RVA 0x%zX)", target, kSharedHandleLeafFn_RVA);
}

void UninstallSharedHandleLeafGuard()
{
    if (!g_hookInstalled.exchange(false))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kSharedHandleLeafFn_RVA);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    LOG_INFO("[SharedHandleLeafGuard] Uninstalled");
}
