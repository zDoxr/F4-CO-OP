#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncContainerSync.h"
#include "CoSyncVMFormArg.h"
#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "GameReferences.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"

#include <mutex>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstdlib>

extern PlayerCharacter* CoSyncGetPlayer();

namespace
{
    static TESObjectREFR* FormToREFR_Ctr(TESForm* form)
    {
        if (!form) return nullptr;
        const auto ft = form->formType;
        return (ft >= kFormType_REFR && ft <= kFormType_PHZD)
            ? static_cast<TESObjectREFR*>(form)
            : nullptr;
    }

    // -------------------------------------------------------------------------
    // Game-thread state — all reads/writes on the game main thread; no mutex.
    // -------------------------------------------------------------------------
    static uint32_t s_nearestContainerFormID = 0;
    static double   s_lastContainerScan      = -1.0;

    static constexpr double kContainerScanInterval = 0.10;      // 10x/sec
    static constexpr float  kContainerMaxDistSq    = 300.f * 300.f;  // ~300 game units

    // -------------------------------------------------------------------------
    // Network-thread → game-thread pending queue.
    // -------------------------------------------------------------------------
    struct PendingContainerOp
    {
        uint32_t containerFormID;
        uint32_t baseFormID;
        uint32_t count;
        // false = peer took items OUT (CTR|)  -> RemoveItem here
        // true  = peer put items IN  (CTRD|)  -> AddItem here
        bool     isDeposit = false;
    };

    static std::mutex                      s_ctrMutex;
    static std::vector<PendingContainerOp> s_pendingOps;
}

// ============================================================================
// Nearest-container scan (game thread)
// ============================================================================

__declspec(noinline) static void UpdateNearestContainer_Impl(double /*now*/)
{
    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc) { s_nearestContainerFormID = 0; return; }

    TESObjectREFR* pcRefr = reinterpret_cast<TESObjectREFR*>(pc);
    TESObjectCELL* cell   = pcRefr->parentCell;
    if (!cell) { s_nearestContainerFormID = 0; return; }

    const float px = pcRefr->pos.x;
    const float py = pcRefr->pos.y;
    const float pz = pcRefr->pos.z;

    uint32_t bestFormID = 0;
    float    bestDistSq = kContainerMaxDistSq;

    for (UInt32 i = 0; i < cell->objectList.count; ++i)
    {
        TESObjectREFR* ref = cell->objectList.entries[i];
        if (!ref || !ref->baseForm) continue;
        if (ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)) continue;
        if ((ref->formID & 0xFF000000) == 0xFF000000) continue;
        if (ref->baseForm->formType != kFormType_CONT) continue;

        const float dx = ref->pos.x - px;
        const float dy = ref->pos.y - py;
        const float dz = ref->pos.z - pz;
        const float dSq = dx*dx + dy*dy + dz*dz;
        if (dSq < bestDistSq)
        {
            bestDistSq = dSq;
            bestFormID = ref->formID;
        }
    }

    s_nearestContainerFormID = bestFormID;
}

static void UpdateNearestContainer_Safe(double now)
{
    __try { UpdateNearestContainer_Impl(now); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        s_nearestContainerFormID = 0;
        LOG_WARN("[ContainerSync] UpdateNearestContainer faulted -- caught");
    }
}

void CoSyncContainerSync::UpdateNearestContainer(double now)
{
    if ((now - s_lastContainerScan) < kContainerScanInterval)
        return;
    s_lastContainerScan = now;
    UpdateNearestContainer_Safe(now);
}

// ============================================================================
// ============================================================================
// Deposit detection (item #8) — 2026-08-04
//
// OnContainerWithdrawal below only ever covered items coming OUT of a
// container (it hangs off the InvAddFromWorld detour, i.e. "something was
// added to the PLAYER"). Putting items IN was never synced, so a shared
// stash only ever agreed in one direction.
//
// Detected by snapshot/diff rather than by hooking an engine add-item path:
// the container's contents are sampled while the player is near it, and any
// item whose count GREW is a deposit. This reuses the exact BGSInventoryList
// traversal CoSyncInventorySync already uses (inventoryList is a
// TESObjectREFR field, so it works for containers as well as actors), and it
// needs no new RVA — which matters, because the last two engine hooks in this
// area (WorkshopScrapHook, and the original PickupHook RVA) both turned out to
// point at the wrong function.
//
// A count DECREASE is deliberately ignored here: that is a withdrawal, which
// the existing CTR| path already reports, and double-reporting it would make
// the two peers fight over the same stack.
// ============================================================================

namespace
{
    constexpr double kDepositPollInterval = 0.5;
    double   s_lastDepositPoll = 0.0;
    uint32_t s_snapshotContainer = 0;                     // which container the snapshot is of
    std::unordered_map<uint32_t, int32_t> s_containerSnapshot;
    uint32_t s_depositsSent = 0;

    // Mirrors CoSyncInventorySync::SnapshotInventory_Impl. noinline + SEH at the
    // caller because this walks engine-owned arrays and a vtable-style Visit.
    __declspec(noinline) void SnapshotContainer_Impl(TESObjectREFR* ctr,
                                                     std::unordered_map<uint32_t,int32_t>& out)
    {
        LOG_REQUIREMENT("CoSyncContainerSync::SnapshotContainer_Impl", "Needs a valid container. Walks the container's item list directly without its own crash guard, so whoever calls this must wrap it in one.");
        BGSInventoryList* invList = ctr->inventoryList;
        if (!invList) return;

        for (UInt32 i = 0; i < invList->items.count; ++i)
        {
            const BGSInventoryItem& item = invList->items[i];
            if (!item.form) continue;

            int32_t total = 0;
            if (item.stack)
            {
                item.stack->Visit([&total](BGSInventoryItem::Stack* s) {
                    total += s->count;
                    return true;
                });
            }
            if (total > 0)
                out[item.form->formID] += total;
        }
    }

    void SnapshotContainerSafe(TESObjectREFR* ctr, std::unordered_map<uint32_t,int32_t>& out)
    {
        __try { SnapshotContainer_Impl(ctr, out); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_WARN("[ContainerSync] SnapshotContainer faulted — recovered");
        }
    }
}

// Folds a remotely-applied change into our own snapshot so the next diff does
// NOT re-report it as a local deposit.
//
// Without this the two peers ping-pong forever: host deposits -> client applies
// AddItem -> client's snapshot diff sees the count grow -> client reports it as
// its own deposit -> host applies -> host's diff sees growth -> ... The first
// live test caught exactly that (one host deposit produced 3 AddItem applies
// across the two machines in ~17s).
static void NoteRemoteContainerChange(uint32_t ctrFID, uint32_t itemFID, int32_t delta)
{
    if (ctrFID != s_snapshotContainer) return;   // not the container we baselined
    int32_t& v = s_containerSnapshot[itemFID];
    v += delta;
    if (v < 0) v = 0;
}

// Shared enqueue used by both directions. Declared before first use.
static void QueueContainerOp(uint32_t ctrFID, uint32_t itemFID, int32_t count, bool isDeposit)
{
    std::lock_guard<std::mutex> lk(s_ctrMutex);
    s_pendingOps.push_back({ ctrFID, itemFID, static_cast<uint32_t>(count), isDeposit });
}

void CoSyncContainerSync::TickDeposits(double now)
{
    LOG_REQUIREMENT("CoSyncContainerSync::TickDeposits", "Needs an active connection and a nearby container already found. Looks the container back up every check since it can become stale after a cell change.");
    if (!CoSyncNet::IsConnected()) return;
    if ((now - s_lastDepositPoll) < kDepositPollInterval) return;
    s_lastDepositPoll = now;

    const uint32_t ctrFID = s_nearestContainerFormID;
    if (ctrFID == 0)
    {
        // Walked away — drop the snapshot so re-approaching re-baselines
        // instead of diffing against a stale picture and reporting phantom
        // deposits for everything someone else added in the meantime.
        if (!s_containerSnapshot.empty())
        {
            s_containerSnapshot.clear();
            s_snapshotContainer = 0;
        }
        return;
    }

    TESObjectREFR* ctr = FormToREFR_Ctr(LookupFormByID(ctrFID));
    if (!ctr) return;

    std::unordered_map<uint32_t,int32_t> current;
    SnapshotContainerSafe(ctr, current);

    // First look at this container (or we just switched containers): take a
    // baseline only. Everything already inside is NOT a deposit.
    if (s_snapshotContainer != ctrFID)
    {
        s_snapshotContainer = ctrFID;
        s_containerSnapshot.swap(current);
        LOG_INFO("[ContainerSync] baseline for container=0x%08X (%u distinct items)",
            ctrFID, (unsigned)s_containerSnapshot.size());
        return;
    }

    for (const auto& kv : current)
    {
        const uint32_t fid = kv.first;
        const int32_t  nowCount = kv.second;
        auto prev = s_containerSnapshot.find(fid);
        const int32_t oldCount = (prev == s_containerSnapshot.end()) ? 0 : prev->second;
        const int32_t delta = nowCount - oldCount;
        if (delta <= 0) continue;   // withdrawals handled by the CTR| path

        std::string pkt = "CTRD|";
        pkt += std::to_string(ctrFID); pkt += "|";
        pkt += std::to_string(fid);    pkt += "|";
        pkt += std::to_string(delta);
        CoSyncTransport::Send(pkt);
        s_depositsSent++;
        LOG_INFO("[ContainerSync] TX deposit container=0x%08X item=0x%08X +%d (total sent=%u)",
            ctrFID, fid, delta, s_depositsSent);
    }

    s_containerSnapshot.swap(current);
}

void CoSyncContainerSync::OnReceiveContainerDeposit(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncContainerSync::OnReceiveContainerDeposit", "Parses a deposit message from the other player. All the fields are checked here, and the actual work is queued up so nothing touches the game engine off the main thread.");
    const char* s = msg.c_str() + 5;
    char* e = nullptr;
    uint32_t ctrFID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (!e || *e != '|' || ctrFID == 0) return;
    s = e + 1;
    uint32_t itemFID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (!e || *e != '|' || itemFID == 0) return;
    s = e + 1;
    int32_t count = static_cast<int32_t>(std::strtol(s, &e, 10));
    if (count <= 0) return;

    LOG_INFO("[ContainerSync] RX deposit container=0x%08X item=0x%08X +%d",
        ctrFID, itemFID, count);

    QueueContainerOp(ctrFID, itemFID, count, /*isDeposit=*/true);
}

// ============================================================================
// Hook callback — called from InvAddFromWorld detour (game thread)
// ============================================================================

void CoSyncContainerSync::OnContainerWithdrawal(uint32_t baseFormID, uint32_t count)
{
    if (!CoSyncNet::IsConnected()) return;
    if (baseFormID == 0) return;

    const uint32_t ctrFID = s_nearestContainerFormID;
    if (ctrFID == 0)
    {
        LOG_WARN("[ContainerSync] TX: container withdrawal detected but no nearby static container cached -- skipping");
        return;
    }

    std::string pkt = "CTR|";
    pkt += std::to_string(ctrFID);
    pkt += "|";
    pkt += std::to_string(baseFormID);
    pkt += "|";
    pkt += std::to_string(count);
    CoSyncTransport::Send(pkt);
    LOG_UPDATED("[ContainerSync] TX: container=0x%08X item=0x%08X count=%u", ctrFID, baseFormID, count);
}

// ============================================================================
// Packet routing (network thread)
// ============================================================================

bool CoSyncContainerSync::IsContainerDeposit(const std::string& msg)
{
    return msg.rfind("CTRD|", 0) == 0;
}

bool CoSyncContainerSync::IsContainerSync(const std::string& msg)
{
    return msg.rfind("CTR|", 0) == 0;
}

void CoSyncContainerSync::OnReceiveContainerSync(const std::string& msg)
{
    if (CoSyncNet::IsHost())
        CoSyncTransport::Send(msg);

    const char* s = msg.c_str() + 4;  // skip "CTR|"
    char* e = nullptr;

    const uint32_t ctrFID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (!e || *e != '|' || ctrFID == 0)
    {
        LOG_WARN("[ContainerSync] Malformed CTR packet (containerFID)");
        return;
    }
    s = e + 1;

    const uint32_t baseFID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (!e || *e != '|' || baseFID == 0)
    {
        LOG_WARN("[ContainerSync] Malformed CTR packet (baseFID)");
        return;
    }
    s = e + 1;

    const uint32_t cnt = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (e == s)
    {
        LOG_WARN("[ContainerSync] Malformed CTR packet (count)");
        return;
    }

    LOG_UPDATED("[ContainerSync] RX queued: container=0x%08X item=0x%08X count=%u", ctrFID, baseFID, cnt);
    std::lock_guard<std::mutex> lk(s_ctrMutex);
    s_pendingOps.push_back({ ctrFID, baseFID, cnt, false });
}

// ============================================================================
// Apply on game thread
// ============================================================================

// Non-trivial dtors (VMArray, VMVariable) live only in this function.
// No __try here — __try is in the Safe wrapper below (C2712 avoidance).
__declspec(noinline) static void FlushPendingContainerOps_Impl(
    const std::vector<PendingContainerOp>& ops)
{
    static const BSFixedString kRemoveItem("RemoveItem");
    for (const auto& op : ops)
    {
        TESForm* ctrForm = LookupFormByID(op.containerFormID);
        if (!ctrForm)
        {
            LOG_WARN("[ContainerSync] container formID=0x%08X not found", op.containerFormID);
            continue;
        }
        TESObjectREFR* ctrRef = FormToREFR_Ctr(ctrForm);
        if (!ctrRef)
        {
            LOG_WARN("[ContainerSync] container formID=0x%08X is not a REFR", op.containerFormID);
            continue;
        }

        TESForm* itemBase = LookupFormByID(op.baseFormID);
        if (!itemBase)
        {
            LOG_WARN("[ContainerSync] item baseFormID=0x%08X not found", op.baseFormID);
            continue;
        }

        // Deposit -> AddItem, withdrawal -> RemoveItem. Both take the same
        // (form, count, silent[, destination]) shape; AddItem has no 4th arg.
        static const BSFixedString kAddItem("AddItem");
        VMArray<VMVariable> args;
        {
            VMVariable v; CoSyncVM::SetFormArg(v, itemBase);
            args.Push(&v);
        }
        {
            SInt32     n = static_cast<SInt32>(op.count);
            VMVariable v; v.Set(&n);
            args.Push(&v);
        }
        {
            bool       b = true;
            VMVariable v; v.Set(&b);
            args.Push(&v);
        }
        if (!op.isDeposit)
        {
            // RemoveItem's 4th arg is the destination container; None = drop.
            VMVariable v; CoSyncVM::SetFormArg(v, nullptr);
            args.Push(&v);
        }

        // Pre-register the change against our snapshot BEFORE the VM call, so
        // even if the next TickDeposits poll lands before the queued Papyrus
        // call has run, the diff is already accounted for.
        NoteRemoteContainerChange(op.containerFormID, op.baseFormID,
                                  op.isDeposit ? static_cast<int32_t>(op.count)
                                               : -static_cast<int32_t>(op.count));

        CallFunctionNoWait(ctrRef, op.isDeposit ? kAddItem : kRemoveItem, args);
        LOG_UPDATED("[ContainerSync] applied %s: container=0x%08X item=0x%08X count=%u",
            op.isDeposit ? "AddItem" : "RemoveItem",
            op.containerFormID, op.baseFormID, op.count);
    }
}

// __try/__except here; const ref parameter has no dtor in this scope (C2712-safe).
__declspec(noinline) static void FlushPendingContainerOpsSafe(
    const std::vector<PendingContainerOp>& ops)
{
    __try { FlushPendingContainerOps_Impl(ops); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[ContainerSync] FlushPendingContainerOps faulted -- caught");
    }
}

void CoSyncContainerSync::FlushPendingContainerOps()
{
    std::vector<PendingContainerOp> ops;
    {
        std::lock_guard<std::mutex> lk(s_ctrMutex);
        s_pendingOps.swap(ops);
    }
    if (ops.empty()) return;
    FlushPendingContainerOpsSafe(ops);
}
