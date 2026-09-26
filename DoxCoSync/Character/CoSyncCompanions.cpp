#include "CoSyncCompanions.h"

#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncGameAPI.h"
#include "CoSyncProfiles.h"
#include "CoSyncProxyLooks.h"
#include "CoSyncNpcWorldSync.h"
#include "CoSyncEntityRegistry.h"
#include "CoSyncCompanionSync.h"
#include "CoSyncVMFormArg.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"

#include "GameForms.h"
#include "GameObjects.h"
#include "GameReferences.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>
#include <vector>

extern RelocAddr<_LookupFormByID> LookupFormByID;
extern CoSyncEntityRegistry       g_CoSyncEntities;
extern PlayerCharacter* CoSyncGetPlayer();

namespace
{
    // Fallout4.esm
    constexpr uint32_t kFollowersQuestID = 0x000289E4;   // "Followers" (FollowersScript)
    constexpr uint32_t kDogmeatRaceID    = 0x0001D698;   // DogmeatRace

    // Engine AI process lists (same RVA/offsets CoSyncNpcWorldSync walks):
    // high and middle-high actor handle arrays, data + count.
    RelocPtr<uintptr_t> g_processLists(0x30E71B8);
    constexpr uintptr_t kPL_Lists[2][2] = { { 0x40, 0x50 }, { 0x70, 0x80 } };

    struct Note { uint32_t ref; bool recruited; };
    std::mutex        s_notesMx;
    std::vector<Note> s_notes;

    struct Mine
    {
        uint32_t ref = 0;
        uint32_t base = 0;
        bool     made = false;       // SetCompanion issued
        bool     announced = false;  // COMPOWN sent
        double   readyAt = 0.0;
    };
    std::vector<Mine>            s_mine;        // client: its own companion copies
    std::unordered_set<uint32_t> s_inherited;   // client: host followers that came with the save
    std::vector<uint32_t>        s_toSpawn;     // client: saved companions still to bring
    double                       s_spawnAt = 0.0;
    bool                         s_characterApplied = false;

    double NowSeconds() { return static_cast<double>(GetTickCount64()) / 1000.0; }

    Actor* ActorByRef(uint32_t ref)
    {
        TESForm* f = ref ? LookupFormByID(ref) : nullptr;
        return (f && f->formType == kFormType_ACHR) ? static_cast<Actor*>(f) : nullptr;
    }

    Mine* FindMine(uint32_t ref)
    {
        for (Mine& m : s_mine)
            if (m.ref == ref)
                return &m;
        return nullptr;
    }

    bool IsDogmeat(Actor* a)
    {
        TESNPC* npc = (a && a->baseForm && a->baseForm->formType == kFormType_NPC_) ? static_cast<TESNPC*>(a->baseForm) : nullptr;
        return npc && npc->race.race && npc->race.race->formID == kDogmeatRaceID;
    }

    void PapyrusOnRef(TESForm* target, const char* fn, VMArray<VMVariable>& args)
    {
        const BSFixedString name(fn);
        CallFunctionNoWait(target, name, args);
    }

    // The vanilla way to make someone YOUR companion: the Followers quest.
    void MakeCompanion(Actor* a)
    {
        TESForm* quest = LookupFormByID(kFollowersQuestID);
        if (!quest || quest->formType != kFormType_QUST)
        {
            LOG_WARN("[Companions] Followers quest 0x%08X not found", kFollowersQuestID);
            return;
        }
        VMArray<VMVariable> args;
        VMVariable vA; CoSyncVM::SetFormArg(vA, a); args.Push(&vA);
        if (IsDogmeat(a))
        {
            PapyrusOnRef(quest, "SetDogmeatCompanion", args);
        }
        else
        {
            bool bTrue = true, bFalse = false;
            VMVariable vSet;  vSet.Set(&bTrue);   args.Push(&vSet);    // SetCompanion
            VMVariable vFill; vFill.Set(&bTrue);  args.Push(&vFill);   // FillCompanionAlias
            VMVariable vMsg;  vMsg.Set(&bFalse);  args.Push(&vMsg);    // SuppressDismissMessage
            PapyrusOnRef(quest, "SetCompanion", args);
        }
    }

    void SetTeammate(Actor* a, bool on)
    {
        VMArray<VMVariable> args;
        bool v = on, favor = on;
        VMVariable a1; a1.Set(&v);     args.Push(&a1);
        VMVariable a2; a2.Set(&favor); args.Push(&a2);
        PapyrusOnRef(a, "SetPlayerTeammate", args);
    }

    void DeleteRef(Actor* a)
    {
        CoSyncGameAPI::DisableWorldActor(a);
        VMArray<VMVariable> args;
        PapyrusOnRef(a, "Delete", args);
    }

    // A follower that came with the HOST's save: not ours. Drop it locally;
    // the host's stream shows the real one following the host.
    void HandBack(Actor* a)
    {
        if (!a || s_inherited.count(a->formID))
            return;
        s_inherited.insert(a->formID);
        SetTeammate(a, false);
        CoSyncGameAPI::DisableWorldActor(a);
        LOG_INFO("[Companions] Host's companion ref=0x%08X base=0x%08X handed back (it follows the host)",
            a->formID, a->baseForm ? a->baseForm->formID : 0);
    }

    bool IsCoSyncBase(uint32_t base)
    {
        return base == 0x00000007u || base == 0x00020749u || (base & 0x00FFFFFFu) == 0x00003599u ||
               CoSyncProxyLooks::IsProxyBaseFormID(base);
    }

    // Every current teammate in the loaded area.
    __declspec(noinline) void CollectTeammates(std::vector<Actor*>& out)
    {
        const uintptr_t pl = *g_processLists;
        if (!pl)
            return;
        PlayerCharacter* pc = CoSyncGetPlayer();
        for (const auto& list : kPL_Lists)
        {
            uint32_t* handles = *reinterpret_cast<uint32_t* const*>(pl + list[0]);
            const uint32_t n  = *reinterpret_cast<const uint32_t*>(pl + list[1]);
            if (!handles || n == 0 || n >= 4096)
                continue;
            for (uint32_t i = 0; i < n; ++i)
            {
                NiPointer<TESObjectREFR> ref;
                if (!LookupREFRByHandle(handles[i], ref) || !ref.m_pObject)
                    continue;
                TESObjectREFR* r = ref.m_pObject;
                if (r->formType != kFormType_ACHR || !r->baseForm || r == reinterpret_cast<TESObjectREFR*>(pc))
                    continue;
                Actor* a = static_cast<Actor*>(r);
                if (a->IsPlayerTeammate() && std::find(out.begin(), out.end(), a) == out.end())
                    out.push_back(a);
            }
        }
    }

    bool SafeCollectTeammates(std::vector<Actor*>& out)
    {
        __try { CollectTeammates(out); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void SpawnMine(uint32_t base, double now)
    {
        TESForm* f = LookupFormByID(base);
        if (!f || f->formType != kFormType_NPC_ || IsCoSyncBase(base))
            return;
        Actor* a = CoSyncGameAPI::SpawnRemoteActor(static_cast<TESNPC*>(f), false);
        if (!a)
        {
            LOG_WARN("[Companions] Could not spawn companion base=0x%08X", base);
            return;
        }
        Mine m;
        m.ref = a->formID;
        m.base = base;
        m.readyAt = now + 1.0;   // let its 3D and AI come up before the Followers quest takes it
        s_mine.push_back(m);
        LOG_INFO("[Companions] Spawned my companion base=0x%08X ref=0x%08X", base, a->formID);
    }

    void ClientHandleNote(const Note& n, double now)
    {
        Actor* a = ActorByRef(n.ref);
        if (n.recruited)
        {
            if (!a || !a->baseForm || FindMine(n.ref) || s_inherited.count(n.ref))
                return;
            const uint32_t base = a->baseForm->formID;
            if (IsCoSyncBase(base))
                return;
            if (!s_characterApplied)
            {
                HandBack(a);   // followed the save's player, i.e. the host
                return;
            }
            // The client recruited someone: it gets ITS OWN copy, which takes
            // over the Followers quest (the recruited actor is dismissed from it
            // and stays in the shared world for everyone else).
            LOG_INFO("[Companions] Recruited base=0x%08X -- spawning my own copy", base);
            SpawnMine(base, now);
            return;
        }
        // Dismissed.
        auto it = std::find_if(s_mine.begin(), s_mine.end(), [&n](const Mine& m) { return m.ref == n.ref; });
        if (it == s_mine.end())
            return;
        if (a)
        {
            const uint32_t eid = g_CoSyncEntities.GetEntityID(a);
            if (eid && CoSyncTransport::IsInitialized())
                CoSyncTransport::Send(CoSyncCompanionSync::SerializeDismiss(eid));
            DeleteRef(a);
        }
        LOG_INFO("[Companions] Dismissed my companion base=0x%08X", it->base);
        s_mine.erase(it);
    }
}

namespace CoSyncCompanions
{
    void NotifyRecruited(Actor* actor)
    {
        if (!actor) return;
        std::lock_guard<std::mutex> lk(s_notesMx);
        s_notes.push_back({ actor->formID, true });
    }

    void NotifyDismissed(Actor* actor)
    {
        if (!actor) return;
        std::lock_guard<std::mutex> lk(s_notesMx);
        s_notes.push_back({ actor->formID, false });
    }

    void ClientOnCharacterApplied(const std::vector<uint32_t>& savedBases)
    {
        LOG_REQUIREMENT("CoSyncCompanions::ClientOnCharacterApplied", "Client, main thread, after its character was applied.");
        s_characterApplied = true;

        std::vector<Actor*> mates;
        if (SafeCollectTeammates(mates))
            for (Actor* a : mates)
                if (!FindMine(a->formID))
                    HandBack(a);

        s_toSpawn.clear();
        for (uint32_t b : savedBases)
            if (b && std::find(s_toSpawn.begin(), s_toSpawn.end(), b) == s_toSpawn.end())
                s_toSpawn.push_back(b);
        s_spawnAt = NowSeconds() + 3.0;
        if (!s_toSpawn.empty())
            LOG_INFO("[Companions] Bringing %zu saved companion(s)", s_toSpawn.size());
    }

    std::vector<uint32_t> GetMyCompanionBases()
    {
        std::vector<uint32_t> out;
        for (const Mine& m : s_mine)
            if (std::find(out.begin(), out.end(), m.base) == out.end())
                out.push_back(m.base);
        return out;
    }

    bool IsCompanionMessage(const std::string& msg)
    {
        return msg.rfind("COMPOWN|", 0) == 0;
    }

    void OnReceive(const std::string& msg, bool isHostRole)
    {
        LOG_REQUIREMENT("CoSyncCompanions::OnReceive", "Main thread (transport inbox). Host only acts.");
        if (!isHostRole)
            return;
        uint32_t base = 0, ref = 0, owner = 0;
        NiPoint3 pos{};
        const char* s = msg.c_str() + 8;
        char* e = nullptr;
        base = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return; s = e + 1;
        ref  = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return; s = e + 1;
        if (std::sscanf(s, "%f,%f,%f", &pos.x, &pos.y, &pos.z) != 3) return;
        const char* bar = std::strchr(s, '|');
        if (!bar) return;
        owner = static_cast<uint32_t>(std::strtoul(bar + 1, nullptr, 10));
        if (IsCoSyncBase(base))
            return;
        CoSyncNpcWorldSync::HostRegisterClientOwnedActor(base, ref, pos, owner);
    }

    void Tick(double /*now*/)
    {
        const double now = NowSeconds();
        std::vector<Note> notes;
        {
            std::lock_guard<std::mutex> lk(s_notesMx);
            notes.swap(s_notes);
        }

        // The host's companions are its real world actors (synced by
        // CoSyncNpcWorldSync); nothing to do for its own notifications.
        const bool client = !CoSyncNet::IsHost() && CoSyncTransport::IsInitialized();
        if (!client)
            return;

        for (const Note& n : notes)
            ClientHandleNote(n, now);

        if (!s_toSpawn.empty() && now >= s_spawnAt && s_characterApplied && !CoSyncProfiles::IsEditorBusy())
        {
            for (uint32_t b : s_toSpawn)
                SpawnMine(b, now);
            s_toSpawn.clear();
        }

        const uint32_t myEid = CoSyncNet::GetMyEntityID();
        for (auto it = s_mine.begin(); it != s_mine.end();)
        {
            Actor* a = ActorByRef(it->ref);
            if (!a)
            {
                it = s_mine.erase(it);   // gone (killed and cleaned up, cell unloaded for good)
                continue;
            }
            if (!it->made && now >= it->readyAt)
            {
                MakeCompanion(a);
                it->made = true;
            }
            if (it->made && !it->announced && myEid && CoSyncNet::IsConnected())
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "COMPOWN|%u|%u|%.2f,%.2f,%.2f|%u",
                    it->base, it->ref, a->pos.x, a->pos.y, a->pos.z, myEid);
                CoSyncTransport::Send(buf);
                it->announced = true;
            }
            ++it;
        }
    }

    void Reset()
    {
        {
            std::lock_guard<std::mutex> lk(s_notesMx);
            s_notes.clear();
        }
        s_mine.clear();
        s_inherited.clear();
        s_toSpawn.clear();
        s_spawnAt = 0.0;
        s_characterApplied = false;
    }
}
