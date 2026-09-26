#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncQuestObjectiveHook.h"
#include "CoSyncQuestSync.h"
#include "CoSyncNet.h"
#include "ConsoleLogger.h"
#include "MinHook.h"

#include "GameForms.h"
#include "PapyrusVM.h"
#include "CoSyncRequirementsLog.h"

#include <atomic>
#include <cstdint>

// UPDATE 2026-06-18 (round 23 audit): all three of these were wrong --
// confirmed via IDA that each one is mid-instruction inside the WRONG,
// unrelated native (0x01188990 landed inside a "play sound on object"
// function; 0x01185BA0 landed inside the REAL SetObjectiveDisplayed's body;
// 0x01185CF0 landed inside the REAL SetObjectiveFailed's body -- confirmed by
// the literal "displayed"/"not displayed" and "failed"/"not failed" message
// strings inside each). Re-derived properly via the Quest native
// registration function (sub_1411861C0): each literal Papyrus native name
// string's xref leads here, and the decompile shows the actual registered
// function pointer for each (`v71[10] = sub_141185A00;` etc). These three
// hooks were therefore EITHER not firing at all (CompleteAllObjectives,
// silently dead -- its detour was hooked into a same-shaped-but-irrelevant
// PlaySound function) OR splicing a JMP into the middle of vanilla
// SetObjectiveDisplayed/SetObjectiveFailed's real bodies (a crash risk
// during NORMAL questing, multiplayer or not -- same bug class as the
// 12-round NpcAnimSync mid-instruction hook).
static constexpr uintptr_t kCompleteAllObjectives_RVA = 0x01188B40;
static constexpr uintptr_t kSetObjectiveCompleted_RVA  = 0x01185D50;
static constexpr uintptr_t kSetObjectiveDisplayed_RVA  = 0x01185B50;

// Round 128 (IDA re-verify): the CompleteAllObjectives native at 0x11887F0 is
// a valid function start but only EIGHT bytes long — `mov rcx, r8; jmp
// sub_14066A0B0` — which MinHook can refuse to patch (too small / jmp in the
// stolen range). That is the "one or more targets" install failure. Fallback:
// hook the thunk's jmp target (the real body) instead; per the thunk, the
// body takes the TESQuest* as its FIRST argument (rcx), vm/stackID dropped.
static constexpr uintptr_t kCompleteAllBody_RVA = 0x0066A0B0;

using CompleteAllFn     = void(__fastcall*)(VirtualMachine*, uint32_t, TESQuest*);
using CompleteAllBodyFn = void(__fastcall*)(TESQuest*);
using SetObjCompleteFn = void(__fastcall*)(VirtualMachine*, uint32_t, TESQuest*, int32_t, bool);
using SetObjDisplayFn  = void(__fastcall*)(VirtualMachine*, uint32_t, TESQuest*, int32_t, bool, bool);

static CompleteAllFn     g_origCompleteAll     = nullptr;
static CompleteAllBodyFn g_origCompleteAllBody = nullptr;
static SetObjCompleteFn g_origSetObjComplete = nullptr;
static SetObjDisplayFn  g_origSetObjDisplay  = nullptr;

static std::atomic<bool> g_hooksInstalled { false };

thread_local bool g_objHookSuppressed = false;

static void RelayCompleteAll(TESQuest* quest)
{
    if (g_objHookSuppressed)                  return;
    if (CoSyncQuestSync::IsRemoteApplying())   return;
    if (!CoSyncNet::IsConnected())             return;
    if (!CoSyncNet::IsHost())                  return;  // host drives quest state
    if (!quest)                                return;

    const uint32_t fid = quest->formID;
    if (fid == 0 || (fid & 0xFF000000u) == 0xFF000000u) return;

    CoSyncQuestSync::NotifyObjAll(fid, CoSyncNet::GetMyEntityID());
    LOG_INFO("[ObjHook] TX OBJ_ALL quest=0x%08X", fid);
}

static void __fastcall Detour_CompleteAllObjectives(
    VirtualMachine* vm, uint32_t stackID, TESQuest* quest)
{
    LOG_REQUIREMENT("Detour_CompleteAllObjectives", "Needs the hook to be pointing at the real start of the original function, not somewhere in the middle, for the passthrough call to be safe.");
    if (g_origCompleteAll)
        g_origCompleteAll(vm, stackID, quest);
    RelayCompleteAll(quest);
}

// Round 128: fallback detour on the thunk's jmp target (real body). Quest in
// rcx per the thunk's `mov rcx, r8`. Note this body may also be reached by
// engine-internal callers other than the Papyrus native — broader coverage,
// same relay semantics.
static void __fastcall Detour_CompleteAllBody(TESQuest* quest)
{
    LOG_REQUIREMENT("Detour_CompleteAllBody", "Needs the fallback hook to be pointing at the real start of the underlying function it targets.");
    if (g_origCompleteAllBody)
        g_origCompleteAllBody(quest);
    RelayCompleteAll(quest);
}

static void __fastcall Detour_SetObjectiveCompleted(
    VirtualMachine* vm, uint32_t stackID, TESQuest* quest, int32_t idx, bool state)
{
    LOG_REQUIREMENT("Detour_SetObjectiveCompleted", "Needs the hook to be pointing at the real start of the original function, not somewhere in the middle, for the passthrough call to be safe.");
    if (g_origSetObjComplete)
        g_origSetObjComplete(vm, stackID, quest, idx, state);

    if (g_objHookSuppressed)                  return;
    if (CoSyncQuestSync::IsRemoteApplying())   return;
    if (!CoSyncNet::IsConnected())             return;
    if (!CoSyncNet::IsHost())                  return;  // host drives quest state
    if (!quest)                                return;

    const uint32_t fid = quest->formID;
    if (fid == 0 || (fid & 0xFF000000u) == 0xFF000000u) return;

    CoSyncQuestSync::NotifyObjSet(fid, idx, state, CoSyncNet::GetMyEntityID());
    LOG_INFO("[ObjHook] TX OBJ_SET quest=0x%08X idx=%d state=%d", fid, idx, (int)state);
}

static void __fastcall Detour_SetObjectiveDisplayed(
    VirtualMachine* vm, uint32_t stackID, TESQuest* quest, int32_t idx, bool displayed, bool force)
{
    LOG_REQUIREMENT("Detour_SetObjectiveDisplayed", "Needs the hook to be pointing at the real start of the original function, not somewhere in the middle, for the passthrough call to be safe.");
    if (g_origSetObjDisplay)
        g_origSetObjDisplay(vm, stackID, quest, idx, displayed, force);

    if (g_objHookSuppressed)                  return;
    if (CoSyncQuestSync::IsRemoteApplying())   return;
    if (!CoSyncNet::IsConnected())             return;
    if (!CoSyncNet::IsHost())                  return;  // host drives quest state
    if (!quest)                                return;

    const uint32_t fid = quest->formID;
    if (fid == 0 || (fid & 0xFF000000u) == 0xFF000000u) return;

    CoSyncQuestSync::NotifyObjDisp(fid, idx, displayed, CoSyncNet::GetMyEntityID());
    LOG_INFO("[ObjHook] TX OBJ_DISP quest=0x%08X idx=%d disp=%d", fid, idx, (int)displayed);
}

// __declspec(noinline) is load-bearing on all four helpers below — same reason
// as ConsoleLogger.h's SafeVsnprintf/SafeTimestampWrap: under /GL+/LTCG the
// optimizer inlines static __try/__except functions into their callers and
// silently drops the scope-table entries from the UNWIND_INFO, so the __except
// handler is never registered and faults propagate out uncaught to the VEH.
__declspec(noinline) static bool SafeCallOrigCompleteAll(VirtualMachine* vm, TESQuest* quest)
{
    LOG_REQUIREMENT("SafeCallOrigCompleteAll", "Needs the hook to already be installed. Guarded so a bad trampoline or quest pointer is caught safely instead of crashing.");
    __try { g_origCompleteAll(vm, 0, quest); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static bool SafeCallOrigSetObjComplete(VirtualMachine* vm, TESQuest* quest, int32_t idx, bool state)
{
    LOG_REQUIREMENT("SafeCallOrigSetObjComplete", "Needs the hook to already be installed. Guarded so a bad trampoline, quest pointer, or index is caught safely instead of crashing.");
    __try { g_origSetObjComplete(vm, 0, quest, idx, state); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static bool SafeCallOrigSetObjDisplay(VirtualMachine* vm, TESQuest* quest, int32_t idx, bool displayed, bool force)
{
    LOG_REQUIREMENT("SafeCallOrigSetObjDisplay", "Needs the hook to already be installed. Guarded so a bad trampoline, quest pointer, or index is caught safely instead of crashing.");
    __try { g_origSetObjDisplay(vm, 0, quest, idx, displayed, force); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static bool SafeCallOrigCompleteAllBody(TESQuest* quest)
{
    LOG_REQUIREMENT("SafeCallOrigCompleteAllBody", "Needs the fallback hook to already be installed. Guarded so a bad trampoline or quest pointer is caught safely instead of crashing.");
    __try { g_origCompleteAllBody(quest); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CoSyncQuestObjectiveHook_ApplyCompleteAll(TESQuest* quest)
{
    LOG_REQUIREMENT("CoSyncQuestObjectiveHook_ApplyCompleteAll", "Needs a valid quest and one of the CompleteAll hooks to already be installed. The game's VM pointer might be null; that case is guarded against.");
    if (!quest) return false;
    bool ok = false;
    if (g_origCompleteAll)
    {
        VirtualMachine* vm = (*g_gameVM) ? (*g_gameVM)->m_virtualMachine : nullptr;
        ok = SafeCallOrigCompleteAll(vm, quest);
    }
    else if (g_origCompleteAllBody)
    {
        ok = SafeCallOrigCompleteAllBody(quest);
    }
    if (!ok) LOG_ERROR("[ObjHook] ApplyCompleteAll faulted or no hook (quest=0x%08X)", quest->formID);
    return ok;
}

bool CoSyncQuestObjectiveHook_ApplySetObjectiveCompleted(TESQuest* quest, int32_t idx, bool state)
{
    LOG_REQUIREMENT("CoSyncQuestObjectiveHook_ApplySetObjectiveCompleted", "Needs a valid quest and the hook to already be installed. The game's VM pointer might be null; that case is guarded against.");
    if (!g_origSetObjComplete || !quest) return false;
    VirtualMachine* vm = (*g_gameVM) ? (*g_gameVM)->m_virtualMachine : nullptr;
    const bool ok = SafeCallOrigSetObjComplete(vm, quest, idx, state);
    if (!ok) LOG_ERROR("[ObjHook] ApplySetObjectiveCompleted faulted (quest=0x%08X idx=%d)", quest->formID, idx);
    return ok;
}

bool CoSyncQuestObjectiveHook_ApplySetObjectiveDisplayed(TESQuest* quest, int32_t idx, bool displayed, bool force)
{
    LOG_REQUIREMENT("CoSyncQuestObjectiveHook_ApplySetObjectiveDisplayed", "Needs a valid quest and the hook to already be installed. The game's VM pointer might be null; that case is guarded against.");
    if (!g_origSetObjDisplay || !quest) return false;
    VirtualMachine* vm = (*g_gameVM) ? (*g_gameVM)->m_virtualMachine : nullptr;
    const bool ok = SafeCallOrigSetObjDisplay(vm, quest, idx, displayed, force);
    if (!ok) LOG_ERROR("[ObjHook] ApplySetObjectiveDisplayed faulted (quest=0x%08X idx=%d)", quest->formID, idx);
    return ok;
}

void InstallQuestObjectiveHooks()
{
    LOG_REQUIREMENT("InstallQuestObjectiveHooks", "Must not already be installed. The hardcoded game addresses need to point at real function starts for this game build, and the module base address needs to resolve correctly.");
    if (g_hooksInstalled.exchange(true)) return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));

    void* tAll  = reinterpret_cast<void*>(base + kCompleteAllObjectives_RVA);
    void* tComp = reinterpret_cast<void*>(base + kSetObjectiveCompleted_RVA);
    void* tDisp = reinterpret_cast<void*>(base + kSetObjectiveDisplayed_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        LOG_ERROR("[ObjHook] MH_Initialize failed: %d", (int)st);
        g_hooksInstalled.store(false);
        return;
    }

    // Round 128: hook each target individually — the old short-circuit chain
    // reported only "one or more failed" and abandoned ALL three when a single
    // one failed. On any failure, dump the live first bytes of the target so
    // the log shows exactly what code the process really has there.
    auto logBytes = [](const char* name, void* target)
    {
        const unsigned char* b = reinterpret_cast<const unsigned char*>(target);
        LOG_ERROR("[ObjHook] %s target=%p live bytes: "
            "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            name, target,
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
            b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    };

    MH_STATUS stAll = MH_CreateHook(tAll,
        reinterpret_cast<void*>(&Detour_CompleteAllObjectives),
        reinterpret_cast<void**>(&g_origCompleteAll));
    if (stAll == MH_OK)
    {
        MH_EnableHook(tAll);
        LOG_INFO("[ObjHook] CompleteAllObjectives thunk hooked @%p", tAll);
    }
    else
    {
        LOG_WARN("[ObjHook] CreateHook(CompleteAllObjectives thunk) failed: %d — "
            "expected for the 8-byte thunk; falling back to its jmp-target body", (int)stAll);
        logBytes("CompleteAllObjectives(thunk)", tAll);
        void* tBody = reinterpret_cast<void*>(base + kCompleteAllBody_RVA);
        MH_STATUS stBody = MH_CreateHook(tBody,
            reinterpret_cast<void*>(&Detour_CompleteAllBody),
            reinterpret_cast<void**>(&g_origCompleteAllBody));
        if (stBody == MH_OK)
        {
            MH_EnableHook(tBody);
            LOG_INFO("[ObjHook] CompleteAllObjectives BODY hooked @%p (fallback)", tBody);
        }
        else
        {
            LOG_ERROR("[ObjHook] CreateHook(CompleteAllObjectives body) failed too: %d", (int)stBody);
            logBytes("CompleteAllBody", tBody);
        }
    }

    MH_STATUS stComp = MH_CreateHook(tComp,
        reinterpret_cast<void*>(&Detour_SetObjectiveCompleted),
        reinterpret_cast<void**>(&g_origSetObjComplete));
    if (stComp == MH_OK)
    {
        MH_EnableHook(tComp);
        LOG_INFO("[ObjHook] SetObjectiveCompleted hooked @%p", tComp);
    }
    else
    {
        LOG_ERROR("[ObjHook] CreateHook(SetObjectiveCompleted) failed: %d", (int)stComp);
        logBytes("SetObjectiveCompleted", tComp);
    }

    MH_STATUS stDisp = MH_CreateHook(tDisp,
        reinterpret_cast<void*>(&Detour_SetObjectiveDisplayed),
        reinterpret_cast<void**>(&g_origSetObjDisplay));
    if (stDisp == MH_OK)
    {
        MH_EnableHook(tDisp);
        LOG_INFO("[ObjHook] SetObjectiveDisplayed hooked @%p", tDisp);
    }
    else
    {
        LOG_ERROR("[ObjHook] CreateHook(SetObjectiveDisplayed) failed: %d", (int)stDisp);
        logBytes("SetObjectiveDisplayed", tDisp);
    }

    LOG_INFO("[ObjHook] Install pass done (thunk=%d comp=%d disp=%d)",
        (int)stAll, (int)stComp, (int)stDisp);
}

void UninstallQuestObjectiveHooks()
{
    LOG_REQUIREMENT("UninstallQuestObjectiveHooks", "Needs the hooks to currently be installed, and the module base address to match what it was at install time.");
    if (!g_hooksInstalled.exchange(false)) return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* tAll  = reinterpret_cast<void*>(base + kCompleteAllObjectives_RVA);
    void* tComp = reinterpret_cast<void*>(base + kSetObjectiveCompleted_RVA);
    void* tDisp = reinterpret_cast<void*>(base + kSetObjectiveDisplayed_RVA);

    MH_DisableHook(tAll);  MH_RemoveHook(tAll);
    MH_DisableHook(tComp); MH_RemoveHook(tComp);
    MH_DisableHook(tDisp); MH_RemoveHook(tDisp);

    LOG_INFO("[ObjHook] Uninstalled");
}
