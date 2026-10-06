#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncMusicChangerCellTeardownGuard.h"

#include "ConsoleLogger.h"
#include "MinHook.h"

#include <atomic>
#include <cstdint>

// RVA verified 2026-08-12 against Fallout4_disk.i64 (build 1.11.221.0, the
// canonical disk-exe database, no dump-IDB skew) and cross-checked against
// the live TTD trace's own exception record (SYMBOL_NAME: Fallout4+4caf53,
// which is 0xB3 bytes into this function's real entry at 0x4CAEA0 --
// confirmed by .pdata function bounds and a clean Hex-Rays decompile with
// no garbage/misaligned instructions). fastcall(rcx=cellOrRefPtr) -> __int64.
static constexpr uintptr_t kMusicChangerResolveFn_RVA = 0x004CB1C0;

using MusicChangerResolveFn = __int64(__fastcall*)(__int64 rcx);

static MusicChangerResolveFn g_origFn = nullptr;
static std::atomic<bool>     g_hookInstalled{ false };

namespace
{
    std::atomic<uint64_t> s_callCount{ 0 };
    std::atomic<uint64_t> s_faultCount{ 0 };

    void LogHeartbeat()
    {
        const uint64_t n = ++s_callCount;
        if (n == 1 || n % 200 == 0)
        {
            LOG_INFO("[MusicChangerCellTeardownGuard] heartbeat: calls=%llu faults=%llu",
                (unsigned long long)n, (unsigned long long)s_faultCount.load());
        }
    }
}

// __declspec(noinline) and the absence of `noexcept` are both load-bearing
// under this project's /GL+/LTCG Release config -- see
// CoSyncLocomotionCalcGuard.cpp's CallOriginalSafe for the same lesson: either
// one missing lets the optimizer silently strip the __try/__except scope.
__declspec(noinline) static __int64 CallOriginalSafe(MusicChangerResolveFn fn, __int64 rcx)
{
    __try
    {
        return fn(rcx);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ++s_faultCount;
        LOG_WARN("[MusicChangerCellTeardownGuard] Crash caught and recovered, ref=%p -- treating it as 'nothing found', same as the game would", (void*)rcx);
        return 0;
    }
}

static __int64 __fastcall Detour_MusicChangerResolveFn(__int64 rcx)
{
    LogHeartbeat();

    if (!g_origFn)
        return 0;

    return CallOriginalSafe(g_origFn, rcx);
}

void InstallMusicChangerCellTeardownGuard()
{
    if (g_hookInstalled.exchange(true))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kMusicChangerResolveFn_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[MusicChangerCellTeardownGuard] MH_Initialize failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Detour_MusicChangerResolveFn),
        reinterpret_cast<void**>(&g_origFn));
    if (st != MH_OK)
    {
        LOG_ERROR("[MusicChangerCellTeardownGuard] MH_CreateHook failed: %d target=%p", (int)st, target);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK)
    {
        LOG_ERROR("[MusicChangerCellTeardownGuard] MH_EnableHook failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    LOG_INFO("[MusicChangerCellTeardownGuard] Hook installed at %p (RVA 0x%zX)", target, kMusicChangerResolveFn_RVA);
}

void UninstallMusicChangerCellTeardownGuard()
{
    if (!g_hookInstalled.exchange(false))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kMusicChangerResolveFn_RVA);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    LOG_INFO("[MusicChangerCellTeardownGuard] Uninstalled");
}
