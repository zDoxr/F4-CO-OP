#include "CoSyncGameAPI.h"

#include "ConsoleLogger.h"
#include "Relocation.h"

#include "GameForms.h"
#include "GameReferences.h"
#include "GameObjects.h"
#include "GameUtilities.h"
#include "PapyrusEvents.h"
#include "PapyrusVM.h"

extern RelocPtr<PlayerCharacter*> g_player;
extern RelocPtr<GameVM*>          g_gameVM;
extern RelocAddr<_PlaceAtMe_Native> PlaceAtMe_Native;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static Actor* GetPlayerActor()
{
    return *g_player;
}

// -----------------------------------------------------------------------------
// SpawnRemoteActor — F4MP-aligned
// -----------------------------------------------------------------------------
Actor* CoSyncGameAPI::SpawnRemoteActor(TESNPC* npcBase)
{
    if (!npcBase)
    {
        LOG_WARN("[CoSyncGameAPI] SpawnRemoteActor: npcBase=null");
        return nullptr;
    }

    Actor* player = GetPlayerActor();
    if (!player)
    {
        LOG_WARN("[CoSyncGameAPI] SpawnRemoteActor: player missing");
        return nullptr;
    }

    GameVM* gameVM = *g_gameVM;
    if (!gameVM || !gameVM->m_virtualMachine)
    {
        LOG_ERROR("[CoSyncGameAPI] SpawnRemoteActor: GameVM missing");
        return nullptr;
    }

    VirtualMachine* vm = gameVM->m_virtualMachine;
    TESForm* form = static_cast<TESForm*>(npcBase);

    TESObjectREFR* spawned = (*PlaceAtMe_Native)(
        vm,
        0,
        reinterpret_cast<TESObjectREFR**>(&player),
        form,
        1,
        true,   // force persist
        false,  // not disabled
        false
        );

    if (!spawned || spawned->formType != Actor::kTypeID)
    {
        LOG_ERROR(
            "[CoSyncGameAPI] PlaceAtMe failed (base=0x%08X)",
            npcBase->formID
        );
        return nullptr;
    }

    return static_cast<Actor*>(spawned);
}

// -----------------------------------------------------------------------------
// PositionRemoteActor
// -----------------------------------------------------------------------------
void CoSyncGameAPI::PositionRemoteActor(
    Actor* actor,
    const NiPoint3& pos,
    const NiPoint3& rot)
{
    if (!actor)
        return;

    UInt32 dummyHandle = 0;

    MoveRefrToPosition(
        actor,
        &dummyHandle,
        actor->parentCell,
        nullptr,
        const_cast<NiPoint3*>(&pos),
        const_cast<NiPoint3*>(&rot)
    );
}

// -----------------------------------------------------------------------------
// GetActorWorldTransform
// -----------------------------------------------------------------------------
bool CoSyncGameAPI::GetActorWorldTransform(
    Actor* actor,
    NiPoint3& outPos,
    NiPoint3& outRot)
{
    if (!actor)
        return false;

    outPos = NiPoint3(actor->pos.x, actor->pos.y, actor->pos.z);
    outRot = NiPoint3(actor->rot.x, actor->rot.y, actor->rot.z);
    return true;
}

// -----------------------------------------------------------------------------
// AI control (Papyrus no-wait)
// -----------------------------------------------------------------------------
void CoSyncGameAPI::DisableActorAI(Actor* actor)
{
    if (!actor)
        return;

    VMArray<VMVariable> args;
    CallFunctionNoWait(actor, BSFixedString("DisableAI"), args);
}

void CoSyncGameAPI::EnableActorAI(Actor* actor)
{
    if (!actor)
        return;

    VMArray<VMVariable> args;
    CallFunctionNoWait(actor, BSFixedString("EnableAI"), args);
}
