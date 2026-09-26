#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncActorProcessGuard.h"

#include "ConsoleLogger.h"
#include "MinHook.h"
#include "CoSyncRequirementsLog.h"

#include <atomic>
#include <cstdint>
#include <mutex>

// RVA CORRECTED round 89 (IDA cross-check): round 86's prologue description
// below matches IDA's disassembly of sub_140DB88C0 exactly -- but the
// literal constant recorded was 0xDB8A60, which is NOT the function entry.
// It's the LAST byte of the 5-byte operand of an unrelated
// `call sub_14165D150` instruction that starts at 0xDB8A5C, well inside the
// function body. Same class of bug as CoSyncLocomotionCalcGuard.cpp and
// CoSyncMovementHandleGuard.cpp's round-89 fixes (see the former's comment
// for the full corruption mechanism) -- MinHook only checks for enough
// relocatable bytes, not whether the target is a real entry point, so this
// has been splicing a jmp into a live stack frame on every call too. Real
// entry is 0xDB88C0.
//
// RVA CORRECTED round 20 (this campaign): round 89's "real entry is
// 0xDB88C0" was ALSO wrong -- ground-truth .pdata (RUNTIME_FUNCTION table)
// shows 0xDB88C0 is 0x13 bytes into the real function, which actually
// starts at 0xDB88AD ([0xDB88AD, 0xDB8941), 0x94 bytes). Heartbeat never
// fired in any session log under the old RVA, confirming it never reached
// real code -- same disease as CoSyncLocomotionCalcGuard/
// CoSyncMovementHandleGuard's round-13 fixes elsewhere this campaign. Crash
// signature this time was Fallout4.exe+0xDB88C1 (old RVA + 1), same
// entry+1-corruption shape as round 10's LocomotionCalcGuard crash, which
// that round wrongly attributed to "a separate legitimate call target" --
// it was just more mid-function corruption, like here.
//
// One more correction: per Hex-Rays decompilation of the real function,
// `rcx` (a1) is NOT "the actor" -- it dereferences a1+88/a1+104 as a
// fixed-size circular queue (round-robin index via
// `(counter + base - 1) % capacity`) and resolves the actual per-call actor
// handle internally via sub_140568240 into a separate local (the
// equivalent of this file's `rdx`-adjacent fault target is actually that
// internal queue-resolved pointer, not rcx itself). rcx looks like a
// scheduler/manager object, plausibly a long-lived singleton across calls.
// This guard's existing per-rcx circuit breaker therefore likely behaves
// as a coarser "disable this whole engine subsystem globally after the
// first fault" rather than a true per-actor breaker -- not wrong or
// unsafe, just less granular than the comment previously implied. Left
// as-is since the RVA fix alone may remove most/all faults here; revisit
// the breaker's granularity only if faults still recur after this fix.
//
// disassembly of the live 1.11.221 Fallout4.exe (round 86): real prologue
// (mov rax,rsp; mov [rax+0x20],rbp; push rsi; push r14; push r15; sub
// rsp,0x70; ...; mov rbp,rcx; movzx r14d,dl; ...) starts here.
// fastcall(rcx=actor, rdx=flag) -> void (caller never consumes a return
// value after `call 0x140db8a60`).
static constexpr uintptr_t kActorProcessFn_RVA = 0x00DB8C3D;

using ActorProcessFn = void(__fastcall*)(void* rcx, void* rdx);

static ActorProcessFn     g_origFn = nullptr;
static std::atomic<bool>  g_hookInstalled{ false };

// Same per-actor-pointer circuit breaker as CoSyncLocomotionCalcGuard.cpp /
// CoSyncMovementHandleGuard.cpp -- see round 84's comment there for the full
// rationale. Once an actor pointer has faulted through this entry, the real
// function is never called for that exact pointer again this session.
namespace
{
    constexpr int kMaxTrackedFaultedActors = 16;
    std::mutex s_faultedActorsMutex;
    void*      s_faultedActors[kMaxTrackedFaultedActors] = {};
    int        s_faultedActorCount = 0;

    bool HasAlreadyFaulted(void* actor)
    {
        LOG_REQUIREMENT("HasAlreadyFaulted", "Just checks a stored pointer value against a list. No game state involved, and the pointer itself is never dereferenced.");
        std::lock_guard<std::mutex> lk(s_faultedActorsMutex);
        for (int i = 0; i < s_faultedActorCount; ++i)
            if (s_faultedActors[i] == actor)
                return true;
        return false;
    }

    void RecordFault(void* actor)
    {
        LOG_REQUIREMENT("RecordFault", "Just stores a pointer value in a list. No game state involved, and the pointer itself is never dereferenced.");
        std::lock_guard<std::mutex> lk(s_faultedActorsMutex);
        for (int i = 0; i < s_faultedActorCount; ++i)
            if (s_faultedActors[i] == actor)
                return;
        if (s_faultedActorCount < kMaxTrackedFaultedActors)
            s_faultedActors[s_faultedActorCount++] = actor;
        // round 94 diagnostic: this is the single most important log line for
        // confirming whether the round-90 RVA fix actually eliminated the
        // fault. If this NEVER appears in a play-session log, the fix held.
        // Recall this guard's breaker is likely coarser than per-actor (see
        // round-89 comment above: rcx is a scheduler/manager object, not the
        // actor itself), so even ONE fault here may suppress this whole
        // engine subsystem for the rest of the session -- worth flagging
        // distinctly from the other two guards if it ever fires.
        LOG_WARN("[ActorProcessGuard] FAULT rcx=%p (now permanently skipped, %d/%d tracked)",
            actor, s_faultedActorCount, kMaxTrackedFaultedActors);
    }

    // round 21: now that the RVA fix makes this hook actually fire for the
    // first time ever (heartbeat had never logged once across the entire
    // campaign before), its first-ever LOG_INFO heartbeat call crashed
    // inside vsnprintf's internal movdqa (stack-alignment-sensitive SSE
    // store) -- the same disease CoSyncActorLocomotionGuard.cpp hit in round
    // 16 from a similarly deep/unusual call context. Round 16's SEH-wrapped
    // log call didn't even contain it there (round 17), so logging nothing
    // at all here too instead of trying to contain it further.
    std::atomic<uint64_t> s_callCount{ 0 };
    void LogHeartbeat()
    {
        LOG_REQUIREMENT("LogHeartbeat", "Just counts calls. No game state involved, and deliberately does not print anything by design.");
        ++s_callCount;
    }
}

// __declspec(noinline) and the absence of `noexcept` are both load-bearing
// under this project's /GL+/LTCG Release config -- see CoSyncDoorHook.cpp's
// SafeReadU32/SafeReadPtr and every other guard in this project for the same
// lesson learned repeatedly: either one missing lets the optimizer silently
// strip the __try/__except scope.
__declspec(noinline) static void CallOriginalSafe(ActorProcessFn fn, void* rcx, void* rdx)
{
    LOG_REQUIREMENT("CallOriginalSafe", "Needs a valid pointer to the real game function. The object passed in may reference an actor that has already gone away, so this is guarded to catch a crash safely instead of taking down the game.");
    __try
    {
        fn(rcx, rdx);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        RecordFault(rcx);
    }
}

static void __fastcall Detour_ActorProcessFn(void* rcx, void* rdx)
{
    LOG_REQUIREMENT("Detour_ActorProcessFn", "Needs the hook to already be installed at the real function entry point. Skips anything that has already caused a crash before -- note this can end up skipping more than just the one actor that first faulted.");
    LogHeartbeat();

    if (!g_origFn)
        return;

    if (HasAlreadyFaulted(rcx))
        return;

    CallOriginalSafe(g_origFn, rcx, rdx);
}

void InstallActorProcessGuard()
{
    LOG_REQUIREMENT("InstallActorProcessGuard", "Must not already be installed. The hardcoded game address needs to point at the real function entry for this game build, and the module base address needs to resolve correctly.");
    if (g_hookInstalled.exchange(true))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kActorProcessFn_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[ActorProcessGuard] MH_Initialize failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Detour_ActorProcessFn),
        reinterpret_cast<void**>(&g_origFn));
    if (st != MH_OK)
    {
        LOG_ERROR("[ActorProcessGuard] MH_CreateHook failed: %d target=%p", (int)st, target);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK)
    {
        LOG_ERROR("[ActorProcessGuard] MH_EnableHook failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    LOG_INFO("[ActorProcessGuard] Hook installed at %p (RVA 0x%zX)", target, kActorProcessFn_RVA);
}

void UninstallActorProcessGuard()
{
    LOG_REQUIREMENT("UninstallActorProcessGuard", "Needs the hook to currently be installed, and the module base address to match what it was at install time.");
    if (!g_hookInstalled.exchange(false))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kActorProcessFn_RVA);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    LOG_INFO("[ActorProcessGuard] Uninstalled");
}
