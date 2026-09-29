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
#include "CoSyncReviveSync.h"
#include "CoSyncProxyWeaponAuthority.h"
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

        COSYNC_VERSION,   // Core/Version.h (e.g. 0.2.4 -> 204)
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

    // P9 (2026-09-26): seven RVA-keyed point patches used to live here
    // (0x1A7FC54, 0x1A7FC9D, 0x1B18C75, 0x1B193CA, 0x1B193E9, 0x2AA94C,
    // 0x70295F), derived against 1.11.221. On 1.11.240 every one of those
    // addresses is mid-instruction or alignment padding (IDA sweep; 0x1A7FC54
    // and 0x1A7FC9D sit inside the UI constructor), and a CPU fault is only
    // ever reported at an instruction start -- so they could never fire, while
    // 0x1B193CA's "safe exit" redirect target was itself mid-instruction.
    // Removed along with the two helpers only they used. See git history.

    // P9 part 2 (2026-09-26, user decision): the generic "any EXEC access
    // violation -> fake a `ret` with RAX=0 and keep running" recovery lived
    // here. It hid bugs instead of fixing them and could corrupt control flow:
    // a faked return into F4SE's task dispatcher silently dropped the next
    // queued task (documented in CoSyncSpawnTasks.cpp). Such faults now fall
    // through to the full crash report below (registers, real-unwind stack,
    // live bytes, minidump) and the process crashes, so the root cause can be
    // found. See git history for the removed code.

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
    // (The round-82 "already diagnosed, skip the minidump" RVAs 0xCC934D and
    // 0x256B80 were removed 2026-09-26: on 1.11.240 they are mid-function /
    // padding, so they matched nothing.)
    // 0.3.5: one dump per process, named after the pid like the log
    // (CoSyncUpdated_<pid>.log <-> CoSync_crash_<pid>.dmp). The single fixed
    // name let the next crash overwrite the last one: the two 2026-09-28
    // crashes survived only as log text. IndirectlyReferencedMemory adds the
    // memory around every pointer found on the stacks and in registers, so the
    // objects a crash touched (the actor, its inventory list) are readable in
    // the dump -- the 2026-09-29 dump had stacks only. Only the newest five
    // crash dumps are kept.
    static bool s_minidumpWritten = false;
    if (!s_minidumpWritten)
    {
        char dir[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameA(nullptr, dir, MAX_PATH);
        char* cut = (n > 0 && n < MAX_PATH) ? strrchr(dir, '\\') : nullptr;
        if (cut)
            cut[1] = '\0';
        else
            strcpy_s(dir, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Fallout 4\\");
        strcat_s(dir, "Data\\F4SE\\Plugins\\");

        char pattern[MAX_PATH];
        sprintf_s(pattern, "%sCoSync_crash_*.dmp", dir);
        struct Old { FILETIME t; char name[MAX_PATH]; };
        Old olds[64]; int nOld = 0;
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(pattern, &fd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (nOld < 64) { olds[nOld].t = fd.ftLastWriteTime; strcpy_s(olds[nOld].name, fd.cFileName); ++nOld; }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
        while (nOld >= 5)   // make room: drop the oldest until four remain
        {
            int oldest = 0;
            for (int i = 1; i < nOld; ++i)
                if (CompareFileTime(&olds[i].t, &olds[oldest].t) < 0) oldest = i;
            char victim[MAX_PATH];
            sprintf_s(victim, "%s%s", dir, olds[oldest].name);
            DeleteFileA(victim);
            olds[oldest] = olds[--nOld];
        }

        char dumpPath[MAX_PATH];
        sprintf_s(dumpPath, "%sCoSync_crash_%lu.dmp", dir, GetCurrentProcessId());
        HANDLE hFile = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION mei{ GetCurrentThreadId(), ep, FALSE };
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                &mei, nullptr, nullptr);
            CloseHandle(hFile);
            LOG_ERROR("[CoSync-VEH] Minidump -> %s", dumpPath);
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

        // (2026-07-27 LoadGame crash guards were VEH point patches keyed to
        // 1.11.221 addresses; removed 2026-09-26 as unreachable on 1.11.240 --
        // see the P9 note in CoSync_VEH.)


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

    // "Revive" instead of "Talk" on a downed player (fixed-address hook, safe
    // this early like the menu hook above).
    CoSyncReviveSync::InstallActivateTextHook();

    // A proxy's own AI must not holster/draw against the owner's replicated
    // weapon state (fixed-address hook, same load-time safety as above).
    CoSyncProxyWeaponAuthority::Install();

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
