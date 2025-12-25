#include "CoSyncPlayer.h"
#include "ConsoleLogger.h"
#include "CoSyncGameAPI.h"

CoSyncPlayer::CoSyncPlayer(const std::string& name)
    : username(name)
{
}

void CoSyncPlayer::UpdateFromState(const LocalPlayerState& state)
{
    lastState = state;
    lastState.username = username;
    lastPacketTime = state.timestamp;

    LOG_DEBUG("[CoSyncPlayer] UpdateFromState '%s' pos(%.2f %.2f %.2f)",
        username.c_str(),
        state.position.x, state.position.y, state.position.z
    );
}

bool CoSyncPlayer::SpawnInWorld(TESObjectREFR* anchor, TESForm* baseForm)
{
    if (hasSpawned && actorRef)
        return true;

    if (!anchor)
    {
        LOG_ERROR("[CoSyncPlayer] Spawn FAILED for '%s' — anchor NULL", username.c_str());
        return false;
    }

    if (!baseForm)
    {
        LOG_ERROR("[CoSyncPlayer] Spawn FAILED for '%s' — baseForm NULL", username.c_str());
        return false;
    }

    // Use new GameAPI function
    Actor* spawned = CoSyncGameAPI::SpawnRemoteActor(baseForm->formID);
    if (!spawned)
    {
        LOG_ERROR("[CoSyncPlayer] SpawnRemoteActor FAILED for '%s' base=%08X",
            username.c_str(), baseForm->formID);
        return false;
    }

    actorRef = spawned;
    hasSpawned = true;

    CoSyncGameAPI::PositionRemoteActor(actorRef, lastState.position, lastState.rotation);

    LOG_INFO("[CoSyncPlayer] Spawned remote actor=%p for '%s'",
        actorRef, username.c_str());

    return true;
}


void CoSyncPlayer::ApplyStateToActor()
{
    if (!actorRef || !hasSpawned)
        return;

    CoSyncGameAPI::ApplyRemotePlayerStateToActor(actorRef, lastState);
}
