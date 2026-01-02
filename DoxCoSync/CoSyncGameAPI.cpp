#include "CoSyncGameAPI.h"

#include "ConsoleLogger.h"
#include "Relocation.h"

#include "GameForms.h"
#include "GameObjects.h"
#include "GameReferences.h"
#include "PapyrusVM.h"

extern RelocPtr<PlayerCharacter*> g_player;
extern RelocPtr<GameVM*>          g_gameVM;

// -----------------------------------------------------------------------------
// F4MP RULE:
// Proxy actors MUST be spawned from a TESNPC base.
// Pick ONE known-good NPC and reuse it for all remote players.
// -----------------------------------------------------------------------------

static constexpr UInt32 kProxyNPCBaseFormID = 00000007;

// -----------------------------------------------------------------------------

static TESObjectREFR* GetPlayerRef()
{
    PlayerCharacter* pc = *g_player;
    return pc ? static_cast<TESObjectREFR*>(pc) : nullptr;
}

// -----------------------------------------------------------------------------

Actor* CoSyncGameAPI::SpawnRemoteActor(UInt32 /*baseFormID*/)
{
    TESObjectREFR* anchor = GetPlayerRef();
    if (!anchor)
    {
        LOG_ERROR("[Spawn] No player anchor");
        return nullptr;
    }

    TESForm* baseForm = LookupFormByID(kProxyNPCBaseFormID);
    if (!baseForm)
    {
        LOG_ERROR(
            "[Spawn] Proxy NPC base missing: 0x%08X",
            kProxyNPCBaseFormID
        );
        return nullptr;
    }

    LOG_INFO(
        "[Spawn] PlaceObjectAtMe proxy NPC base=0x%08X anchor=%p",
        kProxyNPCBaseFormID,
        anchor
    );

    TESObjectREFR* spawned =
        CALL_MEMBER_FN(anchor, PlaceObjectAtMe)(
            baseForm,
            1,
            false, // forcePersist
            true,  // initiallyDisabled  <<< REQUIRED
            false
            );

    if (!spawned)
    {
        LOG_ERROR("[Spawn] PlaceObjectAtMe FAILED");
        return nullptr;
    }

    if (spawned->formType != kFormType_ACHR)
    {
        LOG_ERROR(
            "[Spawn] Spawned ref not Actor (formType=%u)",
            spawned->formType
        );
        return nullptr;
    }

    return static_cast<Actor*>(spawned);
}




// -----------------------------------------------------------------------------

void CoSyncGameAPI::PositionRemoteActor(
    Actor* actor,
    const NiPoint3& pos,
    const NiPoint3& rot)
{
    if (!actor)
        return;

    TESObjectREFR* refr = static_cast<TESObjectREFR*>(actor);

    TESObjectCELL* cell = refr->parentCell;
    TESWorldSpace* ws = CALL_MEMBER_FN(refr, GetWorldspace)();

    if (!cell || !ws)
    {
        TESObjectREFR* anchor = GetPlayerRef();
        if (!anchor)
            return;

        cell = anchor->parentCell;
        ws = CALL_MEMBER_FN(anchor, GetWorldspace)();
    }

    if (!cell || !ws)
        return;

    NiPoint3 p = pos;
    NiPoint3 r = rot;

    MoveRefrToPosition(
        refr,
        nullptr,
        cell,
        ws,
        &p,
        &r
    );
}
