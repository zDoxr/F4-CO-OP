#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncItemDropSync.h"

#include "CoSyncRequirementsLog.h"
#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncGameAPI.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <mutex>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <cstring>

extern RelocAddr<_LookupFormByID> LookupFormByID;
extern PlayerCharacter* CoSyncGetPlayer();

namespace
{
    // Same lootable set as CoSyncWorldSync::IsLootableBaseType (MISC/WEAP/
    // AMMO/KEYM/ALCH) — kept as a local copy rather than exporting that
    // static helper, to avoid touching an unrelated file's interface.
    bool IsLootableBaseType(uint8_t ft)
    {
        return ft == 35 || ft == 43 || ft == 44 || ft == 47 || ft == 48;
    }

    // Detection radius for "the local player just dropped this" — deliberately
    // tight (interaction range, not room-scale) since this is a heuristic, not
    // a real drop event: anything dynamic+lootable+untracked this close to the
    // player is assumed to be a fresh drop.
    constexpr float kDropDetectRadius   = 220.0f;
    constexpr float kDropDetectRadiusSq = kDropDetectRadius * kDropDetectRadius;
    constexpr double kScanInterval      = 1.0;

    double s_lastScanTime = -999.0;
    uint32_t s_nextEntityID = CoSyncItemDropSync::kDropEntityBase;

    // Reporter-side: refs we've broadcast a DROPSEEN for but haven't yet
    // gotten a DROPCREATE confirmation for (avoid re-reporting every tick).
    struct ReportedRef { double reportTime; uint32_t baseFormID; };
    std::unordered_map<uint32_t, ReportedRef> s_reportedRefs; // localRefFormID -> {time, base}

    // Host-side: dedup by (reporterEid, localRefFormID) so the same drop
    // isn't allocated twice if DROPSEEN keeps arriving before the reporter
    // sees its own DROPCREATE confirmation.
    std::unordered_map<uint64_t, uint32_t> s_hostAllocated; // key -> entityID

    // This machine's own tracking: entity <-> local ref (either my real
    // original if I'm the owner, or a clone I spawned for someone else's).
    std::unordered_map<uint32_t, uint32_t> s_entityToLocalRef;
    std::unordered_map<uint32_t, uint32_t> s_localRefToEntity;
    std::unordered_set<uint32_t>           s_ownedEntities; // entities where the local ref is MY real original

    // Reporter-side pending match: baseFormID -> oldest unconfirmed local ref
    // waiting to be matched against my own DROPCREATE confirmation (FIFO by
    // formID; drops of the same item type within the same second may match
    // out of order, which is harmless since they're identical items).
    std::unordered_map<uint32_t, std::vector<uint32_t>> s_pendingByBaseForm;

    uint64_t HostDedupKey(uint32_t reporterEid, uint32_t localRefFormID)
    {
        return (static_cast<uint64_t>(reporterEid) << 32) | localRefFormID;
    }

    struct SpawnOp { uint32_t entityID; uint32_t baseFormID; float x, y, z; };
    struct DespawnOp { uint32_t entityID; };

    std::mutex               s_opMutex;
    std::vector<SpawnOp>     s_pendingSpawns;
    std::vector<DespawnOp>   s_pendingDespawns;
}

namespace CoSyncItemDropSync
{

// Applies an allocated drop entity: if I'm the owner, matches it against my
// own pending report; otherwise queues a clone spawn for the game thread.
// Called both from the network-receive path (DROPCREATE arriving from the
// host) AND directly, in-process, by HostAllocateDrop below — CoSyncTransport
// ::Send() has no local loopback, so the host would never "receive" its own
// broadcast otherwise (same reasoning as NpcWorldSync's Stage-2 host path
// calling ClientBindWorldActor directly instead of waiting on its own CREATE).
static void ApplyDropCreate(uint32_t entityID, uint32_t baseFormID, const NiPoint3& pos, uint32_t ownerEid)
{
    if (entityID == 0 || baseFormID == 0)
        return;
    if (s_entityToLocalRef.count(entityID))
        return; // already handled

    const uint32_t myEid = CoSyncNet::GetMyEntityID();
    if (ownerEid != 0 && ownerEid == myEid)
    {
        // My own report confirmed — match against the oldest unconfirmed
        // local ref I reported for this base form.
        auto it = s_pendingByBaseForm.find(baseFormID);
        if (it == s_pendingByBaseForm.end() || it->second.empty())
        {
            LOG_WARN("[ItemDropSync] OWNER-CONFIRM entity=%u base=0x%08X — no matching pending report found",
                entityID, baseFormID);
            return;
        }
        const uint32_t localRef = it->second.front();
        it->second.erase(it->second.begin());
        if (it->second.empty())
            s_pendingByBaseForm.erase(it);

        s_reportedRefs.erase(localRef);
        s_entityToLocalRef[entityID] = localRef;
        s_localRefToEntity[localRef] = entityID;
        s_ownedEntities.insert(entityID);

        LOG_UPDATED("[ItemDropSync] OWNER-CONFIRM entity=%u base=0x%08X -> my ref=0x%08X",
            entityID, baseFormID, localRef);
        return;
    }

    // Someone else's drop — queue a clone spawn for the game thread.
    std::lock_guard<std::mutex> lk(s_opMutex);
    s_pendingSpawns.push_back({ entityID, baseFormID, pos.x, pos.y, pos.z });
}

// Host-only: allocates an entity ID for a reported drop, broadcasts the
// confirmation, and applies it in-process immediately (see ApplyDropCreate's
// comment on why the host can't just wait to "receive" its own broadcast).
static void HostAllocateDrop(uint32_t baseFormID, uint32_t refFormID, const NiPoint3& pos, uint32_t reporterEid)
{
    if (baseFormID == 0 || refFormID == 0 || reporterEid == 0)
        return;

    const uint64_t key = HostDedupKey(reporterEid, refFormID);
    if (s_hostAllocated.count(key))
        return; // already allocated for this exact (reporter, ref) pair

    const uint32_t eid = s_nextEntityID;
    ++s_nextEntityID;
    if (s_nextEntityID > kDropEntityMax)
        s_nextEntityID = kDropEntityBase;

    s_hostAllocated[key] = eid;

    char buf[192];
    snprintf(buf, sizeof(buf), "DROPCREATE|%u|%u|%.2f,%.2f,%.2f|%u",
        eid, baseFormID, pos.x, pos.y, pos.z, reporterEid);
    CoSyncTransport::Send(std::string(buf));

    LOG_UPDATED("[ItemDropSync] HOST allocated entity=%u base=0x%08X owner=%u pos=(%.1f,%.1f,%.1f)",
        eid, baseFormID, reporterEid, pos.x, pos.y, pos.z);

    ApplyDropCreate(eid, baseFormID, pos, reporterEid);
}

__declspec(noinline) static void ScanForLocalDrops_Impl(double now)
{
    LOG_REQUIREMENT("CoSyncItemDropSync::ScanForLocalDrops_Impl", "Needs a valid player who's currently in a cell. Wrapped in a crash guard by the caller.");
    PlayerCharacter* localPlayer = CoSyncGetPlayer();
    if (!localPlayer)
        return;

    TESObjectCELL* cell = localPlayer->parentCell;
    if (!cell)
        return;

    const float px = localPlayer->pos.x, py = localPlayer->pos.y, pz = localPlayer->pos.z;

    for (uint32_t i = 0; i < static_cast<uint32_t>(cell->objectList.count); ++i)
    {
        TESObjectREFR* refr = cell->objectList[i];
        if (!refr || !refr->baseForm)
            continue;
        if ((refr->formID & 0xFF000000u) != 0xFF000000u)
            continue; // only dynamic refs can be fresh drops
        if (refr->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled))
            continue;
        if (!IsLootableBaseType(refr->baseForm->formType))
            continue;
        if (s_localRefToEntity.count(refr->formID))
            continue; // already tracked (mine or a clone I spawned)
        if (s_reportedRefs.count(refr->formID))
            continue; // already reported, awaiting confirmation

        const float dx = refr->pos.x - px, dy = refr->pos.y - py, dz = refr->pos.z - pz;
        if (dx * dx + dy * dy + dz * dz > kDropDetectRadiusSq)
            continue;

        const uint32_t myEid = CoSyncNet::GetMyEntityID();
        if (myEid == 0)
            continue;

        s_reportedRefs[refr->formID] = { now, refr->baseForm->formID };
        s_pendingByBaseForm[refr->baseForm->formID].push_back(refr->formID);

        const NiPoint3 dropPos{ refr->pos.x, refr->pos.y, refr->pos.z };
        if (CoSyncNet::IsHost())
        {
            // No network round-trip needed — allocate directly.
            HostAllocateDrop(refr->baseForm->formID, refr->formID, dropPos, myEid);
            LOG_UPDATED("[ItemDropSync] LOCAL (host) drop detected base=0x%08X ref=0x%08X pos=(%.1f,%.1f,%.1f)",
                refr->baseForm->formID, refr->formID, refr->pos.x, refr->pos.y, refr->pos.z);
        }
        else
        {
            char buf[160];
            snprintf(buf, sizeof(buf), "DROPSEEN|%u|%u|%.2f,%.2f,%.2f|%u",
                refr->baseForm->formID, refr->formID, refr->pos.x, refr->pos.y, refr->pos.z, myEid);
            CoSyncTransport::Send(std::string(buf));
            LOG_UPDATED("[ItemDropSync] TX DROPSEEN base=0x%08X ref=0x%08X pos=(%.1f,%.1f,%.1f)",
                refr->baseForm->formID, refr->formID, refr->pos.x, refr->pos.y, refr->pos.z);
        }
    }

    // Prune stale unconfirmed reports (dropped packet, or item was picked
    // back up before confirmation arrived) so they can be re-detected, and
    // remove the matching entry from the pending-match table so a later,
    // unrelated DROPCREATE for the same base form can't mis-match it.
    for (auto it = s_reportedRefs.begin(); it != s_reportedRefs.end(); )
    {
        if (now - it->second.reportTime > 10.0)
        {
            auto pendIt = s_pendingByBaseForm.find(it->second.baseFormID);
            if (pendIt != s_pendingByBaseForm.end())
            {
                auto& vec = pendIt->second;
                vec.erase(std::remove(vec.begin(), vec.end(), it->first), vec.end());
                if (vec.empty())
                    s_pendingByBaseForm.erase(pendIt);
            }
            it = s_reportedRefs.erase(it);
        }
        else
            ++it;
    }
}

__declspec(noinline) static void ScanForLocalDropsSafe(double now)
{
    __try
    {
        ScanForLocalDrops_Impl(now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[ItemDropSync] Crashed while scanning for dropped items -- skipping this pass");
    }
}

// noinline, SEH-only, no non-trivial C++ objects in scope (C2712 avoidance).
__declspec(noinline) static TESForm* SafeLookupForm(uint32_t formID)
{
    __try
    {
        return LookupFormByID(formID);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// Non-trivial dtors (VMArray, VMVariable) live only in this function — no
// __try here, matching CoSyncContainerSync.cpp's established _Impl/Safe
// split (C2712 avoidance: __try cannot share a function with objects that
// require unwinding).
__declspec(noinline) static void ProcessSpawnOps_Impl(const std::vector<SpawnOp>& spawns)
{
    for (const SpawnOp& op : spawns)
    {
        if (s_entityToLocalRef.count(op.entityID))
            continue; // already handled (e.g. duplicate DROPCREATE re-send)

        TESForm* base = SafeLookupForm(op.baseFormID);
        if (!base)
        {
            LOG_WARN("[ItemDropSync] SPAWN entity=%u base=0x%08X not found on this client", op.entityID, op.baseFormID);
            continue;
        }

        NiPoint3 pos{ op.x, op.y, op.z };
        TESObjectREFR* clone = CoSyncGameAPI::SpawnItemAtPosition(base, pos);
        if (!clone)
        {
            LOG_WARN("[ItemDropSync] SPAWN entity=%u base=0x%08X failed", op.entityID, op.baseFormID);
            continue;
        }

        s_entityToLocalRef[op.entityID] = clone->formID;
        s_localRefToEntity[clone->formID] = op.entityID;
        LOG_UPDATED("[ItemDropSync] SPAWN clone entity=%u base=0x%08X ref=0x%08X",
            op.entityID, op.baseFormID, clone->formID);
    }
}

__declspec(noinline) static void ProcessDespawnOps_Impl(const std::vector<DespawnOp>& despawns)
{
    static const BSFixedString kDisable("Disable");
    static const BSFixedString kDelete("Delete");

    for (const DespawnOp& op : despawns)
    {
        auto it = s_entityToLocalRef.find(op.entityID);
        if (it == s_entityToLocalRef.end())
            continue;

        const uint32_t localRef = it->second;
        TESForm* form = SafeLookupForm(localRef);

        if (form && (form->formType >= kFormType_REFR && form->formType <= kFormType_PHZD))
        {
            TESObjectREFR* refr = static_cast<TESObjectREFR*>(form);

            VMArray<VMVariable> dArgs;
            bool fadeOut = false;
            VMVariable vFade; vFade.Set(&fadeOut); dArgs.Push(&vFade);
            CallFunctionNoWait(refr, kDisable, dArgs);

            VMArray<VMVariable> delArgs;
            CallFunctionNoWait(refr, kDelete, delArgs);

            LOG_UPDATED("[ItemDropSync] DESPAWN entity=%u ref=0x%08X (Disable+Delete queued)", op.entityID, localRef);
        }

        s_localRefToEntity.erase(localRef);
        s_entityToLocalRef.erase(it);
        s_ownedEntities.erase(op.entityID);
    }
}

void Tick(double now)
{
    LOG_REQUIREMENT("CoSyncItemDropSync::Tick", "Safe to call every tick. All the risky work happens in guarded helper functions.");
    if (!CoSyncNet::IsConnected())
        return;

    if ((now - s_lastScanTime) >= kScanInterval)
    {
        s_lastScanTime = now;
        ScanForLocalDropsSafe(now);
    }

    std::vector<SpawnOp> spawns;
    std::vector<DespawnOp> despawns;
    {
        std::lock_guard<std::mutex> lk(s_opMutex);
        spawns.swap(s_pendingSpawns);
        despawns.swap(s_pendingDespawns);
    }

    if (!spawns.empty())
        ProcessSpawnOps_Impl(spawns);
    if (!despawns.empty())
        ProcessDespawnOps_Impl(despawns);
}

bool IsDropSeen(const std::string& msg)   { return msg.rfind("DROPSEEN|", 0) == 0; }
bool IsDropCreate(const std::string& msg) { return msg.rfind("DROPCREATE|", 0) == 0; }
bool IsDropGone(const std::string& msg)   { return msg.rfind("DROPGONE|", 0) == 0; }

void OnReceiveDropSeen(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncItemDropSync::OnReceiveDropSeen", "Host-only. Needs a properly formatted DROPSEEN message.");
    if (!CoSyncNet::IsHost())
        return;

    const char* s = msg.c_str() + 9; // skip "DROPSEEN|"
    char* e = nullptr;
    const uint32_t baseFormID = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return; s = e + 1;
    const uint32_t refFormID  = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return; s = e + 1;
    NiPoint3 pos{};
    if (std::sscanf(s, "%f,%f,%f", &pos.x, &pos.y, &pos.z) != 3) return;
    const char* bar = std::strchr(s, '|');
    if (!bar) return;
    const uint32_t reporterEid = static_cast<uint32_t>(std::strtoul(bar + 1, nullptr, 10));

    HostAllocateDrop(baseFormID, refFormID, pos, reporterEid);
}

void OnReceiveDropCreate(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncItemDropSync::OnReceiveDropCreate", "Needs a properly formatted DROPCREATE message.");
    const char* s = msg.c_str() + 11; // skip "DROPCREATE|"
    char* e = nullptr;
    const uint32_t entityID   = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return; s = e + 1;
    const uint32_t baseFormID = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return; s = e + 1;
    NiPoint3 pos{};
    if (std::sscanf(s, "%f,%f,%f", &pos.x, &pos.y, &pos.z) != 3) return;
    const char* bar = std::strchr(s, '|');
    if (!bar) return;
    const uint32_t ownerEid = static_cast<uint32_t>(std::strtoul(bar + 1, nullptr, 10));

    // Host already applied this in-process from HostAllocateDrop — this
    // receive path is for every OTHER peer's copy of the broadcast (and is
    // idempotent/no-op via the s_entityToLocalRef guard even if it somehow
    // reaches the host too).
    ApplyDropCreate(entityID, baseFormID, pos, ownerEid);
}

void OnReceiveDropGone(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncItemDropSync::OnReceiveDropGone", "Needs a properly formatted DROPGONE message.");
    const char* s = msg.c_str() + 9; // skip "DROPGONE|"
    char* e = nullptr;
    const uint32_t entityID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (e == s || entityID == 0)
        return;

    std::lock_guard<std::mutex> lk(s_opMutex);
    s_pendingDespawns.push_back({ entityID });
}

void NotifyLocalPickup(uint32_t refFormID)
{
    LOG_REQUIREMENT("CoSyncItemDropSync::NotifyLocalPickup", "Doesn't touch the game. Does nothing if the ref isn't a drop we're tracking.");
    auto it = s_localRefToEntity.find(refFormID);
    if (it == s_localRefToEntity.end())
        return;

    const uint32_t entityID = it->second;

    // Local cleanup now (the ref is already gone/going as far as the engine
    // is concerned — it was just picked up) so a stray late DROPGONE echo
    // for the same entity doesn't double-despawn.
    s_localRefToEntity.erase(it);
    s_entityToLocalRef.erase(entityID);
    s_ownedEntities.erase(entityID);
    s_reportedRefs.erase(refFormID);

    if (!CoSyncNet::IsConnected())
        return;

    std::string pkt = "DROPGONE|" + std::to_string(entityID);
    CoSyncTransport::Send(pkt);

    LOG_UPDATED("[ItemDropSync] TX DROPGONE entity=%u (local pickup of ref=0x%08X)", entityID, refFormID);
}

void Reset()
{
    s_reportedRefs.clear();
    s_hostAllocated.clear();
    s_entityToLocalRef.clear();
    s_localRefToEntity.clear();
    s_ownedEntities.clear();
    s_pendingByBaseForm.clear();
    s_nextEntityID = kDropEntityBase;
    s_lastScanTime = -999.0;
    {
        std::lock_guard<std::mutex> lk(s_opMutex);
        s_pendingSpawns.clear();
        s_pendingDespawns.clear();
    }
    LOG_INFO("[ItemDropSync] Reset — maps cleared for new session");
}

}
