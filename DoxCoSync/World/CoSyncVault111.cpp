#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncVault111.h"

#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncGameAPI.h"
#include "CoSyncPlayer.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncProfiles.h"
#include "CoSyncQuestHook.h"
#include "CoSyncQuestSync.h"
#include "CoSyncRuntime.h"
#include "CoSyncWorld.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"

#include "GameForms.h"
#include "GameReferences.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern PlayerCharacter* CoSyncGetPlayer();
extern RelocAddr<_MoveRefrToPosition> MoveRefrToPosition;
extern CoSyncPlayerManager g_CoSyncPlayerManager;

namespace
{
    using CoSyncVault111::Phase;
    using CoSyncVault111::StagePolicy;

    // ── Fallout4.esm (ESM + compiled scripts, 2026-10-06) ───────────────────
    constexpr uint32_t kQuestMQ101            = 0x0001ED86;
    constexpr uint32_t kQuestMQ102            = 0x0001CC2A;
    constexpr uint32_t kQuestTutorial         = 0x00160147;
    constexpr uint32_t kQuestDialogueVault111 = 0x0001ED62;
    constexpr int      kMQ101VaultStage       = 600;   // the lift reaches PrewarVault111
    constexpr int      kMQ102AwakeStage       = 1;     // control back after the wake-up
    constexpr int      kMQ102PipTaken         = 4;     // Pip-Boy from the skeleton
    constexpr int      kMQ102PipTutorialDone  = 6;
    constexpr int      kMQ102PipPlugged       = 7;     // gear console: the real Pip-Boy
    constexpr int      kMQ102ConsoleDone      = 8;
    constexpr int      kMQ102DoorsStage       = 3;     // shared: QT00 door unlocked
    constexpr int      kMQ102GateStage        = 9;     // shared: the gear door opens
    constexpr int      kMQ102ExitedStage      = 10;    // this player is outside

    constexpr uint32_t kCellPrewarVault111    = 0x000CA260;
    constexpr uint32_t kCellVault111Cryo      = 0x000016D8;

    constexpr uint32_t kRefDescendTrigger     = 0x00020C3A;   // vaultExitElevatorDown (stage >= 9)
    constexpr uint32_t kRefRespecTrigger      = 0x0008C94A;   // RespecTriggerREF (the ride)
    constexpr uint32_t kRefRaiseMarker        = 0x001D6CD3;   // V111EnableRaiseElevatorTriggerEnableMarker
    constexpr uint32_t kRefGearDoor           = 0x000330C3;   // Vault111GearDoorREF

    // The exit's references whose enable state each machine's own scripts set
    // for its own player (traced from vaultExitElevatorSCRIPT,
    // vaultExitElevatorDownSCRIPT, vaultExitCommonWealthSCRIPT, MQ102 0/10).
    // Outside the two cells: the exterior one-shot trigger and its marker.
    constexpr uint32_t kPerPlayerExitRefs[] = {
        0x00020C3A,   // vaultExitElevatorDown trigger -- disables itself
        0x0008C94A,   // RespecTriggerREF -- disabled by MQ102 stage 10
        0x000481EC,   // QuickstartRespecTrigger
        0x001D6CD3,   // V111EnableRaiseElevatorTriggerEnableMarker
        0x001D6CD0,   // V111ElevatorCollisionPlatformEnableMarker
        0x000481D4,   // RespecLightingEnableMarker
        0x002075E4,   // V111ExitLightingEnableMarker
        0x00051877,   // Vault111OneShotExteriorElevatorTrigger -- disables itself
        0x0020E880,   // TutorialEventRegistrationTrigger
        0x00148B68,   // MQ203Vault111MagicDoorToPostWar -- MQ102 stage 0 disables it
        0x00148B69,   // MQ101Vault111PostWarMagicDoor   -- (the freeze -> wake jump)
        0x0001F180,   // Vault111InteriorAutoLoadDoorREF
    };

    // Pods, in both rooms (same layout). Index 0 = the scripted player pod.
    struct Pod { float x, y, z, rz; uint32_t prewarRef, postwarRef; };
    constexpr Pod kPods[] = {
        { 640.f, -288.f, -72.f, 0.f,      0x000CA2A8, 0x0004A011 },   // MQ101PlayerCryopodREF / MQPlayerCryopodREF
        { 384.f, -288.f, -72.f, 0.f,      0x000CA2B4, 0x0004A00F },
        { 128.f, -288.f, -72.f, 0.f,      0x000CA2B5, 0x0004A00E },
        { -64.f, -288.f, -72.f, 0.f,      0x000CA2B6, 0x00001934 },
        { 832.f, -736.f, -72.f, 3.14159f, 0x000CA2B1, 0x0004A016 },
        { 384.f, -736.f, -72.f, 3.14159f, 0x000CA2B2, 0x0004A015 },
        { 128.f, -736.f, -72.f, 3.14159f, 0x000CA2B3, 0x0004A014 },
    };
    constexpr int   kPodCount        = static_cast<int>(sizeof(kPods) / sizeof(kPods[0]));
    constexpr float kAisleY          = -512.f;   // between the two rows
    constexpr float kInPodRadius     = 90.f;     // standing in a pod (XY)
    constexpr float kWakeRadius      = 200.f;    // the wake-up stumble out of the pod
    constexpr float kResidentRadius  = 60.f;     // a resident seated in a pod (XY)
    constexpr float kReapplyRadius   = 300.f;    // still at the scripted pod after waking

    // Status flags (V111|st).
    constexpr uint32_t kStReapplied = 1u << 0;
    constexpr uint32_t kStPipEvent  = 1u << 1;
    constexpr uint32_t kStGate      = 1u << 2;
    constexpr uint32_t kStLowered   = 1u << 3;
    constexpr uint32_t kStRaised    = 1u << 4;
    constexpr uint32_t kStExited    = 1u << 5;

    // ── Local state (game thread unless noted) ──────────────────────────────
    std::atomic<int> s_phase{ static_cast<int>(Phase::Complete) };   // any thread reads
    std::atomic<bool> s_phaseKnown{ false };   // computed at least once since the session began
    Phase    s_lastLoggedPhase = Phase::Complete;
    bool     s_phaseLogged     = false;
    bool     s_reapplied       = false;
    uint32_t s_localFlags      = 0;
    uint32_t s_sentFlags       = 0xFFFFFFFFu;
    int      s_sentPhase       = -1;
    double   s_nextStatus      = 0.0;
    double   s_nextWorldSend   = 0.0;
    double   s_nextPodsSend    = 0.0;
    double   s_nextPipCheck    = 0.0;
    double   s_nextResidentScan = 0.0;
    double   s_nextGateApply   = 0.0;
    bool     s_holdLogged      = false;
    bool     s_respecRestored  = false;
    bool     s_pipSettled      = false;   // outside the vault and holding the Pip-Boy: stop checking
    std::unordered_set<uint32_t> s_vacatedResidents;
    std::string s_lastAssignmentLog;

    // Pods: the host's list, as received (or built, on the host).
    std::mutex            s_podMx;
    std::vector<uint32_t> s_podOrder;

    // Other players' reported state.
    struct Remote { int phase = -1; uint32_t flags = 0; };
    std::mutex                           s_remoteMx;
    std::unordered_map<uint32_t, Remote> s_remote;

    // The host's world (host: computed; client: received).
    std::atomic<bool> s_worldPip{ false };
    std::atomic<bool> s_worldGate{ false };
    std::atomic<bool> s_clientPipEvent{ false };   // host: a client reported its Pip-Boy event

    // Deferred MQ102 stages (any thread adds, game thread drains).
    struct Deferred { bool isRequest; uint32_t sender; uint32_t quest; int stage; };
    std::mutex            s_deferMx;
    std::vector<Deferred> s_deferred;

    double Now() { return static_cast<double>(GetTickCount64()) / 1000.0; }

    bool SessionLive() { return CoSyncTransport::IsInitialized() && CoSyncNet::IsConnected(); }
    bool IsHostRole()  { return CoSyncTransport::IsHost(); }

    __declspec(noinline) uint32_t SafeCellID(TESObjectREFR* ref)
    {
        __try { return (ref && ref->parentCell) ? ref->parentCell->formID : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    __declspec(noinline) bool SafeIsDisabled(TESObjectREFR* ref)
    {
        __try { return ref && (ref->flags & TESObjectREFR::kFlag_IsDisabled) != 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool InPodRoom(uint32_t cell) { return cell == kCellPrewarVault111 || cell == kCellVault111Cryo; }

    TESQuest* Quest(uint32_t id)
    {
        TESForm* f = LookupFormByID(id);
        return (f && f->formType == kFormType_QUST) ? static_cast<TESQuest*>(f) : nullptr;
    }

    // TESQuest flags WORD at +0xF4: 0x1 running, 0x2 completed, 0x80 stopping
    // (the IsRunning / IsCompleted natives, as CoSyncQuestSync reads them).
    __declspec(noinline) uint16_t QuestFlags(TESQuest* q)
    {
        __try { return q ? *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(q) + 0xF4) : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    bool Running(TESQuest* q)   { const uint16_t f = QuestFlags(q); return (f & 0x1) && !(f & 0x80) && !(f & 0x2); }
    bool Completed(TESQuest* q) { return (QuestFlags(q) & 0x2) != 0; }
    bool StageDone(TESQuest* q, int stage) { return q && CoSyncQuestHook_IsStageDone(q, stage) == 1; }

    TESObjectREFR* Ref(uint32_t id)
    {
        TESForm* f = LookupFormByID(id);
        return (f && f->formType == kFormType_REFR) ? static_cast<TESObjectREFR*>(f) : nullptr;
    }

    bool AnyBlockingMenu()
    {
        return CoSyncRuntime_IsMenuOpen("LoadingMenu") || CoSyncRuntime_IsMenuOpen("LooksMenu") ||
               CoSyncRuntime_IsMenuOpen("SPECIALMenu") || CoSyncRuntime_IsMenuOpen("PipboyMenu") ||
               CoSyncRuntime_IsMenuOpen("MessageBoxMenu") || CoSyncRuntime_IsMenuOpen("DialogueMenu");
    }

    // ── Phase (derived from the game every tick) ────────────────────────────
    Phase ComputePhase(PlayerCharacter* pc)
    {
        TESQuest* mq101 = Quest(kQuestMQ101);
        TESQuest* mq102 = Quest(kQuestMQ102);
        if (Running(mq101))
        {
            const uint32_t cell = SafeCellID(reinterpret_cast<TESObjectREFR*>(pc));
            const int stage = CoSyncQuestSync::GetLocalStage(kQuestMQ101);
            return (stage >= kMQ101VaultStage || InPodRoom(cell)) ? Phase::VanillaSequence : Phase::PreWar;
        }
        if (Completed(mq101) && Running(mq102) && !StageDone(mq102, kMQ102ExitedStage))
            return Phase::VaultExplore;
        return Phase::Complete;
    }

    bool InsideVault(Phase p) { return p == Phase::VanillaSequence || p == Phase::VaultExplore; }

    // ── Pods ────────────────────────────────────────────────────────────────
    std::vector<uint32_t> PodOrder()
    {
        {
            std::lock_guard<std::mutex> lk(s_podMx);
            if (!s_podOrder.empty())
                return s_podOrder;
        }
        // Not received yet: the same rule the host uses, from what we know.
        std::vector<uint32_t> order;
        const uint32_t host = IsHostRole() ? CoSyncNet::GetMyEntityID() : CoSyncNet::GetHostEntityID();
        if (host)
            order.push_back(host);
        std::vector<uint32_t> others;
        const uint32_t me = CoSyncNet::GetMyEntityID();
        if (me && me != host)
            others.push_back(me);
        for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
        {
            const CoSyncPlayer* p = kv.second.get();
            if (p && !p->isLocalPlayer && p->createType == CoSyncEntityType::Player &&
                p->entityID != host && p->entityID != me)
                others.push_back(p->entityID);
        }
        std::sort(others.begin(), others.end());
        order.insert(order.end(), others.begin(), others.end());
        return order;
    }

    int IndexIn(const std::vector<uint32_t>& order, uint32_t eid)
    {
        for (size_t i = 0; i < order.size(); ++i)
            if (order[i] == eid)
                return static_cast<int>(i);
        return -1;
    }

    // Where a pod index stands in the room: in the pod, or (7+) in the aisle.
    void PodSpot(int index, float& x, float& y, float& z, float& rz)
    {
        if (index >= 0 && index < kPodCount)
        {
            x = kPods[index].x; y = kPods[index].y; z = kPods[index].z; rz = kPods[index].rz;
            return;
        }
        const int over = (std::max)(0, index - kPodCount);
        x = 640.f - 96.f * static_cast<float>(over); y = kAisleY; z = -72.f; rz = 0.f;
    }

    // In front of a pod, in the aisle (where a player steps out to).
    void StandSpot(int index, float& x, float& y)
    {
        float z = 0.f, rz = 0.f;
        PodSpot(index, x, y, z, rz);
        if (index >= 0 && index < kPodCount)
            y = (y > kAisleY) ? y - 140.f : y + 140.f;
    }

    std::string PlayerName(uint32_t eid)
    {
        if (eid == CoSyncNet::GetMyEntityID())
            return CoSyncNet::GetMyName() ? CoSyncNet::GetMyName() : "";
        for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
            if (kv.second && kv.second->entityID == eid)
                return kv.second->username;
        return std::string();
    }

    void LogAssignment(uint32_t cell)
    {
        const std::vector<uint32_t> order = PodOrder();
        std::string key;
        for (uint32_t e : order) key += std::to_string(e) + ",";
        key += std::to_string(cell);
        if (key == s_lastAssignmentLog)
            return;
        s_lastAssignmentLog = key;
        const bool prewar = cell == kCellPrewarVault111;
        for (int i = 0; i < static_cast<int>(order.size()); ++i)
        {
            const uint32_t eid = order[i];
            if (i < kPodCount)
                LOG_UPDATED("[CoSyncVault111] Player assigned to Cryopod  SteamID=%llu EntityID=%u Username=%s CryopodFormID=0x%08X (pod %d%s)",
                    static_cast<unsigned long long>(eid == CoSyncNet::GetMyEntityID() ? CoSyncNet::GetMySteamID() : CoSyncNet::GetSteamIDForEntity(eid)),
                    eid, PlayerName(eid).c_str(), prewar ? kPods[i].prewarRef : kPods[i].postwarRef, i,
                    i == 0 ? ", the scripted player pod" : "");
            else
                LOG_UPDATED("[CoSyncVault111] Player has no cryopod (all %d taken)  EntityID=%u Username=%s -- stands in the aisle",
                    kPodCount, eid, PlayerName(eid).c_str());
        }
        for (int i = static_cast<int>(order.size()); i < kPodCount; ++i)
            LOG_UPDATED("[CoSyncVault111] Cryopod left empty  CryopodFormID=0x%08X (pod %d, vanilla occupant kept)",
                prewar ? kPods[i].prewarRef : kPods[i].postwarRef, i);
    }

    // Residents leave the pods given to players (this machine only). Pods no
    // player has keep their vanilla occupant.
    __declspec(noinline) void ScanPodResidentsImpl(TESObjectCELL* cell, PlayerCharacter* pc, int podsInUse,
                                                    std::vector<TESObjectREFR*>& out)
    {
        for (UInt32 i = 0; i < cell->objectList.count; ++i)
        {
            TESObjectREFR* r = cell->objectList.entries[i];
            if (!r || r->formType != kFormType_ACHR || r == reinterpret_cast<TESObjectREFR*>(pc))
                continue;
            for (int p = 1; p < podsInUse && p < kPodCount; ++p)
            {
                const float dx = r->pos.x - kPods[p].x, dy = r->pos.y - kPods[p].y;
                if (dx * dx + dy * dy <= kResidentRadius * kResidentRadius && std::fabs(r->pos.z - kPods[p].z) < 150.f)
                {
                    out.push_back(r);
                    break;
                }
            }
        }
    }

    __declspec(noinline) bool SafeScanPodResidents(TESObjectCELL* cell, PlayerCharacter* pc, int podsInUse,
                                                    std::vector<TESObjectREFR*>& out)
    {
        __try { ScanPodResidentsImpl(cell, pc, podsInUse, out); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void VacateAssignedPods(PlayerCharacter* pc)
    {
        TESObjectCELL* cell = pc->parentCell;
        if (!cell)
            return;
        const int podsInUse = static_cast<int>(PodOrder().size());
        if (podsInUse < 2)
            return;   // alone: the vanilla vault
        std::vector<TESObjectREFR*> found;
        if (!SafeScanPodResidents(cell, pc, podsInUse, found))
            return;
        for (TESObjectREFR* r : found)
        {
            const CoSyncPlayer* asPlayer = g_CoSyncPlayerManager.GetPlayerByActorFormID(r->formID);
            if (asPlayer && asPlayer->createType == CoSyncEntityType::Player)
                continue;   // a player's proxy shown in its pod
            if (!s_vacatedResidents.insert(r->formID).second)
                continue;
            CoSyncGameAPI::DisableWorldActor(r);
            LOG_UPDATED("[CoSyncVault111] resident 0x%08X left a player's cryopod (base 0x%08X)",
                r->formID, r->baseForm ? r->baseForm->formID : 0);
        }
    }

    bool RemoteReapplied(uint32_t eid)
    {
        std::lock_guard<std::mutex> lk(s_remoteMx);
        auto it = s_remote.find(eid);
        return it != s_remote.end() && (it->second.flags & kStReapplied) != 0;
    }

    void Teleport(PlayerCharacter* pc, const NiPoint3& pos, float rotZ)
    {
        NiPoint3 p = pos;
        NiPoint3 r(0.f, 0.f, rotZ);
        UInt32 nullHandle = 0;
        MoveRefrToPosition(reinterpret_cast<TESObjectREFR*>(pc), &nullHandle, pc->parentCell, nullptr, &p, &r);
    }

    // ── Reapply: once, when the vanilla wake-up has finished ────────────────
    void Reapply(PlayerCharacter* pc)
    {
        s_reapplied = true;
        const uint32_t me = CoSyncNet::GetMyEntityID();
        const std::vector<uint32_t> order = PodOrder();
        const int idx = IndexIn(order, me);
        LOG_UPDATED("[CoSyncVault111] Vanilla Cryopod sequence completed (MQ101 done, MQ102 stage %d) -- Reapplying multiplayer state",
            kMQ102AwakeStage);
        const uint32_t cell = SafeCellID(reinterpret_cast<TESObjectREFR*>(pc));
        if (cell == kCellVault111Cryo && idx > 0)
        {
            const float dx = pc->pos.x - kPods[0].x, dy = pc->pos.y - kPods[0].y;
            if (dx * dx + dy * dy <= kReapplyRadius * kReapplyRadius)
            {
                float x = 0.f, y = 0.f;
                StandSpot(idx, x, y);
                Teleport(pc, NiPoint3(x, y, pc->pos.z), pc->rot.z);
                LOG_UPDATED("[CoSyncVault111] stood in front of my own cryopod (pod %d%s) at (%.0f,%.0f) -- positions agree on every machine from here",
                    idx, idx < kPodCount ? "" : ", none free: the aisle", x, y);
            }
            else
                LOG_UPDATED("[CoSyncVault111] already away from the scripted pod -- not moved (pod %d)", idx);
        }
        LOG_UPDATED("[CoSyncVault111]   pods=%zu my index=%d  Pip-Boy shared=%d  gear door (host)=%d -- transforms are real from now on",
            order.size(), idx, s_worldPip.load() ? 1 : 0, s_worldGate.load() ? 1 : 0);
        s_nextStatus = 0.0;   // tell everyone now
    }

    // ── Pip-Boy ─────────────────────────────────────────────────────────────
    // Not in the middle of a Pip-Boy scripted moment (skeleton take anim,
    // gear console furniture), the wake-up, or a menu.
    bool PipBoySafeNow(Phase phase, TESQuest* mq102)
    {
        if (phase == Phase::PreWar || phase == Phase::VanillaSequence)
            return false;
        if (phase == Phase::VaultExplore && !s_reapplied)
            return false;
        if (AnyBlockingMenu())
            return false;
        if (mq102 && Running(mq102))
        {
            if (StageDone(mq102, kMQ102PipTaken) && !StageDone(mq102, kMQ102PipTutorialDone))
                return false;
            if (StageDone(mq102, kMQ102PipPlugged) && !StageDone(mq102, kMQ102ConsoleDone))
                return false;
        }
        return true;
    }

    void TickPipBoy(Phase phase, double now)
    {
        if (now < s_nextPipCheck || (s_pipSettled && phase == Phase::Complete))
            return;
        s_nextPipCheck = now + 2.0;
        TESQuest* mq101 = Quest(kQuestMQ101);
        TESQuest* mq102 = Quest(kQuestMQ102);
        const bool introDone = Completed(mq101);
        const bool localEvent = introDone && (StageDone(mq102, kMQ102PipTaken) || StageDone(mq102, kMQ102PipPlugged));
        if (localEvent)
            s_localFlags |= kStPipEvent;

        if (IsHostRole())
        {
            // Persistent: the host's own stages, or the host carrying the
            // Pip-Boy after the intro (it gets one when a client's event comes).
            const bool hostCarries = introDone && Running(mq102) &&
                                     CoSyncProfiles::LocalItemCount(0x00021B3B) > 0;
            const bool pip = localEvent || hostCarries || s_clientPipEvent.load() ||
                             (introDone && !Running(mq102) && StageDone(mq102, kMQ102ExitedStage));
            if (pip && !s_worldPip.load())
                LOG_UPDATED("[CoSyncVault111] Pip-Boy event completed (%s) -- every player gets the Pip-Boy",
                    localEvent ? "the host's own" : hostCarries ? "the host carries it" : "a client's");
            s_worldPip.store(pip);
        }

        if (!s_worldPip.load() || !CoSyncProfiles::IsCharacterApplied())
            return;
        if (!PipBoySafeNow(phase, mq102))
            return;
        const int r = CoSyncProfiles::EnsurePipBoy("the session's Pip-Boy event");
        if (r >= 0 && phase == Phase::Complete)
            s_pipSettled = true;
        if (r == 1)
            LOG_UPDATED("[CoSyncVault111] Pip-Boy fallback applied (phase %s) -- the shared Pip-Boy event had not reached this player",
                CoSyncVault111::PhaseName(phase));
    }

    // ── Vault world state (gear door, elevator) ─────────────────────────────
    void TickVaultWorld(Phase phase, PlayerCharacter* pc, double now)
    {
        TESQuest* mq102 = Quest(kQuestMQ102);
        uint32_t f = s_localFlags & kStPipEvent;
        if (s_reapplied) f |= kStReapplied;
        if (StageDone(mq102, kMQ102GateStage)) f |= kStGate;
        if (StageDone(mq102, kMQ102ExitedStage)) f |= kStExited;
        const uint32_t cell = SafeCellID(reinterpret_cast<TESObjectREFR*>(pc));
        if (cell == kCellVault111Cryo)
        {
            if (SafeIsDisabled(Ref(kRefDescendTrigger))) f |= kStLowered;
            if (TESObjectREFR* m = Ref(kRefRaiseMarker))
                if (!SafeIsDisabled(m)) f |= kStRaised;
        }
        else
            f |= (s_localFlags & (kStLowered | kStRaised));   // keep what was seen inside

        const uint32_t changed = f ^ s_localFlags;
        if (changed & kStGate)
            LOG_UPDATED("[CoSyncVault111] Elevator gate state changed: the gear door (0x%08X) is opening -- MQ102 stage 9 done here", kRefGearDoor);
        if ((changed & kStLowered) && (f & kStLowered))
            LOG_UPDATED("[CoSyncVault111] Elevator state changed: lowered to the bridge on this machine (trigger 0x%08X used)", kRefDescendTrigger);
        if ((changed & kStRaised) && (f & kStRaised))
            LOG_UPDATED("[CoSyncVault111] Elevator state changed: rising with this player (final changes done)");
        if ((changed & kStExited) && (f & kStExited))
            LOG_UPDATED("[CoSyncVault111] left Vault 111 (MQ102 stage 10 here) -- normal CoSync for this player");
        s_localFlags = f;

        if (IsHostRole())
            s_worldGate.store((f & kStGate) != 0);

        // The host's gear door is open and ours is not: apply the shared stage
        // (the vanilla control panel -> gear door script path). QS 9 normally
        // arrives on its own; this covers a lost or ignored one.
        if (!IsHostRole() && s_worldGate.load() && mq102 && Running(mq102) && !(f & kStGate) &&
            InsideVault(phase) && now >= s_nextGateApply)
        {
            s_nextGateApply = now + 15.0;
            CoSyncQuestSync::QueueStageApply(kQuestMQ102, kMQ102GateStage);
            LOG_UPDATED("[CoSyncVault111] Applying Vault state: the host's gear door is open -- MQ102 stage 9 here too");
        }

        // A RespecTriggerREF disabled while this player is still inside and
        // has not ridden: someone else's exit (an older build, or a save made
        // after the host left) -- this player's ride must still exist.
        if (cell == kCellVault111Cryo && phase == Phase::VaultExplore && !s_respecRestored)
        {
            TESObjectREFR* respec = Ref(kRefRespecTrigger);
            if (respec && SafeIsDisabled(respec))
            {
                s_respecRestored = true;
                CoSyncGameAPI::EnableWorldActor(respec);
                LOG_UPDATED("[CoSyncVault111] RespecTriggerREF 0x%08X was disabled by another player's exit -- enabled again for this player's ride",
                    kRefRespecTrigger);
            }
        }
    }

    // ── Network ─────────────────────────────────────────────────────────────
    void SendStatus(Phase phase, double now)
    {
        const int ph = static_cast<int>(phase);
        if (ph == s_sentPhase && s_localFlags == s_sentFlags && now < s_nextStatus)
            return;
        s_sentPhase = ph;
        s_sentFlags = s_localFlags;
        s_nextStatus = now + 5.0;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "V111|st|%u|%d|%u", CoSyncNet::GetMyEntityID(), ph, s_localFlags);
        CoSyncTransport::Send(buf);
    }

    void HostTickLists(double now, bool anyInside)
    {
        // Append-only for the session: host first, then each player as they
        // are first seen. A player who leaves keeps their place (their pod
        // just stays empty), so nobody else's pod changes mid-sequence.
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(s_podMx);
            const uint32_t me = CoSyncNet::GetMyEntityID();
            if (s_podOrder.empty() && me)
            {
                s_podOrder.push_back(me);
                changed = true;
            }
            // Players in the game only (a client still in the lobby has no
            // body to put in a pod -- its pod would just stand empty).
            for (const auto& entry : CoSyncNet::GetRoster())
                if (!entry.isLocal && entry.inGame && entry.entityID &&
                    std::find(s_podOrder.begin(), s_podOrder.end(), entry.entityID) == s_podOrder.end())
                {
                    s_podOrder.push_back(entry.entityID);
                    changed = true;
                }
        }
        if (!anyInside && !changed)
            return;   // nobody in the intro: nothing to tell
        if (changed || now >= s_nextPodsSend)
        {
            s_nextPodsSend = now + 5.0;
            std::string msg = "V111|pods|";
            {
                std::lock_guard<std::mutex> lk(s_podMx);
                for (size_t i = 0; i < s_podOrder.size(); ++i)
                    msg += (i ? "," : "") + std::to_string(s_podOrder[i]);
            }
            CoSyncTransport::Send(msg);
        }
        if (now >= s_nextWorldSend)
        {
            s_nextWorldSend = now + 5.0;
            char buf[48];
            std::snprintf(buf, sizeof(buf), "V111|world|%d|%d", s_worldPip.load() ? 1 : 0, s_worldGate.load() ? 1 : 0);
            CoSyncTransport::Send(buf);
        }
    }

    void DrainDeferred(Phase phase)
    {
        if (InsideVault(phase))
            return;
        std::vector<Deferred> d;
        {
            std::lock_guard<std::mutex> lk(s_deferMx);
            d.swap(s_deferred);
        }
        for (const Deferred& x : d)
        {
            if (x.isRequest)
                CoSyncQuestSync::QueueStageRequest(x.sender, x.quest, x.stage);
            else
                CoSyncQuestSync::QueueStageApply(x.quest, x.stage);
            LOG_UPDATED("[CoSyncVault111] out of the vault: deferred %s quest=0x%08X stage=%d applied now",
                x.isRequest ? "request" : "stage", x.quest, x.stage);
        }
    }

    bool InSet(uint32_t id)
    {
        for (uint32_t r : kPerPlayerExitRefs)
            if (r == id)
                return true;
        return false;
    }
}

namespace CoSyncVault111
{
    Phase GetPhase() { return static_cast<Phase>(s_phase.load()); }

    const char* PhaseName(Phase p)
    {
        switch (p)
        {
        case Phase::PreWar:          return "PreWar (shared intro lockstep)";
        case Phase::VanillaSequence: return "VanillaSequence (vanilla Vault 111, nothing synced into it)";
        case Phase::VaultExplore:    return "VaultExplore (awake in Vault 111)";
        default:                     return "Complete (normal CoSync)";
        }
    }

    StagePolicy ClassifyStage(uint32_t quest, int stage)
    {
        // Per-player scripted events: never cross machines, in any phase.
        if (quest == kQuestMQ101 && stage > kMQ101VaultStage)
            return StagePolicy::Personal;
        if (quest == kQuestMQ102 && stage <= kMQ102ExitedStage && stage != kMQ102DoorsStage && stage != kMQ102GateStage)
            return StagePolicy::Personal;
        const Phase phase = GetPhase();
        if (!InsideVault(phase))
            return StagePolicy::Normal;
        // Inside the vault: this player's tutorial prompts and lines.
        if (quest == kQuestTutorial || quest == kQuestDialogueVault111)
            return StagePolicy::Personal;
        // Someone else's progress after leaving: not here until we leave too.
        if (quest == kQuestMQ102 && stage > kMQ102ExitedStage)
            return StagePolicy::Defer;
        return StagePolicy::Normal;
    }

    bool IsLockstepBlocked(uint32_t quest, int /*stage*/)
    {
        // Fail closed until the first tick has worked the phase out (the
        // caller already requires a running intro).
        return quest == kQuestMQ101 && (!s_phaseKnown.load() || GetPhase() == Phase::PreWar);
    }

    void DeferStage(bool isRequest, uint32_t sender, uint32_t quest, int stage)
    {
        std::lock_guard<std::mutex> lk(s_deferMx);
        for (const Deferred& d : s_deferred)
            if (d.isRequest == isRequest && d.quest == quest && d.stage == stage)
                return;
        if (s_deferred.size() < 64)
            s_deferred.push_back({ isRequest, sender, quest, stage });
    }

    bool IgnoreTrack(uint32_t quest)
    {
        return (quest == kQuestMQ101 || quest == kQuestMQ102) && GetPhase() != Phase::Complete;
    }

    bool IgnoreQuestLifecycle(uint32_t quest)
    {
        return (quest == kQuestMQ101 || quest == kQuestMQ102) && InsideVault(GetPhase());
    }

    bool IgnoreObjective(uint32_t quest)
    {
        return (quest == kQuestMQ101 || quest == kQuestMQ102 || quest == kQuestTutorial || quest == kQuestDialogueVault111) &&
               InsideVault(GetPhase());
    }

    bool IgnoreWorldStateFor(TESObjectREFR* ref)
    {
        if (!ref || GetPhase() == Phase::Complete)
            return false;
        return InSet(ref->formID) || InPodRoom(SafeCellID(ref));
    }

    bool IsIntroLocalActor(TESObjectREFR* ref)
    {
        return SafeCellID(ref) == kCellPrewarVault111;
    }

    bool HostHoldsJoins()
    {
        if (!IsHostRole() || GetPhase() != Phase::VanillaSequence)
        {
            s_holdLogged = false;
            return false;
        }
        if (!s_holdLogged)
        {
            s_holdLogged = true;
            LOG_UPDATED("[CoSyncVault111] joins wait in the lobby: the host is in the vanilla Vault 111 sequence (they come in when the host is awake)");
        }
        return true;
    }

    int PodIndexOf(uint32_t eid)
    {
        return IndexIn(PodOrder(), eid);
    }

    bool RemapProxyIntoPod(uint32_t eid, NiPoint3& pos, NiPoint3& rot)
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc || !SessionLive())
            return false;
        const uint32_t cell = SafeCellID(reinterpret_cast<TESObjectREFR*>(pc));
        if (!InPodRoom(cell))
            return false;
        // Only around the scripted pod: inside it during the freeze, its
        // doorway during the wake-up stumble.
        const float radius = (cell == kCellVault111Cryo) ? kWakeRadius : kInPodRadius;
        const float dx = pos.x - kPods[0].x, dy = pos.y - kPods[0].y;
        if (dx * dx + dy * dy > radius * radius || std::fabs(pos.z - kPods[0].z) > 150.f)
            return false;
        const std::vector<uint32_t> order = PodOrder();
        const int j = IndexIn(order, eid);
        const int k = IndexIn(order, CoSyncNet::GetMyEntityID());
        if (j < 0)
            return false;
        int target = -1;
        if (j == 0)
        {
            // The scripted pod's owner (the host), seen on a client: shown in
            // this client's own pod while this client is in the scripted one.
            if (k <= 0 || s_reapplied)
                return false;
            target = k;
        }
        else
        {
            if (RemoteReapplied(eid))
                return false;   // they stand where they really are
            target = j;
        }
        float x = 0.f, y = 0.f, z = 0.f, rz = 0.f;
        PodSpot(target, x, y, z, rz);
        const float turn = rz - kPods[0].rz;
        const float c = std::cos(turn), s = std::sin(turn);
        pos.x = x + dx * c - dy * s;
        pos.y = y + dx * s + dy * c;
        pos.z = z + (pos.z - kPods[0].z);
        rot.z += turn;
        return true;
    }

    bool IsVaultMessage(const std::string& msg)
    {
        return msg.rfind("V111|", 0) == 0;
    }

    void OnReceive(const std::string& msg, bool isHostRole)
    {
        LOG_REQUIREMENT("CoSyncVault111::OnReceive", "Network side: parses and stores under locks; the tick acts.");
        if (msg.rfind("V111|st|", 0) == 0)
        {
            if (isHostRole)
                CoSyncTransport::Send(msg);   // relay to every client
            uint32_t eid = 0, flags = 0;
            int phase = -1;
            if (std::sscanf(msg.c_str() + 8, "%u|%d|%u", &eid, &phase, &flags) != 3 || eid == 0 ||
                eid == CoSyncNet::GetMyEntityID() || phase < 0 || phase > 3)
                return;
            Remote before;
            {
                std::lock_guard<std::mutex> lk(s_remoteMx);
                Remote& r = s_remote[eid];
                before = r;
                r.phase = phase;
                r.flags = flags;
            }
            if (isHostRole && (flags & kStPipEvent))
                s_clientPipEvent.store(true);
            if (before.phase != phase || before.flags != flags)
                LOG_UPDATED("[CoSyncVault111] entity=%u: %s flags=0x%X%s%s%s%s",
                    eid, PhaseName(static_cast<Phase>(phase)), flags,
                    (flags & kStReapplied) && !(before.flags & kStReapplied) ? " (stood at its pod)" : "",
                    (flags & kStGate) && !(before.flags & kStGate) ? " (gear door open there)" : "",
                    (flags & kStRaised) && !(before.flags & kStRaised) ? " (riding up)" : "",
                    (flags & kStExited) && !(before.flags & kStExited) ? " (left the vault)" : "");
            return;
        }
        if (isHostRole)
            return;   // pods / world only come from the host
        if (msg.rfind("V111|pods|", 0) == 0)
        {
            std::vector<uint32_t> order;
            const char* p = msg.c_str() + 10;
            while (*p && order.size() < 16)
            {
                char* end = nullptr;
                const uint32_t eid = static_cast<uint32_t>(std::strtoul(p, &end, 10));
                if (end == p)
                    break;
                if (eid)
                    order.push_back(eid);
                p = (*end == ',') ? end + 1 : end;
            }
            if (order.empty())
                return;
            std::lock_guard<std::mutex> lk(s_podMx);
            s_podOrder.swap(order);
            return;
        }
        if (msg.rfind("V111|world|", 0) == 0)
        {
            int pip = 0, gate = 0;
            if (std::sscanf(msg.c_str() + 11, "%d|%d", &pip, &gate) != 2)
                return;
            if ((pip != 0) != s_worldPip.load() || (gate != 0) != s_worldGate.load())
                LOG_UPDATED("[CoSyncVault111] Applying Vault state from the host: Pip-Boy event=%d gear door open=%d", pip, gate);
            s_worldPip.store(pip != 0);
            s_worldGate.store(gate != 0);
        }
    }

    void Tick(double /*now*/)
    {
        const double now = Now();
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc || !CoSyncWorld::IsWorldReady())
            return;
        const Phase phase = ComputePhase(pc);
        s_phase.store(static_cast<int>(phase));
        s_phaseKnown.store(true);
        if (!s_phaseLogged || phase != s_lastLoggedPhase)
        {
            if (phase == Phase::VanillaSequence && s_lastLoggedPhase != Phase::VanillaSequence)
                LOG_UPDATED("[CoSyncVault111] Intro detected -- Vanilla Cryopod sequence started (MQ101 stage %d): CoSync stays out of it",
                    CoSyncQuestSync::GetLocalStage(kQuestMQ101));
            LOG_UPDATED("[CoSyncVault111] phase: %s", PhaseName(phase));
            s_lastLoggedPhase = phase;
            s_phaseLogged = true;
        }
        if (!SessionLive())
            return;

        if (phase == Phase::PreWar || phase == Phase::VanillaSequence)
        {
            s_reapplied = false;        // (a New Game started in this session)
            s_respecRestored = false;
            s_pipSettled = false;
        }
        else if (phase == Phase::Complete)
            s_reapplied = true;         // not in the intro, or this player is outside: nothing to reapply
        else if (!s_reapplied && StageDone(Quest(kQuestMQ102), kMQ102AwakeStage) && !AnyBlockingMenu())
            Reapply(pc);

        const uint32_t cell = SafeCellID(reinterpret_cast<TESObjectREFR*>(pc));
        if (phase == Phase::VanillaSequence && InPodRoom(cell))
        {
            LogAssignment(cell);
            if (now >= s_nextResidentScan)
            {
                s_nextResidentScan = now + 1.0;
                VacateAssignedPods(pc);
            }
        }

        TickVaultWorld(phase, pc, now);
        TickPipBoy(phase, now);
        DrainDeferred(phase);

        bool anyInside = InsideVault(phase);
        {
            std::lock_guard<std::mutex> lk(s_remoteMx);
            for (const auto& kv : s_remote)
                anyInside = anyInside || kv.second.phase == static_cast<int>(Phase::VanillaSequence) ||
                                         kv.second.phase == static_cast<int>(Phase::VaultExplore);
        }
        if (IsHostRole())
            HostTickLists(now, anyInside);
        if (anyInside || phase != Phase::Complete || s_sentPhase != static_cast<int>(phase))
            SendStatus(phase, now);
    }

    void Reset()
    {
        s_phase.store(static_cast<int>(Phase::Complete));
        s_phaseKnown.store(false);
        s_lastLoggedPhase = Phase::Complete;
        s_phaseLogged = false;
        s_reapplied = false;
        s_localFlags = 0;
        s_sentFlags = 0xFFFFFFFFu;
        s_sentPhase = -1;
        s_nextStatus = s_nextWorldSend = s_nextPodsSend = s_nextPipCheck = 0.0;
        s_nextResidentScan = s_nextGateApply = 0.0;
        s_holdLogged = false;
        s_respecRestored = false;
        s_pipSettled = false;
        s_vacatedResidents.clear();
        s_lastAssignmentLog.clear();
        {
            std::lock_guard<std::mutex> lk(s_podMx);
            s_podOrder.clear();
        }
        {
            std::lock_guard<std::mutex> lk(s_remoteMx);
            s_remote.clear();
        }
        s_worldPip.store(false);
        s_worldGate.store(false);
        s_clientPipEvent.store(false);
        {
            std::lock_guard<std::mutex> lk(s_deferMx);
            s_deferred.clear();
        }
    }
}
