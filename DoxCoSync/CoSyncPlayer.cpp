#include "CoSyncPlayer.h"

#include "ConsoleLogger.h"
#include "CoSyncGameAPI.h"
#include "GameForms.h"

extern RelocPtr<PlayerCharacter*> g_player;

// -----------------------------------------------------------------------------

static NiPoint3 VoidPos()
{
    return NiPoint3{ 0.f, 0.f, -20000.f };
}

static NiPoint3 ZeroRot()
{
    return NiPoint3{ 0.f, 0.f, 0.f };
}

static TESObjectREFR* GetAnchor()
{
    PlayerCharacter* pc = *g_player;
    return pc ? static_cast<TESObjectREFR*>(pc) : nullptr;
}

// -----------------------------------------------------------------------------

CoSyncPlayer::CoSyncPlayer(const std::string& name)
    : username(name)
{
    isLocalPlayer = false; // ALWAYS remote
}

// -----------------------------------------------------------------------------

bool CoSyncPlayer::SpawnInWorld(TESObjectREFR* anchor, TESForm*)
{
    if (!anchor)
        anchor = GetAnchor();

    if (!anchor)
    {
        LOG_ERROR("[CoSyncPlayer] No anchor for spawn entityID=%u", entityID);
        return false;
    }

    if (hasSpawned)
        return true;

    LOG_INFO(
        "[CoSyncPlayer] Spawn START entityID=%u",
        entityID
    );

    Actor* actor = CoSyncGameAPI::SpawnRemoteActor(0);
    if (!actor)
    {
        LOG_ERROR("[CoSyncPlayer] SpawnRemoteActor FAILED entityID=%u", entityID);
        return false;
    }

    actorRef = actor;

    // F4MP CRITICAL STEP
    CoSyncGameAPI::PositionRemoteActor(actorRef, VoidPos(), ZeroRot());

    hasSpawned = true;

    LOG_INFO(
        "[CoSyncPlayer] Spawn END entityID=%u actor=%p",
        entityID, actorRef
    );

    return true;
}

// -----------------------------------------------------------------------------

void CoSyncPlayer::ApplyPendingTransformIfAny()
{
    if (!hasSpawned || !actorRef || !hasPendingTransform)
        return;

    CoSyncGameAPI::PositionRemoteActor(
        actorRef,
        pendingPos,
        pendingRot
    );

    hasPendingTransform = false;
}

// -----------------------------------------------------------------------------

void CoSyncPlayer::UpdateFromState(const LocalPlayerState& state)
{
    lastState = state;

    pendingPos = state.position;
    pendingRot = state.rotation;
    pendingVel = state.velocity;

    hasPendingTransform = true;
}
