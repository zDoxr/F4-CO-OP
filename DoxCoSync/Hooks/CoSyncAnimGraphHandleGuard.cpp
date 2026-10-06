#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncAnimGraphHandleGuard.h"

#include "ConsoleLogger.h"
#include "MinHook.h"

#include <atomic>
#include <cstdint>

// RVA CORRECTED round 12 (this campaign): round 93's "IDA-confirmed" entry
// 0x4C4DF0 is itself wrong -- a fresh ground-truth read of Fallout4.exe's PE
// exception directory (.pdata RUNTIME_FUNCTION table, independent of any
// disassembly/IDA guess) shows 0x4C4DF0 falls in the padding gap BETWEEN two
// real functions ([0x4C4BD0,0x4C4DEB) and [0x4C4E00,0x4C4E46)), not at any
// function's start. MH_CreateHook silently "succeeded" there anyway (same
// length-disassembler-finds-plausible-bytes disease as every other stale RVA
// this project has hit) -- which is why the heartbeat log NEVER incremented
// even once across multiple live crashes: nothing legitimately calls into
// dead padding. The real entry, per .pdata, is 0x4C4E00 (size 0x46) -- the
// crash site (0x4C4E1A, offset 0x1A) falls cleanly inside that function's
// body, consistent with round 73's described logic:
//   rdi = [rcx+0x48]          (TESObjectREFR::animGraphHolder)
//   rbx = rdi+0x20
//   AddRefHandle(rbx)         <- crashes here when rdi==nullptr
//   ... lookup, ReleaseHandle(rbx), return (rdi2 != 0)
// i.e. it has no null-check on animGraphHolder at all.
static constexpr uintptr_t kAnimGraphHandleFn_RVA = 0x004C5120;

using AnimGraphHandleFn = char(__fastcall*)(void* refr);

static AnimGraphHandleFn  g_origFn = nullptr;
static std::atomic<bool>  g_hookInstalled{ false };

// round 94 diagnostic: this fires extremely often (per-ref, per-frame), so
// the heartbeat interval is much larger than other hooks to avoid log spam.
// Tracks both total calls and how often the null-holder guard actually
// saves a would-be-null-deref -- confirms both that the round-93 RVA fix
// (kAnimGraphHandleFn_RVA) is reached AND that the guard is still useful.
namespace
{
    std::atomic<uint64_t> s_callCount{ 0 };
    std::atomic<uint64_t> s_nullGuardCount{ 0 };
    std::atomic<uint64_t> s_raceGuardCount{ 0 };
}

// __declspec(noinline) and the absence of `noexcept` are both load-bearing
// under this project's /GL+/LTCG Release config (same lesson as
// CoSyncLocomotionCalcGuard.cpp's CallOriginalSafe). Needed here because the
// pre-check above and the original function's own replayed prologue re-fetch
// refr+0x48 independently -- if something tears down the anim graph holder
// between the two reads (e.g. right after a fresh spawn), the pre-check goes
// stale and the original still null-derefs at animGraphHolder+0x20.
__declspec(noinline) static char CallOriginalSafe(AnimGraphHandleFn fn, void* refr)
{
    __try
    {
        return fn(refr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ++s_raceGuardCount;
        return 0;
    }
}

static char __fastcall Detour_AnimGraphHandleFn(void* refr)
{
    const uint64_t n = ++s_callCount;
    if (n == 1 || n % 20000 == 0)
        LOG_INFO("[AnimGraphHandleGuard] heartbeat: calls=%llu nullGuardSaves=%llu raceGuardSaves=%llu",
            (unsigned long long)n, (unsigned long long)s_nullGuardCount.load(), (unsigned long long)s_raceGuardCount.load());

    if (!refr)
        return 0;

    void* animGraphHolder = *reinterpret_cast<void* const*>(
        reinterpret_cast<const uint8_t*>(refr) + 0x48);

    if (!animGraphHolder)
    {
        ++s_nullGuardCount;
        // Mirror the original function's own "nothing found" return path
        // (its AL=0 case) instead of dereferencing animGraphHolder+0x20
        // while animGraphHolder is null.
        return 0;
    }

    return g_origFn ? CallOriginalSafe(g_origFn, refr) : 0;
}

void InstallAnimGraphHandleGuard()
{
    if (g_hookInstalled.exchange(true))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kAnimGraphHandleFn_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[AnimGraphHandleGuard] MH_Initialize failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Detour_AnimGraphHandleFn),
        reinterpret_cast<void**>(&g_origFn));
    if (st != MH_OK)
    {
        LOG_ERROR("[AnimGraphHandleGuard] MH_CreateHook failed: %d target=%p", (int)st, target);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK)
    {
        LOG_ERROR("[AnimGraphHandleGuard] MH_EnableHook failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    LOG_INFO("[AnimGraphHandleGuard] Hook installed at %p (RVA 0x%zX)", target, kAnimGraphHandleFn_RVA);
}

void UninstallAnimGraphHandleGuard()
{
    if (!g_hookInstalled.exchange(false))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kAnimGraphHandleFn_RVA);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    LOG_INFO("[AnimGraphHandleGuard] Uninstalled");
}
