#include "CoSyncItemMods.h"

#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncGameAPI.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "ConsoleLogger.h"

#include "GameForms.h"
#include "GameObjects.h"
#include "GameReferences.h"
#include "GameExtraData.h"
#include "GameFormComponents.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

extern RelocAddr<_LookupFormByID> LookupFormByID;
extern PlayerCharacter* CoSyncGetPlayer();

namespace
{
    // item formID -> its object mods (in instance order)
    using ModSet = std::map<uint32_t, std::vector<uint32_t>>;

    std::string s_lastSent;
    double      s_lastSentAt = 0.0;
    double      s_lastScan = 0.0;
    double      s_lastApply = 0.0;
    std::unordered_map<uint32_t, ModSet> s_wanted;   // entityID -> what its items should carry
    // (entity, item, mod) attach attempts that failed: given up after 3 so an
    // incompatible mod isn't retried every second forever.
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, int> s_failures;

    // Mods on one inventory stack. Runs inside the callers' SEH guards.
    void StackMods(BGSInventoryItem::Stack* s, std::vector<uint32_t>& out)
    {
        BSExtraData* x = s->extraData ? s->extraData->GetByType(kExtraData_ObjectInstance) : nullptr;
        auto* oi = static_cast<BGSObjectInstanceExtra*>(x);
        if (!oi || !oi->data || !oi->data->forms)
            return;
        const UInt32 n = oi->data->blockSize / sizeof(BGSObjectInstanceExtra::Data::Form);
        for (UInt32 k = 0; k < n && k < 64; ++k)
            if (oi->data->forms[k].formId)
                out.push_back(oi->data->forms[k].formId);
    }

    // Weapons/armor with mods: the equipped copies (players) or every piece
    // (Power Armor frames).
    __declspec(noinline) void CollectEquipped(TESObjectREFR* ref, ModSet& out, bool equippedOnly = true)
    {
        BGSInventoryList* inv = ref ? ref->inventoryList : nullptr;
        if (!inv)
            return;
        for (UInt32 i = 0; i < inv->items.count; ++i)
        {
            BGSInventoryItem& item = inv->items[i];
            if (!item.form || (item.form->formType != kFormType_WEAP && item.form->formType != kFormType_ARMO))
                continue;
            for (BGSInventoryItem::Stack* s = item.stack; s; s = s->next)
            {
                if (equippedOnly && !(s->flags & BGSInventoryItem::Stack::kFlagEquipped))
                    continue;
                std::vector<uint32_t> mods;
                StackMods(s, mods);
                if (!mods.empty())
                    out[item.form->formID] = mods;
                break;
            }
        }
    }

    // Mods currently on the (single) copy of `form` a proxy carries.
    // count = how many of that item it holds.
    __declspec(noinline) void ProxyItemMods(TESObjectREFR* ref, uint32_t form, int& count, std::vector<uint32_t>& out)
    {
        count = 0;
        BGSInventoryList* inv = ref ? ref->inventoryList : nullptr;
        if (!inv)
            return;
        for (UInt32 i = 0; i < inv->items.count; ++i)
        {
            BGSInventoryItem& item = inv->items[i];
            if (!item.form || item.form->formID != form)
                continue;
            for (BGSInventoryItem::Stack* s = item.stack; s; s = s->next)
            {
                count += s->count;
                if (out.empty())
                    StackMods(s, out);
            }
            return;
        }
    }

    bool SafeCollectEquipped(TESObjectREFR* ref, ModSet& out, bool equippedOnly = true)
    {
        __try { CollectEquipped(ref, out, equippedOnly); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool SafeProxyItemMods(TESObjectREFR* ref, uint32_t form, int& count, std::vector<uint32_t>& out)
    {
        __try { ProxyItemMods(ref, form, count, out); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    std::string Serialize(const ModSet& set)
    {
        std::string s;
        char buf[16];
        for (const auto& kv : set)
        {
            if (!s.empty()) s += ',';
            std::snprintf(buf, sizeof(buf), "%X:", kv.first);
            s += buf;
            for (size_t k = 0; k < kv.second.size(); ++k)
            {
                std::snprintf(buf, sizeof(buf), k ? "/%X" : "%X", kv.second[k]);
                s += buf;
            }
        }
        return s;
    }

    ModSet Parse(const std::string& text)
    {
        ModSet set;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t end = text.find(',', pos);
            if (end == std::string::npos) end = text.size();
            const std::string entry = text.substr(pos, end - pos);
            const size_t colon = entry.find(':');
            if (colon != std::string::npos)
            {
                const uint32_t item = static_cast<uint32_t>(std::strtoul(entry.substr(0, colon).c_str(), nullptr, 16));
                std::vector<uint32_t> mods;
                size_t m = colon + 1;
                while (m < entry.size())
                {
                    size_t slash = entry.find('/', m);
                    if (slash == std::string::npos) slash = entry.size();
                    const uint32_t id = static_cast<uint32_t>(std::strtoul(entry.substr(m, slash - m).c_str(), nullptr, 16));
                    if (id) mods.push_back(id);
                    m = slash + 1;
                }
                if (item && !mods.empty())
                    set[item] = std::move(mods);
            }
            pos = end + 1;
        }
        return set;
    }

    void SendMine(double now)
    {
        const uint32_t myEid = CoSyncNet::GetMyEntityID();
        if (!myEid || !CoSyncNet::IsConnected())
            return;
        ModSet set;
        if (!SafeCollectEquipped(reinterpret_cast<TESObjectREFR*>(CoSyncGetPlayer()), set))
            return;
        const std::string body = Serialize(set);
        if (body == s_lastSent && now - s_lastSentAt < 20.0)
            return;
        s_lastSent = body;
        s_lastSentAt = now;
        CoSyncTransport::Send("IMODS|" + std::to_string(myEid) + "|" + body);
    }

    void ApplyAll()
    {
        for (const auto& player : s_wanted)
        {
            CoSyncPlayer* proxy = g_CoSyncPlayerManager.GetPlayerByEntityID(player.first);
            if (!proxy || !proxy->hasSpawned || !proxy->actorRef)
                continue;
            TESObjectREFR* ref = reinterpret_cast<TESObjectREFR*>(proxy->actorRef);
            for (const auto& item : player.second)
            {
                int count = 0;
                std::vector<uint32_t> have;
                if (!SafeProxyItemMods(ref, item.first, count, have) || count != 1)
                    continue;   // not carried (yet), or more than one copy (engine won't mod those)
                TESForm* itemForm = LookupFormByID(item.first);
                if (!itemForm)
                    continue;
                size_t attached = 0;
                for (uint32_t modID : item.second)
                {
                    if (std::find(have.begin(), have.end(), modID) != have.end())
                        continue;
                    int& fails = s_failures[std::make_tuple(player.first, item.first, modID)];
                    if (fails >= 3)
                        continue;
                    TESForm* mod = LookupFormByID(modID);
                    if (mod && CoSyncGameAPI::NativeAttachMod(ref, itemForm, mod))
                        ++attached;
                    else if (++fails == 3)
                        LOG_UPDATED_WARN("[ItemMods] entity=%u item=0x%08X: mod 0x%08X won't attach -- giving up",
                            player.first, item.first, modID);
                }
                if (attached)
                    LOG_UPDATED("[ItemMods] entity=%u item=0x%08X: attached %zu mod(s)", player.first, item.first, attached);
            }
        }
    }
}

namespace CoSyncItemMods
{
    std::string DescribeMods(TESObjectREFR* ref, bool equippedOnly)
    {
        ModSet set;
        if (!ref || !SafeCollectEquipped(ref, set, equippedOnly))
            return std::string();
        return Serialize(set);
    }

    int ApplyMods(TESObjectREFR* ref, const std::string& text, uint32_t logTag)
    {
        if (!ref)
            return -1;
        int missing = 0;
        for (const auto& item : Parse(text))
        {
            int count = 0;
            std::vector<uint32_t> have;
            if (!SafeProxyItemMods(ref, item.first, count, have) || count != 1)
            {
                missing += static_cast<int>(item.second.size());
                continue;
            }
            TESForm* itemForm = LookupFormByID(item.first);
            if (!itemForm)
                continue;
            size_t attached = 0;
            for (uint32_t modID : item.second)
            {
                if (std::find(have.begin(), have.end(), modID) != have.end())
                    continue;
                TESForm* mod = LookupFormByID(modID);
                if (mod && CoSyncGameAPI::NativeAttachMod(ref, itemForm, mod))
                    ++attached;
                else
                    ++missing;
            }
            if (attached)
                LOG_UPDATED("[ItemMods] ref=0x%08X (tag %u) item=0x%08X: attached %zu mod(s)",
                    ref->formID, logTag, item.first, attached);
        }
        return missing;
    }

    std::string DescribeWanted(uint32_t eid)
    {
        auto it = s_wanted.find(eid);
        return it == s_wanted.end() ? std::string() : Serialize(it->second);
    }

    bool IsItemModsMessage(const std::string& msg)
    {
        return msg.rfind("IMODS|", 0) == 0;
    }

    void OnReceive(const std::string& msg, bool isHostRole)
    {
        const size_t bar = msg.find('|', 6);
        if (bar == std::string::npos)
            return;
        const uint32_t eid = static_cast<uint32_t>(std::strtoul(msg.substr(6, bar - 6).c_str(), nullptr, 10));
        if (!eid)
            return;
        if (isHostRole)
            CoSyncTransport::Send(msg);   // relay
        if (eid == CoSyncNet::GetMyEntityID())
            return;
        s_wanted[eid] = Parse(msg.substr(bar + 1));
    }

    void Tick(double now)
    {
        if (!CoSyncTransport::IsInitialized())
            return;
        if (now - s_lastScan >= 1.0)
        {
            s_lastScan = now;
            SendMine(now);
        }
        if (now - s_lastApply >= 1.0)
        {
            s_lastApply = now;
            ApplyAll();
        }
    }

    void Reset()
    {
        s_lastSent.clear();
        s_lastSentAt = 0.0;
        s_wanted.clear();
        s_failures.clear();
    }
}
