#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncMovementHandleGuard.h"

#include "ConsoleLogger.h"
#include "MinHook.h"
#include "CoSyncRequirementsLog.h"

#include <atomic>
#include <cstdint>
#include <mutex>

// RVA CORRECTED round 89 (IDA cross-check): round 75's body description
// below is accurate and matches IDA's disassembly of sub_1406C9710 line
// for line -- but the literal constant recorded was 0x6C9730, which is NOT
// the function entry. It's one byte INSIDE the 5-byte operand of an
// unrelated `call sub_140263120` instruction that starts at 0x6C972F, 0x20
// bytes into the function (one of the "flag-gated special-case branches"
// mentioned below). MinHook's length-disassembler doesn't check whether a
// target is a real function entry, only that there are enough bytes to
// relocate, so MH_CreateHook silently "succeeded" here too -- splicing a
// jmp into the middle of this function's live frame on every call, for
// every actor queried for movement speed (i.e. constantly, for the whole
// game, not just CoSync's proxy). Same corruption mechanism as
// CoSyncLocomotionCalcGuard.cpp's round-89 fix; see that file's comment for
// the full mechanism writeup. The real entry is 0x6C9710 -- at true entry
// rcx/rdx are exactly the (rcx, rdx) this guard's typedef and the
// self/field-offset precheck below already assume (the real function's own
// first instructions are `lea rbx,[rcx-58h]` / `mov rdi,rdx`, matching this
// file's kSelfOffset/kFieldOffset logic exactly), so nothing else here
// needs to change.
//
// disassembly of the live 1.11.221 Fallout4.exe (round 75). Original body:
//   rbx = rcx - 0x58            (rcx==0 -> rbx=0)
//   if (rbx==0) return 0.0f
//   ... a couple of flag-gated special-case branches ...
//   rcx = [rbx+0x418]
//   rdx = <passthrough>
//   rcx += 0xA8
//   jmp Fallout4.exe+0x256B80   <- tail call; that leaf does mov rcx,[rcx+8]
//                                  with zero null/sentinel check
//
// RVA CORRECTED round 13 (this campaign): ground-truth .pdata
// (RUNTIME_FUNCTION table) shows 0x6C9710 is NOT a function entry either --
// it's 0x30 bytes into the real function, which actually starts at
// 0x6C96E0 ([0x6C96E0, 0x6C9726), 0x46 bytes). Round 89's "IDA cross-check"
// was wrong by the exact same 0x30-byte offset as CoSyncLocomotionCalcGuard's
// matching mistake -- this hook's heartbeat has never fired even once in any
// session log, proof the real engine call path never reached our detour.
static constexpr uintptr_t kMovementHandleFn_RVA = 0x006C96E0;
static constexpr uintptr_t kSelfOffset           = 0x58;
static constexpr uintptr_t kFieldOffset          = 0x418;

using MovementHandleFn = float(__fastcall*)(void* rcx, void* rdx);

static MovementHandleFn   g_origFn = nullptr;
static std::atomic<bool>  g_hookInstalled{ false };

// UPDATE 2026-06-21 (round 84): see CoSyncLocomotionCalcGuard.cpp's matching
// comment for the full rationale -- repeatedly calling into the real engine
// function before catching its fault means real partial-execution side
// effects happen on every retry, and once round 82 made the catch itself
// nearly free, the engine's retry loop spun fast enough (~7.5ms/cycle) that
// the live test believed fixed in round 83 actually crashed the process
// right after the captured log window. This actor's +0x418 field can never
// become valid for a remote-controlled proxy, so once a given actor pointer
// has faulted here, never call the real function for that exact pointer
// again this session.
namespace
{
    constexpr int kMaxTrackedFaultedActors = 16;
    std::mutex s_faultedActorsMutex;
    void*      s_faultedActors[kMaxTrackedFaultedActors] = {};
    int        s_faultedActorCount = 0;

    bool HasAlreadyFaulted(void* actor)
    {
        LOG_REQUIREMENT("HasAlreadyFaulted", "Checks whether this actor already crashed the movement-speed call before, so it can be skipped instead of crashing again.");
        std::lock_guard<std::mutex> lk(s_faultedActorsMutex);
        for (int i = 0; i < s_faultedActorCount; ++i)
            if (s_faultedActors[i] == actor)
                return true;
        return false;
    }

    void RecordFault(void* actor)
    {
        LOG_REQUIREMENT("RecordFault", "Remembers an actor that crashed the movement-speed call, so it's never tried again this session.");
        std::lock_guard<std::mutex> lk(s_faultedActorsMutex);
        for (int i = 0; i < s_faultedActorCount; ++i)
            if (s_faultedActors[i] == actor)
                return;
        if (s_faultedActorCount < kMaxTrackedFaultedActors)
            s_faultedActors[s_faultedActorCount++] = actor;
        // round 94 diagnostic: this is the single most important log line for
        // confirming whether the round-90 RVA fix actually eliminated the
        // fault. If this NEVER appears in a play-session log, the fix held.
        LOG_WARN("[MovementHandleGuard] FAULT actor=%p (now permanently skipped, %d/%d tracked)",
            actor, s_faultedActorCount, kMaxTrackedFaultedActors);
    }

    // round 94 diagnostic: periodic heartbeat proves the hook is actually
    // firing and tracks distinct-faulted-actor count without log spam.
    std::atomic<uint64_t> s_callCount{ 0 };
    void LogHeartbeat()
    {
        LOG_REQUIREMENT("LogHeartbeat", "Prints a periodic status line so it's easy to confirm this hook is still running and see how many actors have been skipped.");
        const uint64_t n = ++s_callCount;
        if (n == 1 || n % 5000 == 0)
        {
            int faulted;
            { std::lock_guard<std::mutex> lk(s_faultedActorsMutex); faulted = s_faultedActorCount; }
            LOG_INFO("[MovementHandleGuard] heartbeat: calls=%llu faultedActors=%d",
                (unsigned long long)n, faulted);
        }
    }
}

// UPDATE 2026-06-21 (round 76): round 75's value-pattern pre-check
// (field==0 or top-byte-0xFF sentinel) was too narrow -- the very next live
// test crashed again at the identical RIP/call chain, but this time
// actor+0x418 held a DIFFERENT garbage value (0x000100B400000000, no
// recognizable sentinel shape at all). The field isn't reliably set to one
// known "uninitialized" constant -- for a freshly spawned actor it's just
// whatever bit pattern happened to be sitting in that memory, which can be
// anything. Pattern-matching specific "known bad" values can never be
// complete. Wrap the actual call in SEH instead: __declspec(noinline) is
// load-bearing here (a non-noinline __try/__except can be inlined away
// under this project's /GL+/LTCG Release config, silently dropping the SEH
// scope -- see CoSyncDoorHook.cpp's SafeReadU32/SafeReadPtr for the same
// lesson learned earlier this project) and the function must NOT be
// noexcept (the optimizer can use that contradiction to prove the __try
// never fires and strip it).
__declspec(noinline) static float CallOriginalSafe(MovementHandleFn fn, void* rcx, void* rdx)
{
    LOG_REQUIREMENT("CallOriginalSafe", "Calls the real engine function safely. A freshly spawned actor can have unset internal data here, so this catches the crash instead of letting it take down the game.");
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

static float __fastcall Detour_MovementHandleFn(void* rcx, void* rdx)
{
    LOG_REQUIREMENT("Detour_MovementHandleFn", "Runs every time the game asks an actor for its movement speed. Needs the hook to be installed, and skips actors that are null or already known to crash this call.");
    LogHeartbeat();

    if (!g_origFn)
        return 0.0f;

    if (!rcx)
        return g_origFn(rcx, rdx);

    if (HasAlreadyFaulted(rcx))
        return 0.0f;

    const uint8_t* self = reinterpret_cast<const uint8_t*>(rcx) - kSelfOffset;
    const uint64_t field = *reinterpret_cast<const uint64_t*>(self + kFieldOffset);

    // Cheap fast-path for the specific sentinel shape already confirmed
    // live (top byte 0xFF, rest zero -- Bethesda's "field never
    // initialized" convention, same shape seen on TESObjectREFR+0x7C as
    // 0xFF000000) plus plain null. Anything else falls through to the SEH
    // wrap below, which catches whatever other garbage shows up.
    if (field == 0 || (field >> 56) == 0xFF)
        return 0.0f;

    return CallOriginalSafe(g_origFn, rcx, rdx);
}

void InstallMovementHandleGuard()
{
    LOG_REQUIREMENT("InstallMovementHandleGuard", "Only runs once. Hooks the engine's movement-speed function so a bad actor pointer can't crash the game.");
    if (g_hookInstalled.exchange(true))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kMovementHandleFn_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[MovementHandleGuard] MH_Initialize failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Detour_MovementHandleFn),
        reinterpret_cast<void**>(&g_origFn));
    if (st != MH_OK)
    {
        LOG_ERROR("[MovementHandleGuard] MH_CreateHook failed: %d target=%p", (int)st, target);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK)
    {
        LOG_ERROR("[MovementHandleGuard] MH_EnableHook failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    LOG_INFO("[MovementHandleGuard] Hook installed at %p (RVA 0x%zX)", target, kMovementHandleFn_RVA);
}

void UninstallMovementHandleGuard()
{
    LOG_REQUIREMENT("UninstallMovementHandleGuard", "Only runs if the hook is currently installed. Removes the movement-speed hook cleanly.");
    if (!g_hookInstalled.exchange(false))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kMovementHandleFn_RVA);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    LOG_INFO("[MovementHandleGuard] Uninstalled");
}
