#define WIN32_LEAN_AND_MEAN
#include "CoSyncSharedIntro.h"
#include <Windows.h>

#include "CoSyncQuestHook.h"
#include "CoSyncQuestSync.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "ConsoleLogger.h"
#include "MinHook.h"

#include "GameForms.h"
#include "PapyrusVM.h"
#include "CoSyncRequirementsLog.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include "CoSyncThreadDiag.h"

// Quest.SetCurrentStageID Papyrus dispatch — FO4 1.11.221
// Signature: void(VirtualMachine* vm, uint32_t stackID, TESQuest* thisObj, int32_t aiStageID)
// Confirmed: test [r8+0xF4],1  /  mov r14d,r9d  /  mov rdi,r8  /  mov rsi,rcx
// RVA CORRECTED round 93: IDA confirms true function entry is 0x1185DD0, not
// 0x1185F70 (same project-wide .pdata/Capstone disassembly-desync defect
// class found and fixed in rounds 90/93 elsewhere). Very high confidence:
// the true entry's debug strings literally read "Stage %d set%s" and
// "(quest will be started first)".
// RVA CORRECTED 2026-08-04: 0x01185F70 (was 0x01185DD0).
//
// CRASH THIS FIXES: 0x01185DD0 is NOT a function entry. IDA on 1.11.221 shows
// it sits 0xE0 bytes INSIDE an unrelated 6-parameter function bounded
// 0x01185CF0-0x01185E26. So MinHook was writing its jump into the MIDDLE of
// that function's body, and g_origSetStage pointed at relocated mid-body
// instructions. Calling it as g_origSetStage(vm, 0, quest, stage) executed
// garbage -- the client log's
//   [CoSync-VEH] Known bad-function-pointer crash (AV type=EXEC)
//   pre-recovery regs: RBP=0x00000000000229E5   <-- the quest formID as code
// This went unnoticed for a long time because the remote-apply path
// (CoSyncQuestHook_ApplySetCurrentStageID) had NEVER actually run: no QS_STAGE
// packet was ever sent until s_lastKnownStage started being populated from the
// quest object on 2026-08-04. Sessions before that logged 18-24 quest starts
// with zero crashes; the first two sessions after it crashed at 4.
//
// HOW 0x01185F70 WAS DERIVED (registration table, same technique as
// CallFunctionNoWait -- see [[reference-fo4-papyrus-nowait-rvas]]): the Papyrus
// string "SetCurrentStageID" (0x025B0288) is used at 0x011870CA, which calls
// the NativeFunction ctor (0x020F9DE0) and then stores the implementation with
// `lea rax,[rel 1185F70h]; mov [rdi+50h],rax`.
//
// IDA confirms 0x01185F70 is a real entry whose prologue does
// `test byte ptr [r8+0F4h], 1` and `mov r14d, r9d` -- r8 is the TESQuest
// (0xF4 is exactly TESQuest::flags per f4se's layout) and r9d is the stage,
// i.e. the signature is (vm, stackId, quest, stage), matching this file's
// SetStageFn typedef and the g_origSetStage call exactly.
static constexpr uintptr_t kSetCurrentStageID_RVA = 0x011862C0;

using SetCurrentStageID_t = void(__fastcall*)(
    VirtualMachine*  vm,
    uint32_t         stackID,
    TESQuest*        thisObj,
    int32_t          aiStageID);

static SetCurrentStageID_t  g_origSetStage  = nullptr;
static std::atomic<bool>    g_hookInstalled { false };

// Set true by FlushPendingQuestOps before it calls SetCurrentStageID via Papyrus
// so we don't echo received stage changes back to the network.
thread_local bool g_questHookSuppressed = false;

static void __fastcall Detour_SetCurrentStageID(
    VirtualMachine*  vm,
    uint32_t         stackID,
    TESQuest*        thisObj,
    int32_t          aiStageID)
{
    LOG_REQUIREMENT("Detour_SetCurrentStageID", "Needs the quest-stage hook to already be installed correctly, and a valid quest object to relay the stage change.");
    CoSyncThreadDiag::Note(CoSyncThreadDiag::kQuestStage);
    {
        static std::atomic<uint32_t> s_n{ 0 };
        const uint32_t n = ++s_n;
        if (n <= 10 || n % 100 == 0)
            LOG_INFO("[QuestHook] Detour_SetCurrentStageID fired #%u thisObj=%p stage=%d",
                n, thisObj, aiStageID);
    }

    // Remote-apply path (CoSyncQuestHook_ApplySetCurrentStageID bypasses this detour
    // via g_origSetStage directly, but keep the suppression guard for safety).
    if (g_questHookSuppressed)
    {
        if (g_origSetStage) g_origSetStage(vm, stackID, thisObj, aiStageID);
        return;
    }

    // CLIENT (0.4.2): a client's own progress counts. Before this, every stage a
    // client's scripts set (quest item picked up, objective reached, trigger
    // entered) was dropped and lost. Now the client advances its own quest --
    // its stage fragment runs here, so ITS player gets that stage's rewards and
    // journal update -- and asks the host to apply the same stage. The host
    // checks it and sets it through its own engine (its fragment runs: the
    // authoritative world changes), then broadcasts QS to everyone. Received
    // stages are applied through CoSyncQuestHook_ApplySetCurrentStageID (the
    // internal setter, not this native), so they never come back here.
    if (CoSyncNet::IsConnected() && !CoSyncNet::IsHost())
    {
        // 0.5.3: in a shared New Game intro the host's story is the only one.
        // This client's copy of MQ101 reacted to the host's stage 30 by setting
        // 70/65/75/85 itself; forwarded as QSREQ they pushed the host past the
        // Vault-Tec rep (2026-10-05 21:33:01). The host's stages still arrive
        // (applied past this hook) and run their fragments here.
        if (thisObj && CoSyncSharedIntro::BlockClientStage(thisObj->formID))
        {
            LOG_UPDATED("[QuestHook] [CLIENT] quest=0x%08X stage=%d not set here -- the shared intro follows the host's story",
                thisObj->formID, aiStageID);
            return;
        }
        if (g_origSetStage)
            g_origSetStage(vm, stackID, thisObj, aiStageID);
        if (!thisObj) return;
        const uint32_t qid = thisObj->formID;
        if (qid == 0 || (qid & 0xFF000000u) == 0xFF000000u) return;
        CoSyncQuestSync::RequestQuestStage(qid, aiStageID, CoSyncNet::GetMyEntityID());
        LOG_UPDATED("[QuestHook] [CLIENT] quest=0x%08X stage=%d set here -- asked the host to apply it for everyone",
            qid, aiStageID);
        return;
    }

    // HOST (or not connected): let the stage advance, then broadcast it.
    if (g_origSetStage)
        g_origSetStage(vm, stackID, thisObj, aiStageID);

    if (!CoSyncNet::IsConnected() || !thisObj) return;
    const uint32_t formID = thisObj->formID;
    if (formID == 0 || (formID & 0xFF000000u) == 0xFF000000u) return;

    CoSyncQuestSync::NotifyQuestStage(formID, aiStageID, CoSyncNet::GetMyEntityID());
    LOG_INFO("[QuestHook] TX SetCurrentStageID quest=0x%08X stage=%d", formID, aiStageID);
}

void InstallQuestStageHook()
{
    LOG_REQUIREMENT("InstallQuestStageHook", "Must not already be installed, and needs the game's quest-stage function address to be correct for this game version.");
    if (g_hookInstalled.exchange(true)) return;

    const uintptr_t base   = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void*           target = reinterpret_cast<void*>(base + kSetCurrentStageID_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        LOG_ERROR("[QuestHook] MH_Initialize failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Detour_SetCurrentStageID),
        reinterpret_cast<void**>(&g_origSetStage));
    if (st != MH_OK) {
        LOG_ERROR("[QuestHook] MH_CreateHook failed: %d  target=%p", (int)st, target);
        g_hookInstalled.store(false);
        return;
    }

    MH_EnableHook(target);
    LOG_INFO("[QuestHook] Installed at %p  RVA=0x%zX", target, kSetCurrentStageID_RVA);
}

// __declspec(noinline) is load-bearing — same /GL+/LTCG inlining-drops-SEH-scope
// bug confirmed on SafeCallOrigCompleteAll et al.; apply here for consistency.
__declspec(noinline) static bool SafeCallOrigSetStage(VirtualMachine* vm, TESQuest* quest, int32_t stage)
{
    LOG_REQUIREMENT("SafeCallOrigSetStage", "Needs the quest-stage hook to already be installed. Calls the original game function safely in case anything involved turns out invalid.");
    __try
    {
        g_origSetStage(vm, 0, quest, stage);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// TESQuest::SetCurrentStageID(TESQuest*, UInt16 stage) -- the INTERNAL C++
// setter, RVA 0x0066B230. Two arguments, no VirtualMachine, no stack ID.
//
// IDA-verified 2026-08-04 on 1.11.221: entry_match=True, 3 direct callers,
// prologue `test byte ptr [rcx+0F4h],1` (TESQuest::flags) and it walks the
// quest's own stage tables at +0x100/+0x138/+0x144. It is precisely the call
// the Papyrus SetCurrentStageID native (0x01185F70) makes to do the real work.
using SetStageInternalFn = char(__fastcall*)(TESQuest*, uint16_t);
static RelocAddr<SetStageInternalFn> g_setStageInternal(0x0066B550);

__declspec(noinline) static bool SafeSetStageInternal(TESQuest* quest, uint16_t stage)
{
    LOG_REQUIREMENT("SafeSetStageInternal", "Needs a valid quest (checked by the caller). Sets its stage safely in case the quest data turns out malformed.");
    __try
    {
        g_setStageInternal(quest, stage);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool CoSyncQuestHook_ApplySetCurrentStageID(TESQuest* quest, int32_t stage)
{
    LOG_REQUIREMENT("CoSyncQuestHook_ApplySetCurrentStageID", "Needs a valid quest (checked). Sets the stage directly, without needing the scripting engine involved.");
    if (!quest)
        return false;
    if (stage < 0 || stage > 0xFFFF)
    {
        LOG_WARN("[QuestHook] ApplySetCurrentStageID: stage %d out of range for quest=0x%08X",
            stage, quest->formID);
        return false;
    }

    // 2026-08-04: this used to call g_origSetStage (the trampoline to the
    // PAPYRUS NATIVE) as g_origSetStage(vm, 0, quest, stage). That crashed the
    // client the first time a QS_STAGE packet was ever applied:
    //   [CoSync-VEH] Known bad-function-pointer crash (AV type=EXEC)
    //   pre-recovery regs: RBP=0x00000000000229E5   <-- the quest formID
    //
    // Two things were wrong. The hooked RVA was not a function entry (fixed
    // separately above), and more fundamentally the Papyrus native is not safe
    // to call from outside the VM: IDA shows its error paths and its
    // story-manager dispatch (sub_1410D4370 / sub_1410D6140 / sub_1410D45C0)
    // all consume the vm + stackID arguments, and we were fabricating a stack
    // ID of 0. Calling the INTERNAL setter skips that latent-call bookkeeping
    // entirely and just sets the stage, which is all a remote apply wants.
    //
    // The hook on the native itself is still installed and still correct -- it
    // is how the HOST detects and broadcasts its own stage changes.
    const bool ok = SafeSetStageInternal(quest, static_cast<uint16_t>(stage));
    if (!ok)
        LOG_ERROR("[QuestHook] ApplySetCurrentStageID faulted (quest=0x%08X stage=%d)",
            quest->formID, stage);
    return ok;
}

// Engine IsStageDone (1.11.240): the Papyrus native sub_141185BF0 calls
// sub_14066B700(quest, uint16 stage) directly; it walks the quest's stage
// table (+0x138 / +0x144 / +0x100) and returns whether that stage is done.
__declspec(noinline) static int SafeIsStageDone(TESQuest* quest, uint16_t stage)
{
    static const uint8_t kPro[16] = { 0x48,0x89,0x5C,0x24,0x08,0x44,0x8B,0x81,0x44,0x01,0x00,0x00,0x48,0x8B,0x99,0x38 };
    const uintptr_t fn = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL)) + 0x0066B700;
    if (std::memcmp(reinterpret_cast<const void*>(fn), kPro, sizeof(kPro)) != 0)
        return -1;
    __try
    {
        return reinterpret_cast<bool(__fastcall*)(TESQuest*, uint16_t)>(fn)(quest, stage) ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

int CoSyncQuestHook_IsStageDone(TESQuest* quest, int32_t stage)
{
    if (!quest || stage < 0 || stage > 0xFFFF)
        return -1;
    return SafeIsStageDone(quest, static_cast<uint16_t>(stage));
}

void UninstallQuestStageHook()
{
    LOG_REQUIREMENT("UninstallQuestStageHook", "The hook must currently be installed, using the same game module address it was installed with.");
    if (!g_hookInstalled.exchange(false)) return;

    const uintptr_t base   = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void*           target = reinterpret_cast<void*>(base + kSetCurrentStageID_RVA);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    LOG_INFO("[QuestHook] Uninstalled");
}
