#include "CoSyncProxyLooks.h"

#include "CoSyncProfiles.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncGameAPI.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"

#include "GameData.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "GameReferences.h"

#include <Windows.h>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>

extern RelocPtr<DataHandler*> g_dataHandler;
extern RelocAddr<_LookupFormByID> LookupFormByID;

namespace
{
    // CoSync.esp local IDs: RMPlayer, then RMPlayer2..RMPlayer7 (added
    // 2026-09-26 as exact copies of RMPlayer: no template, own face data).
    constexpr uint32_t kSlotLocalIDs[] = { 0x00A797, 0x00B6CF, 0x00B6D0, 0x00B6D1, 0x00B6D2, 0x00B6D3, 0x00B6D4 };
    constexpr int      kSlotCount = sizeof(kSlotLocalIDs) / sizeof(kSlotLocalIDs[0]);

    std::unordered_map<uint32_t, int>         s_slotOf;      // entityID -> slot
    bool                                      s_slotUsed[kSlotCount] = {};
    std::unordered_map<uint32_t, std::string> s_looks;       // entityID -> latest look
    std::unordered_set<uint32_t>              s_pending;     // looks not yet on a live proxy
    std::string                               s_mineSent;    // our last sent look
    ULONGLONG                                 s_lastSelfCheck = 0;

    uint8_t ModIndex()
    {
        static uint8_t s_index = 0xFF;
        if (s_index != 0xFF)
            return s_index;
        DataHandler* dh = *g_dataHandler;
        if (dh)
            s_index = dh->GetLoadedModIndex("CoSync.esp");
        return s_index;
    }

    uint32_t SlotFormID(int slot)
    {
        const uint8_t mod = ModIndex();
        if (mod == 0xFF || slot < 0 || slot >= kSlotCount)
            return 0;
        return (static_cast<uint32_t>(mod) << 24) | kSlotLocalIDs[slot];
    }

    TESNPC* SlotBase(int slot)
    {
        const uint32_t id = SlotFormID(slot);
        TESForm* f = id ? LookupFormByID(id) : nullptr;
        return (f && f->formType == kFormType_NPC_) ? static_cast<TESNPC*>(f) : nullptr;
    }

    // The base this entity OWNS (its slot) -- never the shared fallback, so a
    // look is never painted onto someone else's proxy.
    TESNPC* OwnedBase(uint32_t entityID)
    {
        auto it = s_slotOf.find(entityID);
        return it == s_slotOf.end() ? nullptr : SlotBase(it->second);
    }

    // Applies the stored look to a live proxy. False = not spawned yet.
    bool ApplyToLiveProxy(uint32_t entityID)
    {
        CoSyncPlayer* proxy = g_CoSyncPlayerManager.GetPlayerByEntityID(entityID);
        if (!proxy || !proxy->hasSpawned || !proxy->actorRef)
            return false;
        TESNPC* base = OwnedBase(entityID);
        auto it = s_looks.find(entityID);
        if (!base || it == s_looks.end())
            return true;   // nothing we can apply (shared fallback base) -- drop it
        if (proxy->actorRef->baseForm != base)
            return true;   // spawned before slots existed / from another base
        bool sexChanged = false;
        if (!CoSyncProfiles::WriteLooksToBase(base, it->second, sexChanged))
        {
            LOG_WARN("[ProxyLooks] Could not apply the look of entity=%u", entityID);
            return true;
        }
        if (sexChanged)
            CoSyncGameAPI::RefreshActor3D(proxy->actorRef);   // new skeleton/body: full reload
        else
            CoSyncProfiles::RebuildFace(proxy->actorRef);
        LOG_INFO("[ProxyLooks] Updated the look of entity=%u (%s)", entityID, sexChanged ? "full reload" : "face rebuild");
        return true;
    }
}

namespace CoSyncProxyLooks
{
    uint32_t BaseFormForEntity(uint32_t entityID)
    {
        LOG_REQUIREMENT("CoSyncProxyLooks::BaseFormForEntity", "Main thread (spawn task). Needs CoSync.esp loaded.");
        auto it = s_slotOf.find(entityID);
        if (it != s_slotOf.end() && SlotBase(it->second))
            return SlotFormID(it->second);
        for (int slot = 0; slot < kSlotCount; ++slot)
        {
            if (s_slotUsed[slot] || !SlotBase(slot))
                continue;
            s_slotUsed[slot] = true;
            s_slotOf[entityID] = slot;
            LOG_INFO("[ProxyLooks] entity=%u -> proxy base slot %d (0x%08X)", entityID, slot, SlotFormID(slot));
            return SlotFormID(slot);
        }
        LOG_WARN("[ProxyLooks] No free proxy base for entity=%u (older CoSync.esp, or more than %d players) -- "
                 "using the shared RMPlayer look", entityID, kSlotCount);
        return SlotFormID(0);
    }

    bool IsProxyBaseFormID(uint32_t formID)
    {
        if (!formID || (formID >> 24) != ModIndex())
            return false;
        const uint32_t local = formID & 0x00FFFFFF;
        for (uint32_t id : kSlotLocalIDs)
            if (local == id)
                return true;
        return false;
    }

    void ApplyDisplayName(uint32_t entityID)
    {
        // 0.5.4: the name the game shows for the proxy (crosshair / activate
        // prompt, HUD target bar, kill messages) is its base's FULL, which
        // CoSync.esp ships as "2nd Player" on all seven RMPlayer records.
        // Every remote player owns one of those bases (BaseFormForEntity), so
        // the owner's network username goes on it. The identity itself stays
        // SteamID -> entityID -> CoSyncPlayer; this is display only. A player
        // on the shared fallback base (an 8th+ remote player) is left alone:
        // that base would show somebody else's name.
        TESNPC* base = OwnedBase(entityID);
        CoSyncPlayer* pl = g_CoSyncPlayerManager.GetPlayerByEntityID(entityID);
        if (!base || !pl || pl->username.empty())
            return;
        const char* cur = base->fullName.name.c_str();
        if (cur && pl->username == cur)
            return;
        base->fullName.name = BSFixedString(pl->username.c_str());
        LOG_INFO("[CoSyncProxy] NAME entity=%u base=0x%08X shown as '%s' (was '%s')",
            entityID, base->formID, pl->username.c_str(), cur ? cur : "");
    }

    void ApplyToBaseBeforeSpawn(uint32_t entityID, TESNPC* base)
    {
        if (base && base == OwnedBase(entityID))
            ApplyDisplayName(entityID);
        auto it = s_looks.find(entityID);
        if (!base || it == s_looks.end() || base != OwnedBase(entityID))
            return;
        bool sexChanged = false;   // nothing spawned yet: no reload needed
        if (CoSyncProfiles::WriteLooksToBase(base, it->second, sexChanged))
        {
            s_pending.erase(entityID);
            LOG_INFO("[ProxyLooks] entity=%u spawns with its own look", entityID);
        }
    }

    void OnProxyDespawned(uint32_t entityID)
    {
        auto it = s_slotOf.find(entityID);
        if (it == s_slotOf.end())
            return;
        s_slotUsed[it->second] = false;
        s_slotOf.erase(it);
        s_pending.erase(entityID);
    }

    bool IsLooksMessage(const std::string& msg)
    {
        return msg.rfind("LOOKS|", 0) == 0;
    }

    void OnReceive(const std::string& msg, bool isHostRole)
    {
        LOG_REQUIREMENT("CoSyncProxyLooks::OnReceive", "Main thread (transport inbox).");
        const size_t bar = msg.find('|', 6);
        if (bar == std::string::npos)
            return;
        const uint32_t eid = static_cast<uint32_t>(std::strtoul(msg.substr(6, bar - 6).c_str(), nullptr, 10));
        if (!eid)
            return;
        if (isHostRole)
            CoSyncTransport::Send(msg);   // relay to every client
        if (eid == CoSyncNet::GetMyEntityID())
            return;                       // our own look, bounced back
        std::string blob = msg.substr(bar + 1);
        auto it = s_looks.find(eid);
        if (it != s_looks.end() && it->second == blob)
            return;                       // unchanged
        s_looks[eid] = std::move(blob);
        s_pending.insert(eid);
        LOG_INFO("[ProxyLooks] RX look for entity=%u (%zu bytes)", eid, s_looks[eid].size());
    }

    void SendMine()
    {
        const uint32_t eid = CoSyncNet::GetMyEntityID();
        if (!eid || !CoSyncTransport::IsInitialized())
            return;
        const std::string blob = CoSyncProfiles::CaptureMyLooks();
        if (blob.empty())
            return;
        CoSyncTransport::Send("LOOKS|" + std::to_string(eid) + "|" + blob);
        s_mineSent = blob;
        LOG_INFO("[ProxyLooks] Sent my look (%zu bytes)", blob.size());
    }

    void HostSendAll()
    {
        SendMine();
        for (const auto& kv : s_looks)
            CoSyncTransport::Send("LOOKS|" + std::to_string(kv.first) + "|" + kv.second);
    }

    void Tick()
    {
        // A look changed mid-session (barber, surgeon, mirror): resend it.
        const ULONGLONG nowMs = GetTickCount64();
        if (nowMs - s_lastSelfCheck >= 5000 && !s_mineSent.empty() && CoSyncNet::IsConnected() &&
            CoSyncProfiles::IsCharacterApplied() && !CoSyncProfiles::IsEditorBusy())
        {
            s_lastSelfCheck = nowMs;
            const std::string now = CoSyncProfiles::CaptureMyLooks();
            if (!now.empty() && now != s_mineSent)
            {
                LOG_INFO("[ProxyLooks] My look changed -- resending");
                SendMine();
            }
        }

        for (auto it = s_pending.begin(); it != s_pending.end();)
        {
            if (ApplyToLiveProxy(*it))
                it = s_pending.erase(it);
            else
                ++it;
        }
    }

    void Reset()
    {
        s_slotOf.clear();
        for (bool& used : s_slotUsed) used = false;
        s_looks.clear();
        s_pending.clear();
        s_mineSent.clear();
    }
}
