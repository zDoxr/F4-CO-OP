# CoSync 0.5.1 — ten-bug multiplayer stability pass

Date: 2026-10-05. Starting point: 0.5.0 (`17323f4`). Commits `fe58a73` … `8da1079`
(pushed to `coop`). Version 0.5.1 (network build 501).

**Evidence used:** your in-game test of 0.5.0 on 2026-10-05, 18:13–19:04:
`CoSyncUpdated_37552.log` = HOST (entity 1989621027) and `CoSyncUpdated_37964.log`
= CLIENT (entity 1627429019), plus IDA 9.3 on Fallout4.exe 1.11.240 and FormIDs
read directly from `Fallout4.esm`.

**Testing status, all bugs:** built (Release x64, 0 errors, 0 compiler warnings),
deployed by the post-build step, JS syntax-checked. **Nothing here has been run in
Fallout 4.** The test procedures are at the end of each section and in
`docs/TESTING_MATRIX.md` (section "0.5.1").

---

## Bug #1 — Revive / downed / respawn

**What happened (host):** down → "respawned" about 3 s later → the Downed UI stayed.
Host log: `[LocalPlayer] Down` at 18:53:47.539, then `Co-save: loaded` at
18:53:54 (a save **load**), then `RESPAWN detected … hp=85`, and 22 s later the
host's own revive clock declared the standing host dead
(`host: entity=1989621027's time ran out -- died`). It repeated nine times
(18:59:06 → load 18:59:12, …).

**Root cause (two engine facts + two state bugs):**

1. *The host was never really essential.* The engine decides "bleedout or death" from
   the ACTOR's own flag, `Actor+0x43C` bit `0x40000`. IDA: `sub_140C5DFF0` tests it
   right before the kill core `sub_140C61800`, and the `Actor.IsEssential` native
   `sub_141107A00` returns exactly that bit. That copy is rebuilt from the base ACBS
   flag (or an `ExtraEssentialProtected` override) only by `sub_140C5B2B0`, which the
   engine runs on load and from `Actor.SetEssential`. `SetLocalPlayerEssential`
   wrote only the base flag. The client loads the host's world after connecting, so
   its actor copy picked the flag up. The host never reloads, so it really died, and
   Fallout 4 auto-reloaded its last save. That reload also restored the save's
   (non-essential) flags, which is why every later down was a death as well.
2. *Downed state outlived the reload.* Nothing reset `CoSyncReviveSync`'s
   `s_localDownSince` on a load, and the "HP > 0" path never called `OnLocalUp()`.
   The HUD kept showing DOWNED.
3. *The host's own revive clock.* "Back on my feet" went out with `Send()`, which never
   loops back to the host, so the host's `s_hostDown` entry for itself ran out 30 s
   later.
4. *Client countdown:* the HUD showed "seconds until death" (30) and then started a
   second count (respawn, 6). Neither number was the time until respawn.

The same DMG feedback loop as Bug #8 is what downed both players so fast in your
test.

**Files:** `CoSync/CoSyncGameAPI.cpp/.h`, `Character/CoSynclocalplayer.cpp/.h`,
`Character/CoSyncReviveSync.cpp/.h`, `Character/CoSyncProfiles.cpp`,
`Core/CoSync_Plugin.cpp`, `GUI/CoSyncPrismaUI.cpp`, `views/DoxCoSync/hud.js`.

**Fix (one state machine for host and client):**
ACTIVE → DOWNED (engine bleedout; the host's clock) → REVIVED (host-granted) → ACTIVE,
or DOWNED → DIED (host's clock) → RESPAWN COUNTDOWN → RESPAWN AT THE LAST SAVE'S
POSITION → ACTIVE.
- `SetLocalPlayerEssential` calls the engine's `sub_140C5B2B0` after the base write
  (prologue-checked) and logs the live bit. During a session the bit is verified once
  a second and re-asserted. Under a script's override it backs off to 30 s and logs
  it. Player proxies get the same recompute.
- Any way back up (revive, respawn, the engine's own recovery, a save load) ends the
  local downed state, clears the HUD, tells everyone (`REVIVED|me|0`) and clears the
  host's own clock entry. A save load is handled through F4SE `PostLoadGame` →
  `CoSyncLocalPlayer::OnGameLoaded` (consumed by the next tick, not from the
  message thread).
- **Respawn at the most recent save:** the save is the host's shared world (clients
  never save). On every save the host records its own position and sends
  `SVPT|host|0`. Each client records where it stands. A load and the
  profile's restored position also set the point. Respawn moves the player there.
  Without a point the player respawns where they fell, and the log says so. *Design
  note:* I did not make a respawn literally load a save file. On the host that would
  reload the world under every client, and a client has no save of its own.
- HUD: one countdown to the actual respawn (`secs`), plus the remaining revive window
  (`rv`), for host and client alike.

**Why this fixes it:** the host now goes through the same bleedout as the client, so
the vanilla death reload no longer happens. If a load happens anyway, the state it
replaced is cleared.

**Could regress:** a quest that relies on the player dying during a session now gets
bleedout instead. That is intended, since CoSync already made the player essential
"for the session".

**Test:** see TESTING_MATRIX §0.5.1-1. Host alone: take lethal damage, then (a) stay
down 30 s + 6 s → you stand at your last save's spot, the HUD is gone, no reload,
and the log shows `RESPAWN detected` / `respawned at the last save's position`.
(b) Have the client revive you inside 30 s. (c) Load a save while down → the HUD
clears. Repeat everything with the client down.

---

## Bug #2 — Constant character flickering

**What I traced:** the remote player's lifecycle. There is **no**
spawn/destroy loop: the host's proxy on the client was spawned 4 times in 50
minutes, each after a host reload or a session restart (`SpawnTask Run START
entity=1989621027` 18:19:38, 18:33:59, 18:48:41, 18:58:32).

**What the logs do show, for the whole session on both machines:** the proxy's
OWN engine AI acting every frame. `[WeaponAuthority] refused N AI holster/draw
call(s)` averaged 497 per 10 s on the host (136 windows) and 1034 on the client.
The "last actor" is always a player proxy (`0xFF000DB8`, `0xFF0012AB`, `0xFF0014C2`…).

**Root cause:** player proxies were excluded from the 0.4.3/0.5.0 puppet gates. Each
one kept its own combat entry and its own package (sandbox/wander) while CoSync's tick
wrote the owner's position (SetPosition + controller velocity), pose (bones) and
weapon state back every frame. The result is two simulations of one character on
alternating frames.

**Files:** `World/CoSyncPuppetAuthority.cpp`.

**Fix:** the combat-entry hook refuses every fight a player proxy would start on its
own (NPCs can still fight the proxy, since that is the NPC's combat entry). The
package-chooser hook gives player proxies the same hold-in-place package as puppets
(scene/forced packages untouched). New diagnostic: `player proxies' own combat
refused=` in the `[PuppetAuthority]` line.

**Honest limits:** this is the per-frame fight with direct log evidence. I could not
watch the flicker. Two further candidates are documented under Remaining problems,
not changed:
1. `Pitch` arrives in degrees but is clamped as radians, so every sample is clamped.
2. A proxy skeleton cached while its 3D is partly loaded (`applied 25/93`).

**Could regress:** anything that relied on a player proxy being "in combat" locally.
Nothing in CoSync reads that.

**Test:** host + client standing still, walking, fighting NPCs side by side, in first
and third person. Watch each other for 2 min. The `[WeaponAuthority] refused` count
for player proxies should fall to near 0, and `player proxies' own combat refused=`
should appear when NPCs fight nearby.

---

## Bug #3 — Main-menu lobby

**Before:** hosting required a loaded save (the menu disabled HOST at the main menu).
The content check refused with `host_not_in_game`, and so did a world request.

**Fix (existing pieces, one new state):** the transport already starts before the
world, and `CoSyncNet::Init` already waits for a loaded world. Now:
- Hosting may start at the main menu. The content check runs there (plugins are
  loaded at the main menu).
- A world request the host can't serve yet gets `WORLDWAIT|lobby` and the client
  waits (phase *Lobby*), instead of `WORLDERR`.
- Host CONTINUE / LOAD: once its world has been loaded (and is past the intro) for
  3 s, everyone waiting gets it through the normal save + transfer path.
- Host NEW GAME: see Bug #4.
- A client outside the host's world neither takes the host's session broadcasts
  (receive gate in `CoSyncNet::OnReceive`) nor sends anything but the join protocol
  (send gate `CoSyncTransport::SetClientOutsideSession`) until the host's world is
  loaded.

State machine: MAIN MENU → LOBBY → (host starts) → WORLD DOWNLOAD/LOADING or INTRO →
PLAYER INITIALIZATION (profile, HELLO) → ACTIVE.

**Files:** `Networking/CoSyncWorldTransfer.cpp/.h`, `Networking/CoSyncContentSync.cpp`,
`Networking/CoSyncNet.cpp`, `Networking/CoSyncTransport.cpp/.h`,
`GUI/CoSyncPrismaUI.cpp`, `views/DoxCoSync/multiplayer.js`.

**Single player:** untouched. Nothing runs without a session.

**Test:** H at the main menu hosts; C at the main menu joins → C shows "In the lobby".
H chooses CONTINUE → C downloads and loads H's world with no input. Repeat with LOAD.

---

## Bug #4 — New Game intro, FaceGen, SPECIAL, name, identity

**Before:** a client always received the host's save. A client without a character
was moved into Vault 111's cryo room for CoSync's own editor (the "cryopod").

**Fix:**
- Host presses NEW (in the lobby): `WORLDGO|new` → every client waiting at its own
  main menu starts its own New Game through the main menu's own code object
  (`BGSCodeObj.StartNewGame()`). That is what the menu's New confirm calls
  (MainMenu.as `NEW_CONFIRM_STATE` → MainMenu callback 3, IDA `sub_141070050`). It
  then plays Fallout 4's intro: mirror FaceGen with sex, the Vault-Tec rep's SPECIAL
  and name, Vault 111.
- **Intro complete** = MQ101 (`0x1ED86`) completed, or MQ102 (`0x1CC2A`) started, or
  the player is in the Commonwealth (`0x3C`). The FormIDs were read from
  `Fallout4.esm`; the quest offsets are the ones CoSyncQuestSync already uses. The
  character then goes to the host as a finished profile: face, body, sex, SPECIAL,
  perks, items, position, and the character's name (`cname`, a display property).
  The client joins once the host is out of its intro too (`WORLDWAIT|intro`
  meanwhile).
- A new player joining a host already past the intro also plays the intro first.
  Only a player joining from inside a game of their own keeps the in-world editor.
- **Identity:** the network token is the SteamID64 (plus the CoSyncMultiInstance index
  for a second instance, the same `-P<n>` the profile key uses). The per-process salt
  made every launch a new entityID. It is now only behind
  `[Network] bLegacyPidIdentity=1`. Name, FaceGen, sex and SPECIAL never touch it.
  Your gbe_fork rig already gives each instance its own SteamID (…769250839 /
  …482304659).

**Files:** `Networking/CoSyncWorldTransfer.cpp/.h`, `Core/CoSyncMenuInject.cpp/.h`,
`Character/CoSyncProfiles.cpp/.h`, `Networking/CoSyncNet.cpp`.

**Could regress:** the MainMenu code-object call has not been seen in game. If the
menu never reports `IsMainMenuReady`, the client is told after 60 s to press NEW
itself (same flow afterwards).

**Test:** H and C at the main menu, C in the lobby. H presses NEW → C's game starts a
New Game by itself. Both make characters (change sex, face, SPECIAL, name). C leaves
Vault 111 first → "Waiting for the host". H leaves → C loads H's world with C's own
face/sex/SPECIAL/name, next to the vault exit. Check the log `Identity token=` is the
same after a restart.

---

## Bug #5 — Elevators / Skylift / cell transitions

**Evidence:** your test used Diamond City's public elevator: `DiamondCityExt`
(`0xE50C`) → `ElevTransPub` (`0xE8D80`, the ride cell) → `DiamondCityUpperStands`
(`0xFDA`). That is a LOAD elevator (`LoadElevatorPanel/Door/MasterScript`). These were
deliberately not replicated ("each player rides their own"), so each of you pressed
the panel/door/ride button separately, and one player could be left on the other side
of the cell transition.

**Fix:** a player using a load elevator's panel, car door or ride button sends
`ELEVL|eid|ref`. A machine whose player stands at that same elevator (interior: same
cell; exterior: same worldspace; within 450 units of the reference) activates the same
reference as its own player. Its engine runs the ride and the transition too, and both
arrive in the destination cell, where proxies are rebuilt as on any cell change. One
ride = one ELEVL each way (4 s per-machine window). In-cell moving elevators (`ELEV`)
now also reach a partner one exterior grid cell away.

**Files:** `World/CoSyncElevatorSync.cpp/.h`, `World/CoSyncDoorHook.cpp`,
`Networking/CoSyncFormIdMap.cpp`.

**Could regress:** standing next to an elevator someone else uses now takes you along.
That is the requested behaviour; 450 units is roughly "in the car / at the doors".

**Test:** Diamond City elevator to the Upper Stands together (both arrive). One player
far away → not taken. A moving elevator (e.g. a building's car) → both see it move.

---

## Bug #6 — Team-only vitals and names

**Before:** every player's `HP|` (health, max, level, rads, AP) went to everyone, and
the HUD cards and nameplates drew every player.

**Fix:** data first. A player's vitals now go only to that player's online teammates
(`CoSyncNet::SendMyVitals`; the host's relay filters by `CoSyncTeams`). No team =
nobody. The HUD cards and nameplates are drawn only for teammates
(`CoSyncTeams::AreTeammates`). This is built on the Team system; the removed
faction/territory code is not involved. NPC health is still world state.

**Files:** `World/CoSyncTeams.cpp/.h`, `Networking/CoSyncNet.cpp/.h`,
`Character/CoSynclocalplayer.cpp`, `GUI/CoSyncPrismaUI.cpp`, `GUI/CoSyncNameplates.cpp`.

**Still visible to everyone (deliberately):** event notices that ask for action ("X is
down!", the Revive prompt) and the session roster in the Multiplayer menu.

**Test:** 3 players, A+B in a team, C not. A sees B's card and nameplate, not C's.
C sees nobody's. A host-relay capture shows no `HP|` for A going to C.

---

## Bug #7 — Player interaction / Talk

**Root cause:** not the 0.5.0 player rule. `CoSyncDoorHook`'s older "CLIENT: no control
over dialogue" gate refused a client's activation of **every actor** (base formType 45).
Client log: `[DialogGate] [CLIENT] blocked activation` on each try. Clients lost NPC
Talk, corpse looting and companion commands.

**Fix:** the gate is removed. The player rule (decided above it, unchanged) identifies
another player's proxy as a CoSync **Player** entity (not an NPC). It shows Pickpocket
while sneaking and Revive while downed, never Talk (`ExtraCanTalkToPlayer=false`). Every
NPC goes through the engine's own activation. Quest stages a client's dialogue sets
reach the host as `QSREQ` (0.4.2). The host's conversations are still shared with
nearby clients.

**Files:** `World/CoSyncDoorHook.cpp`.

**Test:** C talks to Codsworth / a Diamond City merchant (normal dialogue and barter).
C activates H without sneaking → nothing; sneaking → Pickpocket. C loots a corpse.

---

## Bug #8 — PVE default, PVP option, the instant health drain

**Root cause of the drain (proved from your logs):** a player proxy's health was
mirrored with `SetBase`. The hit that started it stayed on the proxy as a damage
modifier, so after every `HP|` packet the proxy read (new base − old damage), and
`TickDamageDetection` reported the same hit again. Client log 18:53:46–47: one 9.9
shot became `TX DMG 9.9` every ~165 ms (hp 80→70.1, 78.1→68.2, 68.2→58.3 …) until the
host went down. The host's NPC hits on the client did the same with 2.7.

**Fix:**
- *Authority:* a player's health is that player's machine's alone. Proxies mirror the
  owner's current value (damage-mode delta, stage 1). A hit seen on a proxy is
  reported once, and the proxy goes back to its value, so nothing local can re-report
  it.
- *Rule:* new `CoSyncSessionRules`. The host owns PVE/PVP (`[Session] bPVP`, default
  0 = PVE; "Player Damage: PVE/PVP" in the host's Multiplayer menu), sent as
  `RULES|0/1` on change and every 5 s.
- *PVE:* a hit by our player on another player's proxy is refused at `Actor::DoHitMe`
  before the engine applies anything (no damage, no stagger). `DMG` now names its
  attacker; player-on-player damage is neither sent nor applied in PVE.

**Files:** `Character/CoSyncHealthSync.cpp/.h`, `Networking/CoSyncSessionRules.cpp/.h`
(new), `Weapons/CoSyncCombatAuthority.cpp/.h`, `Networking/CoSyncNet.cpp`,
`CoSync/CoSyncRuntime.cpp`, `GUI/CoSyncPrismaUI.cpp`, `multiplayer.js`, `DoxCoSync.vcxproj`.

**Test:** PVE: shoot each other → no damage, log `hit(s) by my player on another
player refused (PVE)`. Switch to PVP: one shot = one `TX DMG` and one HP drop of the
shot's size, no repeats. An NPC shooting the client → normal damage.

---

## Bug #9 — The loading screen

**Before:** the vanilla LoadingMenu with CoSync's injected background texture. There
were also two loading flags that nothing drew any more, set by a poll in CoSync's
tick, which never saw a loading screen (no `loading menu state changed` line in your
logs). The tick is the F4SE task queue in the world-update job, and that does not run
while the engine loads.

**Fix:** one place decides (`CoSyncLoading`, `Core/CoSyncLoadingScreen.cpp`), one
view shows it: `DoxCoSync/loading.html`, your design unchanged plus a status line,
full screen above every other CoSync view.
- Every engine load: LoadingMenu's own vtable (`0x25B0458`) is patched at slot `0x10`
  (per-frame, the slot the MainMenu hook already uses) and slot 0 (destructor). That
  covers New Game, Continue, save loads, doors, interior/exterior, load elevators,
  fast travel and a cell-changing respawn.
- CoSync's own loads: the host's world downloading/loading on a joining client (with
  %), a respawn, and the lobby's New Game starting. Each expires by itself.
- Updates always go through PrismaUI's dispatcher. The engine's loading itself is
  untouched.

**Files:** `Core/CoSyncLoadingScreen.cpp/.h`, `GUI/CoSyncPrismaUI.cpp/.h`,
`Core/CoSync_Plugin.cpp`, `Core/CoSyncMenuInject.cpp`, `CoSync/CoSyncRuntime.cpp`,
`views/DoxCoSync/loading.html`.

**Could regress / unverified:** whether PrismaUI draws its views over the vanilla
loading menu has not been seen. If it doesn't, the vanilla loading screen shows as
before (nothing breaks).

**Test:** CONTINUE from the main menu, walk through a door, use the Diamond City
elevator, join from the main menu, respawn → the CoSync screen each time. Log:
`[Loading] LoadingMenu up` / `closed`.

---

## Bug #10 — Sharepanions

**Model (kept; it was already designed this way in Phase 7d):** the host's companions
are its real world actors (puppets everywhere else). A client that recruits gets its
own copy, made its companion through the vanilla Followers quest and registered with
the host as client-owned (`COMPOWN`). Everyone sees it following that client;
duplicates are fine; a client's companions are saved with its SteamID-keyed character.

**What failed around it:**
1. Clients could not recruit at all (Bug #7's gate).
2. Recruits were seen only through CoSyncQuest's Papyrus follower poll. Your session
   has no `NotifyCompanionRecruited` line on either machine (the host greeted Dogmeat
   at 18:49:37).
3. When an NPC left the host's area, the host handed it in place to the nearest
   client (0.4.4) with no companion check. A host follower lagging behind a door (or
   told to wait) would then run on a client whose follow package targets that client.

**Fix:** a native scan beside the Papyrus poll (teammates in the loaded area, every
2 s): a new teammate = recruited; one of our copies that is loaded but no longer a
teammate for 3 s = dismissed. One recruit becomes one copy however many detectors
report it. Companions (client-owned entities and the host's teammates) are never
handed to another player.

**Files:** `Character/CoSyncCompanions.cpp`, `World/CoSyncNpcWorldSync.cpp`.

**Test:** H recruits Dogmeat, C recruits Piper. Both see both; each commands only
their own; C leaves and rejoins → Piper comes back with C; H walks through a door
away from Dogmeat → Dogmeat is not handed to C.

---

## Architecture changes

- **Join state machine** (`CoSyncWorldTransfer`): new client phases `Lobby`, `Intro`,
  `WaitingHost`; messages `WORLDWAIT`, `WORLDGO`; `WORLDREQ` gains a ready flag.
- **Client send/receive gates while outside the host's world**
  (`CoSyncTransport::SetClientOutsideSession` + the `CoSyncNet::OnReceive` gate).
- **Host-owned session rules** (`CoSyncSessionRules`, `RULES`).
- **Owner-authoritative player health** (proxies never hold a local value; `DMG`
  carries the attacker).
- **Team-scoped vitals** (`CoSyncNet::SendMyVitals`).
- **One loading-screen decision point** (`CoSyncLoading`) with LoadingMenu vtable
  hooks.
- **Stable identity** = SteamID64 (+ multi-instance index).
- New wire messages: `SVPT`, `ELEVL`, `RULES`, `WORLDWAIT`, `WORLDGO`; `DMG` gains an
  attacker field.

## Remaining problems / not verified without running Fallout 4

- **Nothing was tested in game.** Every root cause above is from logs + code + IDA;
  every fix is built and reviewed, not played.
- Bug #2: if flicker remains, the two documented candidates are next:
  1. `Pitch` is clamped as radians although the graph value is degrees
     (`[PitchDiag]` shows −25…−5 held for minutes).
  2. The proxy bone cache can sit at 57–61 nodes (`applied 25/93`).
- Bug #4: the main-menu `StartNewGame` call through `BGSCodeObj` and the MQ101/MQ102
  "intro complete" test are unexercised. Players in the intro do not see each other
  (each plays their own copy of the intro; there is no shared pre-war world).
- Bug #5: moving elevators in exteriors are only found in the receiver's own cell
  scan (a car one grid cell away is not known there).
- Bug #9: PrismaUI drawing over the vanilla loading menu is unverified.
- Bug #10: a client's companion copy is a runtime reference. It returns on rejoin from
  the saved character, not from the host's save.
- Session-wide: the two `CoSync_crash_*.dmp` from your session were caught (the game
  kept running and the logs continue). They are not addressed in this pass.

## Build status

- Release x64, `/p:PlatformToolset=v145`, full **Rebuild**: **0 errors, 0 compiler
  warnings**. 3 warnings MSB8065 (pre-existing: the Papyrus compiler is not installed,
  so `CoSync*.pex` is not produced by the custom build step).
- Deployed by the post-build step (DLL, PDB, PrismaUI views incl. `loading.html`).
