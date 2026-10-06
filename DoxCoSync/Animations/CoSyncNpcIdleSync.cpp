#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncNpcIdleSync.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncGameAPI.h"
#include "ConsoleLogger.h"
#include "GameReferences.h"
#include "GameForms.h"
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
    constexpr uintptr_t kRVA_SpecialIdle = 0x00CFB0D0;
    constexpr uint8_t   kSpecialIdlePrologue[16] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x44, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41 };
    using SpecialIdle_t = char (*)(void* process, Actor* actor, uint32_t type, TESForm* idle, char flag, TESObjectREFR* target);
    SpecialIdle_t s_orig = nullptr;

    thread_local bool t_replaying = false;

    struct Captured { uint32_t eid, type, idle, target; };
    struct Pending  { uint32_t eid, type, idle, target; };

    std::mutex s_mx;
    std::unordered_map<uint32_t, uint32_t> s_simulated;   // actor formID -> entity (NPCs simulated here)
    std::vector<Captured> s_captured;                     // from the hook
    std::vector<Pending>  s_inbox;                        // from the network

    // Tick thread only.
    struct LastSent { uint32_t idle = 0; double at = 0.0; };
    std::unordered_map<uint32_t, LastSent> s_lastSent;
    double s_nextMapRefresh = 0.0;

    __declspec(noinline) uint32_t SafeFormID(const void* form)
    {
        __try { return form ? static_cast<const TESForm*>(form)->formID : 0u; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
    }

    char Detour_SpecialIdle(void* process, Actor* actor, uint32_t type, TESForm* idle, char flag, TESObjectREFR* target)
    {
        const char ok = s_orig(process, actor, type, idle, flag, target);
        if (!ok || t_replaying || !idle)
            return ok;
        const uint32_t actorID = SafeFormID(actor);
        if (!actorID)
            return ok;
        std::lock_guard<std::mutex> lk(s_mx);
        auto it = s_simulated.find(actorID);
        if (it != s_simulated.end() && s_captured.size() < 256)
            s_captured.push_back({ it->second, type, SafeFormID(idle), SafeFormID(target) });
        return ok;
    }

    __declspec(noinline) char ReplayImpl(Actor* actor, uint32_t type, TESForm* idle, TESObjectREFR* target)
    {
        __try
        {
            void* process = actor->middleProcess;
            if (!process || !actor->GetObjectRootNode())
                return 0;
            t_replaying = true;
            const char r = s_orig(process, actor, type, idle, 1, target);
            t_replaying = false;
            return r;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { t_replaying = false; return 0; }
    }

    void RefreshSimulatedMap()
    {
        std::unordered_map<uint32_t, uint32_t> m;
        for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
        {
            CoSyncPlayer* p = kv.second.get();
            if (!p || p->isLocalPlayer || p->createType != CoSyncEntityType::NPC || !p->actorRef || !p->hasSpawned)
                continue;
            if (p->m_hasRealTransform)
                continue;   // a puppet here: its owner reports its idles
            m[reinterpret_cast<TESForm*>(p->actorRef)->formID] = p->entityID;
        }
        std::lock_guard<std::mutex> lk(s_mx);
        s_simulated.swap(m);
    }
}

namespace CoSyncNpcIdleSync
{
    void Install()
    {
        const uintptr_t target = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) + kRVA_SpecialIdle;
        if (std::memcmp(reinterpret_cast<const void*>(target), kSpecialIdlePrologue, sizeof(kSpecialIdlePrologue)) != 0)
        {
            LOG_ERROR("[NpcIdleSync] special-idle prologue mismatch (not 1.11.240?) -- NPC idles will not replicate");
            return;
        }
        MH_Initialize();
        MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&Detour_SpecialIdle),
                                     reinterpret_cast<void**>(&s_orig));
        if (st == MH_OK)
            st = MH_EnableHook(reinterpret_cast<void*>(target));
        if (st != MH_OK)
        {
            LOG_ERROR("[NpcIdleSync] hook failed: %d", static_cast<int>(st));
            return;
        }
        LOG_INFO("[NpcIdleSync] special-idle hook installed (sub_140CFB0D0)");
    }

    void Tick(double now)
    {
        if (!s_orig || !CoSyncTransport::IsInitialized() || !CoSyncNet::IsConnected())
            return;
        if (now >= s_nextMapRefresh)
        {
            s_nextMapRefresh = now + 0.5;
            RefreshSimulatedMap();
        }

        std::vector<Captured> cap;
        std::vector<Pending> in;
        {
            std::lock_guard<std::mutex> lk(s_mx);
            cap.swap(s_captured);
            in.swap(s_inbox);
        }

        for (const Captured& c : cap)
        {
            LastSent& ls = s_lastSent[c.eid];
            if (ls.idle == c.idle && now - ls.at < 2.0)
                continue;   // the same looping idle restarted: already sent
            ls.idle = c.idle;
            ls.at = now;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "NIDL|%u|%u|%u|%u", c.eid, c.type, c.idle, c.target);
            CoSyncTransport::Send(buf);
            LOG_UPDATED("[NpcSync] entity=%u Animation/Action State: idle 0x%08X (type %u, target 0x%08X) -- sent",
                c.eid, c.idle, c.type, c.target);
        }

        for (const Pending& p : in)
        {
            CoSyncPlayer* pl = g_CoSyncPlayerManager.GetPlayerByEntityID(p.eid);
            if (!pl || !pl->actorRef || !pl->hasSpawned || pl->createType != CoSyncEntityType::NPC || !pl->m_hasRealTransform)
                continue;   // not a puppet here
            TESForm* idle = LookupFormByID(p.idle);
            if (!idle || idle->formType != kFormType_IDLE)
                continue;
            TESForm* tf = p.target ? LookupFormByID(p.target) : nullptr;
            TESObjectREFR* target = (tf && (tf->formType == kFormType_REFR || tf->formType == kFormType_ACHR))
                ? reinterpret_cast<TESObjectREFR*>(tf) : nullptr;
            const char ok = ReplayImpl(pl->actorRef, p.type, idle, target);
            LOG_UPDATED("[NpcSync] entity=%u Animation/Action State: idle 0x%08X replayed on puppet (%s)",
                p.eid, p.idle, ok ? "started" : "engine refused -- actor state does not allow it now");
        }
    }

    bool IsIdleMsg(const std::string& msg)
    {
        return msg.rfind("NIDL|", 0) == 0;
    }

    void OnReceive(const std::string& msg, bool isHostRole)
    {
        Pending p{};
        if (std::sscanf(msg.c_str() + 5, "%u|%u|%u|%u", &p.eid, &p.type, &p.idle, &p.target) != 4 || !p.eid || !p.idle)
            return;
        if (isHostRole)
            CoSyncTransport::Send(msg);   // relay a client-simulated NPC's idle
        std::lock_guard<std::mutex> lk(s_mx);
        if (s_inbox.size() < 256)
            s_inbox.push_back(p);
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lk(s_mx);
        s_simulated.clear();
        s_captured.clear();
        s_inbox.clear();
        s_lastSent.clear();
    }
}
