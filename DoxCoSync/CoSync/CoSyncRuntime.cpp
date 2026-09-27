#include "CoSyncRuntime.h"

#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncWorld.h"
#include "CoSyncTransport.h"
#include "CoSyncPlayerManager.h"
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
#include "CoSyncProfiles.h"
#include "CoSyncProxyLooks.h"
#include "CoSyncCompanions.h"
#include "CoSyncItemMods.h"
#include "CoSyncThreadDiag.h"

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
        LOG_DEBUG("[AnimUtils live] op=%s var=%s actor=%p",
            op ? op : "<null>",
            varName ? varName : "<null>",
            (void*)g_animUtils_lastActor);
    }
}

void CoSyncRuntime_TickGameThread()
{
    LOG_REQUIREMENT("CoSyncRuntime_TickGameThread", "Must run on the main game thread, not the render thread, to avoid a cross-thread race that's been hit before. Relies on the networking/player subsystems gating themselves until they're ready.");
    const double now = NowSeconds();
    CoSyncThreadDiag::TickScope tickScope;   // overlap census, see CoSyncThreadDiag.h

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

    // Shared host save: stream the world to joining clients (host) and
    // apply / upload this player's character (client).
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
