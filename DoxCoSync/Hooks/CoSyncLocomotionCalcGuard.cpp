#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncLocomotionCalcGuard.h"

#include "ConsoleLogger.h"
#include "MinHook.h"
#include "CoSyncRequirementsLog.h"

#include <atomic>
#include <cstdint>
#include <mutex>

// RVA CORRECTED round 89 (IDA cross-check): round 77's comment below
// accurately describes this function's real prologue and confirmed it via
// pdata/.xdata bounds lookup -- but the literal constant recorded was
// 0xCC8AB0, which IDA proves is NOT the function entry. It's a single byte
// inside the 5-byte operand of an unrelated `call sub_140CD9370` instruction
// 0x190 bytes into the function body (the call site itself starts at
// 0xCC8AB0 exactly, so MinHook's length-disassembler found 5+ syntactically
// valid bytes there and silently "succeeded" -- it never validates that a
// target is an actual function entry). That means every single frame, for
// every actor in the game (not just CoSync's), this hook has been splicing
// a jmp into the middle of sub_140CC8920's own live stack frame (it had
// already done `sub rsp,0x68` plus four register pushes and several XMM
// spills by this point) -- our detour's own prologue then overwrites those
// still-live spill slots before MinHook's trampoline jumps back into the
// middle of the original function to continue. That is a fully sufficient
// mechanism for the wandering, hard-to-pin-down crash signatures
// (Fallout4.exe+0x256B80, +0xCC934D, and others) chased since round ~73 --
// they were never independently buggy call sites, just wherever the
// corrupted stack slot happened to get read back downstream. Round 77's
// own prologue description (mov rax,rsp; push rbp; push rdi; push r14;
// push r15; sub rsp,0x68; ...) matches IDA's disassembly EXACTLY at
// 0xCC8920 -- that is the real entry, and at true entry rcx=actor,
// rdx=context exactly as this guard's typedef already assumes, so no other
// logic in this file needs to change.
//
// RVA CORRECTED round 13 (this campaign): ground-truth .pdata
// (RUNTIME_FUNCTION table) shows 0xCC8920 is NOT a function entry -- it's
// 0x30 bytes into the real function, which actually starts at 0xCC88F0
// ([0xCC88F0, 0xCC893F), 0x4F bytes). Round 89/90's "IDA cross-check" of
// 0xCC8920 was itself wrong (same disease that hit AnimGraphHandleGuard,
// see round 12). This means round 10's "entry+1 is a separate legitimate
// call target" diagnosis was also wrong -- there's no real entry at 0xCC8920
// to collide with; MinHook was just splicing a jmp 0x30 bytes into a live
// function body the whole time, identical to every other "successfully
// hooks garbage mid-function" bug this project has hit. Disabling the hook
// (round 10) stopped that corruption but also removed this guard's SEH
// cover for the known +0x256B80 leaf crash, which then started reaching the
// engine unprotected. Re-enabling with the corrected entry restores that
// cover without the entry+1 corruption.
//
// disassembly of the live 1.11.221 Fallout4.exe (round 77): real prologue
// (mov rax,rsp; push rbp; push rdi; push r14; push r15; sub rsp,0x68; ...)
// starts here. fastcall(rcx=actor, rdx=context) -> float.
static constexpr uintptr_t kLocomotionCalcFn_RVA = 0x00CC8C80;

using LocomotionCalcFn = float(__fastcall*)(void* rcx, void* rdx);

static LocomotionCalcFn   g_origFn = nullptr;
static std::atomic<bool>  g_hookInstalled{ false };

// UPDATE 2026-06-21 (round 84): rounds 77-83 made the SEH catch itself
// progressively cheaper (rate limiter, then skipping the minidump for this
// already-diagnosed RVA), but every occurrence STILL called all the way into
// the real engine function first, which runs real code (writes, partial
// state mutation) before hitting the bad dereference. Once the catch became
// nearly free, the engine's own retry/walker loop could re-enter this same
// broken call far faster than before -- 15 occurrences in ~112ms, ~7.5ms
// apart -- and the live test that round 83's "looks fixed" verdict was based
// on actually crashed the process right after that burst (confirmed: the
// log file's last-modified timestamp matches the last logged line exactly,
// with zero further Tick activity -- the process died there, it didn't just
// log quietly). The repeated partial execution at high frequency, not the
// logging cost, is the real suspect. This actor's relevant sub-object can
// never become valid (TickHook's round-79 Update_PerFrame bail already
// proved nothing ever populates it for a remote-controlled proxy), so
// retrying is pointless by construction -- once a given actor pointer has
// faulted here, skip calling the real function for that exact pointer ever
// again this session instead of re-attempting it on every subsequent visit.
namespace
{
    constexpr int kMaxTrackedFaultedActors = 16;
    std::mutex s_faultedActorsMutex;
    void*      s_faultedActors[kMaxTrackedFaultedActors] = {};
    int        s_faultedActorCount = 0;

    bool HasAlreadyFaulted(void* actor)
    {
        LOG_REQUIREMENT("HasAlreadyFaulted", "Doesn't touch the game at all — just checks an actor pointer against its own list of known-bad ones, without reading the actor itself.");
        std::lock_guard<std::mutex> lk(s_faultedActorsMutex);
        for (int i = 0; i < s_faultedActorCount; ++i)
            if (s_faultedActors[i] == actor)
                return true;
        return false;
    }

    void RecordFault(void* actor)
    {
        LOG_REQUIREMENT("RecordFault", "Doesn't touch the game at all — just adds an actor pointer to its own list of known-bad ones, without reading the actor itself.");
        std::lock_guard<std::mutex> lk(s_faultedActorsMutex);
        for (int i = 0; i < s_faultedActorCount; ++i)
            if (s_faultedActors[i] == actor)
                return;
        if (s_faultedActorCount < kMaxTrackedFaultedActors)
            s_faultedActors[s_faultedActorCount++] = actor;
        LOG_WARN("[LocomotionCalcGuard] FAULT actor=%p (now permanently skipped, %d/%d tracked)",
            actor, s_faultedActorCount, kMaxTrackedFaultedActors);
    }

    // round 94 diagnostic: a periodic heartbeat proves the hook is actually
    // firing (not silently uninstalled/never reached) and tracks how many
    // distinct actors have ever faulted here, without spamming the log.
    std::atomic<uint64_t> s_callCount{ 0 };
    void LogHeartbeat()
    {
        LOG_REQUIREMENT("LogHeartbeat", "Doesn't touch the game at all — just reads its own counters.");
        const uint64_t n = ++s_callCount;
        if (n == 1 || n % 5000 == 0)
        {
            int faulted;
            { std::lock_guard<std::mutex> lk(s_faultedActorsMutex); faulted = s_faultedActorCount; }
            LOG_INFO("[LocomotionCalcGuard] heartbeat: calls=%llu faultedActors=%d",
                (unsigned long long)n, faulted);
        }
    }
}

// __declspec(noinline) and the absence of `noexcept` are both load-bearing
// under this project's /GL+/LTCG Release config -- see CoSyncDoorHook.cpp's
// SafeReadU32/SafeReadPtr and CoSyncMovementHandleGuard.cpp's
// CallOriginalSafe for the same lesson learned earlier this project: either
// one missing lets the optimizer silently strip the __try/__except scope.
__declspec(noinline) static float CallOriginalSafe(LocomotionCalcFn fn, void* rcx, void* rdx)
{
    LOG_REQUIREMENT("CallOriginalSafe", "Needs a valid pointer to the real engine function. The actor passed in might not be fully set up yet (like one that just spawned), so this keeps a safety net around the call.");
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

static float __fastcall Detour_LocomotionCalcFn(void* rcx, void* rdx)
{
    LOG_REQUIREMENT("Detour_LocomotionCalcFn", "Needs the hook to already be installed so it has the real function to call. Skips any actor that already crashed here before, so it doesn't call the real function on a known-bad state again.");
    LogHeartbeat();

    if (!g_origFn)
        return 0.0f;

    if (HasAlreadyFaulted(rcx))
        return 0.0f;

    return CallOriginalSafe(g_origFn, rcx, rdx);
}

void InstallLocomotionCalcGuard()
{
    LOG_REQUIREMENT("InstallLocomotionCalcGuard", "Must not already be installed. Needs the saved game address to still point at the right function for this game version, and needs the module handle lookup to succeed.");
    if (g_hookInstalled.exchange(true))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kLocomotionCalcFn_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[LocomotionCalcGuard] MH_Initialize failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Detour_LocomotionCalcFn),
        reinterpret_cast<void**>(&g_origFn));
    if (st != MH_OK)
    {
        LOG_ERROR("[LocomotionCalcGuard] MH_CreateHook failed: %d target=%p", (int)st, target);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK)
    {
        LOG_ERROR("[LocomotionCalcGuard] MH_EnableHook failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    LOG_INFO("[LocomotionCalcGuard] Hook installed at %p (RVA 0x%zX)", target, kLocomotionCalcFn_RVA);
}

void UninstallLocomotionCalcGuard()
{
    LOG_REQUIREMENT("UninstallLocomotionCalcGuard", "The hook must currently be installed, and the module handle lookup must resolve to the same base address used when it was installed.");
    if (!g_hookInstalled.exchange(false))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kLocomotionCalcFn_RVA);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    LOG_INFO("[LocomotionCalcGuard] Uninstalled");
}
