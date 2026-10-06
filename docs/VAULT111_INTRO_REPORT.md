# Vault 111 intro, cryopods, Pip-Boy, elevator (0.5.6)

Date: 2026-10-06. Status: **built (0 errors, 0 C++ warnings) and checked offline. Not run in game yet.**
New module: `DoxCoSync/World/CoSyncVault111.{h,cpp}`. Version 0.5.6 (new `V111|` messages, so every player needs 0.5.6).

Evidence used:

- Logs from the 0.5.3 test on 2026-10-05, 22:08–22:34 (host `CoSyncUpdated_6532`, client `38320`): the client got stuck.
- Logs from the 0.5.5 test on 2026-10-06, 13:55–14:13 (host `14860`, client `5432`).
- `Fallout4.esm` records, read with an ESM indexer.
- The compiled vanilla scripts from `Fallout4 - Misc.ba2`, disassembled: `QF_MQ102_0001CC2A`, `MQ03QuestScript`, `MQ03ControlPanelScript`, `vaultExitElevatorSCRIPT`, `vaultExitElevatorDownSCRIPT`, `vaultExitCommonWealthSCRIPT`, `VaultDoorConsoleScript`, `V111PipboySkeletonScript`, `MQ102PipboyTakeFurnitureScript` and others.

---

## How vanilla Vault 111 works (what CoSync has to work around)

Every script below acts on `Game.GetPlayer()` only, which means it acts on the player of the machine it runs on.

| Step | What runs it | What it does |
|---|---|---|
| Lift down into PrewarVault111 `0xCA260` | MQ101 stage 600 | |
| Suit, walk to the pods, freeze, Kellogg | MQ101 stages 6xx–8xx | The player sits in `MQ101PlayerCryopodREF 0xCA2A8` |
| Freeze → wake jump | MQ101 900 | Magic doors `0x148B68`/`0x148B69` into Vault111Cryo `0x16D8`, player pod `MQPlayerCryopodREF 0x4A011` |
| Wake-up scene | MQ102 stage 0 (`ExitCryoScene`) | |
| Control back | `MQ03QuestScript` on `CameraOverrideStop` | Sets MQ101 1000 and MQ102 1 |
| Pip-Boy from the skeleton | `0xAB6AF`, furniture `0x75039` | MQ102 stage 4 `BlockActivation`s the skeleton and the Pip-Boy ref. Stage 6 follows on the `pipboyIdleRoot` animation event |
| Gear console | `MQ03ControlPanelScript`, needs stage 6 done and 7 not done | Stage 7 adds the Pip-Boy `0x21B3B`, removes `PipboyDusty 0x6D7B8` and **activates the console furniture as the player** |
| Console button | `DepressButton` | Stage 9: control panel `Stage3`. Its `Stage4` event plays the gear door `Vault111GearDoorREF 0x330C3` `Stage2`, turns on the klaxons and `V111ExitLightingEnableMarker`, then sets stage 8 |
| Elevator down to the bridge | Trigger `0x20C3A` (`vaultExitElevatorDownSCRIPT`), needs MQ102 stage ≥ 9 | Plays `V111ElevatorREF 0x244EB` `stage2`, then **disables itself** |
| On the platform | `RespecTriggerREF 0x8C94A` (`vaultExitElevatorSCRIPT`, runs once) | "Final changes" menus, then elevator `stage3` (up) and enables `0x1D6CD3`. Load door `0x1F180` takes the player outside |
| Outside | `Vault111OneShotExteriorElevatorTrigger 0x51877` | **Disables itself**, sets MQ102 stage 10 |
| MQ102 stage 10 fragment | | **`RespecTriggerREF.DeleteMyInputLayer()` → `Disable()`**, then stage 15 and onward |

---

## Root Cause

**The client was trapped because the host's own exit was copied onto the client's machine while the client was still inside.** This is visible in the 2026-10-05 logs:

1. **22:30:03 `WSE|134202|0`**: the host's elevator-down trigger `0x20C3A` disables itself. CoSyncWorldStateSync copies every host script `Disable()` to clients. The client's trigger was disabled, so **its elevator could never come down to the bridge**.
2. **22:30:36 `DOOR|0x1F180`**: the host's auto-load door was replayed on the client. The earlier uncommitted DoorHook load-door skip, now part of this change, fixes that.
3. **22:30:56 QS MQ102 stage 10**: the client applied it. Its fragment **disabled the client's `RespecTriggerREF`**, so there was no ride at all. `WSE|575818|0` did the same again.

The same mechanism also breaks other moments:

- At **22:24:09**, the host's stage 0 sent `WSE|1346408|0`. That disables the client's magic door from the pre-war freeze into the post-war room. A client still in its pod would be stuck in the pre-war vault.
- On **2026-10-06 at 14:12:08**, the host's **stage 7** reached the client. It gave the client a Pip-Boy and **pulled the client into the gear console furniture**, wherever the client was standing.
- The client's **stage 4** (applied on the host at 14:11:26) blocked the host's Pip-Boy skeleton.
- `QS_TRK` was sent twice per scan in both directions while MQ101 and MQ102 were both flagged active.

In short, CoSync treated per-player scripted moments (quest stages, script enable/disable, load doors) as shared world state.

## Cryopod System

There is one list, written by the host and sent as `V111|pods|e0,e1,…` every 5 s while anyone is in the intro. The host is first, then each player in the order they first appeared in the game. Players still in the lobby are not included.

The list only grows during a session. A player who leaves keeps their place, and their pod just stays empty, so nobody else's pod changes mid-sequence.

- Index 0 is the scripted player pod. Indices 1–6 are the six resident pods, nearest first: `0x4A00F 0x4A00E 0x1934 0x4A016 0x4A015 0x4A014` post-war and `0xCA2B4 0xCA2B5 0xCA2B6 0xCA2B1 0xCA2B2 0xCA2B3` pre-war. The spouse pod is never used.
- **More than 7 players** (CoSync allows 8): index 7 and above get **no pod**. Those players are shown standing in the aisle, so nobody is duplicated or stacked.
- During the vanilla sequence every machine's own player is in the scripted pod. That is the cutscene and is not changed. Remote players are drawn in their own pod. Only remote players' **proxies** are remapped, and only while the remote player stands in the scripted pod or its doorway (90 units pre-war, 200 during the wake-up), and only until that player has "reapplied".
- On a client, the host (index 0) is drawn in that client's own pod. The scripted pod is occupied by the local player, so this swap is the only one needed. Every other pod shows the same owner on every machine. The offline simulation for 1–8 players printed this for 4 players:
  `machine 0 {0:0,1:1,2:2,3:3} · machine 1 {0:1,1:0,2:2,3:3} · machine 2 {0:2,1:1,2:0,3:3} · machine 3 {0:3,1:1,2:2,3:0}`
- Log lines: `[CoSyncVault111] Player assigned to Cryopod SteamID= EntityID= Username= CryopodFormID=`.

## Empty Cryopods

Only the pods given to players are cleared. Their seated resident (pre-war) or corpse (post-war) is disabled on each machine. Only players in the list get a pod, so there are no placeholder or "2nd Player" bodies.

The remaining pods are not touched by CoSync. **They keep their vanilla occupant**, because the residents and corpses are part of the vanilla scene. Log line: `Cryopod left empty CryopodFormID=… (vanilla occupant kept)`.

When playing alone the vault is fully vanilla. 0.5.3 used to clear all six pods for any number of players; that is gone.

> If "empty" should mean physically empty (no resident at all), that is a one-line change in `VacateAssignedPods`: vacate pods `1..6` instead of `1..n-1`.

## Cutscene Handling

Each machine's **phase** is derived from game state every tick, with no timers:

| Phase | Condition |
|---|---|
| PreWar | MQ101 running, stage < 600, not in a pod room. The 0.5.3 lockstep is kept (the host's story, SPECIAL with the Vault-Tec rep) |
| VanillaSequence | MQ101 running and (stage ≥ 600 or in PrewarVault111/Vault111Cryo) |
| VaultExplore | MQ101 completed, MQ102 running, stage 10 not done here |
| Complete | MQ102 stage 10 done here, or the game is not in the intro |

During **VanillaSequence**, nothing of the sequence crosses machines:

- **Quest stages**: MQ101 above 600 and MQ102 stages up to 10 (except 3 and 9) are *personal*. The client does not send them to the host. The host refuses them. A client ignores the host's. The Tutorial and DialogueVault111 quests are also personal while inside. `QS_ST`/`QS_DONE` for MQ101/MQ102, and the objective and track packets for the intro quests, are ignored while inside.
- **NPCs**: actors in PrewarVault111 (staff, residents, spouse, Kellogg's group) are never discovered or synced. An already-synced actor that walks in there is dropped by the host. It is not handed to a client, so its own vanilla AI runs on each machine.
- **World state**: WSE/WSO for refs in the two intro cells, and for the exit's own triggers and markers (`0x20C3A 0x8C94A 0x481EC 0x1D6CD3 0x1D6CD0 0x481D4 0x2075E4 0x51877 0x20E880 0x148B68 0x148B69 0x1F180`), are not applied until this player is Complete.
- **The local player is never moved by CoSync** during the sequence. The only player moves are the pre-vault `INTRO|vault` catch-up, which only runs while the player is outside the vault, and the one-time reapply after control returns.
- **Joins**: the host's lobby holds new players (`WORLDWAIT`) while the host is in VanillaSequence. They come in when the host is awake, so nobody joins in the middle of a cutscene. The bathroom mirror spot is only used during PreWar, and a saved position inside PrewarVault111 is never restored.

## Reapplication

The vanilla sequence counts as complete when MQ101 is completed and MQ102 stage 1 is done (control back, set by vanilla's own `CameraOverrideStop` handler) and no menu is open. Then, **once** per machine:

1. `[CoSyncVault111] Vanilla Cryopod sequence completed … Reapplying multiplayer state`.
2. A client still within 300 units of the scripted pod is moved to the aisle in front of **its own pod** (list index). The host stays where it is, because its pod is the scripted one. Over-capacity players go to the aisle.
3. The reapplied flag is sent (`V111|st`). Other machines stop remapping that player's proxy, so from here on **every machine shows real positions**, and they agree. The offline check "everyone reapplied ⇒ shown == real" passed for 1–8 players.
4. The Pip-Boy state is applied (next section). The gear door state is checked against the host's.

## Pip-Boy

**The existing fallback (unchanged).** `CoSyncProfiles` gives the Pip-Boy back with the engine's own add and equip in three places:

- `ReplaceInventory`, when any profile is applied on join after the intro, including fresh characters.
- The fresh-start check after the character editor (`GiveStartingItem(Pip-Boy)`).
- During a pre-war join, no Pip-Boy is given ("it comes the normal way in Vault 111").

That runs on the **client**, once per join, after the host's world is loaded. Its weak point was the "normal way": the skeleton could be blocked by another player's stage 4, and a player who joined pre-war depended on it.

**Now:**

- **Event**: "the Pip-Boy event happened" means MQ102 stage 4 (skeleton) or 7 (console) is done. It is persistent because the host derives it from:
  - its own stages,
  - the host carrying the Pip-Boy after the intro (the host also receives it when a client's event comes),
  - a client's `V111|st` Pip-Boy flag.
- The host sends `V111|world|<pip>|<gate>`. Every machine whose player has no Pip-Boy (neither real nor dusty) gets one through **`CoSyncProfiles::EnsurePipBoy`**. That function is the same `GiveStartingItem` path as the fallback. It does not replace it.
- It is never given mid-scene: not before the reapply, not in a menu, not between stages 4 and 6 (the skeleton animation) or 7 and 8 (the console furniture).
- If the console's own `AddItem` makes a second copy, the extra copy is removed.
- Logs: `Pip-Boy event completed`, `Pip-Boy fallback applied`.
- Late joiner after the event: the existing fallback gives it, and `EnsurePipBoy` confirms there is no duplicate. Joiners during the vault sequence are held until the host is awake, so they arrive into VaultExplore with the Pip-Boy from the fallback.

## Elevator

**What blocked it:** the elevator is not one shared object. It is a ride that each machine's own triggers run for that machine's own player. The host's ride reached the client in three ways: the WSE self-disable of `0x20C3A`, the stage 10 fragment disabling `RespecTriggerREF`, and the replay of load door `0x1F180`. All three are now stopped (see Cutscene Handling).

**What each machine needs to ride out, and now gets:**

- MQ102 stage ≥ 9: shared, see Elevator Gate.
- Its own `0x20C3A` and `0x8C94A` still enabled.
- The host's later MQ102 stages (> 10) kept away while inside. They are **deferred** and applied when this player is out. The same applies on the host for a client that left first.

**Recovery:** if `RespecTriggerREF` is found disabled while this player is still inside and has not ridden (for example an older peer, or a save made after the host left), it is enabled again (`EnableWorldActor`). The trigger's one-shot flag is still set, so the ride works.

**What is deliberately not done:** the host's elevator animation is not played on the client. If the client's elevator went up without the client on it, the client would lose its ride, because the down trigger only fires once. When the host rides, the client sees the host's proxy rise. When both stand on the platform, each machine's own trigger runs and both ride up.

Logs: `Elevator state changed: lowered to the bridge`, `rising with this player`, `left Vault 111`, plus each other player's state from `V111|st`.

## Elevator Gate

The gear door opens through **MQ102 stage 9**, which is *shared and host-authoritative*:

- A client's stage 9 (it pressed the console) goes to the host as QSREQ. The host applies it and opens its gear door. The host's hook then sends QS 9 to everyone. This also works while the host is still waking up; the 0.5.3 blanket intro refusal no longer covers MQ102.
- On each machine, applying stage 9 runs the vanilla path: control panel `Stage3` → `Stage4` → gear door `Stage2`, klaxons, exit lights, stage 8. It also makes that machine's elevator-down trigger usable (stage ≥ 9).
- **Stage 7, the Pip-Boy plug, is personal.** Nobody is pulled into the console furniture any more.
- **Backup:** the host also sends the gear door state (`V111|world|…|gate`). A client inside whose stage 9 is missing applies it locally, at most once every 15 s (`Applying Vault state: the host's gear door is open`).
- Stage 3 (the QT00 security door) is shared the same way.

## Files Changed

- `DoxCoSync/World/CoSyncVault111.h`, `CoSyncVault111.cpp`: **new**. Phases, stage policy, deferral, cryopod list, remap, residents, reapply, Pip-Boy state, gate and elevator state, the WSE filter, the NPC predicate, the join hold, and the `V111` messages.
- `DoxCoSync/World/CoSyncQuestSync.{h,cpp}`: stage policy on QS apply and QSREQ, QS_ST/QS_DONE/objective/track filters, one QS_TRK per scan, and `GetLocalStage`/`QueueStageApply`/`QueueStageRequest`.
- `DoxCoSync/Hooks/CoSyncQuestHook.cpp`: client personal stages are set locally only. MQ101 lockstep only applies in PreWar.
- `DoxCoSync/World/CoSyncSharedIntro.{h,cpp}`: cryopod code moved to Vault111. Lockstep is PreWar-only. MQ102 is out of the blanket refusal. Mirror spot is PreWar-only. The vault catch-up spread now uses the pod list. *(Includes the earlier uncommitted "MQ101 only" gate, superseded.)*
- `DoxCoSync/World/CoSyncDoorHook.cpp`: load doors are never replayed. *(Earlier uncommitted change, kept: it fixes the `0x1F180` replay.)*
- `DoxCoSync/World/CoSyncElevatorSync.cpp`: the earlier uncommitted `vaultExitElevatorSCRIPT` "load elevator" hunk was **reverted**. That script is a trigger with no OnActivate, so riding along by activation could not work.
- `DoxCoSync/World/CoSyncWorldStateSync.cpp`: Vault 111 WSE/WSO receive filter.
- `DoxCoSync/World/CoSyncNpcWorldSync.cpp`: PrewarVault111 actors are excluded from sync, dropped on entry, and never handed off.
- `DoxCoSync/Networking/CoSyncWorldTransfer.cpp`: lobby hold during the host's vanilla sequence.
- `DoxCoSync/Networking/CoSyncNet.cpp`: `V111|` routing and resets.
- `DoxCoSync/CoSync/CoSyncRuntime.cpp`: tick, run before SharedIntro.
- `DoxCoSync/Character/CoSyncPlayer.cpp`: proxy remap goes through Vault111.
- `DoxCoSync/Character/CoSyncProfiles.{h,cpp}`: `EnsurePipBoy`, `LocalItemCount`, no position restore into PrewarVault111.
- `DoxCoSync/Core/Version.h`: 0.5.6.
- `DoxCoSync/DoxCoSync.vcxproj`: new files.
- `CHANGELOG.md`, `docs/TESTING_MATRIX.md`, this report.

## Testing

What was actually done:

- **Build**: full solution, Release x64, toolset v145. 0 errors, 0 C++ warnings.
- **Offline simulation** of the cryopod display rules (`docs/vault111_podsim.py`, which mirrors `RemapProxyIntoPod`/`Reapply`) for 1–8 players. It checks that no pod holds two players on any machine during the freeze, and that after the reapply every machine shows the real positions. **Passed.**
- **Log forensics** on the two real sessions above, which established the root cause.

**Not done:** none of the six Vault 111 scenarios has been run in game yet:

- host only
- host + 1 client
- host + 3 clients
- late join
- elevator from the host's side
- elevator from the client's side

They are listed step by step, with the log lines to look for, in `docs/TESTING_MATRIX.md` § 0.5.6. This is not "fixed" until those pass.

## Known limits / follow-ups

- **WSE design issue, everywhere:** host one-shot triggers that act on `Game.GetPlayer()` and disable themselves are copied to clients by WSE in the whole game, not just Vault 111. Vault 111 is now filtered. A general rule ("a trigger disabled by its own script for the local player is per-player") is a separate change.
- When the host rides up and the client is not on the platform, the client sees the host's proxy rise without the platform.
- Remote proxies jump once per player at that player's reapply, from their pod to the spot in front of it.
