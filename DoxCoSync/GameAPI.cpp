#include "GameAPI.h"
#include "LocalPlayerState.h"
#include "ConsoleLogger.h"

#include "f4se/GameRTTI.h"
#include "f4se/GameObjects.h"
#include "f4se/GameReferences.h"
#include "f4se_common/Relocation.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// FO4 Player Reference
extern RelocPtr<PlayerCharacter*> g_player;

// NOTE: Signature chosen to match Papyrus native PlaceAtMe for FO4.
// If this turns out to be wrong, we will see it in logs / crash point.
typedef TESObjectREFR* (*_PlaceAtMe_Native)(
    VirtualMachine* vm,
    UInt32 stackId,
    TESObjectREFR** target,
    TESForm* form,
    SInt32 count,
    bool bForcePersist,
    bool bInitiallyDisabled,
    bool bDeleteWhenAble
    );

// Provided by GameAPI_Relocs.cpp
extern RelocAddr<_PlaceAtMe_Native> PlaceAtMe_Native;

namespace GameAPI
{
    // ============================================================
    // SpawnRemoteActor
    // ============================================================
    Actor* SpawnRemoteActor(UInt32 baseFormID)
    {
        LOG_INFO("[GameAPI] SpawnRemoteActor begin baseFormID=%08X", baseFormID);

        TESForm* form = LookupFormByID(baseFormID);
        if (!form)
        {
            LOG_ERROR("[GameAPI] LookupFormByID(%08X) returned NULL", baseFormID);
            return nullptr;
        }

        // Make sure this is some kind of actor base
        TESActorBase* actorBase = DYNAMIC_CAST(form, TESForm, TESActorBase);
        if (!actorBase)
        {
            LOG_ERROR("[GameAPI] Form %08X is NOT TESActorBase (cannot spawn as Actor)", baseFormID);
            return nullptr;
        }

        // More specific check for TESNPC (optional; creatures may still be valid)
        TESNPC* npc = DYNAMIC_CAST(form, TESForm, TESNPC);
        if (npc)
        {
            LOG_DEBUG("[GameAPI] Base form %08X is TESNPC (ok to spawn)", baseFormID);
        }
        else
        {
            LOG_DEBUG("[GameAPI] Base form %08X is TESActorBase but NOT TESNPC (creature or other actor type)", baseFormID);
        }

        PlayerCharacter* player = *g_player;
        if (!player)
        {
            LOG_ERROR("[GameAPI] Player not ready (g_player null)");
            return nullptr;
        }

        if (!PlaceAtMe_Native)
        {
            LOG_ERROR("[GameAPI] PlaceAtMe_Native reloc is NULL – cannot spawn");
            return nullptr;
        }

        LOG_DEBUG("[GameAPI] Using PlaceAtMe_Native=%p targetREFR=%p",
            (void*)PlaceAtMe_Native.GetUIntPtr(), player);

        VirtualMachine* fakeVM = nullptr;
        UInt32 fakeStackID = 0;
        TESObjectREFR* target = player;

        TESObjectREFR* spawned = nullptr;

        // Call the native – if the signature is wrong, this is where it will blow up.
        spawned = PlaceAtMe_Native(
            fakeVM,
            fakeStackID,
            &target,
            form,
            1,              // count
            false,          // forcePersist
            true,           // initiallyDisabled so AI does not run
            false           // deleteWhenAble
        );

        if (!spawned)
        {
            LOG_ERROR("[GameAPI] PlaceAtMe_Native returned NULL for form %08X", baseFormID);
            return nullptr;
        }

        Actor* actor = DYNAMIC_CAST(spawned, TESObjectREFR, Actor);
        if (!actor)
        {
            LOG_ERROR("[GameAPI] Spawned ref %p is NOT an Actor (baseFormID=%08X)", spawned, baseFormID);
            return nullptr;
        }

        LOG_INFO("[GameAPI] SpawnRemoteActor OK actor=%p baseFormID=%08X", actor, baseFormID);
        return actor;
    }

    // ============================================================
    // PositionRemoteActor – direct position + rotation write
    // ============================================================
    void PositionRemoteActor(Actor* actor, const NiPoint3& pos, const NiPoint3& rot)
    {
        if (!actor)
            return;

        actor->pos = pos;
        actor->rot = rot;
    }

    // ============================================================
    // ApplyRemotePlayerStateToActor
    // ============================================================
    void ApplyRemotePlayerStateToActor(Actor* actor, const LocalPlayerState& state)
    {
        if (!actor)
            return;

        actor->pos = state.position;
        actor->rot = state.rotation;
    }

    // ============================================================
    // NativeMoveTo – simple pos move
    // ============================================================
    void NativeMoveTo(Actor* actor, const NiPoint3& pos)
    {
        if (!actor)
            return;

        actor->pos = pos;
    }

    // ============================================================
    // SetActorTransform (compat)
    // ============================================================
    void SetActorTransform(Actor* actor, const NiPoint3& pos, const NiPoint3& rot)
    {
        PositionRemoteActor(actor, pos, rot);
    }
}
