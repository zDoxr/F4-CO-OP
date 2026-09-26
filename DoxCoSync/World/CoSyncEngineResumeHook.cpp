#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncEngineResumeHook.h"

#include "CoSyncRequirementsLog.h"
#include "ConsoleLogger.h"
#include "CoSyncNet.h"

#include "MinHook.h"

#include <atomic>
#include <cstring>

// UPDATE 2026-06-18 (round 18): this is NOT a CoSync feature hook -- it's a
// safety net around a confirmed-crashing ENGINE-INTERNAL function that
// CoSync never calls itself. Reproduced 3/3 (rounds 15-17) at the exact
// same RIP (Fallout4.exe+0x1A7FC54), always within a few ms of a second
// player's WorldSync/TimeSync data finishing, always on the main game
// thread (no other thread active in any of the 3 captured minidumps).
//
// Full call chain, confirmed via IDA decompile + 3 live minidumps resolved
// against the matching PDB:
//   sub_140C2FAD0 (engine startup/focus-management function -- contains
//     CoInitialize, a single-instance FindWindowA/SetForegroundWindow
//     check, and registry access under "Software\Bethesda Softworks\
//     Fallout4") calls, deep within its own body (rva+0xE49) ->
//   sub_140C2F580 ("ResetGuard": if (gate) { sub_140C31160(); sub_140C31370(); } ) ->
//   sub_140C31370 ("Reset_Impl": ~1000-line comprehensive subsystem
//     teardown -- audio/input/UI/scripts/scaleform -- using TLS+0x9C0 as a
//     push/pop "current stage" marker) calls, at stage 66 ->
//   sub_140C38680 (tears down ~25 subsystem singleton pointers, including) ->
//   sub_141A7FA40 (THIS function -- drains two locked arrays of refcounted
//     objects; the second array's per-entry vtable dispatch, slot 7
//     (`call [rax+38h]`), is the literal crash instruction -- a dangling or
//     not-yet-constructed vtable on one entry).
//
// sub_140C30FD0 (called as sub_140C31160 above) is a `while(1)` loop gated
// on `GetActiveWindow() == ourWindow` with `Sleep(50)` -- i.e. this whole
// chain is the engine's WINDOW-FOCUS-LOSS/REGAIN handling, not a "quit to
// menu" or "load save" event. User confirmed alt-tabbing/checking other
// windows (log file, TeamViewer) right around when the second player
// connects in every test so far -- this lines up exactly. CoSync doesn't
// cause this; it's a latent vanilla-engine bug only reachable once a second
// player-type actor exists, which CoSync is what makes possible at all.
// Worth fixing here regardless of exact trigger, since real multiplayer
// sessions will alt-tab too (chat, Discord, TeamViewer for P2).
//
// Hooking the INNERMOST confirmed-understood function (this one, fully
// decompiled, single param) rather than anything further up the chain --
// smallest possible blast radius.
//
// UPDATE 2026-06-18 (round 21): the SEH-wrap-the-original-call approach
// (rounds 18-20) doesn't work and isn't fixable by patching individual
// instructions one at a time. Round 19's VEH context-patch caught the
// FIRST fault cleanly, but round 20 found a SECOND crash later in the same
// array entry's processing, and a widened skip (round 20) then exposed a
// THIRD, distinct crash (AV WRITE @ near-null, inside sub_141B18C70) on a
// DIFFERENT entry of the same array after advancing past the first one --
// i.e. this isn't one bad object, multiple/possibly all entries in this
// array are unsafe once a second player-type actor exists. Chasing each
// new crash site with its own instruction-level redirect has diminishing
// returns and real risk: getting the stack-safety analysis wrong on any
// one of them corrupts the stack silently instead of just crashing.
//
// Real fix: don't let the ORIGINAL function run at all while CoSync has an
// active multiplayer session. This is the engine's window-focus-regain
// subsystem-resume path -- not anything CoSync needs to have happen for
// its own features to work -- so skipping it entirely while connected
// trades "some subsystem (audio/input) might not perfectly resume after
// alt-tab during multiplayer" for "no crash," the same risk tradeoff this
// codebase has made elsewhere (VATS, AnimVerify, player-type animation
// gate: disable the unsafe thing outright rather than chase a perfect
// partial fix). When NOT connected, calls through normally -- exact
// vanilla behavior preserved for single-player / no active session, where
// this crash has never been observed.
// UPDATE 2026-06-18 (round 23 audit): this was wrong the entire time --
// confirmed via IDA that 0x01A7FA40 is 0x120 bytes mid-instruction inside the
// REAL function, which actually starts at 0x01A7F920 (sub_141A7F920, single
// __int64 a1 param, exactly matching this file's EventSinkTeardownFn typedef;
// confirmed real caller is sub_140C384F0, matching the established teardown
// call chain from round 15-17's analysis; decompile shows it draining the
// same two locked arrays of refcounted objects via vtable dispatch, matching
// the crash signature exactly). This is the same mid-instruction-hook bug
// class as the 12-round NpcAnimSync disaster and the 3 quest-objective hooks
// found in this same audit pass -- MinHook "successfully" installs a JMP at
// 0x01A7FA40, but nothing ever calls into that exact byte offset directly, so
// the round-21 "skip teardown while IsConnected()" logic could never have
// engaged no matter how correct its own logic was. Hooking the real entry
// point should make that existing skip-while-connected logic actually run
// for the first time.
static constexpr uintptr_t kEventSinkTeardown_RVA = 0x01A7FDE0;

using EventSinkTeardownFn = __int64(__fastcall*)(__int64 a1);

static EventSinkTeardownFn g_origEventSinkTeardown = nullptr;
static std::atomic<bool>   g_hookInstalled{ false };

__declspec(noinline) static __int64 SafeCallOriginal(__int64 a1)
{
    LOG_REQUIREMENT("(global)::SafeCallOriginal", "Needs the hook's saved original-function pointer to be non-null to actually call through. Wrapped in a crash guard since the real teardown function is known to crash sometimes (leftover dangling pointers).");
    __try
    {
        return g_origEventSinkTeardown(a1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static __int64 __fastcall Detour_EventSinkTeardown(__int64 a1)
{
    LOG_REQUIREMENT("(global)::Detour_EventSinkTeardown", "Needs the hook to already be installed. Skips calling the original function entirely while connected to a session, since it's known to be unsafe with a second player around.");
    if (!g_origEventSinkTeardown)
        return 0;

    // UPDATE 2026-06-18 (round 22): round-21's IsConnected() skip did NOT
    // take effect on live retest -- the crash recurred identically (same
    // RIP, same VEH context-patch WARN line, meaning the original function
    // body still ran in full). s_connected is confirmed set well before
    // this point in the log (OnGNSConnected sets it before even logging
    // "Host transport connected", ~2ms before the crash). Unconditional
    // diagnostic log on every call (this function should be rare -- only
    // fires on real focus-regain events, not every tick) to settle whether
    // the detour runs at all vs. the engine reaching the original bytes
    // through some path that bypasses our patched prologue entirely (the
    // same "secondary entry point" risk already confirmed twice elsewhere
    // in this exact call chain -- sub_140C2FC60/sub_140C31160 both turned
    // out to be offsets inside OTHER functions, not real call targets).
    LOG_WARN("[EngineResumeHook] event-sink teardown hook fired a1=0x%llX "
             "connected=%d", (unsigned long long)a1, (int)CoSyncNet::IsConnected());

    if (CoSyncNet::IsConnected())
    {
        static std::atomic<bool> s_warnedOnce{ false };
        if (!s_warnedOnce.exchange(true))
        {
            LOG_WARN("[EngineResumeHook] skipping the focus-regain teardown while a "
                      "multiplayer session is active — it's known to be unsafe with "
                      "a second player around");
        }
        return 0;
    }

    __int64 result = SafeCallOriginal(a1);
    return result;
}

void InstallEngineResumeSafetyHook()
{
    LOG_REQUIREMENT("(global)::InstallEngineResumeSafetyHook", "Needs the game module base to resolve and the target offset to point at the right verified function. Meant to be called only once, guarded by a flag.");
    if (g_hookInstalled.exchange(true))
        return;

    const uintptr_t base   = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void*           target = reinterpret_cast<void*>(base + kEventSinkTeardown_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[EngineResumeHook] hook system init failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Detour_EventSinkTeardown),
        reinterpret_cast<void**>(&g_origEventSinkTeardown));
    if (st != MH_OK)
    {
        LOG_ERROR("[EngineResumeHook] creating the hook failed: %d target=%p", (int)st, target);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK)
    {
        LOG_ERROR("[EngineResumeHook] enabling the hook failed: %d target=%p", (int)st, target);
        g_hookInstalled.store(false);
        return;
    }

    // Read back the first bytes at the target to confirm MinHook actually
    // wrote a jump there -- round-21 live test showed the original
    // function's body still running in full despite "Hook installed"
    // logging with no errors, so don't just trust MH_OK; verify directly.
    unsigned char patched[8] = {};
    memcpy(patched, target, sizeof(patched));
    LOG_INFO("[EngineResumeHook] hook installed at %p (offset 0x%zX) -- patched bytes: "
             "%02X %02X %02X %02X %02X %02X %02X %02X",
        target, kEventSinkTeardown_RVA,
        patched[0], patched[1], patched[2], patched[3],
        patched[4], patched[5], patched[6], patched[7]);
}

void UninstallEngineResumeSafetyHook()
{
    LOG_REQUIREMENT("(global)::UninstallEngineResumeSafetyHook", "Needs the install step to have already succeeded (guarded by a flag). Recomputes the same target address, which must still be valid to remove the hook cleanly.");
    if (!g_hookInstalled.exchange(false))
        return;

    const uintptr_t base   = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void*           target = reinterpret_cast<void*>(base + kEventSinkTeardown_RVA);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    LOG_INFO("[EngineResumeHook] hook removed");
}

// UPDATE 2026-06-18 (round 28): the hook above tries to skip
// EventSinkTeardown itself while connected, but its diagnostic CALLED log
// line never once appeared across 3 separate live tests despite the target
// bytes being correctly patched (confirmed via readback) -- something
// reaches EventSinkTeardown's internals through a path that bypasses its
// own patched entry point entirely (the same "secondary entry point" risk
// already seen twice elsewhere in this exact chain). Result: 4 separate
// VEH point-patches accumulated in CoSync_Plugin.cpp (0x1A7FC54, 0x1B18C75,
// 0x1B193CA, and a 4th still-uncaught null-pointer EXEC crash), each fixing
// one symptom only to reveal the next a few bytes later in the same
// dangling-object chain -- diminishing returns, as predicted back in round
// 21's own notes.
//
// Instead of continuing to chase individual fault sites inside
// EventSinkTeardown, this hooks ONE LEVEL UP: sub_140C384F0, the function
// that calls EventSinkTeardown as the very first step of its own ~25-
// singleton full-subsystem-teardown sequence (confirmed via IDA decompile
// in round 24 -- every single global it tears down is gated by `if (ptr) {
// ptr->vtbl_call(); ptr = 0; }`, EventSinkTeardown's result is itself
// discarded by the caller). Verified via IDA XrefsTo before committing to
// this target: sub_140C384F0 has exactly ONE real call-type xref (from
// sub_140C311E0, type=17/Ordinary_CallNear) -- the other 3 xrefs are data
// references (something storing its address, not calling through it), so
// there's no other call path that could bypass a hook on its own entry the
// way EventSinkTeardown's hook gets bypassed. Raw bytes at the target are a
// textbook clean prologue (`mov [rsp+18h],rbx; mov [rsp+20h],rbp; push r14;
// sub rsp,20h`), not mid-instruction garbage.
//
// Skipping this function entirely while connected means trading "the
// engine's window-focus-regain teardown might not perfectly reset audio/
// input/UI state after alt-tabbing during a multiplayer session" for "none
// of EventSinkTeardown's whole family of dangling-object crashes can occur
// at all" -- the same risk tradeoff already made for VATS/AnimVerify/the
// player-type animation gate elsewhere in this codebase. When NOT
// connected, calls through normally -- exact vanilla behavior preserved.
static constexpr uintptr_t kFullSubsystemTeardown_RVA = 0x00C384F0;

using FullSubsystemTeardownFn = __int64(__fastcall*)();

static FullSubsystemTeardownFn g_origFullSubsystemTeardown = nullptr;
static std::atomic<bool>       g_teardownHookInstalled{ false };

__declspec(noinline) static __int64 SafeCallOriginalTeardown()
{
    LOG_REQUIREMENT("(global)::SafeCallOriginalTeardown", "Needs the hook's saved original-function pointer to be non-null to actually call through. Wrapped in a crash guard since the whole teardown sequence is known to crash sometimes.");
    __try
    {
        return g_origFullSubsystemTeardown();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static __int64 __fastcall Detour_FullSubsystemTeardown()
{
    LOG_REQUIREMENT("(global)::Detour_FullSubsystemTeardown", "Needs the hook to already be installed. Skips calling the original function entirely while connected to a session, since the whole sequence is known to be unsafe with a second player around.");
    if (!g_origFullSubsystemTeardown)
        return 0;

    if (CoSyncNet::IsConnected())
    {
        static std::atomic<bool> s_warnedOnce{ false };
        if (!s_warnedOnce.exchange(true))
        {
            LOG_WARN("[EngineResumeHook] skipping the full subsystem teardown "
                      "(focus-regain path) entirely while a multiplayer session "
                      "is active — this whole sequence is known to be unsafe "
                      "with a second player around");
        }
        return 0;
    }

    return SafeCallOriginalTeardown();
}

void InstallFullSubsystemTeardownHook()
{
    LOG_REQUIREMENT("(global)::InstallFullSubsystemTeardownHook", "Needs the game module base to resolve and the target offset to point at the right verified function. Meant to be called only once, guarded by a flag.");
    // RE-ENABLED round 105: round 40's "garbage at this address" finding was
    // itself wrong, not the address. Independently re-verified from scratch
    // via IDA (not trusting either round 24's or round 40's old claims):
    // EventSinkTeardown's real, freshly-reconfirmed entry (0x1A7F920) has
    // exactly ONE real caller via ida_funcs/CodeRefsTo -- this exact
    // function, sub_140C384F0. Read its raw bytes directly via
    // ida_bytes.get_bytes (not Capstone re-disassembly, not a manual PE
    // parse): 48 89 5C 24 18 48 89 6C 24 20 41 56 48 83 EC 20, which decodes
    // to precisely `mov [rsp+18h],rbx; mov [rsp+20h],rbp; push r14;
    // sub rsp,20h` -- exactly round 24's original "textbook clean prologue"
    // claim, word for word. This function in turn has exactly one real
    // caller of its own (sub_140C311E0, part of the same TLS-stage-tracked
    // teardown sequence). Round 40's contrary finding was a verification
    // error (the same disassembly-desync defect class found and fixed
    // ~20 times elsewhere this campaign), not a real problem with this RVA.
    if (g_teardownHookInstalled.exchange(true))
        return;

    const uintptr_t base   = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void*           target = reinterpret_cast<void*>(base + kFullSubsystemTeardown_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[EngineResumeHook] (teardown) hook system init failed: %d", (int)st);
        g_teardownHookInstalled.store(false);
        return;
    }

    st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Detour_FullSubsystemTeardown),
        reinterpret_cast<void**>(&g_origFullSubsystemTeardown));
    if (st != MH_OK)
    {
        LOG_ERROR("[EngineResumeHook] (teardown) creating the hook failed: %d target=%p", (int)st, target);
        g_teardownHookInstalled.store(false);
        return;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK)
    {
        LOG_ERROR("[EngineResumeHook] (teardown) enabling the hook failed: %d target=%p", (int)st, target);
        g_teardownHookInstalled.store(false);
        return;
    }

    unsigned char patched[8] = {};
    memcpy(patched, target, sizeof(patched));
    LOG_INFO("[EngineResumeHook] full-teardown hook installed at %p (offset 0x%zX) -- patched bytes: "
             "%02X %02X %02X %02X %02X %02X %02X %02X",
        target, kFullSubsystemTeardown_RVA,
        patched[0], patched[1], patched[2], patched[3],
        patched[4], patched[5], patched[6], patched[7]);
}

void UninstallFullSubsystemTeardownHook()
{
    LOG_REQUIREMENT("(global)::UninstallFullSubsystemTeardownHook", "Needs the install step to have already succeeded (guarded by a flag). Recomputes the same target address, which must still be valid to remove the hook cleanly.");
    if (!g_teardownHookInstalled.exchange(false))
        return;

    const uintptr_t base   = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void*           target = reinterpret_cast<void*>(base + kFullSubsystemTeardown_RVA);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    LOG_INFO("[EngineResumeHook] full-teardown hook removed");
}
