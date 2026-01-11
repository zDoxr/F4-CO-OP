#include "CoSyncGameAPI.h"

#include "ConsoleLogger.h"
#include "Relocation.h"

#include "GameForms.h"
#include "GameObjects.h"
#include "GameReferences.h"
#include "GameUtilities.h"
#include "PapyrusEvents.h"
#include "PapyrusVM.h"

extern RelocPtr<PlayerCharacter*> g_player;
extern RelocPtr<GameVM*>          g_gameVM;
extern RelocAddr<_PlaceAtMe_Native> PlaceAtMe_Native;
RelocAddr<_CallFunctionNoWait> CallFunctionNoWait_Internal(0x01132340);

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static TESObjectREFR* GetPlayerRef()
{
    PlayerCharacter* pc = *g_player;
    return pc ? static_cast<TESObjectREFR*>(pc) : nullptr;
}

// -----------------------------------------------------------------------------
// SpawnRemoteActor — F4MP-aligned
//   - Uses PlaceAtMe native (Papyrus)
//   - Does NOT move/position beyond engine defaults
//   - Caller is responsible for first placement
// -----------------------------------------------------------------------------
Actor* CoSyncGameAPI::SpawnRemoteActor(TESNPC* npcBase)
{
    if (!npcBase)
    {
        LOG_WARN("[CoSyncGameAPI] SpawnRemoteActor: npcBase=null");
        return nullptr;
    }

    TESObjectREFR* anchor = GetPlayerRef();
    if (!anchor)
    {
        LOG_WARN("[CoSyncGameAPI] SpawnRemoteActor: anchor missing");
        return nullptr;
    }

    GameVM* gameVM = *g_gameVM;
    if (!gameVM)
    {
        LOG_ERROR("[CoSyncGameAPI] SpawnRemoteActor: GameVM missing");
        return nullptr;
    }

    VirtualMachine* vm = gameVM->m_virtualMachine;
    if (!vm)
    {
        LOG_ERROR("[CoSyncGameAPI] SpawnRemoteActor: VirtualMachine missing");
        return nullptr;
    }

    TESForm* form = static_cast<TESForm*>(npcBase);

    TESObjectREFR* spawned = (*PlaceAtMe_Native)(
        vm,
        0,          // stackId (unused here)
        &anchor,    // target
        form,       // base form
        1,          // count
        true,       // bForcePersist
        false,      // bInitiallyDisabled
        false       // bDeleteWhenAble
        );

    if (!spawned)
    {
        LOG_ERROR("[CoSyncGameAPI] SpawnRemoteActor: PlaceAtMe failed (base=0x%08X)", npcBase->formID);
        return nullptr;
    }

    if (spawned->formType != Actor::kTypeID)
    {
        LOG_ERROR("[CoSyncGameAPI] SpawnRemoteActor: spawned formType=%u is not Actor (base=0x%08X)",
            (uint32_t)spawned->formType, npcBase->formID);
        return nullptr;
    }

    return static_cast<Actor*>(spawned);
}

// -----------------------------------------------------------------------------
// PositionRemoteActor — F4MP-aligned
//   IMPORTANT:
//     Do NOT write actor->pos/rot directly.
//     Use engine move to keep internal state coherent.
// -----------------------------------------------------------------------------
void CoSyncGameAPI::PositionRemoteActor(
    Actor* actor,
    const NiPoint3& pos,
    const NiPoint3& rot)
{
    if (!actor)
        return;

    UInt32 dummyHandle = 0;

    // F4SE signature is not const-correct; cast is intentional and safe
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
// AI control (Papyrus - no wait)
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




