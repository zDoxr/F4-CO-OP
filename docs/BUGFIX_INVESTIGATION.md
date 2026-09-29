# CoSync bug-fix / sync / networking / UI directive — investigation report

Date: 2026-09-28. Build under investigation: 0.3.0 (commit 6cac732).
Evidence: the source tree, the 0.3.0 session logs `CoSyncUpdated_38668.log`
(host) and `CoSyncUpdated_2272.log` (client), and IDA 9.3 decompilation of
Fallout4.exe 1.11.240 (canonical IDB, worked on a copy).

Labels: **PROVEN** (code + log or code + IDA), **VERIFIED** (engine behaviour
confirmed in the binary), **UNVERIFIED** (not proven yet; nothing is built on it).

No code has been changed for this report.

---

## 1. Current architecture (from the code)

```
Game (engine job threads: CoSync tick = F4SE task "NonRendersafeAI")
  CoSyncRuntime tick ──► CoSyncFactions::Tick / CoSyncWorkshopAuthority::Tick
                    └──► CoSyncNet::Tick (only while a session exists)
                           ├─ CoSyncTransport::Tick → DrainInbox → CoSyncNet::OnReceive (text messages, prefix-dispatched)
                           ├─ PlayerManager::Tick   (entity states, proxies, interpolation, anim vars, combat toggles)
                           ├─ NpcWorldSync           (scan, NPCSEEN claims, bind/puppet, cleanup)
                           ├─ WorldSync               (host WS location, debris OBJ, pickups, furniture)
                           └─ Flush* queues          (quests, PA, VATS, outfits, equips, doors, locks, dialog, health, loot...)
Network thread: GNS_Session (GameNetworkingSockets or Steam sockets) → Transport inbox (mutex)
Window thread:  WndProc subclass + PrismaUI (menu/HUD) → commands queued to the tick
```

* Transport: text messages over GNS, reliable by default; BONES unreliable.
  Star topology; the host relays.
* Identity: players = SteamID-derived entityID (salted instance token on LAN);
  profile key for persistence; host binds each connection to one entity (P5/N7).
* Players: `EU|` at ~15 Hz (pos/rot/vel/anim vars/weapon), `BONES|` pose stream,
  proxies = `RMPlayer*` NPCs positioned by raw NiNode write + periodic cell resync.
* NPCs: **"first reporter owns"** per NPC (`CoSyncNpcWorldSync`), host
  arbitrates duplicates by ref and by a quantized position key. The owner runs
  the real AI and streams `EU|` (+ combat target, weapon, `AEV|` fire/reload/throw);
  every other machine (host included) disables its own copy and steers a
  "puppet" clone that keeps its engine AI alive. No ownership handoff.
* Location: `WS|` (worldspace + cell) — **sent by the host only** (see K5).
* UI: PrismaUI views (multiplayer menu, teammate HUD), game-thread-safe via
  PrismaUI's dispatcher.
* Shared world: joining clients load the host's save; the host co-save holds
  profiles (`PROF`), factions (`FACT`), workshop registry (`WSHP`).

---

## 2. Known bugs — root causes

### K1 — FaceGen character appears too far left

```
Bug:        In the character editor the character sits off-centre (left).
Root Cause: CoSync's editor staging (CoSyncProfiles FrameFaceCamera) forces the
            third-person camera and swings it round to face the character. The
            third-person camera keeps an over-the-shoulder offset in CAMERA
            space, so after the swing that offset shifts the view sideways and
            the character is off-centre. PrismaUI is not involved (the
            LooksMenu is vanilla Scaleform).
Affected Files: Character/CoSyncProfiles.cpp
Affected Functions: FrameFaceCamera, RestoreCamera, EditorStep
Evidence:   VERIFIED in IDA (1.11.240), ThirdPersonState (vtable 0x2522A88):
            * +0x60 (x,y,z) = target shoulder offset. Constructor 0x102C7B0 writes
              (fOverShoulderPosX, 0, fOverShoulderPosZ) (GameSetting values at
              0x2F39498 / 0x2F394B0).
            * Slot 17 0x102DAD0 (combat toggle) rewrites +0x60 from the combat
              settings (fOverShoulderCombatPosX 0x2F394C8 ...) or back to the
              normal ones.
            * Slot 11 0x102CD50 (per-frame update) rewrites +0x60 on the
              weapon-drawn edge; slots 14-16 (0x102E440/0x102E5E0/0x102E6F0)
              read +0x60 and copy it into the current offset +0x6C.
Proposed Fix: While the editor owns the camera, zero the X component of +0x60
            and +0x6C each editor tick (world units, so resolution/ultrawide
            independent); save and restore the originals on exit.
Risk:       Low (two floats in a structure already written by this code path).
```

### K2 — Joined players have no Pip-Boy (inventory opens, arm without Pip-Boy)

```
Bug:        After joining, the Pip-Boy is missing.
Root Cause: The fresh-character wipe (ReplaceInventory) calls RemoveAllItems in
            mode 0, which removes EVERYTHING including the Pip-Boy (the comment
            "Pip-Boy stays" is wrong since the mode-0 change). It is then "given
            back" with a queued Papyrus AddItem + EquipItem using form arguments,
            the call class already documented in this codebase as silently
            failing (the reason NativeAddItem exists). The Pip-Boy never returns.
            CoSync does not skip Vault 111 progression -- the joiner is in the
            HOST's world, where MQ102 is already done; only the item is lost.
Affected Files: Character/CoSyncProfiles.cpp
Affected Functions: ReplaceInventory, ApplyFresh, ApplyProfile
Evidence:   PROVEN. Client log 2272: "Inventory wiped: 0 item(s) left -- giving
            the Pip-Boy back", then every upload reports "level 1, 0 items" until
            the player picked items up; CaptureInventory does not filter the
            Pip-Boy (0x21B3B), so it was never in the inventory.
Proposed Fix: Keep the Pip-Boy through the wipe (re-add with NativeAddItem, the
            verified engine add, and equip with the verified direct EquipObject
            path used by CoSyncEquipHook), then verify it is equipped.
            UNVERIFIED until tested: that the equipped Pip-Boy item alone restores
            the first-person model/menus (vanilla MQ102 does the same thing: it
            adds and equips the item).
Risk:       Low.
```

### K3 — NPC sync / combat / animation drift (phases 3-5, 7)

```
Bug:        NPCs behave differently on different machines; combat drifts.
Root Cause: Three proven defects in the puppet path:
  (a) Combat is turned on AND off: PlayerManager::Tick issues Papyrus
      StartCombat(target) on a puppet at the combat edge/target change, while
      CoSyncPlayer::ApplyProxyTransform calls StopActorCombat on every position
      update (20 Hz). Two systems fight over the same puppet.
  (b) The owner's discrete events (fire / reload / throw, AEV|) reach the
      puppet but are never played: they are replayed only in
      CoSyncPlayer::TickAnimVars, which PlayerManager deliberately skips for
      NPC puppets. (The counters also grow forever.)
  (c) With (a) and (b), a puppet's visible attacks come from whatever its own
      local AI does between StopActorCombat calls: independent simulation.
Affected Files: Character/CoSyncPlayer.cpp, Character/CoSyncPlayerManager.cpp,
            Networking/CoSyncNet.cpp (AEV)
Affected Functions: ApplyProxyTransform, PlayerManager::Tick (CombatToggle,
            TickAnimVars gate), CoSyncPlayer::TickAnimVars
Evidence:   PROVEN from code (the gate at CoSyncPlayerManager.cpp ~2153, the
            per-update StopActorCombat at CoSyncPlayer.cpp ~654, the only AEV
            consumer at CoSyncPlayer.cpp ~1677).
Proposed Fix: Puppets never run combat AI (keep StopActorCombat, remove the
            puppet StartCombat); drive their attack/reload/throw/weapon-draw
            visuals from the owner's replicated events on the puppet path
            (engine animation-graph events; the exact event names must be RE'd
            from the behaviour graphs -- UNVERIFIED). Owner stays the single
            authority for AI, target, health and death.
Risk:       Medium (touches the most-used NPC path; needs an in-game test).
```

### K4 — Death / ragdoll not identical

```
Bug:        Corpses differ between machines.
Root Cause: Death is replicated (KILL| -> KillPuppet: SetLifeState dying +
            KillDying, native), but after death the owner stops streaming
            ("Corpses are driven by ragdoll on every machine") and every machine
            ragdolls independently, so final corpse positions differ.
Affected Files: Character/CoSyncHealthSync.cpp, Character/CoSyncPlayerManager.cpp
Affected Functions: HostSendNpcUpdates_Impl (dead skip), OnReceiveKill/KillPuppet
Evidence:   PROVEN from code (the explicit dead skip in HostSendNpcUpdates_Impl).
Proposed Fix: Owner sends one authoritative corpse transform after its ragdoll
            comes to rest (and on late join); non-owners move their corpse to it.
            "Ragdoll at rest" detection = UNVERIFIED (engine RE needed); fallback
            is a fixed settle delay, marked as such.
Risk:       Low-medium.
```

### K5 — "Tether" / players disconnected from their real position; interiors

```
Bug:        Clients appear pulled back / teleported; players in other
            cells/interiors appear in the wrong place.
Root Cause: Location identity is one-sided:
  (a) Only the host sends WS| (worldspace + cell). Clients never do
      (SerializeWorldSync is called only from HostTickWorldSync), and the
      host's OnReceiveWorldSync would only relay it anyway. So the host (and
      every other client) never knows where a client is.
  (b) Every proxy relocation (ResyncActorCellMembership for players, SnapActor
      for NPC puppets) moves the proxy into the LOCAL player's own
      cell/worldspace at the remote coordinates.
  => When a client is in a different interior/worldspace, its proxy is placed
     at meaningless coordinates in the host's space, its cell unloads, the
     engine frees it, and the spawn watchdog re-creates it at the stale HELLO
     position: it "snaps back". Nothing moves the client's own player (the only
     local-player movers are the join teleport, the editor staging, the profile
     position restore and the F6 hotkey; none fired after join in the session).
Affected Files: World/CoSyncWorldSync.cpp, CoSync/CoSyncGameAPI.cpp,
            Character/CoSyncPlayer.cpp, Character/CoSyncPlayerManager.cpp
Affected Functions: HostTickWorldSync, OnReceiveWorldSync,
            ResyncActorCellMembership_Impl, SnapActor_Impl, SpawnWatchdog
Evidence:   PROVEN. Host log 38668: 0 WS| messages from the client all session;
            16:50:18.947 "entity=1200287479 ... freed (unloaded)" while the
            client stood in the Vault 111 editor room, then "SpawnWatchdog ...
            re-enqueuing spawn" with spawnPos = the HELLO position.
Proposed Fix: Every player sends its location (worldspace/cell) on change plus
            a slow keep-alive; the host records it per entity and relays it.
            Proxies are shown only when in the same worldspace (exterior) or the
            same cell (interior), relocated into the REMOTE player's location
            (not the local one), and hidden -- not freed/respawned -- otherwise.
            EU| stays small (location is its own event-driven message).
Risk:       Medium (proxy lifecycle), high value.
```

### K6 — Faction icons not recognizably Fallout
Current emblems are original generic shapes (banners.js). Proposal: redraw
emblems in the style of the four major factions (Minutemen, Brotherhood of
Steel, Railroad, Institute) plus the existing set, as SVG in banners.js (no
game assets copied). **Decision needed** (see §5).

### K7 — Custom banners should be easier
Already auto-discovered since 0.3.0: C++ scans
`Data\PrismaUI_F4\views\DoxCoSync\banners\custom\` every 5 s (png/jpg/jpeg/webp/svg),
the menu lists them, broken images fall back to the emblem. Gaps: files that
are rejected (spaces in the name, too long, wrong extension) are skipped
silently, and nothing tells the user why. Proposal: accept spaces (mapped to a
safe id), and show rejected files with the reason in the Banner screen.

### K8-K13 — Voice / text chat / talking indicator / overhead info
Not present today. Design in §5; one decision needed (voice codec/transport).
Overhead info needs a world-to-screen projection: PrismaUI has none, so the
game camera's projection must be reverse-engineered first (UNVERIFIED).

---

## 3. Unknown bugs found

| # | Bug | Evidence | Severity |
|---|---|---|---|
| U1 | **NPCSEEN carries no cell/worldspace.** In the Vault 111 editor room the client reported Vault 111's cryopod NPCs (refs 0x9F3F1, 0x1A54A3, 0x1A6613, 0xA1775 ...) with interior coordinates; the host created client-owned entities and disabled/puppeted its own copies in its world. These are exactly the refs of the unexplained hardening bug W1 ("15 host NPCs disabled"). | host log 16:50:18.941 "Created entity=65541 owner=1200287479 base=0x475DF ref=0x000A1775 pos=(-1608,1557,-116)" | High (corrupts the host world view) |
| U2 | NPC scanning keeps running while the player is in the editor room (a staging location, not the session). | same | High (cause of U1) |
| U3 | **My 0.3.0 territory claim cannot work for clients**: the host's proximity check uses the client proxy's location, which the host never has (K5a). | code (CoSyncFactions IsNearWorkshopImpl) + K5 | Medium |
| U4 | Puppet AEV counters accumulate forever (never drained on the puppet path). | K3(b) | Low |
| U5 | The spawn watchdog re-creates a freed player proxy at the HELLO position (stale), not the last known position. | host log 16:50:18.948 | Medium |
| U6 | docs/WORKSHOP_FACTIONS_RE.md wrongly says CoSyncWorkshopSync reads 1.11.221 workshop RVAs; the project compiles F4SE 0.7.9's GameWorkshop.cpp (1.11.240). Documentation error, no code impact. | DoxCoSync.vcxproj line 428 | Doc |
| U7 | The legacy `WD|`/`WM|` handlers relay every message from any client without a sender check (left open in P5/N7). | CoSyncNet.cpp WM/WD dispatch | Low. **Fixed (0.3.2):** WM/WD are sender-checked (SelfStateType) and relayed to everyone but the sender. |

Not yet audited in depth for this report (planned in the implementation phase):
packet sequencing beyond BONES, reconnect flows, per-peer memory in long
sessions, excessive logging (host log 45 MB / 28 min).

---

## 4. Fallout 4 reverse-engineering findings (new in this investigation)

```
Function:     ThirdPersonState constructor
Address/RVA:  0x102C7B0
Signature:    void (ThirdPersonState* this, PlayerCamera* cam) [+ callers 0x82E450, 0x10240F0, 0x1025480]
What it does: initialises the state; writes the shoulder offset +0x60 = (fOverShoulderPosX, 0, fOverShoulderPosZ)
How verified: IDA decompilation; GameSetting value addresses from the name-pointer layout (name at value+8)
Relevant data: fOverShoulderPosX 0x2F39498, fOverShoulderPosZ 0x2F394B0, fMinCurrentZoom 0x2F393C0

Function:     ThirdPersonState::SetCombatOffset (vtable 0x2522A88 slot 17)
Address/RVA:  0x102DAD0
Signature:    void (ThirdPersonState* this, bool combat)
What it does: +0x60 = combat shoulder offset (fOverShoulderCombatPosX/AddY/PosZ 0x2F394C8/0x2F394E0/0x2F394F8,
              or the melee set 0x2F39510/28/40) or the normal (PosX, 0, PosZ)
How verified: IDA decompilation

Function:     ThirdPersonState::Update (slot 11)
Address/RVA:  0x102CD50
What it does: per-frame camera update; on the weapon-drawn edge rewrites +0x60 (scaled by 0x2F39678) or calls slot 17
How verified: IDA decompilation (lines writing a1+96/100/104)

Function:     ThirdPersonState slots 14/15/16
Address/RVA:  0x102E440 / 0x102E5E0 / 0x102E6F0
What it does: read +0x60 and copy it into the current offset +0x6C (interpolation / apply)
How verified: IDA vtable scan
```

Existing verified addresses reused (from earlier phases): NativeAddItem 0x1173A90,
EquipObject path (CoSyncEquipHook), KillPuppet natives, MoveRefrToPosition,
ForceThirdPerson, ShowLooksMenu, the workshop set (docs/WORKSHOP_FACTIONS_RE.md).

Still to reverse-engineer (UNVERIFIED, needed by later phases): the main camera's
world-to-screen projection (overhead info), NPC attack/stagger/draw graph event
names for puppet playback, "ragdoll at rest" state.

---

## 5. Implementation plan (priority order) and protocol changes

| Step | Work | Protocol change (documented before building) |
|---|---|---|
| 1 | **Crash/corruption:** U1/U2: NPCSEEN gets location identity and the host rejects sightings from another location than the reporter's; NPC scan paused while the editor staging owns the player. | `NPCSEEN|base|ref|x,y,z|reporter|ws|cell` (added fields; host drops reports without them) |
| 2 | **Location identity (K5, U3, U5):** every player sends `WS|` on location change + 10 s keep-alive; host records per entity and relays; proxies placed in the REMOTE location, hidden (not freed) when in a different world; watchdog respawns at the last known position. | `WS|` from clients too (same format) |
| 3 | **NPC authority (K3):** puppets never run combat AI; owner's events drive puppet visuals; ownership handoff when the owner leaves the NPC's area (host reassigns). | new `NPCOWN|entity|newOwner`; AEV unchanged; new graph-event relay for NPCs after RE |
| 4 | **Death (K4):** authoritative corpse transform after death. | `CORPSE|entity|x|y|z|rx|ry|rz` |
| 5 | **Pip-Boy (K2), FaceGen (K1).** | none |
| 6 | **Networking audit** (sequence/staleness per message family, reconnect, log volume). | as found |
| 7 | **Text chat** (global + faction, host-enforced recipients, rate limit, UTF-8 length caps) + PrismaUI chat widget. | `CHAT|G|text` / `CHAT|F|text` (client → host), `CHATM|scope|senderEid|name|text` (host → recipients) |
| 8 | **Voice** (proximity + faction modes, mute, talking indicator). | **As built (0.3.2):** `VOICE|<P/F>|<seq>|<opus>` client -> host and `VOICEM|<eid>|<P/F>|<seq>|<gain>|<opus>` host -> each recipient, both unreliable. No `VTALK`: a receiver derives "talking" from the frames it is sent, so the indicator can never reveal a conversation the host did not route to you. |
| 9 | **UI:** faction emblems, banner discovery feedback, overhead name/faction/level (after the camera RE). | none (uses existing state) |
| 10 | **Test matrix** for every fix. | — |

### Decisions needed from you

1. **Voice codec/transport.** (a) **Steam Voice** (ISteamUser StartVoiceRecording /
   GetVoice / DecompressVoice): no new libraries, codec built in, works whenever
   Steam is running (LAN and Steam sessions); not with a Steam emulator.
   (b) **Opus + Windows capture**: works without Steam, adds the Opus library
   (BSD) to the build. Playback either way through XAudio2 (part of Windows)
   with distance-based volume. My recommendation: (a).
2. **Faction emblems:** OK to draw Minutemen / Brotherhood / Railroad / Institute
   style emblems as original SVG artwork in the Fallout look (no game files)?
3. **NPC authority:** keep one owner per NPC (current model, fixed as in step 3:
   the machine nearest the NPC simulates it, the host arbitrates and hands off)
   or move ALL NPC AI to the host (simpler authority, but NPCs near a client far
   from the host would not be simulated at all)? My recommendation: keep
   per-NPC ownership with host arbitration + handoff.


## Log volume (network audit, 0.3.2)

Measured on the 0.3.0 host log (CoSyncUpdated_2272.log, 45 MB / 28 min). Top lines by count: ForwardMessage EU 74k, transport SEND debug 88k, "Drained N" debug 45k, ForwardMessage BONES 22k, FPGraph pose 24k, AnimUtils live 24k, ForwardMessage HP 7.7k, PlayerMarker liveness 6.2k, LocalPlayer sent UPDATE 4.5k.

Now: EU / BONES / HP / VOICE arrivals and all sends are counted and summarised every 10 s (`[Transport] Last 10 s: rx EU=... | tx ...`), with every 50th EU still logged in full for the jitter measurement; the per-frame diagnostics go through `LOG_EVERY_MS` (2-15 s). Every other message type is still logged on arrival. Expected: roughly a quarter of the old volume (UNVERIFIED until the next session log).


## Overhead nameplates: world-to-screen (RE, 0.3.2)

- `WorldToScreen(NiPoint3* in, NiPoint3* out)`, 1.11.240 RVA **0xA37C90**. Source: F4SE 0.7.9 `f4se/NiObjects.cpp` (`WorldToScreen_Internal`; F4SE 0.7.9 targets 1.11.240). IDA: returns at once unless the camera is set up (byte 0x3279FC0), takes its own spin lock (0x3279FD0/0x3279FD4), projects through the global camera matrix / viewport (0x3279F80 / 0x3279FD8) via 0x16D2440.
- Output meaning, from its only caller (HUD marker routine 0x9CC840): `out.x` 0..1 from the left, `1 - out.y` 0..1 from the top, `out.z >= 0` in front of the camera. That caller also rescales x for Scaleform's 16:9 stage when the aspect ratio is 2:1 or wider; the PrismaUI view covers the whole window, so CoSync uses x unscaled.
- **UNVERIFIED / not done:** hiding tags behind walls. It needs an engine line-of-sight query that has not been verified yet; tags are culled by location, distance, camera direction and open menus only.
