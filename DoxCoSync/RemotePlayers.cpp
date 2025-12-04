
#include "RemotePlayers.h"
#include "GameAPI.h"
#include "ConsoleLogger.h"
#include <unordered_map>

static std::unordered_map<uint64_t, Actor*> g_remoteActors;

namespace RemotePlayers
{
    void Init()
    {
        g_remoteActors.clear();
        LOG_INFO("[RemotePlayers] Init()");
    }

    void Shutdown()
    {
        g_remoteActors.clear();
        LOG_INFO("[RemotePlayers] Shutdown()");
    }

    Actor* EnsureActor(uint64_t remoteID)
    {
        auto it = g_remoteActors.find(remoteID);
        if (it != g_remoteActors.end())
            return it->second;

        // TEMP SPAWN: use a Raider as a placeholder
        UInt32 npcForm = 0x0001A4A7; // Raider01Template

        Actor* spawned = GameAPI::SpawnActor(npcForm);
        if (!spawned)
        {
            LOG_ERROR("[RemotePlayers] FAILED to spawn remote actor for %llu", remoteID);
            return nullptr;
        }

        LOG_INFO("[RemotePlayers] Spawned new remote actor for ID %llu => %p", remoteID, spawned);
        g_remoteActors[remoteID] = spawned;
        return spawned;
    }

    void ApplyRemoteState(uint64_t remoteID, const LocalPlayerState& state)
    {
        Actor* actor = EnsureActor(remoteID);
        if (!actor)
            return;

        GameAPI::ApplyRemotePlayerStateToActor(actor, state);
    }

    void Remove(uint64_t remoteID)
    {
        g_remoteActors.erase(remoteID);
    }
}
