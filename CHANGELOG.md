# DoxCoSync Changelog

---

## [0.0.95] — 2026-09-26

### Client no longer inherits the host's Power Armor; host crash in outfit verify fixed

Built clean, deployed. **Not yet live-tested.**

- **Host's Power Armor copied to the client:** the client plays in a copy of the host's save, so if the host was wearing Power Armor when the world was saved, the client loaded in wearing it and its fresh character was built inside the host's frame (the inventory clear then stripped the worn pieces). The client now ejects first (native SwitchFromPowerArmor 0x98BE40, via new `CoSyncPowerArmorSync::IsLocalPlayerInPowerArmor` / `ExitLocalPlayerPowerArmor`), waits up to 10 s for the exit, hides its copy of that frame (the host's proxy wears it), and only then clears the inventory and builds the character.
- **A joining player starts with nothing -- like a new save:** the wipe now uses mode 0 of `TESObjectREFR::RemoveAllItems` (no target), which is what the console `removeallitems` does: everything goes, quest items and keys included. Only the Pip-Boy is given back. The wipe also runs the moment the host's world finishes loading (`CoSyncProfiles::ClientStripInheritedInventory` from `CoSyncWorldTransfer::OnPostLoadGame`), so the host's gear is never visible. If the client loaded in the host's Power Armor, it is ejected first and wiped once out. A returning player's saved items are then added on top.
- **Host crash** `FlushPendingOutfitOps+0x1B45` (READ AV through a garbage inventory list): the phase-2 equip and verify passes walked `actor->inventoryList` of a registry actor that could be stale (proxies are rebuilt on cell changes). Both now go through `SafeFindInProxyInventory` (liveness check + SEH), and a stale entry is skipped and dropped instead of dereferenced.

---

## [0.0.94] — 2026-09-26

### Native add/remove corrected -- 0.0.92/0.0.93's "native add" was the engine's RemoveItem

Built clean, deployed. **Not yet live-tested.**

- IDA labels the AddItem and RemoveItem functor vtables swapped. 0x141182A90, wired in 0.0.92 as the AddItem worker, is really **RemoveItem**'s: RemoveItemFunctor::Run 0x14114E930 reports "Cannot remove a None item from a container". The real add worker is **0x141173A90** `(container, item, count, silent, stackID, vm)`, called by AddItemFunctor::Run 0x14114E210 ("Cannot add a None item to a container"). It resolves the item list and adds through the container's own add path (0x140503FD0).
  - Effect of the mistake: proxy weapons and armor fell back to the old hand-built stacks (the "did not land" warning), so proxy weapons were still invisible. Pieces added to Power Armor frames via PAI never arrived, because the call "succeeded" and skipped the Papyrus fallback.
- `CoSyncGameAPI::NativeAddItem` now calls 0x141173A90. New `CoSyncGameAPI::NativeRemoveItem` calls 0x141182A90 `(container, item, count, silent, destination=null, a6, stackID, vm)`. Both are prologue-checked, SEH-guarded, and pass the game VM only for script-error reporting.
- Power Armor frame pieces are now added **and removed** natively (the Papyrus call is only a fallback).
- A restored character's inventory is added natively too (synchronous, same reason as the 0.0.92 removal fix).

---

## [0.0.93] — 2026-09-26

### Phase 8 -- proxy fidelity: weapon mods, armor mods and Power Armor paint on other players

Built clean, deployed. **Not yet live-tested.**

- New `Character/CoSyncItemMods`. Every player reads the object mods on its **equipped** weapons and armor once a second and sends `IMODS|entity|item:mod/mod,...` when they change (and every 20 s for late joiners); the host relays it. Receivers compare each proxy item's current mods with the wanted ones once a second and attach anything missing, so it re-applies after a proxy respawns or re-equips. A mod that fails three times is given up (logged).
- Mods are attached with the engine's own `AttachModToInventoryItem` worker (0x141180DE0, synchronous, refreshes the equipped item), called directly: new `CoSyncGameAPI::NativeAttachMod`. It works on proxies, which have no script object.
- Power Armor paint is a mod on each piece, and in Power Armor the pieces are equipped on the wearer, so paint travels the same way.
- Armor on actor proxies is now also added natively (like weapons in 0.0.92), so mods land on a real inventory stack.
- Face/body changed mid-session (barber, surgeon, mirror) now reaches everyone: each player re-checks its own look every 5 s and resends it when it changed.
- **Unworn Power Armor frames with mixed pieces:** pieces were already synced by count (`PAI`), but their mods never were. New `PAMODS|sender|ref|base|x|y|z|ITEM:MOD/MOD,...` comes from the same frame scan with its own change detection, so a repaint or new mod at a station is sent. Receivers find their copy of the frame the same way as `PAI` and attach the mods, retrying for about 10 s while pieces are still arriving, with echo suppression. Pieces added to a frame now use the engine-native add (the Papyrus call is only a fallback), and the chassis spawned for a remote player in Power Armor gets real piece stacks. The hand-built ExtraDataList behind the 2026-09-25 crash is no longer used for Power Armor.
- Limitation (engine rule): mods are only applied when the proxy carries exactly one copy of that item.

---

## [0.0.92] — 2026-09-26

### Fresh character really empty, stranded-in-Vault-111 fix, closer editor camera, companions follow teleports

Built clean, deployed. **Not yet live-tested.**

- **Fresh character inventory:** the per-item Papyrus `RemoveItem` calls were queued and partly lost across the teleport into the editor room (9 of the host's items survived). Now one synchronous engine call: `TESObjectREFR::RemoveAllItems` (0x140505E40, mode 3, no target), the same call Papyrus `RemoveAllItems` and the console command make, followed by the player inventory update (0x140D60DA0). Quest items and keys stay (engine rule). The Pip-Boy is given back if it was removed. The kept-item count is logged.
- **Stranded in Vault 111 after the editor:** the return target was taken from the host proxy's *cell*, but WorldSync rebuilds proxies into the local player's cell. While the player was in the editor room, the host proxy was "in Vault 111" too, so the player was returned inside the vault (and Vault 111's own scripts then kept pulling them back to the cryopods). It now uses the host's real worldspace or interior from its WorldSync reports (`lastRemoteWorldspaceID` / `lastRemoteCellFormID`), falling back to where the player loaded in.
- **Editor camera** zooms 0.25 past the normal minimum (the engine clamps zoom only in its mouse-zoom input branch).
- **Companions (and every AI puppet) follow teleports:** puppets are steered with `SetPosition`, which never changes cell, so a companion whose owner fast-travelled stayed behind. A jump of more than 2000 units now relocates the puppet once through `MoveRefrToPosition` (new `CoSyncGameAPI::SnapActor`, at most once a second), then steering continues.
- **Remote players' weapons invisible (animations played, no model):** proxy weapons were hand-built inventory stacks, which never got a weapon model. Weapons on proxies are now added through the engine's own AddItem worker (0x141182A90, the function Papyrus AddItem's queued functor runs), called directly because proxies have no script object. The stack is then complete and EquipObject builds the model. Proxies show the weapon's standard configuration (mods aren't copied). Armor and Power Armor frame contents keep the old path. New `CoSyncGameAPI::NativeAddItem`.

---

## [0.0.91] — 2026-09-26

### Weapons destroyed by proxy weapon injection, real UI pointer, shared-world companions

Built clean, deployed. **Not yet live-tested.**

- **Weapons vanishing (host and clients, carried into solo saves):** `DirectInjectInventoryItem` gave a proxy's weapon a *shallow* copy of the local player's `BGSObjectInstanceExtra::Data`, so the proxy and the real player shared one mod array. When the proxy or its weapon was destroyed, the engine freed that array and the player's own weapon was left pointing at freed memory: it broke or disappeared, and every later save kept it that way. The array is now deep-copied. Saves written while the bug was active may already be missing weapons; they can't be repaired from here.
- **UI singleton corrected** to 0x030E8930 (`g_ui` and `g_ui_fixed`). IDA: the UI constructor stores itself there, LooksMenu registers on it, and Game.ForceThirdPerson queries it. The old 0x030E88B0 is BSInputEnableManager, and every menu lookup through it failed ("menuTable size=0"). The loading, pause, Pip-Boy and dialogue menu checks and the main/pause menu lookups now work. The MainMenu open/close sink registration was dropped: its RVA (0x666210) is an empty `ret` on 1.11.240 and its dispatcher offset was wrong, so it had never registered anything. The pause-menu DISCONNECT handler now ignores a second activation of the same press.
- **Companions follow the shared-world rules** (new `Character/CoSyncCompanions`):
  - The host's companions are its real world actors. `CoSyncNpcWorldSync` now syncs the host's teammates, so clients see them following the host, and only the host commands them.
  - Followers inherited through the shared save (which would follow the client) are handed back: the client disables its local copy and the host's stream shows the real one.
  - A client's companion is its own copy: spawned on the client and made its companion through the vanilla Followers quest (`SetCompanion` / `SetDogmeatCompanion`), so it follows and obeys only that client. It is registered with the host as client-owned (`COMPOWN`, no de-duplication), and others see it following that client. The host and every client may all have the same companion. Dismissing removes that copy.
  - A client's companions are saved with its character (`comps=`) and brought back on rejoin.
  - Recruiting no longer makes the host spawn a duplicate (the old COMP_REQ-per-recruit path).

---

## [0.0.90] — 2026-09-26

### PrismaUI multiplayer menu, built-in LAN sessions (8 players), and one shared world held by the host

Phases 1-7 of the multiplayer overhaul. Every piece built clean and was deployed; items marked **not yet live-tested** have not been run in game.

**Multiplayer menu (PrismaUI) -- replaces ImGui**
- New `GUI/CoSyncPrismaUI`: the MULTIPLAYER row on the main menu and F10 open a PrismaUI view (`views/DoxCoSync/multiplayer.*`) styled after Fallout 4's own menus and coloured with the player's HUD colour (Pip-Boy green by default). Screens: Host, Join (LAN list), Direct Connect, Session (live roster), Settings. Keyboard, mouse and controller; vanilla menu sounds (PlayUISound RVA 0x01086570).
- The vanilla main menu is frozen while the view is open (`SetMainPanelFrozen`: list hidden, `disableInput`/`disableSelection`), which fixed clicks falling through and a crash when QUIT was clicked underneath.
- New teammate HUD widget (`views/DoxCoSync/hud.*`): one card per teammate with name, level, health, radiation and AP. Vitals are sampled at 10 Hz and sent on a 0.1 change plus a 1 s resync; bars ease over 150 ms, so AP regen and rads read like the vanilla meters.
- ImGui retired: `GUI/DX11Hook`, `GUI/CoSyncMapOverlay` and `ThirdParty/ImGui` deleted (no more Present hook, d3d11 or D3DCompiler imports). `GUI/CoSyncWindowHook` (WndProc subclass installed from the main-thread tick) now handles F10, the pause-menu DISCONNECT row and WM_CLOSE. `CoSyncOverlay` is data-only. The Pip-Boy map dots and the ImGui loading logo were removed.

**Sessions: LAN discovery, 8-player cap, no Hamachi**
- New `Networking/CoSyncDiscovery`: UDP 48001 query/announce (`CSYN` magic), also on loopback so two instances on one PC find each other. Clients connect to the reply's source address. Hamachi support removed (`HamachiUtil` deleted).
- `GNS_Session`: binds all interfaces with port fallback 48000-48007, enforces the 8-player cap when a client connects (reason 1001 SESSION FULL), tracks client end reasons for the UI, recreates the poll group per session. `EndSession()` runs before `GNS_Shutdown`.
- Version check: a mismatched build is kicked with reason 1002 (VERSION MISMATCH).
- One client leaving no longer tears down the host's whole session: GNS posts a local-only `PEERLEFT|sid`, and a client's `DISCONNECT` is handled per peer.
- Live roster from real player-manager state (`CoSyncNet::GetRoster`, `GetJoiningCount`).
- Multi-instance testing: `CoSyncMultiInstance_Index` export (per-instance SteamID salt with Goldberg), `bAlwaysActive=1` so unfocused instances keep running.
- Removed a 1 Hz access violation from a diagnostic in `CoSyncFirstPersonGraph` (`ValidateAgainstNodeArray`).

**Shared host save (Phase 7)** -- the host's save is the world; nothing is lost between sessions.
- World transfer (`Networking/CoSyncWorldTransfer`): a joining client sends `WORLDREQ`; the host saves `CoSync_World` and streams the `.fos` plus F4SE co-save (`WORLDBEGIN`/`WORLDCHUNK` 128 KB/`WORLDEND`, CRC32 each). The client writes `CoSync_Joined` and loads it; HELLO and the session only start once it has loaded. GNS send/receive buffers were raised to 32 MB and the send rate to 32 MB/s (a 5 MB world now arrives in about 0.5 s instead of 17 s).
- Engine save/load by name (`World/CoSyncSaveLoad`, IDA-verified 1.11.240 RVAs, prologue-checked before every call). The client load skips the "Creations are enabled" prompt, which had aborted every join.
- CoSync proxies are kept out of every save: MinHook on the reference should-save check (0x4F59F0).
- Player characters (`Character/CoSyncProfiles`), stored by the host inside its co-save (`PROF`): level, XP, SPECIAL, perks, inventory with what is equipped, face/body, where the player last stood, and weapon/armor mods. Clients upload every 30 s, after the character editor and when leaving.
- First join is a brand-new character with an empty inventory (the Pip-Boy stays). The character editor opens in the Vault 111 cryo room in front of the player's own pod, with the camera swung to the front and zoomed on the head, and returns the player to the host afterwards. A character only counts as saved once the editor has been finished, so an interrupted one starts over. Actor values are looked up by their ESM record IDs; F4SE's `avName` is not a string on 1.11.240 and crashed.
- Client saves are blocked (pause menu, quicksave, console, autosave, exit save, scripts) with a short message; both engine save entry points are hooked before the save file is created. Saving is allowed again after loading another save or starting a new game.
- When a session ends for a client (leave, host gone, connection lost) the game fades back to the main menu (Game.QuitToMainMenu native).
- Returning players are put back where they last stood.

**Phase 7d -- not yet live-tested**
- Other players see your real face and body: CoSync.esp gains six copies of the proxy NPC (`RMPlayer2`-`RMPlayer7`, 0x00B6CF-0x00B6D4). Each remote player gets their own base, and `LOOKS|entity|blob` is sent before HELLO (and after the editor) so the proxy spawns with the right look. Later changes rebuild the face, or fully reload the proxy on a male/female change. Every "is this a CoSync proxy base?" check now knows all seven bases.
- Weapon and armor mods (including legendary effects) are saved with the character and re-attached on rejoin (`AttachModToInventoryItem`). Limitation: one modded copy per base item.
- Seven map markers instead of four (CoSync.esp 0x00B6D5-0x00B6D7), one per other player in an 8-player session.
- **Power Armor crash fixed at the root:** `DirectInjectInventoryItem` hand-builds an `ExtraDataList` and left `+0x10` null. IDA shows it is the list's tail pointer (ExtraDataList::Add at 0x1402726A0: `*tail = node`), so the first engine-side append during SwitchToPowerArmor wrote to address 0 (Fallout4.exe+0x2726E3). The tail is now initialised, and the object-instance presence bit is set when that entry is linked.

**Known issues found along the way**
- CoSync's `g_ui` (0x030E88B0) is actually BSInputEnableManager on 1.11.240; the real UI singleton is 0x030E8930. Only `CoSyncRuntime_IsMenuOpen` (used by the character editor) was switched so far. The loading/pause/Pip-Boy polls and `g_ui_fixed` still use the old address.
- Companions are not yet part of a player's saved character.

---

## [0.0.89] — 2026-09-05

### Ported from the feature branch: Power Armor frame-inventory sync + enter/exit fallback, workshop placement context + moves, NPC corpse/weapon fixes

These were first built against the older public snapshot (Desktop\DoxCoSync) and re-applied here on top of the 0.0.88 code. Built clean, deployed. **Not yet live-tested on this tree.**

**Power Armor**
- New `PAI|sender|ref|base|x|y|z|fid:count,...` packet: inventory snapshot of an UNWORN frame. Both roles poll frames within 3000 units once a second and broadcast when the ARMO/AMMO contents change (host also publishes the initial snapshot); receivers resolve the frame (persistent ref, else `FindLocalChassisInCell` by base+position) and `AddItem`/`RemoveItem` to match, with a 3 s per-frame echo-suppression window. Frames worn locally, or bound to a proxy (`m_paRealFrameRefFormID` / `m_paSpawnedChassisRefFormID`), are skipped. Covers pieces and fusion cores swapped at a station or via the transfer menu, which previously only reached peers on the next enter.
- C++ enter/exit **fallback**: the engine swaps the wearer's race to `PowerArmorRace` (0x0001D31E) while in a frame. Every 250 ms the local player's race is compared with the Papyrus-reported state (`IsActorKnownInPowerArmor`); if they disagree for 1.5 s the transition is reported from C++ (chassis from `furnitureHandle1`, else nearest keyword-matched frame within 300 units; pieces via `ScanActorEquippedPAPieces`). Papyrus `OnSit`/`OnGetUp` stays the primary path.
- `CoSyncPowerArmorSync::Reset()` added and called from `CoSyncNet::Init`.

**Workshop / Settlement**
- `WBF|` gained trailing `worldspaceFormID|cellFormID|refFormID|workshopFormID` (parser tolerant of the old 8-field form). `AnnounceBuilding` (shared by the host and client scans) fills them from the placed ref and records rotation + placement context in `BuildTrack`.
- Receiver defers a build until it is in the right place: exterior needs the same worldspace within 12000 units, interior needs the same cell (TTL 10 min, retried every 0.5 s). `SpawnItemAtPosition` lands in the local player's cell, so exteriors are followed by a `MoveRefrToPosition` with `cell=nullptr` to let the engine re-home the ref by position.
- `(senderEntityID, senderRef) -> localRef` mapping. A `WBF` for a known ref is a **move**: position-only moves use `MoveRefrToPosition`; a rotation change respawns (rotation cannot be set post-spawn on 1.11.240) and remaps. `TickBuildMoves` re-announces tracked objects whose transform changed (>1 unit / >0.01 rad, 0.5 s min interval) -- this is what corrects the "announced while still being positioned" case the 0.6 s settle window lets through. Before spawning, the receiver also dedupes against an identical base within 4 units (shared saves), then the existing 60-unit/10 s recent-build guard.
- `WDS` scraps resolve through the mapping first; scrap hook and every removal path call `NotifyLocalBuildingRemoved` so the deletion sweep does not double-report and stale mappings are dropped.

**NPC / Enemy**
- **Weapons:** `CoSyncPlayerManager::Tick` discarded weapon changes for every world-actor entity, so the `equippedWeaponFormID` the host already streams for enemies was never applied (enemies fired with empty hands). The inject+equip path now runs for NPC proxies; only `isMyOwnRealActor` entities are excluded.
- **Pre-dead enemies standing up:** `IsActorDead` now checks the ACHR "Starts Dead" record flag (0x200) first -- pre-placed corpses keep positive Health, so the host registered them and clients replaced the corpse with a live idle clone.
- **Corpses resurrecting:** dead actors are no longer treated as departed the instant they die. They stay registered until they unload / leave range, the `DESTROY` then carries `kDestroyReason_Dead`, and `ClientUnbindWorldActor` keeps the proxy corpse and does NOT re-enable the disabled original (alive in the client's save). `KILL` applied locally is remembered (`MarkEntityKilled`) for out-of-order packets; the 5 s `CREATE` re-broadcast for a dead entity is followed by a `KILL` for late joiners; host skips `EU` for dead entities. Also removes the actorValueOwner touch from the departure sweep.
- **Dead on host, alive on client:** `NPCSEEN` for a persistent ref that is dead on the host now gets an `NPCDEAD|refFormID` reply (5 s throttle) and the client `Kill()`s its local copy, instead of the host allocating a live client-owned entity and disabling its own corpse.
- `ClientUnbindWorldActor` unregisters the clone from the TickHook before the `CoSyncPlayer` is destroyed.

---

## [0.0.88] — 2026-08-26

### Bending the engine: forcing a real state edge for PA piece equip instead of a possibly-no-op re-equip

Pattern-matched the piece-rendering problem against a bug already found and fixed earlier tonight: `DrawWeaponMagicHands` turned out to be a state-machine action that silently no-ops when called with the state the actor is already in (the weapon-vanish-after-draw bug). `ActorEquipManager::EquipObject` -- the same underlying engine call behind `CoSyncEquipHook::EquipItemDirect` -- plausibly has the identical "already equipped, nothing to do" early-out that any equip-manager would reasonably have. The native `SwitchToPowerArmorDirect` call already runs its OWN internal `EquipObject(applyNow=0)` on each PA piece, which likely marks the item equipped even though the deferred visual never applies -- so the follow-up `EquipItemDirect(applyNow=1)` call added in [0.0.84] may have been silently doing nothing at all every single time, applyNow or not, because the engine already considered the item equipped and skipped straight past the visual-attach step.

Fix: force a genuine state transition instead of a same-state call that might no-op -- `UnequipItemDirect` immediately before `EquipItemDirect` for every PA piece, in both the fresh-enter and mid-session piece-update paths. Same fix shape as the weapon case (holster then redraw), applied to the equip-manager instead of the weapon-draw state machine.

This is a genuine "bend the engine" move: reusing an already-proven pattern from this exact codebase rather than inventing a new primitive or continuing to chase uncertain internals. Built clean, deployed. **Not yet live-tested** -- this plus [0.0.87]'s scheduled-refresh gap fix should be tested together.

---

## [0.0.87] — 2026-08-26

### PA piece rendering: engine internals dead-ended, but found a real gap in this codebase's own logic instead

The IDA investigation into what `RefreshActor3D` (Disable+Enable) actually does chased two levels deep. First target (`sub_140563A80`, one of two branches inside the enable-worker) was fully decompiled and ruled out -- generic render/LOD flag bookkeeping, nothing armor- or actor-related. The OTHER branch (taken specifically for actors, formType 45) leads into two vtable calls (slots 177 and 110) that weren't resolved this pass -- a third level of static tracing with no confirmed connection to inventory/equipped-items either way. Given two full passes turned up no confirmed mechanism, stopped chasing the engine internals rather than sink a third speculative pass into it.

Went back to this codebase's own logic instead and found a concrete, unambiguous gap: the fresh-PA-enter path schedules a follow-up actor refresh 1.2 seconds after entering (whatever that refresh actually does under the hood). The mid-session piece-UPDATE path added in [0.0.85] never scheduled one at all -- so if that refresh is genuinely load-bearing for making pieces visible (this codebase has empirically relied on the same pattern for regular armor via `CoSyncOutfitSync` for a long time), a piece swapped after the initial enter would never get it, regardless of whether the underlying mechanism is fully understood yet. Added the identical scheduled refresh to the piece-update path.

This is offered honestly as a plausible fix for a known gap, not a confirmed root-cause fix -- the deeper question of what actually makes Enable() render newly-equipped items (if anything) remains open.

Built clean, deployed. **Not yet live-tested.**

---

## [0.0.86] — 2026-08-26

### Third live-test round confirmed [0.0.85]'s fixes; PA exit cleanup now gated on the eject actually taking, retried if not

Confirmed via log from a full single-session test: the repeated-chassis-spawn fix and the chassis DirectInject fix from [0.0.85] both held -- piece changes correctly updated the existing chassis with zero new spawns, and zero `DirectInject FAILED` lines. Weapon-vanish-on-its-own-verify also confirmed still fixed.

New finding from the same log: the native PA eject call itself sometimes silently refuses -- `ExtraPowerArmor` read `1 -> 1` (not `1 -> 0`) right after a `called=1` (non-faulting) eject attempt, following a rapid sequence of piece-swap re-broadcasts. The existing code proceeded to delete the chassis and reshow the real frame regardless of whether the eject actually worked -- meaning when it silently fails, we were tearing down and repositioning objects out from under an actor the engine still considers physically merged into the rig. That's a more direct explanation for "floating chassis" and "a second chassis appears at the exit spot" (the real-frame reshow firing on top of a still-attached original that never actually detached) than anything about the reshow/delete logic itself.

Fixed: extracted the chassis-delete/real-frame-reshow/3D-rebuild/weapon-requeue sequence into a shared `FinishPaExit` function, and gated it behind confirming the eject actually took (`ExtraPowerArmor` reads non-1). If it doesn't take on the first try, the exit is queued for a deferred retry (every 1s, up to 10 attempts) instead of proceeding with cleanup -- only actually tearing anything down once the engine confirms the actor is really out.

**Also currently investigating:** pieces still not appearing on the proxy despite clean equip logs (no failures anywhere in the chain: inject, force-apply, all report success). This points at the same "equipped in inventory but never renders" gap the applyNow=1 fix was supposed to close -- either that fix isn't sufficient on its own, or there's a further rendering step specific to Power Armor pieces that hasn't been found yet. A deep-dive IDA pass into the actual appearance-rebuild mechanism behind Disable()+Enable() (which this codebase has used empirically for years but never actually confirmed the real internals of) is in progress to settle this properly instead of guessing another patch.

Built clean, deployed. **Not yet live-tested.**

---

## [0.0.85] — 2026-08-26

### The real cause of PA exit floating/stretching: every mid-session piece change was spawning an entire new chassis and re-merging, leaking every earlier one

Third live-test round on Power Armor in one evening, and the log finally showed the actual mechanism: `TickLocalPowerArmorPieces` re-announces the wearer's current PA state as a fresh "enter" packet whenever their installed pieces change (by design, so peers pick up mid-session swaps) -- but the receiving side had no concept of "already in PA, this is just a piece update." Every single one of those re-broadcasts ran the FULL fresh-enter sequence: spawn a brand new chassis clone, inject pieces, call `SwitchToPowerArmorDirect` to merge the proxy into it again. One test session showed 4 distinct "spawned chassis=..." log lines for what was a single continuous PA session on the sender's end -- each one overwriting the only record of "which chassis is this proxy's" (`m_paSpawnedChassisRefFormID`), so every earlier clone became permanently orphaned in the world with nothing left to ever clean it up. Repeated real merges into different frames in rapid succession, most of them abandoned mid-attachment, is a far more coherent explanation for "chassis floating above the character" and "model stretching" on exit than anything about the exit path itself -- which had already been checked twice.

Fixed: the receiving side now checks whether the proxy is already in PA with a known chassis before doing anything. If so, the new pieces get applied directly to the EXISTING chassis (same `DirectInjectInventoryItem` + `EquipItemDirect` immediate-apply pair already proven this session) -- no new spawn, no re-merge. Only a genuine first-time enter for that proxy takes the full spawn-and-merge path now.

**Also fixed in the same pass, found from the same log:** `DirectInjectInventoryItem`'s widening to chassis refs (from [0.0.83]) was failing on every single call -- confirmed via its own new failure logging. Root cause: the function assumed the target ref already had a `BGSInventoryList` allocated with at least one existing item to borrow a vtable pointer from (true for actors, which is what it was built for) -- a freshly spawned chassis has `inventoryList == NULL` outright. Fixed properly rather than patched around: `BGSInventoryList` has no vtable (confirmed against its own already-declared layout in this codebase's header, unlike the Stack/ExtraDataList/BGSObjectInstanceExtra structs this same function already allocates, which all needed a borrowed vtable because they ARE polymorphic) -- so a fresh one is now placement-constructed in place when missing, and the separate "borrow a Stack vtable" step falls back to the local player's own inventory (guaranteed non-empty) when the target ref has no items of its own yet.

Built clean, deployed. **Not yet live-tested** -- this round specifically needs a full single-session enter, at least one piece swap while worn, and exit to confirm both fixes together.

---

## [0.0.84] — 2026-08-26

### Found the actual engine mechanism behind "PA pieces equipped but never render" -- applyNow=0 vs 1

Followed up [0.0.83]'s open thread with a dedicated IDA decompile of the real `SwitchToPowerArmor` engine internal, as requested, instead of continuing to patch by symptom.

First had to fix the map: the RVA an earlier session recorded for this function (`sub_140989A80`) turned out to be a transcription error -- mid-instruction inside an unrelated 397-byte function with zero real callers. The actual function (already correctly used at runtime via `CoSyncGameAPI.cpp`'s own `g_EngineSwitchToPowerArmor`, so nothing was actually broken at runtime -- only a comment's address was stale) is `0x989D90`, 3447 bytes, 8 direct callers. Decompiled it in full.

Ruled out the leading theory from [0.0.83]'s notes: the function has exactly 4 blocks gated on "is this the local player singleton," and all 4 are camera/HUD/UI work only (a first-person camera-target transform, two UI toggle calls) -- nothing resembling a 3D/appearance/biped rebuild that a proxy would be missing. The existing 1.2-second post-enter `RefreshActor3D` guess isn't compensating for anything this function actually skips for non-player actors.

Found the real mechanism instead: PA piece installation goes through the exact same general equip-visual pipeline as normal weapon/armor equips (`ActorEquipManager::EquipObject`, RVA `0xCE5A90` here vs. `0xCE5E20` for ordinary equips -- confirmed the same underlying function, not a separate PA-specific path) -- but the engine's own internal call installs each piece with `applyNow=0` (deferred), while `CoSyncEquipHook::EquipItemDirect` (the codebase's own already-proven helper, responsible for every weapon/armor visual that already works correctly) always passes `applyNow=1` (immediate). A live, AI-driven player gets a follow-up engine tick that applies a deferred equip automatically; an AI-disabled proxy never does -- fully explaining the earlier "equipped=1 in inventory, never renders" finding.

Fix: after `SwitchToPowerArmorDirect` completes on the proxy, loop over the same 6 PA piece forms and call `CoSyncEquipHook::EquipItemDirect` on each -- forcing the identical immediate-apply path already proven for weapons and armor, instead of relying on the generic Disable+Enable guess. Reuses an existing, already-correct primitive; no new engine call needed.

Built clean, deployed. **Not yet live-tested.**

---

## [0.0.83] — 2026-08-26

### Second live-test round: real root causes for "weapon vanishes after drawing" and "PA pieces never land"; PA exit floating still unconfirmed (log spanned a reconnect)

**Weapon appears for a second then vanishes -- found and fixed the real cause, not a race.** [0.0.82]'s shared throttle was live and working correctly in this test (confirmed in the log: weapon-verify and outfit-verify refreshes never fired within 5s of each other anymore) -- so the symptom persisting proved the earlier "two triggers racing" theory wasn't the whole story. The actual cause: `ProcessWeaponVerifyQueue` unconditionally fired a full `RefreshActor3D` (Disable+Enable) on every SUCCESSFUL weapon verify, and Enable() drops any individually-equipped weapon (already established elsewhere in this codebase) -- so confirming a weapon landed correctly immediately stripped it again, forcing the ~1s scheduled re-equip that produces exactly this flicker. Not a rare race: a guaranteed cycle on every single weapon equip. Unlike `CoSyncOutfitSync`'s own `RefreshActor3D` call (armor visuals legitimately need the biped-slot rebuild), a weapon's mesh attaches through `DrawWeaponMagicHands`/`EquipObject`'s own weapon-specific chain (IDA-decompiled earlier this session, no biped-slot dependency found) -- this rebuild was never fixing anything for weapons. Removed it entirely; verification failure still has its own recovery path (the existing periodic 10s retry).

**PA pieces never showing -- found the real failure point.** Live `[PASync DIAG]` logging (already present in the code from an earlier session) caught it directly: 6 `CallFunctionNoWait(chassis, "AddItem", ...)` calls were queued for the 6 PA pieces, but a follow-up inventory dump one second later showed only 1 (unrelated) item had actually landed in the chassis's inventory. This is the exact same silent-failure class this codebase already solved for actor refs (`CoSyncGameAPI::DirectInjectInventoryItem`, built specifically because Papyrus `CallFunctionNoWait("AddItem")` silently fails for all form-arg calls on these proxies) -- it had just never been applied to the chassis piece-installation path. Widened `DirectInjectInventoryItem` from `Actor*` to `TESObjectREFR*` (`inventoryList` is a TESObjectREFR-level field, not Actor-specific -- confirmed against the header before changing anything; every existing caller passes `Actor*`, which upcasts implicitly, so no other call site needed changes) and switched the chassis piece loop to use it. Also synchronous now, removing a queue-ordering race against the `SwitchToPowerArmorDirect` call a few lines later.

**Important nuance found in existing code comments, not yet re-verified:** a prior session (2026-08-01/02) already discovered that PA pieces can show `equipped=1` in the proxy's own inventory (i.e. past the failure point just fixed above) and *still* never visually render -- a second-stage problem underneath the one just fixed, which a `s_pendingActorRefresh`/"Post-enter actor 3D refresh" mechanism (`CoSyncPowerArmorSync.cpp`) was already built to attempt a generic fix for. Whether that generic fix is sufficient, now that pieces actually reach the chassis reliably, is unconfirmed -- needs a live retest, and if pieces still don't render, `SwitchToPowerArmor`'s real internals need proper IDA analysis instead of another guess (in progress).

**Also fixed:** the periodic "PA piece changed while worn" mid-session re-broadcast (`TickLocalPowerArmorPieces`) was dead code -- its chassis identifiers were function-local statics that were never actually assigned, so every re-broadcast attempt sent `chassisBaseFormID=0` and was silently dropped before reaching the network (confirmed in the log: the "re-broadcast" line fired with no matching "TX PA" line, every time). Moved the cache to file scope and had `NotifyPowerArmor` itself populate it on our own enter events.

**PA exit floating: could not confirm or rule out from this log.** The specific exit captured spanned a reconnect/session boundary -- no matching enter was recorded in this process's lifetime, so both `m_paSpawnedChassisRefFormID` and `m_paRealFrameRefFormID` were already 0 going into the exit, and neither cleanup path (the [0.0.80] chassis-disable fix or the [0.0.82] real-frame reshow) had anything to act on. Needs a clean single-session enter-then-exit test to actually evaluate.

Built clean, deployed. **None of this round's fixes are live-tested yet.**

---

## [0.0.82] — 2026-08-26

### Live-test round: real chassis visibility for Power Armor (full circle: pre-placed, workshop-built, console-spawned), a shared rebuild throttle, PA exit chassis fix, and a corrected diagnostic

First live test of [0.0.78]-[0.0.81] came back with real data. Working through it:

**AlchDiag diagnostic was broken, not the theory.** Every first-person sample came back `ok=0` regardless of camera state. Root cause: the diagnostic walked `GetActorRootNode(true)` (the raw first-person node tree) looking for `RArm_Hand`/`LArm_Hand` -- but per this codebase's own already-measured finding (`IsFirstPersonCaptureEnabled`'s doc comment), that tree is the "left-arm + Pip-Boy view rig" and contains no right arm and no hand at all. Worse, this session's active config (bone-rep mode 3, the default) doesn't even use that tree for first-person capture -- it reads the engine-ticked parked-graph pose snapshot instead. Rewrote the diagnostic to read `g_localCapture` directly (the actual buffer that gets transmitted, via whichever mechanism is really active) instead of a manually-walked tree that was never in the capture path to begin with. Needs a retest.

**Power Armor exit chassis floating/stretching:** the spawned chassis's cleanup was a fire-and-forget `CallFunctionNoWait(spawned, kDelete, ...)` -- the exact same "invisible failure" blind spot already documented elsewhere in this file for `SwitchFromPowerArmor`. Added a synchronous native `DisableWorldActor` call before the queued delete, so the chassis is guaranteed out of the render list immediately regardless of whether the delete itself lands quickly, slowly, or not at all.

**Weapon mesh not showing / armor "needs re-equip" -- found a real shared cause.** Live log showed `RefreshActor3D` (the Disable+Enable full-body rebuild) firing from two completely independent call sites -- weapon-verify and outfit-verify -- each keeping its own separate 5-second per-entity throttle with no knowledge of the other. That allowed two full rebuilds to race each other, and the weapon re-equip that has to follow every single one of them, on the same proxy within moments. Added `CoSyncGameAPI::TryRefreshActor3DThrottled` -- one shared per-entity cooldown both call sites now go through instead of their own separate maps.

**The Power Armor architecture request -- "full circle": a placed, workshop-built, or console-spawned PA frame should disappear for every observer the instant someone enters it, and reappear at wherever they exited, not just on the wearer's own screen.** The existing clone-and-attach mechanism for the proxy itself already works and is untouched. What was missing: nothing ever told other clients to hide/show the REAL frame the wearer interacted with. Its formID is almost never resolvable cross-machine (dynamic ref for anything workshop-built or console-spawned) -- so the `PA|` packet now also carries the real chassis ref's world position at the moment of the event, and every receiving client locates ITS OWN local copy of that frame by base-form-ID + position proximity (reviving a `FindLocalChassisInCell` helper that existed in this file but was never actually called, extending it with the position match). On enter: found frame gets `DisableWorldActor`'d and its formID remembered per-proxy (`CoSyncPlayer::m_paRealFrameRefFormID`). On exit: repositioned to the proxy's current (exit) location and `EnableWorldActor`'d again. Covers all three placement methods the same way, since position-proximity doesn't care how the object got there -- and a console-spawned or workshop-built frame is already independently synced to every client's world via the existing generic buildable-object scan (FURN=42 is already in its filter list), so there's always a local copy at a matching position to find.

Built clean, deployed. **None of this round's fixes are live-tested yet** -- next session should retest weapon draw, armor swap, PA enter/exit visibility (ideally with another player actually watching the frame from a distance), and the corrected AlchDiag.

---

## [0.0.81] — 2026-08-26

### Stimpak-use-in-first-person diagnostic added (static IDA analysis hit real diminishing returns; live probe instead)

New bug report: using a Stimpak visually shows on a remote proxy when the local player was in third person, but shows nothing when they were in first person. Spent four decompilation passes on Fallout 4's item-equip visual pipeline (`UseStimpakHandler::Handle` -> `ActorEquipManager::EquipObject` -> its per-slot visual worker and every function it calls, down through an `Actor` vtable slot) hunting for a first-person/third-person branch that would explain it, on the theory that this is the same class of bug `WriteParkedGraphLocomotion` ([0.0.77]) already fixed for movement -- the sender's third-person body is "parked" (not ticked by the engine) while in first person, so if the stimpak visual only ever attaches to the active graph, a first-person use would land on the first-person view-model skeleton, which bone-rep never captures from. Every function decompiled came back negative for referencing the player's active-graph index or holder pointer, and the trail kept forking into further unresolved indirect calls. Confirmed via a fresh independent pass: genuine diminishing returns for static analysis, not a wrong guess about where to look each time.

Added a temporary, read-only live diagnostic instead of continuing to guess: `CoSyncEquipHook`'s existing `EquipObject` detour now calls `CoSyncPlayerAnimation::StartAlchEquipDiag()` whenever the local player equips an `ALCH` item (Stimpaks and other consumables). This arms a 90-frame window during which `CaptureBones` (already running every frame) samples `RArm_Hand`/`LArm_Hand` on BOTH the third-person and first-person skeletons every ~10 frames and logs both, tagged with which one is currently being captured from (`[AlchDiag]` lines). Using a stimpak in first person and checking the log will show empirically whether the injection animation moved the third-person bones (meaning the bug is elsewhere), only the first-person bones (confirming the theory), or neither (a third possibility not yet considered). No engine call, no write -- reuses the existing `SampleBoneRotation` diagnostic primitive, extended with a `firstPerson` parameter (default false, existing call site unaffected).

Built clean, deployed. **This is a diagnostic, not a fix** -- remove once the question is answered and update this entry with what was found.

---

## [0.0.80] — 2026-08-26

### Found the real reason armor syncs looked like they needed an extra action: they were silently disarming the proxy's weapon, with nothing re-equipping it

Follow-up to [0.0.79]. Checked the `.psc` files at the user's suggestion to make sure weapon/armor visual sync wasn't secretly Papyrus-driven somewhere I'd missed — it isn't (`CoSyncPlayer.psc`'s own header confirms weapon/armor sync is entirely C++; the only Papyrus equip hook, `CoSyncQuest.psc`'s `OnItemEquipped`, is filtered down to potions only). While designing a fix for the armor same-slot-swap symptom, found `CoSyncOutfitSync::FlushPendingOutfitOps`'s verify pass already had exactly the mechanism needed — a `RefreshActor3D` (Disable+Enable) call once the last pending armor piece for an entity verifies, "so the engine rebuilds the biped with the new gear," reusing the same fix that solved invisible-skeleton spawns. But this is the ONE of three `RefreshActor3D` call sites in the codebase that didn't arm the weapon-survival follow-up — `CoSyncPlayer.h`'s own comments (`m_revealReEquipDelay`, `m_postRefreshWeaponDelay`) and `ProcessWeaponVerifyQueue`'s identical call both already document that `Enable()` re-attaches armor via the biped system but drops any individually-equipped weapon, with nothing to re-arm it unless the caller explicitly schedules a re-equip. This call site didn't, so every armor sync on a proxy holding a weapon silently disarmed them — a real, separate bug from [0.0.79]'s draw-state-edge issue, and one that could easily be misread as "the armor/weapon didn't visually update" when the actual symptom was the weapon vanishing mid-sync.

Fixed by arming `m_postRefreshWeaponDelay`/`m_postRefreshWeaponFormID` right after the `RefreshActor3D` call, mirroring `ProcessWeaponVerifyQueue`'s own pattern exactly — no new mechanism, reused what already existed twice elsewhere in this codebase.

Not fully closed out: the verify-triggered refresh is still throttled to once per 5 seconds per entity, which could still skip a rebuild on rapid consecutive swaps — flagged, not changed this pass, no live evidence yet that it's an actual contributor versus the confirmed weapon-drop bug above.

Built clean, deployed. **Not yet live-tested.**

---

## [0.0.79] — 2026-08-26

### Weapon swap while drawn fixed (proxy visual mesh never refreshed without a holster/redraw edge); melee + armor swap left open, root cause not confirmed

User-reported live symptom: on a remote proxy, drawing/holstering forces a weapon visual update, but switching weapons WHILE already drawn does not — the old mesh keeps showing until an explicit holster-then-redraw. Same pattern reported for armor/clothing (needs an explicit unequip/re-equip, not a direct slot swap). Melee weapons were reported as never showing at all, under any circumstance.

Decompiled `Actor::DrawWeaponMagicHands` (Actor vtable slot 201, current 1.11.240.0 RVA `0xC5D5A0` — the codebase already dispatches through the vtable slot rather than a hardcoded address, so no RVA change needed) to find the real mechanism instead of trusting the existing header comment's claim that the engine's own `ActorEquipManager::EquipObject` chain "fires the full visual attach chain" unconditionally. It doesn't: `DrawWeaponMagicHands` is a weapon-state-machine action queuer that reads the actor's current 4-bit weapon-drawn state and short-circuits to a no-op whenever the requested state already matches — it only actually re-attaches the weapon mesh on a real drawn<->holstered EDGE. `CoSyncPlayerManager.cpp` already had a "redraw after equip" step (`m_weaponDrawDelay = 8` after a successful `EquipItemDirect`), but it was calling `DrawWeaponEngine(actor, true)` while the proxy's weapon-drawn state bit was already 1 — a guaranteed no-op the entire time the proxy was already armed.

Fix: when a weapon-equip change arrives for a proxy that's already drawn (`m_prevWeaponState != 0`, real weapon differs from the pending one), force a synchronous holster (`DrawWeaponEngine(actor, false)`) immediately, so the existing post-equip redraw becomes a genuine 0->1 transition instead of a 1->1 no-op. No new engine primitive needed, reuses the already-correct `DrawWeaponEngine`.

Melee: no weapon-type (ranged/melee/unarmed) branch found anywhere in `DrawWeaponMagicHands` itself — it's entirely state-bit-generic. This makes it plausible melee was hitting the exact same already-drawn no-op bug just fixed above (e.g. if a proxy's first-ever weapon is typically a gun from the default spawn outfit, switching TO melee later is itself an already-drawn swap) rather than a genuinely separate melee-specific gap — but this is NOT confirmed, only plausible. Needs a retest specifically holstering/redrawing with melee equipped to see if the fix above already resolves it before chasing anything melee-specific. One real lead if it isn't the same bug: `CoSyncGameAPI::DirectInjectInventoryItem` synthesizes weapon geometry data (`BGSObjectInstanceExtra`) by cloning it from the local player's own matching weapon, falling back to an empty/default-appearance record if the local player doesn't personally carry that exact weapon — not obviously melee-specific (has a graceful fallback, and the vtable source is scanned generically across the whole inventory) but flagged as unverified, not ruled out.

Armor/clothing same-slot swap: investigated both the engine side (`ActorEquipManager::EquipObject`'s internal per-slot visual rebuild, `CoSyncOutfitSync`'s apply ordering) and found no state-edge gate analogous to the weapon case — the equip-then-unequip ordering in `FlushPendingOutfitOps` already does old-unequip-immediately / new-equip-50ms-later, which is the safe order. **Root cause NOT found this session** — genuinely unresolved, not a guess dressed up as a fix. Needs either a live-test log from a repro session or more specific reproduction detail before continuing.

Built clean, deployed. **Weapon-swap-while-drawn fix not yet live-tested.**

---

## [0.0.78] — 2026-08-26

### Workshop placement orientation fixed; Power Armor bone-rep migration fixed — both via direct engine decompilation (1.11.240.0), not guessing

Two long-standing bugs, both root-caused this session by decompiling the actual engine binary instead of trusting existing in-code assumptions, several of which turned out to be wrong.

**Workshop rotation.** Reported symptom: an object placed via the Workshop build menu shows up on the other player's screen at the right position but facing the wrong way. The existing code already had one documented fix attempt (`MoveRefrToPosition` called after spawn specifically to apply rotation) — decompiled it directly and found its rotation argument is **dead code for any non-player ref**: the function branches on "is this the local PlayerCharacter singleton," and only in that branch does it ever touch the rotation pointer, and even then only for a camera fade effect, never a transform write. Also checked whether `ObjectReference.SetAngle` (the Papyrus native this should have used instead) was a viable alternative — traced its real implementation (`GameScript::SetAngleFunctor`) down to its apply callback and found it **just calls the same dead `MoveRefrToPosition` internally**. Same code path, different name, not an alternative.

The actual fix: `CreateReferenceAtLocation` (the function that spawns the piece in the first place) takes a `dirX/dirY/dirZ` field in its creation struct that's been sitting there unused since this struct was first reverse-engineered — decompiled it and confirmed it unconditionally, synchronously sets `rot.x/y/z` (radians, direct mapping) and fires the same transform-changed notification already proven correct for position, for any ref type, during construction. `CoSyncGameAPI::SpawnItemAtPosition` now takes an optional `rot` parameter wired straight into that field; the Workshop apply path (`CoSyncWorldSync.cpp`) passes the piece's rotation in at spawn time instead of trying to rotate it afterward. Also applied the same fix to the Power Armor chassis frame spawn (`CoSyncPowerArmorSync.cpp`), which had the identical dead-rotation call pattern, and removed both now-redundant post-spawn `MoveRefrToPosition` calls entirely.

**Power Armor bone-replication.** Since [0.0.77]'s clean swap, player proxies are supposed to be pure bone-replicated with no exceptions — but `CoSyncPlayer::TickAnimVars` still checked `m_isInPowerArmor` *before* `createType == Player`, so a player proxy currently in Power Armor never reached the bone-rep apply at all and silently fell back to the old graph-var `LoadPowerArmorLocomotion` path (the same one the `[0.0.69]`-`[0.0.76]` saga never got working). Reordered so the player branch takes priority; PA now only matters for NPCs. Verified before making this change (not just assumed) that Power Armor doesn't need any special bone-rep handling: extracted and diffed the human and Power Armor skeleton `.nif` files and confirmed PA doesn't rename the core body bones bone-rep depends on (COM/Pelvis/spine/limbs) — it just parents `<Bone>_Armor` plate nodes onto the same humanoid bone names, so the plates ride along for free via normal scene-graph parenting once attached.

Also fixed a smaller related gap found while verifying the above: `CoSyncPlayerAnimation::CaptureBones`'s skeleton-root pointer cache (1-second TTL, meant to force-refresh on any "full 3D reload" event) force-invalidated on a camera switch but not on a Power Armor transition, which is the same class of event — up to a second of stale/empty bone capture right after entering or exiting PA. Now keyed on PA state too, via `CoSyncPowerArmorSync::IsActorKnownInPowerArmor`.

Built and deployed clean. **Not yet live-tested.**

---

## [0.0.77] — 2026-08-25

### Player animation clean swap: bone-replication becomes the default, old graph-var player path retired

Decision this session: bone replication (`bDeltaPositionLocomotion=3`, the "minimal preset") is now the primary way CoSync animates PLAYER proxies, replacing the graph-variable-write player path carried since the early NPC animation work. Renamed `CoSyncAnimTesting.cpp/.h` → `CoSyncPlayerAnimation.cpp/.h` (namespace and log tag `[AnimTesting]` → `[PlayerAnim]`, all 13 referencing files + vcxproj/filters updated) and moved the whole animation subsystem into a new `Animations/` folder as part of a broader source reorg (`Core/`, `Networking/`, `World/`, `Character/`, `Weapons/`, `Armor/`, `Clothing/`, `Items/`, `Hooks/`, `GUI/`, `Dialog/`, `CoSync/`) grouping the previously-flat file list by subsystem.

`TickAnimVars`'s player branch is now pure bone-rep, nothing gated: removed the `IsBoneReplicationEnabled` gate, the `LoadAnimationVariables` spawn-bridge fallback, and the `IsEnabled()` local-mirror branch. The old locomotion-driver blocks (Direction/Speed recompute, loco-tier/event dispatch) are no longer a `bonereplPlayer`-gated leftover — they're now a plain `if (createType != Player)` NPC-only route; NOT dead code, still load-bearing for NPC movement until the NPC migration happens.

**Locomotion capture fix (the big one):** in first person the capture source is the parked 3rd-person graph, which the engine never feeds a lateral Direction/gait, so replicated legs slid or played the wrong clip. Added `WriteParkedGraphLocomotion` (`CoSyncFirstPersonGraph.cpp`, called from `EngineTickParkedGraph` before the engine re-ticks graph 0): computes Direction (heading vs. facing from world-position delta), Speed, walk/run tier, and sprint from the local player's real movement, and writes them into the parked graph via the existing graph-scoped setters. Fixes strafe (confirmed working), sprint-instead-of-jog, and the backward lean. Tuning knobs isolated as `kStrafeSign`, `kRunSpeed`, `kSprintSpeed`. Pure bone-rep turns out to do strafe correctly in third person because the engine feeds the live 3P graph a real Direction value — this just gives the parked FP graph the same input so the captured bones carry it too.

Also fixed a teleport/load hang: both players teleporting at once could freeze the client on the load screen (game thread hangs ~4s spawning the host's proxy into a still-loading cell, `parentCell == NULL`, render thread keeps running). `SpawnRemotePlayerTask::Run` (`CoSyncSpawnTasks.cpp`) now defers and re-enqueues while `parentCell` is NULL instead of proceeding, draining once the cell streams in.

Added two read-only diagnostics for an open furniture-related pitch-lock bug: sender-side `[witness]` capture-context logging (camera state/path/real speed/graph vars) and `[cam-state]` change logging (`LogCameraStateDiagOnChange` in `Detour_MgrUpdate`).

CoSync.ini `[Animations]` cleared of the three now-retired testing keys (documented as `=0` to force off, `=2` for the old diagnostic build). Backup of all 9 pre-swap anim source files + ini kept at `_AnimSwapBackup_2026-08-26/`.

Build succeeded, deployed 01:30. **Not yet live-tested except strafe (user-confirmed working)** — next needs to confirm sprint-vs-jog, lean, and the teleport/load-hang fix under real play, then the post-swap migration pass (Power Armor, Weapons, Clothing/Armor, Workshop).

---

## [0.0.76] — 2026-07-31

### [0.0.75]'s acceptance log confirmed: `PowerArmorEnter` and `PAMoveStart` both come back `accepted=0` — but need a control before trusting it

Test result: `[TickAnimVars] entity=... PowerArmorEnter, graph accepted=0` and `PAMoveStart sent, graph accepted=0`, consistently, across multiple PA entries this session. Taken at face value this means the graph rejects animation events for this proxy from the very first one sent on PA entry — a more fundamental problem than event naming.

But this return value has never been checked before *at all*, including for the plain `moveStart`/`sprintStart` events already confirmed working visually in non-PA mode (their gait changes were seen and logged, just never their `SendEvent` return value). Without a same-session comparison, `accepted=0` could mean either "this proxy's graph genuinely rejects PA events" or "this return value has always been meaningless/always false, and success has never actually depended on it." Added the same `accepted=%d` logging to the plain `moveStart`/`sprintStart` dispatch so the next test gives a real control group.

Build succeeded, deployed. **Next test: enter PA and walk/sprint both before and after** — check whether the pre-PA `moveStart`/`sprintStart accepted=` differs from the in-PA `PowerArmorEnter`/`PAMoveStart accepted=`. If both are 0, the signal is worthless and a different verification method is needed. If pre-PA shows 1 and in-PA shows 0, that's real confirmation of a PA-specific rejection.

---

## [0.0.75] — 2026-07-31

### [0.0.74] didn't fix it either — added acceptance logging + decompiled SwitchToPowerArmor's real internals to find the actual delivery gap

User reported the PA-event fix from [0.0.74] still doesn't animate. IDA-decompiled the full real `SwitchToPowerArmor` implementation (`sub_140989A80`, ~700 lines) to check whether the engine's own PA-entry code uses a different event-delivery mechanism than this codebase's `CoSyncAnimUtils::SendEvent`. It mostly doesn't reveal a different mechanism (most of the function is equip-manager/inventory bookkeeping and player-singleton-only camera work that doesn't apply to a proxy) — but it exposed a real, separate bug in this codebase's own instrumentation: `PSC_SendAnimEvent` (`CoSyncPlayer.cpp`) discards `CoSyncAnimUtils::SendEvent`'s return value entirely, so **there has never been any log evidence of whether the graph actually accepted any animation event**, PA-specific or otherwise — including the original `PowerArmorEnter` dispatch that's existed since before this session.

Changed `PSC_SendAnimEvent` to return the acceptance bool, and added a log line at every PA-specific call site (`PowerArmorEnter`, `PAMoveStart`, `PASprintStart`, `PASprintStop`, `PAJumpFall`, `PAJumpEnd`) reporting `graph accepted=0/1`. This is the deciding diagnostic: if `PowerArmorEnter` itself comes back `accepted=0`, the graph isn't recognizing ANY animation event sent to this proxy at all (a much more fundamental problem than event naming — possibly the wrong holder, wrong graph instance, or a proxy-specific limitation), which would mean the last several sessions of PA-event work were never going to succeed regardless of which event names were used. If `PowerArmorEnter` shows `accepted=1` but the PA-prefixed events show `accepted=0`, the names/timing need more work. If everything shows `accepted=1` and it still doesn't animate, the problem is deeper in the state machine's actual transition conditions — not fixable via event names at all, and would need the compiled behavior graph asset itself inspected (a different, heavier kind of investigation than exe decompilation).

Build succeeded, deployed. **Next test: enter PA, walk/sprint/jump, then check the log for every `graph accepted=` line.**

---

## [0.0.74] — 2026-07-31

### Wired the confirmed PA-specific animation events

Two more test dumps came back (client: 593 entries clean; host: 390 entries with 3 recovered VEH crashes mid-walk, unrelated bug in the pre-existing `GetVariableSetDirect` path hit concurrently, not from this dump code). Combined, they confirm a consistent PA-event cluster: `PAMoveStart` [355], `PATurn` [358], `PASprintStart` [359], `PASprintStop` [360], `PAjumpFall` [492], `PAjumpEnd` [493] — sitting right alongside `CameraSprintStart`/`CameraSprintStop`/`CameraHitStop` in the same index range, confirming this really is a coherent "PA locomotion event" cluster in the shared graph, not noise.

Added these as named constants in `CoSyncAnimUtils::Events` (exact casing preserved from the dump — note `PAjumpFall`/`PAjumpEnd` keep the base word lowercase like `jumpStart`/`jumpLand`, unlike the PascalCase `PAMoveStart`/`PASprint*`). Wired them into `CoSyncPlayer::TickAnimVars` at the exact same trigger points as their generic counterparts, gated on `m_isInPowerArmor`: `PAMoveStart` alongside `MoveStart`, `PASprintStart`/`PASprintStop` alongside the generic sprint events, `PAJumpFall`/`PAJumpEnd` alongside `JumpStart`/`JumpLand` (paired by naming symmetry — the engine silently drops unrecognized event names per this codebase's own round-139 finding, so a slightly-off pairing guess costs nothing). No `PAMoveStop`/`PATurn` trigger point found yet — didn't guess at one.

Build succeeded, deployed. **Untested — this is the actual candidate fix for the gliding/attached symptom**, not a diagnostic this time. Next test should walk/sprint/jump while in PA and check whether the frame now visibly animates instead of just translating.

---

## [0.0.73] — 2026-07-31

### PA graph dump finally fired — found the real cause: PA-specific event names, not different variable indices

The dump from [0.0.71]/[0.0.72] finally fired this test (`PA GRAPH DUMP ... bucketCount=512 ... printed=400`). The data reframes the whole investigation: entries like `Speed` [54], `DirectionSmoothed` [49], `iSyncSprintState` [22] sit at the **exact same indices** as the human `TP::Variables` table — meaning `pManager->animationGraphs.Get(0)` is very likely not swapping to a genuinely separate graph object at all when entering PA. What's different is the presence of **PA-prefixed event/state names alongside the shared variables**: `PAMoveStart` [355], `PATurn` [358], `PASprintStop` [360].

That lines up with a standard Bethesda pattern: shared locomotion float variables across modes, but mode-specific **event triggers** to switch the state machine into each mode's own animation branch. The existing code already fires the generic events correctly (`MoveStart`/`MoveStop`/`SprintStart`/`SprintStop`, confirmed live in the logs) — but never the PA-specific ones. That would explain everything so far: the raw variable writes from [0.0.70] land correctly (same indices work in both modes), yet the state machine never gets pushed into its PA-specific locomotion branch, so it just holds a static pose while the position updates externally — the exact "attached instead of controlling" symptom.

The 400-entry print cap in [0.0.71]'s dump cut off before all 512 buckets were covered, so likely siblings (`PAMoveStop`? `PASprintStart`? `PATurnStop`?) may not have been seen yet. Raised the cap to 4000 (comfortably covers any graph this size) so the next dump shows the complete table in one pass.

Build succeeded, deployed. **Next step is just one more PA-enter test** (no movement needed, the dump only needs the graph to load) to get the complete name list, then wire `PSC_SendAnimEvent` to fire the PA-specific variants (alongside or instead of the generic ones) when `m_isInPowerArmor` is true.

---

## [0.0.72] — 2026-07-31

### The graph-variable-name dump never fired at all — traced to a stuck "not ready" gate that silently blocks everything after PA entry

Checked the log from the test meant to trigger [0.0.71]'s dump: it never appeared. Traced why — `[TickAnimVars]` for this proxy fires ~150x/sec continuously, but **every downstream log line stopped completely** right after PA entry (no `PowerArmorEnter` event, no `FULL SNAP`, no `moveStart`/`moveStop`, nothing) even though the pre-existing `PowerArmorEnter` dispatch and my dump are both unconditionally reached on every non-early-return call. Something upstream was returning early on every single tick.

Found it: `CoSyncAnimVarSystem::IsGraphReady(actorRef)` (gates the whole rest of `TickAnimVars`) returns false whenever the graph's variable-set size comes back as 0 or implausibly large. Entering PA swaps the actor's entire behavior graph — a much bigger transition than a normal fresh-spawn 3D load — and the existing recovery logic for "3D/graph not ready" was built for that smaller case: it retries `RefreshActor3D` **exactly once**, then gives up silently for the rest of the session if the graph still isn't ready. In this test, the graph apparently never finished settling within that one retry window (log tail showed real network lag — "8/9 reply timeouts... sending keepalive" — plausibly delaying the VM-queued `SwitchToPowerArmor` itself), so the proxy was stuck "not ready" for the entire ~30s test with zero further log output. This also means `LoadPowerArmorLocomotion` ([0.0.70]/[0.0.71]) can be silently starved the same way in a slow-settling session, which may be part of why sync has looked intermittent — not just wrong variable names.

Fixed: added a periodic (not one-shot) `RefreshActor3D` retry specifically while `m_isInPowerArmor` is true (new `CoSyncPlayer::m_lastPaGraphRetryTime`, retries every 3s instead of once-and-silent-forever), plus a throttled log line (`PA graph still not ready ...`) so this state is visible instead of invisible next time. Left the existing one-shot retry for the normal (non-PA) spawn case untouched — didn't want to risk the already-proven behavior there.

Build succeeded, deployed. **Untested — retest under a clean connection if possible**, since this session's specific stall may have been lag-driven. Check for the new `PA graph still not ready` line if it recurs, and whether the `PA GRAPH DUMP` from [0.0.71] finally appears.

---

## [0.0.71] — 2026-07-31

### PA locomotion: [0.0.70] resolved 23/23 variables but graph still doesn't animate — added a full graph-variable-name dump to find the real names

User confirmed a real walk-around test with [0.0.70] deployed: `[AnimVarSys] PA-PASS` showed `resolved=23/23` consistently, meaning every guessed variable name was found in the PA graph's hash table and written to the correct index — the hash-walk mechanism itself works. But the frame still didn't animate/glided regardless. Conclusion: the PA graph has these names in its table (or names close enough to collide, unlikely) but its actual state machine/blend tree isn't reading them for locomotion — the names borrowed from the human graph's convention aren't necessarily the ones the PA behavior project's animators actually wired up.

Rather than keep guessing names, added `CoSyncAnimVarSystem::DumpGraphVariableNames` — walks the same hash table but extracts and logs the REAL string content of every entry (via `StringCache::Entry::Get<char>()`, a plain pointer chase with no engine call, same safety profile as the resolve function) instead of checking against a guessed list. This gives a genuine, complete answer to "what does this graph actually call its variables" instead of another guess. Wired to fire once, 90 ticks (~1.5s) after a PA-enter transition — via new `CoSyncPlayer::m_paVarDumpDelay` — to give `SwitchToPowerArmor`'s async VM-queued graph swap time to actually land before dumping, so the log reflects the PA graph and not a stale human one.

Build succeeded, deployed. **Next test just needs to enter PA once** (pieces not required for this diagnostic) — check the log for `[AnimVarSys] PA GRAPH DUMP ... printed=N` and the `[N] 'name'` lines under it. That list is the real target for the next locomotion-variable pass.

Also noted from the same test session (unrelated): both host and client hit the long-standing `PlaceAtMe_Native` crash (RIP `Fallout4.exe+0x16590EF`/`+0x165910F`, the same "missing form component" class already found for the PA chassis) ~20-40s after the PA test, coming through `CoSyncWorldSync::Tick` — a different, not-yet-blocklisted base form hitting it via the workshop/world scan. Separate from PA work, flagged for a future session.

---

## [0.0.70] — 2026-07-31

### PA locomotion sync: [0.0.69] was inert (hard-disabled far-call setter) — replaced with a read-only hash-table walk, no engine call

Log check right after [0.0.69] shipped showed it was a complete no-op: `CoSyncAnimUtils::SetFloat/Int/Bool` all hard-return early via `EngineSettersDisabled()`, an unconditional `return true` a prior round (122) put in place after finding this exact far-call chain (`SetGraphVariableFloat` 0x818DE0 → worker 0x81B050 → per-graph 0x1326660) hangs and **never once completes** — not lock contention, a genuine indefinite hang. That worker has its own internal spin-wait on an engine-side lock field, separate from and in addition to this codebase's own already-safe manager-lock wrapper; forcing it open even scoped to PA proxies risked freezing the whole game process on a single occurrence, which is worse than the original gliding bug. Left `EngineSettersDisabled` untouched.

Replaced the whole approach: rather than calling into the engine's by-name setter at all, `ResolveGraphVariableIndex` (`Animations/CoSyncAnimVarSystem.cpp`) reads the graph's own name→index hash table **directly and read-only** — no lock acquisition, no far call, so no hang is possible. Layout IDA-confirmed from the same decompile of `sub_141326660`: `BShkbAnimationGraph+0x370` = hashmap pointer, `+0x84` = bucket count, `+0x90` = chain-end sentinel, `+0xA0` = 24-byte-stride entry array (key ptr / int32 index / next ptr). Deliberately brute-force scans every bucket's chain for a pointer-identity match against a freshly interned `BSFixedString` (interning itself is a different, already-proven-safe RVA, 0x167BEC0 — unrelated to the hanging chain) rather than replicating the engine's own hash function, since a wrong hash guess would silently miss entries whereas an exhaustive scan can only be exhaustive or find nothing. `LoadPowerArmorLocomotion` now resolves each locomotion variable's index this way and writes through the same raw `varSet.data[idx]` mechanism already proven safe for the human graph — same end result as [0.0.69] intended, without going anywhere near the code path known to hang.

Logs a `[AnimVarSys] PA-PASS` line every ~60 calls showing `resolved=N/M` — the real signal to check next test: if resolved stays near 0, the layout guess needs revisiting; if it's mostly-matching, the write is landing and the gliding should visibly improve.

Build succeeded, deployed. **Completely untested — genuinely uncertain RE, but fails safe**: worst case of a wrong offset assumption is 0 resolved (no-op, same as before) or an SEH-caught fault (also safe) — never a hang, since nothing here acquires a lock or calls into the engine.

---

## [0.0.69] — 2026-07-31

### PA "gliding, attached instead of inside" — real root cause found and fixed: locomotion sync was completely disabled while in PA

User pushed back that [0.0.68]'s appearance-refresh fix didn't change the gliding symptom, and reminded that PA has its own animation set and is entered like a suit. Traced `CoSyncPlayer::TickAnimVars` (`Character/CoSyncPlayer.cpp`) and found the real cause: a prior round had already discovered the PA behavior graph uses a completely different variable index table than the human graph, and — rather than solving that — just made `TickAnimVars` `return` immediately for any `m_isInPowerArmor` proxy, skipping `LoadAnimationVariables` (and every event-driven animation trigger below it: sneak, weapon draw/sheathe, reload, block, stagger, move/sprint start-stop) entirely. Position sync kept moving the proxy; the animation graph never received another input again after the one-time `PowerArmorEnter` event. That's exactly "attached to the proxy instead of controlling it."

IDA-decompiled the real engine chain behind `CoSyncAnimUtils::SetFloat/Int/Bool` (`SetGraphVariableFloat` 0x818DE0 → worker 0x81B050 → per-graph setter 0x1326660, canonical `Fallout4_disk.i64`) and confirmed it resolves a variable's index via a **per-graph name→index hash table** (at `BShkbAnimationGraph+0x370`) on every call — unlike the raw `varSet.data[TP::index]` writes `LoadAnimationVariables` uses for its main (human-graph) pass, which assume one fixed index table and would genuinely corrupt a different graph. The by-name path is safe against any graph by construction: an unmatched name just no-ops instead of corrupting an unrelated slot.

Added `CoSyncAnimVarSystem::LoadPowerArmorLocomotion` (`Animations/CoSyncAnimVarSystem.h`/`.cpp`) — writes core locomotion (Speed, Direction, TurnDelta, Pitch, sneak/sprint/walk-run sync ints) exclusively through the by-name setter, extracted from `LoadAnimationVariables`'s own already-written (but globally disabled, `kUseEngineNamedSetters=false`) named-setter block. Left that global flag untouched — round 122 found the by-name far-call worker could hang under high-frequency load across many actors; scoping the by-name calls to PA proxies only (rare, 0-2 per session) avoids reintroducing that risk for the common case. Removed the early `if (m_isInPowerArmor) return;` in `TickAnimVars` entirely (not just narrowed) — everything between it and the actual variable-write call site is event-based (`PSC_SendAnimEvent` for sneak/weapon/reload/block/stagger/sprint) or pure local math, both graph-agnostic, so PA proxies now also get those triggers for the first time instead of just locomotion.

Build succeeded, deployed. **Untested — and flagging a real, previously-encountered risk**: if a hang/freeze occurs specifically when a proxy is in Power Armor, suspect this new by-name call path first (round 122's history) and check whether it's isolated to PA before assuming it's something else.

---

## [0.0.68] — 2026-07-31

### PA chassis "gliding attached to proxy instead of proxy being inside it" — applied the known RefreshActor3D fix pattern to the chassis

User reported a long-standing symptom, separate from tonight's crash/piece work: the PA frame visually attaches to the proxy but looks glued on rather than the actor actually being merged into it, and glides around. Matched this directly against already-documented project history: objects spawned via `CreateReferenceAtLocation` (which [0.0.67] just switched the chassis to) skip the template/facegen/appearance-resolution pass a normal `PlaceAtMe` runs — this is the exact, already-diagnosed reason `RefreshActor3D` (native Disable+Enable cycle) exists for actor proxies (`CoSyncGameAPI.cpp` round 123 history). The chassis never got the equivalent treatment.

Added `CoSyncGameAPI::EnableWorldActor` (`CoSyncGameAPI.h`/`.cpp`) — symmetric to the existing `DisableWorldActor`, both operating on a plain `TESObjectREFR*` rather than requiring an `Actor*` like `RefreshActor3D`/`EnableProxyActor` do. `FlushPendingPowerArmorOps` now runs `DisableWorldActor(chassis)` + `EnableWorldActor(chassis)` immediately after the chassis is spawned and positioned, before pieces are added and before `SwitchToPowerArmor` fires. Kept native/synchronous rather than Papyrus-queued (`CallFunctionNoWait`) deliberately — this codebase already hit a real crash in 2026-07-21 doing the actor-proxy equivalent asynchronously (a queued Enable() not landing before `BGSSaveLoadGame::SaveGame()` could catch the ref mid-teardown), so the native path was proven safer for exactly this kind of Disable+Enable pair.

Build succeeded, deployed. **Untested** — needs a PA-enter test to confirm the chassis now properly merges with the proxy instead of gliding.

---

## [0.0.67] — 2026-07-31

### PA chassis spawn crash: real fix confirmed — switched off PlaceAtMe_Native entirely

Same-night follow-up to [0.0.66]. User pushed back on "contained but not fixed" and asked for an actual working solution. The [0.0.66] IDA trace showed the fault is baked into `PlaceAtMe_Native`'s own internal implementation (its component-lookup-then-lock sequence) for forms like `0x00034309` — not something fixable by changing call arguments. `SpawnChassisAtProxy` (`Armor/CoSyncPowerArmorSync.cpp`) was rewritten to use `CoSyncGameAPI::SpawnItemAtPosition` (the existing `CreateReferenceAtLocation` wrapper this codebase already relies on for NPC/item spawning) instead — a different, lower-level engine primitive that never goes through `PlaceAtMe_Native`'s code path at all, so the fault class doesn't apply. Removed the `IsKnownUnsafePlaceAtMeForm` skip guard from the PA call site (kept the shared helper itself, in case a future form needs it again) so `0x00034309` gets a real spawn attempt through the new path instead of being silently skipped.

**Confirmed working in the very next test** (`CoSyncUpdated_29368.log` client, 00:12): `Enter PA: spawned chassis=0xFF000E7F (base=0x00034309) ... -> SwitchToPowerArmor` — zero crash, zero "failed to spawn" warning, chassis spawned cleanly and PA entry proceeded. The exact form that was a guaranteed `PlaceAtMe_Native` fault three sessions running now spawns without incident.

Still untested: three PA-enter tests in a row (this one included) all had `pieces=[0,0,0,0,0,0]` — [0.0.65]'s piece-equip fix has never actually been exercised. Chassis spawning is now solid; next session should test with pieces actually installed on the frame.

Build succeeded, deployed.

---

## [0.0.66] — 2026-07-30

### PA chassis spawn crash: reused the existing PlaceAtMe crash blocklist, IDA-traced the actual fault mechanism, added a live diagnostic

Same-night follow-up to [0.0.65] after the user tested PA in-game. Log evidence (`CoSyncUpdated_4628.log` client / `_29180.log` host, 23:43): host entered PA in chassis base=0x00034309 (zero pieces installed, so [0.0.65]'s EquipItem fix wasn't actually exercised this test), and the client hit a WRITE AV — `lastTickStep=FlushPendingPowerArmorOps`, `RIP=Fallout4.exe+0x165910F` — the exact crash signature this project has hit many times before with `PlaceAtMe_Native`. SEH-caught, client recovered, but the chassis never spawned ("visual skipped").

The catch: `0x00034309` was **already** in `kWorkshopInternalHelperForms` (`CoSyncWorldSync.cpp`), blocklisted since 2026-07-10 with a comment identifying it as exactly this — "Power Armor FURN frame — placeable in workshop but crashes when independently spawned on a remote client." That blocklist was only ever checked by the workshop build-scan; `CoSyncPowerArmorSync::SpawnChassisAtProxy` never consulted it, so PA hit the same known-fatal form the workshop code was already avoiding. Fixed by exposing the check as `CoSyncWorldSync::IsKnownUnsafePlaceAtMeForm()` and calling it in `FlushPendingPowerArmorOps` before attempting the spawn — skips cleanly with a log line instead of fault-and-recover.

**IDA-traced the actual mechanism** (canonical `Fallout4_disk.i64`, decompiled the real `PlaceAtMe_Native` implementation and its full call chain down to the crash site): the native reads a QWORD "component list" pointer at `[baseForm + 0x100]` and, if non-null, unconditionally acquires a spinlock at `[thatPointer + 0x20]` — no sanity check gates the lock itself. Theory: this field is null/uninitialized on forms the engine never expects to be independently placed (consistent with every other entry already on the blocklist), turning the lock target into garbage and faulting there. This isn't fixable from CoSync's side — it's a property of the specific base record.

Added a read-only, SEH-isolated diagnostic (`LogPlaceAtMeComponentDiag`) that logs `[base+0x100]` for every PA-enter attempt (including blocklisted ones, so 0x00034309 itself still gets a live data point even though its spawn is now skipped) — confirms the theory with real data next session and can catch other unsafe forms before they crash for real instead of after.

Build succeeded, deployed. **Untested**: the blocklist-skip and diagnostic logging both need a session to confirm; also still need a PA test with actual pieces installed to verify [0.0.65]'s EquipItem fix, which neither test tonight exercised.

---

## [0.0.65] — 2026-07-30

### Power Armor pieces: found and fixed the real reason equip never worked, and gave Workshop building an instant sync path

User flagged PA piece equip and Workshop/Settlement building as the top two neglected areas. Audited both end to end against the current code (not memory) before changing anything.

**PA pieces — root cause found**: `FlushPendingPowerArmorOps` (`Armor/CoSyncPowerArmorSync.cpp`) added each piece to the spawned chassis via `ObjectReference.AddItem`, then immediately called `CallFunctionNoWait(chassis, "EquipItem", ...)` on the same chassis ref, believing PA frames wear pieces via `ObjectReference.EquipItem` "like vanilla CK frame scripts." Checked that claim directly against the game's own vendored Papyrus source (`Data\Scripts\Source\Base\ObjectReference.psc` vs `Actor.psc`): `EquipItem` is declared **only on `Actor`**, never on `ObjectReference`, and grepping every vanilla `*PowerArmor*.psc` script turned up zero cases of it being called on a frame ref either. `chassis` is explicitly a plain FURN `TESObjectREFR*`, never an `Actor` — so that `CallFunctionNoWait` had no matching native to dispatch to and was silently failing on every single call. Pieces reached the frame's inventory (`AddItem` is real) but the step meant to make them visually attach never ran. Fixed by removing the dead `EquipItem` call; `AddItem` is kept, relying on `SwitchToPowerArmor`'s own native internals (already IDA-confirmed in this project to call `ActorEquipManager::EquipObject` 0xCE5A90 internally) to pick the pieces up from the frame's inventory on entry, same as a real player-installed frame. Needs an in-game retest — if pieces still don't attach, the frame's real native piece-attach path needs IDA RE next (have a fresh 2026-07-27 full dump + disassembly export available for that).

**Workshop building — instant sync path added**: found three parallel, overlapping build-detection mechanisms in the code. WBF (poll-based cell-scan diff, `CoSyncWorldSync.cpp`) is the hardened, working one, but only fires on its scan interval plus a 2s anti-false-positive settle delay. WDS (MinHook on the engine's real `Workshop::ScrapReference`) correctly handles scrapping/removal end-to-end already. But WB/WD (`CoSyncWorkshopSync.cpp`, driven by the Papyrus `WorkshopObjectBuilt`/`WorkshopObjectDestroyed` custom events, which fire the instant a build/scrap completes) turned out to be a dead stub on the build side — `OnReceiveWorkshopBuild` only logged the packet and relayed it, never actually spawned anything on the receiving side. Fixed by replacing the build path entirely: `CoSync_NotifyWorkshopBuild` (`Papyrus_CoSync.cpp`) now reads the placed ref's real base form + position/rotation directly (the data was always available from the Papyrus event args, just never extracted) and calls new `CoSyncWorldSync::NotifyWorkshopBuildImmediate`, which sends the exact same `WBF|` wire packet the poll path would eventually send — so the receiving side's existing, already-hardened apply/dedupe pipeline (`s_pendingBuilds`, `s_knownDynamicBuildings`) handles it identically, just without the multi-second wait. The sender-side dedupe set is pre-seeded with the new ref's own formID so the later poll pass doesn't rediscover and re-send the same object. Removed the now-fully-dead `WB|` packet type, its serialize/deserialize helpers, and its `CoSyncNet.cpp` dispatch entry.

**Deliberately left alone**: `WD|` (destroy) was NOT wired up the same way, even though the user asked for "WB/WD" together — `WDS` already applies scrap removal correctly, and wiring up a second independent apply path for the same action risks a double-removal picking the wrong nearby duplicate object in a dense build (the receive-side match is proximity-based within a 128-unit radius, not a stable ID). `CoSyncWorkshopSync::Tick` (workshop-mode enter/exit tracking) also remains disabled — still "`hCurrentWorkshop` RVA suspect" since 2026-07-15, confirmed still skipping every tick in the most recent session log (2026-07-27 23:28) — but nothing currently depends on workshop-mode state, so left as a lower-priority follow-up for the fresh dump/IDA work.

Also noted, not yet investigated: a new crash in the 2026-07-27 23:29 client log, `lastTickStep=FlushPendingOutfitOps`, `DoxCoSync.dll+0x23976`, right after receiving an `OUTFIT` packet — unrelated to tonight's PA/Workshop work, flagged for a separate session.

Build succeeded, deployed. **Both fixes are code-complete but untested in-game** — next session should test (1) host enters PA with pieces installed, confirm they show on the client's proxy, and (2) host builds something in a settlement, confirm it appears on the client near-instantly instead of after a multi-second delay.

---

## [0.0.64] — 2026-07-25

### NPC proxy A-pose root-caused (again): IdleForceDefaultState fires before the behavior graph is ready

After [0.0.63]'s iState clamp (which confirmed iState=7 wasn't the real cause — iState is now 0 across all FULL SNAPs), full log analysis revealed the actual root cause:

**`IdleForceDefaultState` fires at ~0.9s via Papyrus but the behavior graph isn't ready until ~3.5s.** `InitNpcProxy` queues the event via `CallFunctionNoWait` at 120-tick defer (~0.9s post-spawn). But `TickAnimVars` doesn't see `IsGraphReady() == true` until ~3.5s. The Papyrus event fires into an unready graph and is silently dropped. By the time the graph IS ready, no animation-starting event has ever been delivered, so the graph stays in bind pose (A-pose) forever even though variables are being written correctly.

Confirmed by log: `FULL SNAP diag#=1` (first successful IsGraphReady pass) fires at 3.5s; `InitNpcProxy` log fires at 0.9s — 2.5 seconds before graph ready.

**Fix** (`Character/CoSyncPlayer.cpp` + `CoSyncPlayer.h`): Added `m_idleEventSent` flag. On the **first** successful `IsGraphReady()` pass in `TickAnimVars`, for NPC entities, fire `IdleForceDefaultState` natively via `CoSyncAnimUtils::SendEvent` (vtable slot 1, SEH-wrapped, already proven safe). The graph is guaranteed live at this point so the event is guaranteed to land. Flag is reset to `false` if a 3D-load retry fires (`m_3dLoadRetried`) so the event re-sends after the new Enable cycle settles.

**Why Papyrus was insufficient**: `CallFunctionNoWait` is async on the Papyrus VM thread and fires ~0.9s post-spawn (120 ticks). The behavior graph requires the Enable cycle → async 3D stream → graph attach → graph init sequence to complete, which takes ~3.5s for quest/vendor NPCs. No amount of tuning the Papyrus defer time reliably accounts for this variable settling window across all NPC types.

---

## [0.0.63] — 2026-07-25

### NPC proxy A-pose root-caused and fixed: iState override was forcing proxy graph into state it couldn't enter without AI events

Log analysis from today's session revealed all 33 NPC proxies showed `NpcSyncDiag: ok` (data pipeline healthy) but were visually A-posed. Root cause: `TickAnimVars` blindly mirrors the host NPC's `iState` variable (index 0 in the integer snapshot, `TP::kiState` in the behavior graph) to the proxy's behavior graph every tick. On the host, the NPC's AI drives it into override states via behavior-graph event transitions (e.g. "BeginCombat" → graph transitions → `iState` becomes 7 for alert-idle). Raw-writing `iState=7` to the proxy's graph skips those preceding events — the graph's state machine can't enter that override state cleanly without them, and falls back to bind pose (A-pose). Confirmed by the FULL SNAP log: all idle NPCs had `iState=7` (alert-idle on the host) with `speed=0`, persisting until the NPC briefly moved (at which point iState cleared to 0 naturally).

**Fix** (`Character\CoSyncPlayer.cpp`, `TickAnimVars`): for world NPC clones (`createType == CoSyncEntityType::NPC`) when `m_smoothSpeed < 1.0f`, clamp `smoothed.integers[0]` (iState) to 0 before calling `LoadAnimationVariables`. This keeps the proxy in the behavior graph's standard idle state — which IdleForceDefaultState already initialized correctly — instead of pushing it into an event-dependent override state it can't properly reach. When speed > 0, the raw host iState is passed through unchanged so locomotion states still apply correctly.

**Not changed**: all other integer snapshot fields (`kiSyncSprintState`, `kiState_NPCMelee`, `kiState_NPCGun`, etc.) are passed through unchanged; the iState clamp only fires during idle.

---

## [0.0.62] — 2026-07-21

### Host crash root-caused to a stale duplicate crash-form blacklist on the client side (build 185 session)
Checked the newest session (build 185, `CoSyncUpdated_1080.log` client / `CoSyncUpdated_3356.log` host) per the open-ended "see what else we're missing" request. NPC-sync diagnostics themselves looked excellent: host `[OwnershipDiag] summary: worldActors=38 owned=38 UNOWNED=0` and `[NpcSyncDiag] summary: total=0`, client `[NpcSyncDiag] summary: total=33 ... STUCK=0` sustained across many samples — confirming the host-only-authority revert (`[0.0.58]`) and the teleport fix (`[0.0.61]`) are both working as intended. But the host crashed once, and the log immediately before the crash was unambiguous:

```
[21:12:05.137][H] [UPDATED] [WorldSync] RX WBF: base=0x001439D8 pos=(-122,-476,-635) from entity=2784291864
[21:12:05.137][H] [ERROR] [CoSync-VEH] CRASH code=0xC0000005  region=MEM_IMAGE(FO4/DLL)
[21:12:05.137][H] [ERROR] [CoSync-VEH]   lastTickStep=CoSyncWorldSync::Tick
[21:12:05.137][H] [ERROR] [CoSync-VEH]   RIP=0x00007FF71EDC910F  (Fallout4.exe+0x165910F)
```

`0x001439D8` is the exact base form blacklisted on the **host** side back in `[0.0.44]` to avoid the known `PlaceAtMe_Native` write-to-read-only-page crash class (`Fallout4.exe+0x165910F`) — but this crash was the **host receiving that same form over the network from the client**, meaning the client was still broadcasting it. Root cause: `kWorkshopInternalHelperForms` was never actually a single shared list. `HostTickObjects_Impl` had its own `static const` set (where `0x001439D8` was added in `[0.0.44]`), and `ClientTickBuildScan_Impl` had a **second, separately hand-copied** `kInternalHelpers` set that was never updated with that entry — so the client kept building and broadcasting `WorkshopBuildFullPacket`s for a form the host had explicitly blacklisted, and the host had no way to protect itself from a peer sending it.

**Fix** (`CoSyncWorldSync.cpp`): moved `kWorkshopInternalHelperForms` to file/anonymous-namespace scope (previously local to `HostTickObjects_Impl`), keeping the full history of prior entries plus `0x001439D8`. Removed `HostTickObjects_Impl`'s local duplicate entirely (now just references the shared set), and removed `ClientTickBuildScan_Impl`'s separate `kInternalHelpers` set, switching its check to the same shared list. There is now exactly one blacklist, so a form added for one role can never silently miss the other.

Build 186 deployed. **Next test: confirm the host no longer crashes on `RX WBF: base=0x001439D8` (or any other previously host-only-blacklisted form)** now that the client won't broadcast it either. The `BGSSaveLoadGame::SaveGame()` null-`parentCell` crash chased since `[0.0.45]` is unrelated to this fix and remains separately open.

---

## [0.0.61] — 2026-07-21

### NPCs teleporting to the door on ext/int transition: root-caused and fixed (4th instance of the same missing-worldspace-check pattern). Crash status re-confirmed still open (build 185)
User reported exterior NPC clones getting teleported to the door P1/P2 entered when transitioning exterior→interior, plus a host crash from the prior session (build 183, before this round's diagnostic-only 184).

**Teleport bug, root-caused**: `SpawnWatchdog` (`CoSyncPlayerManager.cpp`) re-enqueues a spawn for any tracked entity that's been "not spawned" for 3+ seconds (the recovery path for when the engine frees a proxy across a cell/grid transition). Its range-gate compares `authoritativePos` — set **once**, at `CREATE` time, never refreshed afterward — against the local player's *current* raw position, with no worldspace check at all. This is the fourth instance of the exact same missing-worldspace-context bug found and fixed this session (NPC departure check, proxy cell-visibility, and now this): interior and exterior coordinates are numerically incomparable, so crossing ext↔int can make a stale, long-since-irrelevant exterior position spuriously "pass" the range check by coincidence. When it does, the watchdog re-enqueues a spawn — and `CreateReferenceAtLocation` always spawns at the *local player's own current position* (to guarantee landing in a loaded cell) — which is exactly the door the player just walked through.

**Fix**: added `authoritativeWorldspaceID` (`CoSyncPlayer.h`), captured alongside `authoritativePos` at `CREATE` time (`ProcessEntityCreate`, `CoSyncPlayerManager.cpp`) using the local player's worldspace at that moment (`0` = interior, matching the existing `lastRemoteWorldspaceID` convention). `SpawnWatchdog` now checks this against the local player's *current* worldspace before trusting the distance number at all — a mismatch postpones the respawn (same "bump the attempt time, retry later" mechanism the range-gate already used), regardless of what the raw coordinates say.

**Crash status, re-confirmed not resolved**: the host crash in the reported session is the same `BGSSaveLoadGame::SaveGame()` null-`parentCell` signature chased since `[0.0.45]` (`lastTickStep=CoSyncWorldSync::Tick` then `RTGT:end`, the exact two-crash sequence seen before). Checked whether the `Saves_P2` folder-redirect mitigation (`[0.0.48]`-`[0.0.50]`) has ever actually engaged: across **every** `cosync_proxy_*.log` on disk, `"Saves_P2 folder ready"` fires (instance detection and folder creation both work), but **`CreateFileW redirected`/`FindFirstFileW redirected` has never fired even once**. The actual save write still isn't being intercepted by that mechanism, so this crash was never actually fixed — only diagnosed and partially mitigated-in-theory. Not re-investigated further this round (focus was the teleport bug per the user's message) — still a known, open, unresolved crash.

Build 185 deployed. **Next test: confirm NPCs no longer teleport to the door on an ext/int transition** (should now just correctly stay absent/postponed until back in a matching context, consistent with `[0.0.55]`'s departure fix). **The SaveGame crash remains a separate, still-open problem** — worth a dedicated session rather than folding into NPC-sync work.

---

## [0.0.60] — 2026-07-21

### First genuinely good result: client's clones all reached OK with zero stuck. Fixed a false positive in the new diagnostic (build 184)
Checked the first session's logs with `[0.0.59]`'s new `[NpcSyncDiag]` in place. **Client: 31 tracked NPCs went spawning → initializing → all 31 `OK`, zero `STUCK`**, within about 10 seconds — the first clean, fully-successful result across this entire investigation. The throttled spawning (`[0.0.57]`) and 3D-load retry (`[0.0.56]`) appear to be doing exactly what they were built for.

Host showed `total=5 STUCK=5` the entire session, which looked alarming but turned out to be a bug in the diagnostic itself, not a real problem: those 5 entities' `spawnAge` read ~17556 seconds (≈4.9 hours) — because they're host's own **real, natively-discovered actors** (confirmed by the log: bound via the discovery path, `[NpcWorldSync] Registered actor ...`, not a clone spawn), so `m_spawnTime`/`m_lastAnimApplyTime` were never set for them in the first place (they were bound, not spawned) — and per this project's own established `[0.0.21]` design, entities the local machine is directly authoritative for **deliberately skip `TickAnimVars` entirely**, since native AI drives them, not CoSync. The diagnostic didn't know to exclude locally-owned entities and mislabeled correctly-working real actors as stuck.

**Fix**: `LogNpcSyncHealthDiag` now skips any entity where `isLocallyOwned` is true before evaluating it at all — only clones (entities this machine doesn't own, which are the only ones that actually depend on the spawn→graph-ready→animate pipeline) get reported. Purely a diagnostic-accuracy fix, no behavior change.

Build 184 deployed. **Next test: host's `[NpcSyncDiag] summary` should now correctly show `total=0`** (host-only authority means it never clones anything, so nothing left to report) **while client's should keep showing `STUCK=0`** as the real, meaningful success signal.

---

## [0.0.59] — 2026-07-21

### Added a single per-entity NPC-sync health diagnostic to replace manual log archaeology (build 183)
Every round of this A-pose investigation so far needed manually cross-referencing `RefreshActor3D` log lines against `AnimVarSystem` apply lines by hand (grep + `comm`) to find which specific proxies were stuck and where in the pipeline. Added `LogNpcSyncHealthDiag()` (`CoSyncPlayerManager.cpp`, called from `Tick()` every 5s alongside the existing `[OwnershipDiag]`) to put that same cross-reference directly into the log as one line per tracked world-actor entity, covering the full spawn → graph-ready → actively-animating pipeline end to end.

New `CoSyncPlayer` field (`CoSyncPlayer.h`): `m_lastAnimApplyTime`, set in `TickAnimVars` (`CoSyncPlayer.cpp`) the moment an entity gets past the `IsGraphReady` gate — i.e. the moment it actually starts (or continues) receiving real animation state, not just spawning.

Each `[NpcSyncDiag]` line reports: `entityID`, a computed verdict (`NOT_SPAWNED` / `INITIALIZING` / `OK` / `STUCK`), time since spawn, whether `IsGraphReady` is true *right now*, whether it has *ever* applied animation data, seconds since its last successful apply, whether the `[0.0.56]` 3D-load retry already fired for it, and which role is reporting. A trailing summary line (`total=X notSpawned=Y initializing=Z ok=W STUCK=V`) gives the at-a-glance health of the whole tracked population without reading every line.

Also doubles as direct confirmation for `[0.0.58]`'s host-only-authority revert: since host no longer clones anything, its `[NpcSyncDiag]` summary should show `total=0` for world-actor entities going forward — a concrete, checkable assertion instead of just reasoning about the code.

Build 183 deployed. **Next test: watch the `[NpcSyncDiag]` summary line on both roles.** Host should show `total=0`. Client's `STUCK` count is the number that actually matters now — any entity still showing `STUCK` after the `[0.0.56]`/`[0.0.57]` retry+throttle fixes is the concrete, named place to keep digging, instead of another round of manual cross-referencing.

---

## [0.0.58] — 2026-07-21

### Reverted to host-only world-actor authority, per user's explicit design decision (build 182)
Clarified the current architecture with the user before touching anything: ownership is proximity-based, not host-only — whichever peer is physically near an NPC claims it (confirmed by `[OwnershipDiag]`: host owned 5/38, client owned 33/37 in the last session, split by who was actually nearby). That means **both** sides can end up cloning NPCs they don't own, which is why P1 was also showing some A-posed NPCs — host's own clones of client-discovered NPCs, hitting the same stuck-3D issue as client's clones of host's NPCs. This is deliberate: earlier in this project, host-only discovery caused a real reported bug ("enemy attacked p2 was not in sync, p1 still far away and not aggro") — proximity-based ownership was built specifically to fix that gap.

Presented the actual tradeoff and user chose to go back to host-only authority anyway — simpler model, at the cost of reintroducing the "NPCs only near P2 don't sync to P1 at all" gap.

**Change**: `Networking/CoSyncNet.cpp` — `TickScanWorldActors` (the discovery/ownership scan) is now gated `if (s_isHost && s_connected)` instead of just `if (s_connected)`. `TickScanWorldActors_Impl`'s own internal role branch already contains the client-side `NPCSEEN` report path entirely (`if (!isHost) { ...send NPCSEEN...; continue; }` vs. direct claim/allocate for host) — gating the call site means the client never runs this function at all anymore, not even to report sightings. Everything downstream falls out naturally with no further changes needed: `HostSendNpcUpdates_Impl`'s existing `isLocallyOwned` gate will now always be false for world actors on the client side (since nothing ever sets it true there anymore), so the client correctly never attempts to drive/send for world NPCs either.

Net effect matches exactly what the user described wanting: host only ever deals with its own real, natively-AI-driven discovered actors (no cloning, no `RefreshActor3D`/retry/throttle machinery ever needed for them) — only the client spawns clones of host-broadcast NPCs, so the animation-fix work from `[0.0.51]`/`[0.0.55]`/`[0.0.56]`/`[0.0.57]` now only ever matters on the client side in practice, without needing to be artificially restricted there in code (the host simply has nothing to apply it to anymore).

`TickSuppressLocalDynamicSpawns` (client-only, unaffected) and the keep-radius departure/cleanup logic (still inside `TickScanWorldActors_Impl`, now host-only by extension of this gating) both continue working as-is — departure decisions and `EntityDestroy` broadcasts are host's sole responsibility now, exactly matching host-authoritative design.

Build 182 deployed. **Next test: confirm NPCs near only P2 no longer sync to P1 at all (the known, accepted tradeoff) — and confirm P1 no longer shows any A-posed NPCs of its own**, since it should no longer be cloning anything.

---

## [0.0.57] — 2026-07-21

### The concrete version of "preload on connect": spaced out spawn creation so the engine's 3D-streaming queue isn't hit with a burst (build 181)
User's instinct ("preload the 3D graph on connect") pointed at the right underlying problem — too much simultaneous streaming load — but a literal preload doesn't quite apply here: each NPC's 3D is a fresh, instance-specific attachment created only when `CreateReferenceAtLocation` runs for that specific actor, so there's nothing to warm up before that instance exists (the underlying skeleton/behavior-graph files the engine caches on first use are shared and likely already fast by the 2nd+ identical clone, matching the screenshot's multiple-clones-of-one-base-NPC).

Checked how spawning actually happens before changing anything: `PumpDeferredSpawns()` (`CoSyncPlayerManager.cpp`) already only pops and processes **one** pending spawn per game tick, not the whole queue at once — so it wasn't literally simultaneous to begin with. But at ~60 ticks/sec, 30-40 NPCs still land within under a second, which can outpace the engine's own 3D-streaming/attach queue even though our enqueue side is already "one at a time."

**Fix**: added a minimum 150ms interval between processed spawns (~6-7/sec instead of ~60/sec) — real breathing room for the engine's streaming to keep up with each new actor before the next one lands. This is the practical, buildable version of "reduce simultaneous streaming pressure" the preload idea was reaching for. Pairs with `[0.0.56]`'s retry fix: fewer proxies should get stuck in the first place now, and any that still do get one automatic `RefreshActor3D` kick after 3 seconds instead of staying frozen for the rest of the session.

Build 181 deployed. **Next test: same populated-area scenario as the last two rounds.** Spawning will visibly take a bit longer to finish populating a dense area (by design — 40 NPCs now take ~6s instead of under 1s to all appear), but each one should have a better chance of animating correctly the first time, and the `[TickAnimVars] ... retrying RefreshActor3D` line from `[0.0.56]` should show up less often if this is working.

---

## [0.0.56] — 2026-07-21

### Checked the radius hypothesis against real log data — not the bottleneck; found the actual permanent-A-pose gate and added a retry (build 180)
User suspected the discovery/keep radius was too small or too slow to keep up, causing A-posed NPCs for both P1 and P2. Checked the latest `[OwnershipDiag]`/`[SendDiag]` logs first, since guessing at radius tuning without evidence would just repeat earlier rounds' mistakes:

- Entity counts are now bounded (37-38 tracked world actors, not the 100+ seen before `[0.0.55]`'s departure fix) — that fix is holding.
- Ownership has flipped based on proximity as designed: client now owns 33/37 (it's the one near most NPCs this session), host owns 5/38 — both correctly send only for what they own, both showing healthy `notDriving=0`/`sent=high` numbers for their own owned entities. **Ownership and sending are not the bottleneck this round.**
- Cross-referencing the host's own "refreshed" (spawned) actor pointers against ones that ever got a real animation apply: still a gap, but the *reason* is different from before.

Traced the actual permanent gate: `CoSyncAnimUtils::IsGraphReady` (`Animations/CoSyncAnimationUtils.cpp`) checks `Is3DLoaded()` (`GetObjectRootNode() != nullptr`) **first, with no retry**. If a proxy's 3D genuinely never finishes streaming in — much more likely with 30-40+ simultaneous spawns in a dense area than with the 5-9 NPCs every earlier test used — `TickAnimVars` (`CoSyncPlayer.cpp:503`) returns immediately on every single call, forever, and that proxy is never animated again for the rest of the session. This is a real match for "too slow/not in render": not a radius-size problem, but a spawn-burst/streaming problem — and extending the radius would make it *worse* (more simultaneous spawns competing for the same streaming budget), not better.

**Fix**: added two fields to track this per-entity (`m_3dNotLoadedSince`, `m_3dLoadRetried`, `CoSyncPlayer.h`). If `IsGraphReady` is still false `kAnimGraphSettleDelay*6` (3s) after first observed not-ready, `TickAnimVars` now fires one `RefreshActor3D` retry — the same proven "redo the sequence" fix already confirmed working for the ext/int proxy-hide case in `[0.0.55]` — instead of giving up on that proxy for the rest of the session. Bounded to one retry per proxy lifetime so a genuinely-broken actor doesn't get hammered forever.

Build 180 deployed. **Next test: same populated-area scenario. Watch for `[TickAnimVars] entity=%u 3D/graph still not ready %.1fs after spawn -- retrying RefreshActor3D` lines** — their presence confirms the stuck-3D theory directly, and whether the affected NPCs actually animate afterward confirms whether the retry is sufficient or whether the underlying spawn-burst/streaming pressure needs a different mitigation (e.g. staggering how many new proxies spawn per tick).

---

## [0.0.55] — 2026-07-21

### Two real bugs found and fixed: NPC entity count ballooning across cell transitions (never unloading), and the remote proxy/HP-AP overlay never properly coming back after ext↔int (build 179)
User reported two related, concrete bugs from live play: NPCs should unload when a cell area is left (currently causing duplicate spawns building up over a session), and the remote player's proxy plus the HP/AP overlay disappear going exterior→interior and never recover — needing "the same sequence on load" to restore properly. Checked the `[0.0.54]` `[SendDiag]` logs first and found the smoking gun for scale: `worldActors=104` on host, `62` on client — both far higher than any reasonable "NPCs actually near either player right now" count, confirming stale entities were genuinely accumulating rather than being cleaned up.

**Bug 1 — NPC departure check (`TickScanWorldActors_Impl`, `CoSyncNpcWorldSync.cpp`)**: the keep-radius cleanup compared raw Euclidean distance between a tracked actor's cached position and the local player's current position, with **no worldspace/cell-identity check at all**. Interior cells use small, arbitrary local coordinates completely unrelated to the exterior worldspace grid — crossing ext↔int (or between two different interiors) makes the two positions numerically incomparable. Depending on pure coincidence, the squared distance could land under the keep threshold (actor never recognized as departed → never cleaned up → silently re-discovered as a "new" entity next time that area is revisited → duplicate spawn) or wildly over it. Fixed: now requires the actor's worldspace to match the player's current worldspace (or, when both are interior — both report a null worldspace — the exact same `parentCell`) before trusting the distance number at all. Any actor whose location context doesn't match the player's current one is now treated as departed immediately, regardless of what the raw coordinates say.

**Bug 2 — proxy re-show after a hidden cell (`TickProxyCellVisibility_Impl`, `CoSyncWorldSync.cpp`)**: this already correctly detected an ext↔int worldspace mismatch and hid the remote proxy via `Disable()` — but used async `CallFunctionNoWait` Papyrus calls (same class of issue fixed in `[0.0.51]`), and critically, re-showing the proxy only ever called a bare `Enable()`. This project's own established history already root-caused why that's not enough: a bare `Enable()` without an immediately-preceding `Disable()` does not rerun the engine's full appearance/anim-graph rebuild — that's the exact reason `RefreshActor3D` exists as a *cycle*, not a single call. Fixed: hiding now uses the native synchronous `DisableWorldActor`; re-showing now runs a full `RefreshActor3D` cycle instead of a bare `Enable()`, so the proxy comes back in the same properly-initialized state a first-time spawn produces.

Not separately investigated this round: whether the HP/AP overlay bar has its own dependency beyond the proxy's visibility state — it isn't wired directly to `m_proxyHiddenForCell` in the code, so it's plausible its disappearance was purely a downstream symptom of the same broken proxy state and will resolve itself now that Bug 2 is fixed; worth confirming specifically in the next test rather than assuming.

Also flagged, not fixed: two different interior cells still both report worldspace `0`, so an actor departing into one interior while the player is in a *different* interior could still be misjudged as "same location" — a real remaining gap, but narrower than the ext↔int case this round targeted, and would need a new wire field (remote cell formID, not just worldspace formID) to close fully.

Build 179 deployed. **Next test: the exact scenario reported — play in a populated area long enough to accumulate NPCs, change cell areas a few times and confirm the entity count in `[OwnershipDiag]`/`[SendDiag]` stays bounded instead of climbing across the session; separately, transition exterior→interior→exterior and confirm the remote proxy AND the HP/AP overlay both come back properly, not just visible-but-still-broken.**

---

## [0.0.54] — 2026-07-21

### Ownership diagnostic confirmed ownership is NOT the bug — narrowed to the send loop itself with per-reason skip counters (build 178)
User sent a screenshot from the live session: multiple identical NPC clones standing frozen in A-pose in Diamond City — visual confirmation this is real and ongoing, and honestly described not knowing what the actual fix should be, which is fair; this is exactly why the diagnostic-first approach matters here.

Checked the `[0.0.53]` `[OwnershipDiag]` output from this exact session: **host owns all 45 world-actor entities (`owned=45 UNOWNED=0`), client owns 0 (correctly — only one side should drive each)**. This rules out the ownership-gap theory entirely — the host isn't failing to claim these entities, it already owns every one of them, so it should be sending updates for all 45. Also double-checked `RemoteControlled` (a second suspect gate in the send loop): confirmed set at both host discovery call sites in `CoSyncNpcWorldSync.cpp`, so that's not blocking it either.

Since ownership is confirmed fine but the client still only ever receives animation data for 9 of 44/45, the gap has to be in `HostSendNpcUpdates_Impl`'s send loop itself, past the ownership check. Added per-reason skip counters (`notSpawned`, `noState`, `notWorldType`, `notDriving`, `notRemoteControlled`, `transformFail`, `sent`) logged as a `[SendDiag]` summary every 5 seconds, covering every remaining `continue` in the loop after the ownership gate. Whichever counter is large next test is the actual blocker — most likely candidate given the evidence so far is `transformFail` (`CoSyncGameAPI::GetActorWorldTransform` failing for NPCs whose `parentCell`/3D isn't in a state the transform read expects), but this will show the real answer instead of another guess.

Build 178 deployed. **Next test: check both the client's `[OwnershipDiag]` (should stay owned=0 UNOWNED=45, confirming this isn't the cause) and the host's new `[SendDiag]` summary lines side by side.** Whichever skip reason accounts for the ~35-36 missing entities each cycle is the next concrete thing to fix — no more guessing needed once this lands.

---

## [0.0.53] — 2026-07-21

### Pivoted focus to the real, current NPC A-pose problem at scale — found the actual gap, added ownership diagnostics before touching the fragile ownership subsystem (build 177)
User redirected priority: the save-collision investigation is parked (crash appears fixed per `[0.0.51]`, `Saves_P2` folder-path work can wait) — the live, ongoing pain point is being surrounded by NPCs in a populated area (Diamond City) where most are stuck in A-pose. This has been a recurring complaint for a while; this round found something new and much more specific than prior rounds' fixes (which were all real, but addressed different mechanisms — createType mis-stamping, missing RefreshActor3D, wrong graph-var indices, etc.).

**Checked the actual current session's logs rather than re-explaining prior history**: 44 distinct world-actor NPCs got tracked this session. Every single one completed setup cleanly — `RefreshActor3D` (native, synchronous per `[0.0.51]`) fired and finished for all 44, `graphDriven=1` logged 3602 times with zero `=0` occurrences, no faults, no re-spawn churn. But cross-referencing actor pointers between "completed RefreshActor3D" and "ever received an `AnimVarSystem` apply" found only **9 of the 44** ever got a single animation-variable update afterward. The other 35 spawn and initialize perfectly, then sit frozen in whatever pose `CreateReferenceAtLocation` left them in for the rest of the session — never touched again.

Traced to `HostSendNpcUpdates_Impl`'s per-entity gate:
```cpp
const bool drivesEntity = CoSyncNpcWorldSync::IsWorldActorEntityID(npc.entityID)
    ? st.isLocallyOwned
    : (CoSyncNet::IsHost() && st.hostAuthoritative);
if (!drivesEntity) continue;
```
If neither peer ever marks a world-actor entity `isLocallyOwned`, nobody sends its animation data, ever — silently, no error, no log line, it just never happens. This didn't show up in any prior test because those all had 5-9 NPCs clustered near the host, where ownership resolves correctly by proximity alone. At real scale (a whole city, spread out, some discovered by the client's own local scan rather than the host's), the gap becomes the majority case instead of an edge case.

**Did not touch the ownership logic itself this round.** Project history shows this exact subsystem has been redesigned and reverted three separate times (`[0.0.16]` symmetric peer-discovery, `[0.0.17]` host-arbitrated-claim, `[0.0.19]` staged diagnostic pipe) — each attempt caused new regressions before being confirmed working, each time because there wasn't enough concrete evidence of which specific step failed. Given that pattern, added a targeted diagnostic instead of a fourth redesign: `LogEntityOwnershipDiag()` (`Character/CoSyncPlayerManager.cpp`, called from `Tick()` every 5s on both roles) logs every tracked world-actor entity's `isLocallyOwned`/`hostAuthoritative` state, how it was bound (direct-bind via a real placed formID vs. clone-fallback), and its `ownerEntityID`, plus a summary line (`owned=X UNOWNED=Y`) per role. Purely read-only, no behavior change.

Build 177 deployed. **Next test: play in a densely populated area again, then check both host's and client's `[OwnershipDiag] summary` lines.** A large `UNOWNED` count on both roles for the same entity IDs is the confirmation needed before designing the actual fix (whichever peer discovers/clones an entity should be the one that ends up marking it owned) — this diagnostic is what the last three attempts didn't have before shipping a redesign.

---

## [0.0.52] — 2026-07-21

### Good news: no crash this session (first real evidence [0.0.51]'s fix might be working). Saves_P2 still empty — added broader matching + unconditional diagnostic logging to finally pin down the real path format (build 176).
User confirmed the `[0.0.51]` `RefreshActor3D` fix survived a full session with no `Fallout4.exe+0xBF712D` crash — the first clean result after four consecutive crashes chasing that bug. Not confirmed yet as fully fixed (one clean session isn't proof), but a genuinely good sign.

Separately, `Saves_P2` is still empty. Checked the freshest `cosync_proxy_<pid>.log`: instance detection fired correctly (`err=183`, "my instance"), `Saves_P2 folder ready` logged, and `Saves-redirect all-module IAT patches: 22` confirms the hooks are installed across every loaded module — but zero `CreateFileW redirected`/`FindFirstFileW redirected` lines ever appeared, meaning `RedirectSavesPath` never even matched, not that it matched and something else went wrong. Two live possibilities, since the exact literal path string used has never actually been observed: (a) the engine builds the path with forward slashes or some other separator the old exact `"\Saves\"` backslash-only substring check misses, or (b) the call comes from a module that wasn't loaded yet when this DLL's one-time module snapshot ran.

**Changes (diagnostic-first, no more guessing)**:
- `RedirectSavesPath` broadened: now matches a `Saves` path *component* bounded by a separator on both sides, accepting either `\` or `/` in any combination (`\Saves\`, `/Saves/`, `\Saves/`, etc.) instead of the old exact-backslash-only substring check.
- Added `HookCreateFileA` (ANSI) alongside the existing `CreateFileW` hook, in case the actual call is ANSI — patched across all modules the same way.
- Added unconditional diagnostic logging (`LooksSaveRelated`) that logs **every** `CreateFileW`/`FindFirstFileW`/`CreateFileA` call whose path contains "saves" anywhere or ends in `.fos`/`.f4se`/`.fos.tmp` — independent of whether `RedirectSavesPath` itself would redirect it. If the broadened match still doesn't fire on the next save, this logs the exact literal string the engine actually passes, which is the one piece of ground truth that's been missing through three attempts at this.

Build 176 deployed. **Next test: trigger a save on the second-launched instance and check `cosync_proxy_<pid>.log` for any `(save-related, diag)` line at all.** If one appears now, either it redirects successfully (problem solved) or its exact path string will finally show why the match still fails. If literally nothing appears even with this much broader net, the call is happening through a mechanism these three hooks can't see at all (e.g. a Steam Cloud API call, if Goldberg's `steam_api64.dll` intercepts save I/O before it ever reaches a raw file API) — a different investigation, not just a stricter path check.

---

## [0.0.51] — 2026-07-21

### The SaveGame crash: actual root cause found and fixed, not just diagnosed (build 175)
User reported another crash after `[0.0.50]`. This one gave a perfect, unambiguous match instead of another correlation: the crash register `R12=0x0000015F58BFF890` (the reference being processed at the fault site, per the IDA disassembly from `[0.0.45]`) is the **exact same pointer** as `entity=1029663708 actor=0000015F58BFF890 formID=0xFF00219F parentCell=0000000000000000` — logged by the `[SaveDiag]` scan microseconds earlier as a null-`parentCell` hit. `RSI` even held the formID (`0xFF00219F`) directly. This is no longer "a reference with a null parentCell, probably"; it's the confirmed, specific one.

Entity `1029663708`'s ID pattern and dynamic (`0xFF`-prefixed) formID identify it as a **player proxy**, not a world NPC. Traced every place that touches player-proxy `parentCell` state and found `CoSyncGameAPI::RefreshActor3D` (`CoSync/CoSyncGameAPI.cpp:795`): it queues `ObjectReference.Disable(false)` then `Enable(false)` via `CallFunctionNoWait` — **asynchronous** Papyrus calls. `Disable()` genuinely detaches the actor from its cell when it actually executes on some VM tick; the queued `Enable()` doesn't necessarily run the same tick, since Papyrus VM queue processing isn't instant. That's a real, measurable window where the actor's `parentCell` is null — and `RefreshActor3D` fires constantly for player proxies (every weapon-verify and outfit-change "biped rebuild" cycle, per its callers in `CoSyncPlayerManager.cpp`/`CoSyncOutfitSync.cpp`), so the window opens often. If the engine's own autosave/quicksave timer fires inside it, `BGSSaveLoadGame::SaveGame()` walks straight into this exact actor mid-cycle and crashes on the unguarded null read — precisely what happened here.

**Fix**: `RefreshActor3D` now calls `DisableWorldActor`/`EnableProxyActor` — the native, synchronous engine calls already used elsewhere in this same file for exactly this reason (they exist because Papyrus's own Disable/Enable silently refuse refs with an enable-state parent) — back-to-back, with no VM queue between them. Same visual effect (forces the engine to tear down and rebuild the 3D/anim graph), but the disable-to-enable gap collapses to zero: nothing can observe the actor mid-cycle anymore, since both calls complete synchronously within the same game-thread call. No wire/protocol change, no new hook, single function body changed.

Build 175 deployed. **Next test: keep playing normally (weapon swaps, outfit changes — the things that fire RefreshActor3D constantly) through an autosave/quicksave and confirm no more `Fallout4.exe+0xBF712D` crash.** If this was the whole story, saves should now complete cleanly even with the same amount of proxy churn as before. The Saves_P2 folder-redirect work (`[0.0.48]`–`[0.0.50]`) stays in regardless — reduces the *chance* of any similar collision-driven state issue and gets each Goldberg instance a clean, independent save history, but this fix addresses the crash's actual mechanism directly.

---

## [0.0.50] — 2026-07-21

### Instance detection confirmed working, but the redirect hooks still never fired — root cause: IAT patching only the EXE misses CRT-routed file I/O. Also: first-ever direct evidence of the null-parentCell SaveGame bug via [SaveDiag] (build 174).
User relaunched after `[0.0.49]` — real progress this time: `cosync_proxy_26728.log` shows `DetectMyInstance: dedicated mutex CreateMutexW err=183` → `this is "my" instance` → `Saves_P2 folder ready`, and the folder genuinely appeared on disk. But the user still saw no save land inside it.

**Confirmed why**: a real save fired during this session (`CoSyncUpdated_26728.log`: `[SaveDiag] PreSaveGame fired` at 18:44:46.932) — and the session survived it (clean `GNS Background network thread stopped`, no crash this time). But `cosync_proxy_26728.log` shows **zero** `CreateFileW redirected`/`FindFirstFileW redirected` lines despite the IAT patch reporting success (`EXE IAT patches: 12`). The patch only rewrites the **main EXE's own** import table. The EXE's own import list includes several `api-ms-win-crt-*.dll` api-sets (stdio, filesystem) — meaning the engine's actual save I/O plausibly goes through **C runtime file functions** (`ucrtbase.dll`'s own `fopen`/`_wrename`-family implementations), which call the real `CreateFileW`/`MoveFileW` through **`ucrtbase.dll`'s own IAT**, a completely separate table my patch never touched. Exactly the same class of problem this file's own pre-existing `PatchExitProcessAllModules()` was written to solve for `ExitProcess` — just not yet applied to the newer save-redirect hooks.

**Fix**: generalized that pattern into `PatchIATAllModules(dll, fn, hook)` — enumerates every currently-loaded module via `CreateToolhelp32Snapshot`/`Module32FirstW/NextW` and patches each one's own IAT, not just the EXE's. `HookCreateFileW`/`HookFindFirstFileW` now installed this way instead of via a single `PatchIAT(exe, ...)` call.

**Also, independent of the redirect bug**: this is the first session to actually capture the `[SaveDiag]` `parentCell` scan added in `[0.0.46]` catching real hits during a live `PreSaveGame`. Multiple disabled-original NPC refs (`0x000199D5`, `0x00002F09`, `0x00002F1F`, `0x001A041B`) and one **live tracked proxy actor** (`entity=65539`, `formID=0xFF000AC3`) all showed `parentCell=NULL` at the exact moment the engine started walking changed forms — the first direct, name-the-reference evidence for the `BGSSaveLoadGame::SaveGame()` bug chased since `[0.0.45]`, not just the theoretical mechanism. Notably the save did NOT crash this time despite multiple null hits — confirming a null `parentCell` is necessary but not alone sufficient; the engine's own global-fallback lookup (`sub_140516FC0`) must ALSO fail in the same instant for the unguarded read to actually fault. That a *dynamic* proxy actor (`0xFF000AC3`) is now also confirmed as a hit, not just disabled originals, is new information for whoever eventually chases the engine-side mechanism further.

Build 174 deployed. **Next test: relaunch both instances, trigger a save on the second-launched one, and confirm `cosync_proxy_<pid>.log` now shows `CreateFileW redirected`/`FindFirstFileW redirected` lines and an actual save file lands in `Saves_P2`.**

---

## [0.0.49] — 2026-07-21

### Saves_P2 redirect from [0.0.48] never actually fired — the instance-detection signal was based on something that never happens. Fixed with a dedicated mutex (build 173).
User relaunched both instances after `[0.0.48]` and never saw a `Saves_P2` folder appear. Checked the fresh `cosync_proxy_<pid>.log` files rather than guessing: neither process ever logged a `CreateMutexA`/`CreateMutexW` line — and a sweep across **every** `cosync_proxy_*.log` from the entire day (dozens of sessions) confirms that line has **never once appeared**. Fallout4.exe's single-instance protection turns out to be `FindWindowA`/`FindWindowW`-only — it never calls a named mutex at all. `s_isMyInstance` was designed to flip true when the *game's own* `CreateMutexA/W` call collided with `ERROR_ALREADY_EXISTS` — since that call never happens, the flag never flipped, and the entire `[0.0.48]` Saves_P2 redirect was silently inert from the moment it was built (not broken by a bug in the redirect logic itself, which was never actually reached).

Also caught, while investigating: the newest proxy log (`cosync_proxy_26116.log`) showed a VEH exception at the exact same `Fallout4.exe+0xBF712D` address from `[0.0.45]`/`[0.0.46]`, firing right after the `FindWindowA` single-instance check — i.e. very early at boot, before the process ever reached a `CreateMutexA` call (there wasn't one to reach). Confirmed this isn't something the new `CreateFileW`/`FindFirstFileW` hooks caused: `s_isMyInstance` was still `false` at that point (nothing had set it yet), so both hooks were pure passthroughs with zero effect — ruled out by the code path itself, not just by inspection. Whether this is the exact same bug now also triggering during a boot-time save/load, or an unrelated coincidence, is unconfirmed and not chased further this round.

**Fix**: replaced the piggybacked detection with a dedicated, self-contained one. `DetectMyInstance()` (called at the top of `DllMain`, before the IAT patches) has the proxy create its own private named mutex (`CoSyncMultiInstance_FirstInstance`) directly — a plain, unhooked call from the proxy's own code, unaffected by the IAT patches applied to the EXE's import table. Whichever process's `DllMain` runs second reliably observes `ERROR_ALREADY_EXISTS` on that call and sets `s_isMyInstance` deterministically, independent of anything the game itself does. The existing (never-firing, but harmless) mutex-collision check inside `HookCreateMutexA/W` is left in place as an inert fallback.

Build 173 deployed. **Next test: relaunch both instances, then check each `cosync_proxy_<pid>.log` for `DetectMyInstance: dedicated mutex CreateMutexW err=...` — exactly one of the two should show `ERROR_ALREADY_EXISTS` and the "Saves folder redirected" line, and a `Saves_P2` folder should actually appear under `Documents\My Games\Fallout4\` this time.**

---

## [0.0.48] — 2026-07-21

### Separate Saves folder for the second Goldberg instance — the version of [0.0.47] that actually stays menu-visible (build 172)
`[0.0.47]`'s `.co` extension idea got reverted because FO4's Load menu hardcodes scanning for `.fos`/`.f4se` — a save renamed to a different extension is invisible to it. This is the same underlying fix (stop the two Goldberg instances from writing the identical `Autosave1_00000000M_DiamondCity_....fos` filename into one shared folder), done the way that keeps saves fully normal and menu-visible: redirect the **folder**, not the file extension.

Added to `CoSyncMultiInstance/dllmain.cpp` (same IAT-patch technique already used for `CreateMutexA/W`/`FindWindowA/W`):
- `HookCreateFileW` and `HookFindFirstFileW` both check every path for a `\Saves\` segment; for "my" instance (same existing mutex-collision-bypass signal as before — whichever process launched second), that segment gets rewritten to `\Saves_P2\` before the real call runs.
- Because *both* the file-open path (`CreateFileW`, used for every save/load/rename) *and* the enumeration path (`FindFirstFileW`, used to build the Load menu's list) are redirected consistently, the game sees a completely ordinary, fully populated Saves folder — it just happens to live in a separate directory the other instance never touches. Files inside it are untouched, real `.fos`/`.f4se` saves.
- Only paths containing `\Saves\` are touched — `Fallout4Custom.ini`/`Fallout4Prefs.ini`, mods, and everything else the two instances currently share stays shared exactly as before.
- `EnsureSavesP2FolderExists()` creates the `Saves_P2` directory (via `SHGetFolderPathW(CSIDL_PERSONAL)` + `\My Games\Fallout4\Saves_P2`, not a hardcoded path) the first time "my" instance is identified, since `CreateFileW` won't create a missing parent directory on its own.

Build 172 deployed (`nvToolsExt64_1.dll`). **Not yet tested — next session should confirm**: (1) `cosync_proxy_<pid>.log` for the second-launched instance shows `Saves_P2 folder ready` plus `CreateFileW redirected`/`FindFirstFileW redirected` lines during actual save activity, (2) a `Saves_P2` folder appears under `Documents\My Games\Fallout4\` containing normal-looking saves, (3) that instance's in-game Load menu still shows its own saves normally, and (4) whether this alone stops the `BGSSaveLoadGame::SaveGame()` crash from `[0.0.45]`/`[0.0.46]` — if it does, the crash really was the file collision; if it still happens, the `[SaveDiag]` parentCell scan is still in place to keep chasing the engine-side mechanism.

---

## [0.0.47] — 2026-07-21

### Likely real root cause of the SaveGame crash found: both Goldberg instances share one save folder and collide on identical autosave filenames. Mitigated via a distinct .co extension for the second instance (build 171, CoSyncMultiInstance).
User raised the right question after `[0.0.46]`: both host and client in every test session are two Goldberg-emulated instances of the same game running on the same PC, not two separate machines — so before chasing the engine's null-`parentCell` bug any further, checked whether they're actually stepping on each other's save file.

**Confirmed with hard evidence, not speculation:**
- All three `BGSSaveLoadGame::SaveGame()` crashes found across this session (16:58:11, 17:21:34, 17:37:39) each have a matching orphaned `Autosave1_00000000M_DiamondCity_..._3_2.fos.tmp` in `Documents\My Games\Fallout4\Saves\` at the exact crash timestamp — the engine writes to `.tmp` and only renames to `.fos` on success, so an orphaned `.tmp` is a save that started and never finished. Three sessions, three crashes, three matching `.tmp` files.
- **All three are `Autosave1`** — not a spread across different slot numbers, which is what independent, uncorrelated saves would produce. That's consistent with both instances deterministically racing for the same "next autosave slot."
- Checked `CoSyncMultiInstance/dllmain.cpp` (the proxy DLL — built as `nvToolsExt64_1.dll`, injected into both instances via DLL-name spoofing — that makes running 2 copies of the same install possible at all): it bypasses the single-instance mutex/window checks and `SteamAPI_RestartAppIfNecessary`, but does **zero** save-path or documents-folder separation. Both instances use the literal same `Saves` folder, with the same generic Goldberg-faked profile ID (`00000000M`) baked into every filename — there was never any isolation between them.

**Fix**: rather than the more invasive route (per-instance Windows user accounts, or hooking known-folder resolution to redirect an entire documents path), added a minimal, targeted mitigation directly in the existing proxy DLL: `HookMoveFileExW`/`HookMoveFileW` (same IAT-patch technique this file already uses for `CreateMutexA/W`, `FindWindowA/W`) intercept the save's final tmp→real rename and rewrite the destination's `.fos` extension to `.co` for whichever instance is "mine" — reusing the existing mutex-collision-bypass signal (`s_isMyInstance`, set exactly where the code already detects `ERROR_ALREADY_EXISTS` and knows it's the second-launched instance) rather than adding new detection logic. Since the two instances now write genuinely different filenames (`...fos` vs `...co`), they physically cannot collide or overwrite each other regardless of slot-number timing, without needing real folder-level separation.

**Known limitation, explicitly accepted**: FO4's own in-game Load menu hardcodes scanning for `.fos`/`.f4se` — a save renamed to `.co` will not appear there. Rename it back to `.fos` manually in Explorer if it ever needs to be loaded through the UI. This only affects "my" (the second-launched) instance's saves; the other instance's saves are untouched and behave exactly as before.

Build 171 deployed (`nvToolsExt64_1.dll` redeployed to the FO4 root). **Not yet tested — next session should confirm**: (1) the second-launched instance's log (`cosync_proxy_<pid>.log`) shows `MoveFileExW dest rewritten for .co` lines during autosaves, (2) new `.co` files actually appear in the Saves folder instead of colliding `.fos` files, (3) whether this alone stops the `BGSSaveLoadGame::SaveGame()` crash — if it does, the crash was the file collision, not (or not only) CoSync's own actor-disable/enable lifecycle; if it still crashes, the `[SaveDiag]` parentCell scan from `[0.0.46]` is still in place to keep chasing the engine-side mechanism.

**REVERTED same day, immediately after landing**: user asked why `.co` saves can't show up in FO4's own in-game Load menu (answer: the menu builds its list via a hardcoded `.fos`/`.f4se` file-enumeration scan plus a header-read for each match, so a `.co` file is invisible to it — making it appear would need hooking the enumeration and file-open calls too, a meaningfully bigger and riskier surface than the one rename hook this fix added). Once told that tradeoff, user decided it wasn't worth it and asked to drop the idea entirely rather than accept saves that don't appear in the UI. `CoSyncMultiInstance/dllmain.cpp` reverted in full — `HookMoveFileExW`/`HookMoveFileW`, `RewriteToCoExtension`, `s_isMyInstance`, and their IAT-patch registrations all removed; file is back to its pre-`[0.0.47]` state (mutex/window/ExitProcess bypasses only). Rebuilt clean, redeployed. **The underlying save-collision finding from this entry still stands and is still unaddressed** — worth a different fix later (e.g. a same-extension, distinct-basename rename that stays `.fos`-visible in the menu while still being a physically different filename, avoiding both the collision and the menu-visibility tradeoff) if this keeps causing crashes.

---

## [0.0.46] — 2026-07-21

### SaveGame crash empirically confirmed live; parentCell-null scan added to pinpoint the exact offending reference (build 170)
The `[SaveDiag]` logging added in `[0.0.45]` caught it on the very next test: `PreSaveGame fired` at 17:37:39.249, crash 19ms later at 17:37:39.268, same signature as before (`Fallout4.exe+0xBF712D`, `fault=0x14`). Confirms the IDA-derived mechanism empirically, not just theoretically — only the host crashed, client ran clean and disconnected normally once it noticed. Right before the save, a burst of ~20 `[Pickup TX] Item removed` lines fired within 2ms (a "take all" on a big container) — picking up a real placed item is itself completely vanilla-safe (the engine does this constantly with zero issue), so not the direct cause, but a signal of heavy world-object churn in that exact window.

Also cross-checked the crash mechanism against this codebase's own vendored engine header: `ThirdParty/Pap/GameReferences.h:210` has `STATIC_ASSERT(offsetof(TESObjectREFR, parentCell) == 0xB8)` — exactly the field the crashing engine code reads and finds null. Fully closes the loop: the crash requires a reference in the save's changed-forms list with `parentCell == nullptr`, which isn't a normal vanilla state for anything actually placed in the world.

No way to identify *which* reference from the log alone (no WinDbg/cdb installed for the `.dmp`), so added a safe, read-only diagnostic instead of guessing further: `CoSyncNpcWorldSync::DiagnoseDisabledOriginalsForSave()`, called from the existing `kMessage_PreSaveGame` handler, walks both plausible categories of CoSync-created reference that could end up "changed" with a stale `parentCell`:
1. Disabled-original NPC refs (`s_disabledOriginalRefs`) — resolved via `LookupFormByID`, logged with `parentCell` state.
2. Live tracked proxy/clone actors (`g_CoSyncPlayerManager.GetAllPlayers()`) — same check on `actorRef->parentCell`.

Any hit logs `<-- NULL, prime suspect for the SaveGame crash` with the formID/entityID, so the next crash can be matched against a specific reference instead of guessing. Purely additive, SEH-wrapped, mutates nothing.

Build 170 deployed. **Next test: force a save (or let one happen naturally) during active NPC disable/enable/spawn/despawn churn, and check the `[SaveDiag]` lines emitted right before any crash for which formID/entity shows a null `parentCell`.** That's the one to trace back to its specific lifecycle operation for the real fix.

---

## [0.0.45] — 2026-07-21

### The recurring "bIsUnderwater" crash, fully root-caused via IDA: it's a vanilla engine null-deref inside BGSSaveLoadGame::SaveGame() itself, not our code. Save-event diagnostic logging added (build 169).
This crash first appeared as a "second crash, 38s after the WBF one" in the previous test session (assumed at the time to be downstream corruption from that crash) and then reproduced completely independently — different session, different machine role (host, not client), no WBF crash anywhere nearby. That ruled out the "just corruption ripple" theory, so it got the full IDA treatment (headless `idat.exe` against the canonical `Fallout4_disk.i64`, decompiling the actual crash function and its full caller chain rather than guessing from the log tag).

**Fully understood now:**
- The `[AnimUtils] lastOp=GetBool lastVarName=bIsUnderwater` diagnostic in the crash log is a **red herring** — it's just the last thing our own `CoSyncAnimUtils` system happened to do a moment earlier (the only caller of that combination is `CoSynclocalplayer.cpp:601`, checking the *local* player's swim state). It has nothing to do with the actual crash.
- The real crashing function (`Fallout4.exe+0xBF712D`, resolved via IDA to `sub_140BF6F60`) and its direct caller (`sub_140BEDD20`) are confirmed — via literal embedded strings, not inference — to be **`BGSSaveLoadGame::SaveGame()` itself**: `"***Saving game: %s (%s)"`, `"---Finished saving game: %s"`, and `"Could not find form %08X with change flags %08X during BGSSaveLoadGame::SaveGame()"`. Both of its callers are the engine's own save-trigger entry points (autosave/quicksave/manual save, indistinguishable at this level).
- Mechanism: `SaveGame()` walks the engine's changed-forms list and, for each one, calls a location-descriptor builder to record where that reference currently is in the save file. That builder calls a helper that returns either the reference's own cell/worldspace pointer, or a global "default" fallback if the reference doesn't have one — then unconditionally reads a formID-like field 0x14 bytes into whatever that returned, **with no null check**. When both the reference's own location and the global fallback come back empty, that's a read from `[NULL+0x14]` — confirmed exactly against the crash registers (`RAX=0x0`) and live disassembly (`mov edx, [rax+14h]`).
- **Zero `DoxCoSync.dll` frames anywhere in this call stack** — this is entirely vanilla engine code, reached from the engine's own save pipeline. Nothing here to edit directly. It requires a "changed form" with no valid cell/worldspace attachment at the exact instant a save fires — not a normal state for an ordinary NPC, but exactly the kind of transient state CoSync's own actor-lifecycle machinery (`DisableWorldActor`/`EnableProxyActor` native calls on real placed NPCs, the constant spawn/delete cycle of dynamic proxy clones) can plausibly leave a reference in, momentarily.

**Why not fixed directly:** the only two real options are (a) hooking this engine function to add the missing null check — high-risk given this exact codebase's own documented history of hook-based fixes causing worse regressions in sensitive engine paths (e.g. the animation-graph-event hook that broke all NPC animation), or (b) finding exactly which CoSync actor-lifecycle operation leaves a reference cell-less long enough to be caught by a save, and closing that specific window. Neither is safe to attempt blind — (b) needs to know precisely when saves actually fire relative to CoSync's own Disable/Enable/spawn/delete timestamps, which nothing in the current logs captures (CoSync doesn't hook or log save events at all).

**Fix this round:** added exactly that missing visibility, nothing else. `Core/CoSync_Plugin.cpp`'s existing `OnF4SEMessage` handler (already used for `kMessage_GameLoaded`) now also logs `kMessage_PreSaveGame`/`kMessage_PostSaveGame` (both already-defined, already-used-elsewhere-in-the-codebase F4SE message types — not a new engine hook, just listening on the existing messaging interface). Purely diagnostic, zero behavior change.

Build 169 deployed. **Next step, not this round:** get a session where a save happens to fire (or force one — F5 quicksave — while NPCs are actively being disabled/enabled/spawned/despawned) and check whether `[SaveDiag] PreSaveGame fired` appears shortly before a crash, and correlate its timestamp against the nearest `DisableWorldActor`/`EnableProxyActor`/spawn/delete log lines to identify the exact operation that leaves a reference cell-less. Only once that's known should a targeted fix (narrow the gap in that specific operation, or a carefully-scoped hook) be attempted.

---

## [0.0.44] — 2026-07-21

### Client crash on join, root-caused: new instance of the known WBF/PlaceAtMe_Native crash class, one more base form blacklisted (build 168)
User reported: host went into a big/heavily-populated city, loaded fine, then P2 (client) crashed within about a second of joining. Checked the newest logs (`CoSyncUpdated_12424.log` = client, `CoSyncUpdated_2696.log` = host — host never crashed, ran clean the whole session and shut down normally).

Client log shows two VEH-caught fatal crashes 38 seconds apart, right as P2 joined:
- **Crash 1** (16:58:11): `lastTickStep=CoSyncWorldSync::Tick`, `RIP=Fallout4.exe+0x165910F`, `AV type=WRITE`, immediately preceded in the log by `RX WBF: base=0x001439D8 pos=(-122,-476,-635) from entity=3888389800`. Disassembled the live bytes at the fault RIP: `F0 0F B1 4F 04` = `LOCK CMPXCHG [RDI+4], ECX` — an engine refcount-release instruction, faulting because the pointer it's operating on resolves to a read-only page inside the game's own image rather than a valid heap object (RDI ends up numerically inside the Fallout4.exe module range, and the fault address is exactly `RDI+4`).
- **Crash 2** (16:58:49, 38s later): different location (`lastTickStep=RTGT:end`, `RIP=Fallout4.exe+0xBF712D`), null-ish READ AV at address `0x14`.

This is not a new bug — it's the **same crash class this codebase has hit and fixed reactively at least four times before** (see the `kWorkshopInternalHelperForms` set in `World/CoSyncWorldSync.cpp`: entries dated 2026-07-10, 2026-07-17, and twice on 2026-07-20, all citing the identical `Fallout4.exe+0x165910F` / `LOCK CMPXCHG`-on-read-only-page signature). The pattern is well-understood in this codebase even without knowing *why* the engine's `PlaceAtMe_Native` corrupts state for these specific base forms: the host's workshop-build scanner (`HostTickWorldSync`) picks up a buildable-type object near it, broadcasts it as a `WBF` packet so the client can place a matching copy, and for a small, empirically-discovered set of base forms, independently `PlaceAtMe`-ing a copy on a different machine reliably corrupts engine memory instead of just working. `PlaceAtMeOnPlayerSafe` wraps the native call in SEH specifically to catch and blacklist these at runtime per-session (`s_workshopFaultedBaseForms`) — but that only helps *after* a first fault is cleanly caught. This one wasn't: it went straight to a fatal, VEH-logged crash (matching this project's own round-31 finding that an uncontained `PlaceAtMe_Native` fault can leave engine state corrupted enough that unrelated work crashes again shortly after — explaining crash 2).

Base form `0x001439D8` was never in the static exclusion list, so the host kept broadcasting it every time a client got near/joined a session where this object exists nearby (a "big city" with lots of built/placed objects makes this much more likely to be hit). Added it to `kWorkshopInternalHelperForms` alongside the other 9 previously-found offenders, with the disassembly evidence documented same as the others. Same category as `0x0013FF17` — unidentified, blacklisted on empirical crash evidence rather than a known EDID.

Build 168 deployed. **Not yet tested — next test should replicate the same scenario (host in that same populated city, client joins) and confirm no crash; if it still crashes, check the new log for a *different* base form than `0x001439D8` in the RX WBF line right before the crash, since this fix only covers the one form seen this time.**

---

## [0.0.43] — 2026-07-21

### Real root cause found for "infinite loading on Load Game": cross-worldspace NPC bind was disabling real placed NPCs the client never loaded (build 167)
User's own account pinned this down where guessing hadn't: two test sessions, both hung on Load afterward, and both times P1 was in a heavily-populated **interior** ("a city") while P2 was **exterior** — with P2 "constantly trying to load all of them." That's specific enough to check directly against the code instead of theorizing further.

Confirmed in `World/CoSyncNpcWorldSync.cpp` + `Networking/Packets_EntityCreate.h`:
- `EntityCreatePacket` (the wire format for both player and NPC creates) carries only raw `spawnPos`/`spawnRot` floats — **no worldspace or cell field at all**. The player-proxy sync (`CoSyncWorldSync.cpp`) tracks `worldspaceFormID` and explicitly checks for a mismatch; NPC sync has nothing like it.
- Host discovers every NPC near it (interior city = dozens) and broadcasts `CREATE` for each, re-broadcasting every ~5s as a bind-retry mechanism.
- Client's primary bind path (`ClientBindWorldActor_Impl`, `CoSyncNpcWorldSync.cpp:994-1030`) resolves the NPC via `LookupFormByID(p.reserved0)` — explicitly documented as "cell-independent," meaning it resolves the real placed NPC's form **regardless of whether that NPC's cell is loaded on the client at all**, with no distance or worldspace check of any kind.
- Once resolved, the client called `SpawnProxyForWorldActor` → `CoSyncGameAPI::DisableWorldActor(realActor)` (an **engine-internal Disable()**) on a reference whose cell was never attached/loaded on that client, then spawned a visual clone at the host's raw interior-local coordinates (meaningless in the client's own worldspace).
- Disabling a real, persistent, ESM/ESP-placed NPC sets an actual engine "changed form" flag — which gets baked into **the client's own save file**, for an NPC the client's game never legitimately loaded or attached, potentially dozens of times per session (once per NPC near the host), continuously re-triggered by the host's 5s re-broadcast for as long as both players were online.

This is exactly load-shaped rather than crash-shaped: no VEH fault during the live session (matching every log collected so far — all clean), but a save full of nonsense changed-form disable flags for far-away persistent NPCs, which the engine then has to reconcile on the next Load.

**Fix**: added a guard to the primary bind path — `actor->parentCell != nullptr` — before proceeding with Disable+clone. If the resolved actor isn't actually attached to a loaded cell on this client (the cross-worldspace case), skip the direct bind and fall through to the existing cell-scan/clone-fallback path below, which was already scoped correctly (it only ever scans the local player's own current cell, so it naturally never touches far-away NPCs) and already safely no-ops the Disable step when the NPC isn't found locally. Single guarded condition change plus a log line explaining the skip reason (`CoSyncNpcWorldSync.cpp:1008-1023, 1047-1056`) — no changes to the fallback logic itself, no wire format change.

Superseds `[0.0.42]`'s map-marker theory as the leading suspect — that fix is still in (harmless, the feature never worked anyway) but this mechanism is a far better match: it's the first one found that's specific to *a real, repeated, user-described scenario* (interior/exterior split) rather than a generic "could plausibly happen" story.

Build 167 deployed. **Not yet tested — next test should replicate the exact scenario that triggered it: one player deep in a populated interior, the other exterior, play for a while, then have the exterior player Load a save from that session.** If the log now shows `not found in save or not loaded locally ... parentCell=00000000` for the interior NPCs instead of `CLIENT disabled ref=... spawning proxy`, the guard is doing its job.

---

## [0.0.42] — 2026-07-21

### Remote-player map marker disabled — leading suspect for "infinite loading on Load Game" (build 166)
User reported the game gets stuck on an endless loading screen specifically when pressing **Load Game** from the main menu — never on a fresh boot straight into hosting/joining, which is all four session logs collected tonight actually exercised. Checked the obvious things first and ruled them out with evidence, not guesses:
- The MainMenu row-injection code (`InjectMultiplayerEntry`) only ever touches `MainPanel_mc.List_mc` — decompiled the real `MainMenu.as` (already sitting in `swf_extract/decompiled/scripts/`) and confirmed the Load-game screen uses a completely separate widget, `SaveLoadObj.List_mc`. No collision possible.
- CoSync registers no custom F4SE save-data callback beyond the generic id-0 boilerplate — it writes nothing of its own into the co-save, so a corrupt custom chunk on load isn't the mechanism.
- Wrote a full structural validator and ran it against the live and repo `CoSync.esp` (byte-identical): every GRUP/record/subrecord accounts for its declared size exactly through to EOF, the one declared master (`Fallout4.esm`) is clean, and the 3 compressed `NPC_` records all decompress with matching declared/actual sizes. The esp container itself is not corrupt.

That same esp check did turn up something real, independent of the hang: the `[0.0.36]` entry claims a hex-patch added `XMRK`/`FNAM`/`TNAM` subrecords to the map-marker `REFR` (`0x0100B6CA`) to give it actual map-marker data. Direct inspection shows that record is still exactly `NAME`+`DATA`, 40 bytes, nothing else — that patch was never actually applied to the file that's live. Combined with `[0.0.41]`'s LoadScreenArt override folder also turning out to not exist on disk despite being marked "deployed," that's now two changelog entries describing esp/asset patches that didn't actually take effect — worth treating any future "deployed to live Data folder" claim as unverified until checked directly.

With the esp itself cleared, re-examined what CoSync's own code does to that specific REFR every session: `CoSyncPlayerMarker::ShowForProxy` (`Character/CoSyncPlayerMarker.cpp`) calls `Enable()` + `AddToMap(true)` on it whenever a remote player proxy spawns, and `Tick()` was calling `MoveTo(proxyActor, 0,0,0, false)` on it once a second for the entire session. Per `[0.0.37]`'s own earlier finding, this REFR's placement (`WRLD` → exterior-cell-block → "Cell Persistent Children" GRUP) has **no real backing `CELL` record anywhere in the hierarchy** — it's structurally orphaned. `MoveTo` works by detaching a ref from its current cell and reattaching it to the target's — doing that once a second, all session, to a ref with no valid parent cell to begin with is exactly the kind of state a save file would bake in as a "changed form," and restoring a changed form's cell attachment on Load for a ref whose origin cell doesn't really exist is a very plausible way to get a load that spins forever instead of failing cleanly. This is the first and only mechanism found tonight that's specific to *loading a save* rather than a fresh session — matching the actual reported symptom instead of just being plausible in general.

Since the esp was never actually finished (no XMRK/FNAM/TNAM — confirmed above), `AddToMap` has never revealed anything on the Pip-Boy map regardless of formID, so this feature has never done anything visible for the player. Disabled it at zero functional cost: `ShowForProxy` and `HideMarker` no longer touch the REFR at all (Enable/AddToMap/Disable removed, not stubbed out with dead unreachable code); `Tick`'s existing guard (`if (!s_markerRef || !s_proxyActor) return;`) now short-circuits permanently on its own since `s_proxyActor` is never set, so no code change was needed there. All 3 call sites (`CoSyncPlayer.cpp`, `CoSyncPlayerManager.cpp` ×2) are untouched. `Init()`'s read-only resolve-and-log still runs (harmless, doesn't touch the ref) so the diagnostic logging isn't lost.

Build 166 deployed (rebuilt — this was a real C++ change, unlike the last few asset-only entries). **Not proven — a hypothesis with a real mechanism behind it, not a confirmed root cause.** Next test: play a multiplayer session long enough for the marker's Enable/MoveTo activity to have been baked into a save (or just save mid-session), then try loading that save. If it no longer hangs, this was it. If it still hangs, this mechanism is ruled out and the next place to look is a fresh log captured during an actual Load-Game hang (still not obtained as of this entry — every log collected tonight went straight from the main menu into hosting/joining instead).

---

## [0.0.41] — 2026-07-21

### Loading-screen random model, take three: extracted the real game assets and patched a single float, instead of writing a NIF by hand
Build [0.0.40] reverted the hand-crafted empty-NiNode overrides after they caused the game to hang on load the first time they ever actually went live (see that entry). Confirmed: not a coincidence — a NIF file assembled from a general understanding of the Gamebryo header format, with no reference file and no validation tool, was never something to trust with 97 live deployments in the first place.

New approach, built instead of guessed:
1. **Wrote a small BA2 archive reader from scratch** (the format is genuinely simple and well-documented — 36-byte file-table entries + a length-prefixed name table) and used it to pull the real `LoadScreenArt\*.nif` files directly out of `Fallout4 - Meshes.ba2`/`MeshesExtra.ba2`. These are the exact bytes the game already loads successfully every time it shows one of these models — the strongest possible ground truth.
2. Rather than trying to fully parse or rebuild the NIF block structure (the exact mistake that caused the hang), found a **single, well-known, low-risk value to change**: a Gamebryo `NiAVObject`'s scale is a lone 4-byte float, always immediately preceded by an identity rotation matrix on an unrotated root object — a 40-byte pattern (`1,0,0, 0,1,0, 0,0,1, 1.0`) that's essentially unique and trivial to locate directly in the raw bytes without understanding anything else about the file. Setting that one float to `0.0` collapses the entire object hierarchy under it to zero size — invisible — while leaving literally every other byte in the file, including the header, block table, and all geometry, untouched and byte-identical to the shipped game asset.
3. **Verified each patch site before accepting it**, not after: confirmed the bytes immediately following the candidate scale float match the expected shape (a `0xFFFFFFFF` "no collision object" reference followed by a plausible small child count) before touching anything. 3 of the 97 candidates failed this check (implausible child counts in the billions — the pattern-match found a false positive elsewhere in those specific files) and were correctly **skipped rather than guessed at**; 2 more weren't present in either archive under the expected path. The other 91 passed and were patched.
4. Confirmed byte-for-byte that only the intended 4 bytes changed in each file (patched file size == original size, enforced by an assertion during the batch run, not eyeballed after the fact).

Deployed the 91 verified-safe overrides to the live Data folder, replaced the old broken repo copy at `F4se-Data\Data\Meshes\LoadScreenArt\` with these, and re-enabled the post-build deploy step (previously disabled in `[0.0.40]`) now that it points at genuinely different, much lower-risk content.

**Still only on the one machine tested tonight — the user separately reported the previous (broken) fix "removed the model for host but not client," meaning these files have never been on the client machine at all. They need to be copied there the same way the DLL is for every build.**

Not rebuilt (no C++ changed) — pure asset replacement, same as `[0.0.39]`/`[0.0.40]`. **Untested. This is the third attempt at this specific fix; given what happened with the second one, treat the first loading screen after deploying this as a real test, not a formality — if anything looks wrong, stop and report back immediately rather than continuing to play.**

---

## [0.0.39] — 2026-07-21

### The loading-screen random 3D model fix from build 156 has NEVER actually been deployed — real path bug found and fixed (no rebuild needed)
User clarified: the pre-existing `CoSyncLoadingScreen.cpp` background-texture injection ("Texture already present, skipping inject" in every log tonight) is untouched and working correctly — the actual complaint is a random image/3D model that appears on top of/during the loading screen, which was diagnosed hours ago as FO4's `LoadingMenu` randomly picking one of 97 `LoadScreenArt\*.nif` models baked into `Fallout4.esm`, neutralized by deploying an empty-`NiNode` override for each.

That fix has **never actually taken effect** — a bash path-substitution bug when the 97 files were originally deployed produced filenames like `LoadScreenArtArmor01PowerArmor1.nif` instead of the required `Armor01PowerArmor1.nif`. The files sat in the correct `Data\Meshes\LoadScreenArt\` folder the entire time, just under names the engine was never going to look for — a loose-file override only works if the filename matches exactly. Confirmed via direct inspection: all 97 deployed files had the mangled name, zero had the correct one, so the vanilla random-3D-model behavior has been active this whole time regardless of anything shipped since.

Fixed by renaming all 97 files to their correct names (verified against the original 97-path list, zero missing). Also stashed a corrected copy in the repo at `F4se-Data\Data\Meshes\LoadScreenArt\` (previously these existed *only* as a hand-deployed one-off in the live Data folder, not tracked anywhere, so a future clean install would have silently lost the fix) and wired an `xcopy` step into the existing post-build deploy command so it's now maintained the same way `CoSync.esp` and the logo PNG already are.

No C++ changes, no rebuild — purely a deployment/asset-path bug. The separate ImGui-drawn logo overlay (DX11Hook + `PollLoadingMenuState`) is unaffected by this and remains a secondary, still-unconfirmed piece — this fix addresses the primary complaint (the random model itself) independent of whether the logo overlay ever successfully draws on top.

**Untested. This is the first time the NIF-neutering fix has ever actually been live — next loading screen should show no random 3D model/image at all.**

---

## [0.0.38] — 2026-07-21

### Root-caused the "named NPCs desync/duplicate" complaint: a real, repeatable crash-storm in the wide-radius discovery scan (build 165)
User reported: certain NPCs — specifically ones with dialogue or special actions (unique/quest NPCs) — either sync as a visible duplicate (clone spawned, original never removed) or, for named enemies, don't sync at all; enemy *variants* (legendary, super mutant types, ghoul/glowing ghoul) also affected. Checked the two-machine session logs (07:27-07:41, three log files spanning a client reload) rather than guessing.

Found a real, repeatable, previously-undiagnosed bug: `TickScanWorldActors_Impl`'s wide-radius discovery filter (`considerRefr`, from build 154) calls `IsActorDead()` — which touches `actorValueOwner`, fatal on any AI-disabled actor per this codebase's own `CoSyncActorValueSafety.h` rule (round 30) — **before** checking whether the actor is already tracked. Our own clone-fallback NPC proxies are AI-disabled by design and use the *real* NPC's base form (not a distinguishable dynamic one), so the existing "skip CoSync's own actors" filter never catches them. Result: once a clone existed, the scan re-discovered it and re-ran the fatal AV check on it **every single scan tick, forever** — confirmed directly in the client log: the same actor pointer, same Health AVI, faulting on `GetValue` then `GetMaximum`, three times within one second, repeating throughout the session. It's SEH-caught (not an actual crash to desktop — this codebase's exception recovery worked as designed), but repeatedly hitting the fault/recovery path is exactly the kind of thing this project's own history already flags as capable of "masking/mutating into delayed corruption." A registration or disable step landing in a corrupted state after being interrupted by this exact crash loop is a very plausible mechanism for "clone appeared, original was never removed."

Two fixes:
1. **Root cause**: `considerRefr` now excludes already-tracked actors (`s_actorToEID`/`g_CoSyncEntities`) *before* calling `IsActorDead`, not after — matching what the later registration loop already checked, just too late to prevent the repeated fault.
2. **Safety net, codebase-wide**: `IsActorDead` itself (used at 4 call sites — discovery filter, departure check, dynamic-spawn suppression) now permanently blacklists any actor whose `actorValueOwner` access ever faults, sharing the same set the HP/legendary broadcast loop uses. That broadcast loop was the one place in the codebase still missing this blacklist (every sibling health-check loop elsewhere already has it) — fixed to match.

Also checked while investigating: the recurring `0x165910F` WBF/PlaceAtMe fault (thought fixed at build 152) is still happening for *new* transient object base forms not on the earlier static blacklist — but it's self-healing (SEH-caught, auto-blacklists per object after one fault, non-fatal) and unrelated to the NPC-duplication complaint, so not chased further tonight. Legendary sync (`LGD|`) never fired at all this session — no legendary enemy was actually encountered/logged, so the variant-enemy angle specifically is still unconfirmed either way; worth testing again with an actual legendary/glowing-ghoul/super-mutant-variant encounter now that this fix is in.

Map marker and loading logo: still not visually confirmed by the user. No new information this round beyond `[0.0.37]`'s diagnosis (marker needs the CK after all; logo now has real diagnostic logging waiting on a test that includes an actual loading screen).

Build 165 deployed. **Untested. Next test should specifically include: a named/unique/dialogue NPC near a client (to check duplication is gone), and if possible a legendary or glowing/variant enemy encounter (to get the first-ever `LGD|` log evidence and confirm or rule out that angle).**

---

## [0.0.37] — 2026-07-21

### First real test of builds 156-163: doors and item-drop sync CONFIRMED working; map marker still broken; loading logo unverified; one unrelated pre-existing crash (build 164)
User ran a real two-machine session on build 163 (07:10-07:15). Results:

**Doors — confirmed working end-to-end.** 4 separate door activations across the session, both directions (client opens → host applies; host opens → client applies), clean `TX DOOR`/`RX DOOR`/`FLUSH applied` sequences every time, zero faults. The hook that had never fired once in this project's history now demonstrably works.

**Item-drop sync — confirmed working on its very first real test**, and it exercised the trickiest path on the first try: the *host* dropped the item locally. Log sequence: `HOST allocated entity=196608` → `OWNER-CONFIRM ... -> my ref=...` (the direct in-process apply path for host-originated drops, added specifically because `CoSyncTransport::Send()` has no loopback) → client `SPAWN clone entity=196608`. Exactly the designed flow, no manual intervention. Found one cosmetic bug in the same log: the "TX DROPSEEN" message printed unconditionally even on the host's direct-apply path, where no such packet is ever sent — fixed to log `LOCAL (host) drop detected` instead. No pickup was tested this session (no `DROPGONE` in either log) — that half of the loop is still unverified.

**Map marker — still broken.** Despite the build-163 ESP patch, `LookupFormByID` still can't find the marker record on either machine (`Marker REFR 0x0100B6CA not found`). Dug further and found the underlying record's placement is more structurally unusual than the patch accounted for: it sits in a `GRUP` type 8 ("Cell Persistent Children") directly under a type-1 "World Children" group, with **no actual `CELL` record present anywhere in that hierarchy** — group label `0x3D` presumably refers to some cell, but nothing in the file defines it. This looks like the record was already unloadable in its original, pre-patch form (never noticed before because nothing had ever tried to resolve it), and my patch — while structurally self-consistent by the checks I ran — didn't fix the actual defect, because I was checking group/subrecord byte-accounting, not the deeper "does this GRUP nest under a real owning CELL" requirement. Deeper Fallout4.esm archaeology to confirm the exact fix would be disproportionate effort at this point — **this most likely needs the original Creation Kit path after all**, with the added benefit that we now know exactly what's wrong (no backing CELL record) if whoever does it wants to verify the fix in xEdit/CK first.

**Loading screen logo — unverified, added real diagnostics.** The PNG texture loaded successfully (`loading logo loaded 4222x4092`) but there was **zero log visibility** into whether the `LoadingMenu` open-state poll (the thing that's actually supposed to trigger drawing it) ever fired or returned true — I never added a log line for it. Confirmed via exe string search that `"LoadingMenu"` is the correct menu name, so that's not the bug. Most likely explanation: this test session never triggered a loading screen after the initial boot (the very first boot screen is structurally invisible to this hook regardless — `Present()` can't be hooked before DX11 initializes). Added proper heartbeat + state-transition logging (`PollLoadingMenuState heartbeat`, `LoadingMenu state changed -> OPEN/closed`) so the next test — ideally one that includes a fast-travel or cell load — gives an unambiguous answer instead of another guess.

**One crash, unrelated to any of tonight's work.** Client crashed twice in quick succession (second one fatal, ended the session) inside `TickHook:ActorUpdate_Hook`, a pre-existing function untouched this entire session. No door/drop/PA/dialogue activity in the window immediately before it — just routine NPC position-sync traffic for the 5 tracked world actors. Flagging as a real, separate bug worth investigating later, but not something introduced by tonight's changes.

**Untested this session** (no in-game opportunity arose): Power Armor pieces, dialogue restriction (neither player initiated dialogue), AP bar visual confirmation (packets with AP fields ARE flowing correctly on the wire, but no visual confirmation from the user).

Build 164 deployed (loading-screen diagnostics + item-drop log fix only — no behavior changes to anything already confirmed working).

---

## [0.0.36] — 2026-07-21

### Map marker: actually finished, no Creation Kit required (build 163)
Task #1, the one item left open at the end of tonight's roadmap pass — was blocked on a manual CK step. Found a way to finish it without CK after all.

Re-examined `CoSync.esp` directly (6549 bytes, tiny) rather than accepting "add a new record in CK" as the only path. Found a **worldspace-placed `REFR` already sitting in the file** (Commonwealth worldspace, correct base form `0x00000010` — the vanilla generic `MapMarker` STAT confirmed present in `Fallout4.esm` earlier tonight — and the correct `Initially Disabled` flag, `0x800`) — someone got partway through this in the CK on 2026-07-15, placed the reference correctly, but never actually checked "Has Map Marker" (no `XMRK`/`FNAM`/`TNAM` subrecords existed on it at all) and it landed at local formID `0x00B6CA`, not the `0x0000BE64` the C++ code has been hardcoded to look for since that same day. Two independent misses stacked on each other — either one alone would have kept this broken.

Fixed by hand-patching the esp directly, informed by a real, working multiplayer mod's own file-format parser (TiltedEvolution/Fallout Together's `es_loader` component ships exact `XMRK`/`FNAM`/`TNAM` subrecord layouts, sourced from real reverse-engineering rather than guesswork) — appended `XMRK` (zero-length has-marker flag) + `FNAM` (`0x03` = `VISIBLE | TRAVEL_LOC`) + `TNAM` (`0` = generic icon) to the existing record (21 bytes total), and propagated the size delta through its 3 enclosing `GRUP` headers (exterior-cell-block → world-children → top-level). The record happened to be the very last thing in the file, so this was a pure append + 4 integer-field patches — no byte-shifting elsewhere in the file. **Self-verified before ever touching the live game**: wrote a script that fully re-parses the patched file end-to-end (every group's declared size checked against its actual byte span, every record's subrecords checked to land exactly on the record boundary, whole-file walk confirmed to land exactly on EOF) — this is the check that was missing from the "raw ESP patching is too risky without a validator" concern raised earlier tonight; with it, the risk profile is much closer to the safe end.

`CoSyncPlayerMarker.cpp`'s `kMarkerLocalFormID` updated from the never-correct `0x0000BE64` to the real `0x0000B6CA`. Original esp backed up (`CoSync.esp.bak_before_marker_patch_20260721`) before the patched version replaced it in the repo; the existing post-build deploy step carried it to the live Data folder alongside the DLL, same as every other build tonight.

Combined with `[0.0.32]`'s two code fixes (NPC spawns no longer steal the marker; fast-travel is no longer hardcoded off), this should be functionally complete now. Build 163 deployed. **Untested — next test: the marker should appear on both players' Pip-Boy maps tracking the other player, and fast-travel to it should work.**

---

## [0.0.35] — 2026-07-21

### Quest sync audit: already far more complete than expected. Host-only dialogue restriction added. RVA regression caught and fixed before it ever reached a test (builds 161-162)
Task #10, the last of tonight's roadmap. Audited `World/CoSyncQuestSync.cpp` before writing anything new, expecting to build most of this from scratch — instead found a genuinely complete, already-host-authoritative system:
- Quest **start** and **completion** are polled host-side (`TickLocalQuests_Impl`, gated `if (!CoSyncNet::IsHost()) return;`) and broadcast; clients apply via `Start()`/`CompleteAllObjectives()` Papyrus calls, with a 15s re-broadcast of all active quests so late joiners catch up.
- Quest **stage advancement** goes through a live, verified engine hook (`CoSyncQuestHook.cpp`, `SetCurrentStageID` RVA `0x1185DD0`) that explicitly **blocks clients from advancing stages locally** ("Host is authoritative — clients must not advance quest stages on their own") and replays the host's stage-set through the real engine function on the client. Because it's the *real* `SetCurrentStageID`, not a side-channel, any reward-granting fragment script Bethesda attached to that stage **already runs on the client too** — "quest complete, receive rewards" was already solved, not something this session needed to add.
- Objective-level sync (complete-all / set-completed / set-displayed) also exists and is wired.

The one gap against the user's ask: nothing prevented a client from independently opening dialogue with any NPC (quest sync tracked the host's dialogue for display purposes, but never blocked the client's own). Fixed in **Papyrus**, not C++ — deliberately: blocking Activate at the engine level this late in a long session felt like unnecessary risk when a standard, always-safe vanilla call does the job. `CoSyncQuest.psc` now registers for `DialogueMenu` open/close (same `RegisterForMenuOpenCloseEvent` pattern already used for `WorkshopMenu`/`PipboyMenu`) and reactively closes it with `UI.CloseMenu("DialogueMenu")` the instant a non-host client opens one, with a `Debug.Notification` explaining why. Scoped to *all* NPCs, not just quest NPCs — Papyrus's `OnMenuOpenCloseEvent` doesn't pass which NPC is being talked to, and there's no cheap way to answer "is this specific NPC a quest NPC" from inside that handler; blocking client-initiated dialogue universally is simpler, safer, and still satisfies "only host converses." Recompiles automatically via the existing post-build Papyrus step — confirmed both compile passes succeeded and the deployed `.pex` timestamp updated.

**Caught and fixed a real bug in tonight's OWN earlier work before it ever reached a test**: while reading `CoSyncDialogSync.cpp` for reference, found it documents a round-104 finding that the exact `UI::IsMenuOpen` RVA I derived and shipped in build 156 (`0x1A7FE60`, for the loading-screen logo) is **not actually a function entry** — mid-body of a different function, per an earlier session's more careful investigation (entry_match=False). My own fresh IDA check tonight found `is_func_head=True` there with a plausible-looking decompile, which turned out to be a false positive (some other, coincidentally-shaped function happens to start at that address). Replaced with `CoSyncDialogSync.cpp`'s own already-battle-tested address (`0x1A7FDD0`, live in dialogue detection already) — same signature, drop-in swap. Also hit a genuine C2712 (`__try` sharing a function with a `static const BSFixedString` — the *static-initialization guard* needs unwind info too, not just automatic-storage objects) while wiring the fix; caught by the compiler immediately, fixed by isolating the SEH call into its own tiny function, matching `CoSyncDialogSync.cpp`'s own `SafeIsDialogueMenuOpen` shape exactly.

Also noted, not investigated further: `CoSyncQuest.psc`'s `ScanCellDoors()`/`OnActivate`-per-door registration appears to be a **second, independent, Papyrus-side door-sync mechanism** (`CoSync.NotifyActivate`/`IsDoor` natives) running in parallel with the C++ `CoSyncDoorHook` MinHook fixed earlier tonight. Likely redundant rather than conflicting (both would broadcast the same door state), but worth knowing if door sync ever double-fires or behaves oddly in testing — flagging for whoever investigates that next, not chasing it now.

Build 162 deployed (includes every fix from tonight's session: builds 156-162). **Dialogue block untested. Quest stage/start/complete sync is NOT newly built by me tonight — it was already live going into this session — so if it doesn't work in your next test, that's a pre-existing gap to diagnose, not a regression from tonight.**

---

## [0.0.34] — 2026-07-21

### Dropped-item sync — new feature, both directions (build 160)
Task #9: P1 drops something from inventory, P2 sees it appear and can pick it up (and vice versa). No vanilla Papyrus event exists for "player dropped this item," and adding a new unverified engine hook in the same session as the door-hook fix (never fired before tonight) felt like unnecessary stacked risk — so detection is polling-based instead: a tight-radius (220-unit) scan around the local player, once a second, for newly-appeared untracked dynamic lootable refs (MISC/WEAP/AMMO/KEYM/ALCH — same filter `CoSyncWorldSync::IsLootableBaseType` already uses), reusing the exact cell-scan pattern already proven in NpcWorldSync/ContainerSync.

Architecture mirrors this session's already-tested NPC "Stage 2" ownership design: the reporting machine sends `DROPSEEN`, the host allocates an entity ID (new range `0x00030000-0x0003FFFF`) and confirms with `DROPCREATE` (`ownerEntityID` = reporter), the owner just tracks its own real ref, every other peer spawns a visual clone via a new `CoSyncGameAPI::SpawnItemAtPosition` (same `CreateReferenceAtLocation` primitive `SpawnRemoteActor` already uses, minus actor-specific setup — deliberately not `PlaceAtMe_Native`, which `CoSyncPowerArmorSync.cpp`'s own comments describe as having "a documented fault history"). Picking up either copy (via the already-fixed pickup hook) broadcasts `DROPGONE` so every peer removes their copy — the real owner deletes their original, clone-holders despawn their clone.

Caught and fixed two design bugs before shipping (not in-game — by tracing the actual data flow):
1. **`CoSyncTransport::Send()` has no local loopback.** If the host is the one who drops an item, broadcasting `DROPSEEN` outward and waiting to "receive" the resulting `DROPCREATE` back would never fire — the host never receives its own broadcast. Fixed by branching in the scan: if I'm the host, allocate directly in-process (`HostAllocateDrop`) instead of sending; the same function also applies its own result in-process immediately after broadcasting, for the same reason. This is the identical lesson already learned for NPC Stage 2 (`ClientBindWorldActor` called directly rather than waiting on self-receipt) — worth remembering as a general rule for this architecture, not a one-off.
2. **C2712 hazard**: the first draft mixed raw `__try` blocks with non-trivial-dtor objects (`std::vector`, `VMArray`) in the same function — the exact class of bug this codebase's own comments warn about repeatedly. Restructured into the established `_Impl` (objects, no `__try`) + `Safe` (SEH, no objects) split before it ever reached a compiler.

Known simplification: stack counts aren't preserved (dropping 5 of an item spawns a clone as a single item, not a 5-stack) — visually correct, quantity not yet matched. Not fixed now; flagged for a follow-up if it matters in practice.

Build 160 deployed. **Completely untested in-game — this is a brand-new feature, staged similarly to how NPC sync was proven incrementally. Test focus: drop an item near the other player, confirm `TX DROPSEEN`/`HOST allocated`/`SPAWN clone` log lines and a visible, pickup-able item on the peer's screen; then pick it up on either side and confirm `TX DROPGONE` removes both copies.**

---

## [0.0.33] — 2026-07-21

### Door sync: dead RVA (never fired, ever) + disabled install, both fixed. Power Armor pieces: missing EquipItem call fixed (builds 158-159)
Continuing the roadmap: tasks #7 (doors) and #5 (Power Armor).

**Doors — never actually worked, for two independent reasons, both fixed:**
1. `kActivateWorker_RVA` (`0x514180`) and `kSetOpenState_RVA` (`0x305760`) both land in `0xCC` (int3) padding in the disk IDB, not a function head — the exact same +0x10 dump-skew defect class already fixed for the pickup hook in round 133. Scanned forward past the padding: real entries are `0x514190` (59 callers, 7-arg signature matches this file's existing `ActivateWorkerFn` exactly) and `0x305770` (26 callers). `SetOpenState` turns out to be dead/unused in the current apply path anyway (doors are re-toggled by re-invoking `ActivateWorker` with a null activator, not by calling `SetOpenState` directly) — fixed its stored constant for correctness, zero behavior change.
2. Even with a correct RVA, `InstallDoorHook()` was commented out at the call site, disabled since round 146 over a suspected use-after-free (`RAX=FEEEFEEE` at `f4se+0x161F4`) blamed on "every door-open hits a cell-transition→UAF path." That crash signature is *exactly* what the `Core/Tasks.h` ITaskDelegate vtable-slot-order bug produced (root-caused and fixed this session at build 151) — a global F4SE task-queue corruption bug, unrelated to doors, that happened to get triggered by the door-open→cell-transition→NPC-rescan sequence. With that root cause fixed, the actual blocker is gone. Re-enabled.
Net effect: the door-sync hook has never fired even once in this project's history until now — RVA dead, and the install call disabled on top of that.

**Power Armor pieces — root cause of "bare frame" found:** the receive-side piece-attach code (`CoSyncPowerArmorSync.cpp`, already present) called `AddItem` on the spawned chassis for each piece but never `EquipItem` — items landed in the frame's inventory but were never worn, so nothing visually attached. Fixed: each piece now gets `AddItem` followed by `EquipItem`, both queued on the chassis ref's own VM queue (order guaranteed) and packed through the round-141 typed-VM-arg fix (`CoSyncVM::SetFormArg`) so the handle carries the piece's real `ARMO` type. Deliberately NOT using `CoSyncEquipHook::EquipItemDirect` here — that function calls `ActorEquipManager::EquipObject`, which requires a real `Actor` (`actorValueOwner` etc.); a PA chassis is a plain `TESObjectREFR` (FURN), so the native actor-equip path doesn't apply — `ObjectReference.EquipItem` (the same function vanilla CK PA-frame scripts use) is the correct call here.
Audited the rest of the PA pipeline while in the file: entry/exit spawn+switch+chassis-cleanup, PA-specific locomotion gait thresholds (distinct speed tiers for PA vs normal movement), and the weapon-draw settle delay are all already implemented — this looks substantially more complete than the "bare frame only" status suggested. Also confirmed the full trigger chain is real and wired: `CoSyncQuest.psc`'s `OnSit`/`OnGetUp` handlers (F4SE has no native PA enter/exit events, so this piggybacks on furniture-sit events + `IsInPowerArmor()`) call `CoSync.NotifyPowerArmor()`, which reaches `Papyrus_CoSync.cpp`'s native and scans the chassis inventory for equipped `ARMO` pieces before sending.
Build 159 deployed. **Not yet tested. Next test: open/close a door on one machine, confirm it mirrors on the other (log: `[DoorHook] TX DOOR` sender-side, `[DoorHook] FLUSH applied` receiver-side). Separately, enter Power Armor with pieces installed and confirm the proxy's chassis shows the pieces attached, not just a bare frame.**

---

## [0.0.32] — 2026-07-21

### Map marker root-caused: the CK record has never existed in any copy of CoSync.esp; two real code bugs also found and fixed (build 157)
Investigated task #1 ("map marker does not work, needs to allow teleport, needs to show on both host+client"). Scanned every `CoSync.esp` copy found on disk (deployed copy, repo copy, `GameFiles` copies, and a suspiciously-named `CoSync.esp.bak_markerfix`) for the EDID `CoSyncRemotePlayerMarker` that `CoSyncPlayerMarker.cpp` has depended on since 2026-07-15 (formID `0x0100BE64`/local `0x00BE64`) — **none of them have ever contained it.** `SafeResolveMarkerRef` has been silently failing every single session (`[PlayerMarker] ... marker REFR not resolved` in the logs), so `s_markerRef` has always been null and the whole subsystem has always been a no-op. This is separate from and in addition to two real code bugs found and fixed in the same pass:
1. **The marker was being stolen by NPC spawns.** `CoSyncPlayerMarker::ShowForProxy` was called from `CoSyncPlayer::SpawnInWorld` for every spawned proxy — player AND NPC/companion clones alike — repointing the single marker at whatever last spawned, usually an NPC clone within the first second of a session. Fixed: `CoSyncPlayer.cpp` now gates the call on `createType == CoSyncEntityType::Player`.
2. **Fast travel was hardcoded off.** `AddToMap(false)` — the boolean is the fast-travel-allowed flag on the standard Bethesda `ObjectReference.AddToMap` scripting function. Changed to `AddToMap(true)`.
Considered fixing the missing record without the CK: dynamically creating the marker via the same `CreateReferenceAtLocation` engine call already used for actor proxies, from the vanilla `MapMarker` STAT base (`0x00000010`, confirmed present in `Fallout4.esm` by direct record scan — same formID as Skyrim's legacy generic marker). But `AddToMap`/`RemoveFromMap` are not simple engine natives (no native registration found in the disk IDB — the "AddToMap" string only appears once, at a generic Papyrus function-registrar call site, not a native impl); a reference gets its actual map-marker data from a CK-baked `ExtraMapMarker` block, which nothing in the vendored SDK or a found native exposes a safe way to attach at runtime. Going further would mean guessing at an undocumented internal extra-data layout with no way to test it before it reaches a live game — the wrong byte there risks crashing the Pip-Boy map, not just leaving the marker missing. Deferred rather than risked.
**User action needed** (CreationKit.exe is installed at the game root): open CoSync.esp in the CK, place a new reference of base `MapMarker` (STAT `0x00000010`) in any interior cell (position doesn't matter — the plugin `MoveTo`s it every tick), give it EditorID `CoSyncRemotePlayerMarker`, check "Has Map Marker" for map marker data, name it e.g. "Remote Player", and set it Initially Disabled. Save. That's the exact record the code has been looking for since round 15 — once it exists, the two round-32 code fixes above should make it track the remote player and allow fast travel on both sides.
Build 157 deployed (the two code fixes are live regardless of the CK step). **Task #1 stays open pending the CK addition** — not something I can complete without GUI/CK access.

---

## [0.0.31] — 2026-07-21

### Overlay: AP bar added, all text stripped from health bar; custom loading-screen logo; vanilla loading-screen art suppressed (build 156)
User requested a larger feature roadmap (10 items tracked); this build covers the two overlay/UI items.

**AP bar under health bar, text removed from health bar:**
- `HP|` wire packet extended with two optional trailing fields (`ap`, `apMax`) after the existing `level`/`rads` fields — old 4/5/6-field packets from a mismatched-build peer still parse correctly (`CoSyncHealthSync::SerializeHP`/`OnReceiveHP`).
- Local player now reads the `ActionPoints` ActorValue (formID `0x0002D5`) alongside Health/Rads each tick (`CoSynclocalplayer.cpp`) and sends it on a 2-point delta threshold.
- `CoSyncOverlay_UpdateLobbyAP` is the new apply-side entry point (`GUI/CoSyncOverlay.h/.cpp`), called from `CoSyncHealthSync`'s `SetHealth` apply path whenever a packet carries AP.
- `CoSyncOverlay_RenderLobbyBar`: removed the player name, level chip, and the "current / max HP" text drawn over the health bar — the bar is now pure color, no text at all. Added a thin (6px) AP bar directly under the health bar, filled proportionally, with a small "AP" label to its right (the only text on the card now).

**Loading screen: removed the random vanilla art/icon, added a custom logo:**
- Root cause: FO4's `LoadingMenu` picks a random `LoadScreenArt\*.nif` from a fixed set of 97 vanilla models (armor/creature/city renders) baked into `Fallout4.esm` — that's the "random icon with the custom background" the user saw, not something CoSync was drawing. Neutralized by generating a minimal valid empty `NiNode` NIF (176 bytes, correct Gamebryo/BSStream 20.2.0.7/130 header for FO4) and deploying it as a same-named override for all 97 `LoadScreenArt\*.nif` paths (loose-file override via the game's existing `bInvalidateOlderFiles=1` archive setting — already enabled in this install's `Fallout4Custom.ini`, no .ini change needed). The vanilla loading menu still runs; it just renders nothing.
- Added a real custom logo in its place: `DX11Hook.cpp` gained a WIC-based PNG loader (`TryLoadLoadingLogo`, one-shot, decodes to `DXGI_FORMAT_R8G8B8A8_UNORM` via `IWICImagingFactory`/`IWICFormatConverter`) that uploads an immutable `ID3D11Texture2D` + SRV from `Data\F4SE\Plugins\CoSyncLoadingLogo.png`, and `RenderLoadingLogo()` draws it centered (28% of screen height, aspect-correct) into ImGui's background draw list whenever the loading screen is active — gated on a new game-thread poll, not a guess: `CoSyncRuntime.cpp`'s `PollLoadingMenuState` calls `UI::IsMenuOpen("LoadingMenu")` every 15 ticks (RVA `0x1A7FE60` on `g_ui` at `0x30DD8B0`, IDA-verified 2026-07-21 against the disk IDB — decompile confirmed it's the locked `menuTable` lookup checking the instance's 0x40 open-flag byte) and feeds the existing `CoSyncOverlay_ShowLoadingScreen` flag (previously only ever set by the world-actor CREATE-on-loading-screen-signal path, never by an actual menu-state check).
- Deployed the user-supplied gold "Commonwealth Online" logo as `CoSyncLoadingLogo.png`; also copied into the repo at `F4se-Data\Data\CoSyncLoadingLogo.png` and wired into the existing post-build deploy step (`DoxCoSync.vcxproj`) so a fresh checkout/build deploys it automatically, matching how `CoSync.esp` is already handled. The PNG loader is defensive — if the file is missing, `TryLoadLoadingLogo` logs once and no logo draws; nothing crashes or blocks the loading screen.
Build 156 deployed. **Not yet tested. Next test: enter a loading screen (fast travel, cell transition, or session connect) and confirm (1) no random vanilla loading art appears, only the custom background, (2) the gold logo renders centered, (3) the lobby health bars show color-only bars with a new thin AP bar and "AP" label, no other text.**

---

## [0.0.30] — 2026-07-20

### Split-up crash root-caused: player proxies were never covered by the freed-actor guard (build 155)
23:20 session (host `_36500` / client `_8356`): the host walked ~9,400 units from the camp — beyond the client's loaded cell grid — and the client's engine unloaded the cell holding the HOST'S PLAYER PROXY, freeing the actor. Two SEH/VEH-recovered READ AVs at the freed-heap poison address followed, PDB-resolved to `TickAnimVars → IsGraphReady → Is3DLoaded` and `TickSmoothing → PositionRemoteActor` — both dereferencing the freed `actorRef`. The round-149 freed-actor guard at the top of the entity tick loop only ever applied to `IsWorldActorEntityID` entities; **player-proxy (and companion) entities had no guard at all** — a latent bug reachable any time the two players split beyond grid distance, surfaced now because this was the first real split-up test. (Build 154's wide-radius discovery itself behaved: 5 NPCs registered, no churn, no WBF/0x165910F noise.)
Fixes:
- The freed-actor `IsFormStillLive` guard now covers EVERY remote entity with an `actorRef` (player proxies, companions, world actors alike). World actors still drop `spawnQueued` on clear (they re-bind via the owner's CREATE re-broadcast); player proxies keep it so the SpawnWatchdog can respawn them.
- `SpawnWatchdog` gained a range gate: it postpones respawning any proxy whose authoritative position is >12288 units (3 cells) from the local player — without this, the freed host-proxy would spawn/free/spawn every 3 seconds for as long as the players stay apart. Spawning resumes automatically once the remote side is back in range (position then corrected by the next EU as usual).
Build 155 deployed. **Next split-up test: expect a single `entity=... actor=... freed (unloaded) — clearing` line on each side when the distance opens up, no crashes/VEH storms, and the proxy re-appearing cleanly when players regroup.**

---

## [0.0.29] — 2026-07-20

### NPC discovery widened from "the one cell the player stands in" to the entire loaded area (build 154)
User request: "instead of host-gets-near-it-updates, sync anything within 5-10 cells." Engine reality: Fallout 4 only loads/simulates cells in the `uGridsToLoad` grid (~±2 cells) around each local player — beyond that no actor objects exist to sync (which is exactly the gap the Stage 2 client-report pipe covers from the other player's side). But the scan wasn't even using what IS loaded: `TickScanWorldActors_Impl` only ever walked `localPlayer->parentCell` — a single cell — so an NPC one cell over was invisible to discovery despite being fully simulated.
Discovery now walks the engine's own **ProcessLists high-actor handle array** — the AI manager's flat list of every actively-simulated actor in the loaded area (whole grid + attached interiors). IDA-verified for 1.11.221 (disk IDB): singleton at RVA `0x30DC140` (found via the console `ToggleAI` handler 0x5EA650; struct layout cross-confirmed against Fallout Together's FO4 headers and the engine's own AI-resume walker `sub_140DAF350` — handle data at +0x40, count at +0x50), handles resolved through the already-vendored `LookupREFRByHandle` (0x2A0540, refcount-correct via `NiPointer`). The old parentCell objectList loop remains as an automatic fallback if the singleton reads null/insane, and the whole filter chain (NPC-base, proxy-base exclusions, teammate, disabled, dead, dynamic-base) is unchanged — hoisted into one shared predicate used by both paths and both roles (so the client's Stage 2 NPCSEEN reports also cover its full loaded area now; combined coverage = host grid ∪ client grid, the practical maximum).
New `kDiscoverRadius` gate = 16384 (4 exterior cells) around the local player; departure `kKeepRadius` raised 12288 → 20480 (5 cells) so keep > discover and boundary NPCs don't churn through register/destroy cycles. Sanity caps on the handle array (count < 4096) before trusting it.
Build 154 deployed. **Not yet tested. Next test: NPCs a cell or two away from either player should register/sync without anyone walking right up to them — watch for more `Registered actor` entries per camp than before, and no register/destroy churn in the log while standing near a camp edge.**

---

## [0.0.28] — 2026-07-20

### Originals never disappearing on the client, root-caused: enable-parent refs refuse Papyrus/console Disable — replaced with the engine's internal disable (build 153)
Build-152 test (host `_17996` / client `_36844`): perfect log hygiene — zero WBF broadcasts, zero PlaceAtMe faults, zero VEH crashes, zero NPCSEEN chatter, all 5 clones bound+spawned first-attempt. But the user's visual report was the key: **clones spawned and followed the host's NPCs, while the client's original NPCs were still standing there too** — `Disable()` on the originals has been a silent no-op the whole time.
Root cause, IDA-proven (disk IDB, via the console command table — `Enable` exec 0x5C45F0 / `Disable` exec 0x5C47D0, ObScript opcodes 0x1021/0x1022): both the console and Papyrus disable paths explicitly check whether the ref has an **enable-state parent** and, if so, log "Disable is being called on reference ... even though it has enable state parent ... will be ignored" (to the Papyrus log we never read) and return without doing anything. CK-placed camp/settlement NPCs routinely have enable parents (tied to quests/encounters), so every camp original survived. The handlers then call the real engine internals for the non-parented case:
- disable = `sub_1405B3EF0(TESObjectREFR*, char fadeOut)` (RVA 0x5B3EF0)
- enable = `sub_1405B4440(ref)` + `sub_1405B4150(ref)` when `flags & 0x800` (RVAs 0x5B4440/0x5B4150)
All three are engine-internally critical-section guarded and are exactly what runs after the guard — calling them directly bypasses only the refusal, not the teardown. `CoSyncGameAPI::DisableWorldActor`/`EnableProxyActor` now call these natively (SEH-wrapped, game thread), replacing the Papyrus `CallFunctionNoWait` route; DisableWorldActor also skips already-disabled/deleted refs like the console handler does. Log lines changed to `engine disable OK refID=...` / `engine enable OK refID=...`.
On the "SO many more PlaceAtMe IDs" concern: no more IDs are needed — the `[0.0.27]` 2s-persistence settle filter is the general fix (this test's logs show zero WBF broadcasts at the same camp that spammed dozens in build 151); the static ID list is only belt-and-braces for the three already-seen bases.
Known caveat (pre-existing, now more reachable): an engine-disabled original persists into the save if the session ends without the unbind re-enable running (crash/quit while bound). The unbind path re-enables on entity destroy; a future session-teardown sweep could re-enable all of `s_disabledOriginalRefs` on disconnect.
Build 153 deployed. **Next test: originals should visibly vanish the moment clones bind (log `engine disable OK`), leaving exactly one copy of each NPC on both machines; walking the host away should re-enable the client's originals (`engine enable OK`).**

---

## [0.0.27] — 2026-07-20

### Build-151 test verified the vtable fix; killed the remaining WBF combat-FX crash spam + NPCSEEN chatter (build 152)
Test 22:48 (host `_33460` / client `_22040`): **the [0.0.26] ITaskDelegate vtable fix is confirmed** — zero `Known bad-function-pointer` VEH recoveries, zero SpawnWatchdog re-enqueues, all 5 NPC clones spawned on the first attempt. The multi-session task-drop saga is closed. Two remaining issues visible in the same logs, both fixed here:
- **WBF combat-FX false positives = the recurring 0x165910F crash at NPC camps.** Both machines TX'd `WBF` packets every ~300ms for bases `0x000CA064`/`0x000CA066`/`0x000EB2B4` at constantly-shifting positions — nobody was building; these are transient combat-FX dynamic refs (muzzle-flash-light/explosion-class bases share the buildable form types the scan accepts, LIGH etc.), spawned continuously while camp NPCs fight. Each receive side then attempted `PlaceAtMe_Native` on them → the familiar WRITE AV at `Fallout4.exe+0x165910F` (3 caught per machine before the per-base session blacklist kicked in). Fix: new `WbfSettleElapsed` gate in BOTH build scans (`HostTickObjects_Impl` + `ClientTickBuildScan_Impl`) — a dynamic ref must still exist ~2s after first sighting before it can be broadcast (player builds persist; FX refs die in well under a second), with self-pruning of the first-seen map. The three observed bases are also added to both static crash-block lists. This should finally quiet the 0x165910F class during NPC combat — which is exactly when it always flared.
- **NPCSEEN report loop for disabled originals.** The client kept re-reporting its own Disable()d original NPCs (5 reports/sec) and the host kept resending the matching CREATEs (correctly deduped — no double entities, proving the Stage 2 claim map works — but pure wasted traffic). The `kFlag_IsDisabled` scan check never caught them because the Papyrus `Disable()` sets the engine flag asynchronously (or not at all for these). Fix: explicit `s_disabledOriginalRefs` bookkeeping — populated at both Disable sites (`SpawnProxyForWorldActor`, clone-fallback), skipped in the client report scan, erased on unbind re-enable, cleared on Reset.
Build 152 deployed. **Expected next NPC-camp test: zero `TX WBF` lines unless someone actually builds, zero 0x165910F VEH crashes, NPCSEEN reports stop within ~1s of binding.**

---

## [0.0.26] — 2026-07-20

### THE task-drop root cause, finally: `Core/Tasks.h` ITaskDelegate vtable slots were swapped vs. F4SE's ABI (build 151)
Build-150 test (host `_28396` / client `_33152`): the [0.0.25] fixes verified — `isPlayerProxy=0` for NPC clones, no VM calls queued at spawn time, and the new SpawnWatchdog recovered both dropped tasks (all 5 camp NPCs present within ~6s, EU position sync flowing at 20Hz with derived speeds). But EXEC AVs still fired between spawn tasks with *nothing* Papyrus-related left in the path — so the "VM-not-ready Papyrus call" theory was killed, and the real mechanism was hunted down by disassembling `f4se_1_11_221.dll` itself (fresh headless IDA analysis; the fake-ret site f4se+0x161F6 sits in the task pump at f4se+0x16130, which per iteration does `call [vtbl+8]` intending Run, then re-reads the object's vtable pointer and does `call [vtbl+0]`(this, 1) intending the deleting destructor).
**The bug: our vendored `Core/Tasks.h` declared `virtual void Run() = 0;` BEFORE the virtual destructor.** MSVC lays vtable slots in declaration order, so our tasks had slot 0 = Run, slot 1 = deleting dtor — exactly backwards from what the running F4SE calls. Every `SpawnRemotePlayerTask` ever queued had its *deleting destructor* invoked as "Run" (with a garbage flags register — when bit 0 happened to be clear, nothing freed and the subsequent slot-0 call ran the real Run, which is why spawning ever worked at all) and its *Run* invoked through the destructor slot. When the garbage flag bit was 1, the object freed itself before running; the pump then re-read the vtable pointer from freed, reallocator-scribbled memory → EXEC AV at a heap-garbage RIP → the VEH fake-ret made F4SE skip the task — **the entire "F4SE silently drops the next queued task" failure class from 2026-07-17 onward was this one header, not F4SE behavior**. `CoSyncRuntime`'s main tick task was never affected because it derives from `ThirdParty/Pap/GameThreads.h`'s ITaskDelegate, which has the correct destructor-first order (as does `Core/GameTasks.h`) — `Core/Tasks.h` was the only wrong one, and `CoSyncSpawnTasks.cpp` the only user.
Fix: destructor declared first in `Core/Tasks.h`, with a comment marking the slot order as an F4SE ABI contract. The [0.0.25] watchdog stays as a safety net but should now never fire for this reason. Also confirmed in the same logs: the 22:33:55 minidump was the separate, known WBF `PlaceAtMe_Native` fault (bases 0xCA064/0xCA066, RVA 0x165910F, SEH-caught + permanently blacklisted) — pre-existing, unrelated, still open.
Build 151 deployed. **Expected next test: zero `Known bad-function-pointer` VEH lines around spawn batches, all NPC clones on the first attempt, zero `SpawnWatchdog` re-enqueues.**

---

## [0.0.25] — 2026-07-20

### Root-caused the [0.0.24] test failure: NPC clones silently missing on the client (build 150)
Test session 22:16 (host `CoSyncUpdated_22056` / client `_37564`, together-case, camp with 5 NPCs): host registered and broadcast all 5, client bound all 5 and disabled its originals — but only 3 clones ever spawned (65536/65538/65540). **Entities 65537 and 65539 were silently swallowed, so the client was missing 2 of 5 NPCs — the "visually not in sync" report.** The log captured the exact mechanism twice: `Run COMPLETE entity=65536` → EXEC AV at a garbage heap pointer, VEH fake-return to the F4SE dispatch site (`0x...761F6`) → next queued spawn task consumed. Same class as the 2026-07-17 host-proxy drop.
Two real bugs underneath, both fixed:
1. **`CoSyncPlayer.createType` was never set for client-side world-NPC clones** — `ProcessEntityCreate` (which stamps it) is bypassed for world actors, and neither `SpawnProxyForWorldActor` nor the spawn task wrote the member, so it stayed at its default `Player`. `SpawnInWorld` passes `createType == Player` as `isPlayerProxy` into `SpawnRemoteActor`, so every NPC clone took the player-proxy path: real NPC base marked ESSENTIAL (against round-138 intent), joined PlayerFaction, and — the killer — queued the spawn-time `SetFactionRank` Papyrus call on a VM-not-ready actor. That queued call is what EXEC-AV'd ~7ms later, eating the next spawn task. This is also the long-unexplained "createType reads back as Player at the deferred tick" anomaly from the 2026-07-18 sessions, now root-caused. Fix: `SpawnRemotePlayerTask::Run` stamps `pl.createType = m_createType` before `SpawnInWorld` (`Character\CoSyncSpawnTasks.cpp`).
2. **`SetFactionRank` was still queued at spawn time at all** — the same VM-not-ready hazard the 07-17 fix deferred for `SetEssential`/`SetProtected`. Moved out of `SetupPlayerProxyProtections` into a new `CoSyncGameAPI::QueueProxyFactionRank` (IsFormStillLive-guarded), fired from the deferred post-spawn tick via a new `m_pendingFactionDelay = 120` counter (player proxies; same proven timing as `m_pendingNpcInitDelay`).
Defense-in-depth: the world-NPC clone spawn path is now covered by the existing `SpawnWatchdog` (`SpawnProxyForWorldActor` and the clone-fallback set `spawnQueued`/`lastSpawnAttemptTime`), so any future dropped/failed spawn task re-enqueues after 3s instead of leaving a permanently missing NPC. The freed-actor cell-transition clear also resets `spawnQueued` so the watchdog can't respawn-loop a clone the engine unloaded.
Also observed in the same logs, NOT addressed here: 3 SEH-caught `PlaceAtMe_Native` faults (client, `CoSyncWorldSync` WBF path, known-bad workshop bases 0xCA064/0xCA066, RVA 0x165910F) — pre-existing, blacklisted after first fault, non-fatal. The `[0.0.23]` BSFixedString sweep held: zero uncaught crashes, no crash loop this session.
Build 150 deployed to both-machine staging (copy the DLL to machine 2). **Not yet re-tested.**

---

## [0.0.24] — 2026-07-20

### Peer-Aware NPC Discovery v3, Stage 2 — client-reported NPCs are now real synced entities (build 149)
Builds directly on the `[0.0.19]` Stage 1 diagnostic pipe, which the `[0.0.20]` test session confirmed working end-to-end (131 reports received, dedup verdicts correct, one actor caught that the host structurally couldn't see). Stage 2 makes the pipe real, using the ownership plumbing (`isLocallyOwned`) that already existed but was only half-wired:
- **Host** (`OnReceiveNpcSeen`, `World\CoSyncNpcWorldSync.cpp`): a new real-actor report now allocates an entity ID, records the claim (`s_claimedRefToEID`, upgraded from a set to a ref→entity map), and broadcasts the normal `EntityCreate` with `ownerEntityID` set to the reporting client. Reports for already-claimed refs resend that entity's CREATE (throttled to one per 2s per ref) — self-healing for lost packets, replacing Stage 1's log-only verdict. The host also binds its own puppet for the new entity through the exact same disable-original+clone path clients have always used for host-discovered NPCs (`ClientBindWorldActor`), with the owner-client's 5s CREATE re-broadcast serving as the retry driver — same proven retry pattern as the existing host→client direction, just mirrored. Dynamic (0xFF) actors are deliberately NOT reported/claimed (session-relative refs can't be deduped across machines — the doubled-enemies trap that sank the `[0.0.16]`/`[0.0.17]` attempts); they remain host-authority-only for now.
- **Owning client** (`ClientBindWorldActor_Impl`): recognizes its own confirmation (`ownerEntityID == my entity ID`), binds its real local actor directly — no disable, no clone, the engine keeps simulating it natively — and becomes the entity's update source: the (now misnamed) `HostSendNpcUpdates_Impl` position/anim/HP send loops and the `TickScanWorldActors` HP/legendary/CREATE-rebroadcast/departure sections all now run for locally-owned entities on either role, gated per-entity on `isLocallyOwned` for world actors (companions keep the old host-only gating, untouched).
- **Ownership semantics cleaned up** across `Character\CoSyncPlayerManager.cpp`: `hostAuthoritative` now consistently means "the host peer owns this entity" (false for client-owned; `SpawnProxyForWorldActor` derives it from `ownerEntityID` instead of hardcoding true), and `isLocallyOwned` means "this machine drives it". `ProcessEntityUpdate` drops relayed echoes of updates for entities this machine owns, and the host now applies (instead of dropping) updates for client-owned world NPCs onto its puppet. `HandleTimeouts` never times out an entity this machine owns (owned entities receive no network updates by design).
- **Fixed a latent `[0.0.21]` regression before it ever shipped to a test**: the "host's own real NPCs skip TickAnimVars" gate keyed on `hostAuthoritative` — which `SpawnProxyForWorldActor` set to true on the CLIENT too, so the untested `[0.0.23]` build would have frozen every client-side NPC puppet (no position/anim applies at all). The gate now keys on `isLocallyOwned`, which is only true on the machine that actually drives the actor.
- **Damage/health routing generalized to ownership** (`Character\CoSyncHealthSync.cpp`): DMG application and the damage-detection reporter now key on "do I own this entity" instead of "am I host", so the host's hits on a client-owned puppet are reported to the owner and applied to the real actor there, exactly mirroring the existing client→host direction.
- **NPC no longer vanishes from a puppet-holder's world after the owner departs**: `ClientUnbindWorldActor` now re-enables the disabled original (via `CoSyncGameAPI::EnableProxyActor`) when despawning a fallback clone — previously the original stayed disabled forever unless the same entity got re-created. Claims are also released on both departure-destroy and unbind (`ReleaseClaimForEntity`), fixing stale-claim zombies that would have resent CREATEs for dead entities.
- Misc hardening: `AllocateEntityID` also skips IDs that already have a `CoSyncEntityState` (client-owned entities aren't in `s_eidToActor`, so the old check alone could recycle a live ID); the world-actor scan skips engine-disabled refs on both roles (a client's own disabled originals were being NPCSEEN-reported back to the host in a pointless loop); `NPCSEEN` wire format gains the reporter's entity ID (`NPCSEEN|base|ref|pos|reporterEid`) — build number bumped to 149 so the VER handshake flags any mixed-build session; world-actor CREATE/DESTROY messages received by the host are now relayed to other clients (was a silent 3+-player gap) and world-actor DESTROYs are properly unbound on the host instead of falling into the player-entity path.
**Not yet tested in-game.** Test focus: (1) together-case regression — host+client in one camp must behave no worse than `[0.0.13]`; (2) split-case — client alone near story NPCs must produce host-log `STAGE2 allocated entity=...` + client-log `OWNER-BIND entity=...`, NPCs visibly animating for the client (its own real actors, now also sending `EU` updates), and puppets appearing for the host on approach; (3) client kills its NPC → host's puppet dies. Note the `[0.0.23]` BSFixedString crash-loop sweep ALSO rides into this test untested — if sessions still crash-loop in NPC-heavy areas, check the crash RIP against `[0.0.22]`'s signature first before blaming Stage 2.

---

## [0.0.23] — 2026-07-18

### Full sweep: cached every literal BSFixedString construction codebase-wide
Tested `[0.0.22]`'s targeted fix (5 call sites in `CoSyncWorldSync.cpp`) — the crash recurred with the identical signature (`Fallout4.exe+0x165910F`, same fault address `0x248BDF4`), proving the fix was necessary but not sufficient: the same hazard exists at other call sites reachable from the same broad tick chain. Given the fix is a mechanical, additive transformation (caching, not a logic change) rather than the kind of architectural rewrite that failed twice earlier tonight, swept the entire codebase instead of chasing one file at a time.
Found and fixed 46 literal `BSFixedString("...")` constructions passed to `CallFunctionNoWait` across 16 files (`Armor\CoSyncPowerArmorSync.cpp`, `Character\CoSyncCompanionSync.cpp`/`CoSyncHealthSync.cpp`/`CoSyncPlayer.cpp`/`CoSyncPlayerManager.cpp`/`CoSyncPlayerMarker.cpp`, `Clothing\CoSyncEquipHook.cpp`/`CoSyncOutfitSync.cpp`, `CoSync\CoSyncGameAPI.cpp`/`CoSyncPapyrusHelper.cpp`, `Items\CoSyncConsumableSync.cpp`/`CoSyncContainerSync.cpp`/`CoSyncInventorySync.cpp`/`CoSyncLootSync.cpp`, `World\CoSyncQuestSync.cpp`) — every one now a function-local or file-scoped `static const BSFixedString`, constructed (and risked) once per process instead of every call. Left two categories alone: genuinely dynamic strings built from a runtime `const char*` (`CoSyncPapyrusHelper::SendProxyAnimEvent`, value varies per call, can't be cached) and two `BSFixedString("")` empty-string stub return values in `Papyrus_CoSync.cpp` (not the risky pattern — no `CallFunctionNoWait` involved).
Two real compiler issues surfaced and were fixed along the way, both informative:
- `World\CoSyncQuestSync.cpp`: a `static const BSFixedString` inside a function that also used `__try` hit C2712 ("cannot use __try in functions that require object unwinding") — the same compiler restriction this codebase has hit before for non-trivial-dtor objects. Fixed with the same established pattern already used elsewhere: split into a small non-`__try` helper holding the static + call, wrapped by a `__try`-only outer function with no local objects.
- `CoSync\CoSyncGameAPI.cpp`: one site (`InitNpcProxy`'s `SendAnimationEvent` value argument) passes the `BSFixedString` by pointer into `VMVariable::Set()`, which needed a non-`const` instance — `Set()`'s template instantiates differently for `const BSFixedString` and hit an unresolved linker symbol (`PackValue<StringCache::Ref const>`) when tried. Fixed by leaving that one variable non-const while everything else (the function-name arguments passed directly to `CallFunctionNoWait`) stayed `const`.
**Not yet re-tested** — go back to the same NPC-heavy area from the last two crash sessions and confirm it holds this time.

---

## [0.0.22] — 2026-07-18

### Root-caused and mitigated the recurring `CoSyncWorldSync::Tick` crash loop
Investigated the crash first flagged (unfixed) in `[0.0.13]`, which recurred repeatedly tonight and, in a test session near several NPCs, hit the host 3 times in 1.3 seconds and ended the session. Resolved `Fallout4.exe+0x165910F` (the crashing RIP) against the canonical disk-exe IDA database (`Fallout4_disk.i64`): the instruction is `lock cmpxchg [rdi+4], ecx` inside a `GetCurrentThreadId`-guarded critical-section acquire — a lock-enter routine. All three fault addresses observed across tonight's crashes resolve into `.rdata` (read-only data), meaning the lock object pointer passed in is garbage — consistent with `BSFixedString` constructed fresh from a string literal on every call, a known-but-previously-unfixed hazard already noted in project history (the interning path's lock can be entered with a bad pointer under contention).
`World\CoSyncWorldSync.cpp` (the file this crash's `lastTickStep` pointed at) constructs `BSFixedString("Disable")`/`BSFixedString("Enable")` fresh, from literals, at 5 call sites — including one (`TickProxyCellVisibility_Impl`) that runs every tick over every tracked proxy, exactly the kind of hot, frequently-re-entered path that would repeatedly hit this race. Fixed all 5 by caching each as a function-local `static const BSFixedString`, constructed (and interned) once per process instead of on every call — the standard mitigation for this exact class of hazard.
**Scope note**: this same `BSFixedString("literal")` pattern exists in dozens of other places across the codebase and was NOT touched in this pass — only the file directly implicated by tonight's actual crash evidence. If the crash recurs from a different call site, the same caching fix applies there too.
**Not yet re-tested.**

---

## [0.0.21] — 2026-07-18

### Fixed: host's own real NPCs had their native animation overwritten every tick
Dug deeper into the `[20:38-20:42]` test session logs after an initial too-shallow pass. Found the host log spamming `[TickAnimVars] entity=65536 FALLBACK (no recv snapshot)` continuously for an NPC the **host itself** discovered and owns (`base=0x001069FC`, a real world actor, not a clone). Root cause: `CoSyncPlayerManager::Tick()`'s main entity loop (`Character\CoSyncPlayerManager.cpp:1370`) calls `ent.TickAnimVars()` unconditionally for every tracked entity — but that function exists to apply *received* network animation data (or, failing that, a dead-reckoned fallback built from `m_lastVel`) onto a remote puppet. Host's own directly-discovered real NPCs are never recipients of anything — `m_lastVel` never gets populated for them (only `ApplyUpdate`, called on receiving a packet, ever sets it), so the fallback path forces an effectively-frozen idle snapshot onto the actor's real animation graph every single tick, stomping whatever the actual AI/engine was doing natively.
Fixed: entities the host is directly authoritative for (checked via `CoSyncEntityState::hostAuthoritative`, restricted to world-actor-range entity IDs) now skip both `ApplyPendingTransformIfAny()` and `TickAnimVars()` in the main tick loop — nothing should ever be "applying remote data" onto an actor the host is natively simulating. This bug is independent of and predates tonight's Stage 1 diagnostic work; it's a standing architecture gap in how the tick loop treats host-owned vs. remote-controlled entities, and is very likely the real explanation for repeated "NPCs are in A-pose" reports tonight, more directly than the `RefreshActor3D` gate fixed in `[0.0.20]` (which only affects client-side cloned proxies, a different code path).
**Not yet re-tested.**

---

## [0.0.20] — 2026-07-18

### Fixed regression: A-pose gate reintroduced by tonight's reverts
Tested `[0.0.19]`: the diagnostic pipe worked (see below), but the user also reported NPCs A-posing again. Root cause: the `[0.0.13]` "A-pose/glide fixed" entry's actual working fix was the *unconditional* deferred `RefreshActor3D` call — but the `createType == CoSyncEntityType::NPC` gate around it (which never worked; `ent.createType` reads back as `Player` at that point for reasons never conclusively root-caused earlier tonight) was only removed in `[0.0.14]`. Every subsequent revert tonight (`[0.0.16]`, `[0.0.18]`) targeted "back to `[0.0.13]`" as the baseline — which still has the broken gate, since the working unconditional version only ever existed inside `[0.0.14]`, which got reverted along with the unrelated ownership/ID changes that actually caused problems. So the A-pose fix got silently undone three revert-cycles ago and nobody re-noticed until this test. Removed the gate again (`Character\CoSyncPlayerManager.cpp`) — same one-line fix as `[0.0.14]`, unrelated to and independent of the Stage 1 diagnostic work in `[0.0.19]`.

### Stage 1 diagnostic pipe confirmed working
Test session logs (`CoSyncUpdated_17180.log` host / `_5300.log` client) confirm the `[0.0.19]` reporting pipe works end-to-end: 131 `NPCSEEN` reports received and correctly classified, including one real case (`base=0x000EFBB4`) that the host never discovered on its own and correctly logged `WOULD_ALLOCATE` — proof the client saw something the host structurally couldn't, and the pipe successfully carried that information across. Also confirmed the dedup logic is live/correct: `base=0x0003D638` logged `WOULD_ALLOCATE` for 3 ticks, then flipped to `already claimed, WOULD_IGNORE` the moment the host's own scan independently discovered the same actor a second later. Stage 2 (make it real) is next.

---

## [0.0.19] — 2026-07-18

### Peer-Aware NPC Discovery v3, Stage 1 — diagnostic-only pipe (no gameplay change)
Third attempt at "P1 far from P2 doesn't sync NPCs," after two full implementations (`[0.0.16]`, `[0.0.17]`) both failed in testing without concrete log evidence of why. This time staged deliberately: land the reporting pipe in a log-only mode first, verify it from logs, *then* wire up the real spawn/sync behavior in a follow-up. Nothing in this entry changes what either player sees in-game.
- `TickScanWorldActors` (`World\CoSyncNpcWorldSync.cpp`) runs on both roles again. Host's own discovery is completely unchanged. A non-host peer's discovery no longer self-registers — for each actor it finds that isn't already tracked, it sends a stateless `NPCSEEN|baseFormID|refFormID|pos` report every scan tick, unconditionally, with zero local bookkeeping (no pending-claim map, unlike `[0.0.17]` — if a message drops, the next tick just re-sends it).
- New host-side `OnReceiveNpcSeen`: parses the report and logs whether it *would* allocate a new entity for it or ignore it as already-claimed (checked against the same `s_claimedRealActorFormIDs` set host's own discovery now also populates) — but doesn't actually allocate an ID, create any state, or broadcast anything. Purely observable via the new `[NpcWorldSync] DIAG NPCSEEN ... -> WOULD_ALLOCATE` / `-> already claimed, WOULD_IGNORE` log line.
- **Next test session**: split up, check host's log for these DIAG lines corresponding to what the client is actually near. If they show up correctly, Stage 2 (make it real: actual allocation + `EntityCreate` broadcast + client-side confirmation recognition) gets planned and built next. If they don't, we'll know the reporting pipe itself is the problem before writing any more spawn logic.

---

## [0.0.18] — 2026-07-18

### Reverted [0.0.17]
The host-arbitrated NPCCLAIM discovery rework still wasn't syncing in testing ("big problem" per user report, no further detail given). Reverted all four touched files (`Character\CoSyncPlayerManager.cpp`, `Networking\CoSyncNet.cpp`, `World\CoSyncNpcWorldSync.h`/`.cpp`) back to `[0.0.13]` — discovery is host-only again, no `NPCCLAIM` protocol, no grid-cell primitive, `hostAuthoritative`/`HostSendNpcUpdates_Impl` gated on `CoSyncNet::IsHost()` again. Build verified clean, deployed. This is the second peer-aware-discovery attempt reverted tonight (see `[0.0.16]` for the first). The "P1 far from P2 doesn't sync" problem is unsolved and untouched — back to exactly the `[0.0.13]` baseline (A-pose/glide fix for cloned NPCs still intact).

---

## [0.0.17] — 2026-07-18

### Peer-Aware NPC Discovery v2 — host-arbitrated claim model (informed by Fallout Together)
Researched how Fallout Together (TiltedEvolution, a mature shipped multiplayer mod for this engine family) solved the identical "player far from host can't sync NPCs" problem: client-driven discovery with single-authority server arbitration, using grid-cell proximity (bucketed by the engine's real exterior cell size, 4096 units) instead of exact-cell matching. Adapted to CoSync's host/client (no dedicated server) architecture — host keeps its existing role as sole entity-ID allocator/arbiter, but a client can now become the *owner* (data-source) for an NPC it discovers near itself, instead of only ever being a passive recipient of host-discovered NPCs. This is a from-scratch redesign of the `[0.0.14]`/`[0.0.15]` attempt reverted earlier tonight (that version made both peers fully symmetric with no arbitration, which caused new problems) — this version keeps a single arbiter to avoid the race conditions the first attempt had.
- New grid-cell proximity primitive (`World\CoSyncNpcWorldSync.cpp`): buckets position into 4096-unit cells (FO4's real exterior cell size) and checks a ±2 cell window; used for dynamic-NPC-duplicate suppression, replacing both of tonight's earlier failed relevance checks (exact-cell-match was too strict outdoors; raw Euclidean distance worked but doesn't match the engine's own cell semantics). Interior cells still use exact `TESObjectCELL` formID match.
- `TickScanWorldActors` now runs on both host and client. Host's own discovery path is unchanged from `[0.0.13]`. A client's discovery now sends a new `NPCCLAIM` message (baseFormID/refFormID/position) instead of self-registering; the host — still the sole ID allocator — accepts or silently rejects (real/non-dynamic actors de-duplicated by formID, dynamic actors always accepted) and, on acceptance, broadcasts the normal `EntityCreate` with `ownerEntityID` set to the requesting client.
- The requesting client recognizes host's confirmation (`ownerEntityID == its own entity ID`) in `ClientBindWorldActor_Impl` and hooks up the actor it already has locally — no clone, no disable-then-spawn dance, since it already possesses the real actor from its own scan.
- Relinquish/reassignment needed no new protocol: the existing keep-radius departure/destroy loop already only evaluates entities the running peer itself owns (`s_actorToEID` was already "things I discovered," host or client), so running it on both roles makes owner-leaves-range self-destroy work for client-owned entities for free. Host cleans up its claimed-formID bookkeeping (`HostReleaseClaim`) whenever any world-actor entity is destroyed, by itself or by a client, so a released real actor can be re-claimed later.
- `Character\CoSyncPlayerManager.cpp`: `HostSendNpcUpdates_Impl` no longer requires `IsHost()`; `state.hostAuthoritative` (gating both the position/anim send loop and the HP-broadcast loop) now derives from `isLocallyOwned` instead of `IsHost()` — same one-line generalization as the reverted attempt, still correct here.
- **Known limitation, not solved this pass**: the host itself doesn't yet visually bind/see a client-owned NPC if the host later walks up to it (host never runs the bind-or-clone path for entities it doesn't own) — only the originally-owning client sees it. Deferred to keep this pass's scope bounded after last attempt's overreach. Also: a client whose claim is silently rejected retries every 5s indefinitely (minor network chatter, not a correctness issue).
- **Not yet tested in-game.**

---

## [0.0.16] — 2026-07-18

### Reverted [0.0.14] and [0.0.15]
Both the duplicate-suppression distance-check change and the peer-aware NPC discovery rework (host+client both scanning, split entity ID ranges, ownership-based authority) ran into new issues in testing. Reverted all four touched files (`Character\CoSyncPlayerManager.cpp`, `Networking\CoSyncNet.cpp`, `World\CoSyncNpcWorldSync.h`/`.cpp`) back to their `[0.0.13]` state — NPC discovery is host-only again, `TickSuppressLocalDynamicSpawns` is client-only again with the cell-formID proximity check, entity IDs are a single unpartitioned range, and `hostAuthoritative`/`HostSendNpcUpdates_Impl` are gated on `CoSyncNet::IsHost()` again. `[0.0.13]`'s A-pose/glide fix (deferred `RefreshActor3D` for NPC-type clones) is intact. Build verified clean. Trying a different approach for the "enemy near a distant player doesn't sync" problem next, rather than the full peer-authority rework.

---

## [0.0.15] — 2026-07-18

### Peer-Aware NPC Discovery (enemies near a distant player now sync)
- Root cause of "enemy attacked P2, P1 saw nothing": NPC discovery (`TickScanWorldActors`) only ever ran on the host, scanning only the host's own current cell. If the two players split up, anything only the non-host player was near was never discovered or synced at all — not a bug in tonight's earlier fixes, the architecture has always worked this way. Confirmed via log: host discovered exactly 3 NPCs the entire session, zero combat ever detected, while the client was fighting something the host never saw.
- Changed from host-only authority to per-entity ownership, reusing `EntityCreatePacket::ownerEntityID` / `CoSyncEntityState::isLocallyOwned` (already built for companion ownership, just never wired up for world NPCs):
  - `World\CoSyncNpcWorldSync.cpp`: `TickScanWorldActors` and `TickSuppressLocalDynamicSpawns` now run on **both** host and client (`Networking\CoSyncNet.cpp` gates dropped), each scanning their own current cell. Newly-discovered NPCs are tagged with `ownerEntityID = CoSyncNet::GetMyEntityID()` instead of hardcoded 0.
  - World-actor entity IDs are now split into two static halves (host / non-host) so both peers can allocate independently without colliding.
  - New `s_claimedRealActorFormIDs` set prevents both peers from independently claiming the same real (Creation-Kit-placed) actor if they're both nearby it — checked before claiming in the scan, populated on both the claiming and receiving side.
  - `Networking\CoSyncNet.cpp`: the receive-side bind/clone-fallback dispatch (`ClientBindWorldActor`) no longer skips for the host role — a host receiving a client-discovered NPC's CREATE now runs the same disable-local-copy-then-clone logic every peer already used, instead of falling through to a path that skipped the disable step.
  - `Character\CoSyncPlayerManager.cpp`: `HostSendNpcUpdates_Impl` no longer requires `IsHost()` — both peers now send position/animation/health/combat-target updates for whatever they personally own. `state.hostAuthoritative` (which already gated both send loops) now derives from `isLocallyOwned` instead of `IsHost()`.
- **Not yet tested** — needs a session where the two players deliberately split up so one gets attacked away from the other, confirming the other side now sees it.

---

## [0.0.14] — 2026-07-18

### Dynamic NPC duplicates and A-pose — real root causes found via log trace
Tested `[0.0.13]` in-game: still duplicating, still not animating. Traced the actual code path (not just logs) this time and found both were architectural, not the narrow bugs assumed last pass.

- **Duplicates**: `SpawnProxyForWorldActor` (`World\CoSyncNpcWorldSync.cpp`) — the existing, already-proven handler used for the primary bind path — does two things: disables the client's own local copy of the NPC, *then* spawns a synced clone. That disable step is what has prevented duplicates for Codsworth/Dogmeat/every regular NPC all along. Dynamic NPCs deliberately get `reserved0=0` (from `[0.0.12]`, since their ref ID is meaningless across sessions) which skips the primary bind path entirely — so they never hit `SpawnProxyForWorldActor`'s disable step. The `[0.0.12]` suppression scan was meant to cover this gap but required the client to be in the *exact same* cell formID as the host — outdoors, players are very often one cell over from each other even while together, so it matched effectively never (confirmed: zero suppressions logged all session). Changed the gate to a straight distance check (4096 units, ~1 cell) from the host's actual tracked position instead of an exact cell-formID match.
- **Animation**: the `[0.0.13]` fix (deferred `RefreshActor3D` call) never actually fired — `InitNpcProxy` logged correctly for every NPC but the following `RefreshActor3D` branch, gated on `createType == NPC`, silently never took. Rather than keep guessing at why that specific comparison wasn't matching, removed the redundant check entirely (the surrounding delay is already NPC-only by construction — `m_pendingNpcInitDelay` is only ever set for NPC-type spawns in `CoSyncSpawnTasks.cpp`) and added the actual `createType` value to the log line as a fallback diagnostic in case it still doesn't fire.
- **Not yet re-tested.**

---

## [0.0.13] — 2026-07-18

### Cloned World NPCs A-Pose / Glide Fixed
- Tested `[0.0.12]`'s dynamic-NPC sync in-game: newly-synced NPCs were appearing and moving (position sync working) but stuck in A-pose with no animation ("gliding"). Log confirmed this wasn't a data problem — the client's `AnimVarSystem` was correctly loading the full 299-variable humanoid graph onto these actors (`graphDriven=1`, correct locomotion state) — the animation graph itself was never activated.
- Root cause: every clone-fallback world NPC goes through `CoSyncPlayer::SpawnInWorld` -> `CoSyncGameAPI::SpawnRemoteActor`, i.e. a raw `CreateReferenceAtLocation`, same as player proxies — which come up as "invisible skeletons" until a `Disable()`+`Enable()` cycle (`CoSyncGameAPI::RefreshActor3D`) forces the engine to rebuild the 3D/animation graph. That refresh was already correctly gated off *synchronous* firing for NPC-type spawns in `SpawnRemotePlayerTask::Run` (calling it immediately after creation risks the same Papyrus-VM-not-ready EXEC AV that caused the 2026-07-17 host-proxy-drop bug) — but nothing ever fired it *later* either, so cloned NPCs never got it at all. Log confirmed `RefreshActor3D` fired only for the player-type entity all session; zero calls for any of the 7 cloned world NPCs.
- Fix: `CoSyncPlayerManager.cpp`'s existing deferred-init tick (the same 120-tick-after-spawn point that already fires `InitNpcProxy` once the Papyrus VM object is confirmed ready, from the 07-17 fix) now also calls `CoSyncGameAPI::RefreshActor3D` for NPC-type entities at that same trigger. Reuses the already-proven "wait for VM readiness" timing rather than a new guess.
- **Not yet re-tested** — needs another dynamic-encounter session to confirm animation now plays.

### Known issue found, not yet fixed: intermittent WRITE AV in `CoSyncWorldSync::Tick`
- Same test session logged a caught-and-recovered VEH crash (`CoSync_crash.dmp`, 18:23:50) on **both** host and client simultaneously — same RIP (`Fallout4.exe+0x165910F`), same fault address, same `lastTickStep=CoSyncWorldSync::Tick`. Both processes survived (VEH-caught) and the session continued normally afterward. This is in a different subsystem (`World\CoSyncWorldSync.cpp` — doors/containers/scenes/pickups) than anything touched this session; likely a pre-existing latent bug that this session's much higher NPC count made more likely to trigger. Not investigated yet.

---

## [0.0.12] — 2026-07-18

### Sync Dynamically-Spawned NPCs (random encounters, wildlife, ambushes)
- `TickScanWorldActors_Impl` (`World\CoSyncNpcWorldSync.cpp`) previously excluded any actor whose own reference formID was `0xFF------` — i.e. anything spawned at runtime via a leveled list / random encounter rather than placed in the Creation Kit. That covered most ambient wildlife and a large share of raider/ambush encounters, and was the actual reason those never synced at all (not a quality issue — they were never sent). Removed that exclusion; these actors are now discovered and broadcast the same as any other world NPC. The base-form dynamic check (a `TESNPC` synthesized entirely at runtime with no stable ID) is unchanged and still excluded — out of scope.
- Since a dynamic actor's own reference formID is session-relative (meaningless on another client, and could coincidentally collide with an unrelated local actor), `EntityCreatePacket::reserved0` is now `0` for these instead of the real ref formID, forcing every client straight to the existing clone-fallback spawn path instead of attempting (and potentially mis-binding via) `LookupFormByID`.
- New client-side-only `TickSuppressLocalDynamicSpawns` (`World\CoSyncNpcWorldSync.cpp`, wired into the main tick in `Networking\CoSyncNet.cpp`): the risk with syncing dynamic spawns is the client's own game independently rolling the *same* encounter locally, producing doubled enemies (confirmed happening in prior play sessions). This function scans the client's current cell for dynamic-ref NPCs that aren't already CoSync-tracked, aren't a teammate or already dead, aren't flagged essential/protected/unique (`TESActorBaseData` flags — the safety gate against touching quest-critical actors, same flag-read pattern already used in `CoSyncGameAPI::SetupPlayerProxyProtections`), and are in a cell the host is also known to be in (via the already-tracked `lastRemoteCellFormID` on the host's own proxy) — and despawns them via the existing `CoSyncGameAPI::DespawnProxyActor` (Disable+Delete, already proven for proxy cleanup on disconnect). A 2-tick grace window avoids touching an actor mid-engine-spawn.
- **Not yet tested in-game** — needs a session where host+client travel together into a random encounter; verify via `[NpcWorldSync] Registered actor ... base=0x...` (host, now firing for dynamic spawns) and `[NpcWorldSync] Suppressed local dynamic-spawn duplicate` (client) log lines, plus a visual check for no duplicate enemies and no unexpected quest-actor disappearance.

---

## [0.0.11] — 2026-07-18

### Enemy NPC Sync (animations, combat target, appearance)
- **Animation root cause found and fixed**: `HostSendNpcUpdates_Impl` (`Character\CoSyncPlayerManager.cpp`) was calling `CoSyncCreatureAnimSync::SaveCreatureVars` directly for every hostile NPC/companion, which never populates `actorStateFlags1`/`actorStateFlags2` — the exact fields `CoSyncPlayer::TickAnimVars` reads to fire `attackStart`/`attackStartMelee`/`attackBashStart`/`attackStop`/weapon-draw events. This meant no enemy NPC, humanoid or creature, could ever trigger an attack animation client-side. Fixed by calling `CoSyncAnimVarSystem::SaveAnimationVariables` instead — the same function already proven for the local player at 20Hz, which internally dispatches to the existing creature path for small graphs and fills the full humanoid snapshot (incl. `actorStateFlags1/2`) otherwise. **Not yet exercised in-game** — the 2026-07-18 17:48–17:53 test session (`CoSyncUpdated_5584.log`/`_5068.log`) only involved player-vs-player combat, no hostile NPC was encountered, so this fix has not yet been confirmed live.
- **Combat target wired (approximation)**: `EntityUpdatePacket::combatTargetEntityID` existed on the wire but had no producer or consumer. Host now resolves the nearest tracked Player-type entity (by distance) to each in-combat NPC and sends it; client logs target transitions (`[PlayerMgr] CombatTarget entity=... -> target=...`). This is a distance heuristic, not the AI's real selected target — true fidelity would need the (currently unmapped) `AIProcess` combat-controller field, which is new IDA work not attempted here. Also untested this session (no NPC combat occurred).
- **Appearance/variant diagnostic added**: new `[AppearanceDiag]` log in `CoSyncSpawnTasks.cpp` compares a clone-fallback NPC's live `baseForm` formID against the `EntityCreatePacket`'s requested base formID, to determine whether the engine gives cloned "Randomize"-flagged NPCs a private per-instance `TESNPC` copy (safe to mutate for a future appearance-sync fix) or points them at the shared editor template (unsafe). Didn't fire this session — no NPC took the clone-fallback spawn path.
- Widened the NPC bind-fallback retry window (`kBindFailSpawnDelay` in `World\CoSyncNpcWorldSync.cpp`) from 12s to 24s, giving the direct-lookup/cell-scan bind path (which never has the appearance-desync problem) more time to succeed before falling back to cloning.
- Health sync for NPCs was audited and found already working (`Character\CoSyncHealthSync.*`) — no change made.

### Verified by log (2026-07-18 17:48–17:53 session)
- Confirms the 2026-07-17 host-proxy VEH fix is holding: both host and client player proxies spawned successfully with no dropped `SpawnTask`s.
- Zero crashes, zero `HostSendNpcUpdates faulted` across a full connect/spawn/combat/disconnect cycle.
- Player-vs-player attack animation triggering (pre-existing) still works correctly after the `HostSendNpcUpdates_Impl` change above, confirming no regression to the player path.

---

## [0.0.10] — 2026-07-17

### Host Proxy Not Spawning on Client (VEH task-drop)
- Root cause: `CoSyncGameAPI::InitNpcProxy` queued three Papyrus VM calls (`SetEssential`/`SetProtected`/`IdleForceDefaultState`) immediately during `SpawnTask::Run`, before the actor's Papyrus VM script object was ready. The resulting EXEC access violation at a null vtable triggered the VEH handler to "simulate return" at F4SE's task-dispatch site, which silently dropped the *next* queued task — in testing this dropped the host player's own proxy spawn on the client.
- Fix: `CoSyncSpawnTasks.cpp` now sets `pl.m_pendingNpcInitDelay = 120` instead of calling `InitNpcProxy` directly. `CoSyncPlayerManager.cpp` ticks the delay down and fires `InitNpcProxy` at 0 (~2s after spawn, once the VM object is ready). New member `CoSyncPlayer::m_pendingNpcInitDelay`.

### PowerArmor Weapon Sync
- Fixed two bugs preventing weapons from appearing on Power Armor proxies: `m_weaponDrawDelay` countdown ran after the `m_isInPowerArmor` early-return guard in `TickAnimVars` (moved before it, `[PA]` log tag added); and proxies entering PA already holding a weapon had `m_weaponDrawDelay` stuck at 0 (`FlushPendingPowerArmorOps` now resets it to 30 frames on PA entry when a weapon is equipped).

---

## [0.0.9] — 2026-07-15

### Invisible Proxy Weapons Fixed
- Root cause: proxy weapon draw used the raw `weaponDraw` graph event, which only animates the arms, not the weapon mesh. Fix: `CoSyncEquipHook::DrawWeaponEngine` now calls `Actor` vtable slot 201 (`DrawWeaponMagicHands`) directly.

### Item Pickup Re-enabled
- `PickupHook` had been disabled against the wrong RVA (`0x500430`); corrected to the real entry point `0x500440` and re-enabled. Also throttled the per-frame pickup scan and fixed dynamic-base spam in the world-build-full (WBF) path.

### Map Marker
- Fixed remote-player map marker using CK reference `CoSyncRemotePlayerMarker` (formID `0x0100BE64`); `kMarkerLocalFormID` corrected to `0x00BE64`.

### Overlay Fixes
- Lobby health bar no longer shows the local player (only other proxies).
- Live player count now reads `CoSyncPlayerManager::GetConnectedPlayerCount()` (counts `createType==Player` entities) instead of the old counter, which only advanced on host and was picking up unrelated cell entities.
- Radiation now renders as a red chunk on the health bar. The Rads actor value has no stable formID on next-gen (name-registry-keyed getter); resolved by scanning `DataHandler->arrAVIF` for `fullName == "Rads"`, SEH-guarded and cached.
- Lobby bar repositioned to lower-left.

### PowerArmor Proxy Spawn
- Root cause: `SwitchToPowerArmor` hard-requires a valid PA furniture reference, but clients almost never have the host's exact chassis ref nearby, so the call was silently no-op'ing. Fix (`CoSyncPowerArmorSync.cpp`): `SpawnChassisAtProxy` now `PlaceAtMe`s a fresh frame from the host-sent base at the proxy's location, moves it into position, then calls `SwitchToPowerArmor`; the spawned chassis is tracked and deleted on PA exit. Individual armor pieces (arms/legs/torso/helmet) remain a follow-up — only the bare frame is synced so far.

### NPC A-Pose Root Cause
- `InstallAnimEventHook` (`TickHook.cpp`) was splicing a trampoline into `(*(player+0x38))[1]`, which on next-gen is the anim-graph event sink for **all** actors, not a player-specific hook — breaking post-install graph activation project-wide (confirmed: even console-spawned NPCs A-posed with this installed). The hook itself was already a functional no-op on next-gen. Disabled the install as the bisection test; NPC/proxy A-pose resolved as a result.

---

## [0.0.8] — 2026-07-14

### RVA Corrections
- `CoSyncGameAPI::LookupFormByID`: corrected from `0x311850` (mid-instruction, landed on a ModRM byte → `#UD`) to the real entry `0x311860`.
- `CoSyncEquipHook`: corrected `EquipObject`/`UnequipObject` from mid-function addresses with zero callers (`0xCE5DA0`/`0xCE5900`) to the real entry points `0xCE5A90`/`0xCE5F30`.

---

## [0.0.7] — 2026-07-10

### Container Sync
- New system: `CoSyncContainerSync` (`Items/CoSyncContainerSync.h/.cpp`) synchronises loot withdrawn from CONT-type containers between host and client.
- Detection: `Detour_InvAddFromWorld` (hook at RVA `0x00500430`) distinguishes container withdrawals from world pickups by checking `parentCell == nullptr`. On withdrawal, reads `TESObjectREFR::baseForm` (offset 0xE0) and its `formID` (offset 0x14) to identify the item taken.
- Nearest-container cache: `UpdateNearestContainer` scans `cell->objectList` every 100 ms for the closest CONT-type static ref within 300 game units and caches its formID in `s_nearestContainerFormID`. Called from `CoSyncWorldSync::Tick` whenever connected.
- Packet format: `CTR|containerFormID|baseFormID|count`. Host relays to all clients; clients queue a `PendingContainerOp`.
- Apply: `FlushPendingContainerOps` drains the queue on the game thread and calls `ObjectReference.RemoveItem(item, count, true, None)` via `CallFunctionNoWait`. C2712-safe `_Impl`/`Safe` split keeps non-trivial dtors (`VMArray`, `VMVariable`) out of the `__try` scope.
- Called from `CoSyncPlayerManager::Tick` after `FlushPendingLegendaryOps`.

### Cutscene / Scene Sync
- New system: `CoSyncSceneSync` (`Dialog/CoSyncSceneSync.h/.cpp`) synchronises BGSScene start and stop events between host and client.
- Detection: `TickLocalScene` polls `TESObjectREFR::GetCurrentScene()` (vtable slot 0x5C, declared in `GameReferences.h`) every 250 ms on the host. On scene enter, broadcasts `SCN|formID|1`; on scene exit, broadcasts `SCN|prevFormID|0`. Wrapped in `SafeGetCurrentScene` (`__try/__except`, `__declspec(noinline)`).
- Packet format: `SCN|formID|flag` (flag `1` = start, `0` = stop). Parsed with `strtoul`.
- Apply: `FlushPendingSceneOps` calls `LookupFormByID`, verifies `formType == kFormType_SCEN` (126), then dispatches `Scene.Start()` or `Scene.Stop()` via `CallFunctionNoWait` with an empty `VMArray<VMVariable>`. C2712-safe `ApplySceneOp_Impl`/`ApplySceneOpSafe` split.
- `TickLocalScene` called from `CoSyncNet.cpp` host-only tick block after `TickLocalDialogue`. `FlushPendingSceneOps` called unconditionally after `FlushPendingDialogOps`.

---

## [0.0.6] — 2026-07-10

### Quest Sync
- Host-only authority: `TickLocalQuests_Impl` and all quest hooks (`SetCurrentStageID`, `CompleteAllObjectives`, `SetObjectiveCompleted`, `SetObjectiveDisplayed`) now gate on `IsHost()` — clients never broadcast quest events, only receive and apply them.
- Late-joiner re-broadcast: host re-sends all active quest `QS_ST` packets every 15 seconds so clients who connect after quest start receive the correct state.

### Dialogue Sync
- Host-only authority: `TickLocalDialogue` now gates on `IsHost()` — only the host detects and broadcasts `DLG|O` / `DLG|C` packets.
- Client-side camera zoom: on `DLG|O` the client switches to `kCameraState_Dialogue` (index 12, pre-allocated by the engine), framing the NPC proxy during host dialogue. Previous camera state is saved and restored on `DLG|C`. Both operations are SEH-wrapped with `__declspec(noinline)` following the project's standard pattern.

### Map Marker Tracking
- Pre-placed map marker REFR (`CoSync.esp`, formID `0x0000B6CA`, base `MapMarker [STAT:00000010]`, Initially Disabled) teleported to the remote proxy actor every 1 second via `ObjectReference.MoveTo`.
- `CoSyncPlayerMarker::ShowForProxy(Actor*)` — called from `CoSyncPlayer::SpawnInWorld()` when the proxy spawns; enables the marker and registers it on the Pip-Boy map via `AddToMap(false)`.
- `CoSyncPlayerMarker::HideMarker()` — called from `CoSyncPlayerManager::DespawnEntity()` when the proxy despawns; disables the marker.
- `CoSyncPlayerMarker::Tick(double)` — called from `CoSyncPlayerManager::Tick()` at 1-second throttle; moves the marker via Papyrus `MoveTo` on the Papyrus VM thread to avoid raw-pointer races.
- Host and proxy both see the other's position on the map in real time.

### Pickup / Scrap / Debris Sync Fix
- **Bug 1 fixed** (`OnReceivePickup`): Host was relaying `PU|` packets to other clients then returning immediately, so the item was never disabled in the host's own world. Now the host falls through and queues the formID into `s_pendingPickups` same as a client does. Also inserts the formID into `s_broadcastPickups` to suppress redundant re-broadcast from the cell scan.
- **Bug 2 fixed** (`Tick` host branch): `ClientTickPickup()` was only called in the client branch, so the host's pending pickup queue was never drained. Added `ClientTickPickup()` to the host branch alongside the existing `ClientTickWorkshopBuilds()` and `ClientTickWorkshopScraps()` calls.

---

## [0.0.5] — prior

- Remote player fast movement sync between host and client.
- NPC sync on connection (host ↔ client).
- NPC spawning restored.
- Remote aspect added.
