#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncInventorySync.h"

#include "ConsoleLogger.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncRequirementsLog.h"

#include "GameReferences.h"
#include "GameFormComponents.h"
#include "GameForms.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"

#include <mutex>
#include <vector>
#include <sstream>
#include <cstdlib>
#include <cstring>

bool CoSyncInventorySync::IsInventory(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncInventorySync::IsInventory", "Just checks the text of a message. No game state needed.");
    return msg.rfind("INV|", 0) == 0;
}

std::string CoSyncInventorySync::SerializeInventory(uint32_t entityID,
                                                    const std::vector<ItemEntry>& items)
{
    LOG_REQUIREMENT("CoSyncInventorySync::SerializeInventory", "Just turns a player's item list into text. Doesn't touch the game at all.");
    if (items.empty())
        return "";

    std::ostringstream ss;
    ss << "INV|" << entityID;
    for (const ItemEntry& e : items)
        ss << '|' << e.formID << ':' << e.count;
    return ss.str();
}

// UPDATE 2026-06-18 (round 23 audit, exhaustive pass): split into a noinline
// _Impl plus a thin SEH wrapper -- raw loop over invList->items (an
// engine-owned array), plus a vtable-style Stack::Visit() traversal, with
// zero protection. Same unguarded-engine-access pattern fixed several other
// times this session. Called on every remote NPC death while host+connected.
__declspec(noinline) static void SnapshotInventory_Impl(Actor* actor, std::vector<CoSyncInventorySync::ItemEntry>& result)
{
    LOG_REQUIREMENT("CoSyncInventorySync::SnapshotInventory_Impl", "Needs a valid actor. Only meant to be called through SnapshotInventorySafe, which wraps it safely.");
    BGSInventoryList* invList = actor->inventoryList;
    if (!invList)
        return;

    for (UInt32 i = 0; i < invList->items.count; ++i)
    {
        const BGSInventoryItem& item = invList->items[i];
        if (!item.form)
            continue;


        int32_t total = 0;
        if (item.stack)
        {
            item.stack->Visit([&total](BGSInventoryItem::Stack* s) {
                total += s->count;
                return true;
            });
        }

        if (total <= 0)
            continue;

        CoSyncInventorySync::ItemEntry e;
        e.formID = item.form->formID;
        e.count  = total;
        result.push_back(e);
    }
}

// C2712: __try cannot share a function with local C++ objects requiring
// unwinding. Kept separate from SnapshotInventory() (which owns the
// std::vector) so this one only ever sees a reference parameter.
static void SnapshotInventorySafe(Actor* actor, std::vector<CoSyncInventorySync::ItemEntry>& result)
{
    LOG_REQUIREMENT("CoSyncInventorySync::SnapshotInventorySafe", "Needs a valid actor. Reads their inventory safely, even if it turns out to be corrupted.");
    __try
    {
        SnapshotInventory_Impl(actor, result);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[InventorySync] SnapshotInventory faulted -- caught");
    }
}

std::vector<CoSyncInventorySync::ItemEntry> CoSyncInventorySync::SnapshotInventory(void* actorVoid)
{
    LOG_REQUIREMENT("CoSyncInventorySync::SnapshotInventory", "Handles a missing actor gracefully (returns an empty result). Otherwise needs a real actor to be passed in.");
    std::vector<ItemEntry> result;

    Actor* actor = static_cast<Actor*>(actorVoid);
    if (!actor)
        return result;

    SnapshotInventorySafe(actor, result);

    return result;
}

struct InventoryOp
{
    uint32_t entityID = 0;
    std::vector<CoSyncInventorySync::ItemEntry> items;
};

static std::mutex               s_opMutex;
static std::vector<InventoryOp> s_pendingOps;

void CoSyncInventorySync::OnReceiveInventory(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncInventorySync::OnReceiveInventory", "Needs the message to actually be an inventory packet, checked before reading it.");
    if (!IsInventory(msg))
        return;


    const char* p = msg.c_str() + 4;
    char* end = nullptr;

    uint32_t entityID = (uint32_t)std::strtoul(p, &end, 10);
    if (!end || entityID == 0)
    {
        LOG_WARN("[InventorySync] Malformed INV packet: '%s'", msg.c_str());
        return;
    }

    InventoryOp op;
    op.entityID = entityID;

    p = end;
    while (*p == '|')
    {
        ++p;
        char* colonPos = nullptr;
        uint32_t formID = (uint32_t)std::strtoul(p, &colonPos, 10);
        if (!colonPos || *colonPos != ':')
            break;

        char* nextEnd = nullptr;
        int32_t count = (int32_t)std::strtol(colonPos + 1, &nextEnd, 10);
        if (!nextEnd)
            break;

        if (formID != 0 && count > 0)
        {
            ItemEntry e;
            e.formID = formID;
            e.count  = count;
            op.items.push_back(e);
        }

        p = nextEnd;
    }

    if (!op.items.empty())
    {
        std::lock_guard<std::mutex> lk(s_opMutex);
        s_pendingOps.push_back(std::move(op));
    }
}

// Round 106 audit: e.formID is peer-controlled (parsed straight off an
// "INV|" network packet, see ParseInventory above) and was fed into
// LookupFormByID with zero SEH protection -- the exact crash pattern this
// codebase already guards everywhere else (CoSyncMapSync::FlushPendingMapOps,
// CoSyncPowerArmorSync, CoSyncCompanionSync's HostScanCompanions, etc.), just
// missed here. Split into _Impl (holds the real loop, including the
// VMArray/VMVariable locals that would otherwise trip C2712 if built inside
// a __try-containing function) + thin Safe wrapper, same convention as
// FlushPendingMapOps_Impl/FlushPendingMapOpsSafe.
__declspec(noinline) static void ApplyInventoryOps_Impl(const std::vector<InventoryOp>& ops)
{
    LOG_REQUIREMENT("CoSyncInventorySync::ApplyInventoryOps_Impl", "Needs the player manager to be ready, and checks each stand-in character is actually spawned before touching it. Only meant to be called through ApplyInventoryOpsSafe, which wraps it safely.");
    static const BSFixedString kAddItem("AddItem");
    for (const auto& op : ops)
    {
        CoSyncPlayer* proxy = g_CoSyncPlayerManager.GetPlayerByEntityID(op.entityID);
        if (!proxy || !proxy->actorRef || !proxy->hasSpawned)
            continue;

        Actor* actor = proxy->actorRef;

        LOG_INFO("[InventorySync] Applying %u items to entity=%u corpse",
            (unsigned)op.items.size(), op.entityID);

        for (const auto& e : op.items)
        {
            TESForm* form = LookupFormByID(e.formID);
            if (!form)
                continue;


            VMArray<VMVariable> args;

            VMVariable vForm;
            vForm.Set(&form);
            args.Push(&vForm);

            VMVariable vCount;
            SInt32 count = e.count;
            vCount.Set(&count);
            args.Push(&vCount);

            VMVariable vSilent;
            bool silent = true;
            vSilent.Set(&silent);
            args.Push(&vSilent);

            CallFunctionNoWait(actor, kAddItem, args);
        }
    }
}

__declspec(noinline) static void ApplyInventoryOpsSafe(const std::vector<InventoryOp>& ops)
{
    LOG_REQUIREMENT("CoSyncInventorySync::ApplyInventoryOpsSafe", "Handles a bad item ID from another player safely instead of crashing. Needs the player manager to be ready.");
    __try
    {
        ApplyInventoryOps_Impl(ops);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[InventorySync] EXCEPTION in ApplyInventoryOpsSafe — corrupt formID in peer inventory op?");
    }
}

void CoSyncInventorySync::FlushPendingInventoryOps()
{
    LOG_REQUIREMENT("CoSyncInventorySync::FlushPendingInventoryOps", "Safe to call any time. Applies whatever inventory updates are queued up, if any.");
    std::vector<InventoryOp> ops;
    {
        std::lock_guard<std::mutex> lk(s_opMutex);
        if (s_pendingOps.empty())
            return;
        ops.swap(s_pendingOps);
    }

    ApplyInventoryOpsSafe(ops);
}
