#include "CoSyncRuntime.h"

#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncWorld.h"
#include "CoSyncTransport.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncTeams.h"
#include "CoSyncSettlementSync.h"
#include "CoSyncPickpocket.h"
#include "CoSyncSessionRules.h"
#include "CoSyncElevatorSync.h"
#include "CoSyncWorkshopAuthority.h"
#include "CoSyncNet.h"
#include "CoSyncLocalPlayer.h"
#include "TickHook.h"
#include "PluginAPI.h"
#include "GameThreads.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

static double NowSeconds()
{
    LOG_REQUIREMENT("NowSeconds", "Doesn't touch any game state — just needs the system's performance-counter frequency to be non-zero (assumed, not checked).");
    static double s_freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
        }();

    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / s_freq;
}

// Round 111: g_lastTickStep (CoSyncNet.cpp) only covers steps INSIDE
// CoSyncNet::Tick() -- it stays at its initial value for the entire window
// before that function ever gets a turn, including this whole function and
// CoSyncNet::Init() (called from PerformPendingInit() below). A real crash
// happened in exactly that blind spot, on the first test where IsWorldReady()
// actually went true (real save loaded) and Init() ran for the first time
// this campaign. Reusing the same diagnostic global here so the VEH log's
// lastTickStep= field is actually informative for this phase too.
extern volatile const char* g_lastTickStep;

// Round 117: g_npcAnimSync_lastA1/lastActorFormID/callIndex (TickHook.cpp) and
// g_animUtils_lastOp/lastVarName/lastActor (CoSyncAnimationUtils.cpp) are
// captured via plain volatile assignment, NEVER via LOG_*, because they're
// written from inside NpcAnimSync_Hook -- a true MinHook function-splice
// trampoline on a live engine callback, the exact context round 112 already
// proved crashes the CRT's printf machinery when logged from directly. That
// makes them invisible in the live Debug Console (ConsoleLogger.h) until
// CoSync_VEH's crash report happens to print them after the fact -- too late
// to watch as it happens. This function runs on a normal main-thread task
// (AddTaskPermanent, not a hook splice), so it's a safe place to poll those
// globals every frame and print a LOG_DEBUG line the moment they change --
// visible on screen in real time, without ever calling LOG_* from the unsafe
// trampoline itself.
extern volatile void* g_npcAnimSync_lastA1;
extern volatile uint32_t g_npcAnimSync_lastActorFormID;
extern volatile uint64_t g_npcAnimSync_callIndex;
extern volatile const char* g_animUtils_lastOp;
extern volatile const char* g_animUtils_lastVarName;
extern volatile void* g_animUtils_lastActor;

#include "GameMenus.h"
#include "CoSyncOverlay.h"
#include "CoSyncTeleport.h"
#include "CoSyncWindowHook.h"
#include "CoSyncPrismaUI.h"
#include "CoSyncWorldTransfer.h"
#include "CoSyncContentSync.h"
#include "CoSyncProfiles.h"
#include "CoSyncProxyLooks.h"
#include "CoSyncCompanions.h"
#include "CoSyncItemMods.h"
#include "CoSyncThreadDiag.h"
#include "CoSyncSteamLobby.h"

#include <DbgHelp.h>
#include <atomic>
#include <string>

// ---------------------------------------------------------------------------
// Hang watchdog (2026-09-26). In the 0.2.4 test the client's tick stopped for
// good at 23:39:14 while its network thread kept receiving; the game was
// closed without a crash, so nothing showed what the engine was doing. A
// plain OS thread (never joined, dies with the process) watches the tick's
// timestamp: a stall of kHangDumpSeconds writes one CoSync_hang.dmp with
// every thread's stack, and any stall over kStallLogSeconds is logged when
// the tick comes back (loading screens run 15-43 s in the 26 Sep logs, so
// they show up as logged stalls, not dumps). Diagnostic only.
// ---------------------------------------------------------------------------
namespace
{
    constexpr uint64_t kHangDumpSeconds = 90;
    constexpr uint64_t kStallLogSeconds = 20;

    std::atomic<uint64_t> g_lastTickMs{ 0 };
    std::atomic<bool>     g_watchdogStarted{ false };

    // 0.3.6: render-pass cycle probe. The 0.3.5 client hang (CoSync_hang.dmp,
    // 21:10:35) had the main thread (the window thread, 12712) spinning in
    // sub_14221F620 at 0x14221F780..0x14221F798:
    //     for (p = head; p; p = *(p + 0x40)) if (!*p) sub_14221FCC0(p);
    // called from the frame render sub_140C33250 -> sub_140C38240 -- a render
    // pass list whose +0x40 chain had become a cycle. The dump has no heap,
    // so the objects in the cycle could not be named. On a stall this
    // suspends the main thread, and if RIP is in that function walks the
    // chain from RBX (the loop cursor) with SEH-guarded reads. Pass +0x18 is
    // the BSGeometry (sub_14221FCC0 reads its +0x108 flags / +0x140 fields);
    // NiAVObject name is the BSFixedString at +0x10 (entry string at +0x18),
    // parent at +0x28. Nothing is logged while the thread is suspended (the
    // logger's lock could be held by it).
    constexpr uintptr_t kRenderPassWalkStart = 0x221F620;
    constexpr uintptr_t kRenderPassWalkEnd   = 0x221F620 + 0x2BF;

    __declspec(noinline) bool ProbeRead(uintptr_t a, uintptr_t& out)
    {
        __try { out = *reinterpret_cast<const uintptr_t*>(a); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    __declspec(noinline) void ProbeName(uintptr_t obj, char* out, size_t cap)
    {
        out[0] = 0;
        uintptr_t entry = 0;
        if (!obj || !ProbeRead(obj + 0x10, entry) || !entry)
            return;
        __try
        {
            const char* s = reinterpret_cast<const char*>(entry + 0x18);
            size_t i = 0;
            for (; i + 1 < cap && s[i]; ++i)
                out[i] = (s[i] >= 32 && s[i] < 127) ? s[i] : '?';
            out[i] = 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
    }

    DWORD FindMainWindowThread()
    {
        struct Ctx { DWORD pid; DWORD tid; } ctx{ ::GetCurrentProcessId(), 0 };
        ::EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            const DWORD tid = ::GetWindowThreadProcessId(h, &pid);
            char cls[32] = {};
            if (pid == c->pid && ::GetClassNameA(h, cls, sizeof(cls)) && strcmp(cls, "Fallout4") == 0)
            {
                c->tid = tid;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.tid;
    }

    void ProbeRenderPassCycle()
    {
        const DWORD tid = FindMainWindowThread();
        if (!tid)
        {
            LOG_ERROR("[HangWatchdog] render probe: main window thread not found");
            return;
        }
        HANDLE th = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
        if (!th)
            return;
        const uintptr_t exe = reinterpret_cast<uintptr_t>(::GetModuleHandleA(nullptr));

        struct Node { uintptr_t pass, geo; char geoName[64]; char chain[256]; };
        Node nodes[8] = {};
        int nodeCount = 0, walked = 0, cycleAt = -1;
        uintptr_t rip = 0;
        bool inLoop = false;

        if (::SuspendThread(th) != (DWORD)-1)
        {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_FULL;
            if (::GetThreadContext(th, &c))
            {
                rip = c.Rip - exe;
                inLoop = rip >= kRenderPassWalkStart && rip < kRenderPassWalkEnd;
                if (inLoop)
                {
                    uintptr_t seen[512];
                    uintptr_t p = c.Rbx;
                    while (p && walked < 512 && cycleAt < 0)
                    {
                        for (int k = 0; k < walked; ++k)
                            if (seen[k] == p) { cycleAt = k; break; }
                        if (cycleAt >= 0) break;
                        seen[walked++] = p;
                        uintptr_t next = 0;
                        if (!ProbeRead(p + 0x40, next)) break;
                        p = next;
                    }
                    const int from = cycleAt >= 0 ? cycleAt : 0;
                    for (int k = from; k < walked && nodeCount < 8; ++k)
                    {
                        Node& n = nodes[nodeCount++];
                        n.pass = seen[k];
                        ProbeRead(n.pass + 0x18, n.geo);
                        ProbeName(n.geo, n.geoName, sizeof(n.geoName));
                        uintptr_t cur = n.geo;
                        size_t len = 0;
                        for (int depth = 0; depth < 8; ++depth)
                        {
                            uintptr_t parent = 0;
                            if (!ProbeRead(cur + 0x28, parent) || !parent) break;
                            char nm[48];
                            ProbeName(parent, nm, sizeof(nm));
                            const int w = _snprintf_s(n.chain + len, sizeof(n.chain) - len, _TRUNCATE, "%s<-", nm[0] ? nm : "?");
                            if (w < 0) break;
                            len += static_cast<size_t>(w);
                            cur = parent;
                        }
                    }
                }
            }
            ::ResumeThread(th);
        }
        ::CloseHandle(th);

        LOG_ERROR("[HangWatchdog] render probe: main tid=%lu rip=Fallout4+0x%llX (%s the render-pass walk sub_14221F620)",
            tid, (unsigned long long)rip, inLoop ? "INSIDE" : "not in");
        if (!inLoop)
            return;
        LOG_ERROR("[HangWatchdog] render probe: walked %d pass node(s), %s", walked,
            cycleAt >= 0 ? "CYCLE found (list loops back)" : "no cycle within 512 nodes");
        for (int k = 0; k < nodeCount; ++k)
            LOG_ERROR("[HangWatchdog]   pass=%p geo=%p '%s' parents: %s",
                (void*)nodes[k].pass, (void*)nodes[k].geo, nodes[k].geoName, nodes[k].chain);
    }

    void WriteHangDump(uint64_t stalledSec)
    {
        char exePath[MAX_PATH] = {};
        if (::GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0)
            return;
        std::string path(exePath);
        const size_t slash = path.find_last_of("\\/");
        path = (slash == std::string::npos) ? std::string() : path.substr(0, slash + 1);
        path += "Data\\F4SE\\Plugins\\CoSync_hang.dmp";

        HANDLE hFile = ::CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
            return;
        const BOOL ok = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), hFile,
            static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
            nullptr, nullptr, nullptr);
        ::CloseHandle(hFile);
        LOG_ERROR("[HangWatchdog] tick stalled %llus -- %s CoSync_hang.dmp",
            (unsigned long long)stalledSec, ok ? "wrote" : "FAILED to write");
    }

    DWORD WINAPI HangWatchdogProc(void*)
    {
        bool dumped = false;
        for (;;)
        {
            ::Sleep(1000);
            const uint64_t last = g_lastTickMs.load(std::memory_order_relaxed);
            if (last == 0)
                continue;
            const uint64_t stalled = (::GetTickCount64() - last) / 1000;
            if (!dumped && stalled >= kHangDumpSeconds)
            {
                const char* step = g_lastTickStep ? const_cast<const char*>(g_lastTickStep) : "<none>";
                LOG_ERROR("[HangWatchdog] no CoSync tick for %llus (lastTickStep=%s, loadingMenu=%d)",
                    (unsigned long long)stalled, step, CoSyncOverlay_IsLoadingScreenActive() ? 1 : 0);
                ProbeRenderPassCycle();
                WriteHangDump(stalled);
                dumped = true;   // once per process
            }
        }
    }

    // Tick side: stamp, log a long stall once it ends, start the thread once.
    void HangWatchdogTick()
    {
        const uint64_t nowMs = ::GetTickCount64();
        const uint64_t prev = g_lastTickMs.exchange(nowMs, std::memory_order_relaxed);
        if (prev != 0 && nowMs - prev >= kStallLogSeconds * 1000)
            LOG_WARN("[HangWatchdog] tick resumed after a %.1fs stall (lastTickStep=%s)",
                (nowMs - prev) / 1000.0, g_lastTickStep ? const_cast<const char*>(g_lastTickStep) : "<none>");
        if (!g_watchdogStarted.exchange(true))
        {
            HANDLE h = ::CreateThread(nullptr, 0, HangWatchdogProc, nullptr, 0, nullptr);
            if (h)
                ::CloseHandle(h);
            else
                g_watchdogStarted.store(false);
        }
    }
}

// LoadingMenu open-state poll — drives CoSyncOverlay_IsLoadingScreenActive()
// (the player marker holds position while a loading screen is up).
//
// CORRECTED 2026-07-21 (same session, before this ever reached a test): the
// original RVA here (0x1A7FE60) was MY OWN fresh IDA check tonight, which
// read is_func_head=True and a plausible-looking lock+lookup+flag-check
// decompile — but Dialog/CoSyncDialogSync.cpp already has a round-104
// finding (from an EARLIER session) that specifically investigated this
// exact address and disproved it: entry_match=False, actually mid-body of a
// different bounded function (0x1A7FDD0-0x1A7FE8C). That file's own
// UI::IsMenuOpen call has been live/deployed/exercised via dialogue
// detection already; my address had shipped in build 156 but was still
// completely untested. Trusting the battle-tested address over my own
// fresh, unverified-in-practice one — same s_IsMenuOpen RVA and signature
// as CoSyncDialogSync.cpp's SafeIsDialogueMenuOpen.
typedef bool (*_UI_IsMenuOpen_Fn)(UI* ui, const BSFixedString& name);
static RelocAddr<_UI_IsMenuOpen_Fn> g_UI_IsMenuOpen_Fn(0x1A80320);

// Matches CoSyncDialogSync.cpp's SafeIsDialogueMenuOpen exactly: the __try
// lives in its own noinline function with no non-trivial objects of its own
// (the BSFixedString is constructed by the caller and passed by reference) —
// a static BSFixedString's first-construction guard needs unwind info too,
// so it can't share a function with __try either (C2712).
__declspec(noinline) static bool SafeIsMenuOpen(UI* ui, const BSFixedString& name)
{
    __try
    {
        return g_UI_IsMenuOpen_Fn(ui, name);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// The real UI singleton on 1.11.240 (IDA 2026-09-26): LooksMenu registers
// itself on qword_1430E8930 (0x140A863F0), and Game.ForceThirdPerson calls
// IsMenuOpen(qword_1430E8930, "PipboyMenu"). g_ui's 0x030E88B0 is
// BSInputEnableManager (0x140BB0430 passes it to the input-enable functions),
// so IsMenuOpen on it never saw any menu -- the first editor test returned the
// player while the LooksMenu was still open.
// g_ui itself is now corrected (GameGlobals.cpp).
bool CoSyncRuntime_IsMenuOpen(const char* menuName)
{
    UI* ui = *g_ui;
    if (!ui || !menuName || !*menuName)
        return false;
    const BSFixedString name(menuName);
    return SafeIsMenuOpen(ui, name);
}

__declspec(noinline) static void PollLoadingMenuState_Impl()
{
    // Diagnostic-first: build 163's test session showed the logo texture
    // loading fine but zero evidence either way of the menu-state poll
    // itself (no log line existed for it at all) — could mean the RVA/menu
    // name is wrong, or could just mean that test never triggered a loading
    // screen after initial boot (Present() can't hook before DX11 init, so
    // the very first boot screen is structurally unobservable regardless).
    // This heartbeat settles it on the next test either way.
    static uint32_t s_pollCount = 0;
    ++s_pollCount;
    if (s_pollCount <= 3 || s_pollCount % 200 == 1)
        LOG_INFO("[CoSyncRuntime] loading-menu poll heartbeat #%u ui=%p", s_pollCount, (void*)*g_ui);

    UI* ui = *g_ui;
    if (!ui)
        return;
    static const BSFixedString kLoadingMenu("LoadingMenu");
    const bool isOpen = SafeIsMenuOpen(ui, kLoadingMenu);

    static bool s_lastOpen = false;
    if (isOpen != s_lastOpen)
    {
        s_lastOpen = isOpen;
        LOG_UPDATED("[CoSyncRuntime] loading menu state changed -> %s", isOpen ? "open" : "closed");
    }

    // Report the GAME's menu state only. This used to call
    // ShowLoadingScreen(isOpen), which clobbered CoSync's own join-flow
    // request a few frames after it was made -- see the note in
    // CoSyncOverlay.cpp.
    CoSyncOverlay_SetGameLoadingMenuOpen(isOpen);
}

__declspec(noinline) static void PollLoadingMenuState()
{
    static int s_ctr = 0;
    if (++s_ctr % 15 != 0)
        return;
    __try
    {
        PollLoadingMenuState_Impl();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[CoSyncRuntime] checking the loading menu crashed but was caught");
    }
}

// Diagnostic for the Pipboy/PauseMenu desync complaint: this project's own
// network/anim tick (CoSyncNet::Tick -> PlayerManager::Tick) runs off
// AddTaskPermanent, which is driven by F4SE's message-pump task queue, not
// the engine's own paused-simulation walk that vtable21/ActorUpdate_Hook
// depends on -- so it should already keep receiving/applying peer position
// and anim data while this client's own Pipboy or pause menu is open,
// without any gating change needed here. This just proves that empirically
// instead of asserting it from static analysis: logs whenever Pipboy/
// PauseMenu open-state changes, and confirms with a heartbeat while either
// is open that CoSyncNet::Tick is still being reached every frame. If a test
// session shows the heartbeat going silent while one of these is open, the
// assumption above is wrong somewhere and needs fixing; if it keeps firing,
// the sync side of this complaint is already covered by the existing
// architecture and nothing else needs to change here.
__declspec(noinline) static void PollPipboyPauseMenuState_Impl()
{
    UI* ui = *g_ui;
    if (!ui)
        return;

    static const BSFixedString kPipboyMenu("PipboyMenu");
    static const BSFixedString kPauseMenu("PauseMenu");
    const bool pipboyOpen = SafeIsMenuOpen(ui, kPipboyMenu);
    const bool pauseOpen  = SafeIsMenuOpen(ui, kPauseMenu);

    static bool s_lastPipboyOpen = false;
    static bool s_lastPauseOpen  = false;
    if (pipboyOpen != s_lastPipboyOpen)
    {
        s_lastPipboyOpen = pipboyOpen;
        LOG_UPDATED("[CoSyncRuntime] pip-boy menu state changed -> %s", pipboyOpen ? "open" : "closed");
    }
    if (pauseOpen != s_lastPauseOpen)
    {
        s_lastPauseOpen = pauseOpen;
        LOG_UPDATED("[CoSyncRuntime] pause menu state changed -> %s", pauseOpen ? "open" : "closed");
    }

    if (pipboyOpen || pauseOpen)
    {
        static uint32_t s_openHeartbeat = 0;
        if ((++s_openHeartbeat % 60) == 1)
            LOG_UPDATED("[CoSyncRuntime] pip-boy/pause menu open, tick still running (heartbeat #%u) pipboy=%d pause=%d",
                s_openHeartbeat, (int)pipboyOpen, (int)pauseOpen);
    }
}

__declspec(noinline) static void PollPipboyPauseMenuState()
{
    static int s_ctr = 0;
    if (++s_ctr % 15 != 0)
        return;
    __try
    {
        PollPipboyPauseMenuState_Impl();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[CoSyncRuntime] checking the pip-boy/pause menu crashed but was caught");
    }
}

static void PollAndPrintAnimDiagnostics()
{
    LOG_REQUIREMENT("PollAndPrintAnimDiagnostics", "Reads shared diagnostic values written elsewhere in the animation sync code. Safe to call any time — the values just default to null/0 until those hooks actually fire.");
    static uint64_t s_lastSeenCallIndex = 0;
    const uint64_t callIndex = g_npcAnimSync_callIndex;
    if (callIndex != s_lastSeenCallIndex)
    {
        s_lastSeenCallIndex = callIndex;
        LOG_DEBUG("[NpcAnimSync live] call#=%llu a1=%p actor=0x%08X",
            (unsigned long long)callIndex,
            (void*)g_npcAnimSync_lastA1,
            g_npcAnimSync_lastActorFormID);
    }

    static volatile const char* s_lastSeenOp = nullptr;
    static volatile const char* s_lastSeenVarName = nullptr;
    volatile const char* op = g_animUtils_lastOp;
    volatile const char* varName = g_animUtils_lastVarName;
    if (op != s_lastSeenOp || varName != s_lastSeenVarName)
    {
        s_lastSeenOp = op;
        s_lastSeenVarName = varName;
        LOG_EVERY_MS(5000, LOG_DEBUG("[AnimUtils live] op=%s var=%s actor=%p",
            op ? op : "<null>",
            varName ? varName : "<null>",
            (void*)g_animUtils_lastActor));
    }
}

void CoSyncRuntime_TickGameThread()
{
    LOG_REQUIREMENT("CoSyncRuntime_TickGameThread", "Must run on the main game thread, not the render thread, to avoid a cross-thread race that's been hit before. Relies on the networking/player subsystems gating themselves until they're ready.");
    const double now = NowSeconds();
    CoSyncThreadDiag::TickScope tickScope;   // overlap census, see CoSyncThreadDiag.h
    HangWatchdogTick();

    // Unconditional heartbeat -- proves the main-thread task itself is still
    // getting turns at all. If it goes silent, g_lastTickStep's last value
    // pins the exact blocking call.
    {
        static uint32_t s_tickCount = 0;
        static double s_lastHeartbeatLog = 0.0;
        ++s_tickCount;
        if (s_tickCount <= 3 || (now - s_lastHeartbeatLog) >= 2.0)
        {
            s_lastHeartbeatLog = now;
            LOG_INFO("[CoSyncRuntime] main tick heartbeat #%u lastStep=%s", s_tickCount, g_lastTickStep ? g_lastTickStep : "<none>");
        }
    }

    // Hook calling-thread census (diagnostic only, see CoSyncThreadDiag.h).
    CoSyncThreadDiag::Report(now);

    // Steam lobby (host lobby / friend browse / join) async polling.
    CoSyncSteamLobby::Tick(now);

    PollAndPrintAnimDiagnostics();
    PollLoadingMenuState();
    PollPipboyPauseMenuState();

    // Formerly driven from the DX11 Present hook (render thread). That hook is
    // gone with ImGui; these belong on the main thread anyway.
    g_lastTickStep = "RTGT:WindowHook";
    CoSyncWindowHook_EnsureInstalled();
    g_lastTickStep = "RTGT:TickHookRetry";
    TickHook_RetryInstallIfNeeded();
    g_lastTickStep = "RTGT:TeleportHotkeys";
    CoSyncTeleport::PollHotkeys();
    g_lastTickStep = "RTGT:PrismaHud";
    CoSyncPrismaUI::TickHud();
    g_lastTickStep = "RTGT:Nameplates";
    CoSyncPrismaUI::TickNameplates();

    // Shared host save: stream the world to joining clients (host) and
    // apply / upload this player's character (client).
    g_lastTickStep = "RTGT:ContentSync";
    CoSyncContentSync::Tick();
    g_lastTickStep = "RTGT:WorldTransfer";
    CoSyncWorldTransfer::Tick();
    g_lastTickStep = "RTGT:Profiles";
    CoSyncProfiles::Tick(now);
    g_lastTickStep = "RTGT:ProxyLooks";
    CoSyncProxyLooks::Tick();
    g_lastTickStep = "RTGT:Companions";
    CoSyncCompanions::Tick(now);
    g_lastTickStep = "RTGT:ItemMods";
    CoSyncItemMods::Tick(now);

    g_lastTickStep = "RTGT:PerformPendingInit";
    if (!CoSyncNet::IsInitialized())
        CoSyncNet::PerformPendingInit();


    g_lastTickStep = "RTGT:CoSyncTransport::Tick";
    if (CoSyncTransport::IsInitialized())
        CoSyncTransport::Tick(now);

    g_lastTickStep = "RTGT:CaptureLocalAnimVars";
    TickHook_CaptureLocalAnimVars();


    // Removed direct g_CoSyncPlayerManager.Tick() call here -- CoSyncNet::Tick()
    // below already calls it internally (CoSyncNet.cpp, gated on s_initialized),
    // so this ran every remote entity's PlayerManager::Tick() -- including
    // CoSyncPlayer::TickAnimVars() -- TWICE per frame. Confirmed via call-graph
    // tracing: this is the only live caller of CoSyncRuntime_TickGameThread()
    // (wired from the DX11 Present hook), and CoSyncNet::Tick() is the only
    // other caller of PlayerManager::Tick(). Doubling TickAnimVars() doubles its
    // exponential-smoothing rate every frame (m_smoothSpeed +=
    // (target - m_smoothSpeed) * kAlpha, run twice against the same unchanged
    // target instead of once) and re-applies LoadAnimationVariables redundantly
    // -- a real, frame-rate-dependent distortion of the animation smoothing
    // math, not just wasted CPU.

    g_lastTickStep = "RTGT:ScheduleBoneTasks";
    TickHook_ScheduleBoneTasks();


    // Teams run in and out of a session: a solo player is their own
    // authority and can manage their settlements offline.
    g_lastTickStep = "RTGT:CoSyncTeams::Tick";
    CoSyncTeams::Tick(now);

    // Team / personal settlement state (ratings + workbench), host-authoritative.
    g_lastTickStep = "RTGT:CoSyncSettlementSync::Tick";
    CoSyncSettlementSync::Tick(now);

    // Player-to-player pickpocket exchange + "no Talk" on player proxies.
    g_lastTickStep = "RTGT:CoSyncPickpocket::Tick";
    CoSyncPickpocket::Tick(now);

    // 0.5.1: the host's PVE / PVP rule, re-sent every 5 s.
    g_lastTickStep = "RTGT:CoSyncSessionRules::Tick";
    CoSyncSessionRules::Tick(now);
    g_lastTickStep = "RTGT:CoSyncElevatorSync::Tick";
    CoSyncElevatorSync::Tick(now);

    // Workshop authority: installs the verified workshop hooks once a world
    // is loaded, then gates / replicates every build, move, scrap, repair.
    g_lastTickStep = "RTGT:CoSyncWorkshopAuthority::Tick";
    CoSyncWorkshopAuthority::Tick(now);

    g_lastTickStep = "RTGT:CoSyncNet::Tick";
    if (CoSyncNet::IsInitialized())
        CoSyncNet::Tick(now);


    g_lastTickStep = "RTGT:LocalPlayer::Tick";
    CoSyncLocalPlayer::Tick(now);

    g_lastTickStep = "RTGT:end";
}

namespace
{
    class CoSyncMainThreadTickTask final : public ITaskDelegate
    {
    public:
        void Run() override
        {
            LOG_REQUIREMENT("CoSyncMainThreadTickTask::Run", "Invoked by the game's main-thread task queue. No other precondition beyond being called from that queue.");
            CoSyncRuntime_TickGameThread();
        }
    };
}

void CoSyncRuntime_InstallMainThreadTick(const F4SETaskInterface* taskInterface)
{
    LOG_REQUIREMENT("CoSyncRuntime_InstallMainThreadTick", "Needs a non-null task interface (checked), and must be called exactly once at plugin load time since the task instance registers itself permanently.");
    if (!taskInterface)
    {
        LOG_ERROR("[CoSyncRuntime] InstallMainThreadTick: no task interface was given!");
        return;
    }

    // AddTaskPermanent's target is never removed/deleted (matches its name --
    // see f4se-0.7.8/f4se/Hooks_Threads.cpp's s_tasksPermanent), so a single
    // static instance registered once here runs Run() forever, once per
    // main-thread message-queue pump.
    static CoSyncMainThreadTickTask s_tickTask;
    taskInterface->AddTaskPermanent(&s_tickTask);

    LOG_INFO("[CoSyncRuntime] main-thread tick installed");
}
