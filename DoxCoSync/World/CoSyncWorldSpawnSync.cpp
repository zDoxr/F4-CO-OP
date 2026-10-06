#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncWorldSpawnSync.h"
#include "CoSyncWorkshopEngine.h"
#include "CoSyncGameAPI.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncPlayerManager.h"
#include "ConsoleLogger.h"
#include "CoSyncVMFormArg.h"
#include "GameReferences.h"
#include "GameForms.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"
#include "MinHook.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

extern RelocAddr<_LookupFormByID> LookupFormByID;

namespace
{
    // sub_1405E0900: console placement worker (see the header).
    constexpr uintptr_t kRVA_ConsolePlaceWorker = 0x005E0900;
    constexpr uint8_t   kConsolePlacePrologue[16] = {
        0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xE9, 0x48, 0x81, 0xEC };
    using ConsolePlace_t = uint32_t* (*)(uint32_t* outHandle, TESObjectREFR* atRef, TESForm* base,
                                         int count, int distance, int direction, int unk7, char unk8);
    ConsolePlace_t s_origPlace = nullptr;

    constexpr double kResendSec = 10.0;

    std::mutex            s_mx;
    std::vector<uint32_t> s_hostNew;          // handles from the hook, consumed on the tick (host)
    std::vector<uint32_t> s_hostTracked;      // host refs replicated so far

    struct ClientSpawn
    {
        uint32_t base = 0;
        NiPoint3 pos, rot;
        uint32_t ws = 0, cell = 0;
        uint32_t localRef = 0;                // 0 = not created here yet
    };
    std::unordered_map<uint32_t, ClientSpawn> s_client;   // hostRef -> spawn (tick thread only)
    std::vector<std::string>                  s_inbox;    // under s_mx
    double s_lastResend = 0.0;

    uint32_t* Detour_ConsolePlace(uint32_t* outHandle, TESObjectREFR* atRef, TESForm* base,
                                  int count, int distance, int direction, int unk7, char unk8)
    {
        uint32_t* r = s_origPlace(outHandle, atRef, base, count, distance, direction, unk7, unk8);
        // Host only: a client's console placement stays local to that client
        // (the host is the world authority).
        if (r && *r && CoSyncNet::IsConnected() && CoSyncNet::IsHost())
        {
            std::lock_guard<std::mutex> lk(s_mx);
            s_hostNew.push_back(*r);
        }
        return r;
    }

    bool IsActorBase(TESForm* base)
    {
        return base && (base->formType == kFormType_NPC_ || base->formType == kFormType_LVLN);
    }

    void RefLocation(TESObjectREFR* ref, uint32_t& ws, uint32_t& cell)
    {
        TESWorldSpace* w = CALL_MEMBER_FN(ref, GetWorldspace)();
        ws = w ? reinterpret_cast<TESForm*>(w)->formID : 0u;
        cell = ref->parentCell ? reinterpret_cast<TESForm*>(ref->parentCell)->formID : 0u;
    }

    std::string SpawnMsg(TESObjectREFR* ref)
    {
        uint32_t ws = 0, cell = 0;
        RefLocation(ref, ws, cell);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "WSPAWN|%u|%u|%.2f|%.2f|%.2f|%.4f|%.4f|%.4f|%u|%u",
            ref->formID, ref->baseForm ? ref->baseForm->formID : 0u,
            ref->pos.x, ref->pos.y, ref->pos.z, ref->rot.x, ref->rot.y, ref->rot.z, ws, cell);
        return buf;
    }

    bool RefGone(TESObjectREFR* ref)
    {
        return !ref || !CoSyncGameAPI::IsFormStillLive(static_cast<TESForm*>(ref)) ||
               (ref->flags & (TESObjectREFR::kFlag_IsDisabled | TESObjectREFR::kFlag_IsDeleted));
    }

    TESObjectREFR* LookupRef(uint32_t formID)
    {
        TESForm* f = formID ? LookupFormByID(formID) : nullptr;
        if (!f || !CoSyncGameAPI::IsFormStillLive(f))
            return nullptr;
        return (f->formType == kFormType_REFR || f->formType == kFormType_ACHR)
            ? reinterpret_cast<TESObjectREFR*>(f) : nullptr;
    }

    void HostTick(double now)
    {
        std::vector<uint32_t> handles;
        {
            std::lock_guard<std::mutex> lk(s_mx);
            handles.swap(s_hostNew);
        }
        for (uint32_t h : handles)
        {
            TESObjectREFR* ref = CoSyncWorkshopEngine::ResolveHandle(h);
            if (!ref || !ref->baseForm)
                continue;
            if (IsActorBase(ref->baseForm))
            {
                LOG_UPDATED("[WorldSpawn] Host placed actor ref=0x%08X base=0x%08X -- replicated by NPC sync, not here",
                    ref->formID, ref->baseForm->formID);
                continue;
            }
            s_hostTracked.push_back(ref->formID);
            CoSyncTransport::Send(SpawnMsg(ref));
            LOG_UPDATED("[WorldSpawn] Entity Created (host console placement): ref=0x%08X base=0x%08X at (%.0f,%.0f,%.0f)",
                ref->formID, ref->baseForm->formID, ref->pos.x, ref->pos.y, ref->pos.z);
        }

        if (now - s_lastResend < kResendSec)
            return;
        s_lastResend = now;
        for (auto it = s_hostTracked.begin(); it != s_hostTracked.end();)
        {
            TESObjectREFR* ref = LookupRef(*it);
            if (RefGone(ref))
            {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "WSDESPAWN|%u", *it);
                CoSyncTransport::Send(buf);
                LOG_UPDATED("[WorldSpawn] Entity Destroyed (host ref=0x%08X disabled/deleted)", *it);
                it = s_hostTracked.erase(it);
                continue;
            }
            CoSyncTransport::Send(SpawnMsg(ref));   // late joiners / moved objects
            ++it;
        }
    }

    void RemoveLocal(uint32_t hostRef, ClientSpawn& s)
    {
        static const BSFixedString kDelete("Delete");
        if (TESObjectREFR* local = LookupRef(s.localRef))
        {
            CoSyncGameAPI::DisableWorldActor(local);
            VMArray<VMVariable> args;
            CallFunctionNoWait(local, kDelete, args);
        }
        LOG_UPDATED("[WorldSpawn] Entity Destroyed: host ref=0x%08X -> local ref=0x%08X removed", hostRef, s.localRef);
        s.localRef = 0;
    }

    void ClientTick()
    {
        std::vector<std::string> inbox;
        {
            std::lock_guard<std::mutex> lk(s_mx);
            inbox.swap(s_inbox);
        }
        for (const std::string& m : inbox)
        {
            if (m.rfind("WSDESPAWN|", 0) == 0)
            {
                const uint32_t hostRef = static_cast<uint32_t>(std::strtoul(m.c_str() + 10, nullptr, 10));
                auto it = s_client.find(hostRef);
                if (it != s_client.end())
                {
                    RemoveLocal(hostRef, it->second);
                    s_client.erase(it);
                }
                continue;
            }
            uint32_t hostRef = 0;
            ClientSpawn s;
            if (std::sscanf(m.c_str() + 7, "%u|%u|%f|%f|%f|%f|%f|%f|%u|%u", &hostRef, &s.base,
                    &s.pos.x, &s.pos.y, &s.pos.z, &s.rot.x, &s.rot.y, &s.rot.z, &s.ws, &s.cell) != 10 || !hostRef || !s.base)
                continue;
            auto it = s_client.find(hostRef);
            if (it == s_client.end())
                s_client.emplace(hostRef, s);
            else
            {
                s.localRef = it->second.localRef;
                it->second = s;   // the host's current position wins on the next create
            }
        }

        for (auto& kv : s_client)
        {
            ClientSpawn& s = kv.second;
            if (s.localRef)
            {
                if (!RefGone(LookupRef(s.localRef)))
                    continue;
                s.localRef = 0;   // unloaded or removed here: create it again when back in range
            }
            if (!CoSyncPlayerManager::IsLocalLocation(s.ws, s.cell))
                continue;
            TESForm* base = LookupFormByID(s.base);
            if (!base || IsActorBase(base))
                continue;
            TESObjectREFR* local = CoSyncGameAPI::SpawnItemAtPosition(base, s.pos, s.rot);
            if (!local)
                continue;
            s.localRef = local->formID;
            LOG_UPDATED("[WorldSpawn] Entity Created: host ref=0x%08X base=0x%08X -> local ref=0x%08X at (%.0f,%.0f,%.0f)",
                kv.first, s.base, s.localRef, s.pos.x, s.pos.y, s.pos.z);
        }
    }
}

namespace CoSyncWorldSpawnSync
{
    void Install()
    {
        const uintptr_t target = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) + kRVA_ConsolePlaceWorker;
        if (std::memcmp(reinterpret_cast<const void*>(target), kConsolePlacePrologue, sizeof(kConsolePlacePrologue)) != 0)
        {
            LOG_ERROR("[WorldSpawn] console placement worker prologue mismatch (not 1.11.240?) -- host spawns will not replicate");
            return;
        }
        MH_Initialize();   // idempotent (MH_ERROR_ALREADY_INITIALIZED)
        MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&Detour_ConsolePlace),
                                     reinterpret_cast<void**>(&s_origPlace));
        if (st == MH_OK)
            st = MH_EnableHook(reinterpret_cast<void*>(target));
        if (st != MH_OK)
        {
            LOG_ERROR("[WorldSpawn] hook failed: %d", static_cast<int>(st));
            return;
        }
        LOG_INFO("[WorldSpawn] console placement hook installed (sub_1405E0900)");
    }

    void Tick(double now)
    {
        if (!CoSyncTransport::IsInitialized() || !CoSyncNet::IsConnected())
            return;
        if (CoSyncNet::IsHost())
            HostTick(now);
        else
            ClientTick();
    }

    bool IsSpawnMsg(const std::string& msg)
    {
        return msg.rfind("WSPAWN|", 0) == 0 || msg.rfind("WSDESPAWN|", 0) == 0;
    }

    void OnReceive(const std::string& msg, bool isHostRole)
    {
        if (isHostRole)
            return;   // host-originated only
        std::lock_guard<std::mutex> lk(s_mx);
        if (s_inbox.size() < 4096)
            s_inbox.push_back(msg);
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lk(s_mx);
        s_hostNew.clear();
        s_hostTracked.clear();
        s_inbox.clear();
        s_client.clear();   // local copies stay (the world is left as it is)
        s_lastResend = 0.0;
    }
}
