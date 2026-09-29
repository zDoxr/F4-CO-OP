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

## 7. Scale (4P, 8P)

| # | Step | Expect |
|---|------|--------|
| 7.1 | 4 players in one exterior, 10 min | Everyone sees everyone; `[Transport] Last 10 s` shows no inbox eviction warnings |
| 7.2 | 8 players split across 3 locations | Rows 2.x hold for every pair |
| 7.3 | One player disconnects and rejoins | Proxy removed and recreated; their NPCs handed to others |

## Known limits (not failures)

- Voice has no left/right panning yet, and uses the Windows default devices.
- Overhead name / faction / level tags are not built yet (they need the camera projection reverse-engineered first).
