# CoSync testing matrix (0.3.2)

Everything since 0.3.0 was built and compiled but **not run in game**. Work
through this in order: each block assumes the ones above it passed. Record the
result (✅ / ❌ / ⚠️ and a note) and keep every player's `CoSyncUpdated_<pid>.log`
for failures.

Roles: **H** = host, **C1** / **C2** = clients. "4P / 8P" rows need that many
players (a Steam-emulator second instance counts). Default keys: chat `Y`,
voice mode `U`.

## 1. Session basics (2P: H + C1)

| # | Step | Expect | Log evidence |
|---|------|--------|--------------|
| 1.1 | H hosts, C1 joins from the Multiplayer menu | C1 loads H's world, both see each other | `[Transport] Last 10 s:` lines on both, rx EU > 0 |
| 1.2 | C1 opens the character editor on join | Face centred on screen (not off to the side) | editor camera centred |
| 1.3 | C1 opens the Pip-Boy after joining | Pip-Boy on the wrist and working | `Pip-Boy in the inventory and equipped` |
| 1.4 | Both walk together for 2 min | No rubber-banding / tether back to a spot | no respawn of the proxy at the join position |
| 1.5 | Log size after 10 min | Roughly a quarter of the 0.3.0 rate (was 45 MB / 28 min) | file size |

## 2. Locations: interior / exterior (2P)

| # | Step | Expect |
|---|------|--------|
| 2.1 | C1 enters a building, H stays outside | H no longer sees C1 outside; C1 doesn't see H inside |
| 2.2 | H follows C1 into the same interior | Both see each other in the interior, correct positions |
| 2.3 | C1 leaves while H stays inside | Same as 2.1 in reverse; no proxy stuck at the door |
| 2.4 | Players in two different interiors | Neither proxy appears in the other's cell |
| 2.5 | Fast travel by one player | Proxy disappears for the other and reappears correctly on return |

## 3. NPCs (2P, then 4P)

| # | Step | Expect |
|---|------|--------|
| 3.1 | Both near raiders; fight them | Same raiders on both screens; no "standing puppets" |
| 3.2 | C1 shoots an NPC that H owns | Damage lands; the NPC reacts on both screens |
| 3.3 | An owned NPC fires / reloads / swings a melee weapon | The other player sees the same actions |
| 3.4 | Kill an NPC | Corpse ends up at the same place on both (within ~4 s) |
| 3.5 | H walks >12000 units away, C1 stays near NPCs | Ownership passes to C1 (host log `handoff`), NPCs keep fighting |
| 3.6 | NPC types: human, ghoul, super mutant, dog, deathclaw, robot, turret, creature (mirelurk / radroach) | Each type: existence, combat, death as above |
| 3.7 | C1 goes through the Vault 111 editor trip while H is in the world | No NPCs claimed or moved by C1 during it |
| 3.8 | 4P: two players fight the same group from different sides | Every player sees the same targets alive / dead |

## 4. Factions and workshop (see FACTIONS_WORKSHOP_TESTPLAN.md for detail)

| # | Step | Expect |
|---|------|--------|
| 4.1 | Create a faction with each new emblem (Minutemen, Brotherhood, Railroad, Institute) | Emblem draws in the banner preview and the teammate HUD |
| 4.2 | Two players in different factions, both on the HUD | Each banner keeps its own colours (they used to repaint each other) |
| 4.3 | Put `my banner.png`, `x.gif`, an empty `e.png`, a PNG renamed `p.jpg`, and a copy of a good image in `banners/custom` | Banner screen lists each under "Unusable Images" with the matching reason |
| 4.4 | Claim territory, build / move / scrap inside and outside it | As in the workshop test plan |

## 5. Text chat (2P, then 3P with two factions)

| # | Step | Expect |
|---|------|--------|
| 5.1 | `Y`, type, Enter | Message appears for everyone |
| 5.2 | Tab to FACTION and send | Only members of the sender's faction receive it |
| 5.3 | Send 6 messages in a few seconds | "You are sending messages too fast." |
| 5.4 | Emoji / accented text | Shown correctly |
| 5.5 | `Y` while the Pip-Boy or a menu is open | Chat does not open |

## 6. Voice (2P, then 3P with two factions)

| # | Step | Expect | Log evidence |
|---|------|--------|--------------|
| 6.1 | Session starts | HUD shows "PROXIMITY VOICE · OPEN MIC" (or NO MICROPHONE) | `[Voice] Microphone open`, `[Voice] Playback ready` |
| 6.2 | C1 talks next to H | H hears C1; C1's HUD indicator lights; H's HUD shows the speaker mark on C1; chat box shows "▶ C1" | H: `[Voice] Hearing entity` |
| 6.3 | C1 walks away while talking | Volume fades; silent beyond ~3000 units |  |
| 6.4 | C1 goes into an interior, H stays outside | H hears nothing |  |
| 6.5 | Both in one faction, C1 presses `U` (FACTION) and talks from far away | H hears C1 at full volume |  |
| 6.6 | 3P: C2 is not in C1's faction, C1 in FACTION mode | C2 hears nothing and sees no talking mark for C1 |  |
| 6.7 | Not in a faction, press `U` | Stays PROXIMITY |  |
| 6.8 | `iPushToTalkKey=86` in CoSync.ini | Only transmits while V is held |  |
| 6.9 | Background noise opens the mic | Raise `fThreshold`, retest |  |
| 6.10 | 4P / 8P: everyone talks at once | Audio stays clear; no growing delay |  |

## 6b. Overhead nameplates (2P, then 3P)

| # | Step | Expect |
|---|------|--------|
| 6b.1 | Stand near C1 | Name, LVL, and rank · faction (if any) float above C1's head and follow it smoothly |
| 6b.2 | Walk away | Tag shrinks and fades; gone beyond ~3500 units |
| 6b.3 | Turn away from C1 | Tag disappears (no mirrored tag behind you) |
| 6b.4 | C1 in another interior / exterior | No tag |
| 6b.5 | Open the Pip-Boy, pause menu, Multiplayer menu | Tags hidden, back when closed |
| 6b.6 | C1 talks on voice | Speaker mark on C1's tag |
| 6b.7 | Ultrawide (21:9) screen | Tag stays centred over the head (x is not rescaled) |
| 6b.8 | C1 behind a wall | Tag still shows (known limit: no line-of-sight check yet) |

## 7. Scale (4P, 8P)

| # | Step | Expect |
|---|------|--------|
| 7.1 | 4 players in one exterior, 10 min | Everyone sees everyone; `[Transport] Last 10 s` shows no inbox eviction warnings |
| 7.2 | 8 players split across 3 locations | Rows 2.x hold for every pair |
| 7.3 | One player disconnects and rejoins | Proxy removed and recreated; their NPCs handed to others |

## Known limits (not failures)

- Voice has no left/right panning yet, and uses the Windows default devices.
- Nameplates are not hidden behind walls yet (no verified engine line-of-sight call).

---

# 0.5.1 — ten-bug stability pass (built, NOT yet run in game)

Report: `docs/MP_STABILITY_051_REPORT.md`. Logs: `CoSyncUpdated_<pid>.log` from every player.
Setup: gbe_fork two-instance rig or two PCs. Keep `[Session] bPVP=0` unless a row says PVP.

## 0.5.1-1 Host only

| # | Step | Expect | Log evidence |
|---|------|--------|--------------|
| 1 | Main menu → Multiplayer → Host on Local Network | Hosting starts at the main menu ("Lobby open") | `[GNS] Host` |
| 2 | NEW GAME; play to the bathroom mirror | Normal intro, nothing CoSync-specific | no `held back` / no errors |
| 3 | Save (any save) | — | `respawn point (game saved)` |
| 4 | Take lethal damage | Bleedout, NOT death; no save reload | `set local player essential=1 ... actor essential now=1`, `[LocalPlayer] Down` |
| 5 | Stay down | One countdown (revive window + respawn) | HUD `DOWNED`, then `YOU DIED` |
| 6 | Wait for 0 | Stand up at the step-3 spot, HUD gone, full health | `respawned at the last save's position` |
| 7 | Go down again, then load a save while down | HUD cleared after the load | `Save loaded -- downed/respawn state cleared` |

## 0.5.1-2 Host + client

| # | Step | Expect | Log evidence |
|---|------|--------|--------------|
| 1 | H hosts at main menu; C joins from main menu | C: "In the lobby" | C: `In the host's lobby -- the host is at the main menu` |
| 2 | H chooses NEW | C's game starts a New Game by itself | C: `Lobby: started a New Game from the main menu (ok)` |
| 3 | Both: mirror (change sex + face), Codsworth, Vault-Tec rep (SPECIAL + name) | Each gets the full character creation | — |
| 4 | Cryopod | Each in their own intro; cryopod only after the walk to Vault 111 | — |
| 5 | C exits Vault 111 first | C: "Waiting for the host" | C: `My intro is complete`, `Uploaded the character from my own intro` |
| 6 | H exits Vault 111 | C loads H's world with C's own face/sex/SPECIAL/name | H: `The world is ready -- sending it`; C: `Character name restored` |
| 7 | Restart both games, rejoin | Same `Identity token` as before, same character, no duplicate player | `Identity token=` identical |
| 8 | C goes down, H revives with a Stimpak | Revive animation, C up at 50 % | `revive of entity=... confirmed by the host` |
| 9 | C goes down, no revive | C respawns at its last save point (H's last save) | C: `respawned at the last save's position` |
| 10 | H goes down, C revives | Same as 8 with roles swapped | H: `Revived by entity=` |
| 11 | H goes down, no revive | Same as 9; no H save reload | no `Co-save: loaded` on H |
| 12 | Walk side by side 2 min, first & third person, NPC fight nearby | No flicker of either character | `[WeaponAuthority] refused` near 0 for player proxies; `player proxies' own combat refused=` |
| 13 | Door into an interior and back out together | Both arrive; proxies reappear | `Local transition -- rebuilding proxy` |
| 14 | Diamond City public elevator (Skylift) to the Upper Stands, both in the car | Both ride and arrive together | `TX ELEVL` / `ELEVL applied: riding along` |
| 15 | Same, partner far away | Partner not taken | `ELEVL ... not at that elevator` |
| 16 | H recruits Dogmeat, C recruits Piper | Both see both companions; each commands only their own | C: `Recruited base=... spawning my own copy`, H: `Companion entity=... owned by` |
| 17 | H walks through a door away from Dogmeat | Dogmeat is not handed to C | no `takes it over in place` for Dogmeat's entity |
| 18 | C activates H without sneaking / sneaking | nothing / Pickpocket | — |
| 19 | C talks to an NPC (Codsworth, a merchant) | Normal dialogue / barter on C | no `[DialogGate]` |
| 20 | Not in a team: look at each other | No HUD card, no nameplate | — |
| 21 | Same team | Card (HP/AP/rads) + nameplate | H relays `HP|` to C only now |
| 22 | PVE: shoot each other | No damage | `hit(s) by my player on another player refused (PVE)` |
| 23 | H menu: Player Damage → PVP; shoot once | One drop the size of the shot, no drain | one `TX DMG ... attacker=` per hit |
| 24 | Any load (CONTINUE, door, elevator, join, respawn) | The CoSync loading screen | `[Loading] LoadingMenu up` / `closed` |

---

# 0.5.6 — Vault 111 intro, cryopods, Pip-Boy, elevator (built, NOT yet run in game)

Report: `docs/VAULT111_INTRO_REPORT.md`. Every machine logs its phase: `[CoSyncVault111] phase: ...`.
Pass = every row on both logs. Repeat each scenario at least twice.

## 0.5.6-1 Host only (New Game)

| # | Step | Expect | Log evidence |
|---|------|--------|--------------|
| 1 | New Game, play to the lift | Vanilla | `phase: PreWar` |
| 2 | Into the vault, suit, pod, freeze, wake | Vanilla, untouched; all residents in their pods | `Intro detected -- Vanilla Cryopod sequence started`, `Player assigned to Cryopod ... pod 0`, six `Cryopod left empty` |
| 3 | Control back | Not moved (host) | `Vanilla Cryopod sequence completed ... Reapplying`, `phase: VaultExplore` |
| 4 | Skeleton Pip-Boy, console, gear door | Vanilla | `Pip-Boy event completed (the host's own)`, `Elevator gate state changed` |
| 5 | Bridge, platform, menus, ride up | Vanilla; outside | `lowered to the bridge`, `rising with this player`, `left Vault 111` |

## 0.5.6-2 Host + 1 client

| # | Step | Expect | Log evidence |
|---|------|--------|--------------|
| 1 | Shared New Game to the vault (0.5.3 flow) | Both in the vault | `INTRO|vault` |
| 2 | Pods | H in the scripted pod, C in pod 1 (`0xCA2B4`) on H's screen; on C's screen C in the scripted pod and H in pod 1; pods 2–6 have their vanilla resident | both: `Player assigned to Cryopod` with the same EntityID→pod list; `resident ... left a player's cryopod` once |
| 3 | Freeze / Kellogg / wake | Each machine's own cutscene, no snapping | no `QS apply` for MQ101 > 600 on C; C: `QS quest=0x0001ED86 stage=... not applied`; no NPC entity in `0xCA260` |
| 4 | Control back | C stands in front of pod 1, H at the scripted pod, both screens agree | C: `stood in front of my own cryopod (pod 1)`; both: `entity=... (stood at its pod)` |
| 5 | H takes the skeleton Pip-Boy first | C gets a Pip-Boy (if it has none) without being pulled anywhere | H: `Pip-Boy event completed`; C: `Pip-Boy fallback applied` |
| 6 | H plugs into the console | Only H sits; C's gear door opens too | C: no stage 7 applied; C: `QS ... stage=9` applied, `Elevator gate state changed` |
| 7 | H rides up first | C's elevator still comes down for C, C's platform still works | C: no `WSE` applied for `0x20C3A`/`0x8C94A`; C: `QS quest=0x0001CC2A stage=10 not applied`, `stage=15 deferred` |
| 8 | C rides up | C outside | C: `left Vault 111`, `deferred stage quest=0x0001CC2A stage=15 applied now` |

## 0.5.6-3 Host + 3 clients

| # | Step | Expect |
|---|------|--------|
| 1 | Pods | Four different pods, pods 4–6 vanilla, same EntityID→pod list on all four logs |
| 2 | Wake | Each client in front of its own pod; nobody overlaps |

## 0.5.6-4 Late join

| # | Step | Expect | Log evidence |
|---|------|--------|--------------|
| 1 | Join while H is in the pod sequence | Waits in the lobby | H: `joins wait in the lobby: the host is in the vanilla Vault 111 sequence` |
| 2 | H wakes | Joiner comes in next to H, with Pip-Boy + jumpsuit, no intro replay | `Inventory Initialization ... giving the Pip-Boy back`; joiner `phase: VaultExplore` |
| 3 | Join after H left the vault | Joiner outside with H, Pip-Boy | joiner `phase: Complete` |

## 0.5.6-5 / -6 Elevator from both sides

| # | Step | Expect |
|---|------|--------|
| 1 | C does the console and rides first, H later | H's gear door opens (C's QSREQ 9), H's platform still works; H defers C's stage 15+ |
| 2 | Both on the platform together | Each gets the menus and both ride up |

