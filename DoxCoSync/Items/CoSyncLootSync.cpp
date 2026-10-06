#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncLootSync.h"

#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncEntityRegistry.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"

#include "GameForms.h"
#include "GameReferences.h"
#include "GameFormComponents.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"

#include <mutex>
#include <vector>
#include <unordered_set>
#include <sstream>
#include <random>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>

extern RelocAddr<_LookupFormByID> LookupFormByID;
extern CoSyncEntityRegistry       g_CoSyncEntities;
extern CoSyncPlayerManager        g_CoSyncPlayerManager;

namespace
{

    uint32_t s_sessionSalt = 0;

    struct PersonalLootOp
    {
        uint32_t                                    targetEntityID = 0;
        uint32_t                                    sourceEntityID = 0;
        std::vector<CoSyncInventorySync::ItemEntry> items;
    };

    std::mutex                   s_mutex;
    std::vector<PersonalLootOp>  s_pending;

    std::unordered_set<uint32_t> s_reseedledContainers;

}

static uint32_t MixHash(uint32_t a, uint32_t b)
{


    uint32_t h = a ^ b;
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    return h;
}

void CoSyncLootSync::SetSessionSalt(uint32_t salt)
{
    s_sessionSalt = salt;
    LOG_UPDATED("[LootSync] Session salt set: 0x%08X", salt);
}

uint32_t CoSyncLootSync::GetSessionSalt()
{
    return s_sessionSalt;
}

std::vector<CoSyncInventorySync::ItemEntry> CoSyncLootSync::GeneratePersonalLoot(
    const std::vector<CoSyncInventorySync::ItemEntry>& baseItems,
    uint32_t npcEntityID,
    uint32_t targetPlayerEntityID)
{
    if (baseItems.empty())
        return {};


    uint32_t seed = MixHash(s_sessionSalt ^ npcEntityID, targetPlayerEntityID * 0x9E3779B9u);

    std::mt19937 rng(seed);


    std::uniform_real_distribution<float> dropRoll(0.f, 1.f);


    std::uniform_real_distribution<float> countRoll(0.5f, 1.5f);

    std::vector<CoSyncInventorySync::ItemEntry> result;
    result.reserve(baseItems.size());

    for (const CoSyncInventorySync::ItemEntry& item : baseItems)
    {
        if (dropRoll(rng) < 0.80f)
        {
            const float mult  = countRoll(rng);
            const int   count = std::max(1, static_cast<int>(
                static_cast<float>(item.count) * mult + 0.5f));

            result.push_back({ item.formID, count });
        }
    }


    if (result.empty())
    {
        std::uniform_int_distribution<size_t> pickIdx(0, baseItems.size() - 1);
        const auto& pick = baseItems[pickIdx(rng)];
        result.push_back({ pick.formID, std::max(1, pick.count) });
    }

    LOG_UPDATED("[LootSync] GeneratePersonalLoot: npc=%u target=%u base=%u → personal=%u items",
        npcEntityID, targetPlayerEntityID,
        (unsigned)baseItems.size(), (unsigned)result.size());

    return result;
}

void CoSyncLootSync::HostBroadcastPersonalLoot(
    uint32_t                                           npcEntityID,
    const std::vector<CoSyncInventorySync::ItemEntry>& baseItems)
{
    if (!CoSyncNet::IsHost() || !CoSyncNet::IsConnected())
        return;

    if (baseItems.empty())
        return;


    std::vector<uint32_t> peerEntityIDs = CoSyncNet::GetConnectedPeerEntityIDs();

    for (uint32_t clientEID : peerEntityIDs)
    {
        if (clientEID == 0 || clientEID == CoSyncNet::GetMyEntityID())
            continue;

        std::vector<CoSyncInventorySync::ItemEntry> personal =
            GeneratePersonalLoot(baseItems, npcEntityID, clientEID);

        if (personal.empty())
            continue;

        const std::string pkt = SerializePersonalLoot(clientEID, npcEntityID, personal);
        CoSyncTransport::Send(pkt);

        LOG_UPDATED("[LootSync] TX PLOOT client=%u npc=%u items=%u",
            clientEID, npcEntityID, (unsigned)personal.size());
    }
}

bool CoSyncLootSync::IsSalt(const std::string& msg)
{
    return msg.rfind("SALT|", 0) == 0;
}

bool CoSyncLootSync::IsPersonalLoot(const std::string& msg)
{
    return msg.rfind("PLOOT|", 0) == 0;
}

std::string CoSyncLootSync::SerializeSalt(uint32_t salt)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "SALT|%u", salt);
    return buf;
}

std::string CoSyncLootSync::SerializePersonalLoot(
    uint32_t                                           targetEntityID,
    uint32_t                                           sourceEntityID,
    const std::vector<CoSyncInventorySync::ItemEntry>& items)
{

    std::ostringstream ss;
    ss << "PLOOT|" << targetEntityID << "|" << sourceEntityID;
    for (const auto& item : items)
        ss << "|" << item.formID << ":" << item.count;
    return ss.str();
}

void CoSyncLootSync::OnReceiveSalt(const std::string& msg)
{

    const char* p = msg.c_str() + 5;
    char* end = nullptr;
    uint32_t salt = static_cast<uint32_t>(std::strtoul(p, &end, 10));
    if (end == p || salt == 0)
    {
        LOG_UPDATED_WARN("[LootSync] Malformed SALT packet: '%s'", msg.c_str());
        return;
    }
    SetSessionSalt(salt);
}

void CoSyncLootSync::OnReceivePersonalLoot(const std::string& msg)
{

    const char* s = msg.c_str() + 6;
    char* e = nullptr;

    uint32_t targetEID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (!e || *e != '|' || targetEID == 0) { LOG_UPDATED_WARN("[LootSync] PLOOT: bad targetEID"); return; }
    s = e + 1;


    if (targetEID != CoSyncNet::GetMyEntityID())
        return;

    uint32_t sourceEID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (!e || (*e != '|' && *e != '\0')) { LOG_UPDATED_WARN("[LootSync] PLOOT: bad sourceEID"); return; }
    s = e;

    PersonalLootOp op{};
    op.targetEntityID = targetEID;
    op.sourceEntityID = sourceEID;

    while (*s == '|')
    {
        ++s;
        char* sep = nullptr;
        uint32_t fid   = static_cast<uint32_t>(std::strtoul(s, &sep, 10));
        if (!sep || *sep != ':' || fid == 0) break;
        s = sep + 1;
        int count = static_cast<int>(std::strtol(s, &e, 10));
        if (!e) break;
        s = e;
        if (fid != 0 && count > 0)
            op.items.push_back({ fid, count });
    }

    if (op.items.empty())
    {
        LOG_UPDATED_WARN("[LootSync] PLOOT: empty item list for source=%u", sourceEID);
        return;
    }

    LOG_UPDATED("[LootSync] RX PLOOT source=%u items=%u — queuing",
        sourceEID, (unsigned)op.items.size());

    std::lock_guard<std::mutex> lk(s_mutex);
    s_pending.push_back(std::move(op));
}

// UPDATE 2026-06-20 (round 41 audit): same unguarded peer-controlled
// item.formID -> LookupFormByID crash pattern fixed elsewhere this session.
// SEH-wrapped, noinline (load-bearing under /GL+/LTCG).
// Total count of `form` already in `ref`'s inventory (all stacks), read under
// the inventory read lock. -1 when the list can't be read.
__declspec(noinline) static int CountHeld_Impl(TESObjectREFR* ref, TESForm* form)
{
    BGSInventoryList* inv = ref->inventoryList;
    if (!inv)
        return 0;
    int total = 0;
    BSReadLocker lock(&inv->inventoryLock);
    for (UInt32 i = 0; i < inv->items.count; ++i)
    {
        const BGSInventoryItem& e = inv->items.entries[i];
        if (e.form != form)
            continue;
        for (BGSInventoryItem::Stack* s = e.stack; s; s = s->next)
            total += s->count;
    }
    return total;
}

static int CountHeld(TESObjectREFR* ref, TESForm* form)
{
    __try { return CountHeld_Impl(ref, form); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

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

void CoSyncLootSync::FlushPendingLootOps()
{
    static const BSFixedString kAddItem("AddItem");
    std::vector<PersonalLootOp> ops;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_pending.empty())
            return;
        ops.swap(s_pending);
    }

    for (const PersonalLootOp& op : ops)
    {

        Actor* actor = g_CoSyncEntities.GetActor(op.sourceEntityID);
        if (!actor)
        {
            LOG_UPDATED_WARN("[LootSync] FLUSH: source entity=%u has no actor (proxy not yet spawned?)",
                op.sourceEntityID);
            continue;
        }

        for (const CoSyncInventorySync::ItemEntry& item : op.items)
        {
            TESForm* form = SafeLookupForm(item.formID);
            if (!form)
            {
                LOG_UPDATED_WARN("[LootSync] FLUSH: formID=0x%08X not found — skipping", item.formID);
                continue;
            }

            // Reconcile, don't stack: the corpse already carries what the NPC
            // had (its gun above all). Adding the full personal roll on top
            // doubled those items -- and a second copy of the NPC's weapon is
            // what set up the 2026-09-29 equip crash. Add only the missing part.
            const int held = CountHeld(actor, form);
            const int missing = (held < 0) ? item.count : item.count - held;
            if (missing <= 0)
                continue;

            VMArray<VMVariable> addArgs;
            VMVariable vF; vF.Set(&form); addArgs.Push(&vF);
            SInt32 cnt = static_cast<SInt32>(missing);
            VMVariable vC; vC.Set(&cnt);  addArgs.Push(&vC);
            bool sil = true;
            VMVariable vS; vS.Set(&sil);  addArgs.Push(&vS);
            CallFunctionNoWait(actor, kAddItem, addArgs);
        }

        LOG_UPDATED("[LootSync] FLUSH: Applied %u personal loot items to entity=%u proxy",
            (unsigned)op.items.size(), op.sourceEntityID);
    }
}

void CoSyncLootSync::OnContainerFirstOpen(uint32_t containerRefFormID,
                                           uint32_t playerEntityID)
{
    if (containerRefFormID == 0)
        return;


    if (s_reseedledContainers.count(containerRefFormID))
        return;
    s_reseedledContainers.insert(containerRefFormID);


    uint32_t seed = MixHash(
        s_sessionSalt ^ containerRefFormID,
        playerEntityID * 0x6C62272Eu);

    std::mt19937 rng(seed);

    std::uniform_real_distribution<float> factorRoll(0.70f, 1.30f);
    const float factor = factorRoll(rng);

    LOG_UPDATED("[LootSync] Container reseed: refFormID=0x%08X player=%u salt=0x%08X factor=%.2f",
        containerRefFormID, playerEntityID, s_sessionSalt, factor);

}
