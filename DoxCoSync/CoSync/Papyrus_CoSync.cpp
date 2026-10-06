#include "CoSyncProxyLooks.h"
#include "CoSyncCompanions.h"
#include "Papyrus_CoSync.h"


#include "PapyrusVM.h"
#include "PapyrusNativeFunctions.h"
#include "PapyrusArgs.h"
#include "GameForms.h"
#include "GameReferences.h"
#include "GameObjects.h"
#include "GameFormComponents.h"
#include "ConsoleLogger.h"
#include "Logger.h"
#include "CoSyncNet.h"
#include "GNS_Session.h"
#include "CoSyncLocalPlayer.h"
#include "CoSyncWorkshopSync.h"
#include "CoSyncWorldSync.h"
#include "CoSyncQuestSync.h"
#include "CoSyncPowerArmorSync.h"
#include "CoSyncMapSync.h"
#include "CoSyncConsumableSync.h"
#include "CoSyncTransport.h"
#include "CoSyncEntityRegistry.h"
#include "CoSyncModEventBus.h"
#include "CoSyncTimeSync.h"
#include "CoSyncCompanionSync.h"
#include "CoSyncPapyrusHelper.h"
#include "GameRTTI.h"

#include <cmath>
#include <cstdio>
#include "CoSyncThreadDiag.h"

using PapyrusActionType = TESForm*;

static SInt32 CoSync_GetClientInstanceID(StaticFunctionTag*)
{

    return 0;

}


static void CoSync_SetClient(StaticFunctionTag*, SInt32 instanceID)
{
    Console_Print("[CoSync] SetClient(%d) (stub)", instanceID);
}

static bool CoSync_IsConnected(StaticFunctionTag*)
{
    const bool connected = CoSyncNet::IsConnected();
    Console_Print("[CoSync] IsConnected() -> %s", connected ? "true" : "false");
    LOG_DEBUG("[Papy-CoSync] IsConnected() -> %s", connected ? "true" : "false");
    return connected;
}

static bool CoSync_Connect(
    StaticFunctionTag*,
    Actor* player,
    TESNPC* playerActorBase,
    BSFixedString address,
    SInt32 port)

{
    Console_Print("[CoSync] Connect(player=%p base=%p addr=%s port=%d) -> false (stub)",
        player, playerActorBase, address.c_str(), port);

    LOG_DEBUG("[Papy-CoSync] Connect(player=%p base=%p addr=%s port=%d) -> false (stub)",
        player, playerActorBase, address.c_str(), port);
    return false;
}

static bool CoSync_Disconnect(StaticFunctionTag*)
{

    Console_Print("[CoSync] Disconnect() -> true (stub)");
    return true;
}

static void CoSync_Tick(StaticFunctionTag*)
{


}

static void CoSync_SyncWorld(StaticFunctionTag*)
{
    Console_Print("[CoSync] SyncWorld() (stub)");
}

static SInt32 CoSync_GetPlayerEntityID(StaticFunctionTag*)
{
    const SInt32 entityID = static_cast<SInt32>(CoSyncNet::GetMyEntityID());
    LOG_DEBUG("[Papy-CoSync] GetPlayerEntityID() -> %d", entityID);
    return entityID;
}

static SInt32 CoSync_GetEntityID(StaticFunctionTag*, TESObjectREFR* ref)
{
    Console_Print("[CoSync] GetEntityID(ref=%p) -> -1 (stub)", ref);
    return -1;
}

static void CoSync_SetEntityRef(StaticFunctionTag*, SInt32 entityID, TESObjectREFR* ref)
{
    Console_Print("[CoSync] SetEntityRef(id=%d ref=%p) (stub)", entityID, ref);
}

static bool CoSync_IsEntityValid(StaticFunctionTag*, SInt32 entityID)
{
    const bool ok = (entityID >= 0);
    Console_Print("[CoSync] IsEntityValid(%d) -> %s (stub)", entityID, ok ? "true" : "false");
    return ok;
}

static VMArray<float> CoSync_GetEntityPosition(StaticFunctionTag*, SInt32 entityID)
{
    Console_Print("[CoSync] GetEntityPosition(%d) -> [0,0,0] (stub)", entityID);

    VMArray<float> arr;
    float z = 0.0f;
    arr.Push(&z);
    arr.Push(&z);
    arr.Push(&z);
    return arr;
}

static void CoSync_SetEntityPosition(StaticFunctionTag*, SInt32 entityID, float x, float y, float z)
{
    Console_Print("[CoSync] SetEntityPosition(id=%d x=%.3f y=%.3f z=%.3f) (stub)", entityID, x, y, z);
}

static void CoSync_SetEntVarNum(StaticFunctionTag*, SInt32 entityID, BSFixedString name, float value)
{
    Console_Print("[CoSync] SetEntVarNum(id=%d name=%s val=%.3f) (stub)", entityID, name.c_str(), value);
}

static void CoSync_SetEntVarAnim(StaticFunctionTag*, SInt32 entityID, BSFixedString animState)
{
    Console_Print("[CoSync] SetEntVarAnim(id=%d anim=%s) (stub)", entityID, animState.c_str());
}

static float CoSync_GetEntVarNum(StaticFunctionTag*, SInt32 entityID, BSFixedString name)
{
    Console_Print("[CoSync] GetEntVarNum(id=%d name=%s) -> 0 (stub)", entityID, name.c_str());
    return 0.0f;
}

static BSFixedString CoSync_GetEntVarAnim(StaticFunctionTag*, SInt32 entityID)
{
    Console_Print("[CoSync] GetEntVarAnim(id=%d) -> \"\" (stub)", entityID);
    return BSFixedString("");
}

static VMArray<TESObjectREFR*> CoSync_GetRefsInCell(StaticFunctionTag*, TESObjectCELL* cell)
{
    Console_Print("[CoSync] GetRefsInCell(cell=%p) -> [] (stub)", cell);

    VMArray<TESObjectREFR*> arr;
    return arr;
}

static float CoSync_Atan2(StaticFunctionTag*, float y, float x)
{
    const float r = atan2f(y, x);

    return r;
}

static BSFixedString CoSync_GetWalkDir(StaticFunctionTag*, float dX, float dY, float angleZ)
{

    (void)dX; (void)dY; (void)angleZ;
    Console_Print("[CoSync] GetWalkDir(dx=%.3f dy=%.3f ang=%.3f) -> \"\" (stub)", dX, dY, angleZ);
    return BSFixedString("");
}

static PapyrusActionType CoSync_GetAction(StaticFunctionTag*, BSFixedString name)
{

    Console_Print("[CoSync] GetAction(%s) -> None (stub)", name.c_str());
    return nullptr;
}

static void CoSync_Inspect(StaticFunctionTag*, VMArray<TESForm*> forms)
{
    Console_Print("[CoSync] Inspect(forms=%u) (stub)", forms.Length());
}

static bool CoSync_AnimLoops(StaticFunctionTag*, BSFixedString animState)
{
    Console_Print("[CoSync] AnimLoops(%s) -> false (stub)", animState.c_str());
    return false;
}

static void CoSync_CopyAppearance(StaticFunctionTag*, TESNPC* src, TESNPC* dest)
{
    Console_Print("[CoSync] CopyAppearance(src=%p dest=%p) (stub)", src, dest);
}

static void CoSync_CopyWornItems(StaticFunctionTag*, Actor* src, Actor* dest)
{
    Console_Print("[CoSync] CopyWornItems(src=%p dest=%p) (stub)", src, dest);
}

static void CoSync_PlayerHit(StaticFunctionTag*, SInt32 hitter, SInt32 hittee, float damage)
{
    Console_Print("[CoSync] PlayerHit(hitter=%d hittee=%d dmg=%.3f) (stub)", hitter, hittee, damage);
}

static void CoSync_PlayerFireWeapon(StaticFunctionTag*)
{
    Console_Print("[CoSync] PlayerFireWeapon() (stub)");
}

static void CoSync_TopicInfoBegin(StaticFunctionTag*, TESForm* info, TESObjectREFR* speaker)
{
    Console_Print("[CoSync] TopicInfoBegin(info=%p speaker=%p) (stub)", info, speaker);
}

static VMArray<TESObjectREFR*> CoSync_GetEntitySyncRefs(StaticFunctionTag*, bool clear)
{
    Console_Print("[CoSync] GetEntitySyncRefs(clear=%s) -> [] (stub)", clear ? "true" : "false");
    VMArray<TESObjectREFR*> arr;
    return arr;
}

static VMArray<float> CoSync_GetEntitySyncTransforms(StaticFunctionTag*, bool clear)
{
    Console_Print("[CoSync] GetEntitySyncTransforms(clear=%s) -> [] (stub)", clear ? "true" : "false");
    VMArray<float> arr;
    return arr;
}

static bool CoSync_SpawnNpc(StaticFunctionTag*, TESObjectREFR* ref)
{
    Console_Print("[CoSync] SpawnNpc(ref=%p) -> false (stub)", ref);
    return false;
}

static VMArray<SInt32> CoSync_GetAllEntityIDs(StaticFunctionTag*)
{
    VMArray<SInt32> arr;
    return arr;
}

static TESObjectREFR* CoSync_GetActorByEntityID(StaticFunctionTag*, SInt32 entityID)
{
    CoSyncThreadDiag::Note(CoSyncThreadDiag::kPapyrusGetActor);
    if (entityID < 0)
        return nullptr;

    Actor* actor = g_CoSyncEntities.GetActor(static_cast<uint32_t>(entityID));
    LOG_DEBUG("[Papy-CoSync] GetActorByEntityID(%d) -> %p", entityID, actor);

    return static_cast<TESObjectREFR*>(actor);
}

static SInt32 CoSync_GetEntityIDForActor(StaticFunctionTag*, TESObjectREFR* akRef)
{
    if (!akRef)
        return -1;

    // 2026-07-04: DYNAMIC_CAST removed — minidump-confirmed broken RTTI (see
    // CoSyncPlayerManager.cpp PumpDeferredSpawns_Impl). formType check by
    // engine contract instead (kFormType_ACHR == Actor reference).
    Actor* actor = (akRef->formType == kFormType_ACHR)
        ? static_cast<Actor*>(akRef) : nullptr;
    if (!actor)
        return -1;

    const uint32_t id = g_CoSyncEntities.GetEntityID(actor);
    if (id == 0)
    {
        LOG_DEBUG("[Papy-CoSync] GetEntityIDForActor(%p) -> not registered", akRef);
        return -1;
    }

    LOG_DEBUG("[Papy-CoSync] GetEntityIDForActor(%p) -> %u", akRef, id);
    return static_cast<SInt32>(id);
}

static SInt32 CoSync_GetPendingWeatherFormID(StaticFunctionTag*)
{
    const uint32_t formID = CoSyncTimeSync::GetLockedWeatherFormID();
    LOG_DEBUG("[Papy-CoSync] GetPendingWeatherFormID() -> 0x%08X", formID);
    return static_cast<SInt32>(formID);
}

static bool CoSync_IsDoor(StaticFunctionTag*, TESObjectREFR* akRef)
{
    if (!akRef || !akRef->baseForm)
        return false;

    const bool isDoor = (akRef->baseForm->formType == 32);
    return isDoor;
}

static void CoSync_NotifyActivate(StaticFunctionTag*, TESObjectREFR* akRef, SInt32 aiOpenState)
{
    if (!akRef)
    {
        LOG_WARN("[Papy-CoSync] NotifyActivate: null ref");
        return;
    }

    const uint32_t entityID  = CoSyncNet::GetMyEntityID();
    const uint32_t refFormID = akRef->formID;

    char buf[64];
    std::snprintf(buf, sizeof(buf), "ACT|%u|%u|%d", entityID, refFormID, static_cast<int>(aiOpenState));
    CoSyncTransport::Send(std::string(buf));

    LOG_INFO("[Papy-CoSync] NotifyActivate: entity=%u ref=0x%08X state=%d",
        entityID, refFormID, static_cast<int>(aiOpenState));
}

static bool CoSync_IsHost(StaticFunctionTag*)
{
    return CoSyncNet::IsHost();
}

static void CoSync_NotifyWeatherChanged(StaticFunctionTag*, TESForm* )
{
    LOG_DEBUG("[Papy-CoSync] NotifyWeatherChanged called (C++ handles weather now)");
}

static void CoSync_NotifyWorkshopBuild(StaticFunctionTag*, TESObjectREFR* akWorkshop, TESObjectREFR* akPlaced)
{
    if (!CoSyncNet::IsHost())
        return;
    if (!akPlaced || !akPlaced->baseForm)
        return;

    // Sends the same "WBF|" packet HostTickObjects_Impl's poll would
    // eventually send for this object, just the instant the Papyrus build
    // event fires instead of waiting on the cell-scan + settle delay. See
    // CoSyncWorldSync::NotifyWorkshopBuildImmediate.
    CoSyncWorldSync::WorkshopBuildFullPacket p{};
    p.entityID   = CoSyncNet::GetMyEntityID();
    p.baseFormID = akPlaced->baseForm->formID;
    p.posX = akPlaced->pos.x; p.posY = akPlaced->pos.y; p.posZ = akPlaced->pos.z;
    p.rotX = akPlaced->rot.x; p.rotY = akPlaced->rot.y; p.rotZ = akPlaced->rot.z;

    // ── INSTANT BROADCAST DISABLED 2026-08-05 ───────────────────────────────
    // This path announced a build the moment the Papyrus event fired, and the
    // periodic cell scan announced it again once it settled. Two problems, both
    // reported:
    //   * DOUBLE SPAWNS -- the two paths do not reliably agree on the placed
    //     ref's formID, so the instant path's dedupe pre-seed marks the wrong
    //     ID and the scan re-broadcasts a second copy. The receiver-side
    //     dedupe added on 2026-08-04 catches most of these, but only within
    //     its radius/time window, which is why duplicates still slip through
    //     "sometimes".
    //   * WRONG POSITION/ROTATION -- akPlaced's transform at build-event time
    //     is PRE-SETTLE. The engine finishes placing the piece afterwards, so
    //     the instant packet ships coordinates that are close but not exact,
    //     which is the "not exactly where the builder puts it" report.
    //
    // The cell scan reads the FINISHED ref, so it has the settled transform and
    // one announcement per object. Making it the single source fixes both
    // symptoms. Cost: peers see the build after the scan interval instead of
    // instantly -- acceptable for workshop building, and correctness wins.
    //
    // Re-enable by restoring the NotifyWorkshopBuildImmediate call, but only
    // once the two paths provably agree on the placed ref.
    LOG_UPDATED("[Papy-CoSync] NotifyWorkshopBuild: base=0x%08X placed=0x%08X — instant "
                "broadcast suppressed, the cell scan owns this (settled transform, "
                "single announcement)",
        p.baseFormID, akPlaced->formID);
}

static void CoSync_NotifyWorkshopDestroy(StaticFunctionTag*, TESObjectREFR* akWorkshop, TESObjectREFR* akScrapped)
{
    if (!CoSyncNet::IsHost())
        return;
    uint32_t workshopFormID  = akWorkshop  ? akWorkshop->formID  : 0;
    uint32_t scrappedFormID  = akScrapped  ? akScrapped->formID  : 0;
    if (!workshopFormID || !scrappedFormID)
        return;
    CoSyncWorkshopSync::NotifyWorkshopDestroy(workshopFormID, scrappedFormID, CoSyncNet::GetMyEntityID());
    LOG_INFO("[Papy-CoSync] NotifyWorkshopDestroy: workshop=0x%08X scrapped=0x%08X", workshopFormID, scrappedFormID);
}

static void CoSync_NotifyWorkshopPower(StaticFunctionTag*, TESObjectREFR* , bool )
{

    LOG_DEBUG("[Papy-CoSync] NotifyWorkshopPower (stub)");
}

static void CoSync_NotifyWorkshopMode(StaticFunctionTag*, TESObjectREFR* akWorkshop, bool isEntering)
{

    uint32_t workshopFormID = akWorkshop ? akWorkshop->formID : 0;
    CoSyncWorkshopSync::NotifyWorkshopMode(workshopFormID, CoSyncNet::GetMyEntityID(), isEntering);
    LOG_INFO("[Papy-CoSync] NotifyWorkshopMode: ref=0x%08X entering=%d", workshopFormID, isEntering ? 1 : 0);
}

// SEH-isolated scan of a chassis ref's BGSInventoryList for ARMO items.
// noinline: /GL+LTCG silently drops __try scopes on inlined copies.
__declspec(noinline) static int ScanChassisForPABits(
    TESObjectREFR* chassis, uint32_t outPieces[6])
{
    int count = 0;
    if (!chassis) return 0;
    __try
    {
        BGSInventoryList* invList = chassis->inventoryList;
        if (!invList) return 0;
        for (UInt32 i = 0; i < invList->items.count && count < 6; ++i)
        {
            const BGSInventoryItem& item = invList->items[i];
            if (!item.form || item.form->formType != kFormType_ARMO) continue;
            outPieces[count++] = item.form->formID;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[Papy-CoSync] ScanChassisForPABits: crashed while scanning the chassis inventory, recovered");
    }
    return count;
}

// Fallback for when the chassis's own inventory is already empty at scan
// time. Actor.OnSit only fires once akActor.IsInPowerArmor() already
// returns true (see the guard in the OnSit handler this feeds) -- meaning
// the engine's own PA-enter transition has already run by the time this
// Papyrus event lands, and a live 2026-08-01 test confirmed
// ScanChassisForPABits finds pieces=[0,0,0,0,0,0] even with pieces actually
// installed. Most likely explanation: the frame's pieces have already been
// merged onto the actor's own equipped-item state by then, leaving the
// chassis container looking empty. Scans the WEARER actor's own equipped
// ARMO items instead -- same stack-visit/kFlagEquipped pattern already
// proven working in CoSyncOutfitSync.cpp's GetEquippedArmorFormIDs (regular
// outfit sync), applied here to the actor entering PA.
// Non-static: CoSyncPowerArmorSync re-scans equipped pieces on a timer so a
// helmet (or any piece) swapped WHILE in power armor still syncs. Pieces used
// to be captured only in the enter event, which is why the helmet stopped
// matching after the first change.
__declspec(noinline) int ScanActorEquippedPAPieces(
    Actor* actor, uint32_t outPieces[6])
{
    int count = 0;
    if (!actor) return 0;
    __try
    {
        BGSInventoryList* invList = actor->inventoryList;
        if (!invList) return 0;
        for (UInt32 i = 0; i < invList->items.count && count < 6; ++i)
        {
            const BGSInventoryItem& item = invList->items[i];
            if (!item.form || item.form->formType != kFormType_ARMO) continue;
            if (!item.stack) continue;

            bool equipped = false;
            item.stack->Visit([&equipped](BGSInventoryItem::Stack* s) {
                if (s->flags & BGSInventoryItem::Stack::kFlagEquipped)
                {
                    equipped = true;
                    return false;
                }
                return true;
            });
            if (!equipped) continue;

            outPieces[count++] = item.form->formID;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[Papy-CoSync] ScanActorEquippedPAPieces: crashed while scanning the actor's inventory, recovered");
    }
    return count;
}

static void CoSync_NotifyPowerArmor(
    StaticFunctionTag*,
    Actor*          akActor,
    TESObjectREFR*  akPowerArmor,
    bool            abEntering)
{
    if (!akActor || !akPowerArmor)
    {
        LOG_WARN("[Papy-CoSync] NotifyPowerArmor: null args");
        return;
    }

    const uint32_t chassisRefFormID  = akPowerArmor->formID;
    const uint32_t chassisBaseFormID = akPowerArmor->baseForm
        ? akPowerArmor->baseForm->formID
        : 0;

    if (chassisBaseFormID == 0)
    {
        LOG_WARN("[Papy-CoSync] NotifyPowerArmor: chassis has no baseForm");
        return;
    }

    // Sender-side PA state tracking (see CoSyncPowerArmorSync.h for why):
    // lets SaveAnimationVariables force the humanoid capture path for this
    // actor instead of trusting the variable-set-size heuristic alone.
    CoSyncPowerArmorSync::MarkActorPowerArmorState(akActor->formID, abEntering);

    // On PA enter: scan the chassis's inventory for PA pieces (ARMO items).
    // The chassis is a container-furniture; its BGSInventoryList holds the
    // installed PA pieces before the actor enters it.
    uint32_t pieceFormIDs[6] = {};
    int pieceCount = 0;
    if (abEntering)
    {
        pieceCount = ScanChassisForPABits(akPowerArmor, pieceFormIDs);
        if (pieceCount == 0)
        {
            pieceCount = ScanActorEquippedPAPieces(akActor, pieceFormIDs);
            if (pieceCount > 0)
                LOG_INFO("[Papy-CoSync] NotifyPowerArmor: chassis inventory was empty, found %d piece(s) on actor's equipped items instead",
                    pieceCount);
        }
    }

    CoSyncPowerArmorSync::NotifyPowerArmor(
        CoSyncNet::GetMyEntityID(),
        chassisBaseFormID,
        chassisRefFormID,
        abEntering,
        pieceFormIDs,
        akPowerArmor->pos);

    LOG_INFO("[Papy-CoSync] NotifyPowerArmor base=0x%08X ref=0x%08X entering=%d pieces=%d",
        chassisBaseFormID, chassisRefFormID, abEntering ? 1 : 0, pieceCount);
}

static void CoSync_NotifyPipboyOpen(StaticFunctionTag*, bool abOpening)
{
    const uint32_t entityID = CoSyncNet::GetMyEntityID();

    char buf[32];
    std::snprintf(buf, sizeof(buf), "PB|%u|%d", entityID, abOpening ? 1 : 0);
    CoSyncTransport::Send(std::string(buf));

    LOG_INFO("[Papy-CoSync] NotifyPipboyOpen: entity=%u opening=%d", entityID, abOpening ? 1 : 0);
}

static void CoSync_NotifyConsumableUsed(StaticFunctionTag*, TESForm* akItem)
{
    if (!akItem)
    {
        LOG_WARN("[Papy-CoSync] NotifyConsumableUsed: null item");
        return;
    }

    const uint32_t entityID = CoSyncNet::GetMyEntityID();
    const std::string msg = CoSyncConsumableSync::SerializeConsumable(entityID, akItem->formID);
    CoSyncTransport::Send(msg);

    LOG_INFO("[Papy-CoSync] NotifyConsumableUsed: formID=0x%08X entity=%u",
        akItem->formID, entityID);
}

static void CoSync_NotifySpellApplied(StaticFunctionTag*, TESForm* akSpell)
{
    if (!akSpell)
        return;

    const uint32_t entityID = CoSyncNet::GetMyEntityID();
    const std::string msg = CoSyncConsumableSync::SerializeSpell(entityID, akSpell->formID);
    CoSyncTransport::Send(msg);

    LOG_INFO("[Papy-CoSync] NotifySpellApplied: formID=0x%08X entity=%u",
        akSpell->formID, entityID);
}

static void CoSync_NotifyMapMarkerDiscovered(StaticFunctionTag*, TESObjectREFR* akMarker)
{
    if (!akMarker)
    {
        LOG_WARN("[Papy-CoSync] NotifyMapMarkerDiscovered: null marker");
        return;
    }
    CoSyncMapSync::BroadcastDiscovery(akMarker->formID);
}

static void CoSync_NotifyQuestStage(StaticFunctionTag*, TESQuest* akQuest, SInt32 aiStage)
{
    if (!akQuest)
    {
        LOG_WARN("[Papy-CoSync] NotifyQuestStage: null quest");
        return;
    }
    CoSyncQuestSync::NotifyQuestStage(
        akQuest->formID,
        static_cast<int>(aiStage),
        CoSyncNet::GetMyEntityID());
    LOG_INFO("[Papy-CoSync] NotifyQuestStage quest=0x%08X stage=%d",
        akQuest->formID, static_cast<int>(aiStage));
}

static void CoSync_NotifyQuestStart(StaticFunctionTag*, TESQuest* akQuest)
{
    if (!akQuest)
    {
        LOG_WARN("[Papy-CoSync] NotifyQuestStart: null quest");
        return;
    }
    CoSyncQuestSync::NotifyQuestStart(akQuest->formID, CoSyncNet::GetMyEntityID());
    LOG_INFO("[Papy-CoSync] NotifyQuestStart quest=0x%08X", akQuest->formID);
}

static void CoSync_NotifyQuestDone(StaticFunctionTag*, TESQuest* akQuest)
{
    if (!akQuest)
    {
        LOG_WARN("[Papy-CoSync] NotifyQuestDone: null quest");
        return;
    }
    CoSyncQuestSync::NotifyQuestDone(akQuest->formID, CoSyncNet::GetMyEntityID());
    LOG_INFO("[Papy-CoSync] NotifyQuestDone quest=0x%08X", akQuest->formID);
}

static void CoSync_NotifyCompanionRecruited(StaticFunctionTag*, Actor* akCompanion)
{
    if (!akCompanion || !akCompanion->baseForm)
    {
        LOG_UPDATED_WARN("[Papy-CoSync] NotifyCompanionRecruited: null actor or baseForm");
        return;
    }


    // A synced actor (e.g. the host's companion as a client sees it) is NOT
    // skipped any more: a client recruiting it gets its own copy
    // (CoSyncCompanions), which is exactly the shared-companion rule.
    const uint32_t baseFormID = akCompanion->baseForm->formID;

    // Round 141: reject CoSync's own proxy/ghost bases. Live 2026-07-05
    // session: a stale save-persisted proxy actor (base 0x003599, teammate-
    // flagged from an old build) got picked up by whatever quest fragment
    // calls this and was registered as a real companion, spawning a SECOND
    // proxy on the peer's screen. Any CoSync-owned base must never become a
    // Papyrus-tracked companion.
    if (baseFormID == 0x00000007u ||
        baseFormID == 0x00020749u ||
        (baseFormID & 0x00FFFFFFu) == 0x00003599u ||
        CoSyncProxyLooks::IsProxyBaseFormID(baseFormID))
    {
        LOG_UPDATED_WARN("[Papy-CoSync] NotifyCompanionRecruited: base=0x%08X is a CoSync proxy/ghost base -- REJECTED (not a real companion)",
            baseFormID);
        return;
    }

    // 2026-09-26: companions follow the shared-world rules in
    // CoSyncCompanions (host's = its real actors; a client's = its own copy).
    // No more host-spawned duplicate per recruit (old COMP_REQ path).
    CoSyncCompanions::NotifyRecruited(akCompanion);
    LOG_UPDATED("[Papy-CoSync] NotifyCompanionRecruited: base=0x%08X ref=0x%08X", baseFormID, akCompanion->formID);
}

static void CoSync_NotifyCompanionDismissed(StaticFunctionTag*, Actor* akCompanion)
{
    if (!akCompanion)
    {
        LOG_UPDATED_WARN("[Papy-CoSync] NotifyCompanionDismissed: null actor");
        return;
    }

    CoSyncCompanions::NotifyDismissed(akCompanion);
    LOG_UPDATED("[Papy-CoSync] NotifyCompanionDismissed: ref=0x%08X", akCompanion->formID);
}

// ── Generic mod event channel ──────────────────────────────────────────────
// Any Nexus mod can call CoSync.SendModEvent("MyMod", "eventName", "payload")
// and poll for incoming events with HasModEvent / PollModEvent.
// PollModEvent returns a 2-element string array [eventName, payload], or an
// empty array when the queue is empty.

static void CoSync_SendModEvent(StaticFunctionTag*,
    BSFixedString modID, BSFixedString eventName, BSFixedString payload)
{
    if (!modID.c_str() || !eventName.c_str())
        return;

    CoSyncModEventBus::Send(
        std::string(modID.c_str()),
        std::string(eventName.c_str()),
        payload.c_str() ? std::string(payload.c_str()) : std::string());
}

static bool CoSync_HasModEvent(StaticFunctionTag*, BSFixedString modID)
{
    if (!modID.c_str())
        return false;
    return CoSyncModEventBus::HasPendingEvent(std::string(modID.c_str()));
}

static VMArray<BSFixedString> CoSync_PollModEvent(StaticFunctionTag*, BSFixedString modID)
{
    VMArray<BSFixedString> result;
    if (!modID.c_str())
        return result;

    std::string eventName, payload;
    if (CoSyncModEventBus::PopEvent(std::string(modID.c_str()), eventName, payload))
    {
        BSFixedString evtStr(eventName.c_str());
        BSFixedString payStr(payload.c_str());
        result.Push(&evtStr);
        result.Push(&payStr);
    }
    return result;
}

bool RegisterCoSyncPapyrus(VirtualMachine* vm)
{
    if (!vm)
        return false;

    constexpr const char* kClass = "CoSync";


    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, SInt32>(
        "GetClientInstanceID", kClass, CoSync_GetClientInstanceID, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, void, SInt32>(
        "SetClient", kClass, CoSync_SetClient, vm));


    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, bool>(
        "IsConnected", kClass, CoSync_IsConnected, vm));


    vm->RegisterFunction(new NativeFunction4<StaticFunctionTag, bool, Actor*, TESNPC*, BSFixedString, SInt32>(
        "Connect", kClass, CoSync_Connect, vm));


    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, bool>(
        "Disconnect", kClass, CoSync_Disconnect, vm));


    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, void>(
        "Tick", kClass, CoSync_Tick, vm));


    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, void>(
        "SyncWorld", kClass, CoSync_SyncWorld, vm));


    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, SInt32>(
        "GetPlayerEntityID", kClass, CoSync_GetPlayerEntityID, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, SInt32, TESObjectREFR*>(
        "GetEntityID", kClass, CoSync_GetEntityID, vm));


    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, void, SInt32, TESObjectREFR*>(
        "SetEntityRef", kClass, CoSync_SetEntityRef, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, bool, SInt32>(
        "IsEntityValid", kClass, CoSync_IsEntityValid, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, VMArray<float>, SInt32>(
        "GetEntityPosition", kClass, CoSync_GetEntityPosition, vm));


    vm->RegisterFunction(new NativeFunction4<StaticFunctionTag, void, SInt32, float, float, float>(
        "SetEntityPosition", kClass, CoSync_SetEntityPosition, vm));


    vm->RegisterFunction(new NativeFunction3<StaticFunctionTag, void, SInt32, BSFixedString, float>(
        "SetEntVarNum", kClass, CoSync_SetEntVarNum, vm));


    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, void, SInt32, BSFixedString>(
        "SetEntVarAnim", kClass, CoSync_SetEntVarAnim, vm));


    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, float, SInt32, BSFixedString>(
        "GetEntVarNum", kClass, CoSync_GetEntVarNum, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, BSFixedString, SInt32>(
        "GetEntVarAnim", kClass, CoSync_GetEntVarAnim, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, VMArray<TESObjectREFR*>, TESObjectCELL*>(
        "GetRefsInCell", kClass, CoSync_GetRefsInCell, vm));


    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, float, float, float>(
        "Atan2", kClass, CoSync_Atan2, vm));


    vm->RegisterFunction(new NativeFunction3<StaticFunctionTag, BSFixedString, float, float, float>(
        "GetWalkDir", kClass, CoSync_GetWalkDir, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, PapyrusActionType, BSFixedString>(
        "GetAction", kClass, CoSync_GetAction, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, void, VMArray<TESForm*>>(
        "Inspect", kClass, CoSync_Inspect, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, bool, BSFixedString>(
        "AnimLoops", kClass, CoSync_AnimLoops, vm));


    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, void, TESNPC*, TESNPC*>(
        "CopyAppearance", kClass, CoSync_CopyAppearance, vm));


    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, void, Actor*, Actor*>(
        "CopyWornItems", kClass, CoSync_CopyWornItems, vm));


    vm->RegisterFunction(new NativeFunction3<StaticFunctionTag, void, SInt32, SInt32, float>(
        "PlayerHit", kClass, CoSync_PlayerHit, vm));


    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, void>(
        "PlayerFireWeapon", kClass, CoSync_PlayerFireWeapon, vm));


    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, void, TESForm*, TESObjectREFR*>(
        "TopicInfoBegin", kClass, CoSync_TopicInfoBegin, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, VMArray<TESObjectREFR*>, bool>(
        "GetEntitySyncRefs", kClass, CoSync_GetEntitySyncRefs, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, VMArray<float>, bool>(
        "GetEntitySyncTransforms", kClass, CoSync_GetEntitySyncTransforms, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, bool, TESObjectREFR*>(
        "SpawnNpc", kClass, CoSync_SpawnNpc, vm));


    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, VMArray<SInt32>>(
        "GetAllEntityIDs", kClass, CoSync_GetAllEntityIDs, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, TESObjectREFR*, SInt32>(
        "GetActorByEntityID", kClass, CoSync_GetActorByEntityID, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, SInt32, TESObjectREFR*>(
        "GetEntityIDForActor", kClass, CoSync_GetEntityIDForActor, vm));


    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, SInt32>(
        "GetPendingWeatherFormID", kClass, CoSync_GetPendingWeatherFormID, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, bool, TESObjectREFR*>(
        "IsDoor", kClass, CoSync_IsDoor, vm));


    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, void, TESObjectREFR*, SInt32>(
        "NotifyActivate", kClass, CoSync_NotifyActivate, vm));


    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, bool>(
        "IsHost", kClass, CoSync_IsHost, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, void, TESForm*>(
        "NotifyWeatherChanged", kClass, CoSync_NotifyWeatherChanged, vm));


    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, void, TESObjectREFR*, TESObjectREFR*>(
        "NotifyWorkshopBuild", kClass, CoSync_NotifyWorkshopBuild, vm));


    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, void, TESObjectREFR*, TESObjectREFR*>(
        "NotifyWorkshopDestroy", kClass, CoSync_NotifyWorkshopDestroy, vm));


    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, void, TESObjectREFR*, bool>(
        "NotifyWorkshopPower", kClass, CoSync_NotifyWorkshopPower, vm));


    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, void, TESObjectREFR*, bool>(
        "NotifyWorkshopMode", kClass, CoSync_NotifyWorkshopMode, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, void, TESObjectREFR*>(
        "NotifyMapMarkerDiscovered", kClass, CoSync_NotifyMapMarkerDiscovered, vm));


    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, void, TESQuest*, SInt32>(
        "NotifyQuestStage", kClass, CoSync_NotifyQuestStage, vm));

    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, void, TESQuest*>(
        "NotifyQuestStart", kClass, CoSync_NotifyQuestStart, vm));

    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, void, TESQuest*>(
        "NotifyQuestDone", kClass, CoSync_NotifyQuestDone, vm));

    vm->SetFunctionFlags(kClass, "NotifyQuestStage", IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "NotifyQuestStart", IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "NotifyQuestDone",  IFunction::kFunctionFlag_NoWait);


    vm->RegisterFunction(new NativeFunction3<StaticFunctionTag, void, Actor*, TESObjectREFR*, bool>(
        "NotifyPowerArmor", kClass, CoSync_NotifyPowerArmor, vm));

    vm->SetFunctionFlags(kClass, "GetClientInstanceID",      IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "IsConnected",              IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "GetPlayerEntityID",        IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "IsEntityValid",            IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "Atan2",                    IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "IsHost",                   IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "GetActorByEntityID",       IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "GetEntityIDForActor",      IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "GetPendingWeatherFormID",  IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "IsDoor",                   IFunction::kFunctionFlag_NoWait);


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, void, TESForm*>(
        "NotifyConsumableUsed", kClass, CoSync_NotifyConsumableUsed, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, void, TESForm*>(
        "NotifySpellApplied", kClass, CoSync_NotifySpellApplied, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, void, bool>(
        "NotifyPipboyOpen", kClass, CoSync_NotifyPipboyOpen, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, void, Actor*>(
        "NotifyCompanionRecruited", kClass, CoSync_NotifyCompanionRecruited, vm));


    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, void, Actor*>(
        "NotifyCompanionDismissed", kClass, CoSync_NotifyCompanionDismissed, vm));

    vm->SetFunctionFlags(kClass, "NotifyCompanionRecruited", IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "NotifyCompanionDismissed", IFunction::kFunctionFlag_NoWait);


    vm->RegisterFunction(new NativeFunction3<StaticFunctionTag, void, BSFixedString, BSFixedString, BSFixedString>(
        "SendModEvent", kClass, CoSync_SendModEvent, vm));

    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, bool, BSFixedString>(
        "HasModEvent", kClass, CoSync_HasModEvent, vm));

    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, VMArray<BSFixedString>, BSFixedString>(
        "PollModEvent", kClass, CoSync_PollModEvent, vm));

    vm->SetFunctionFlags(kClass, "SendModEvent", IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "HasModEvent",  IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "PollModEvent", IFunction::kFunctionFlag_NoWait);

    LOG_INFO("Papyrus_CoSync: Registered FULL CoSync native contract (stubs).");
    return true;
}
