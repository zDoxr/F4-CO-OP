#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncSceneSync.h"

#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "GameReferences.h"
#include "GameForms.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"

#include <mutex>
#include <vector>
#include <string>
#include <cstdlib>

extern PlayerCharacter* CoSyncGetPlayer();

// BGSScene is forward-declared in GameReferences.h.
// For formID access we cast to TESForm* (BGSScene inherits TESForm;
// formID is at offset 0x14 on all TESForm subclasses).

namespace
{
    // -------------------------------------------------------------------------
    // Host-side state (game thread only; no mutex needed).
    // -------------------------------------------------------------------------
    static uint32_t s_lastBroadcastSceneFormID = 0;  // last formID we TX'd
    static double   s_lastScenePoll             = -1.0;
    static constexpr double kScenePollInterval  = 0.25;

    // -------------------------------------------------------------------------
    // Pending ops — queued on network thread, drained on game thread.
    // -------------------------------------------------------------------------
    struct PendingSceneOp
    {
        uint32_t sceneFormID;
        bool     start;   // true = Start(), false = Stop()
    };

    static std::mutex                   s_sceneMutex;
    static std::vector<PendingSceneOp>  s_pendingOps;
}

// ============================================================================
// Host-side scene detection
// ============================================================================

// Reading the player's current scene through the virtual dispatch
// (TESObjectREFR::GetCurrentScene, vtable slot 0x5C) is safe on the game
// thread but SEH-wrapped in case the engine isn't fully settled.
__declspec(noinline) static BGSScene* SafeGetCurrentScene(TESObjectREFR* refr)
{
    __try
    {
        return refr->GetCurrentScene();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

void CoSyncSceneSync::TickLocalScene(double now)
{
    if ((now - s_lastScenePoll) < kScenePollInterval)
        return;
    s_lastScenePoll = now;

    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc) return;

    TESObjectREFR* pcRefr = reinterpret_cast<TESObjectREFR*>(pc);
    BGSScene* scene = SafeGetCurrentScene(pcRefr);

    // BGSScene is a TESForm; formID sits at offset 0x14 on every TESForm.
    uint32_t currentFormID = 0;
    if (scene)
        currentFormID = reinterpret_cast<TESForm*>(scene)->formID;

    if (currentFormID == s_lastBroadcastSceneFormID)
        return;  // no change

    const uint32_t prev = s_lastBroadcastSceneFormID;
    s_lastBroadcastSceneFormID = currentFormID;

    if (currentFormID != 0)
    {
        // Entered a new scene.
        std::string pkt = "SCN|";
        pkt += std::to_string(currentFormID);
        pkt += "|1";
        CoSyncTransport::Send(pkt);
        LOG_UPDATED("[SceneSync] TX scene start: formID=0x%08X", currentFormID);
    }

    if (prev != 0)
    {
        // Left the previous scene (either to a new one or to none).
        std::string pkt = "SCN|";
        pkt += std::to_string(prev);
        pkt += "|0";
        CoSyncTransport::Send(pkt);
        LOG_UPDATED("[SceneSync] TX scene stop: formID=0x%08X", prev);
    }
}

// ============================================================================
// Packet routing (network thread)
// ============================================================================

bool CoSyncSceneSync::IsSceneSync(const std::string& msg)
{
    return msg.rfind("SCN|", 0) == 0;
}

void CoSyncSceneSync::OnReceiveSceneSync(const std::string& msg)
{
    // Host is the sender — no relay needed.
    const char* s = msg.c_str() + 4;  // skip "SCN|"
    char* e = nullptr;

    const uint32_t sceneFormID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (!e || *e != '|' || sceneFormID == 0)
    {
        LOG_WARN("[SceneSync] Malformed SCN packet (formID)");
        return;
    }
    s = e + 1;

    const uint32_t flag = static_cast<uint32_t>(std::strtoul(s, &e, 10));

    LOG_UPDATED("[SceneSync] RX queued: formID=0x%08X %s",
        sceneFormID, flag ? "start" : "stop");
    std::lock_guard<std::mutex> lk(s_sceneMutex);
    s_pendingOps.push_back({ sceneFormID, flag != 0 });
}

// ============================================================================
// Apply on game thread
// ============================================================================

// Non-trivial dtors (VMArray, VMVariable) live here; no __try (C2712-safe).
__declspec(noinline) static void ApplySceneOp_Impl(uint32_t sceneFormID, bool start)
{
    TESForm* sceneForm = LookupFormByID(sceneFormID);
    if (!sceneForm)
    {
        LOG_WARN("[SceneSync] scene formID=0x%08X not found", sceneFormID);
        return;
    }
    if (sceneForm->formType != kFormType_SCEN)
    {
        LOG_WARN("[SceneSync] formID=0x%08X is not a SCEN (type=%u)", sceneFormID, (unsigned)sceneForm->formType);
        return;
    }

    VMArray<VMVariable> emptyArgs;
    CallFunctionNoWait(sceneForm, BSFixedString(start ? "Start" : "Stop"), emptyArgs);
    LOG_UPDATED("[SceneSync] applied Scene.%s: formID=0x%08X",
        start ? "Start" : "Stop", sceneFormID);
}

// __try/__except wrapper; takes plain primitives so no C2712 issue.
__declspec(noinline) static void ApplySceneOpSafe(uint32_t sceneFormID, bool start)
{
    __try { ApplySceneOp_Impl(sceneFormID, start); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[SceneSync] ApplySceneOp faulted (formID=0x%08X %s) -- caught",
            sceneFormID, start ? "start" : "stop");
    }
}

void CoSyncSceneSync::FlushPendingSceneOps()
{
    std::vector<PendingSceneOp> ops;
    {
        std::lock_guard<std::mutex> lk(s_sceneMutex);
        s_pendingOps.swap(ops);
    }
    for (const auto& op : ops)
        ApplySceneOpSafe(op.sceneFormID, op.start);
}
