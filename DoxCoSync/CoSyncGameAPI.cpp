#include "CoSyncGameAPI.h"

// Define globals ONCE
NiPoint3 CoSyncGameAPI::rot = { 0.0f, 0.0f, 0.0f };
NiPoint3 CoSyncGameAPI::pos = { 0.0f, 0.0f, 0.0f };

// Implementations (stubs or real)
Actor* CoSyncGameAPI::SpawnRemoteActor(UInt32 baseFormID)
{
    // TODO: real implementation
    return nullptr;
}

void CoSyncGameAPI::PositionRemoteActor(
    Actor* actor,
    const NiPoint3& pos,
    const NiPoint3& rot
)
{
    // TODO
}

void CoSyncGameAPI::ApplyRemotePlayerStateToActor(
    Actor* actor,
    const LocalPlayerState& state
)
{
    // TODO
}
