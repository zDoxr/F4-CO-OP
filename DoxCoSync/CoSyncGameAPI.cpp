// CoSyncGameAPI.cpp
#include "CoSyncGameAPI.h"

#include "GameReferences.h"
#include "GameObjects.h"
#include "GameUtilities.h"

#include "PapyrusVM.h"
#include "ConsoleLogger.h"

#include "CoSyncActorValues.h"
#include "LocalPlayerState.h"

// F4SE globals you already define elsewhere
extern RelocPtr<PlayerCharacter*> g_player;

// ============================================================
// CoSyncGameAPI globals
// ============================================================
NiPoint3 CoSyncGameAPI::rot = { 0.0f, 0.0f, 0.0f };
NiPoint3 CoSyncGameAPI::pos = { 0.0f, 0.0f, 0.0f };

// ============================================================
// Spawn a remote actor using Papyrus native PlaceAtMe
// ============================================================
Actor* CoSyncGameAPI::SpawnRemoteActor(UInt32 baseFormID)
{
    // Ensure GameVM / VirtualMachine is available
    if (!*g_gameVM || !(*g_gameVM)->m_virtualMachine)
    {
        LOG_ERROR("[CoSyncGameAPI] GameVM not available");
        return nullptr;
    }

    VirtualMachine* vm = (*g_gameVM)->m_virtualMachine;

    // Resolve the base form
    TESForm* form = LookupFormByID(baseFormID);
    if (!form)
    {
        LOG_ERROR("[CoSyncGameAPI] Base form %08X not found", baseFormID);
        return nullptr;
    }

    // Get local player
    PlayerCharacter* player = *g_player;
    if (!player)
    {
        LOG_ERROR("[CoSyncGameAPI] PlayerCharacter is null");
        return nullptr;
    }

    TESObjectREFR* playerRef = static_cast<TESObjectREFR*>(player);

    // Spawn
    TESObjectREFR* spawned = PlaceAtMe_Native(
        vm,
        0,              // stackId
        &playerRef,     // target
        form,
        1,              // count
        true,           // force persist
        false,          // initially disabled
        false           // delete when able
    );

    if (!spawned)
    {
        LOG_ERROR("[CoSyncGameAPI] PlaceAtMe_Native failed for %08X", baseFormID);
        return nullptr;
    }

    // Convert to Actor*
    if (spawned->formType != Actor::kTypeID)
    {
        LOG_ERROR("[CoSyncGameAPI] Spawned form is not an Actor (%u)", spawned->formType);
        return nullptr;
    }

    return static_cast<Actor*>(spawned);
}



// ============================================================
// Move / rotate an actor using engine-native MoveRefrToPosition
// ============================================================
void CoSyncGameAPI::PositionRemoteActor(
    Actor* actor,
    const NiPoint3& inPos,
    const NiPoint3& inRot
)
{
    if (!actor)
        return;

    // Cache into globals (some systems prefer stable addresses)
    CoSyncGameAPI::pos = inPos;
    CoSyncGameAPI::rot = inRot;

    // IMPORTANT:
    // MoveRefrToPosition expects a handle pointer OR nullptr.
    // Do NOT pass refcount or fake values.
    MoveRefrToPosition(
        actor,
        nullptr,                // handle (nullptr is safe here)
        actor->parentCell,
        nullptr,                // worldspace (engine resolves)
        &CoSyncGameAPI::pos,
        &CoSyncGameAPI::rot
    );
}

// ============================================================
// Apply replicated state (pos/rot + basic actor values)
// ============================================================
void CoSyncGameAPI::ApplyRemotePlayerStateToActor(
    Actor* actor,
    const LocalPlayerState& state
)
{
    if (!actor)
        return;

    // Position + rotation first
    PositionRemoteActor(actor, state.position, state.rotation);

    // Resolve AVs once
    static ActorValueInfo* avHealth = GetActorValueInfoByName("Health");
    static ActorValueInfo* avAP = GetActorValueInfoByName("ActionPoints");

    // Apply values (if found)
    if (avHealth)
        Actor_SetActorValue(actor, avHealth, state.health);

    if (avAP)
        Actor_SetActorValue(actor, avAP, state.ap);
}
