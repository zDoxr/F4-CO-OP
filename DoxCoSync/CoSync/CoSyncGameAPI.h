#pragma once

#include "NiTypes.h"
#include "GameReferences.h"
#include "GameForms.h"

namespace CoSyncGameAPI
{

    Actor* SpawnRemoteActor(TESNPC* npcBase, bool isPlayerProxy);

    // Spawns a plain (non-actor) reference of `itemBase` at `pos`, in the
    // local player's current cell/worldspace context (CreateReferenceAtLocation
    // needs a loaded interior/worldspace to place into regardless of the
    // target XYZ). Same proven engine primitive as SpawnRemoteActor, minus
    // the actor-specific post-spawn setup. Returns nullptr on failure.
    //
    // `rot` (radians, direct x/y/z -> rot.x/y/z, defaults to no rotation) is
    // passed straight into CreateReferenceAtLocation's own dir field --
    // IDA-verified (1.11.240.0, sub_1402E5110 -> sub_140513AC0) to
    // unconditionally set the new ref's rot AND fire the same transform-
    // changed notify already proven correct for position, synchronously
    // during construction. Set it here rather than rotating after spawn:
    // MoveRefrToPosition/ObjectReference.SetAngle are BOTH confirmed dead
    // for a non-actor ref post-spawn (SetAngle's own native just calls
    // MoveRefrToPosition internally) -- this is the only verified way to
    // land a static ref's orientation correctly.
    TESObjectREFR* SpawnItemAtPosition(TESForm* itemBase, const NiPoint3& pos, const NiPoint3& rot = NiPoint3());

    // One proper relocation (cell membership included) into the LOCAL player's
    // cell/worldspace -- for a network actor whose target jumped far away
    // (fast travel, door, teleport), which per-frame steering can't follow.
    void SnapActor(Actor* actor, const NiPoint3& pos, const NiPoint3& rot);

    // 0.3.4: re-file a proxy that is filed under the wrong cell for where it
    // stands (e.g. spawned while the local player was in another interior),
    // through the same engine MoveRefrToPosition path as the per-update cell
    // resync. No-op when its cell already holds its position, when its entity
    // is not in the local world, and at most once a second per actor.
    void RefileMisplacedActor(Actor* actor);

    // 0.3.4: death authority for NPC puppets. Sets/clears the actor's RUNTIME
    // essential bit (uiFlags 0x40000). With it set, the engine's SetLifeState
    // turns a lethal Dying transition into essential-down (life state 7)
    // instead of death (IDA-verified 2026-08-18, SetLifeState vtable 0x105),
    // so a machine that does not own the NPC can knock it down but never kill
    // it; KillPuppet clears the bit when the owner's KILL arrives. No-op on an
    // actor that is already dead or dying.
    void SetRuntimeEssential(Actor* actor, bool essential);

    void PositionRemoteActor(
        Actor* actor,
        const NiPoint3& pos,
        const NiPoint3& rot
    );

    // Plain-REFR counterpart to PositionRemoteActor's raw scene-graph write,
    // for short-lived cosmetic refs that must move every frame (the in-flight
    // throwable stand-ins). Deliberately does NOT go through
    // MoveRefrToPosition: that native dispatches a BSTaskPool job and runs
    // NavMesh queries, which is fine at the 2Hz debris rate but not at frame
    // rate for an object that lives ~2 seconds.
    //
    // Uses GetObjectRootNode (TESObjectREFR vtable slot 0x8C) rather than any
    // Actor-level slot -- 2026-08-03 exe check: the TESObjectREFR vtable is
    // only 198 slots, so Actor's SetPosition (0xCA) would run off the end of
    // it on a plain ref. 0x8C is the REFR-level primitive that is actually
    // safe here.
    //
    // yaw rotates about Z (world up); tumble rotates about Y, giving a thrown
    // grenade its spin. Both in radians. No cell/worldspace bookkeeping is
    // done -- the ref must already have been created in the local player's
    // cell (SpawnItemAtPosition does that) and must not outlive it.
    void PositionRefRaw(
        TESObjectREFR* refr,
        const NiPoint3& pos,
        float yaw,
        float tumble
    );

    bool GetActorWorldTransform(
        Actor* actor,
        NiPoint3& outPos,
        NiPoint3& outRot
    );

    void DisableActorAI(Actor* actor);
    void EnableActorAI(Actor* actor);

    // ── Partner-style "puppet" mode for world-NPC (enemy) proxies ────────────
    // Keep the engine AI + behavior graph ALIVE so the actor animates natively,
    // and only STEER it over the network (native SetPosition each tick +
    // avoidance off) instead of AI-disabling it and hand-driving a 299-variable
    // graph (which A-poses on any data gap). Player proxies are unaffected.
    // Mirrors commonwealth-online's F4TNpcSync (NeutralizePuppet + SetPosition
    // steering). Flip to false to A/B against the legacy AI-disabled path.
    inline bool PuppetModeEnabled() { return true; }

    // Actor vtable slot 0xCA — SetPosition(NiPoint3&, updateCharController=true):
    // native, cell/worldspace-aware move (unlike PositionRemoteActor's raw
    // NiNode write). headingZ is written to actor->rot.z (== GetHeading) first.
    void SteerPuppetPosition(Actor* actor, const NiPoint3& pos, float headingZ);

    // 0.3.3: set the actor's Havok character-proxy linear velocity (game
    // units/s). IDA 1.11.240: the controller is process(+0x300)->middleHigh
    // (+0x08)->+0x3E0 (sub_140C5CD50 / sub_140D347C0); its vtable slot 0x3A
    // (bhkCharProxyController sub_1418B7110) stores an hkVector4 at
    // characterProxy(+0x470)+0xA0, slot 0x39 (sub_1418B70E0) reads it back.
    // Actor::SetPosition(pos, true) passes the zero vector to this slot, so
    // every steer used to leave the puppet "standing still" to the engine.
    void SetActorControllerVelocity(Actor* actor, const NiPoint3& velocity);

    // Actor vtable slot 0xC8 — keeps an AI-alive puppet from side-stepping the
    // network-steered position. VERIFIED NO-OP in build 1.11.221.0 (checked
    // against Fallout4_disk.i64 and commonlibf4's own Actor.h, which declares
    // this exact slot as a literal inline no-op) -- kept only because it's
    // free/harmless and the reference implementation calls it too; the real
    // work is StopActorCombat below.
    void SetActorAvoidanceDisabled(Actor* actor, bool disabled);

    // Actor vtable slot 0x100 — StopCombat(), gated on IsInCombat(). The half
    // of the reference F4TNpcSync::NeutralizePuppet pattern that actually does
    // something: without this, a puppet enemy whose local AI re-enters combat
    // (e.g. against the local client's own player) keeps running its own local
    // movement/attack logic every tick, fighting SteerPuppetPosition's
    // network-authoritative placement. Call at puppet-bind time AND
    // periodically thereafter (TickSmoothing) since local combat can start at
    // any point during a long session, not just at spawn.
    void StopActorCombat(Actor* actor);

    // Native puppet death: SetLifeState(kDying, slot 0x105) + KillDying (0xCB) —
    // full ragdoll/loot, unlike a bare HP=0 base-value write.
    void KillPuppet(Actor* actor);

    // Engine life state, ActorState bits 17-20 at actor+0x130 (IDA 1.11.240:
    // Actor::SetLifeState, vtable 0x105 = sub_140C83990). 0 alive, 1 dying,
    // 2 dead, 7 essential-down (the CK enum). -1 if unreadable.
    int  GetLifeState(Actor* actor);
    bool IsDeadOrDying(Actor* actor);

    // Returns the actor's REAL current combat target (the actor its AI is
    // fighting), or nullptr when it has none. Reads the BSPointerHandle at
    // Actor+0x380 (IDA-verified via the console GetCombatTarget handler) and
    // resolves it through the engine handle table. Host-side use only — lets
    // every peer's puppet attack the same actor instead of a distance guess.
    Actor* GetCombatTargetActor(Actor* actor);
    void EnableProxyActor(Actor* actor);
    void QueueProxyFactionRank(Actor* actor);
    void DespawnProxyActor(Actor* actor);

    // Round 123: Disable()+Enable() cycle queued on the ref's Papyrus VM
    // queue (executes in order) — forces the engine to tear down and rebuild
    // the ref's 3D with full appearance resolution (template/facegen/outfit).
    // Fix for proxies spawning as invisible skeletons: nameplate + anim graph
    // + root node all present, but no body geometry.
    void RefreshActor3D(Actor* actor);

    // 2026-08-26: RefreshActor3D has TWO independent callers (weapon-verify in
    // CoSyncPlayerManager.cpp, outfit-verify in CoSyncOutfitSync.cpp), each
    // previously keeping its OWN separate 5s-per-entity throttle map. Live log
    // from a real test session showed both firing on the same proxy within
    // seconds of each other repeatedly -- two independent full Disable+Enable
    // body rebuilds racing each other and the scheduled post-refresh weapon
    // re-equip that has to follow EVERY rebuild (Enable() drops weapons, see
    // CoSyncPlayer.h's m_postRefreshWeaponDelay comment). That's a strong
    // candidate for "weapon plays its animation but the mesh never actually
    // lands" -- a re-equip landing just as a SECOND, uncoordinated rebuild
    // tears the body down again. This coordinates BOTH call sites through one
    // shared per-entity cooldown instead of two that know nothing about each
    // other. Returns true (and calls RefreshActor3D) if at least
    // cooldownSeconds have passed since the LAST refresh for this entityID
    // from ANY caller; false (no-op, nothing called) otherwise. Main-thread
    // only, same as RefreshActor3D itself.
    bool TryRefreshActor3DThrottled(uint32_t entityID, Actor* actor, double cooldownSeconds = 5.0);

    // 0.3.3 (Stage 3): the engine's own "rebuild this actor's worn 3D"
    // request. IDA 1.11.240: Actor::AddWornItem (Actor vtable 0x6F,
    // sub_140CAEBC0) ends an armor equip with sub_140D363C0(process, 1) and
    // (process, 0x10), which OR the flags into the middle-high process data
    // (process+0x08)->update3DModel (+0x496). Actor::Update (vtable 0x13,
    // sub_140C8B5A0) consumes them next frame through sub_140D02600 ->
    // sub_140D27280 when the actor has 3D. sub_140D363C0 silently does
    // nothing when process+0x08 is null (actor not in middle/high process),
    // which is how an equip can land in the inventory with no visual change.
    //
    // HasMiddleHighProcess: would an equip's model-update request stick now?
    // RequestActorModelUpdate: issue that same request (flags 0x11); returns
    //   false (and does nothing) when it would be dropped.
    bool HasMiddleHighProcess(Actor* actor);

    // Is formID in the actor's inventory, and is any stack of it equipped?
    // Liveness-checked and SEH-guarded (a rebuilt proxy leaves stale actor
    // pointers behind). Returns false if the actor could not be read.
    bool GetInventoryEquipState(Actor* actor, uint32_t formID, bool& present, bool& equipped);
    bool RequestActorModelUpdate(Actor* actor);

    // Queues SetEssential(false), SetProtected(false), and
    // SendAnimationEvent("IdleForceDefaultState") on an NPC proxy actor.
    // Call after RefreshActor3D — VM queue ordering ensures these fire after
    // the Enable() that RefreshActor3D queues, so the actor is fully loaded
    // before the animation event and flag clears land.
    void InitNpcProxy(Actor* actor);

    // Queues ObjectReference.Disable(false) on a world-placed NPC (quest NPC,
    // etc.) without Delete() — used when the client needs to suppress their
    // original NPC copy after a synced clone has been spawned in its place.
    // Unlike DespawnProxyActor, does NOT call Delete() because the ref is
    // a real world actor, not a CoSync-created FF-ref proxy.
    void DisableWorldActor(TESObjectREFR* refr);

    // Symmetric native-enable counterpart to DisableWorldActor, for the same
    // "works on refs Papyrus/console Enable silently refuses" reason. Used to
    // run a Disable+Enable appearance-rebuild cycle on a plain (non-Actor)
    // ref spawned via CreateReferenceAtLocation, same rationale as
    // RefreshActor3D but without requiring an Actor.
    void EnableWorldActor(TESObjectREFR* refr);

    // Extra-data type ID stamped by ExtraPowerArmor's constructor at object
    // offset 0x12 (IDA-verified 2026-08-02, sub_1402B1DF0).
    static const uint8_t kExtraTypePowerArmor = 0xBB;

    // Ground-truth answer to "is this actor ACTUALLY in power armor?", read
    // straight out of the engine's own ExtraDataList presence bitfield rather
    // than inferred from whether our own call appeared to succeed.
    // Returns 1 = in PA, 0 = not in PA, -1 = unreadable/invalid actor.
    int GetActorPowerArmorExtraState(Actor* actor);

    // Enters `actor` into `chassis` by calling the engine's real PA-enter
    // internal directly, bypassing the Papyrus VM entirely -- same rationale
    // (and same proven shape) as CoSyncEquipHook::EquipItemDirect bypassing
    // Papyrus EquipItem. Logs the ExtraPowerArmor presence bit before and
    // after so a silent refusal is visible instead of inferred.
    // Returns false only if the call itself faulted or the args were invalid;
    // check GetActorPowerArmorExtraState for whether PA state actually took.
    bool SwitchToPowerArmorDirect(Actor* actor, TESObjectREFR* chassis);

    // Directly injects `form` into `refr`'s BGSInventoryList from C++ without
    // using the Papyrus VM. Returns true when the item is now in inventory
    // (freshly injected or was already present). The injection is synchronous —
    // call EquipItemDirect in the same tick, no async delay needed.
    //
    // Root cause context: Papyrus CallFunctionNoWait("AddItem") silently fails
    // for all form-arg calls on the RMPlayer proxy NPC (confirmed across every
    // session since 2026-07-05). No-arg calls (Disable/Enable/StartCombat)
    // work fine, but AddItem/EquipItem with form handles never land.
    //
    // 2026-08-26: widened from Actor* to TESObjectREFR* -- `inventoryList` is
    // a TESObjectREFR-level field (GameReferences.h), not Actor-specific, and
    // live-log evidence showed the EXACT same silent-AddItem-failure class
    // hitting a plain FURN chassis ref (PA piece installation: 6 CallFunctionNoWait
    // AddItem calls queued, only 1 unrelated item ever landed in the chassis's
    // inventory). Every existing call site passes Actor*, which upcasts to
    // TESObjectREFR* implicitly -- no caller changes needed.
    bool DirectInjectInventoryItem(TESObjectREFR* refr, TESForm* form);

    // Precondition for calling the engine's equip/unequip directly on a proxy:
    // every inventory stack of `item` must own an ExtraDataList. The engine's
    // stack walk inside EquipObject (sub_140CDFCD0) reads each stack's list
    // with no null check, but the engine itself creates stacks without one
    // (lists are made lazily) -- bug E1, a crash at Fallout4+0x165940F.
    // Creates any missing list the engine's own way (MemoryManager::Allocate
    // + the ExtraDataList constructor, under the inventory write lock).
    // Returns the number of lists created, or -1 if the ref/constructor is not
    // usable. MAIN THREAD.
    int EnsureStackExtraLists(TESObjectREFR* ref, TESForm* item);

    // Makes the FIRST inventory stack of a weapon carry its instance data, the
    // engine's own way, and returns that instance (owned by the stack's list;
    // not AddRef'd). Equipping with {form, this instance} lets EquipObject
    // find the stack. Equipping with an instance no stack owns makes
    // BGSInventoryItem::SetWorn (sub_14034AF10) split a count>1 stack into a
    // new stack with a NULL ExtraDataList, which EquipObject's next stack walk
    // (sub_140CDFCD0 -> sub_14027F090) dereferences: the client crash at
    // Fallout4+0x165940F on 2026-09-29 (IDA-verified, see
    // docs/NPC_COMBAT_WORLD_REPORT.md). Uses ExtraDataList::GetInstanceData
    // (sub_14027F090) and ::SetInstanceData (sub_14027F120). Needs
    // EnsureStackExtraLists first. Returns nullptr if anything is unusable.
    // MAIN THREAD.
    TBO_InstanceData* BindWeaponInstanceToStack(TESObjectREFR* ref, TESForm* weapon);

    // True for an item ref CoSync spawned (SpawnItemAtPosition) that now has
    // neither a worldspace nor a parent cell. The engine's save writer
    // (sub_140BF72F0, location cases 4-6) reads ->formID of
    // sub_140BF82F0(ref) = worldspace ?: parentCell with no null check: the
    // 0.3.0 host crash at Fallout4+0xBF74BD on 2026-09-28 (ref 0xFF000EC8, a
    // CoSync item copy). The save filter keeps such refs out of the save.
    bool IsDetachedCoSyncItem(TESObjectREFR* ref);

    // The engine's own AddItem (what Papyrus AddItem runs), called directly --
    // works on refs without a script object (proxies). Synchronous. MAIN THREAD.
    bool NativeAddItem(TESObjectREFR* target, TESForm* item, UInt32 count);

    // The engine's own RemoveItem (what Papyrus RemoveItem runs), called
    // directly: the items are destroyed. Synchronous. MAIN THREAD.
    bool NativeRemoveItem(TESObjectREFR* target, TESForm* item, UInt32 count);

    // The engine's AttachModToInventoryItem, called directly (works on proxies).
    // ref must hold exactly one of item. Synchronous. MAIN THREAD.
    bool NativeAttachMod(TESObjectREFR* ref, TESForm* item, TESForm* mod);

    // Queues ObjectReference.MoveTo(target, 0,0,0, matchRotation=false) via
    // CallFunctionNoWait to move `actor` onto `target`'s current position.
    // Same primitive CoSyncPlayerMarker uses for map-marker tracking -- MoveTo
    // handles cell/worldspace transitions correctly, unlike PositionRemoteActor's
    // raw scene-graph write. Test-only teleport button entry point.
    void TeleportActorTo(Actor* actor, TESObjectREFR* target);

    // Returns true when form is non-null, readable under SEH, and
    // LookupFormByID(form->formID) returns the exact same pointer — confirming
    // the engine still owns this object and hasn't freed/reused it.
    // Call before any CallFunctionNoWait(form, ...) when the pointer may be
    // stale (e.g. cell-transition UAF: actor freed while our maps hold it).
    bool IsFormStillLive(TESForm* form);

    // Toggles kFlagEssential on the LOCAL PLAYER's own NPC record (base 0x7)
    // for the duration of a session. NOT part of the (removed) revive gameplay
    // -- this is purely a CRASH GUARD: while Essential the engine's own
    // collapse/bleedout handling applies, so lethal damage never reaches the
    // vanilla death menu -> "Continue" -> LoadGame, which crashes on a CoSync
    // proxy ref baked into the save (confirmed 2026-08-08 host crash: unwind
    // through BGSLoadFormBuffer / LoadGameMissingContentCallBack / ExtraAttachRef).
    // A death-menu reload is incompatible with multiplayer anyway (it reloads
    // only one peer's whole game). Call true on connect, false on disconnect.
    void SetLocalPlayerEssential(bool essential);

    // ── On-screen notification (the vanilla "Debug.Notification" toast) ──────
    //
    // ShowNotification calls the engine's HUD-message helper directly. It takes
    // a raw const char* -- no Papyrus VM, no BSFixedString, no marshalling.
    //
    // RVA 0x00A36F60, verified 2026-08-04 against Fallout4.exe 1.11.221.0 by
    // reading the disassembly, not by trusting a header:
    //   "Debug"/"Notification" strings (0x025BA510 / 0x025BA600) sit adjacent
    //   -> the Debug native registration at 0x0111DD00 stores impl 0x0111C510
    //      into NativeFunction+0x50
    //   -> 0x0111C510 empty-checks the BSFixedString, converts it to char*,
    //      sets (edx=0, r8d=1, r9b=1) and TAIL-JUMPS 0x00A36F60.
    // ~133 direct callers across the binary, so this is a mainstream engine
    // helper rather than an edge path. See [[reference-fo4-ui-notification-rvas]].
    //
    // MUST be called on the game thread.
    void ShowNotification(const char* text);

    // Thread-safe entry point for code running off the game thread (the whole
    // network receive path). Queues the text; FlushNotifications drains it.
    void QueueNotification(const char* text);

    // Drains queued notifications. Call once per tick from the game thread.
    void FlushNotifications();
}
