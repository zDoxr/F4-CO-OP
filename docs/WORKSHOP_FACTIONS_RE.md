# Workshop / Settlement reverse engineering (Fallout4.exe 1.11.240)

Phase 2 of the Factions + Territory + Workshop directive (2026-09-28).
Every address below is a module-relative RVA (image base 0x140000000) on the
disk build of **1.11.240**, the only runtime CoSync supports.

## Method

Three independent sources were used for each function, and an address was
only accepted when they agreed:

1. **IDA 9.3** headless decompilation of the canonical 1.11.240 IDB
   (`Fallout4_1_11_240_CoSync_FPGraph.i64`, worked on a copy).
2. **CommonLibF4** (Dear-Modding-FO4/commonlibf4 `main`, `include/RE/W/Workshop.h`
   and `include/RE/IDs.h`): the NG Address-Library IDs were mapped through the
   installed `Data\F4SE\Plugins\version-1-11-240-0.bin`
   (format: `u64 count`, then `count` x `(u64 id, u64 offset)`).
3. **F4SE 0.7.9** `f4se/GameWorkshop.cpp` (CURRENT_RELEASE_RUNTIME 1.11.240).

Status labels: **VERIFIED** means the decompiled body matches the claimed
behaviour and signature, and the address agrees with at least one other
source. **UNRESOLVED** means it was not proven and nothing in the code
depends on it.

## Globals

| RVA | Meaning | Evidence | Status |
|---|---|---|---|
| 0x30F7698 | `Workshop::hCurrentWorkshop` (BSPointerHandle) | F4SE 0.7.9; resolved at the top of ScrapReference, Store, PlaceCurrentReference, FreeBuild; written by StartWorkshop | VERIFIED |
| 0x30F7680 | `Workshop::PlacementItem`: the object being placed or edited (handle) | CommonLibF4 ID 4797235 → 0x30F7680; written by SetSelectedEditItem; read by PlaceCurrentReference / store / repair prompts | VERIFIED |
| 0x30F7700 | `Workshop::CurrentPlacementItemData` | CommonLibF4 ID 4797261 | VERIFIED (address only) |
| 0x30F7818 | `BGSKeyword*` WorkshopItemKeyword (links a built object to its workbench) | used as the keyword argument of SetLinkedRef/GetLinkedRef in PlaceCurrentReference, ScrapReference, Store and SetSelectedEditItem | VERIFIED |
| 0x30F7AF0 | `BGSKeyword*` WorkshopLinkedPrimitive (build-area primitives) | the linked-ref keyword iterated by IsLocationWithinBuildableArea | VERIFIED |
| 0x30F6EE8 | `bool`: an existing item is being edited | set by 0x397780 (begin edit), cleared by 0x397420 / scrap callback | VERIFIED (not used by CoSync) |
| 0x30F6F20 / 0x30F6F24 | current triangles / draws of the current workshop | read by the budget ratio; set by StartWorkshop from AVs | VERIFIED |
| 0x30F76CC / 0x30F76DC | max triangles / draws of the current workshop | same | VERIFIED |

## Functions

| RVA | Name | Signature (x64 fastcall) | Evidence | Status |
|---|---|---|---|---|
| 0x398CB0 | `Workshop::PlaceCurrentReference` | `bool (const ContextData*)` | CLib 2195155; body finalizes PlacementItem into hCurrentWorkshop, calls SetLinkedRef(item, workshop, WorkshopItemKeyword) when not already linked, then fires **ItemPlacedEvent** (source 0x3A9740, notify 0x3C0540, event `{workshop, item}`). Returns 1 on success, 0 when the placement is refused (the preview stays in hand). Called by the WorkshopMenu input handler 0xB2C170 (5 sites) and 0x397420. Also finishes the drop of a grabbed existing item and of wires. | VERIFIED |
| 0x393F30 | `Workshop::ScrapReference` | `void (const ContextData*, NiPointer<TESObjectREFR>*, BSTArray<BSTTuple<TESBoundObject*,u32>>* rewards /*nullable*/)` | CLib 2195125 = F4SE 0.7.9; body credits rewards to the workbench container, unlinks WorkshopItemKeyword, disables the ref, fires **ItemDestroyedEvent** (source 0x3A94D0, notify 0x3BFD90) and ItemScrappedEvent (source 0x3A98E0, CLib 2195291). Callers: RequestScrapCallback::Run 0x3C19C0, multi-select 0x39B470 / 0x3A7AD0, 0x397780. | VERIFIED |
| 0x393220 | Workshop store/remove ("Store" when arg3 = 1) | `void (const ContextData*, NiPointer<TESObjectREFR>*, bool store, u8, bool silent)` | uses `sWorkshopItemStored` (0x2EDF550) and the "UIWorkshopModeItemStore" sound; adds the base object to the workbench (0x388720); fires ItemDestroyedEvent. Called by RequestStoreCallback::Run 0x3B8180 with `(ctx, &ref, 1, 0, 0)`. | VERIFIED |
| 0x3C19C0 | `Workshop::RequestScrapCallback` vtable slot 1 (Run) | `void (this)` | vtable 0x2483828 (IDA name); holds the ref handle at +0x10, rewards at +0x18 and ContextData at +0x20; calls ScrapReference. | VERIFIED |
| 0x3B8180 | `Workshop::RequestStoreCallback` vtable slot 1 (Run) | `void (this, char button /*0 = yes*/)` | vtable 0x2483878; ref at +0x10, ContextData at +0x18; calls 0x393220(ctx,&ref,1,0,0). | VERIFIED |
| 0x3B7E00 | `Workshop::RequestRepairCallback` vtable slot 1 (Run) | `void (this, char button /*0 = yes*/)` | vtable 0x2483898; ref handle at +0x10; pays the repair cost and repairs. | VERIFIED |
| 0x3844D0 | `Workshop::IsLocationWithinBuildableArea` | `bool (const TESObjectREFR* workshop, const NiPoint3* pos)` | CLib 2194956; tests the WorkshopLinkedPrimitive refs, falling back to a radius around the workshop/location. Callers: the placement-status update 0x37E6A0 (the "outside buildable area" message), SetSelectedEditItem's edit check 0x384810, FindNearestValidWorkshop's per-workshop test 0x3A7C80, 0x39C1A0, 0x39D3E0, 0x3A1770, 0x3A3B60. | VERIFIED |
| 0x385720 | `Workshop::FindNearestValidWorkshop` | `TESObjectREFR* (const TESObjectREFR*)` | CLib 2194970 = F4SE 0.7.9; picks the nearest workshop in the same worldspace whose buildable area contains the point (through 0x3A7C80 → 0x3844D0). | VERIFIED |
| 0x37D6C0 | workshop budget ratio | `float ()` = `min(max(curTris/maxTris, curDraws/maxDraws), 1)` | the only budget test: the WorkshopMenu refuses to place while it returns 1.0 (0xB2E940 `< 1.0`; 0xB2E480 shows `sWorkshopCanBuildNoMore` at ≥ 1.0; also 0xB2C170, 0xB2EA40, 0xB33110). | VERIFIED |
| 0x3960A0 | `Workshop::StartWorkshop` | `void (TESObjectREFR*)` | CLib 2195134; sets hCurrentWorkshop, reads the budget AVs, fires WorkshopModeEvent (source 0x3A9CF0). | VERIFIED (not hooked) |
| 0x392D60 | `Workshop::SetSelectedEditItem` | `void (ObjectRefHandle*)` | CLib 2195119; sets PlacementItem and stores the item's original pos/rot in 0x30F76C0 / 0x30F76D0. | VERIFIED (not hooked) |
| 0x397420 | cancel/finish edit of a grabbed item | `void (const ContextData*)` | restores the transform captured by SetSelectedEditItem, fires **ItemMovedEvent** (source 0x3A9670, notify 0x3C02B0). Wires go through PlaceCurrentReference instead. | VERIFIED (not hooked) |
| 0x5648D0 | `TESObjectREFR::SetLinkedRef` | `void (TESObjectREFR* ref, TESObjectREFR* target /*null = unlink*/, BGSKeyword*)` | 29 callers, including every workshop function above with WorkshopItemKeyword | VERIFIED |
| 0x282F40 | `ExtraDataList::GetLinkedRef` | `TESObjectREFR* (ExtraDataList*, BGSKeyword*, bool)` | uses the LinkedRefFinder visitor; called as `GetLinkedRef(ref->extraDataList /*+0x100*/, kw, 1)` | VERIFIED |
| 0x383830 / 0x383860 | Register/UnregisterForItemPlaced | `void (BSTEventSink<ItemPlacedEvent>*)` | CLib; body = source 0x3A9740 + add/remove sink | VERIFIED |
| 0x383950 / 0x383980 | Register/UnregisterForItemMoved | same shape, source 0x3A9670 | VERIFIED |
| 0x3838F0 / 0x383920 | Register/UnregisterForItemDestroyed | same shape, source 0x3A94D0 | VERIFIED |
| 0x383BF0 / 0x383C20 | Register/UnregisterForWorkshopModeEvent | same shape, source 0x3A9CF0; event `{NiPointer workshop, bool start}` | VERIFIED |
| 0x3A9740 + 0x3C0540 | ItemPlacedEvent source + Notify | `source* ()`, `void (source*, const {REFR* workshop, REFR* item}*)` | PlaceCurrentReference's own dispatch | VERIFIED |
| 0x3A94D0 + 0x3BFD90 | ItemDestroyedEvent source + Notify | same shape | ScrapReference / Store dispatch | VERIFIED |
| 0x3A9670 + 0x3C02B0 | ItemMovedEvent source + Notify | same shape | 0x397420 dispatch | VERIFIED |

### Corrections to existing CoSync code

* `ThirdParty/Pap/GameWorkshop.cpp` (compiled into DoxCoSync) has **1.11.221**
  RVAs: `hCurrentWorkshop` 0x030EC618, ScrapReference 0x393C10,
  FindNearestValidWorkshop 0x3853F0. On 1.11.240, 0x3853F0 is the *store
  prompt*. CoSyncWorkshopSync::Tick therefore read a wrong global. The new
  code uses the verified 1.11.240 values above.
* `CoSyncWorkshopScrapHook` targets 0x393EC6, which is not a function entry
  on 1.11.240 (ScrapReference starts at 0x393F30). That hook stays disabled
  and is superseded.

## How the workshop state is represented (what must be synchronized)

* A built object is an ordinary **reference** (usually a runtime-created
  `0xFF` ref) whose base is the buildable form. Its identity to the workshop
  is a **linked ref to the workbench with WorkshopItemKeyword**; that link is
  what makes it scrappable/movable in workshop mode and counted as part of
  the settlement.
* **Position / rotation** are the ref's own `pos` / `rot`. New refs get
  their rotation at construction (CreateReferenceAtLocation); moving an
  existing static goes through MoveRefrToPosition.
* **Removal** (scrap/store) = unlink + disable (+ delete) + reward materials
  to the workbench container, then ItemDestroyedEvent.
* Settlement **stats** (food, beds, defense ...) are Papyrus
  (WorkshopParentScript) reactions to the ItemPlaced/Destroyed/Moved events;
  they are not engine state. The HOST's world is the persistent one (clients
  play in the host's save and cannot save), so the host fires those events
  for objects that clients build, and clients only need the linked objects.

## Unresolved (deliberately not relied on)

* Power-grid registration of remotely built objects (0x3883D0 is called by
  PlaceCurrentReference with the workshop ExtraData; semantics not proven).
  Wires and power from a remote builder may need re-wiring on the host.
* The begin-edit ("grab") function 0x397780 was not fully decompiled. Moves
  are gated when the object is dropped (PlaceCurrentReference returns false),
  so the player can only cancel, which puts the object back.
* Workbench **component cost** of a build is paid from the builder's local
  workbench; the host's workbench is not debited for a client's build
  (container sync is a separate system).
* Settler assignment / command mode are not covered by any permission.
