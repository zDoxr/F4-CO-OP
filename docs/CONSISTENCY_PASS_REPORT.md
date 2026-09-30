# CoSync 0.3.6 — Bug Fix, Sync & Gameplay Consistency Pass

Date: 2026-09-29. Version 0.3.6 (network 306): the wire format gained
PAOWN, PADENY, WSPAWN, WSDESPAWN, WKW and WKQ|W, so peers must run the same build.

**Nothing in this pass has been tested in game.** Every change builds (Release x64, v145).
Each item below gives the evidence the fix is based on. Where a cause is inferred rather than
proven, the item says so.

Evidence sources: the 0.3.5 session logs `CoSyncUpdated_11680.log` (host) and
`CoSyncUpdated_44620.log` (client), `CoSync_hang.dmp` (21:10:35), and IDA on the 1.11.240 IDB.

Commits: 2e5fe51, 74ba88a, c61b44a, c674127, 1dbe0c0, 3a53eb4, plus the version/report commit.

---

## 5. Exterior -> Interior desync and the hang (highest priority)

### 5a. The host lost the client after exterior -> interior (root cause found)
Timeline in the host log (11680):
- 21:02:13.783: `Proxy entity=3338437275 HIDDEN: remote ws=0 vs local ws=0x3C` (Disable).
- 21:02:13.785: `PA graph still not ready 0.0s after PA entry -- retrying RefreshActor3D`. That call is
  Disable+Enable, so it **re-enabled the proxy that had just been hidden**, at coordinates from another worldspace.
- 21:02:13.839: `actor ... freed (unloaded) — clearing`. The proxy was dropped, and its PA chassis was deleted.
- 21:02:35 onwards: both players were in the same interior (cell 97783), yet the host logged
  `No spawned proxy for sender=3338437275` until **21:09:03**.

Why the proxy never came back: `SpawnWatchdog` range-gates the respawn on `authoritativePos`, and
`authoritativePos` is written only at CREATE (`CoSyncPlayerManager.cpp:476`). A player's proxy is never
re-CREATEd. So the watchdog compared the client's old exterior coordinates with the host's interior
position (about 95,000 units, gate 12,288) and postponed the respawn indefinitely. The proxy only came
back once the host walked back near the original exterior spot.

Fixes:
- `CoSyncPlayerManager::SpawnWatchdog` gates on the newest EU position (`st.lastUpdate`, which is stored before any spawn check).
- `CoSyncPlayer::TickAnimVars` never runs 3D retries on a proxy WorldSync has hidden. The PA retry also
  requires the not-ready state to have lasted the settle window. Before, a stale timer made it fire on the first
  frame after every loading screen.
- `CoSyncWorldSync::TickProxyCellVisibility` decides visibility **before** the local-transition rebuild. Before,
  every proxy got Disable+Enable and was then disabled again in the same frame.

### 5b. The hang (`CoSync_hang.dmp`)
- The hung thread is 12712, the window/main thread (`[WindowHook] Installed ... (thread 12712)`). It is **busy, not blocked**.
- Stack (IDA): `sub_140C339D0` (frame) -> `sub_140C33250` (render, uses the "CopyMainToMenuBG" string) ->
  `sub_140C38240` -> ... -> `sub_14221F620`. RIP is 0x14221F783, inside the loop at 0x14221F780..798:
  `for (p = head; p; p = *(p+0x40)) if (!*p) sub_14221FCC0(p);`. This walks a render pass list, and its
  `+0x40` chain has become a **cycle**.
- The CoSync tick is not running concurrently with it. The tick runs from F4SE's task hook `sub_140C45150`, which
  is called by the job "NonRendersafeAI" (`sub_140BD6170`, engine job table 0x2F30B00 index 62) and after the
  render in `sub_140C339D0`. Every worker thread is idle in the dump. So this is list corruption, not a live race.
- The last client tick before the freeze (21:09:03.617) did, in one frame: Disable+Enable+RefreshActor3D on
  the host's proxy **while it sat in power armor**, unbinding of 8 NPC proxies, and the queued delete of one
  NPC clone. The same Disable/Enable stacking (see 5a) happened after every load in this session.
  **Inferred, not proven:** repeated same-frame disable/enable cycles on a PA-merged actor corrupted a
  render pass list.
- The dump holds no heap (`MiniDumpWithDataSegs`), so the objects in the cycle could not be named. Next time:
  the watchdog now suspends the main thread and, if it is inside `sub_14221F620`, walks the chain from RBX and
  logs the geometry names and parent chain in the cycle. Dumps now also include
  `MiniDumpWithIndirectlyReferencedMemory`. The hang is **not suppressed**. The stacked disable/enable cycles
  (the likely trigger) are removed by the 5a fixes.

## 1. Power Armor paint (root cause found)
- The host's OUTFIT packet contains the same six PA piece IDs as its PA packet
  (0x154ABD/0x154AC1/0x154AC4/0x154AC5/0x154AC6/0x154AC8).
- OutfitSync injected them into the proxy as normal armor, and injected a new copy on every rebuild
  (it injected even when the piece was present but unequipped). ItemMods painted those copies (log 20:58:31).
- PA entry then injected **bare** copies into the spawned chassis, and `SwitchToPowerArmor` moved them onto
  the proxy. The proxy now held two of each piece.
- IDA: the AttachModToInventoryItem worker `sub_141180DE0` refuses count > 1 ("Can only mod singular items").
  No PA piece mod attached after the 21:00:16 entry. The copy from the local frame copy found nothing, because
  the sender had already taken the pieces out of that frame.
- Fixes:
  - OutfitSync leaves the sender's PA pieces to PASync (`IsSenderPowerArmorPiece`) and equips a carried copy instead of injecting another.
  - PA entry removes stray copies from the proxy, then paints the chassis pieces from the **sender's own IMODS**
    (the mods on the pieces it wears) before `SwitchToPowerArmor`.
  - IDA: `sub_141181E20` re-equips an equipped actor item after a mod (ActorEquipManager 0xCE62C0 / 0xCE5E20),
    so later IMODS changes also refresh the model.
- Weapons worked because a weapon is never delivered twice.

## 2. Save detection / new players
- The existing detection is CoSync's profile. It lives in the host world's co-save (record `PROF`, keyed by
  SteamID), and the client reads it when it loads the transferred world. Logs show it working
  (`returning` -> `Restoring 'gse orca' level 2 ... 26 items`). It was not replaced, and no second save system was added.
- The one "new" misfire in the logs (23:45) was a **different SteamID key** (76561198482304659 vs
  76561199769250839-P2). That is an identity change under the emulator, not a missed save.
- New-player bug: `ApplyFresh` wiped the inventory with engine RemoveAllItems mode 0 and gave back only the
  Pip-Boy. The Vault 111 jumpsuit (0x1EED7) was declared for this purpose and never given. It is now added and
  equipped. Both the Pip-Boy and the jumpsuit are re-checked when the player leaves the face editor.
  The join decision is logged as `Player Join: key=... save FOUND/NOT FOUND`.

## 3. Duplicate overhead names
- Traced: there is one plate view per process ("Created nameplates view" once per log), the script initializes
  once, entities are created once per player and destroyed on disconnect, and the page keys plates by entity ID.
- **Probable cause, not proven:** on an empty list the plate view was Hide()n right after being sent "[]",
  before it could repaint. The hidden view kept its last frame, and Show() brought back plates at old positions.
- Changes:
  - The view is no longer hidden; the empty list clears it.
  - C++ emits one plate per proxy actor and never one for the local player.
  - The page removes any plate element it does not track.
  If ghosts still appear, a screenshot plus the log's `[PrismaUI:js]` lines are needed.

## 4. Wires
- Before: host wires were not replicated, and client wires were refused ("Wires are not shared in multiplayer yet.").
- Now: a wire is sent as a spline base plus two endpoint IDs, read from the wire's `ExtraPowerLinks`.
  Endpoint IDs are host net IDs or save form IDs.
- The rebuild uses the engine sequence F4SE 0.7.9's `ObjectReference.AttachWire` uses. Every RVA was checked as a
  function start in IDA: ContextData 0x3897F0, UpdateSpline 0x38DB50, ConnectSpline 0x38DFE0,
  ExtraData AddItem 0x387090, AddConnection 0x3876D0, UpdateMovingWirelessItem 0x38ECB0,
  EstablishTerminalLinks 0x38FB40, hCurrentWorkshop 0x30F7698.
- A client's wire becomes a `WKQ|W` request, permission-checked like a build. The host builds it and broadcasts
  `WKW`; the placing client binds its own wire. Other machines rebuild it once both endpoints have 3D.
- Log lines: `Wire Created`, `Wire Endpoint A/B`, `Wire Synchronized`.

## 7-9. Power Armor behaviour and ownership
- Entry and exit already run the engine's own functions on the proxy (native SwitchToPowerArmor /
  SwitchFromPowerArmor), so the animations play and the frame stays at the exit point.
- New: **ownership**. The host is the authority: the first PA enter it sees for an unowned frame wins.
  - `PAOWN` is broadcast, and re-sent every 10 s for late joiners.
  - A player who lost a simultaneous entry gets `PADENY` and is ejected.
  - Non-owners are refused at `ActivateWorker` with the HUD message "This Power Armor belongs to another player."
    Everyone still sees the frame, the occupant, the entry and the exit.
  - Ownership follows the frame to where its owner got out.
  - Ownership lasts for the session and is released 30 s after the owner leaves.
  - Log lines: `PA Owner Assigned`, `PA Exit Completed`, `PA Enter refused`, `PA entry DENIED`.

## 10. Host spawned objects (placeatme)
- IDA call tree: console PlaceAtMe entry 0x142EEACB0 -> `sub_1405C5110` -> `sub_1405E0900` -> `sub_1405E0B30` ->
  CreateReferenceAtLocation `sub_1402E5110`.
- CreateReferenceAtLocation has about 30 callers: workshop, drops, the PA exit, CoSync's own spawns, and Papyrus
  PlaceAtMe (`sub_141159E80` -> `sub_141181600`, which runs on every machine running the script). Replicating that
  level would duplicate objects. `sub_1405E0900` is only reached from console commands.
- It is now hooked on the host. `WSPAWN`/`WSDESPAWN` are re-sent every 10 s, and clients create the object once
  they are in the same location.
- Actors are left to NpcWorldSync.

## 6. NPC behaviour (sitting, leaning, smoking, weapon idles)
**Not changed in this pass.** It needs its own RE investigation of furniture/idle/package state,
and nothing was changed here without one. Directive 4/5 work on NPC graph inputs and weapon authority stands.

## 11. Logging
New transition lines are listed with each item above. There is no per-frame spam: the lines fire on
state changes, and the resends are 10 s.

## 12. Testing
Build only. No in-game test was possible from here.

Suggested order: exterior->interior->exterior with both players (watch for `SpawnWatchdog ... Entity Recovered`
and no `HIDDEN` followed by `PA graph` lines), PA paint (enter a painted frame and look from the other machine),
PA ownership (second player activates an owned frame), placeatme of a static/furniture, a wire between a
generator and a light (host and client builder), and a fresh join (Pip-Boy + jumpsuit).

## Known limitations
- The hang's exact corrupt object is unidentified until the new probe fires.
- A client's own console placeatme stays local.
- PA ownership is not saved across sessions.
- A scrapped wire is removed with the object delete path, not the engine's wire scrap, so endpoint power links may
  need re-wiring after a remote scrap.
- NPC contextual animation sync is untouched (item 6).
