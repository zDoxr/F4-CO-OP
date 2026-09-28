#pragma once

#include <cstdint>

class TESObjectREFR;
class NiPoint3;

// Thin, SEH-guarded wrappers over the Fallout 4 1.11.240 workshop internals
// verified in docs/WORKSHOP_FACTIONS_RE.md. Nothing here keeps engine
// pointers; callers resolve what they need, use it, and let it go.
// Game-side calls (everything except the pure getters) belong on CoSync's
// tick or inside an engine hook, like every other engine call in CoSync.
namespace CoSyncWorkshopEngine
{
    // BSPointerHandle -> ref, using the engine's own inline validation
    // (index + generation). No reference is taken. nullptr if stale.
    TESObjectREFR* ResolveHandle(uint32_t handle);

    // Workshop::hCurrentWorkshop (0x30F7698): the workbench of the workshop
    // mode the local player is in, or nullptr.
    TESObjectREFR* GetCurrentWorkshop();

    // Workshop::PlacementItem (0x30F7680): the object being placed / edited.
    TESObjectREFR* GetPlacementItem();

    // Settlement workbench? (has WorkshopKeyword, 0x30F77E0)
    bool IsWorkshopRef(TESObjectREFR* ref);

    // The workbench a built object is linked to by WorkshopItemKeyword
    // (0x30F7818), or nullptr. This link is what makes an object part of a
    // settlement.
    TESObjectREFR* GetLinkedWorkshop(TESObjectREFR* item);

    // Link (or, with workshop == nullptr, unlink) a built object.
    bool SetLinkedWorkshop(TESObjectREFR* item, TESObjectREFR* workshop);

    // Workshop::FindNearestValidWorkshop (0x385720).
    TESObjectREFR* FindNearestValidWorkshop(TESObjectREFR* from);

    // Fire the engine's own workshop events for an object, exactly as
    // PlaceCurrentReference / ScrapReference / edit-finish do. Papyrus
    // (WorkshopParentScript) reacts to these to keep settlement stats.
    void DispatchItemPlaced(TESObjectREFR* workshop, TESObjectREFR* item);
    void DispatchItemDestroyed(TESObjectREFR* workshop, TESObjectREFR* item);
    void DispatchItemMoved(TESObjectREFR* workshop, TESObjectREFR* item);

    // Verified function entries, for hooks (module-relative RVAs).
    constexpr uintptr_t kRVA_PlaceCurrentReference       = 0x398CB0;
    constexpr uintptr_t kRVA_ScrapReference              = 0x393F30;
    constexpr uintptr_t kRVA_StoreReference              = 0x393220;
    constexpr uintptr_t kRVA_RepairCallbackRun           = 0x3B7E00;
    constexpr uintptr_t kRVA_IsLocationWithinBuildableArea = 0x3844D0;
    constexpr uintptr_t kRVA_BudgetRatio                 = 0x37D6C0;
}
