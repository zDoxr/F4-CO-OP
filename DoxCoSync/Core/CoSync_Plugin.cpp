#include "ITypes.h"
#include "CoSyncAddressScan.h"
#include "IErrors.h"
#include "IDebugLog.h"
#include <Windows.h>
#include <DbgHelp.h>
#pragma comment(lib, "DbgHelp.lib")
#include "PluginAPI.h"
#include "f4se_version.h"
#include "Logger.h"
#include "ConsoleLogger.h"
#include "Version.h"
#include "CoSync_Main.h"
#include "CoSyncPrismaUI.h"
#include "CoSyncSaveLoad.h"
#include "CoSyncWorldTransfer.h"
#include "CoSyncProfiles.h"
#include "TickHook.h"
#include "CoSyncMainMenu.h"
#include "CoSyncMenuInject.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncSpawnTasks.h"
#include "CoSyncRuntime.h"
#include "CoSyncWorldSync.h"
#include "CoSyncNpcWorldSync.h"
#include "CoSyncLoadingScreen.h"
#include "CoSyncRequirementsLog.h"
#include "../Animations/CoSyncPlayerAnimation.h"
#include "CoSyncFirstPersonGraph.h"

extern bool RegisterCoSyncPapyrus(VirtualMachine* vm);

// UPDATE 2026-06-20 (round 41): breadcrumb defined in CoSyncNet.cpp, set
// before every FlushPendingXOps-style call in CoSyncNet::Tick. Logged below
// in the CRASH block so the next fault says exactly which call was live
// instead of relying on stale stack-data inference.
extern volatile const char* g_lastTickStep;

// Round 115: diagnostic capture globals defined in TickHook.cpp, set via
// plain assignment (no printf) inside SafeCallNpcAnimSyncOriginal right
// before calling through to the engine's own NpcAnimSync function -- see
// the 0xD322E4 handler below.
extern volatile void* g_npcAnimSync_lastA1;
extern volatile uint32_t g_npcAnimSync_lastActorFormID;
extern volatile uint64_t g_npcAnimSync_callIndex;

// Round 117: CoSyncAnimationUtils.cpp's own Get/Set/SendEvent calls (the same
// actor+0x48 holder vtable / real-native far-calls the engine's NpcAnimSync
// uses internally) are CoSync's most-direct analog to that crashing engine
// function, and were entirely unbreadcrumbed before now.
extern volatile const char* g_animUtils_lastOp;
extern volatile const char* g_animUtils_lastVarName;
extern volatile void* g_animUtils_lastActor;

extern "C"
{
    __declspec(dllexport) F4SEPluginVersionData F4SEPlugin_Version =
    {
        F4SEPluginVersionData::kVersion,

        1,
        "DoxCoSync",
        "Doxr",

        0,
        0,
        // P8 (2026-09-26): only the runtime every hardcoded address was
        // verified against. 1.11.191/.221 were listed too, but the Phase 6
        // IDA sweep showed addresses from those builds land mid-function on
        // .240 and vice versa -- F4SE should refuse to load us elsewhere
        // rather than let us call the wrong code.
        { RUNTIME_VERSION_1_11_240, 0 },

        0,
    };
}

static PluginHandle g_pluginHandle = kPluginHandle_Invalid;
static F4SEMessagingInterface* g_messaging = nullptr;

// UPDATE 2026-06-17: CoSync_VEH is a process-wide VectoredExceptionHandler --
// ANY thread that raises an AV anywhere in the process invokes it, not just
// our own hooks. Without serialization, two threads faulting near-simultaneously
// (confirmed in a live log: our usual NpcAnimSync_Hook cascade racing against
// an unrelated-looking fault in an unnamed/no-unwind-info code region, almost
// certainly Scaleform/GFx JIT'd ActionScript on the UI thread) interleave their
// LOG_ERROR calls line-by-line into the same log file, producing garbled,
// undiagnosable output. A CRITICAL_SECTION is reentrant per-thread (a thread
// already holding it can re-enter without deadlocking, e.g. if logging itself
// faults recursively on the same thread) but still serializes different
// threads against each other.
static CRITICAL_SECTION g_vehLock;

static const char* VEH_ModuleLabel(uintptr_t addr, char* buf, size_t bufsz)
{
    LOG_REQUIREMENT("(global)::VEH_ModuleLabel", "Needs a writable output buffer. The address can be any value, even garbage, since it's only looked up, never read from directly.");
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &hMod) && hMod)
    {
        char path[MAX_PATH];
        GetModuleFileNameA(hMod, path, sizeof(path));
        const char* nm = strrchr(path, '\\');
        nm = nm ? nm + 1 : path;
        _snprintf_s(buf, bufsz, _TRUNCATE, "%s+0x%llX", nm, (unsigned long long)(addr - (uintptr_t)hMod));
    }
    else
    {
        _snprintf_s(buf, bufsz, _TRUNCATE, "<unknown>");
    }
    return buf;
}

// UPDATE 2026-06-19 (round 38): the 0x1A7FC54 recovery path has only ever
// logged a single WARN line, never the real caller chain -- so across the
// whole rounds-9-37 investigation, we've never actually seen WHAT calls
// into EventSinkTeardown at the exact moment of the very first fault in
// this chain. Round 37's thread-affinity fix (moving CoSyncRuntime_TickGameThread
// onto the main thread) did NOT make this crash go away, proving it isn't a
// render-thread/main-thread race after all -- the trigger is deterministic
// and tied to something specific, still unidentified. Extracted the same
// real-unwind call-stack walk the fatal/uncaught path already does into a
// reusable helper so the recovered (non-fatal) 0x1A7FC54 path can log it
// too, before bailing out -- this is the one piece of ground truth that's
// been missing the entire session.
static void VEH_LogCallStack(const CONTEXT& ctx)
{
    LOG_REQUIREMENT("(global)::VEH_LogCallStack", "Needs a real register snapshot taken at or near a crash. Stops walking the call chain safely once it reaches a point with no further information to follow.");
    CONTEXT walkCtx = ctx;
    for (int frame = 0; frame < 24; ++frame)
    {
        char lbl[256];
        VEH_ModuleLabel((uintptr_t)walkCtx.Rip, lbl, sizeof(lbl));
        LOG_ERROR("[CoSync-VEH]   #%-2d RIP=0x%016llX  (%s)", frame,
            (unsigned long long)walkCtx.Rip, lbl);

        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION func = RtlLookupFunctionEntry(walkCtx.Rip, &imageBase, nullptr);
        if (!func)
        {
            LOG_ERROR("[CoSync-VEH]   (no unwind info at this RIP -- leaf frame or "
                      "raw asm without a prologue; stopping walk)");
            break;
        }

        void* handlerData = nullptr;
        DWORD64 establisherFrame = 0;
        RtlVirtualUnwind(
            UNW_FLAG_NHANDLER,
            imageBase,
            walkCtx.Rip,
            func,
            &walkCtx,
            &handlerData,
            &establisherFrame,
            nullptr);

        if (walkCtx.Rip == 0)
            break;
    }
}

// UPDATE 2026-06-18 (round 34): the hand-crafted "jump to a hardcoded
// landing pad inside the loop" recovery (used by the original 0x1A7FC54
// patch since round 19) was just confirmed to be able to HANG the game --
// when multiple consecutive array entries are all corrupted (not just the
// one entry assumed in every earlier round), redirecting back into the
// loop just re-reaches the same broken comparison with the same garbage
// register state forever: a live test produced 1345+ identical
// fault/recover cycles in under half a second, then the process never
// logged again (no crash, no resumed gameplay -- a true hang, confirmed
// dead via Get-Process afterward). A hang is worse than the crash it
// replaced: a crash at least ends the session.
//
// This is a real .pdata/UNWIND_INFO-driven unwind, not a hand-simulated
// one -- it asks the OS's own exception-dispatch metadata (present in
// every MSVC-built x64 function, the same data RtlLookupFunctionEntry/
// RtlVirtualUnwind already use for this file's diagnostic call-stack walk)
// what the CURRENT function's real caller and register state would be if
// it returned right now, and resumes there. Unlike jumping to a specific
// byte offset inside the function, this can't land back inside the same
// loop -- it always exits the function entirely, the same outcome a
// __except handler around the original call would have produced, just
// without needing unwind info to survive a MinHook trampoline (the reason
// this whole VEH approach was chosen in round 19 over plain SEH).
// Returns false (caller should fall through to other handling) if there's
// no unwind info at this RIP (a genuine leaf frame / raw asm) -- in that
// case the simulate-ret pattern used elsewhere in this file is the right
// tool instead.
static bool VEH_TryRealUnwindRecover(EXCEPTION_POINTERS* ep)
{
    LOG_REQUIREMENT("(global)::VEH_TryRealUnwindRecover", "Assumes it's always called from the crash handler with real exception data. Returns false if there isn't enough information to safely recover at the current spot.");
    CONTEXT unwoundCtx = *ep->ContextRecord;
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION func = RtlLookupFunctionEntry(unwoundCtx.Rip, &imageBase, nullptr);
    if (!func)
        return false;

    void* handlerData = nullptr;
    DWORD64 establisherFrame = 0;
    RtlVirtualUnwind(
        UNW_FLAG_NHANDLER,
        imageBase,
        unwoundCtx.Rip,
        func,
        &unwoundCtx,
        &handlerData,
        &establisherFrame,
        nullptr);

    if (unwoundCtx.Rip == 0)
        return false;

    unwoundCtx.Rax = 0; // "this call accomplished nothing" sentinel -- same convention as every other recovery in this file
    *ep->ContextRecord = unwoundCtx;
    return true;
}

static LONG WINAPI CoSync_VEH(EXCEPTION_POINTERS* ep)
{
    LOG_REQUIREMENT("(global)::CoSync_VEH", "Relies on Windows supplying valid crash data when it calls this handler. The lock it uses must already be set up first, and this handler must stay installed process-wide so every thread goes through the same lock.");
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT || code == DBG_PRINTEXCEPTION_C ||
        code == 0x406D1388 || code == 0xE06D7363)
        return EXCEPTION_CONTINUE_SEARCH;

    EnterCriticalSection(&g_vehLock);
    __try
    {

    const CONTEXT& ctx = *ep->ContextRecord;
    uintptr_t rip  = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    uintptr_t base = (uintptr_t)GetModuleHandleA("Fallout4.exe");
    uintptr_t rva  = (base && rip > base) ? (rip - base) : rip;

    // UPDATE 2026-06-18 (round 19): RVA 0x1A7FC54 is the one, fully
    // decompiled, confirmed-reproducible (4/4) `call qword ptr [rax+38h]`
    // inside the engine's window-focus-regain subsystem teardown -- NOT
    // anything CoSync calls directly (SpawnTask::Run/PlaceAtMe never even
    // fires before this crash, confirmed across every capture so far).
    // Tried wrapping the call in SEH via a MinHook detour first (round 18,
    // hash ea37a4c0) -- did NOT catch it. Root cause: x64 SEH dispatch is
    // table-driven (.pdata/UNWIND_INFO) and the search has to walk through
    // EVERY frame between the fault and our __except, including MinHook's
    // dynamically-generated trampoline -- which has no unwind info at all,
    // so the search can't reliably get past it. A VEH runs at the raw
    // kernel exception-dispatch level, before any unwind-table search, so
    // it has no such gap -- patch the context directly instead.
    // UPDATE 2026-06-18 (round 20): round-19's redirect (skip to +0x1A7FC57,
    // just past the single `call qword ptr [rax+38h]`) was confirmed to
    // catch the original fault cleanly (WARN logged, resumed) -- but the
    // SAME corrupted object (`rdi`/`v19`) is touched by several more calls
    // later in this same loop iteration (`sub_141B19560` conditionally,
    // then unconditionally `sub_141B19700` and `sub_141B18CF0`), and the
    // live retest crashed a second time a few instructions later, inside
    // `sub_141B18CF0` (called from Fallout4.exe+0x1A7FC88) -- same root
    // cause, just a different method on the same dangling object. Skipping
    // only the first call wasn't enough. Redirecting all the way to
    // Fallout4.exe+0x1A7FC90 (`add rbx, 28h` -- confirmed via the original
    // disassembly capture as the loop's next-entry advance) skips the
    // ENTIRE rest of this one corrupted array entry's processing instead of
    // just the first call -- the only way to guarantee none of its several
    // remaining unsafe calls run.
    //
    // IMPORTANT: only rva == 0x1A7FC54 (the original fault) is safe to
    // intercept this way. That fault happens while computing the indirect
    // call's target (`[rax+38h]`), i.e. mid-instruction, BEFORE the `call`
    // pushes a return address -- RSP is still exactly where it was in
    // sub_141A7FA40's own frame, so jumping elsewhere in the SAME function
    // needs no stack fixup. The second crash this round (deep inside
    // sub_141B18CF0, called from +0x1A7FC88) is NOT safe to catch the same
    // way -- by then a real `call` has pushed a return address and the
    // callee's own prologue has adjusted RSP further; blindly resetting Rip
    // without also restoring Rsp would corrupt the stack (the next `ret`
    // anywhere in this function would pop garbage). Fixed by never reaching
    // that call at all -- catching only the first, stack-safe fault and
    // skipping straight past the rest of this entry.
    if (code == EXCEPTION_ACCESS_VIOLATION && base && rva == 0x1A7FC54)
    {
        // UPDATE 2026-06-18 (round 34): previously redirected to
        // base+0x1A7FC90 to "skip the rest of this array entry and keep
        // looping" -- worked 4/4 times until a run with multiple
        // consecutive corrupted entries turned that redirect into a true
        // infinite loop (1345+ fault cycles, then a hang -- see the
        // VEH_TryRealUnwindRecover comment above). Switched to a real
        // .pdata-driven unwind that bails out of this function entirely
        // instead of gambling on the loop making progress.
        // UPDATE 2026-06-19 (round 38): log the REAL caller chain before
        // recovering -- this is the one piece of ground truth this whole
        // investigation has never actually captured for this exact fault
        // (every prior round only logged the one-line WARN below). Uses
        // the ORIGINAL, pre-unwind context (captured before
        // VEH_TryRealUnwindRecover mutates ep->ContextRecord in place) so
        // frame #0 is the true fault site and frame #1+ is whatever
        // actually called into EventSinkTeardown this time.
        LOG_ERROR("[CoSync-VEH] 0x1A7FC54 fault -- logging real caller chain before recovery:");
        VEH_LogCallStack(*ep->ContextRecord);

        // UPDATE 2026-06-20 (round 43): log callee-saved registers at THIS
        // exact moment, before VEH_TryRealUnwindRecover mutates the context.
        // Every fatal crash chased since round 40 ends with R13 holding a
        // stale DoxCoSync.dll address (most recently DoxCoSync.dll+0x145F47,
        // a generic _Mtx_unlock return address) that later gets used by
        // completely unrelated engine code (Fallout4.exe's own Present/
        // message-pump chain) on what looks like the long-running main
        // thread. This line answers whether that corruption already exists
        // at the FIRST fault in the chain (implicating something even
        // earlier/external) or only appears later (implicating CoSync's own
        // code running between this recovery and the eventual fatal write --
        // e.g. SpawnRemotePlayerTask::Run's anchor lookup, which the user
        // has now confirmed only runs when P2 joins while P1 is already
        // loaded into a world, exactly the condition that reproduces this).
        {
            const CONTEXT& preCtx = *ep->ContextRecord;
            LOG_ERROR("[CoSync-VEH]   pre-recovery regs: RBX=0x%016llX RBP=0x%016llX "
                      "RDI=0x%016llX RSI=0x%016llX R12=0x%016llX R13=0x%016llX "
                      "R14=0x%016llX R15=0x%016llX",
                      (unsigned long long)preCtx.Rbx, (unsigned long long)preCtx.Rbp,
                      (unsigned long long)preCtx.Rdi, (unsigned long long)preCtx.Rsi,
                      (unsigned long long)preCtx.R12, (unsigned long long)preCtx.R13,
                      (unsigned long long)preCtx.R14, (unsigned long long)preCtx.R15);
        }

        if (VEH_TryRealUnwindRecover(ep))
        {
            LOG_WARN("[CoSync-VEH] Caught a known crash in the game's own focus-regain code "
                     "(Fallout4.exe+0x1A7FC54) -- recovered safely by exiting that function "
                     "instead of crashing or getting stuck in a loop");
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    // UPDATE 2026-06-18 (round 33): a new fault at Fallout4.exe+0x1A7FC9D,
    // AV READ fault=0x100000050. This one is no longer a guess -- the
    // round-32 live ReadProcessMemory byte dump caught its REAL bytes
    // directly from the running process (the static IDA database is
    // confirmed unreliable in this exact region, see round 32 notes):
    //   0x1A7FC9D: cmp qword [rbx+0x20], r12   <- faults here (rbx=0x100000030,
    //                                              so [rbx+0x20]=0x100000050,
    //                                              matching the fault address
    //                                              exactly -- confirms the
    //                                              decode)
    //   0x1A7FCA1: jz  short loc_1A7FC90       <- same landing pad as the
    //                                              fault above, already
    //                                              trusted and live-validated
    //   0x1A7FCA3: jmp loc_1A7FBD0
    // This single `cmp` IS the loop's "have I reached the end of the
    // array?" check, and its own taken-branch target is base+0x1A7FC90 --
    // exactly the same address the fault above already redirects to. Since
    // rbx is corrupted here, we can't know the "correct" answer, but
    // treating it the same way (jump to the already-validated landing pad)
    // is the only available recovery and reuses code with a real track
    // record instead of inventing a new one. `cmp` never writes memory and
    // the stack is untouched (mid-instruction fault, no call pushed) -- a
    // same-function-style redirect is stack-safe here.
    //
    // Real-unwind call stack for this fault traces through
    // gameoverlayrenderer64.dll and Fallout4.exe's own Present chain, NOT
    // the network/connect thread -- meaning this shared array-walk code is
    // reached concurrently from the RENDER thread (almost certainly via
    // CoSync's own per-frame IsMenuOpen check, which round 26 confirmed
    // shares this exact lock/array with EventSinkTeardown) at the same
    // time the connect-sequence thread is draining the array. That also
    // explains why fault #1A7FC54 sometimes fires twice in one log: it's
    // two different threads hitting the same shared instruction, not one
    // function looping twice.
    if (code == EXCEPTION_ACCESS_VIOLATION && base && rva == 0x1A7FC9D)
    {
        // UPDATE 2026-06-18 (round 34): the redirect-to-0x1A7FC90 fix below
        // (added round 33) was confirmed live to be able to spin forever --
        // when several consecutive array entries are all corrupted, jumping
        // back into the loop just re-reaches this same `cmp` with the same
        // garbage rbx every time (1345+ cycles in one capture, then a hang,
        // no crash, process found dead afterward). Switched to the same
        // real-unwind bailout as the 0x1A7FC54 fault -- exits the function
        // for good instead of gambling that the next entry is clean.
        if (VEH_TryRealUnwindRecover(ep))
        {
            LOG_WARN("[CoSync-VEH] Caught a known crash reading a stale array "
                     "(Fallout4.exe+0x1A7FC9D) -- recovered safely by exiting that function "
                     "instead of crashing or getting stuck in a loop");
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    // UPDATE 2026-06-18 (round 26): a SECOND, different crash found
    // immediately downstream of the one above (same test, ~3ms later):
    // Fallout4.exe+0x1B18C75, AV type=WRITE fault=0x9 -- a near-null `this`
    // pointer, decompiled to the entry of an IMenu-style constructor that
    // writes through its first parameter with no stack-frame setup of its
    // own (a true leaf call -- RSP at the fault still holds the genuine
    // return address its caller pushed, untouched). IDA-confirmed this
    // function and the one above (EventSinkTeardown) share the same
    // internal lock (unk_143DA6A70) and the same release helper
    // (sub_141B18BD0) that CoSyncDialogSync.cpp's every-tick IsMenuOpen call
    // also uses -- strong evidence both faults trace back to the SAME
    // underlying corrupted list, just touched by two different call paths a
    // few ms apart: skipping the first fault didn't repair that list, so the
    // very next normal touch of it crashed too.
    //
    // Unlike the fault above, we don't know this function's internals well
    // enough to jump to a specific safe address inside it -- but because
    // it's a genuine leaf call with no prologue executed yet, we can safely
    // simulate a `ret` instead: pop the real return address off the stack
    // into Rip, restore Rsp to match, and force Rax=0 so the caller sees
    // "construction failed" -- the same defensive-sentinel idea as the fault
    // above, just unwinding one frame instead of redirecting within it.
    if (code == EXCEPTION_ACCESS_VIOLATION && base && rva == 0x1B18C75)
    {
        uintptr_t retAddr = *reinterpret_cast<uintptr_t*>(ep->ContextRecord->Rsp);
        LOG_WARN("[CoSync-VEH] Caught a known crash touching a stale menu object "
                 "(Fallout4.exe+0x1B18C75) -- returning safely to the caller (0x%llX) "
                 "instead of crashing",
                 (unsigned long long)retAddr);
        ep->ContextRecord->Rip = retAddr;
        ep->ContextRecord->Rsp += 8;
        ep->ContextRecord->Rax = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // UPDATE 2026-06-18 (round 27, continued): a THIRD crash, immediately
    // downstream of the second one's recovery, same test:
    // Fallout4.exe+0x1B193CA, AV READ fault=0x41. Caller (one real-unwound
    // frame up) resolved to Fallout4.exe+0x1A7FC2A -- still inside
    // EventSinkTeardown's own bounds -- confirming this whole chain is one
    // connected family of symptoms off the same dangling/corrupted object,
    // not unrelated crashes. Decompiled the faulting function
    // (sub_141B19300, a small state-flag-check helper with a normal
    // prologue/epilogue, NOT a leaf call like the second fault): the branch
    // that reaches this RVA is a pure sequence of `cmp byte ptr [rdi+N], 0`
    // reads off whatever dangling object got passed in as a1/rdi, with no
    // further calls -- and critically, this exact function ALREADY HAS its
    // own "give up, return false" exit a few bytes away (Fallout4.exe+0x1B193A8,
    // the same destination several of this function's own `jz short` checks
    // already jump to whenever an earlier flag check comes back unexpected).
    // Redirecting there is safer than the previous fix: it's not a
    // hand-simulated return, it's the function's OWN real epilogue (restores
    // rbx/rsi, fixes rsp, pops rdi, retn) running exactly as the compiler
    // wrote it -- we're just making it take the "this object isn't in the
    // state I expected" exit instead of touching the field that's actually
    // dangling. No manual Rax patch needed; that real code path already
    // zeroes it itself.
    if (code == EXCEPTION_ACCESS_VIOLATION && base && rva == 0x1B193CA)
    {
        LOG_WARN("[CoSync-VEH] Caught a known crash touching a stale object "
                 "(Fallout4.exe+0x1B193CA) -- sending it to that function's own safe exit "
                 "(Fallout4.exe+0x1B193A8) instead of crashing");
        ep->ContextRecord->Rip = base + 0x1B193A8;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // UPDATE 2026-06-18 (round 30): a FIFTH crash, same overall family,
    // different shape: Fallout4.exe+0x1B193E9, AV READ fault=0x1000000E0
    // (not near-null this time -- a garbage-but-plausible-looking pointer
    // value). Same function as fault #3 (sub_141B19300, RVA
    // 0x1B19300-0x1B19438) but a different branch: per the earlier
    // decompile, this is `*a3 == *(_QWORD*)(a1+72)` -- the corrupted value
    // this time is the `a3` parameter itself (dereferencing it directly
    // faults at exactly the value it holds), not the `a1`/`rdi` object from
    // fault #3. Same fix applies though: this function's own "give up,
    // return false" exit (Fallout4.exe+0x1B193A8) is the single shared
    // destination for every failure path in this function regardless of
    // which check failed, so redirecting here is correct for this fault too.
    if (code == EXCEPTION_ACCESS_VIOLATION && base && rva == 0x1B193E9)
    {
        LOG_WARN("[CoSync-VEH] Caught a known crash touching a stale object "
                 "(Fallout4.exe+0x1B193E9) -- sending it to that function's own safe exit "
                 "(Fallout4.exe+0x1B193A8) instead of crashing");
        ep->ContextRecord->Rip = base + 0x1B193A8;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // UPDATE 2026-07-27: BGSSaveLoadGame::LoadGame per-reference extra-data
    // crash, the recurring death-screen "Continue" reload fatal (see project
    // memory: LoadGame ExtraAliasInstanceArray crash). rva == 0x2AA94C is
    // ExtraAliasInstanceArray::ExtraAliasInstanceArray (IDA disk-IDB
    // decompile + live-byte cross-check both confirm the instruction: `mov
    // byte ptr [rcx+0x12], 0x88`, offset 0xC into the ctor at 0x2AA940) --
    // faults WRITE when the reference's extraData slot passed in as `this`
    // is corrupted (root cause: a CoSync force-persisted proxy actor that
    // doesn't round-trip save/load cleanly). First tried a MinHook detour +
    // __try/__except around the original call (CoSyncExtraAliasCtorGuard.cpp,
    // now removed) -- did NOT reliably catch it, reproducing this exact
    // codebase's OWN round-18 finding above: MinHook's trampoline has no
    // .pdata/UNWIND_INFO, so the SEH search can't always walk through it.
    // VEH_TryRealUnwindRecover uses the REAL unwind metadata for this
    // (normally-compiled, non-trampoline) function to bail out to its actual
    // caller with Rax=0 -- same proven tool already used for the
    // EventSinkTeardown family below.
    if (code == EXCEPTION_ACCESS_VIOLATION && base && rva == 0x2AA94C)
    {
        if (VEH_TryRealUnwindRecover(ep))
        {
            LOG_WARN("[CoSync-VEH] Caught a known crash loading corrupted save data "
                     "(Fallout4.exe+0x2AA94C, during LoadGame) -- recovered safely by "
                     "exiting that setup step instead of crashing");
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    // UPDATE 2026-07-27: sibling crash to the one above, same root cause
    // (corrupted CoSync proxy actor), different saved field -- an actor's AI
    // package-override restore. rva == 0x70295F is the leaf allocator
    // (BGSNamedPackageData<IPackageDataAIWorldLocationHandle>/
    // BGSPackageDataPointerTemplate/BGSPackageDataLocation construction,
    // called several frames below the per-actor package-override loader at
    // RVA 0xCEA0A0) where the WRITE AV actually lands, per IDA decompile of
    // the live crash capture. Same VEH_TryRealUnwindRecover treatment --
    // this function has real unwind info (it's normally compiled, not a
    // trampoline), so bailing out to its caller is safe and sufficient.
    if (code == EXCEPTION_ACCESS_VIOLATION && base && rva == 0x70295F)
    {
        if (VEH_TryRealUnwindRecover(ep))
        {
            LOG_WARN("[CoSync-VEH] Caught a known crash loading a corrupted actor's AI data "
                     "(Fallout4.exe+0x70295F, during LoadGame) -- recovered safely by "
                     "exiting that setup step instead of crashing");
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    // UPDATE 2026-06-18 (round 29): a FOURTH crash, same chain, same test --
    // RIP=0x0 exactly, AV type=EXEC fault=0x0. Confirmed the round-28
    // higher-level hook (blocking sub_140C384F0 entirely while connected)
    // did NOT prevent this -- its own diagnostic log line never appeared,
    // meaning EventSinkTeardown is reached by some path other than the one
    // that hook blocks. (Leading new theory, not yet confirmed: round 24's
    // own XrefsTo dump for EventSinkTeardown itself, not sub_140C384F0,
    // showed a SECOND xref besides sub_140C384F0 -- a type=1 DATA reference
    // at 0x144039454, i.e. EventSinkTeardown's address is also stored as a
    // raw function pointer somewhere and likely called *indirectly* from
    // there, completely bypassing both the entry hook and the higher-level
    // block.)
    //
    // UPDATE 2026-06-18 (round 31): a SIXTH crash showed up keyed to RIP=
    // 0x2463CE39E60 -- AV type=EXEC, but NOT literally 0 this time, and the
    // address lives in a private heap region (MEM_PRIVATE, no unwind info,
    // leaf frame) that has nothing to do with Fallout4.exe or DoxCoSync.dll.
    // The real-unwound stack still shows the same callers as every fault
    // before it (Fallout4.exe+0x1A7FC2A inside EventSinkTeardown,
    // Fallout4.exe+0x1A7FD40 from the IsMenuOpen helper) -- same root cause,
    // same corrupted-vtable-dispatch family, just a DIFFERENT garbage value
    // sitting in the vtable slot this run. A 7th point-patch keyed to this
    // exact heap address would be useless -- it's a heap allocation, it'll
    // be a different number next run. The narrow `rip == 0` check below was
    // only ever a special case of a more general, fully self-describing
    // signature: AV type=EXEC means "the CPU tried to fetch an instruction
    // from memory that isn't valid code," which is *exactly* what every
    // indirect call through a corrupted/dangling vtable pointer looks like,
    // no matter what garbage value that pointer happens to hold. Broadened
    // the condition from `rip == 0` to "any EXEC-type AV" so this same
    // recovery now catches this whole class generically instead of needing
    // a new patch every time the heap happens to put a different garbage
    // value in that slot.
    //
    // The recovery itself is unchanged: a `call` through a bad function
    // pointer still completes the implicit push of the return address as
    // part of the `call` mechanism before faulting on the fetch at the
    // (invalid) target -- so by the time this handler runs, [RSP] already
    // holds the genuine return address, exactly like fault #2 above. Same
    // recovery: simulate the `ret` the never-executed function would have
    // done, forcing Rax=0 (the same "this call accomplished nothing"
    // sentinel convention used by every other recovery in this codebase).
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 1 &&
        ep->ExceptionRecord->ExceptionInformation[0] == 8)
    {
        uintptr_t retAddr = *reinterpret_cast<uintptr_t*>(ep->ContextRecord->Rsp);

        // round 33 (P2 fatal crash, ClientTickObjectMoves_Impl ->
        // ApplyFurnitureToProxy): this blanket "any EXEC AV -> fake a ret"
        // recovery was broadened in round 31 to cover EventSinkTeardown/
        // IsMenuOpen's garbage-vtable-pointer family (Fallout4.exe-side code
        // with NO function-level SEH guard of its own). But the VEH always
        // runs before any frame-based __except gets a chance, so this same
        // unconditional catch was *also* hijacking faults inside functions
        // that DO have their own, more-correct __try/__except a few frames
        // up (e.g. ApplyFurnitureToProxy/ClientTickObjectMovesSafe) -- this
        // exact crash's RSI register (frozen across the whole cascade, since
        // the fake-ret never executes real code to change it) resolved via
        // dbghelp to ClientTickObjectMoves_Impl/ApplyFurnitureToProxy inside
        // DoxCoSync.dll, confirming the immediate caller was our own
        // already-guarded code, not bare Fallout4.exe. A single fake `ret`
        // only undoes ONE call level -- if the real fault is several calls
        // deep (e.g. inside DYNAMIC_CAST/LookupFormByID), that's nowhere
        // near enough to reach the correct recovery point, which is exactly
        // why this kept cascading through more and more garbage RIPs instead
        // of settling. Fix: only apply this generic recovery when the
        // candidate return address is Fallout4.exe-side code (the original,
        // still-valid use case) -- if it's inside DoxCoSync.dll itself, let
        // the exception keep propagating (CONTINUE_SEARCH) so the real,
        // local __except up our own call stack gets the chance to do a
        // proper full unwind instead of this one-level hack.
        {
            HMODULE retMod = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)retAddr, &retMod);
            char retModPath[MAX_PATH] = {};
            if (retMod)
                GetModuleFileNameA(retMod, retModPath, sizeof(retModPath));
            if (retMod && strstr(retModPath, "DoxCoSync.dll") != nullptr)
            {
                // 2026-08-04: log the MODULE-RELATIVE offset too. The absolute
                // address is useless across runs (ASLR), but base+RVA can be
                // resolved against the linker .map to name the exact function
                // that made the bad call. Repeatedly guessing which recent
                // change caused a crash is what this replaces.
                LOG_WARN("[CoSync-VEH] Crash return address points back into our own code "
                         "(0x%llX, DoxCoSync.dll+0x%llX) -- skipping the generic recovery here "
                         "and letting that code's own local safety guard handle it",
                         (unsigned long long)retAddr,
                         (unsigned long long)(retAddr - (uintptr_t)retMod));
                // NOTE: do not LeaveCriticalSection here -- this whole block sits inside
                // the __try guarded by a __finally { LeaveCriticalSection(&g_vehLock); }
                // further down (see end of this function), which already runs
                // automatically on every exit path out of the __try, including this
                // early return. An explicit call here would double-release the lock.
                return EXCEPTION_CONTINUE_SEARCH;
            }
        }

        // Name the module (and module-relative offset) that the fake-return
        // will hand control back to. Without this the log only ever shows raw
        // ASLR'd addresses, which cannot be tied to a function after the fact.
        {
            HMODULE m = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)retAddr, &m);
            char p[MAX_PATH] = {};
            if (m) GetModuleFileNameA(m, p, sizeof(p));
            const char* leaf = p[0] ? strrchr(p, '\\') : nullptr;
            LOG_ERROR("[CoSync-VEH]   retAddr module: %s + 0x%llX",
                      leaf ? leaf + 1 : (p[0] ? p : "<unknown>"),
                      m ? (unsigned long long)(retAddr - (uintptr_t)m) : 0ull);
        }

        LOG_WARN("[CoSync-VEH] Caught a known bad-function-pointer crash (RIP=0x%llX) "
                 "-- returning safely to the caller (0x%llX) instead of crashing",
                 (unsigned long long)rip, (unsigned long long)retAddr);
        // UPDATE 2026-06-20 (round 43): same callee-saved-register snapshot
        // as the 0x1A7FC54 fault above -- this fault is the SECOND event in
        // the chain (fires immediately after 0x1A7FC54 in every log so far),
        // so logging R13 etc. here too gives a second timeline data point
        // for narrowing down exactly when the stale-register corruption
        // first appears.
        LOG_ERROR("[CoSync-VEH]   pre-recovery regs: RBX=0x%016llX RBP=0x%016llX "
                  "RDI=0x%016llX RSI=0x%016llX R12=0x%016llX R13=0x%016llX "
                  "R14=0x%016llX R15=0x%016llX",
                  (unsigned long long)ep->ContextRecord->Rbx, (unsigned long long)ep->ContextRecord->Rbp,
                  (unsigned long long)ep->ContextRecord->Rdi, (unsigned long long)ep->ContextRecord->Rsi,
                  (unsigned long long)ep->ContextRecord->R12, (unsigned long long)ep->ContextRecord->R13,
                  (unsigned long long)ep->ContextRecord->R14, (unsigned long long)ep->ContextRecord->R15);
        ep->ContextRecord->Rip = retAddr;
        ep->ContextRecord->Rsp += 8;
        ep->ContextRecord->Rax = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Round 113/114/115/116: rounds 113-115 all gated this diagnostic on one
    // specific RVA (Fallout4.exe+0xD322E4, a callee reached from inside
    // NpcAnimSync). Round 116's live retest faulted at a DIFFERENT RVA
    // (Fallout4.exe+0xCA9608 -- `lock xadd [rcx+8],eax` with rcx==garbage
    // 0xFFFF0002, an AddRef-style refcount bump, this time INSIDE
    // NpcAnimSync itself rather than a callee) -- so the rva==0xD322E4 gate
    // never matched and the capture never got logged at all, even though
    // TickHook.cpp's globals were still being set every call. Lesson: gating
    // a breadcrumb on one specific instruction address is fragile whenever
    // the same underlying bad state can manifest as a fault at more than one
    // point in the same function. Moved this log to the general crash report
    // below (always runs, regardless of which exact RIP faulted) instead of
    // chasing each new RVA with another narrow condition.

    // UPDATE 2026-06-21 (round 78): rounds 76/77's SEH guards (CallOriginalSafe
    // in CoSyncMovementHandleGuard.cpp / CoSyncLocomotionCalcGuard.cpp) DO
    // catch these specific faults -- the process no longer terminates -- but
    // the engine re-enters the exact same broken code path for the same
    // under-initialized actor on essentially every tick (both the main
    // thread and a background worker thread), and every single occurrence
    // was paying the full cost of this function: a real-unwind stack walk
    // plus a fresh MiniDumpWriteDump to disk (~230-250ms each, confirmed via
    // log timestamps). At tick rate that reads as the game hitching/freezing
    // even though nothing actually crashes -- 1869 log lines and ~30 dump
    // writes in under 8 seconds from one test. The diagnostic value of the
    // 50th identical dump for the same RIP is zero; cap it so repeat
    // occurrences log one cheap line instead of repeating all of this.
    // g_vehLock is already held for the duration of this function (see the
    // EnterCriticalSection/__finally pair around the whole handler body),
    // so this static table needs no locking of its own.
    {
        struct RipCount { uintptr_t rip; int count; };
        static RipCount s_seen[16] = {};
        static int s_seenCount = 0;
        constexpr int kMaxFullReportsPerRip = 3;

        RipCount* slot = nullptr;
        for (int i = 0; i < s_seenCount; ++i)
        {
            if (s_seen[i].rip == rip) { slot = &s_seen[i]; break; }
        }
        if (!slot && s_seenCount < 16)
        {
            slot = &s_seen[s_seenCount++];
            slot->rip = rip;
            slot->count = 0;
        }

        if (slot)
        {
            ++slot->count;
            if (slot->count > kMaxFullReportsPerRip)
            {
                char ripLabel[256];
                VEH_ModuleLabel(rip, ripLabel, sizeof(ripLabel));
                LOG_ERROR("[CoSync-VEH] CRASH (repeat #%d, suppressing full report+minidump) "
                          "code=0x%08X RIP=0x%016llX (%s) lastTickStep=%s",
                          slot->count, code, (unsigned long long)rip, ripLabel,
                          g_lastTickStep ? g_lastTickStep : "<null>");
                return EXCEPTION_CONTINUE_SEARCH;
            }
        }
    }

    // -- region type at crash RIP --
    MEMORY_BASIC_INFORMATION mbi = {};
    VirtualQuery((LPCVOID)rip, &mbi, sizeof(mbi));
    const char* regionType = "?";
    if      (mbi.Type == MEM_IMAGE)   regionType = "MEM_IMAGE(FO4/DLL)";
    else if (mbi.Type == MEM_PRIVATE) regionType = "MEM_PRIVATE(pool)";
    else if (mbi.Type == MEM_MAPPED)  regionType = "MEM_MAPPED";

    char ripLabel[256];
    VEH_ModuleLabel(rip, ripLabel, sizeof(ripLabel));

    LOG_ERROR("[CoSync-VEH] CRASH code=0x%08X  region=%s", code, regionType);
    LOG_ERROR("[CoSync-VEH]   lastTickStep=%s", g_lastTickStep ? g_lastTickStep : "<null>");
    LOG_ERROR("[CoSync-VEH]   RIP=0x%016llX  (%s)", (unsigned long long)rip, ripLabel);
    LOG_ERROR("[CoSync-VEH]   AllocBase=0x%016llX  RegionBase=0x%016llX  RegionSize=0x%llX",
        (unsigned long long)mbi.AllocationBase,
        (unsigned long long)mbi.BaseAddress,
        (unsigned long long)mbi.RegionSize);
    // Round 116: log unconditionally instead of gating on a specific RVA --
    // see the comment above where this used to be a narrow rva==0xD322E4
    // check. Always cheap (3 plain reads), only meaningful when the crash is
    // actually related to NpcAnimSync, harmless noise otherwise.
    LOG_ERROR("[CoSync-VEH]   NpcAnimSync diagnostic: lastA1=%p lastActorFormID=0x%08X callIndex=%llu",
        (void*)g_npcAnimSync_lastA1,
        g_npcAnimSync_lastActorFormID,
        (unsigned long long)g_npcAnimSync_callIndex);
    // Round 117: same always-on, unconditional convention as the block above
    // -- CoSyncAnimationUtils.cpp's Get/Set/SendEvent calls are CoSync's own
    // closest analog to the engine's NpcAnimSync call, now breadcrumbed too.
    LOG_ERROR("[CoSync-VEH]   AnimUtils diagnostic: lastOp=%s lastVarName=%s lastActor=%p",
        g_animUtils_lastOp ? g_animUtils_lastOp : "<null>",
        g_animUtils_lastVarName ? g_animUtils_lastVarName : "<null>",
        (void*)g_animUtils_lastActor);

    // UPDATE 2026-06-18 (round 32): this exact function's tail (where every
    // point-patch in this file lives) has now been confirmed UNRELIABLE in
    // our static IDA database at least 4 separate times -- most recently,
    // the decompiled/disassembled bytes at 0x1A7FC54 itself show a harmless
    // `lea`/direct-`call` sequence that cannot fault, yet the live game has
    // crashed there 4+ times with a clean AV signature. There is no
    // debugger (WinDbg/cdb) on this machine to get a live disassembly, so
    // the only ground truth available for any FUTURE uncaught crash in this
    // region is the live process's own memory -- dump it directly via
    // ReadProcessMemory (safe even if the page turns out to be unreadable;
    // it fails gracefully instead of faulting again) rather than trusting
    // IDA's static read of this address range.
    {
        unsigned char ripBytes[32] = {};
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)rip, ripBytes, sizeof(ripBytes), &bytesRead) && bytesRead > 0)
        {
            char hex[3 * sizeof(ripBytes) + 1] = {};
            char* p = hex;
            for (SIZE_T i = 0; i < bytesRead; ++i)
            {
                p += _snprintf_s(p, hex + sizeof(hex) - p, _TRUNCATE, "%02X ", ripBytes[i]);
            }
            LOG_ERROR("[CoSync-VEH]   Live bytes at RIP (ground truth, not from IDA): %s", hex);
        }
        else
        {
            LOG_ERROR("[CoSync-VEH]   Live bytes at RIP: ReadProcessMemory failed (page likely unmapped)");
        }
    }

    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
    {
        uintptr_t faultAddr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        char faultLabel[256];
        VEH_ModuleLabel(faultAddr, faultLabel, sizeof(faultLabel));
        LOG_ERROR("[CoSync-VEH]   AV type=%s  fault=0x%016llX  (%s)",
            ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "READ" :
            ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "WRITE" : "EXEC",
            (unsigned long long)faultAddr, faultLabel);
    }

    // -- full register dump --
    LOG_ERROR("[CoSync-VEH] Registers:");
    LOG_ERROR("[CoSync-VEH]   RAX=0x%016llX  RBX=0x%016llX  RCX=0x%016llX  RDX=0x%016llX",
        (unsigned long long)ctx.Rax, (unsigned long long)ctx.Rbx,
        (unsigned long long)ctx.Rcx, (unsigned long long)ctx.Rdx);
    LOG_ERROR("[CoSync-VEH]   RSI=0x%016llX  RDI=0x%016llX  RBP=0x%016llX  RSP=0x%016llX",
        (unsigned long long)ctx.Rsi, (unsigned long long)ctx.Rdi,
        (unsigned long long)ctx.Rbp, (unsigned long long)ctx.Rsp);
    LOG_ERROR("[CoSync-VEH]   R8 =0x%016llX  R9 =0x%016llX  R10=0x%016llX  R11=0x%016llX",
        (unsigned long long)ctx.R8,  (unsigned long long)ctx.R9,
        (unsigned long long)ctx.R10, (unsigned long long)ctx.R11);
    LOG_ERROR("[CoSync-VEH]   R12=0x%016llX  R13=0x%016llX  R14=0x%016llX  R15=0x%016llX",
        (unsigned long long)ctx.R12, (unsigned long long)ctx.R13,
        (unsigned long long)ctx.R14, (unsigned long long)ctx.R15);

    // UPDATE 2026-06-17: added a real call-stack walk using
    // RtlLookupFunctionEntry/RtlVirtualUnwind. The old "StackWalk64 fails
    // without PDB" reasoning was a misunderstanding -- on x64, unwinding
    // walks the PE's embedded .pdata/UNWIND_INFO tables (present in every
    // MSVC-built x64 binary, required for SEH to work at all), not a PDB.
    // A PDB is only needed to turn addresses into *names*; the unwind itself
    // needs no symbol info, no SymInitialize, nothing beyond ntdll exports
    // that come for free with Windows.h. This gives an actually-reliable
    // call chain instead of guessing from raw stack bytes, which mixes real
    // return addresses with unrelated leftover local-variable garbage that
    // happens to look like a code pointer.
    LOG_ERROR("[CoSync-VEH] Call stack (real unwind):");
    {
        CONTEXT walkCtx = ctx;
        for (int frame = 0; frame < 24; ++frame)
        {
            char lbl[256];
            VEH_ModuleLabel((uintptr_t)walkCtx.Rip, lbl, sizeof(lbl));
            LOG_ERROR("[CoSync-VEH]   #%-2d RIP=0x%016llX  (%s)", frame,
                (unsigned long long)walkCtx.Rip, lbl);

            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION func = RtlLookupFunctionEntry(walkCtx.Rip, &imageBase, nullptr);
            if (!func)
            {
                LOG_ERROR("[CoSync-VEH]   (no unwind info at this RIP -- leaf frame or "
                          "raw asm without a prologue; stopping walk)");
                break;
            }

            void* handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(
                UNW_FLAG_NHANDLER,
                imageBase,
                walkCtx.Rip,
                func,
                &walkCtx,
                &handlerData,
                &establisherFrame,
                nullptr);

            if (walkCtx.Rip == 0)
                break;
        }
    }

    // -- raw RSP stack dump (kept as a supplementary fallback) --
    LOG_ERROR("[CoSync-VEH] Stack (RSP):");
    const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(ctx.Rsp);
    for (int i = 0; i < 32; i++)
    {
        uintptr_t val = 0;
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &val, sizeof(val), &read) || read != sizeof(val))
            break;
        char lbl[256];
        VEH_ModuleLabel(val, lbl, sizeof(lbl));
        LOG_ERROR("[CoSync-VEH]   [RSP+%03X] 0x%016llX  %s", (unsigned)(i * 8), (unsigned long long)val, lbl);
    }

    // -- minidump --
    // UPDATE 2026-06-21 (round 81): every full report (up to kMaxFullReportsPerRip
    // per RIP) was writing its own MiniDumpWriteDump to the SAME fixed filename,
    // so occurrences #2/#3 just overwrote occurrence #1's dump for zero added
    // diagnostic value -- yet each write cost ~200-250ms, and a single burst of
    // 3 distinct full reports (e.g. one RIP hit 3x in under a second, the
    // observed pattern) measured as an ~800ms hitch from this alone. The
    // register dump + real-unwind stack walk above are cheap (microseconds,
    // just memory reads + log lines) and stay uncapped per the existing
    // per-RIP limiter; only the actual disk-writing minidump is now capped to
    // once per process lifetime, since later dumps add nothing the first one
    // didn't already capture.
    // UPDATE 2026-06-21 (round 82): rounds 73-81 fully root-caused both RVAs
    // below (Bethesda's locomotion calculator / movement-handle function
    // dereferencing a freshly-PlaceAtMe-spawned proxy actor's not-yet-settled
    // sub-objects; call chain, registers, and live bytes all already on file
    // across many prior session logs) -- another minidump for either one
    // adds no new forensic information, yet still costs ~250-300ms of disk
    // I/O on the very first occurrence even after round 81's per-session cap.
    // Skip the dump outright for these two known addresses; still capture one
    // for any other (i.e. genuinely new/unknown) crash RVA.
    constexpr uintptr_t kKnownLocomotionCalcRVA = 0xCC934D;
    constexpr uintptr_t kKnownMovementHandleRVA = 0x256B80;
    const uintptr_t crashRVA = rip - base;
    const bool isKnownDiagnosedCrash =
        (crashRVA == kKnownLocomotionCalcRVA) || (crashRVA == kKnownMovementHandleRVA);

    static bool s_minidumpWritten = false;
    if (isKnownDiagnosedCrash)
    {
        LOG_ERROR("[CoSync-VEH] Minidump skipped (RVA 0x%llX already fully diagnosed, rounds 73-81)",
            (unsigned long long)crashRVA);
    }
    else if (!s_minidumpWritten)
    {
        HANDLE hFile = CreateFileA(
            "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Fallout 4\\Data\\F4SE\\Plugins\\CoSync_crash.dmp",
            GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION mei{ GetCurrentThreadId(), ep, FALSE };
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                MiniDumpWithDataSegs, &mei, nullptr, nullptr);
            CloseHandle(hFile);
            LOG_ERROR("[CoSync-VEH] Minidump -> CoSync_crash.dmp");
            s_minidumpWritten = true;
        }
    }
    else
    {
        LOG_ERROR("[CoSync-VEH] Minidump skipped (already captured one this session)");
    }

    }
    __finally
    {
        LeaveCriticalSection(&g_vehLock);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

// Save filter predicate (CoSyncSaveLoad): true only for references CoSync
// spawned itself (runtime-created 0xFF formID) that the player manager tracks
// as a proxy -- never for a real placed NPC the host merely syncs.
static bool IsCoSyncProxyRef(TESObjectREFR* ref)
{
    if (!ref || (ref->formID >> 24) != 0xFF)
        return false;
    for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
    {
        const CoSyncPlayer* p = kv.second.get();
        if (p && !p->isLocalPlayer && reinterpret_cast<TESObjectREFR*>(p->actorRef) == ref)
            return true;
    }
    return false;
}

void OnF4SEMessage(F4SEMessagingInterface::Message* msg)
{
    LOG_REQUIREMENT("(global)::OnF4SEMessage", "Safely ignores an empty message. When the game finishes loading, it assumes it's now safe to set up the rest of CoSync's systems.");
    if (!msg)
        return;

    switch (msg->type)
    {
    case F4SEMessagingInterface::kMessage_GameLoaded:
        LOG_INFO("GameLoaded::Initializing CoSync...");

        cosync::CoSync_Main::Get().Init();

        CoSyncWorldSync::Init();


        LOG_INFO("CoSync: Installing TickHook...");
        // Round 123 bisection: ruled out (re-enabled round 125,
        // NpcAnimSync_Hook confirmed as the sole cause -- this was never it).
        InstallTickHook();

        LOG_INFO("CoSync: Installing MainMenu injection...");
        CoSyncMenuInject_Init();

        // PrismaUI (optional dependency): acquire the API only. Views are
        // created lazily on first open, once PrismaUI's game-thread
        // dispatcher is ready -- it refuses callback views before that.
        CoSyncPrismaUI::Init();

        // 2026-07-27: the LoadGame crash guards moved to VEH-based point
        // patches (see CoSync_VEH's rva==0x2AA94C/0x70295F branches above) --
        // a MinHook detour + SEH __try/__except here did NOT reliably catch
        // them (this codebase's own round-18 finding: MinHook's trampoline
        // has no unwind info for the SEH search to walk through). No install
        // call needed; the VEH is already active process-wide.


        break;

    case F4SEMessagingInterface::kMessage_PreSaveGame:
        LOG_UPDATED("[SaveDiag] PreSaveGame fired -- engine save starting");
        CoSyncNpcWorldSync::DiagnoseDisabledOriginalsForSave();
        break;
    case F4SEMessagingInterface::kMessage_PostLoadGame:
        // data = bool: did the load succeed. Completes a client's switch into
        // the host's world (CoSyncWorldTransfer).
        CoSyncWorldTransfer::OnPostLoadGame(msg->data != nullptr);
        break;
    case F4SEMessagingInterface::kMessage_NewGame:
        CoSyncWorldTransfer::OnNewGame();
        break;
    case F4SEMessagingInterface::kMessage_PostSaveGame:
        LOG_UPDATED("[SaveDiag] PostSaveGame fired -- engine save finished (no crash if we got here)");
        CoSyncNpcWorldSync::RestoreActorsAfterSave();
        break;

    }
}


bool RegisterCoSyncPapyrus(VirtualMachine* vm);

extern "C"
__declspec(dllexport)
bool F4SEPlugin_Load(const F4SEInterface* f4se)
{
    LOG_REQUIREMENT("(global)::F4SEPlugin_Load", "Needs a valid interface handed to it by the mod loader. Called exactly once, right at the start, before any of the other systems it sets up are used.");
    LOG_INFO("CoSync - F4SEPlugin_Load  version=%s", COSYNC_VERSION_STR);

    InjectLoadingScreenTexture();

    InitializeCriticalSection(&g_vehLock);
    AddVectoredExceptionHandler(1, CoSync_VEH);
    LOG_INFO("[CoSync-VEH] Crash logger installed");

    // Round 129: LIVE-vs-DISK-vs-dump-IDB layout referee. Round-128 byte dumps
    // proved the F4Project IDB (built from a MEMORY DUMP via F4Dumper) and the
    // on-disk exe disagree in some .text regions (LookupFormByID prologue is
    // at 0x311860 on disk but 0x311850 in the IDB — a +0x10 skew — while
    // 0x2E4DF0 matches exactly), and the exe carries a Steam-DRM .bind
    // section, so disk bytes may not equal loaded bytes everywhere. This dump
    // of the RUNNING process's bytes at every disputed RVA is the ground
    // truth that decides which reference (disk exe vs dump-IDB, and per
    // region) all hook/call RVAs must come from.
    {
        static const uint32_t kDiagRVAs[] = {
            0x2E4DF0,           // CreateReferenceAtLocation — known-good anchor
            0x311850, 0x311860, // LookupFormByID: IDB start vs disk prologue
            0x563320, 0x563330, // ForceUnlock: IDB start vs +0x10 candidate
            0x563360, 0x563370, // ForceLock: round-128 IDB twin vs +0x10 candidate
            0x818D80, 0x81AE50, // anim SetInt thunk + worker (the eternal hang)
            0x1185A00, 0x1185B50, // SetObjectiveCompleted / Displayed
        };
        const uintptr_t exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL));
        for (uint32_t rva : kDiagRVAs)
        {
            const unsigned char* b = reinterpret_cast<const unsigned char*>(exeBase + rva);
            LOG_INFO("[LiveBytes] RVA 0x%07X: %02X %02X %02X %02X %02X %02X %02X %02X "
                     "%02X %02X %02X %02X %02X %02X %02X %02X",
                rva,
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        }
    }

    // Verify every hardcoded RVA still points at the bytes it was harvested
    // from before anything calls one. Off unless [Diagnostics] bScanAddresses=1;
    // see CoSyncAddressScan.h for why this exists.
    CoSyncAddrScan::Run(reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL)));

    CoSyncPlayerAnimation::Init(reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL)));

    LOG_INFO("CoSync: Installing first-person third-person-graph drive hooks...");
    if (!CoSyncFirstPersonGraph::Install(reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL))))
        LOG_ERROR("[FPGraph] Hook installation failed -- remote ghosts will freeze in first person");

    LOG_INFO("CoSync: Installing MainMenu InitMainList hook...");
    CoSyncMenuInject_InstallHooks();

    // (No DX11 Present hook any more: the ImGui overlay was retired in favour
    // of PrismaUI. The game-window hook is installed from the main-thread
    // tick -- see CoSyncWindowHook.)

    // Re-enabled 2026-06-17: 0xCA94F0 was indeed the wrong function -- it was
    // 0x190 bytes into the middle of an unrelated preceding function
    // (sub_140CA9360, a reference-counted name-lookup helper), which explains
    // firing during menu navigation rather than gameplay. Fixed RVA
    // (0xCA9520, see TickHook.cpp) decompiles to a function that calls the
    // confirmed-real per-graph Float/Bool/Int setters to push Speed and
    // sneak/state bools into an NPC's animation graph -- the real NpcAnimSync.
    // Hooks a fixed code address (not a live object's vtable), so it's safe
    // to install this early regardless of game-load state, same as the DX11
    // hook just above.
    // Round 124 bisection: disabled entirely. Six straight clean
    // eliminations so far (real engine call via settle-delay, SaveAnimVars,
    // AnimEventHook, Disconnect button, the whole ActorUpdate_Hook/TickHook
    // subsystem) -- none of them changed the crash. Never yet tested
    // removing this MinHook patch itself: the hook body's logic is cleared,
    // but the trampoline/relocation MinHook installs on this exact function
    // is still live every time. If a fixed-address hook's relocated bytes
    // are subtly wrong, that can corrupt state independent of anything the
    // hook body does.
    // InstallNpcAnimSyncHook();

    g_pluginHandle = f4se->GetPluginHandle();

    // Shared host save: player profiles live in the F4SE co-save, and CoSync's
    // proxy actors are kept out of every save (loading one crashed).
    CoSyncProfiles::RegisterSerialization(f4se, g_pluginHandle);
    CoSyncSaveLoad::InstallSaveFilter(&IsCoSyncProxyRef);
    // Clients never save: the host's save is the shared world.
    CoSyncSaveLoad::InstallSaveBlock(&CoSyncWorldTransfer::IsInHostWorld);

    g_messaging = (F4SEMessagingInterface*)f4se->QueryInterface(kInterface_Messaging);
    if (!g_messaging)
    {
        LOG_ERROR("Messaging interface missing!");
        return false;
    }

    LOG_INFO("Registering GameLoaded listener...");
    g_messaging->RegisterListener(g_pluginHandle, "F4SE", OnF4SEMessage);

    auto* papyrus = (F4SEPapyrusInterface*)f4se->QueryInterface(kInterface_Papyrus);
    if (!papyrus)
    {
        LOG_ERROR("Papyrus interface missing!");
        return false;
    }

    if (!papyrus->Register(RegisterCoSyncPapyrus))
    {
        LOG_ERROR("Papyrus->Register(RegisterCoSyncPapyrus) failed!");
        return false;
    }

    LOG_INFO("Papyrus registration queued successfully.");

    auto* taskIFace = (F4SETaskInterface*)f4se->QueryInterface(kInterface_Task);
    if (!taskIFace)
    {
        LOG_ERROR("Task interface missing!");
    }
    else
    {
        CoSyncSpawnTasks::Init(taskIFace);
        CoSyncRuntime_InstallMainThreadTick(taskIFace);
    }

    // Register Scaleform plugin — fires when any menu SWF loads, we filter to MainMenu
    auto* scaleform = (F4SEScaleformInterface*)f4se->QueryInterface(kInterface_Scaleform);
    CoSyncMainMenu_Register(scaleform);

    LOG_INFO("Plugin load complete");
    return true;
}
