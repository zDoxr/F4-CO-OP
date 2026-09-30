# CoSync 0.3.5 — Crash, NPC Combat, World Events, Power Armor Mods, Dialogue

Directive: "Crash Analysis + World Events + NPC Synchronization + Power Armor Mod Sync + Weapon/Combat Synchronization" (2026-09-29).

- **Status:** everything below is **built, committed and pushed** to `coop`, and **nothing has been tested in game**.
- **Version:** 0.3.5 (network 305). Peers must match.
- **Commits:** `a05a784`, `9800ea2`, `191d34b`, `86b6ccb`, `f82dc7b`, `8844fbf`, `febfe61`.
- **Target:** Fallout4.exe 1.11.240. Every address named here was read in IDA this session, or resolved through the installed Address Library (`version-1-11-240-0.bin`) and then checked in IDA.
- **Runtime guards:** every engine function CoSync now calls or hooks is prologue-checked or vtable-slot-checked at runtime. A mismatch disables only that feature and logs it.

Evidence labels: **VERIFIED** means proven from dumps, logs or IDA. **INFERRED** means reasoned but not proven. **NEEDS TEST** means only an in-game run can settle it.

---

## Section 1 — Crash analysis

Sources:
- `CoSync_crash.dmp`, the only CoSync dump, which each crash overwrote.
- The CoSync logs in `Data/F4SE/Plugins`: 11 session logs, 3 crash records.
- 10 Windows Error Reporting dumps in `%LOCALAPPDATA%\CrashDumps`, from Sep 25–26.

Tools:
- Python `minidump` package.
- dbghelp symbolization against the DLL's own PDB.
- IDA on the 1.11.240 IDB.
- For the old dumps, a rebuild of snapshot `5b37b78` in an isolated worktree, to recover symbols.

### C1 — Client, 2026-09-29 17:21:22 (`CoSyncUpdated_40916.log`, `CoSync_crash.dmp`) — **FIXED**

- **Thread:** the game thread, inside the CoSync tick (job "NonRendersafeAI").
- **Fault:** `Fallout4.exe+0x165940F`, a READ at `0x20`. `RBX=RCX=RDI=0x20`.
- **Call stack (PDB-symbolized):**
  - `CoSyncRuntime_TickGameThread`
  - → `CoSyncNet::Tick`
  - → `CoSyncPlayerManager::Tick` (`CoSyncPlayerManager.cpp:2450`, step `WeaponChange`)
  - → `CoSyncEquipHook::EquipItemDirect` (`:621`)
  - → EquipObject `+0xCE60A8` → `+0xCE850D` → `sub_140CAD0A0` → `sub_140CDFCD0` → `sub_14027F090` → lock `sub_141659400(0x20)`.
- **CoSync involvement:** direct. CoSync called the engine's equip.

**Root cause (VERIFIED in IDA).**

1. `sub_140CDFCD0` walks every inventory stack of the item and calls `ExtraDataList::GetInstanceData(stack->extraData)` (`sub_14027F090`) with no null check.
2. That stack had no list because of `BGSInventoryItem::SetWorn` (`sub_14034AF10`). When it is given **no** extra list and the stack count is greater than the equip count, it **splits** the stack and appends a new `Stack` with `extraData = NULL`.
3. The list was null because CoSync equipped a **freshly cloned** `TBO_InstanceData` that no stack owned. So `sub_140CAD0A0` found no stack (index −1, list null).
4. The count was 2 because of the log sequence 135 ms earlier:
   - `KILL|65572`;
   - then `PLOOT` added `1127225` (= `0x113339`, the NPC's own gun) to the corpse;
   - then the 10 s weapon watchdog re-equipped the **dead** NPC.

The Sep 26 "E1" fix (`EnsureStackExtraLists`) runs **before** the equip, so it could not cover a stack the engine creates **during** the equip.

**Fix:**
- `BindWeaponInstanceToStack` puts the instance on the first stack's own list, using the engine's `ExtraDataList::GetInstanceData`/`SetInstanceData` (`sub_14027F090`/`sub_14027F120`). The engine then matches that stack, and any split shares its non-null list.
- A corpse is never re-armed: the watchdog and the pending weapon work check life state and `KILL`.
- `PLOOT` adds only the missing part of each item.
- Crash dumps are now `CoSync_crash_<pid>.dmp`, keep the newest 5, and include indirectly referenced memory. Today's dump had no heap, so the inventory could only be reconstructed from IDA.
- **Confidence:** high. Every step of the path was read in IDA.

### C2 — Host, 2026-09-28 18:10:24, version 0.3.0 (`CoSyncUpdated_26724.log`, log only) — **FIXED**

- **Fault:** `Fallout4.exe+0xBF74BD`, `mov edx,[rax+14h]` with `RAX=0`, a READ at `0x14`.
- **Where:** the save writer, `sub_140BF72F0` case 5. `sub_140BF82F0(ref)` returns worldspace, or else parent cell, or null. The next instruction reads `->formID`.
- **Stack:** the save path, entered through the F4SE save hook (`f4se+0xE942`).
- **Ref:** `0xFF000EC8`, an **item copy CoSync spawned** 8 minutes earlier (`SpawnItemAtPosition`, base `0x230327`). It was in no cell at save time.
- **Why the existing guard missed it:** the save filter existed but covered only player-manager proxies.
- **Fix:** CoSync records every item ref it spawns. The save filter skips one that has neither a worldspace nor a parent cell, which is exactly the writer's failure condition.
- **Confidence:** high for the mechanism. Why the ref lost its cell is **INFERRED** (it was streamed out or despawned).

### C3 — Host, 2026-09-28 23:52:22 (`CoSyncUpdated_40280.log`) — **already fixed in 0.3.4 (RC9)**

`CoSyncAnimVarSystem::DumpGraphVariableNames+0x290`. The diagnostic call was removed in 0.3.4.

### C4 — 8 WER dumps, Sep 25 21:49 to Sep 26 17:54, code `0xC0000409` with parameter 7 — **already fixed Sep 26 (`4ed912b`)**

- **Fault site:** the same instruction in each, `mov ecx,7; int 29h`. That is the CRT `abort()` statically linked into DoxCoSync.
- **Symbolized dumps:** 19852 and 41636 captured the aborting thread's stack. Snapshot `5b37b78`, rebuilt without deploying, has the fault site at the identical RVA `0x219461`, and the faulting code bytes match.
- **Stack (snapshot PDB):** `_DllMainCRTStartup` → `dllmain_crt_process_detach` → `common_exit` → `_execute_onexit_table` → the static destructor of `` `anonymous namespace'::g_thread `` → `std::terminate` → `abort`.
- **Cause:** at game **exit**, a still-joinable global `std::thread` (the LAN discovery worker) was destroyed.
- **Status:** `4ed912b` (Sep 26 18:24) made its wrapper detach instead. Every abort dump predates that commit.
- **Confidence:**
  - Proven for 19852 and 41636.
  - For the other six, only the fault site survives (no stack captured). It is the same `abort()` code and consistent with the same cause, but that is not proven.

### C5 — 2 WER dumps, Sep 26 20:52, code `0xC0000374` (heap corruption) — **UNPROVEN, not reproduced since**

- **Stack:** the main thread. Main loop `0xC2F935` → `0xC320F5` → F4SE hook (`f4se+0xA0170`/`+0x163FF`) → **PrismaUI_F4** per-frame update → a heap free that found a corrupt block.
- **CoSync frames:** none on the stack.
- **Timing:** host and client died in the same second, which suggests a common input.
- **Why unproven:** the corrupt block's memory is not in the dumps. Heap corruption is detected when the next free happens, not when the corruption is written, so the writer cannot be named.
- **Not reproduced** in any of the 7 logged sessions since.
- **Needs:** a dump with heap memory. The C1 fix now captures indirectly referenced memory.

---

## Section 2 — Power Armor

**Preserved as is:**
- the enter/exit state machine (Entering phase, engine-paced eject, post-exit body check);
- the chassis clone the proxy wears;
- piece injection;
- `IMODS` for pieces while worn;
- `PAI` (frame inventory);
- `PAMODS` change detection;
- echo suppression and the occupied-frame guards.

**What was broken (VERIFIED in the client log):**
- `PAI: no local frame` appeared **74×**.
- **Every one** of the 13 `PAMODS` received failed with `-1`, meaning the frame was never resolved.

Why:
- The host's frames are runtime (`0xFF`) refs. The receiver only searched the **local player's own cell** by base form and position.
- A machine without its own copy of the sender's frame **deleted** the proxy's chassis on exit, so the frame vanished there.
- On enter, the proxy's pieces were injected as bare base forms, and there was no frame copy to take the mods from.
- Net effect: station work (paint, armor, material and special mods) never reached anyone.

**Change: frames get a network identity (`86b6ccb`).**
- The mapping `(sender entity, sender's frame ref) → local frame copy` is learned on enter, and resolution tries it before the position search.
- When a machine has no copy, the chassis the proxy leaves becomes that copy. The engine's own exit (`sub_14098D8C0`) has already returned the pieces to it and placed it at the exit point.
- **Flow is event → state → apply, with no per-tick rebuilds.** A station edit changes the frame's mod description, which sends `PAMODS` once. Receivers resolve the frame through the mapping and attach each OMOD with the engine's `AttachModToInventoryItem` worker. When the owner re-enters, the enter step copies those mods onto the chassis, and `IMODS` covers the worn pieces.
- **Packets:** unchanged. `PAMODS`, `PAI`, `PA` and `IMODS` keep their format.
- **Limit:** a frame the sender placed while this machine was never near it, and never entered in view, still has no copy here. Replicating frame placement is world-object replication, not modding.

---

## Section 3 — World events

Principle: capture the engine's choke point, not a list of cases (`f82dc7b`, `World/CoSyncWorldStateSync`).

| What | Engine mechanism (VERIFIED) | Captured | Replicated |
|---|---|---|---|
| Destruction of any destructible: cars, barrels, gas pumps, doors, gates, "objects destroyed" | Stage is **derived** from destructible health (extra `0x55`, `sub_14028CFD0`) against `BGSDestructibleObjectForm` thresholds (`sub_140300730`). **Every** stage change is announced by `sub_14052C7F0` (TESDestructionStageChangedEvent send; callers: damage path `sub_1402FE390`, `ClearDestruction` `sub_1403014C0`). | Hook on `sub_14052C7F0` → `WSD|ref|base|pos|old|new|health` | The receiver damages its copy through the engine's `DamageObject` task (`sub_140C461E0`, task 7) by exactly the health difference. The engine then plays model swap, debris and explosion itself. Monotonic: late reports can't un-destroy. `ClearDestruction` resets. Unloaded refs are retried up to about 10 min. |
| Vehicle and car explosions | The final destruction stage's explosion | Through destruction (above) | `ExplosionSync` skips an `EXP` that matches an explosion that already happened here (same base, 400 units, 3 s). World state is ticked before explosions so the local one exists first. |
| Grenade, missile and mini-nuke explosions | EXPL refs; PROJ refs in flight | Existing `ExplosionSync` / `ThrowableSync` (unchanged) | Plus the duplicate check above |
| Doors, gates, manholes, hatches opened or closed by anyone (player, NPC, script) | `BGSOpenCloseForm::SetOpenState` `sub_140305A90`, 21 callers including AI door use and the Papyrus `SetOpen` native `sub_14115E0C0`. The Concord Deathclaw manhole is **DOOR `MQ104_RoadManholeDoor01`**. | Hook: animated changes only, when `GetOpenState` (`sub_140305CE0`) differs, and not during a player activation (`DOOR` already covers those) → `WSO|ref|base|pos|open` | Absolute state, applied with `SetOpenState` |
| Quest and script world changes: things appearing or disappearing | Client quest stages are suppressed (`CoSyncQuestHook`), so only the host's scripts run. Papyrus Enable/Disable go through `EnableFunctor`/`DisableFunctor` (vtables `0x25CCA58`/`0x25CCAF8`, run slot 1 → `TESObjectREFR::Enable` `sub_140515970` / virtual Disable, slot `0xB0`) | Slot swap. **Host only.** Non-actor refs with a static formID only → `WSE|ref|0/1` | Engine Enable/Disable. A client never accepts it from anyone but the host. Frames CoSync represents are never touched. |
| Quest stages | Existing `QS` sync (unchanged) | — | — |
| NPC-caused changes (the Deathclaw opening a grate) | The NPC's machine runs the door or destruction code | Same hooks, on the machine that simulates the NPC (the host) | as above |

**Not covered (documented, NEEDS TEST before extending):**
- Scripted animations on activators (`PlayAnimation` / `PlayGamebryoAnimation`).
- Havok physics debris, which `WorldSync` object moves already stream.
- Container contents changed by scripts.

---

## Section 4 — NPC synchronization

### Findings (VERIFIED, client log 2026-09-29)

- **Host NPC gun shots:** **0** sent. The host sent 298 NPC `AEV` events, all reload or throw codes. Shot detection read the `isFiring`/`iSyncFireState` graph variables, which NPC graphs never change.
- **Replicated NPC animation events:** **0 of 828 accepted** by the puppets' graphs. They were captured at the `BSAnimationGraphEvent` sink, which carries what a graph *emits* (`attackStateEnter`, `sightedStateExit`, …), and a graph has no transition on those names.
- **Damage:** had **no hit attribution**. It was detected by HP-delta polling. `TickDamageDetection` itself documents the resulting "phantom damage" from a machine's local AI.
- **Puppets' own combat behavior tree** kept drawing their weapons against the owner's state. Up to 129 contradicting calls per 10 s, from caller `base+0xF34257`.
- **Crash C1** came from the weapon pipeline acting on a dead NPC.

### Model after 0.3.5

| Aspect | Authority | How |
|---|---|---|
| Identity | host | Placed NPCs bind by their persistent ref formID (direct bind). Dynamic ones spawn a controlled copy, and the local duplicate is hidden. Unchanged. |
| Owner | **host**, wherever the host's game simulates the NPC | New in `HostTickOwnershipHandoff`: the host takes back any NPC it is near enough to simulate. A client owns an NPC only outside the host's loaded area. Engine limit: actors are simulated only in the cells around each player. |
| Position, rotation, movement | owner | `EU` (~15 Hz, timestamp-ordered), interpolated 120 ms behind, steered with the engine's `SetPosition`, and the owner's velocity is handed to the character controller. Unchanged. |
| Animation / actions | owner | **New:** the owner's graph **inputs** — the Actor holder vtable slot 1 (`0x2568DB0`) → `NotifyAnimationGraphImpl` → `BSAnimationGraphManager::NotifyAnimationGraph` — for accepted gameplay events (attack, block, reload, stagger, recoil, hit reactions, aim/ready, throw, jump, sneak, cower/panic). Death, ragdoll, furniture, dialogue, draw and movement are excluded. They are replayed into the puppet's graph as `AE`. The output-sink branch was removed. |
| Target / aim | owner | Each shot carries the owner's target (`AIProcess+0xBC`, the handle `Fire` aims at). Continuous look-at is not replicated (open item). |
| Weapon equip / draw | owner | `EU` weapon plus the weapon-authority hook. **New:** the combat behavior tree (`sub_140F34140`, `CombatBehaviorEquipRangedWeapon`) is refused too. No equip on dead actors. |
| **Firing** | the machine simulating the shooter | **New `CoSyncCombatAuthority`.** Hook on `TESObjectWEAP::Fire` (`sub_1404799B0`). A shooter simulated here → `SHOT|shooter|seq|weapon|equipIndex|target|flags`. A proxy's own engine fire is refused. Receivers run the **engine's own `Fire` on the proxy**, with the local copy of the target written into `AIProcess+0xBC` for that call: real projectile, muzzle flash, sound and impacts. Plus `attackStart` for the recoil/attack clip. Explosive projectiles are animation-only (`ThrowableSync`/`ExplosionSync` carry them). |
| **Damage** | the machine simulating the aggressor | **New `DoHitMe` gate** (`sub_140CD2CA0`, aggressor at `HitData+0x40`): a hit whose aggressor is simulated elsewhere is refused. The aggressor's machine applies it, and the result reaches the victim's owner as `DMG` (unchanged HP relay). This also ends phantom damage and the double damage of replayed shots. |
| Health | owner | `HP` broadcast as the authoritative correction (unchanged). Non-owners keep the runtime-essential bit, so local damage can down a puppet but never kill it. |
| Death | owner | `KILL` (reliable, idempotent). **New:** stale `EU` after `KILL` is dropped. No transform smoothing on a corpse, so the ragdoll stays a ragdoll and `CORPSE` places it. No forwarded animation events or shots from the dead. No re-equip. |
| Loot | owner rolls, each player gets a personal roll | `PLOOT` now tops up to the rolled counts instead of adding on top |
| Spawning / destruction | host | Unchanged. Tombstones stop resurrection by late creates. |

**Projectile architecture chosen: option C (hybrid), because of how the engine works.** `TESObjectWEAP::Fire` builds its launch data itself: origin from the proxy's own weapon fire node, aim from `AIProcess+0xBC`.

- Option A (replicating projectile refs) would mean matching short-lived runtime refs across machines, which never share formIDs.
- Option B alone would let every machine apply damage.

So: the authoritative shot and all damage belong to the shooter's machine, and the visual projectile is reproduced by the engine on every other machine, with its damage refused.

**Sequencing.** `EU` is timestamp-ordered. `SHOT` carries a per-shooter sequence (duplicates and old shots are dropped, as are shots over 1 s old). `KILL` is final. Destruction is monotonic.

**Threads.** Every new hook (Fire, DoHitMe, graph input, destruction, SetOpenState, the functors) only enqueues, under a mutex, or reads. All sends and engine actions run in the CoSync game-thread tick. Replay paths set thread-local guards so the engine's reaction isn't captured again.

**"Host and client see the same NPC state" — what is and isn't guaranteed.** The following are shared authoritative state:
- position (interpolated 120 ms behind);
- equipped weapon and draw state;
- each shot, with the same target;
- HP;
- death and corpse position.

**Engine-inherent differences (INFERRED):**
- projectile spread and impact points can differ slightly, because each machine's engine rolls its own spread;
- continuous head and aim tracking between shots isn't replicated;
- locomotion clips on puppets come from their own movement graph fed by the replicated velocity.

---

## Section 5 — Dialogue

- **Engine limit (VERIFIED):** Fallout 4 player dialogue is scene-driven and bound to the single local `PlayerCharacter`. `MenuTopicManager` (`0x30E6E50`, speaker `+0x14`) shows the current scene action's four choices. A conversation therefore exists only in the host's engine. A client cannot be the scene's "player" without re-implementing the dialogue system.
- **What exists now (`8844fbf`, `Dialog/CoSyncSharedDialogue`):**
  - **Host authority:** conversations run in the host's engine. A client's activation of an NPC is still refused. The host is told (`DLGQ`) so they can start it.
  - **Client visibility:** the host captures the four option texts at `DialogueMenu::ProcessMessage` (slot 3, `"DialogueSetButtonText"`, index `+0x18`, text `+0x20`) → `DLGM|speaker|seq|name|4 options`. Clients within 900 units see the NPC and the options in the chat panel.
  - **Client choice:** F1–F4 → `DLGP|client|speaker|seq|choice`.
  - **Validation (host):**
    - the sender owns the client entity (host message check);
    - the menu is still open on the same speaker;
    - it is the same option set (seq);
    - the option exists;
    - the client's player is within 600 units.

    It then runs the engine's own click path, `MenuTopicManager::SelectOption` `sub_140BAF7C0` (what `DialogueMenu::Call` case 1 does). `DLGR` reports the result.
  - **Quest and world consequences:** the NPC's reply (`DLG|L`), quest stages (`QS`) and world changes (`WSD`/`WSO`/`WSE`) all come from the host's engine.
  - **Clients cannot:** start dialogue, change dialogue state, pick invalid, stale or out-of-range options, or drive the AI.
- **NPC line text:** it reaches clients as the spoken line (`Say` replay with the game's subtitles). The response text itself is not sent in chat (open item).

---

## Section 6 — Weapon synchronization

| Topic | State |
|---|---|
| Player weapons | `EU` equipped weapon and draw state (unchanged). **Shots now use `SHOT`**, from the engine `Fire` hook, one per real shot. The old `AEV` fire was retired because it would have doubled every shot. Reload and throw keep `AEV`. |
| NPC weapons | The same `SHOT` path (0 → every shot). Equip is `EU` plus the engine-native, stack-owned instance (C1 fix). |
| Visibility / "the weapon appears, then vanishes" | 0.3.4 refused the AI-update holster. **0.3.5** also refuses the combat-tree draw that the 0.3.4 diagnostic named (`sub_140F34140`). There is no "force visible every frame": every refusal is an authority rule on a verified engine caller. Any remaining contradicting caller is logged with its return address. |
| Attachment | Engine equip (`EquipObject` → `AddWornItem`) with the stack-owned instance. Fully modded weapons keep their mesh through the instance data. |
| Firing, projectile, muzzle flash, recoil | Engine `Fire` on the proxy (projectile, flash, sound). The `attackStart` graph event plays the attack clip. Players also get `weaponFire`/`weaponFireEffect`. |
| Reload | `AEV` 2/3 → `reloadStart`/`reloadEnd` (working: 133/134 NPC replays in the 0.3.4 log) |
| Damage | Aggressor's machine only (`DoHitMe` gate) |

---

## Test matrix — all NOT RUN (needs in-game test)

| Area | Test | Log line to watch | Expected |
|---|---|---|---|
| C1 | Kill an armed NPC near a client; loot it | `[GameAPI] weapon … first stack given its instance data`, `entity=… is dead -- pending weapon equip dropped` | no crash, no re-equip on the corpse |
| Dumps | Any crash | `Minidump -> …CoSync_crash_<pid>.dmp` | one dump per crash, newest 5 kept |
| NPC fire | Host fights raiders with a client watching | host `[CombatAuthority] TX SHOT`, client `replayed SHOT … anim=accepted` | the client sees muzzle flashes and tracers aimed at the right target |
| NPC fire | Enemies target client 2 | host `TX SHOT … target=<client2 eid>` | client 2 sees the NPC aim and fire at them; damage arrives as `DMG` |
| Damage | Stand in front of a puppet that is shooting | client `[CombatAuthority] … remote-aggressor hits refused=N` | HP drops only via the host's `DMG`, once |
| Animation | Melee and stagger NPCs | host `[AnimEventHook] NPC graph inputs …`, client `AnimEvent RX … accepted=1` | a mostly accepted ratio (previously 0/828) |
| Death | Kill an NPC while it moves | client `EU for dead entity=… dropped` | the ragdoll falls freely, then settles at `CORPSE` |
| Ownership | A client explores away, then the host arrives | host `Handoff: … takes it over` (to the host) | the NPC returns to host authority |
| Weapon | An NPC puppet in local combat | `[WeaponAuthority] refused …` (no more `OUTSIDE … 0xF34257`) | the weapon follows the owner |
| PA | Paint or mod a frame at the station, watched from another machine | `frame … is frame … on this machine`, then `PAMODS` applied, and no `-1` | the same paint and mods, before and after re-entering |
| PA | Wearer exits somewhere the other machine had no frame | `Exit PA: kept chassis … as this machine's copy` | the frame stays visible there |
| Destruction | Shoot a car until it explodes (host, then client) | `[WorldState] TX WSD`, then `applied` on the peer; `[ExplDiag] … skippedKnown` | the same stage and one explosion on both |
| Doors | A quest script or NPC opens a door; the Concord manhole scene | host `TX WSO`, client `applied` | the door moves on both |
| Enable/Disable | A quest step that shows or hides objects | host `TX WSE`, client `applied` | the same objects visible |
| Dialogue | Host talks to an NPC with a client nearby | host `[SharedDialogue] TX options`; client chat shows `[F1]…` | the client sees the options |
| Dialogue | The client presses F2 | host `entity=… chose 1 … executed by the host` | the conversation advances on the host |
| Dialogue | The client presses F2 from far away or after the menu closed | `refused choice … too far / conversation is over` | refused, with the reason in chat |

## Open items

1. Continuous NPC aim and head tracking between shots is not replicated. The target is sent only with each shot.
2. Engine-inherent projectile spread differs per machine. It is visual only; damage is authoritative.
3. Replayed shots damage world objects locally. Destruction merges monotonically, so it converges, but a client's copy can run ahead of the host until the host's value arrives.
4. Frames never seen by a machine, scripted activator animations, and script container changes are not replicated.
5. NPC response text is not mirrored into chat; players rely on the game's subtitles for the spoken line.
6. C5 (PrismaUI heap corruption) needs a dump with heap memory; dumps now include it.
