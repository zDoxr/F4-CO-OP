#include "CoSyncPlayerManager.h"
#include "GameAPI.h"
#include "ConsoleLogger.h"
#include "CoSyncWorld.h"
#include "GameReferences.h"
#include "GameRTTI.h"
#include "Relocation.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

extern RelocPtr<PlayerCharacter*> g_player;

CoSyncPlayerManager g_CoSyncPlayerManager;





// ----------------------------------------------------------
// Time helper
// ----------------------------------------------------------
static double NowSeconds()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}




void CoSyncPlayerManager::EnqueueIncoming(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(m_inboxMutex);
    m_inbox.push_back(msg);
}


// ----------------------------------------------------------
// GetOrCreate
// ----------------------------------------------------------
CoSyncPlayer& CoSyncPlayerManager::GetOrCreate(const std::string& username)
{
    auto it = m_players.find(username);
    if (it != m_players.end())
        return it->second;

    m_players.emplace(username, CoSyncPlayer(username));
    return m_players.at(username);
}

// ----------------------------------------------------------
// ProcessIncomingState
// ----------------------------------------------------------
void CoSyncPlayerManager::ProcessIncomingState(const std::string& msg)
{
   


    LOG_INFO("[CoSyncPM] ProcessIncomingState: %s", msg.c_str());




    if (!IsPlayerStateString(msg))
        return;

    LocalPlayerState st;
    if (!DeserializePlayerStateFromString(msg, st))
        return;


    LOG_INFO("[CoSyncPM] Parsed user='%s' pos=(%.2f %.2f %.2f)",
        st.username.c_str(), st.position.x, st.position.y, st.position.z);


    if (st.username.empty())
        return;

    // 1) Get/Create player entry
    CoSyncPlayer& p = GetOrCreate(st.username);

    // 2) Always update the cached state
    p.UpdateFromState(st);

    // 3) If the world isn't ready yet, do NOT spawn/move.
    if (!CoSyncWorld::IsWorldReady())
    {
        LOG_DEBUG("[CoSyncPM] World not ready; queued state for '%s'", st.username.c_str());
        return;
    }

    // 4) World is ready: ensure spawned, then apply
    if (!p.hasSpawned || !p.actorRef)
    {
        // You can pass anchor = local player ref, baseForm = whatever you chose (or resolve from st/baseFormID)
        auto* anchor = static_cast<TESObjectREFR*>(*g_player);
        static TESForm* s_baseForm = nullptr;
        if (!s_baseForm)
            s_baseForm = LookupFormByID(0x0014890C);

        TESForm* baseForm = s_baseForm;


        if (!p.SpawnInWorld(anchor, baseForm))
        {
            LOG_WARN("[CoSyncPM] SpawnInWorld failed for '%s'", st.username.c_str());
            return;
        }
    }
    auto* anchor = static_cast<TESObjectREFR*>(*g_player);
    LOG_INFO("[CoSyncPM] SpawnCheck worldReady=%d hasSpawned=%d actorRef=%p anchor=%p",
        CoSyncWorld::IsWorldReady() ? 1 : 0, p.hasSpawned ? 1 : 0, p.actorRef, anchor);

    TESForm* baseForm = LookupFormByID(0x0014890C);
    LOG_INFO("[CoSyncPM] baseForm LookupFormByID(0014890C) -> %p", baseForm);


    p.ApplyStateToActor();
}

void CoSyncPlayerManager::ProcessInbox()
{
    


    // Move inbox into a local vector quickly (minimal lock time)
    std::vector<std::string> local;
    {
        std::lock_guard<std::mutex> lock(m_inboxMutex);
        local.swap(m_inbox);

        LOG_DEBUG("[CoSyncPM] ProcessInbox drained %zu msgs", local.size());

    }

    // Now process on game thread without holding the lock
    for (auto& msg : local)
    {
        ProcessIncomingState(msg);
    }
}


// ----------------------------------------------------------
// Tick – timeout cleanup
// ----------------------------------------------------------
void CoSyncPlayerManager::Tick()
{
    // 1) Apply queued network updates on the GAME THREAD
    ProcessInbox();

    // 2) Timeout cleanup
    double now = NowSeconds();

    for (auto it = m_players.begin(); it != m_players.end(); )
    {
        CoSyncPlayer& rp = it->second;

        if (now - rp.lastPacketTime > 45.0)
        {
            LOG_WARN("[CoSyncPM] Remote player '%s' timed out", rp.username.c_str());

            if (rp.actorRef)
            {
                rp.actorRef->pos = NiPoint3(0, 0, -10000);
                rp.actorRef = nullptr;
            }

            it = m_players.erase(it);
            continue;
        }

        ++it;
    }
}

