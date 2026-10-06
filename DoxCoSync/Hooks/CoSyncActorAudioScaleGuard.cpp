#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncActorAudioScaleGuard.h"

#include "ConsoleLogger.h"
#include "MinHook.h"

#include <atomic>
#include <cstdint>
#include <cstring>

// RVA verified 2026-08-11 against Fallout4_disk.i64 (build 1.11.221.0, the
// canonical disk-exe database, no dump-IDB skew) and cross-checked against the
// live TTD trace's own exception record (fault at Fallout4+0xE5B7B1, which is
// 0x31 bytes into this function's real entry at 0xE5B780 -- confirmed by
// .pdata bounds 0xE5B780-0xE5B954 and a clean Hex-Rays decompile with no
// garbage/misaligned instructions).
//
// Signature confirmed three ways: (1) Hex-Rays gives
// `__m128 __fastcall sub_140E5B780(__int64 a1, __int64 a2)` returning in xmm0;
// (2) all arithmetic on the returned value is single-precision (movss / mulss
// / divss / maxss / comiss), so it is a float, not the double Hex-Rays
// occasionally infers for an __m128 return; (3) the traced caller
// (sub_140CABA20+0x6E) consumes the result with `mulss xmm7, xmm0`.
static constexpr uintptr_t kActorAudioScaleFn_RVA = 0x00E5BB10;

using ActorAudioScaleFn = float(__fastcall*)(void* subsystem, void* actor);

static ActorAudioScaleFn g_origFn = nullptr;
static std::atomic<bool> g_hookInstalled{ false };

// The function's own neutral result -- see the header for the derivation.
// Do NOT change this to 0.0f: this value is used multiplicatively by callers.
static constexpr float kNeutralScale = 1.0f;

namespace
{
    std::atomic<uint64_t> s_callCount{ 0 };
    std::atomic<uint64_t> s_nullGuardCount{ 0 };
    std::atomic<uint64_t> s_faultCount{ 0 };

    // Return-value instrumentation. The whole point of this guard is that we
    // substitute a value when the engine can't produce one, so we measure what
    // the engine actually returns the rest of the time and confirm our
    // substitute is in-range rather than assuming it.
    std::atomic<uint64_t> s_retNeutral{ 0 };   // returned exactly kNeutralScale
    std::atomic<uint64_t> s_retOther{ 0 };     // returned something else
    std::atomic<uint32_t> s_retMinBits{ 0x7F800000u };  // +inf
    std::atomic<uint32_t> s_retMaxBits{ 0xFF800000u };  // -inf

    inline uint32_t FloatBits(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }

    inline float BitsToFloat(uint32_t b)
    {
        float f;
        std::memcpy(&f, &b, sizeof(f));
        return f;
    }

    void RecordReturn(float v)
    {
        if (v == kNeutralScale)
            ++s_retNeutral;
        else
            ++s_retOther;

        // NaN would poison the min/max compare loops below, and the engine can
        // legitimately hand back garbage if its own state is half-torn-down.
        if (!(v == v))
            return;

        uint32_t cur = s_retMinBits.load(std::memory_order_relaxed);
        while (v < BitsToFloat(cur)
            && !s_retMinBits.compare_exchange_weak(cur, FloatBits(v), std::memory_order_relaxed))
        {
        }

        cur = s_retMaxBits.load(std::memory_order_relaxed);
        while (v > BitsToFloat(cur)
            && !s_retMaxBits.compare_exchange_weak(cur, FloatBits(v), std::memory_order_relaxed))
        {
        }
    }

    void LogStats(const char* why)
    {
        const uint64_t neutral = s_retNeutral.load();
        const uint64_t other   = s_retOther.load();
        const uint32_t minB    = s_retMinBits.load();
        const uint32_t maxB    = s_retMaxBits.load();
        const bool haveRange   = (neutral + other) > 0;

        LOG_INFO("[ActorAudioScaleGuard] %s: calls=%llu nullGuards=%llu faults=%llu | returns: ==%.3f x%llu, other x%llu, observed range [%s]",
            why,
            (unsigned long long)s_callCount.load(),
            (unsigned long long)s_nullGuardCount.load(),
            (unsigned long long)s_faultCount.load(),
            kNeutralScale,
            (unsigned long long)neutral,
            (unsigned long long)other,
            haveRange ? "see next line" : "no samples yet");

        if (haveRange)
            LOG_INFO("[ActorAudioScaleGuard]   min=%.6f max=%.6f (substituted value %.3f is %s this range)",
                BitsToFloat(minB), BitsToFloat(maxB), kNeutralScale,
                (kNeutralScale >= BitsToFloat(minB) && kNeutralScale <= BitsToFloat(maxB)) ? "INSIDE" : "*** OUTSIDE ***");
    }
}

// __declspec(noinline) and the absence of `noexcept` are both load-bearing
// under this project's /GL+/LTCG Release config -- see
// CoSyncLocomotionCalcGuard.cpp / CoSyncMusicChangerCellTeardownGuard.cpp for
// the same lesson: either one missing lets the optimizer silently strip the
// __try/__except scope.
__declspec(noinline) static float CallOriginalSafe(ActorAudioScaleFn fn, void* subsystem, void* actor)
{
    __try
    {
        return fn(subsystem, actor);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        const uint64_t n = ++s_faultCount;
        LOG_WARN("[ActorAudioScaleGuard] FAULT inside real fn (subsystem=%p actor=%p) -- caught, returning neutral %.3f (fault #%llu)",
            subsystem, actor, kNeutralScale, (unsigned long long)n);
        return kNeutralScale;
    }
}

static float __fastcall Detour_ActorAudioScaleFn(void* subsystem, void* actor)
{
    const uint64_t n = ++s_callCount;

    if (!subsystem)
    {
        const uint64_t ng = ++s_nullGuardCount;
        // Rate-limited: if the subsystem stays torn down this can fire every
        // frame, and we must not turn a survivable crash into a log flood.
        if (ng <= 10 || ng % 500 == 0)
            LOG_WARN("[ActorAudioScaleGuard] NULL subsystem (qword_1430EEF50) actor=%p -- returning neutral %.3f instead of crashing (null-guard save #%llu)",
                actor, kNeutralScale, (unsigned long long)ng);
        return kNeutralScale;
    }

    if (!g_origFn)
        return kNeutralScale;

    const float result = CallOriginalSafe(g_origFn, subsystem, actor);
    RecordReturn(result);

    // Sample real return values so the kNeutralScale choice is confirmed by
    // observation, not just by reading the decompile.
    if (n <= 20)
        LOG_INFO("[ActorAudioScaleGuard] sample call#%llu subsystem=%p actor=%p -> %.6f",
            (unsigned long long)n, subsystem, actor, result);
    else if (n % 20000 == 0)
        LogStats("heartbeat");

    return result;
}

void InstallActorAudioScaleGuard()
{
    if (g_hookInstalled.exchange(true))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kActorAudioScaleFn_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[ActorAudioScaleGuard] MH_Initialize failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Detour_ActorAudioScaleFn),
        reinterpret_cast<void**>(&g_origFn));
    if (st != MH_OK)
    {
        LOG_ERROR("[ActorAudioScaleGuard] MH_CreateHook failed: %d target=%p", (int)st, target);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK)
    {
        LOG_ERROR("[ActorAudioScaleGuard] MH_EnableHook failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    LOG_INFO("[ActorAudioScaleGuard] Hook installed at %p (offset 0x%zX) -- pairs with MusicChangerCellTeardownGuard, which resets around the same time",
        target, kActorAudioScaleFn_RVA);
}

void UninstallActorAudioScaleGuard()
{
    if (!g_hookInstalled.exchange(false))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kActorAudioScaleFn_RVA);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    LogStats("final");
    LOG_INFO("[ActorAudioScaleGuard] Uninstalled");
}
