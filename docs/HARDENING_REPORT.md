# CoSync hardening directive: final report (Phase 10)

Build: 0.2.3 (network build 203), runtime 1.11.240 only. Report date 2026-09-26.
Evidence labels: **PROVEN** (log, dump or IDA shows it), **STRONGLY SUPPORTED**
(consistent evidence, no direct proof), **UNCONFIRMED** (plausible, not shown).

## Updates after the report (0.2.4)

- **Blocker 3 closed (P5/N7), 809cef7 + 540ed59:** the host only accepts
  EU/BONES/CREATE/DESTROY, NPCSEEN/COMPOWN and 23 self-state message types
  from the entity's owner. Identity binds to the first player-naming message
  per connection. Replay of the 0.2.3 host log: 35,586 client messages, 0
  dropped. Still open by design: DMG, KILL, revive, PU, DOOR/LOCK, containers,
  quests (these need game-rule checks, not ownership).
- **Blocker 2 mostly closed (N4), 7a3c0e6:** BONES ~0.6 KB instead of ~3.5 KB
  (mean rotation error 0.08°). Projected host upload at 8 players drops from
  ~2.1 MB/s to roughly 0.6 MB/s. Wire format changed: **version 0.2.4**.
- Both are untested in game.

## 1. Verdict

**Not yet ready for 8 players over the public internet.** Two-player play on one
PC / LAN ran a 14-minute session (logs 16236 / 44332) with no fatal fault
until the quit, and that exit crash is now fixed. What still blocks the target:

| # | Blocker | Status |
|---|---|---|
| 1 | No internet path. Only direct IP / LAN discovery exists; the chosen free route (Steam lobbies + Valve SDR relay, PROVEN available under app 377160) is not implemented. | open |
| 2 | Bandwidth: BONES costs ~35 KB/s per player. At 8 players the host must upload ~2.1 MB/s (~17 Mbit/s). | open (N4) |
| 3 | The host relays ~50 message types without checking the sender owns what the message is about. Any client can move, kill or rewrite anyone's entities. | open (P5/N7) |
| 4 | Thread overlap between the window thread (menus, DISCONNECT) and the tick job is UNCONFIRMED; the next test measures it. | diagnostic in f3d47d2 |

## 2. Thread model (PROVEN, Phase 10)

- CoSync's tick is an F4SE permanent task. In game it runs inside the engine
  job **"NonRendersafeAI"** (`sub_140BD6170` → `sub_140C365F0` → hooked
  message-queue processor `0xC45150`), part of the frame job list built by
  `sub_140BD44A0`. The main frame only signals job stages (`sub_141698250` =
  `SetEvent`).
- The 0.2.3 logs show the tick on 6-7 worker threads (host: 29460, 29548,
  30020, 15120, 38584, 34748, 38980) and only 12 times on the window thread
  (main menu).
- WndProc and every PrismaUI UI callback (Host/Join/Leave, `EndSession` →
  `CoSync_Main::Shutdown`) run on the window thread; PrismaUI's "game thread"
  is the window thread (1735 of 1735 UI events).
- Consequence found and fixed: the teammate HUD disabled itself on the first
  in-game tick (1f2c73b).
- Open question: can the window thread run while the tick job is running? If
  the next test logs `[ThreadDiag] window-thread code ran WHILE the tick was
  running`, session start/stop must be queued into the tick.

## 3. Fixes (all on zDoxr/F4-CO-OP master)

"0.2.3 run" = exercised in the 26 Sep 0.2.3 session without a fault.

| Commit | ID | Fix | Test status |
|---|---|---|---|
| 4ed912b | T2 | exit crash from the discovery thread | verified in game |
| 38b97e3 | E1/E1b | equip on NPC proxies: null stack lists; clone ownership | 0.2.3 run (55 direct equips; null-list case not hit) |
| 02f0aeb | T1 | QuestSync stage map race (Papyrus threads) | untested |
| 5f38588 | P2 | spawn task resurrecting a destroyed entity | untested |
| 3b6c766 | P3 | entity registry locked | untested |
| 3c5603b | L2 | freed-proxy path releases registry/hook/bone cache | 0.2.3 run (16 frees) |
| bc26d10 | N1 | world NPCs gone for good after timeout/unload | untested |
| bb958d5 | — | lock read / remote lock apply addresses for .240 | 0.2.3 run (21 lock events) |
| 2956fcc | S-SEC1 | CUSTOMEVENT refused while relay disabled | untested |
| 5258ff1 | P7 | non-finite positions rejected | untested |
| f3e24a0 | P4 | ModSync never overwrites plugins | untested |
| 8fb9323 | P6 | dead peer detected in 10 s, not 5 min | untested |
| 3e82bc4 | P1 | inbox 65536, evicts stale state first | 0.2.3 run (never full) |
| bf3e784 | W1-A | truthful "hid" logs; hidden originals restored | 0.2.3 run |
| de0eb67 | W2 | host takes back a departed client's NPCs | untested |
| 9f5b19d | L3 | proxies / hidden NPCs cleaned at session end | untested |
| e41dbe7 | P8 | only 1.11.240 declared compatible | — |
| eaeab3e | P10 | start-up thread joined before shutdown | untested |
| 4ee8ae6 | N5 | no false departure for a reconnecting identity | untested |
| 54a7c24 | S-SEC2 | caps on WORLDREQ, world size, MODEVENT | untested |
| 05a8f74 | N2 | no echo of EU/BONES to the sender | **PROVEN working** (client got 0 of its own BONES, was 48%) |
| 0c05709 | N3 | BONES sequence numbers | 0.2.3 run |
| c6828d4 | — | single version source 0.2.3 | — |
| f38d330 / 5521638 | P9 | dead VEH patches and fake-ret recovery removed | 0.2.3 run (VEH now only reports) |
| dbad543 | — | QUIT confirm click no longer opens the Multiplayer menu (exit heap corruption) | untested |
| 3bb49ea | — | freed actors swept before any tick step uses them | untested |
| 1f2c73b | — | teammate HUD works in game | untested |
| b80e79e | — | ThreadDiag log flood (~96k lines/session) | — |
| f3d47d2 | — | tick / window-thread overlap census | diagnostic |

### Crashes explained in this phase

- **Exit heap corruption, both machines (PROVEN).** The click that confirmed
  QUIT also opened the Multiplayer menu. The engine then destroyed
  BSInputEnableManager (`0x140C3CF20` nulls `0x30E88B0`, PrismaUI Address
  Library ID `0x492F89`). PrismaUI's unfocus logged `mgr=false` and CRT-freed
  the game-owned input layer. Fixed on CoSync's side. **The PrismaUI defect
  remains:** quitting any other way while its view has focus would still
  crash.
- **Client read faults 20:47:57 (PROVEN).** An NPC unloaded by the engine was
  used by `HostSendNpcUpdates` before the end-of-tick sweep cleared it.

## 4. 8-player review (measured on 0.2.3, 2 players)

| Stream | Measured | Notes |
|---|---|---|
| BONES | ~3.5 KB/msg, 8.5-10/s per player → ~35 KB/s | 80%+ of all bytes; text, 4-decimal quaternions |
| Player EU | ~0.41 KB/msg, ~15/s → ~6 KB/s | |
| NPC EU (host→client) | ~15 KB/s | round-robin, 24 NPCs per 50 ms max |
| everything else | < 1 KB/s | except world download (one-off 4-6 MB) |

Projection for 8 players (star, host relays):

| | now | with BONES at ~0.5 KB |
|---|---|---|
| Host upload | ~2.1 MB/s (~17 Mbit/s) | ~0.65 MB/s (~5 Mbit/s) |
| Host download | ~0.29 MB/s | ~0.08 MB/s |
| Client download | ~0.30 MB/s | ~0.09 MB/s |
| Client upload | ~0.04 MB/s + owned NPCs | ~0.01 MB/s |

- GNS send rate (PROVEN from the GNS source): `SendRateMin`/`Max` clamp a
  pacing rate; this GNS has no loss-based backoff. The global 1 MB/s minimum
  is not a floor on traffic. The Phase 3 wording "hard floor" overstated it.
  Only the world download bursts at it.
- Session cap 8 is enforced at the GNS connect callback (Phase 4 memory).
- Per-peer state never pruned (N8) grows with joins/leaves, not with players
  online; minor at 8.

**Recommendation:** N4 first. Send each skeleton's bone-hash list once, then
per frame only quantized rotations (smallest-three, 4 bytes/bone). 92 bones →
~0.4-0.5 KB, a 7-8× cut.

## 5. Still open (priority order)

1. **Steam lobby + SDR transport** (internet play; blocker 1).
2. **P5/N7 relay authorization:** host checks `conn` owns the entity for
   EU/BONES/HP/KILL/OUTFIT/etc. before applying or relaying.
3. **N4 BONES size** (section 4).
4. **Thread overlap** — act on the f3d47d2 result.
5. **PrismaUI exit defect** — only avoidable from our side; report upstream.
6. **S-1/S-2 interpolation:** uses local receive time, the host overwrites
   relayed timestamps, and the 120 ms delay is shorter than the 167 ms idle gap.
7. **W3/W4 containers:** withdrawal credited to the nearest container; no
   arbitration of duplicate takes.
8. **W5 quests:** client-caused stage changes are suppressed and lost.
9. **L1:** raw `Actor*` everywhere; engine handles (0x21E710 / 0x2A0860)
   would replace the IsFormStillLive heuristic.
10. **N6:** SALT/VER sent once; resend at HELLO. **N8:** per-peer leaks.
11. GNS still runs `IP_AllowWithoutAuth=1` (unauthenticated certs, PROVEN by
    "unsigned cert" log line). Steam's authenticated sockets would close this
    with item 1.

## 6. Failure test plan (for the next batch test)

Use two instances as before. Record each result; send both CoSync logs,
`PrismaUI_F4.log`, and any `.dmp`.

| # | Do | Expect |
|---|---|---|
| F1 | Main menu: QUIT → confirm | game exits, no `.dmp` in `%LOCALAPPDATA%\CrashDumps` |
| F2 | Main menu: open MULTIPLAYER, then Alt+F4 | log `Window closing -- releasing multiplayer menu focus first`, clean exit |
| F3 | Host + join, play 2 min | teammate HUD bars visible on both |
| F4 | Client leaves via Multiplayer menu | host log `Peer left`, client proxy gone on host, host NPCs normal |
| F5 | Client uses pause-menu DISCONNECT | same as F4, no fault in either log |
| F6 | Kill the client process (Task Manager) | host drops it within ~10 s (P6), `Released N world NPC(s)` if it owned any (W2) |
| F7 | Client rejoins right after F6 | joins, no duplicate proxy, no false `left` for the new connection (N5) |
| F8 | Walk the client far from the host (cell unload) and back | `freed (unloaded)` then proxy respawns; no read faults |
| F9 | Host enters an interior, waits 30 s, exits | client sees NPCs restored; no NPC stays hidden |
| F10 | Equip/unequip weapons on both | proxies show the weapon; no fault |
| F11 | Lock/unlock a door on one side | other side matches |
| F12 | Complete a quest stage on the host | client follows |
| F13 | After all tests grep both logs for `window-thread code ran WHILE` | report the count (answers blocker 4) |

## 7. Regression checklist

Changed behaviour that an old feature could trip over:

- Main menu MULTIPLAYER must still open with a normal click/Enter (dbad543
  requires `currentState == MAIN_STATE`; log `Activate ignored` if not).
- Session end now despawns all proxies and re-enables hidden NPCs (L3):
  check the host world looks normal after every leave.
- VEH no longer "recovers" from EXEC faults (P9): a fault that used to be
  silently skipped now crashes; send the dump.
- Version check: both instances must run 0.2.3 (203) — mixed builds refuse
  each other by design.
- Only 1.11.240 loads the plugin (P8).
