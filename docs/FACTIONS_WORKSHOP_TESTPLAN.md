# Factions, territory and workshop sync: test plan (CoSync 0.3.0)

Everything in 0.3.0's faction/workshop feature is **untested in game**. This
is the plan for the first multiplayer passes. Each case lists what to do, what
every machine should show, and the log lines (`CoSyncUpdated_<pid>.log`) that
prove it. Run with 2 machines (A = host, B = client) and, for the three-way
cases, a third (C). The CoSyncMultiInstance rig works for A+B on one PC.

Design and verified engine addresses: `docs/WORKSHOP_FACTIONS_RE.md`.

## 0. Start-up (every machine, every test)

| Check | Expected |
|---|---|
| Load a save | `[Workshop] engine hooks installed: build/move/wire, scrap, store, repair, buildable area, budget` then `[WorldSync] Legacy cell-scan workshop sync DISABLED ...` |
| If instead `prologue mismatch ... Workshop authority DISABLED` | wrong exe version: stop, report the bytes in the line |
| Multiplayer > Workshop Settings (host) | "Workshop Protection: Active" |
| Host starts a session, B joins | host: `[Factions] TX state vN`, `[Workshop] joining conn=..: N registered object(s), M not yet in the host world sent`; B: `[Factions] RX state vN` |
| Versions | both 0.3.0 (network build 300); a 0.2.x peer is refused (VERSION MISMATCH) |

## 1. Factions

| # | Case | Steps | Expected on all machines |
|---|---|---|---|
| F1 | Create | A: Factions > Create Faction, name "Minutemen Reborn", pick emblem/colour, Found | A toast "Faction Minutemen Reborn founded."; A's Factions screen shows banner, rank "General · Leader"; B: Factions > All Factions lists it. Log A: `request 'create' ... -> OK` |
| F2 | Duplicate name | B: create "minutemen reborn" | Refused "That faction name is taken." |
| F3 | Invite | A: Invite Players > B | B toast "... invited you to join Minutemen Reborn"; B's Factions shows "Invitation: Minutemen Reborn" |
| F4 | Reject | B: open invitation > Decline | A toast "B declined ..."; invitation gone |
| F5 | Accept | A invites again; B: Accept | B toast "You joined ... as Recruit"; everyone: Members shows B as Recruit; HUD on A shows B's banner + "Recruit · Minutemen Reborn" |
| F6 | Promote | A: Members > B > New Rank = Soldier > Apply Rank | B toast "You were promoted to Soldier"; HUD updates on all |
| F7 | Demote | A: B > Recruit > Apply | B toast "You were demoted to Recruit" |
| F8 | Rank outranking | Make B Officer (can kick). B tries to kick A | No Kick item for A (A outranks B); a forged request is refused by the host: "You can only act on members ranked below you." |
| F9 | Kick | A: B > Kick (press twice) | B toast "You were removed from ..."; B no longer in Members on all machines |
| F10 | Ban | A invites B, B accepts, A: B > Ban (twice) | B removed + banned; A: Members > Banned Players lists B; A can no longer invite B ("banned ... Unban them first") |
| F11 | Unban | A: Banned Players > B (twice) | B can be invited again |
| F12 | Change rank name / perms | A: Ranks > Soldier: rename "Knight", toggle Scrap on, Save | All: rank shows "Knight"; HUD rank text changes |
| F13 | New / delete rank | A: New Rank "Paladin" priority 70; then delete it | Members holding it move to the lowest rank |
| F14 | Rename faction | A: Faction Settings > Rename | All menus and HUDs show the new name |
| F15 | Change banner | A: Banner > new emblem/colour > Apply | All: faction screens and HUD pennant change |
| F16 | Custom banner | Put `test.png` in A's and B's `Data\PrismaUI_F4\views\DoxCoSync\banners\custom`; A picks it | A and B see the image; C (no file) sees the fallback emblem |
| F17 | Leave (member) / leave (leader) | B leaves; later A (leader) leaves with B still in | B removed; when A leaves, B becomes leader (toast "... is now led by B") |
| F18 | Disband | Leader: Faction Settings > Disband (twice) | Faction gone everywhere; its settlements unclaimed |
| F19 | Persistence | Host saves, quits to menu, reloads | `[Factions] Co-save: wrote N faction(s)` then `loaded N`; factions, ranks, bans, territory intact |

## 2. Territory

| # | Case | Steps | Expected |
|---|---|---|---|
| T1 | Claim | A (General) stands in Sanctuary (near the workbench): Territory > name "Sanctuary Hills" > Claim | Toast "Sanctuary Hills is now Minutemen Reborn territory."; B's All Factions lists it; Factions > "Settlement Here: Sanctuary Hills / Controlled By Minutemen Reborn" |
| T2 | Claim from far away | Claim while not in a settlement | "Claim This Settlement" disabled ("Stand in a settlement ..."). A forged claim is refused: "You must be at the settlement to claim it." |
| T3 | Claim someone else's | B (other faction, Manage Territory) stands in Sanctuary | Claim disabled "Already held by Minutemen Reborn"; forged request refused |
| T4 | Release | A: Territory > Sanctuary Hills > Release (twice) | Unclaimed on all machines |
| T5 | Change ownership | A releases, B's faction claims | Owner changes everywhere |
| T6 | Enter friendly / enemy / no-faction territory | Each player stands in the claimed settlement and opens Factions | "You May Here" lists exactly what that player's rank allows; enemy and no-faction players see "Nothing" |

## 3. Building permissions (Sanctuary claimed by faction X)

Enter workshop mode at the claimed settlement for each case.

| # | Player | Action | Expected |
|---|---|---|---|
| B1 | X, rank with Build | place a wall | placed; appears on every machine at the same spot and rotation (see S1) |
| B2 | X, Recruit (no Build) | place a wall | the wall stays in hand ("cannot place"); toast "Your rank (Recruit) cannot build here."; log `local build refused` |
| B3 | other faction | place | refused: "Minutemen Reborn territory. Only its members can do that here." |
| B4 | no faction | place | refused, same message |
| B5 | X with Scrap | scrap an object | scrapped everywhere; components credited to the host's workbench |
| B6 | X without Scrap | scrap / store | nothing happens; toast "Your rank ... cannot scrap or store objects here." |
| B7 | other faction / no faction | scrap / store | refused |
| B8 | X without Move | pick up an object and drop it | the drop is refused (object stays held); cancel puts it back; nothing changes on other machines |
| B9 | X with Move | move an object | same final position/rotation everywhere |
| B10 | X without Repair | repair a damaged object | refused |
| B12 | client in a session | build a turret | refused: "Turrets, robots and other actors can only be built by the host in multiplayer." |
| B11 | Forged request (host-side proof) | from B with a no-permission rank, run a debug build without the local gate, or change rank on the host between B's local check and the request | host log `[Workshop] DENIED P from conn=...`; B's predicted object is removed (WKN); nobody else ever sees it |

## 4. Synchronization

| # | Case | Expected |
|---|---|---|
| S1 | A builds | B and C see the object at the same position/rotation. A log `host built object N`; B/C `placed object N base=...` |
| S2 | B builds (client) | host log `accepted build N ... from conn=..`, then `placed object N ... as 0x..` when the host is near; C sees it; B's own copy is bound (no duplicate) |
| S3 | A destroys/scraps | removed on B and C (`WKX`) |
| S4 | B scraps an object A built | removed on A and C; host workbench gets the components |
| S5 | B moves an object | all machines show the same final position; a rotated object is replaced by a correctly-rotated copy (log shows a new form on receivers) |
| S6 | Late join | A builds 20 objects, saves, B joins afterwards | B sees all 20 (they are in the save B loads); `joining conn=..: 20 registered object(s), 0 not yet in the host world sent` |
| S7 | Build while the host is far away | host stays in Diamond City; B builds in Sanctuary | accepted (host log), not yet placed on the host; host travels to Sanctuary -> `placed object N` and the settlement counts it; a player joining in between also gets it (`1 not yet in the host world sent`) |
| S8 | Persistence of the registry | host saves after S7, reloads | `[Workshop] Co-save: wrote/loaded N settlement object(s)`; build counts intact |

## 5. Build limits and world restrictions (host: Workshop Settings)

| # | Setting | Expected |
|---|---|---|
| L1 | Fallout 4 Default | vanilla size bar; "can build no more" at the vanilla limit |
| L2 | Custom, limit 3 | after 3 CoSync-built objects in one settlement: "Build limit reached for this settlement (3 / 3)." on every player (local) and at the host (forged); Territory > settlement shows "Objects Built 3 / 3" |
| L3 | Unlimited | size bar never fills; building continues past the vanilla limit |
| W1 | World Build Limits = Removed | can enter workshop mode and build outside the green border (object belongs to the nearest settlement) |
| W2 | Removed + faction territory | a non-member building next to, but outside, Minutemen territory is allowed; building inside the claimed settlement's real border is still refused (the check uses the engine's real border test) |
| U1 | Unclaimed = Claim Required | nobody can build in an unclaimed settlement: "This settlement is unclaimed. A faction must claim it ..." |

## 6. Regression checklist (existing systems must be unchanged)

- Player sync (EU/BONES), proxies, HUD vitals: teammates still move, animate and show HP/AP/rads.
- Debris / carried objects (OBJ), pickups (PU), furniture (FU): unchanged (only the WBF/WDS workshop scan was switched off).
- NPC sync, quests, doors, locks, containers, companions: unchanged.
- Join flow: world transfer, character restore, LooksMenu for a new character.
- Leaving and rejoining: B leaves (Factions state on B is cleared), rejoins, gets the state again.
- Solo play (no session): the workshop works normally; a solo player's own claims and settings still apply (they are the authority).
- Quit to desktop from the pause menu while the Multiplayer menu is closed: no crash.

## Known limitations (by design in 0.3.0; see the RE doc)

- Wires are gated but not replicated (spline data).
- Actor buildables (turrets, robots) are permission-gated; in a session only
  the host may build them (clients are refused with a message). The host's
  turrets reach clients through the save and the existing NPC sync, not as
  workshop objects.
- Component cost of a client's build is paid from that client's local
  workbench; the host's workbench is not debited. Items stored by a client
  are credited only in that client's local workshop.
- Repair on other machines clears destruction (`ClearDestruction`); vanilla
  resource-damage bookkeeping is only updated on the builder's machine.
- The custom limit counts objects built through CoSync (0.3.0+), not objects
  that existed before.
- Settler assignment / command mode is not covered by permissions.
- Territory names are typed by the claimer (no verified engine accessor for
  a settlement's display name was found).
