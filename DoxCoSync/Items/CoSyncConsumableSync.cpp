#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncConsumableSync.h"
#include "ConsoleLogger.h"
#include "CoSyncEntityRegistry.h"

#include "GameReferences.h"
#include "GameRTTI.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"
#include "GameTypes.h"

#include <mutex>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>

// Round 106 audit: op.itemFormID comes straight off the wire ("CONS|"/"SPELL|"
// packets) and was fed into LookupFormByID with zero SEH -- same unguarded-
// peer-FormID crash pattern already found and fixed in QuestSync/EquipHook/
// OutfitSync/PowerArmorSync/DoorHook/LockHook/SpecialSync. This file had no
// <Windows.h> include at all before (needed for __try/__except/EXCEPTION_EXECUTE_HANDLER).
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

struct ConsumableOp
{
    uint32_t entityID;
    uint32_t itemFormID;
    bool     isSpell;
};

static std::mutex              s_mutex;
static std::vector<ConsumableOp> s_pending;

bool CoSyncConsumableSync::IsConsumable(const std::string& msg)
{
    return msg.size() > 5 && msg[0] == 'C' && msg[1] == 'O' &&
           msg[2] == 'N' && msg[3] == 'S' && msg[4] == '|';
}

bool CoSyncConsumableSync::IsSpell(const std::string& msg)
{
    return msg.size() > 6 && msg[0] == 'S' && msg[1] == 'P' &&
           msg[2] == 'E' && msg[3] == 'L' && msg[4] == 'L' && msg[5] == '|';
}

std::string CoSyncConsumableSync::SerializeConsumable(uint32_t entityID, uint32_t itemFormID)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "CONS|%u|%u", entityID, itemFormID);
    return buf;
}

std::string CoSyncConsumableSync::SerializeSpell(uint32_t entityID, uint32_t effectFormID)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "SPELL|%u|%u", entityID, effectFormID);
    return buf;
}

void CoSyncConsumableSync::OnReceiveConsumable(const std::string& msg)
{

    uint32_t eid = 0, fid = 0;
    if (std::sscanf(msg.c_str() + 5, "%u|%u", &eid, &fid) != 2)
    {
        LOG_UPDATED_WARN("[ConsSync RX] Malformed CONS packet: '%s'", msg.c_str());
        return;
    }

    LOG_UPDATED("[ConsSync RX] CONS packet queued: entity=%u itemFormID=0x%08X", eid, fid);

    std::lock_guard<std::mutex> lk(s_mutex);
    s_pending.push_back({ eid, fid, false });
}

void CoSyncConsumableSync::OnReceiveSpell(const std::string& msg)
{

    uint32_t eid = 0, fid = 0;
    if (std::sscanf(msg.c_str() + 6, "%u|%u", &eid, &fid) != 2)
    {
        LOG_UPDATED_WARN("[ConsSync RX] Malformed SPELL packet: '%s'", msg.c_str());
        return;
    }

    LOG_UPDATED("[SpellSync RX] SPELL packet queued: entity=%u spellFormID=0x%08X", eid, fid);

    std::lock_guard<std::mutex> lk(s_mutex);
    s_pending.push_back({ eid, fid, true });
}

void CoSyncConsumableSync::FlushPendingConsumableOps()
{
    static const BSFixedString kEquipItem("EquipItem");
    static const BSFixedString kAddSpell("AddSpell");
    std::vector<ConsumableOp> ops;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        ops.swap(s_pending);
    }
    if (ops.empty()) return;

    LOG_UPDATED("[ConsSync FLUSH] Processing %u pending consumable/spell op(s)", (unsigned)ops.size());

    GameVM* vm = *g_gameVM;
    if (!vm || !vm->m_virtualMachine)
    {
        LOG_UPDATED_WARN("[ConsSync FLUSH] GameVM not ready — ops dropped!");
        return;
    }

    for (const ConsumableOp& op : ops)
    {
        Actor* actor = g_CoSyncEntities.GetActor(op.entityID);
        if (!actor)
        {
            LOG_UPDATED_WARN("[ConsSync FLUSH] entity=%u not found as Actor (not spawned yet?)", op.entityID);
            continue;
        }

        TESForm* form = SafeLookupForm(op.itemFormID);
        if (!form)
        {
            LOG_UPDATED_WARN("[ConsSync FLUSH] formID=0x%08X not found in form table", op.itemFormID);
            continue;
        }

        if (!op.isSpell)
        {

            VMArray<VMVariable> args;

            VMVariable vItem;
            vItem.Set(&form);
            args.Push(&vItem);

            bool preventRemoval = false;
            VMVariable vPrev;
            vPrev.Set(&preventRemoval);
            args.Push(&vPrev);

            bool silent = true;
            VMVariable vSilent;
            vSilent.Set(&silent);
            args.Push(&vSilent);

            CallFunctionNoWait(actor, kEquipItem, args);

            LOG_UPDATED("[ConsSync APPLY OK] Consumable equipped: entity=%u formID=0x%08X actor=%p",
                op.entityID, op.itemFormID, static_cast<void*>(actor));
        }
        else
        {


            SpellItem* spell = DYNAMIC_CAST(form, TESForm, SpellItem);
            if (!spell)
            {
                LOG_UPDATED_WARN("[ConsumableSync] FormID=0x%08X is not a SpellItem", op.itemFormID);
                continue;
            }

            VMArray<VMVariable> args;
            VMVariable vSpell;
            vSpell.Set(&spell);
            args.Push(&vSpell);

            CallFunctionNoWait(actor, kAddSpell, args);

            LOG_UPDATED("[SpellSync APPLY OK] Spell added: entity=%u formID=0x%08X actor=%p",
                op.entityID, op.itemFormID, static_cast<void*>(actor));
        }
    }
}
