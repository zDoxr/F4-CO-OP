#include "CoSyncQuestSync.h"

#include "CoSyncRequirementsLog.h"
#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "GameForms.h"
#include "GameRTTI.h"
#include "GameData.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"
#include "CoSyncQuestHook.h"
#include "CoSyncQuestObjectiveHook.h"
#include "CoSyncWorld.h"

#include <mutex>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <chrono>

extern RelocAddr<_LookupFormByID> LookupFormByID;
extern RelocPtr<DataHandler*>     g_dataHandler;

// ─── Internal state ───────────────────────────────────────────────────────────

namespace
{
    // Pending ops written from transport thread, flushed on game main thread
    struct QuestOp
    {
        uint32_t questFormID = 0;
        int      stage       = 0;
    };
    struct QuestStartOp { uint32_t questFormID = 0; };
    struct QuestDoneOp  { uint32_t questFormID = 0; };

    // Objective ops
    struct QuestObjAllOp  { uint32_t questFormID = 0; };
    struct QuestObjSetOp  { uint32_t questFormID = 0; int32_t idx = 0; bool state = false; };
    struct QuestObjDispOp { uint32_t questFormID = 0; int32_t idx = 0; bool displayed = false; };

    std::mutex                   s_mutex;
    std::vector<QuestOp>         s_pending;
    std::vector<QuestStartOp>    s_pendingStarts;
    std::vector<QuestDoneOp>     s_pendingDones;
    std::vector<QuestObjAllOp>   s_pendingObjAll;
    std::vector<QuestObjSetOp>   s_pendingObjSet;
    std::vector<QuestObjDispOp>  s_pendingObjDisp;

    // Host: client stage requests (QSREQ), 0.4.2.
    struct QuestReqOp { uint32_t sender = 0; uint32_t questFormID = 0; int stage = 0; };
    std::vector<QuestReqOp>      s_pendingReqs;

    // ── Poll state (game main thread only — no mutex needed) ─────────────────
    std::unordered_set<uint32_t> s_knownActive;     // quests seen as running
    std::unordered_set<uint32_t> s_knownCompleted;  // quests seen as completed

    // questFormID -> the stage this machine last announced. See the note in
    // NotifyQuestStage: this is the ONLY current-stage record available,
    // because TESQuest's stage field is not exposed by the f4se struct.
    std::unordered_map<uint32_t, int> s_lastKnownStage;
    // T1 (2026-09-26): NotifyQuestStage runs inside the SetCurrentStageID
    // hook, which the thread census proved executes on Papyrus worker threads,
    // while TickLocalQuests reads/writes this map on the game thread. Every
    // access takes this lock; never call out (NotifyQuestStage, Send) while
    // holding it.
    std::mutex s_stageMutex;

    // Tracks quests that were started/completed via a remote packet so
    // TickLocalQuests doesn't echo them back.
    std::unordered_set<uint32_t> s_remoteStarted;
    std::unordered_set<uint32_t> s_remoteCompleted;
}

// True while FlushPendingQuestOps is applying remote state so that any
// game-thread quest-flag changes don't trigger an outbound echo.
static bool g_inRemoteQuestApply = false;

// CallFunctionNoWait_Internal's real RVA (0x01132200) was found via IDA
// against Fallout4.exe 1.11.221 -- see PapyrusNoWaitReloc.cpp. The two
// previously-tried addresses (0x011324E0, 0x01132340) both jumped through a
// null/wrong function pointer (RIP=0, AV type=EXEC); this one is a genuine,
// confirmed function with the right calling convention (verified
// structurally against the confirmed-good SendCustomEvent_Internal sibling).
// Re-enabled; the SEH guard + latch below stays as a safety net in case
// Quest objects specifically hit some edge case the verification missed.
static bool s_papyrusDispatchKnownBad = false;

// UPDATE 2026-06-17: __declspec(noinline) is load-bearing -- a non-noinline
// static __try/__except function risks silent SEH-removal via inlining
// under this project's /GL+/LTCG Release config (see CoSyncDoorHook.cpp's
// SafeReadU32/SafeReadPtr for the fuller writeup and confirmed repro).
__declspec(noinline) static void CallStartNoTry(TESForm* form, VMArray<VMVariable>& args)
{
    static const BSFixedString kStart("Start");
    CallFunctionNoWait(form, kStart, args);
}

__declspec(noinline) static bool SafeCallFunctionNoWaitStart(TESForm* form, VMArray<VMVariable>& args)
{
    LOG_REQUIREMENT("(global)::SafeCallFunctionNoWaitStart", "Needs a valid form and the game's script system to be fully loaded. Wrapped in a safety guard so a bad pointer can't crash the game.");
    __try
    {
        CallStartNoTry(form, args);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Same pattern as SafeCallFunctionNoWaitStart but for CompleteAllObjectives.
// QS_DONE previously called through CoSyncQuestObjectiveHook_ApplyCompleteAll
// (the MinHook trampoline), which requires a TESQuest* cast via DYNAMIC_CAST
// and whose SafeCallOrig* SEH guards were non-noinline (silently inlined away
// under /GL+/LTCG — same bug as the logging crash). Using CallFunctionNoWait
// here goes through the same async Papyrus VM path as QS_ST, needs no cast,
// and is already verified correct at RVA 0x01132200.
__declspec(noinline) static void CallCompleteAllObjectivesNoTry(TESForm* form, VMArray<VMVariable>& args)
{
    static const BSFixedString kCompleteAllObjectives("CompleteAllObjectives");
    CallFunctionNoWait(form, kCompleteAllObjectives, args);
}

__declspec(noinline) static bool SafeCallFunctionNoWaitCompleteAll(TESForm* form, VMArray<VMVariable>& args)
{
    LOG_REQUIREMENT("(global)::SafeCallFunctionNoWaitCompleteAll", "Needs a valid form and the game's script system to be fully loaded. Wrapped in a safety guard so a bad pointer can't crash the game.");
    __try
    {
        CallCompleteAllObjectivesNoTry(form, args);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// ─── Packet: Stage (QS|senderID|formID|stage) ────────────────────────────────

bool CoSyncQuestSync::IsQuestStage(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncQuestSync::IsQuestStage", "Doesn't touch the game -- just checks whether the message text starts with the right prefix.");
    return msg.rfind("QS|", 0) == 0;
}

std::string CoSyncQuestSync::SerializeQuestStage(
    uint32_t senderEntityID, uint32_t questFormID, int stage)
{
    LOG_REQUIREMENT("CoSyncQuestSync::SerializeQuestStage", "Doesn't touch the game -- just builds a text message out of a few numbers.");
    char buf[128];
    std::snprintf(buf, sizeof(buf), "QS|%u|%u|%d", senderEntityID, questFormID, stage);
    return std::string(buf);
}

static bool ParseQuestStage(const std::string& msg,
    uint32_t& outSender, uint32_t& outFormID, int& outStage)
{
    LOG_REQUIREMENT("(global)::ParseQuestStage", "Doesn't touch the game -- just reads a text message apart. A badly formed message is safely rejected instead of crashing anything.");
    if (msg.rfind("QS|", 0) != 0) return false;
    std::stringstream ss(msg.substr(3));
    std::string tok;
    if (!std::getline(ss, tok, '|') || tok.empty()) return false;
    outSender = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (!std::getline(ss, tok, '|') || tok.empty()) return false;
    outFormID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (outFormID == 0) return false;
    if (!std::getline(ss, tok) || tok.empty()) return false;
    outStage = static_cast<int>(std::strtol(tok.c_str(), nullptr, 10));
    return true;
}

void CoSyncQuestSync::NotifyQuestStage(
    uint32_t questFormID, int stage, uint32_t senderEntityID)
{
    LOG_REQUIREMENT("CoSyncQuestSync::NotifyQuestStage", "Needs an active connection and a real quest ID. Stays quiet while we're applying a quest update that came from someone else, so we don't echo it straight back to them.");
    if (!CoSyncNet::IsConnected() || questFormID == 0) return;
    if (g_inRemoteQuestApply) return;

    // Remember the stage we last announced. TESQuest's current stage is not
    // reachable from the f4se struct (it falls inside the unk0F8 blob), and
    // stage changes only ever reach us through CoSyncQuestHook's
    // SetCurrentStageID detour -- so tracking what we broadcast IS the only
    // record of current stage we have. TickLocalQuests_Impl replays it for
    // late joiners, who would otherwise receive QS_ST (quest active) with no
    // stage at all and sit at stage 0 forever.
    {
        std::lock_guard<std::mutex> lk(s_stageMutex);
        s_lastKnownStage[questFormID] = stage;
    }

    const std::string msg = SerializeQuestStage(senderEntityID, questFormID, stage);
    CoSyncTransport::Send(msg);
    LOG_INFO("[QuestSync] TX QS sender=%u quest=0x%08X stage=%d",
        senderEntityID, questFormID, stage);
}

void CoSyncQuestSync::OnReceiveQuestStage(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncQuestSync::OnReceiveQuestStage", "Needs a valid message string. Relies on the parser to reject a badly formed message, and ignores anything that just echoed back from our own sender ID.");
    uint32_t senderEntityID = 0, questFormID = 0;
    int stage = 0;
    if (!ParseQuestStage(msg, senderEntityID, questFormID, stage))
    {
        LOG_WARN("[QuestSync] Malformed QS: '%s'", msg.c_str());
        return;
    }
    if (senderEntityID != 0 && senderEntityID == CoSyncNet::GetMyEntityID()) return;

    LOG_INFO("[QuestSync] RX QS quest=0x%08X stage=%d", questFormID, stage);
    std::lock_guard<std::mutex> lk(s_mutex);
    s_pending.push_back({ questFormID, stage });
}

// ─── Packet: client stage request (QSREQ|senderID|formID|stage), 0.4.2 ───────

__declspec(noinline) static void CallSetStageNoTry(TESForm* form, VMArray<VMVariable>& args)
{
    static const BSFixedString kSetStage("SetStage");
    CallFunctionNoWait(form, kSetStage, args);
}

__declspec(noinline) static bool SafeCallFunctionNoWaitSetStage(TESForm* form, VMArray<VMVariable>& args)
{
    LOG_REQUIREMENT("(global)::SafeCallFunctionNoWaitSetStage", "Host, game thread, world loaded. Queues Quest.SetStage on the Papyrus VM; SEH-guarded.");
    __try
    {
        CallSetStageNoTry(form, args);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool CoSyncQuestSync::IsQuestStageRequest(const std::string& msg)
{
    return msg.rfind("QSREQ|", 0) == 0;
}

void CoSyncQuestSync::RequestQuestStage(uint32_t questFormID, int stage, uint32_t senderEntityID)
{
    LOG_REQUIREMENT("CoSyncQuestSync::RequestQuestStage", "Client, any thread (the SetCurrentStageID hook runs on Papyrus threads). Only sends.");
    if (!CoSyncNet::IsConnected() || CoSyncNet::IsHost() || questFormID == 0 || senderEntityID == 0)
        return;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "QSREQ|%u|%u|%d", senderEntityID, questFormID, stage);
    CoSyncTransport::Send(std::string(buf));
    LOG_INFO("[QuestSync] TX QSREQ quest=0x%08X stage=%d (client progress)", questFormID, stage);
}

void CoSyncQuestSync::OnReceiveQuestStageRequest(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncQuestSync::OnReceiveQuestStageRequest", "Host only; the sender check in CoSyncNet already bound the first field to the connection. Parses and queues.");
    if (!CoSyncNet::IsHost())
        return;
    QuestReqOp op{};
    if (std::sscanf(msg.c_str() + 6, "%u|%u|%d", &op.sender, &op.questFormID, &op.stage) != 3 ||
        op.questFormID == 0 || (op.questFormID & 0xFF000000u) == 0xFF000000u || op.stage < 0 || op.stage > 0xFFFF)
    {
        LOG_WARN("[QuestSync] Malformed QSREQ: '%s'", msg.c_str());
        return;
    }
    std::lock_guard<std::mutex> lk(s_mutex);
    if (s_pendingReqs.size() < 256)
        s_pendingReqs.push_back(op);
}

// Host, game thread: applies queued client requests.
static void ApplyClientStageRequests(std::vector<QuestReqOp>& reqs)
{
    static std::unordered_map<uint32_t, std::pair<double, int>> s_budget;   // sender -> (window, count)
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (const QuestReqOp& op : reqs)
    {
        auto& b = s_budget[op.sender];
        if (now - b.first >= 1.0) { b.first = now; b.second = 0; }
        if (++b.second > 30)
        {
            LOG_UPDATED_WARN("[QuestSync] QSREQ from entity=%u over 30/s -- dropped quest=0x%08X stage=%d",
                op.sender, op.questFormID, op.stage);
            continue;
        }
        TESQuest* quest = nullptr;
        TESForm* form = LookupFormByID(op.questFormID);
        if (form && form->formType == kFormType_QUST)
            quest = static_cast<TESQuest*>(form);
        if (!quest)
        {
            LOG_UPDATED_WARN("[QuestSync] QSREQ quest=0x%08X from entity=%u is not a quest here -- ignored", op.questFormID, op.sender);
            continue;
        }
        const int done = CoSyncQuestHook_IsStageDone(quest, op.stage);
        if (done == 1)
        {
            LOG_UPDATED("[QuestSync] QSREQ quest=0x%08X stage=%d from entity=%u: already done here -- nothing to do",
                op.questFormID, op.stage, op.sender);
            continue;
        }
        if (s_papyrusDispatchKnownBad)
            continue;
        VMArray<VMVariable> args;
        SInt32 st = op.stage;
        VMVariable vStage; vStage.Set(&st); args.Push(&vStage);
        if (SafeCallFunctionNoWaitSetStage(form, args))
            LOG_UPDATED("[QuestSync] QSREQ quest=0x%08X stage=%d from entity=%u -> SetStage on the host (stage done here: %s); "
                        "the host's hook broadcasts it to everyone",
                op.questFormID, op.stage, op.sender, done == 0 ? "no" : "unknown");
        else
            LOG_ERROR("[QuestSync] QSREQ quest=0x%08X stage=%d: SetStage through the script system faulted", op.questFormID, op.stage);
    }
}

// ─── Packet: Start (QS_ST|senderID|formID) ───────────────────────────────────

bool CoSyncQuestSync::IsQuestStart(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncQuestSync::IsQuestStart", "Doesn't touch the game -- just checks whether the message text starts with the right prefix.");
    return msg.rfind("QS_ST|", 0) == 0;
}

std::string CoSyncQuestSync::SerializeQuestStart(uint32_t senderEntityID, uint32_t questFormID)
{
    LOG_REQUIREMENT("CoSyncQuestSync::SerializeQuestStart", "Doesn't touch the game -- just builds a text message out of a few numbers.");
    char buf[128];
    std::snprintf(buf, sizeof(buf), "QS_ST|%u|%u", senderEntityID, questFormID);
    return std::string(buf);
}

void CoSyncQuestSync::NotifyQuestStart(uint32_t questFormID, uint32_t senderEntityID)
{
    LOG_REQUIREMENT("CoSyncQuestSync::NotifyQuestStart", "Needs an active connection and a real quest ID. Stays quiet while we're applying a quest update that came from someone else, so we don't echo it straight back to them.");
    if (!CoSyncNet::IsConnected() || questFormID == 0) return;
    if (g_inRemoteQuestApply) return;

    CoSyncTransport::Send(SerializeQuestStart(senderEntityID, questFormID));
    LOG_UPDATED("[QuestSync] TX QS_ST sender=%u quest=0x%08X", senderEntityID, questFormID);
}

void CoSyncQuestSync::OnReceiveQuestStart(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncQuestSync::OnReceiveQuestStart", "Needs the message to be at least as long as its \"QS_ST|\" prefix and contain valid numbers separated by '|'. A badly formed message is rejected.");
    const char* p = msg.c_str() + 6;
    char* e = nullptr;
    uint32_t senderID = static_cast<uint32_t>(std::strtoul(p, &e, 10));
    if (!e || *e != '|') { LOG_UPDATED_WARN("[QuestSync] Malformed QS_ST: '%s'", msg.c_str()); return; }
    uint32_t formID = static_cast<uint32_t>(std::strtoul(e + 1, nullptr, 10));
    if (formID == 0) return;
    if (senderID != 0 && senderID == CoSyncNet::GetMyEntityID()) return;

    LOG_UPDATED("[QuestSync] RX QS_ST quest=0x%08X — queueing Start", formID);
    std::lock_guard<std::mutex> lk(s_mutex);
    s_pendingStarts.push_back({ formID });
}

// ─── Packet: Done (QS_DONE|senderID|formID) ──────────────────────────────────

bool CoSyncQuestSync::IsQuestDone(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncQuestSync::IsQuestDone", "Doesn't touch the game -- just checks whether the message text starts with the right prefix.");
    return msg.rfind("QS_DONE|", 0) == 0;
}

std::string CoSyncQuestSync::SerializeQuestDone(uint32_t senderEntityID, uint32_t questFormID)
{
    LOG_REQUIREMENT("CoSyncQuestSync::SerializeQuestDone", "Doesn't touch the game -- just builds a text message out of a few numbers.");
    char buf[128];
    std::snprintf(buf, sizeof(buf), "QS_DONE|%u|%u", senderEntityID, questFormID);
    return std::string(buf);
}

void CoSyncQuestSync::NotifyQuestDone(uint32_t questFormID, uint32_t senderEntityID)
{
    LOG_REQUIREMENT("CoSyncQuestSync::NotifyQuestDone", "Needs an active connection and a real quest ID. Stays quiet while we're applying a quest update that came from someone else, so we don't echo it straight back to them.");
    if (!CoSyncNet::IsConnected() || questFormID == 0) return;
    if (g_inRemoteQuestApply) return;

    CoSyncTransport::Send(SerializeQuestDone(senderEntityID, questFormID));
    LOG_UPDATED("[QuestSync] TX QS_DONE sender=%u quest=0x%08X", senderEntityID, questFormID);
}

void CoSyncQuestSync::OnReceiveQuestDone(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncQuestSync::OnReceiveQuestDone", "Needs the message to be at least as long as its \"QS_DONE|\" prefix and contain valid numbers separated by '|'. A badly formed message is rejected.");
    const char* p = msg.c_str() + 8;
    char* e = nullptr;
    uint32_t senderID = static_cast<uint32_t>(std::strtoul(p, &e, 10));
    if (!e || *e != '|') { LOG_UPDATED_WARN("[QuestSync] Malformed QS_DONE: '%s'", msg.c_str()); return; }
    uint32_t formID = static_cast<uint32_t>(std::strtoul(e + 1, nullptr, 10));
    if (formID == 0) return;
    if (senderID != 0 && senderID == CoSyncNet::GetMyEntityID()) return;

    LOG_UPDATED("[QuestSync] RX QS_DONE quest=0x%08X — queueing CompleteAllObjectives", formID);
    std::lock_guard<std::mutex> lk(s_mutex);
    s_pendingDones.push_back({ formID });
}

// ─── Flush (game main thread) ─────────────────────────────────────────────────

// UPDATE 2026-06-20 (round 41 audit): peer-controlled op.questFormID fed
// straight into LookupFormByID+DYNAMIC_CAST with zero SEH, same shape as the
// CoSyncDoorHook/CoSyncLockHook crash already found and fixed twice this
// session (round 39/40) -- and confirmed via TTD+live-log analysis to be the
// most likely actual source of the post-recovery "garbage RIP" fatal crash
// chasing every connect attempt (CoSyncNet::Tick calls into this and 7 other
// FlushPendingXOps functions every tick; any one of them faulting on a
// peer-sent garbage FormID would explain a crash landing in heap garbage
// shortly after Tick resumes from the unrelated 0x1A7FC54 recovery). Wrapped
// in the same noinline-SEH-helper pattern as SafeLookupAndCastDoorRefr.
__declspec(noinline) static TESForm* SafeLookupForm(uint32_t formID)
{
    LOG_REQUIREMENT("(global)::SafeLookupForm", "Works safely with any ID value, even a bad one sent by another player, because the lookup runs inside a safety guard. Returns nothing if it fails or isn't found.");
    __try
    {
        return LookupFormByID(formID);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

__declspec(noinline) static TESQuest* SafeLookupAndCastQuest(uint32_t formID)
{
    LOG_REQUIREMENT("(global)::SafeLookupAndCastQuest", "Works safely with any ID value, even a bad one sent by another player, because the lookup and type check both run inside a safety guard. Returns nothing if it fails, isn't found, or isn't actually a quest.");
    __try
    {
        TESForm* form = LookupFormByID(formID);
        if (!form) return nullptr;

        // 2026-08-04: was DYNAMIC_CAST(form, TESForm, TESQuest). That routes
        // through Runtime_DynamicCast -> __RTDynamicCast, which walks the
        // object's vtable and RTTI locators and CALLS through pointers read
        // out of memory -- consistent with the crash signature here (VEH
        // "Known bad-function-pointer crash", RIP = a heap address, which a
        // plain RelocAddr call can never produce). A form-type comparison
        // needs no RTTI at all and is the pattern used everywhere else in this
        // codebase (FormToREFR, AsProjectileBase, ...).
        //
        // Uses the SYMBOL, never a hardcoded number -- restating a form-type
        // value by hand is what silently broke the throwable sync (PROJ is 51,
        // not 62).
        if (form->formType != TESQuest::kTypeID) return nullptr;
        return static_cast<TESQuest*>(form);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

void CoSyncQuestSync::FlushPendingQuestOps()
{
    LOG_REQUIREMENT("CoSyncQuestSync::FlushPendingQuestOps", "Waits until the game world is fully loaded before doing anything, since the script system it calls into isn't ready before then.");
    // CallFunctionNoWait() dispatches through the Papyrus VM's internal
    // function table. On a freshly-connecting client that table isn't set
    // up yet until the world has actually finished loading (same window
    // where g_player/CoSyncGetPlayer() is still null) -- a QS_ST packet
    // that arrives and gets flushed during that window jumps through a
    // null pointer (RIP=0, AV type=EXEC). Don't drain the pending queues
    // until the world is ready; leave them queued so a later tick (once
    // ready) applies them, same defer-not-drop pattern as PlayerMgr spawns.
    if (!CoSyncWorld::IsWorldReady())
        return;

    std::vector<QuestStartOp> starts;
    std::vector<QuestOp>      stages;
    std::vector<QuestDoneOp>  dones;
    std::vector<QuestReqOp>   reqs;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_pendingStarts.empty() && s_pending.empty() && s_pendingDones.empty() && s_pendingReqs.empty())
            return;
        starts.swap(s_pendingStarts);
        stages.swap(s_pending);
        dones.swap(s_pendingDones);
        reqs.swap(s_pendingReqs);
    }
    // Client progress first, outside the remote-apply bracket: SetStage runs
    // later on the VM and the host's own hook must broadcast it.
    if (!reqs.empty())
        ApplyClientStageRequests(reqs);

    g_inRemoteQuestApply = true;

    for (const QuestStartOp& op : starts)
    {
        // Always mark as remote-started so TickLocalQuests doesn't echo it back.
        s_remoteStarted.insert(op.questFormID);

        // Skip repeated Start() calls — host re-broadcasts active quests every ~16s
        // for late joiners; calling Start() on an already-running quest is harmless
        // but spams the VM queue. s_knownActive is populated by TickLocalQuests as
        // it polls the DataHandler quest array.
        if (s_knownActive.count(op.questFormID))
        {
            LOG_UPDATED("[QuestSync] QS_ST: quest=0x%08X already active — skipped re-Start", op.questFormID);
            continue;
        }

        TESForm* form = SafeLookupForm(op.questFormID);
        if (!form) { LOG_UPDATED_WARN("[QuestSync] QS_ST: quest 0x%08X not found", op.questFormID); continue; }
        // UPDATE 2026-06-17: this comment was stale -- CallFunctionNoWait_Internal
        // WAS fixed (see PapyrusNoWaitReloc.cpp, RVA 0x01132200, confirmed via IDA),
        // and SafeCallFunctionNoWaitStart below already calls the fixed version
        // (CallFunctionNoWait<T> -> CallFunctionNoWait_Internal). Matches the
        // up-to-date comment at this file's top (line ~71-73). SEH-guard + latch
        // kept as a safety net only, same as that comment says.
        if (!s_papyrusDispatchKnownBad)
        {
            VMArray<VMVariable> noArgs;
            if (SafeCallFunctionNoWaitStart(form, noArgs))
            {
                LOG_UPDATED("[QuestSync] Started quest=0x%08X", op.questFormID);
            }
            else
            {
                s_papyrusDispatchKnownBad = true;
                LOG_ERROR("[QuestSync] Starting the quest through the script system failed -- "
                          "quest Start syncing is turned off for this session "
                          "(quest=0x%08X)", op.questFormID);
            }
        }
    }

    for (const QuestOp& op : stages)
    {
        // Bracket each step. The stages loop crashed three sessions running
        // with NO log between "Started quest" and the VEH line, which left the
        // failing helper ambiguous (lookup+cast vs. the stage setter). These
        // two lines make the next occurrence self-identifying instead of
        // costing another rebuild to bisect.
        LOG_UPDATED("[QuestSync] QS apply: resolving quest=0x%08X stage=%d",
            op.questFormID, op.stage);

        TESQuest* quest = SafeLookupAndCastQuest(op.questFormID);
        if (!quest) { LOG_UPDATED_WARN("[QuestSync] QS: quest 0x%08X not found or not a TESQuest", op.questFormID); continue; }

        LOG_UPDATED("[QuestSync] QS apply: quest resolved, calling stage setter");

        if (CoSyncQuestHook_ApplySetCurrentStageID(quest, op.stage))
            LOG_UPDATED("[QuestSync] SetCurrentStageID quest=0x%08X stage=%d", op.questFormID, op.stage);
    }

    for (const QuestDoneOp& op : dones)
    {
        TESForm* form = SafeLookupForm(op.questFormID);
        if (!form) { LOG_UPDATED_WARN("[QuestSync] QS_DONE: quest 0x%08X not found", op.questFormID); continue; }

        s_remoteCompleted.insert(op.questFormID);
        if (!s_papyrusDispatchKnownBad)
        {
            VMArray<VMVariable> noArgs;
            if (SafeCallFunctionNoWaitCompleteAll(form, noArgs))
            {
                LOG_UPDATED("[QuestSync] CompleteAllObjectives quest=0x%08X", op.questFormID);
            }
            else
            {
                s_papyrusDispatchKnownBad = true;
                LOG_ERROR("[QuestSync] Completing the quest through the script system failed -- "
                          "quest Done syncing is turned off for this session (quest=0x%08X)", op.questFormID);
            }
        }
    }

    g_inRemoteQuestApply = false;
}

// ─── IsRemoteApplying accessor (used by objective hook detours) ──────────────

bool CoSyncQuestSync::IsRemoteApplying()
{
    LOG_REQUIREMENT("CoSyncQuestSync::IsRemoteApplying", "Doesn't touch the game -- just reads an internal flag, safe to call from anywhere.");
    return g_inRemoteQuestApply;
}

// ─── Packets: Objective – CompleteAll (QS_OBJ_ALL|sender|formID) ─────────────

bool CoSyncQuestSync::IsQuestObjAll(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncQuestSync::IsQuestObjAll", "Doesn't touch the game -- just checks whether the message text starts with the right prefix.");
    return msg.rfind("QS_OBJ_ALL|", 0) == 0;
}

std::string CoSyncQuestSync::SerializeObjAll(uint32_t questFormID, uint32_t senderEntityID)
{
    LOG_REQUIREMENT("CoSyncQuestSync::SerializeObjAll", "Doesn't touch the game -- just builds a text message out of a few numbers.");
    char buf[128];
    std::snprintf(buf, sizeof(buf), "QS_OBJ_ALL|%u|%u", senderEntityID, questFormID);
    return std::string(buf);
}

void CoSyncQuestSync::NotifyObjAll(uint32_t questFormID, uint32_t senderEntityID)
{
    LOG_REQUIREMENT("CoSyncQuestSync::NotifyObjAll", "Needs an active connection and a real quest ID. Stays quiet while we're applying a quest update that came from someone else, so we don't echo it straight back to them.");
    if (!CoSyncNet::IsConnected() || questFormID == 0) return;
    if (g_inRemoteQuestApply) return;
    CoSyncTransport::Send(SerializeObjAll(questFormID, senderEntityID));
}

void CoSyncQuestSync::OnReceiveObjAll(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncQuestSync::OnReceiveObjAll", "Needs the message to be at least as long as its \"QS_OBJ_ALL|\" prefix and contain valid numbers separated by '|'. A badly formed message is rejected.");
    const char* p = msg.c_str() + 11; // skip "QS_OBJ_ALL|"
    char* e = nullptr;
    uint32_t senderID = static_cast<uint32_t>(std::strtoul(p, &e, 10));
    if (!e || *e != '|') return;
    uint32_t formID = static_cast<uint32_t>(std::strtoul(e + 1, nullptr, 10));
    if (formID == 0) return;
    if (senderID != 0 && senderID == CoSyncNet::GetMyEntityID()) return;

    LOG_INFO("[QuestSync] RX QS_OBJ_ALL quest=0x%08X", formID);
    std::lock_guard<std::mutex> lk(s_mutex);
    s_pendingObjAll.push_back({ formID });
}

// ─── Packets: Objective – SetCompleted (QS_OBJ_SET|sender|formID|idx|state) ──

bool CoSyncQuestSync::IsQuestObjSet(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncQuestSync::IsQuestObjSet", "Doesn't touch the game -- just checks whether the message text starts with the right prefix.");
    return msg.rfind("QS_OBJ_SET|", 0) == 0;
}

std::string CoSyncQuestSync::SerializeObjSet(
    uint32_t questFormID, int32_t idx, bool state, uint32_t senderEntityID)
{
    LOG_REQUIREMENT("CoSyncQuestSync::SerializeObjSet", "Doesn't touch the game -- just builds a text message out of a few numbers and a true/false flag.");
    char buf[160];
    std::snprintf(buf, sizeof(buf), "QS_OBJ_SET|%u|%u|%d|%d",
        senderEntityID, questFormID, idx, state ? 1 : 0);
    return std::string(buf);
}

void CoSyncQuestSync::NotifyObjSet(
    uint32_t questFormID, int32_t idx, bool state, uint32_t senderEntityID)
{
    LOG_REQUIREMENT("CoSyncQuestSync::NotifyObjSet", "Needs an active connection and a real quest ID. Stays quiet while we're applying a quest update that came from someone else, so we don't echo it straight back to them.");
    if (!CoSyncNet::IsConnected() || questFormID == 0) return;
    if (g_inRemoteQuestApply) return;
    CoSyncTransport::Send(SerializeObjSet(questFormID, idx, state, senderEntityID));
}

void CoSyncQuestSync::OnReceiveObjSet(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncQuestSync::OnReceiveObjSet", "Needs the message to be at least as long as its \"QS_OBJ_SET|\" prefix and contain four valid fields separated by '|', including a flag character at the end.");
    const char* p = msg.c_str() + 11; // skip "QS_OBJ_SET|"
    char* e = nullptr;
    uint32_t senderID = static_cast<uint32_t>(std::strtoul(p, &e, 10));
    if (!e || *e != '|') return;
    p = e + 1;
    uint32_t formID = static_cast<uint32_t>(std::strtoul(p, &e, 10));
    if (!e || *e != '|') return;
    p = e + 1;
    int32_t idx = static_cast<int32_t>(std::strtol(p, &e, 10));
    if (!e || *e != '|') return;
    bool state = (*(e + 1) == '1');

    if (formID == 0) return;
    if (senderID != 0 && senderID == CoSyncNet::GetMyEntityID()) return;

    LOG_INFO("[QuestSync] RX QS_OBJ_SET quest=0x%08X idx=%d state=%d", formID, idx, (int)state);
    std::lock_guard<std::mutex> lk(s_mutex);
    s_pendingObjSet.push_back({ formID, idx, state });
}

// ─── Packets: Objective – SetDisplayed (QS_OBJ_DISP|sender|formID|idx|state) ─

bool CoSyncQuestSync::IsQuestObjDisp(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncQuestSync::IsQuestObjDisp", "Doesn't touch the game -- just checks whether the message text starts with the right prefix.");
    return msg.rfind("QS_OBJ_DISP|", 0) == 0;
}

std::string CoSyncQuestSync::SerializeObjDisp(
    uint32_t questFormID, int32_t idx, bool displayed, uint32_t senderEntityID)
{
    LOG_REQUIREMENT("CoSyncQuestSync::SerializeObjDisp", "Doesn't touch the game -- just builds a text message out of a few numbers and a true/false flag.");
    char buf[160];
    std::snprintf(buf, sizeof(buf), "QS_OBJ_DISP|%u|%u|%d|%d",
        senderEntityID, questFormID, idx, displayed ? 1 : 0);
    return std::string(buf);
}

void CoSyncQuestSync::NotifyObjDisp(
    uint32_t questFormID, int32_t idx, bool displayed, uint32_t senderEntityID)
{
    LOG_REQUIREMENT("CoSyncQuestSync::NotifyObjDisp", "Needs an active connection and a real quest ID. Stays quiet while we're applying a quest update that came from someone else, so we don't echo it straight back to them.");
    if (!CoSyncNet::IsConnected() || questFormID == 0) return;
    if (g_inRemoteQuestApply) return;
    CoSyncTransport::Send(SerializeObjDisp(questFormID, idx, displayed, senderEntityID));
}

void CoSyncQuestSync::OnReceiveObjDisp(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncQuestSync::OnReceiveObjDisp", "Needs the message to be at least as long as its \"QS_OBJ_DISP|\" prefix and contain four valid fields separated by '|', including a flag character at the end.");
    const char* p = msg.c_str() + 12; // skip "QS_OBJ_DISP|"
    char* e = nullptr;
    uint32_t senderID = static_cast<uint32_t>(std::strtoul(p, &e, 10));
    if (!e || *e != '|') return;
    p = e + 1;
    uint32_t formID = static_cast<uint32_t>(std::strtoul(p, &e, 10));
    if (!e || *e != '|') return;
    p = e + 1;
    int32_t idx = static_cast<int32_t>(std::strtol(p, &e, 10));
    if (!e || *e != '|') return;
    bool displayed = (*(e + 1) == '1');

    if (formID == 0) return;
    if (senderID != 0 && senderID == CoSyncNet::GetMyEntityID()) return;

    LOG_INFO("[QuestSync] RX QS_OBJ_DISP quest=0x%08X idx=%d disp=%d", formID, idx, (int)displayed);
    std::lock_guard<std::mutex> lk(s_mutex);
    s_pendingObjDisp.push_back({ formID, idx, displayed });
}

// ─── Flush objective ops (game main thread) ───────────────────────────────────

void CoSyncQuestSync::FlushPendingQuestObjectiveOps()
{
    LOG_REQUIREMENT("CoSyncQuestSync::FlushPendingQuestObjectiveOps", "Waits until the game world is fully loaded before doing anything, for the same reason as FlushPendingQuestOps.");
    // Same Papyrus-VM-not-ready hazard as FlushPendingQuestOps() above.
    if (!CoSyncWorld::IsWorldReady())
        return;

    std::vector<QuestObjAllOp>  allOps;
    std::vector<QuestObjSetOp>  setOps;
    std::vector<QuestObjDispOp> dispOps;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_pendingObjAll.empty() && s_pendingObjSet.empty() && s_pendingObjDisp.empty())
            return;
        allOps.swap(s_pendingObjAll);
        setOps.swap(s_pendingObjSet);
        dispOps.swap(s_pendingObjDisp);
    }

    g_inRemoteQuestApply = true;

    for (const QuestObjAllOp& op : allOps)
    {
        TESQuest* quest = SafeLookupAndCastQuest(op.questFormID);
        if (!quest) { LOG_WARN("[QuestSync] QS_OBJ_ALL: quest 0x%08X not found or not a TESQuest", op.questFormID); continue; }

        if (CoSyncQuestObjectiveHook_ApplyCompleteAll(quest))
            LOG_UPDATED("[QuestSync] CompleteAllObjectives quest=0x%08X", op.questFormID);
    }

    for (const QuestObjSetOp& op : setOps)
    {
        TESQuest* quest = SafeLookupAndCastQuest(op.questFormID);
        if (!quest) { LOG_WARN("[QuestSync] QS_OBJ_SET: quest 0x%08X not found or not a TESQuest", op.questFormID); continue; }

        if (CoSyncQuestObjectiveHook_ApplySetObjectiveCompleted(quest, op.idx, op.state))
            LOG_UPDATED("[QuestSync] SetObjectiveCompleted quest=0x%08X idx=%d state=%d",
                op.questFormID, op.idx, (int)op.state);
    }

    for (const QuestObjDispOp& op : dispOps)
    {
        TESQuest* quest = SafeLookupAndCastQuest(op.questFormID);
        if (!quest) { LOG_WARN("[QuestSync] QS_OBJ_DISP: quest 0x%08X not found or not a TESQuest", op.questFormID); continue; }

        if (CoSyncQuestObjectiveHook_ApplySetObjectiveDisplayed(quest, op.idx, op.displayed, false))
            LOG_UPDATED("[QuestSync] SetObjectiveDisplayed quest=0x%08X idx=%d disp=%d",
                op.questFormID, op.idx, (int)op.displayed);
    }

    g_inRemoteQuestApply = false;
}

// ─── Tick: poll DataHandler quest array for start/complete transitions ────────

// UPDATE 2026-06-18 (round 23 audit, exhaustive pass): split into a noinline
// _Impl plus a thin SEH wrapper -- same unguarded raw-engine-array pattern
// already confirmed and fixed 4 other times this session (HostTickObjects,
// TickScanWorldActors, ClientBindWorldActor, HostScanCompanions): a raw loop
// over an engine-owned tArray (here dh->arrQUST), casting and dereferencing
// ->formID/->flags/->IsCompleted() on every entry, with zero protection,
// running every 2s while connected.
// Set by CoSyncQuestSync::RequestFullStateBroadcast when a client joins;
// consumed by the next TickLocalQuests_Impl, which forces a scan first.
static bool s_fullBroadcastPending = false;

// Last quest the HOST was seen tracking, so a change can be broadcast once.
static uint32_t s_lastTrackedQuest = 0;
// Received tracked-quest request (0 = none pending) and what we last applied.
static uint32_t s_pendingTrack       = 0;
static uint32_t s_clientTrackedQuest = 0;

// TESQuest::SetActive(TESQuest*, bool) -- the internal behind Papyrus
// Quest.SetActive. IDA-verified 2026-08-04 (native 0x01188A90 tail-jumps here):
// it rewrites the 0x800 bit of the flags word at +0xF4 and then calls the
// engine's quest-UI refresh (sub_140680490 / sub_14067FFC0), so the Pip-Boy
// actually updates rather than just the bit changing.
using SetQuestActiveFn = void(__fastcall*)(TESQuest*, bool);
static RelocAddr<SetQuestActiveFn> g_setQuestActive(0x0066AE00);

__declspec(noinline) static bool SafeSetQuestTracked(TESQuest* quest, bool tracked)
{
    LOG_REQUIREMENT("(global)::SafeSetQuestTracked", "Needs a valid quest object, checked by the caller. Calls the game's own tracking function inside a safety guard, since it changes game state and refreshes the quest UI.");
    __try
    {
        g_setQuestActive(quest, tracked);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// TESQuest current stage: UInt16 at +0x2B4.
//
// Verified 2026-08-04 against Fallout4.exe 1.11.221 (IDA, canonical disk IDB),
// derived rather than guessed: the Papyrus "GetCurrentStageID" string
// (0x025CF800) leads to its native registration at 0x01186711, which stores
// impl 0x011889C0; that impl is just `mov rcx,r8; jmp sub_14066B800`, and
// 0x0066B800 is `movzx eax, word ptr [rcx+2B4h]; retn`. So this reads exactly
// what Papyrus GetCurrentStageID returns, without a call.
//
// WHY THIS EXISTS: s_lastKnownStage used to be populated ONLY by the Papyrus
// SetCurrentStageID hook. A host that loaded a save and never advanced a quest
// during the session therefore had an EMPTY stage table, so a joining client
// received "quest started" with no stage and sat at stage 0 -- the journal
// entry appears but with no progress, which is what "quests were not working"
// looked like. Confirmed in the 2026-08-04 log: plenty of QS_ST/QS_DONE, not a
// single QS_STAGE. Reading the field also picks up stage changes made by
// native/engine code, which the Papyrus-only hook never saw.
static constexpr uintptr_t kQuestCurrentStage_Offset = 0x2B4;

// "Is this quest running?" -- the exact expression Papyrus Quest.IsRunning
// evaluates, IDA-verified 2026-08-04 on 1.11.221 (native 0x011889F0, reached
// from the "IsRunning" string 0x024C3B48 -> registration 0x01186BD3):
//
//     (flags & 1) != 0  &&  (flags & 0x80) == 0  &&  *(uint64*)(quest+0x2D0) == 0
//
// WHY THIS REPLACED `flags & 0x800`: 0x800 is f4se's SetActive bit, and it
// corresponds to NEITHER IsRunning NOR IsCompleted. With it, the host's scan
// matched exactly ONE quest for an entire session (0x000229E5) and never
// noticed another quest starting or any stage moving -- so a client could
// never follow the host onto a different quest. Verified against the same
// runtime: IsCompleted really is (flags & 2), which is what this file already
// used and is left alone.
//
// NOTE: Quest.IsActive is a DIFFERENT concept (sub_14066AAC0) -- that is the
// Pip-Boy "tracked quest" selection, not whether the quest is in progress.
// Progress sync wants IsRunning.
static constexpr uintptr_t kQuestFlags_Offset = 0xF4;

// "Is this the quest the player is TRACKING in the Pip-Boy?"
//
// IDA-verified 2026-08-04: Papyrus Quest.IsActive (native 0x011889D0) tail-
// jumps sub_14066AAC0, which is exactly
//     return (*(WORD*)(quest + 0xF4) & 0x800) != 0;
//
// So `flags & 0x800` -- which this file originally used and which I wrongly
// "corrected" -- was never a broken guess. It reads the TRACKED-quest bit, and
// only one quest is tracked at a time, which is why the host's scan legitimately
// reported a single quest.
//
// Replacing it with the real IsRunning test was the actual mistake: IsRunning
// is true for ~293 quests on a normal save (FO4 keeps many hidden/system quests
// running), so the host broadcast 585 QS_ST packets and the CLIENT FORCE-STARTED
// ~293 system quests, corrupting its journal. Quest starts must stay scoped to
// what the player is actually doing.
__declspec(noinline) static bool IsQuestTracked(TESQuest* quest)
{
    LOG_REQUIREMENT("(global)::IsQuestTracked", "Needs a valid quest object. Reads a small flags value from the game's own quest data inside a safety guard, reporting false if that fails.");
    if (!quest) return false;
    __try
    {
        const uint16_t flags =
            *reinterpret_cast<const uint16_t*>(
                reinterpret_cast<const uint8_t*>(quest) + kQuestFlags_Offset);
        return (flags & 0x800u) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

__declspec(noinline) static uint16_t GetQuestCurrentStage(TESQuest* quest)
{
    LOG_REQUIREMENT("(global)::GetQuestCurrentStage", "Needs a valid quest object. Reads the quest's current stage number from the game's own data inside a safety guard, returning 0 if that fails.");
    if (!quest) return 0;
    __try
    {
        return *reinterpret_cast<const uint16_t*>(
            reinterpret_cast<const uint8_t*>(quest) + kQuestCurrentStage_Offset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

void CoSyncQuestSync::RequestFullStateBroadcast()
{
    LOG_REQUIREMENT("CoSyncQuestSync::RequestFullStateBroadcast", "Doesn't touch the game -- just sets a flag that the next quest check will pick up, so it's safe to call as soon as a new player says hello.");
    s_fullBroadcastPending = true;
}

__declspec(noinline) static void TickLocalQuests_Impl(double now)
{
    LOG_REQUIREMENT("(global)::TickLocalQuests_Impl", "Needs the game's data handler to be ready. Loops over the game's own quest list, so every entry needs to stay valid for the whole loop, and is skipped entirely unless we're connected to someone.");
    // A pending join bypasses the 2s throttle: the scan below is what fills
    // s_knownActive/s_knownCompleted in the first place, and before any client
    // connects this whole function returns early (IsConnected is false), so at
    // the moment a HELLO lands those tables are still EMPTY. Broadcasting
    // straight from the HELLO handler would therefore have sent nothing at
    // all -- the scan has to run first.
    // 0.4s, was 2.0s. The scan is a flag read per quest, and the tracked-quest
    // swap is a UI action the other player is waiting on -- a 2s worst case
    // felt laggy. Start/complete authority is unaffected by the rate.
    static double s_lastTick = 0.0;
    if (!s_fullBroadcastPending && (now - s_lastTick < 0.4)) return;
    s_lastTick = now;

    if (!CoSyncNet::IsConnected()) return;

    // NOTE: NOT host-gated any more. Quest TRACKING is mirrored between
    // everyone -- any peer may switch quests and the rest follow. Quest
    // start/complete/stage authority below IS still host-only; a client
    // changing which quest it tracks must not start or advance anything.
    const bool isHost = CoSyncNet::IsHost();
    if (g_inRemoteQuestApply) return;
    if (!g_dataHandler || !*g_dataHandler) return;

    DataHandler* dh = *g_dataHandler;
    tArray<TESForm*>& questArr = dh->arrQUST;
    uint32_t myID = CoSyncNet::GetMyEntityID();

    for (uint32_t i = 0; i < static_cast<uint32_t>(questArr.count); ++i)
    {
        TESForm* form = questArr[i];
        if (!form) continue;
        TESQuest* quest = static_cast<TESQuest*>(form);
        uint32_t  formID = quest->formID;

        // Skip FF+ dynamic forms (shouldn't exist for quests but be safe)
        if ((formID & 0xFF000000u) == 0xFF000000u) continue;

        // The TRACKED quest only -- see IsQuestTracked. Deliberately NOT
        // IsRunning: that matches ~293 quests and force-started all of them on
        // the client.
        const bool isActive    = IsQuestTracked(quest);
        const bool isCompleted = quest->IsCompleted();   // (flags & 2), verified correct

        // ── Active transition ────────────────────────────────────────────────
        if (isActive)
        {
            if (isHost && !s_knownActive.count(formID))
            {
                s_knownActive.insert(formID);
                // Only broadcast if this wasn't a remote-triggered start
                if (!s_remoteStarted.count(formID))
                    CoSyncQuestSync::NotifyQuestStart(formID, myID);
                else
                    s_remoteStarted.erase(formID);
            }

            // Tell peers WHICH quest this machine is tracking, so everyone's
            // Pip-Boy selection (and quest marker) mirrors. Starting a quest is
            // not the same as tracking it -- that gap is why switching quests
            // never showed up on the other side even when QS_ST applied.
            //
            // Runs on HOST AND CLIENT: tracking is mirrored between everyone,
            // so any peer may switch quests.
            //
            // s_lastTrackedQuest is "the tracked quest as far as the NETWORK is
            // concerned" and is updated in BOTH places -- here when we
            // broadcast our own switch, and in ApplyPendingTrack when we adopt
            // a peer's. That single variable is all the echo suppression this
            // needs.
            //
            // 2026-08-04 BUG THIS REPLACES: the test also required
            // `formID != s_clientTrackedQuest`, and ApplyPendingTrack updated
            // ONLY s_clientTrackedQuest. So after adopting a peer's quest B,
            // s_lastTrackedQuest was left stale at A -- and thereafter the host
            // could never broadcast A (equal to the stale value) nor B (equal
            // to s_clientTrackedQuest), progressively locking it out. Live log:
            // client TX 10 vs host TX 3, i.e. "only the client can change
            // quests".
            if (formID != s_lastTrackedQuest)
            {
                s_lastTrackedQuest = formID;
                char tbuf[64];
                std::snprintf(tbuf, sizeof(tbuf), "QS_TRK|%u|%u", myID, formID);
                CoSyncTransport::Send(std::string(tbuf));
                LOG_INFO("[QuestSync] TX QS_TRK quest=0x%08X (now tracking, role=%s)",
                    formID, isHost ? "host" : "client");
            }

            // Read the REAL current stage off the quest rather than trusting
            // the Papyrus hook to have populated s_lastKnownStage. Broadcast on
            // change, so engine/native stage advances sync too -- the hook only
            // ever saw Papyrus SetCurrentStageID calls.
            if (isHost)
            {
                const uint16_t curStage = GetQuestCurrentStage(quest);
                bool stageChanged = false;
                {
                    std::lock_guard<std::mutex> lk(s_stageMutex);
                    auto known = s_lastKnownStage.find(formID);
                    if (known == s_lastKnownStage.end())
                        s_lastKnownStage[formID] = curStage;
                    else if (known->second != static_cast<int>(curStage))
                    {
                        known->second = curStage;
                        stageChanged = true;
                    }
                }
                if (stageChanged)
                {
                    CoSyncQuestSync::NotifyQuestStage(formID, curStage, myID);
                    LOG_UPDATED("[QuestSync] stage change detected quest=0x%08X -> %u (field read)",
                        formID, (unsigned)curStage);
                }
            }
        }
        else
        {
            s_knownActive.erase(formID);
        }

        // ── Completion transition (HOST ONLY: quest progress authority) ──────
        if (isHost && isCompleted && !s_knownCompleted.count(formID))
        {
            s_knownCompleted.insert(formID);
            if (!s_remoteCompleted.count(formID))
                CoSyncQuestSync::NotifyQuestDone(formID, myID);
            else
                s_remoteCompleted.erase(formID);
        }
    }

    // One line per scan showing what the corrected IsRunning test actually
    // finds. The old `flags & 0x800` test silently matched a single quest for
    // a whole session; a visible count makes that failure mode obvious rather
    // than something to infer from missing packets.
    {
        static size_t s_lastActiveCount = SIZE_MAX;
        if (s_knownActive.size() != s_lastActiveCount)
        {
            LOG_INFO("[QuestSync] running quests: %u (completed tracked: %u, stages tracked: %u)",
                (unsigned)s_knownActive.size(), (unsigned)s_knownCompleted.size(),
                (unsigned)[] { std::lock_guard<std::mutex> lk(s_stageMutex); return s_lastKnownStage.size(); }());
            s_lastActiveCount = s_knownActive.size();
        }
    }

    // Join catch-up: the scan above has just refreshed the tables, so this is
    // the earliest point at which a full broadcast can actually carry data.
    if (s_fullBroadcastPending)
    {
        s_fullBroadcastPending = false;
        LOG_INFO("[QuestSync] Client joined — pushing full quest state now");
        CoSyncQuestSync::BroadcastFullState();
        return;
    }

    // The 15s full re-broadcast is GONE. With ~292 running quests it re-sent
    // hundreds of packets every 15 seconds forever, for state the client
    // already had. Stage changes and completions are now detected and pushed
    // as they happen (see the scan above), so the only thing a full push is
    // needed for is a joining client -- which RequestFullStateBroadcast
    // already handles, once, spread over several ticks.
}

// Remaining quests still to be pushed to a joiner, drained a few per tick.
//
// 2026-08-04: correcting the "is running" test took the host's active-quest
// count from 1 to 292 -- Fallout 4 keeps a large number of hidden/system
// quests running, not just the ones in the player's journal. Sending the whole
// set at once meant ~584 packets in a single tick and 3230 quest messages in
// one short session. Even though the crash that session turned out to be the
// player marker, a burst that size is its own hazard, so the join push is now
// spread across ticks instead of dumped in one go.
static std::vector<uint32_t> s_pendingStartPush;
static std::vector<uint32_t> s_pendingDonePush;
static constexpr size_t kQuestPushPerTick = 12;

bool CoSyncQuestSync::IsQuestTrack(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncQuestSync::IsQuestTrack", "Doesn't touch the game -- just checks the message prefix.");
    return msg.rfind("QS_TRK|", 0) == 0;
}

void CoSyncQuestSync::OnReceiveQuestTrack(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncQuestSync::OnReceiveQuestTrack", "Reads a message from another player and just queues the quest ID -- nothing in the game changes until the update is applied on the main game thread.");
    const char* s = msg.c_str() + 7;
    char* e = nullptr;
    const uint32_t sender = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (!e || *e != '|') return;
    const uint32_t questFormID = static_cast<uint32_t>(std::strtoul(e + 1, nullptr, 10));
    if (questFormID == 0) return;
    if (sender != 0 && sender == CoSyncNet::GetMyEntityID()) return;

    LOG_INFO("[QuestSync] RX QS_TRK quest=0x%08X", questFormID);
    std::lock_guard<std::mutex> lk(s_mutex);
    s_pendingTrack = questFormID;
}

// Applies a received tracked-quest change. Game thread only.
void CoSyncQuestSync_ApplyPendingTrack()
{
    uint32_t questFormID = 0;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        questFormID = s_pendingTrack;
        s_pendingTrack = 0;
    }
    if (questFormID == 0) return;

    TESQuest* quest = SafeLookupAndCastQuest(questFormID);
    if (!quest)
    {
        LOG_UPDATED_WARN("[QuestSync] QS_TRK: quest 0x%08X not found or not a TESQuest", questFormID);
        return;
    }

    // Untrack whatever we were tracking first -- the engine allows only one
    // tracked quest, but clearing explicitly avoids relying on that.
    if (s_clientTrackedQuest != 0 && s_clientTrackedQuest != questFormID)
    {
        if (TESQuest* prev = SafeLookupAndCastQuest(s_clientTrackedQuest))
            SafeSetQuestTracked(prev, false);
    }

    if (SafeSetQuestTracked(quest, true))
    {
        s_clientTrackedQuest = questFormID;   // what we last SetActive'd (for untracking)
        // CRITICAL: also move the network's idea of the tracked quest, or our
        // own scan will see this adopted quest as "a change" and echo it back,
        // and worse, s_lastTrackedQuest goes stale and locks this peer out of
        // ever broadcasting its own switches. See the send test above.
        s_lastTrackedQuest = questFormID;
        LOG_UPDATED("[QuestSync] now tracking quest=0x%08X (following peer)", questFormID);
    }
    else
    {
        LOG_ERROR("[QuestSync] Failed to set quest=0x%08X as tracked", questFormID);
    }
}

void CoSyncQuestSync::BroadcastFullState()
{
    LOG_REQUIREMENT("CoSyncQuestSync::BroadcastFullState", "Only runs on the host. Just queues up the quests we already know about to be sent out later -- doesn't touch the game or send anything by itself.");
    if (!CoSyncNet::IsConnected() || !CoSyncNet::IsHost())
        return;

    s_pendingStartPush.assign(s_knownActive.begin(), s_knownActive.end());
    s_pendingDonePush.assign(s_knownCompleted.begin(), s_knownCompleted.end());

    LOG_INFO("[QuestSync] Full state queued for push: active=%u completed=%u "
             "(%u per tick)",
        (unsigned)s_pendingStartPush.size(), (unsigned)s_pendingDonePush.size(),
        (unsigned)kQuestPushPerTick);
}

// Drains the join-push queues a slice at a time. Called from the quest tick.
static void DrainQuestPushQueues()
{
    if (s_pendingStartPush.empty() && s_pendingDonePush.empty())
        return;
    if (!CoSyncNet::IsConnected() || !CoSyncNet::IsHost())
    {
        s_pendingStartPush.clear();
        s_pendingDonePush.clear();
        return;
    }

    const uint32_t myID = CoSyncNet::GetMyEntityID();
    size_t sent = 0;

    while (!s_pendingStartPush.empty() && sent < kQuestPushPerTick)
    {
        const uint32_t fid = s_pendingStartPush.back();
        s_pendingStartPush.pop_back();

        CoSyncQuestSync::NotifyQuestStart(fid, myID);
        ++sent;

        // Stage MUST follow the start, or the joiner ends up with the quest
        // running but stuck at stage 0 and would only catch up if the host
        // happened to advance that quest again.
        int  knownStage = 0;
        bool haveStage  = false;
        {
            std::lock_guard<std::mutex> lk(s_stageMutex);
            auto st = s_lastKnownStage.find(fid);
            if (st != s_lastKnownStage.end())
            {
                knownStage = st->second;
                haveStage  = true;
            }
        }
        if (haveStage)
        {
            CoSyncQuestSync::NotifyQuestStage(fid, knownStage, myID);
            ++sent;
        }
    }

    while (!s_pendingDonePush.empty() && sent < kQuestPushPerTick)
    {
        CoSyncQuestSync::NotifyQuestDone(s_pendingDonePush.back(), myID);
        s_pendingDonePush.pop_back();
        ++sent;
    }
}

void CoSyncQuestSync::TickLocalQuests(double now)
{
    LOG_REQUIREMENT("CoSyncQuestSync::TickLocalQuests", "No special requirements -- just wraps the quest scan in a safety guard so an error during the scan is caught instead of taking down the game.");
    __try
    {
        TickLocalQuests_Impl(now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[QuestSync] Quest scan hit an error -- caught, skipping this scan");
    }

    // Outside the scan's own throttle: the join push must keep draining every
    // tick until empty, not once every 2 seconds.
    DrainQuestPushQueues();

    // Apply a received tracked-quest change (game thread).
    __try
    {
        CoSyncQuestSync_ApplyPendingTrack();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[QuestSync] Applying the tracked-quest update hit an error -- caught");
    }
}
