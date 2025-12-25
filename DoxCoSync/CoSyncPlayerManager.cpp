#include "CoSyncPlayerManager.h"
#include "GameAPI.h"
#include "ConsoleLogger.h"

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
    LocalPlayerState st;
    if (!DeserializePlayerStateFromString(msg, st))
        return;

    auto& rp = GetOrCreate(st.username);
    rp.UpdateFromState(st);

    // TEMP: disable spawning while we debug the crash
#if 0
    if (!rp.hasSpawned)
    {
        constexpr UInt32 kBaseFormID = 0x00020749;
        TESForm* base = LookupFormByID(kBaseFormID);

        if (!base)
        {
            LOG_ERROR("[CoSyncPM] Failed to find base NPC %08X", kBaseFormID);
        }
        else
        {
            LOG_INFO("[CoSyncPM] Spawning remote NPC for '%s'...", st.username.c_str());

            TESObjectREFR* anchor = reinterpret_cast<TESObjectREFR*>(*g_player);

            if (!rp.SpawnInWorld(anchor, base))
            {
                LOG_ERROR("[CoSyncPM] SpawnInWorld FAILED for '%s'", st.username.c_str());
            }
        }
    }
#endif

    // Still apply state if we ever add actors back in
    rp.ApplyStateToActor();
}


// ----------------------------------------------------------
// Tick – timeout cleanup
// ----------------------------------------------------------
void CoSyncPlayerManager::Tick()
{
    double now = NowSeconds();

    for (auto it = m_players.begin(); it != m_players.end(); )
    {
        CoSyncPlayer& rp = it->second;

        // timeout handling

        if (now - rp.lastPacketTime > 45.0)
        {
            LOG_WARN("[CoSyncPM] Remote player '%s' timed out", rp.username.c_str());

            if (rp.actorRef)
            {
                // hide underground instead of immediate delete
                rp.actorRef->pos = NiPoint3(0, 0, -10000);
                rp.actorRef = nullptr;
            }

            it = m_players.erase(it);
            continue;
        }

        ++it;
    }
}
