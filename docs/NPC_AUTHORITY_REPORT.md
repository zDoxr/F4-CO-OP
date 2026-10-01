# CoSync 0.4.3 — NPC / Enemy Authority, Death State, HUD, Nameplates, Flashlight, Mines

Directive: "NPC / Enemy / Creature synchronization — investigate first, prove the engine behaviour, then implement" (2026-10-01).

- **Build:** 0.4.3 (network 403). Release x64, `/p:PlatformToolset=v145`, 0 errors, 0 warnings. Deployed.
- **Target:** Fallout4.exe **1.11.240**, F4SE 0.7.9. Every address below is a 1.11.240 RVA (image base `0x140000000`), read in IDA 9.3 on the 1.11.240 IDB. Every hook is prologue-checked at load; a mismatch disables only that part.
- **Evidence labels:** **VERIFIED** = read in IDA / the ESM / the real session logs, or proven by an offline test. **INFERRED** = reasoned, not read. **NEEDS TEST** = only an in-game session can settle it.

## Evidence base

| Source | What it gave |
|---|---|
| `CoSyncUpdated_30332.log` (host) / `CoSyncUpdated_34756.log` (client), 0.4.2, 2026-09-30 21:08–21:43 | The real session in which the reported bugs happened. Every root cause below that is marked "log" is traced in these two files. |
| IDA 9.3, Fallout4.exe 1.11.240 | 39 scripted batches: actor AI update, combat entry, locomotion channels, HUD enemy meter, Pip-Boy light, explosion hits, mine trigger. |
| Fallout4.esm index | Light forms, keywords, placed mines (PGRE), explosion forms. |
| Ultralight 1.4.0 harness (the exact DLLs PrismaUI ships) | Rendered CoSync's real `nameplates.html` offscreen and measured the bitmap and dirty rectangles. |
| PrismaUI_F4.dll (IDA) | How PrismaUI copies a view's surface. |
| TiltedEvolution (Skyrim Together) source | Design reference only: how a working multiplayer framework treats remote actors. No engine fact was taken from it. |

---

# PASS 1 — Architecture (what CoSync does, end to end)

```
GAME EVENT            DETECTION                         ENTITY STATE                   PACKET
host NPC moves/acts → NpcWorldSync scan (1 Hz, ProcessLists → CoSyncEntityState         → EC (CREATE, 5 s re-broadcast)
                      High+MiddleHigh + player cell)      (PlayerManager, per eid)      → EU (~20 Hz, 24 actors/tick round robin)
                      EnemySync hooks (kill core,                                       → ENS (combat/target/class/life, ≤5 Hz)
                      KillImpl, ModActorValue,                                          → HP, KILL, CORPSE, SHOT, AE (graph inputs)
                      Spell.Cast), CombatAuthority
                      (Fire, DoHitMe), AnimEventHook
                      (NotifyAnimationGraph inputs)
        → NETWORK (GNS, reliable-ordered per connection; host relays)
        → RECEIVE (network thread queues only) → CoSyncNet::OnReceive in the CoSync tick
          (engine job "NonRendersafeAI", serialized) → module queues → Flush* on the game thread
        → ACTOR: client binds the host's NPC by ref (hides its own copy) and spawns a
          PlaceAtMe clone = the PUPPET; CoSyncPlayer::TickSmoothing steers it
          (SetPosition + controller velocity), replays SHOT/AE/KILL/HP onto it
        → GAME STATE / UI: vanilla HUD reads the puppet actor; PrismaUI views read CoSync state
```

Authority before 0.4.3 (verified in code): the host owns every NPC it simulates; a client owns an NPC only outside the host's loaded area (`HostTickOwnershipHandoff`). Players own themselves. Damage to a puppet is reported (`EHIT`) by the attacker's machine and applied by the owner; KILL comes only from the owner.

What already worked and was kept unchanged: transport, relay, inbox, entity IDs/registry, CREATE-only spawning, ownership handoff, SHOT replay, kill-core gate, EHIT, ENS, KILL with killer, corpse placement, PrismaUI views, player bone replication.

---

# PASS 2 + 3 — Root causes (engine research and network audit)

## BUG #1 — NPC/creature animation and AI desync

### What Fallout 4 actually does (VERIFIED, IDA)

| Engine state | Where | Evidence |
|---|---|---|
| Per-actor AI update | `sub_140C63BC0`, called by the ProcessLists loops (`sub_140DA9880`, `sub_140DB2430`, …). Returns at once when `Actor+0x2D0 & 2` (AI enabled; `EnableAI` native → `sub_140C76AB0` toggles exactly this bit) is clear. Inside: the process-level AI (`sub_140CEF0B0` → packages/combat/detection, `sub_140D12010` for high process) and the movement controller update (`Actor+0x318` vfunc +0x40). | decompile |
| **Combat entry** | `sub_140D00F80(AIProcess*, Actor*, target, …)` — every way into combat: detection (`sub_140D10AE0`, `sub_140D2D5C0` …), the Papyrus `StartCombat` task 97, scripts. 26 callers. Starts the combat controller (`Actor+0x328`) and the "CombatAnger" bark. | decompile + callers |
| Combat update / stop | Actor vfunc `0xFF` = `sub_140CD0310` (runs the combat controller), vfunc `0x100` = `sub_140CD04E0` (StopCombat, frees `Actor+0x328`), vfunc `0xFE` = IsInCombat (`+0x328` exists). | vtable + decompile |
| **Locomotion channels** | `sub_140C66160` (movement tick) → `sub_140C66910` asks the movement controller for its **desired** velocity and writes it into the graph channels: Speed = middle-high process `+0x428` (setter `sub_140D35520`), Direction = `+0xC0` (setter `sub_140D355C0`). The graph's Speed/Direction data channels read those fields. | decompile + bytes |
| Actions → graph | `sub_140E13D70` → `sub_140E13DD0(actor, TESActionData*, …)` resolves an action to the actor's own graph event and sends it; CoSync already captures the result one level lower (`NotifyAnimationGraph`, holder vtable slot 1). | decompile |

### What CoSync's puppet did (VERIFIED, code + log)

- A puppet is an **AI-alive** clone. Its AI detected the local player, **entered combat locally** (`sub_140D00F80`), selected its own target, ran its own combat behaviour, played its own packages, and its movement controller wrote **its own intention** into Speed/Direction.
- CoSync then fought it every frame: `StopActorCombat` in `CoSyncPlayer::ApplyProxyTransform` (every tick), `SetPosition` (every frame), Fire refused, holster/draw refused.
- The owner's actions arrived only as a filtered subset of graph inputs: the host accepted **7,888** NPC graph inputs and forwarded **554 (7 %)**. On the client, owner inputs were often rejected (`rifleSightedStart accepted=0` ×11, `boltChargeStart accepted=0` ×5, …) because the puppet's graph was already in a state its own AI had put it in.

**Root cause:** the puppet ran a second, independent decision-maker (combat entry, target selection, packages, locomotion intention, local hit reactions). Transform sync could never fix that.

### Correct architecture (implemented)

| Layer | Owner (host or simulating machine) | Puppet |
|---|---|---|
| Combat entry / target | decides | **combat entry gated** to exactly the owner's target; the owner's fight is started on the puppet once through the engine's own StartCombat task (`sub_140C46B90`, task 97) |
| Combat behaviour (attacks, aim, reload, sneak, cover, stagger, hit reactions) | decides | **graph-input gate**: the puppet's own decision inputs are refused; the owner's arrive as `AE` (plus `SHOT`, `KILL`) and pass (replay guard) |
| Locomotion | its movement controller | **Speed/Direction channel writes replaced** by the owner's raw values (new EU fields) → the engine's own blend trees play the owner's walk/run/strafe |
| Position | real | interpolated (unchanged), now with the owner's real velocity (was always zero for NPCs) |
| Health | real | **owner-only** (see Bug #3) |
| Death | KillImpl | engine kill core replay (unchanged) |

Nothing is "forced": no animation clip is played by name, no bone is written for NPCs, the AI is not disabled (which would have stopped the movement controller and with it the locomotion channels — the 0.2.x experience).

## BUG #2 — Enemies stand up and die again on the client

### Exact trace (VERIFIED, log 34756, entity 65555; 65556 identical)

```
21:26:03.652 host   TX KILL entity=65555 killer=736831791          (engine death)
21:26:03.683 client replayed engine kill entity=65555  (death animation)            ✓
21:26:06–17  host   EC + KILL re-broadcast every 5 s; NO EU (corpses stream no transform, by design)
21:26:18.663 client TIMEOUT entity=65555 — no update for 15s, removing             ✗ (EU-only timeout)
21:26:18.664 client unbound DEAD -- keeping proxy corpse; s_killedEntities ERASED
21:26:22.012 client EC re-broadcast for an unknown entity → EnqueueSpawn          ✗ new body
21:26:27.050 client re-sent KILL → replayed engine kill … ragdoll=1               ✗ "dies again"
```

**Root cause:** the client decided an entity is *lost* from the absence of `EU` alone, and a dead entity never streams `EU`. The unbind then erased "this entity is dead", so the next CREATE re-broadcast looked like a new, live entity.

### Correct state model (implemented)

```
UNKNOWN ──EC──▶ ALIVE(bound|spawn pending) ──KILL──▶ DEAD(corpse)
   ▲                 │  ▲                               │
   │      DESTROY(0) │  │ EC (owner speaks)             │ DESTROY(Dead) / owner silent 15 s
   │                 ▼  │                               ▼
   └──── GONE ◀──────┘  └── UNLOADED (engine freed      TOMBSTONE (corpse kept; "known dead")
                             the body; next EC rebinds)      │  EC(Dead)   → ignored (no new body)
                                                             │  EC(alive)  → ID reused → ALIVE
```

- **Liveness = the owner says anything about the entity** (`EC`, `KILL`, `ENS`, `HP`, `CORPSE`, `EU`), stamped in `CoSyncEntityState::lastContactLocalTime`. A corpse's 5 s `EC`+`KILL` re-broadcast keeps it.
- **CREATE carries `Dead`** (`EntityCreatePacket::Dead = 1<<3`) on the host's re-broadcast of a corpse.
- **Tombstones** (`NpcWorldSync::s_corpseTombstones`): an unbound corpse stays "known dead"; a `Dead` CREATE never builds a living body; a non-`Dead` CREATE for the same ID is the host reusing the ID for a new actor and clears the tombstone.
- Unchanged and correct: a late joiner with no body gets the corpse spawned and laid down by KILL (instant ragdoll); `DESTROY` with the Dead flag keeps the corpse.

## BUG #3 — Enemy health bar + name flicker, wrong health

### What the HUD does (VERIFIED, IDA)

- The enemy meter is the vanilla `HUDEnemyHealthMeter`. Its per-frame data update is `sub_140A331F0`. It picks the crosshair / auto-aim / VATS actor (`PlayerActivatePickRefEvent`, `PlayerAutoAimActorEvent`, …) and decides **visibility** in `sub_140A343E0`. For a living actor that is `sub_140CD0940(actor, player)` — "the actor's **combat group** (vfunc `0xF2`) targets the player" (`sub_140E92800`) — or a short display latch.
- The **percent** is `sub_140A349F0` → `sub_140C63650(actor, Health)`, i.e. the **local** actor's Health / max.
- The event is pushed only when the value changes. A flip between visible/hidden every frame is therefore drawn as flicker.

### Root cause (VERIFIED code path; the per-frame flip follows from it)

1. **Visibility flips every frame:** the puppet's own AI re-entered combat through detection (`sub_140D00F80`, creating the combat group that targets the player), and CoSync's `StopActorCombat` destroyed it again on the next tick (`ApplyProxyTransform`, every tick). `sub_140CD0940` alternated true/false → name and bar blinked.
2. **Wrong / jumping health:** the client's own hits changed the puppet's local Health at once (local prediction). The owner's `HP` (1 Hz scan or ≥1 HP change) then corrected it, and a correction sent before the owner had applied the client's `EHIT` pulled the bar back up.

### Fix (implemented)

- The combat-entry gate (Bug #1) makes the puppet's combat state **stable and equal to the owner's**: the meter shows exactly while the owner's NPC fights *you* (plus the vanilla post-hit latch), and never blinks.
- **Owner-only puppet health:** `ModActorValue` (Health, mode 2) on a guarded puppet is refused unless CoSync applies the owner's value (`CoSyncEnemySync::ScopedHealthWrite`: HealthSync's `HP` apply, the engine kill replay, the `KillPuppet` fallback). The hit is still reported to the owner as before.
- The owner sends `HP` **on change at 5 Hz** (`OwnerStateTick`), not only from the 1 Hz scan.
- The meter's lifecycle is the engine's own (one widget, value events); CoSync no longer feeds it contradictory state, so nothing had to be recreated or hidden.

## BUG #4 — Name overlays multiply ("stale copies left behind")

### Traced (VERIFIED)

| Layer | Finding |
|---|---|
| CoSync data | One nameplates view per process (`Created nameplates view` once in each log, and once per process in `PrismaUI_F4.log`). One plate per player proxy actor (`CoSyncNameplates::Build`, de-duplicated by actor). Player entities never time out; no second Player entity existed in the session. |
| JS page | One element per id, moved in place; elements for absent ids are removed; any element the map does not own is removed. |
| Ultralight 1.4.0 (PrismaUI's own DLLs, offline harness `scratchpad/ul/ulplates.cpp`) | Plate moved 40 times, then two plates, then emptied: the bitmap never kept a stale plate (`ghost check … ink=0`, `after empty: ink=0`); every dirty rectangle covered both the old and the new position. |
| PrismaUI surface copy (`PrismaUI_F4.dll`, `sub_1801FE470`) | When a view reports dirty bounds it `memcpy`s the **whole** bitmap into a new frame and clears the bounds — no partial upload. |
| PrismaUI compositor (`sub_180291DA0`, `[Compositor] committed-frame reuse … heldStreak`, `held-source capture`, `offscreen compose`) | Holds committed frames and per-view held sources between composes. |

**Conclusion (honest):** the copies the user sees are produced **after** CoSync and after Ultralight — in PrismaUI's compositor (held sources / committed-frame reuse). Nothing in CoSync's data or page creates a second plate. No CoSync change was made for this: adding a cleanup call would hide nothing that CoSync produces, and the directive forbids that. **Next step:** reproduce with PrismaUI `[Debug] TextureProbe=1` / `[Perf] Profiler=1` and send the PrismaUI maintainers the facts above (whole-frame copy is fine; trail appears in composition). The harness can be re-run against any future PrismaUI build.

## BUG #5 — Player flashlight

### What Fallout 4 uses (VERIFIED, IDA + ESM)

- `PlayerCharacter::TogglePipboyLight` `sub_140D76930` → `SetPipboyLight(player, on, silent)` `sub_140D769C0`. The light is ON iff `PlayerCharacter+0xC38` is non-null.
- ON: form = a light marker in the actor's third-person 3D (`sub_140D86850`), else `PipboyLightOverride_DO` when the actor has `PipboyLightOverrideKeyword_DO` (`PowerArmorHelmetLightOverride` 0xB34A6, on every PA helmet OMOD) → **`defaultPowerArmorHeadlampLight` 0xB48A0, a 70° spot light**; else DefaultObject 374 → `defaultPipboyLight` 0x1ED2D (90°, Pip-Boy gobo). Parent = that marker node, else the third-person root (`GetActorRootNode(false)`, slot 0x8B). Built by `sub_140454880` (generic LIGH → scene light, 19 callers). Power Armor enter/exit (`sub_140989D90` / `sub_14098B580`) re-run it — that is the headlamp switch.
- OFF: `sub_1421B9E50(ShadowSceneNode, &light)`, holder `+0x28` vfunc `0x1E8`, release.

### Root cause
Nothing synchronized it.

### Fix (implemented, `CoSyncFlashlightSync`)
- The owner sends **state only**: `FL|eid|on`, on change and every 5 s (from `+0xC38`).
- Every other machine runs the **same engine steps on the proxy** (same form choice, same parent, same builder; the OFF steps in the engine's order).
- The light rides the proxy's own 3D: its position and direction follow the replicated, interpolated body/head pose. No light transform is streamed, so there is nothing to jitter, and the update rate is the pose's. A PA change on the proxy changes the chosen form and the light is rebuilt (headlamp ↔ Pip-Boy light).
- Weapon-mounted lights: vanilla Fallout 4 has none. Mod lights that use the same engine path are covered.

## BUG #6 — Landmine without warning

### Exact trace (VERIFIED, logs)

```
21:40:23.794 host   ExplSync TX base=0xEECE9 (fragMineExplosion) at (-51763,45510)   ← the HOST's mine
21:40:23.889 host   TX DMG entity=142044819 (the client's proxy) 60.3 → DOWN
21:40:23.828 client RX EXP … applied — the client never had this mine: no beep, no blink, no indicator
```
- No Fallout4.esm PGRE mine lies within 11 km of it: it was a **runtime** (thrown/placed) mine. `CoSyncThrowableSync` is switched off (`kThrowableVisualsEnabled = false`); nothing else replicates mines.
- At 21:39:20 the host stepped on another mine: the client's **replica explosion** hit the host's proxy and the client relayed it (`TX DMG 60.3 → DOWN`) — **double damage** from one explosion.

### What Fallout 4 does (VERIFIED, IDA)
- Mine state machine `sub_140E50A40`: `Projectile+0x230` detonation countdown (FLT_MAX = untriggered), `+0x234` blink/beep timer (`fMinesBlink*`).
- **Trigger** `sub_140E51300`: every actor in the high-process list (`ProcessLists+0x40/+0x50`) and the player, within the radius (× `fMineExteriorRadiusMult` outdoors), is asked **`sub_140E48440(projectile, actor, shooter, …)`** (shooter = `Projectile+0x1A8`; dead/allied actors and, unless `iProjectileMineShooterCanTrigger`, the shooter never). The first yes starts the countdown. **Proxies are high-process actors → they triggered real mines.**
- **HUD explosive indicator** (`MineIcon_mc`): `sub_1409C4A20`, from the player's position over the engine's own live projectile lists — a mine must exist **locally**.
- **Explosion hits**: `sub_140E44930` → `sub_140E37220(hit, aggressor = 0, target, explosion)` → task 14 → `DoHitMe` (its only direct caller is the task executor). Explosion hits have **no aggressor**; the explosion is `HitData+0x48`.

### Fix (implemented)
- **Trigger authority** (`CoSyncMineSync`, hook on `sub_140E48440`): player proxies and NPC puppets **never** trigger a mine. Each player's own machine decides with the exact position; an NPC's owner decides for it. No trigger race exists.
- **Existence** (`MINE|sender|owner|ref|proj|shooter|x|y|z|rz|ws|cell`): a runtime mine is announced once at rest and every 15 s; peers place the same PROJ (the engine builds a **real live mine** from it — the behaviour ThrowableSync documented on 2026-08-04) and give it the same shooter (`+0x1A8`, only on a verified `GrenadeProjectile`). A mine that already exists (CK mine, same mine seen twice) is **linked**, not doubled. → beep, blink, HUD indicator and disarm all work natively on every machine.
- **End** (`MINEX|sender|owner|ref`): when this machine's copy detonates, is disarmed or picked up, every other machine removes its copy **without detonating it**. The explosion itself arrives as `EXP`.
- **Explosion damage authority** (`CombatAuthority` DoHitMe gate): an aggressor-less hit on another **player's proxy** is refused — that player's own machine applies it from its own copy (origin or `EXP` replica). Puppets take no local damage (Bug #3 rule); the owner's copy of the explosion damages the real NPC. Each victim is damaged exactly once.

---

# Authority model (0.4.3)

**HOST (and any machine, for the NPCs it simulates — the existing ownership handoff):**
- NPC/creature/boss/legendary/companion AI: packages, detection, combat entry, target selection, combat behaviour, movement.
- Hits applied to its NPCs (its own and reported `EHIT`), health, death + killer, legendary mutation (script self-casts), corpses.
- Mine triggers by its NPCs; its own runtime mines' existence.
- Quest/world state (unchanged).

**CLIENT:**
- Its own player: position, pose, weapon, actions, health, damage it receives from aggressor-less sources (explosions, hazards), flashlight state.
- Hits *its player* deals to puppets (reported as `EHIT`; never applied locally).
- Mine triggers by its own player; its own runtime mines' existence.

**Replicated / shared (owner → everyone):**
- NPC: existence (`EC`, with `Dead`), transform + velocity + locomotion channels (`EU`), combat state + target + class + life + casts (`ENS`), graph inputs (`AE`), shots (`SHOT`), `HP`, `KILL`, `CORPSE`.
- Player: `EU`/`BONES`/`WS`, `FL`.
- World: `EXP` (visual + victim-side damage), `MINE`/`MINEX`, destruction/doors (unchanged).

**Never decided on a puppet:** combat against any target but the owner's, attacks/aim/reload/sneak/cover/stagger/hit reactions, locomotion intention, health, death, mutation, mine triggers.

# Packet model (changes only)

| Message | Fields | Direction | Delivery / ordering | Notes |
|---|---|---|---|---|
| `EC` | `spawnFlags` gains **`Dead` (1<<3)** | owner → all | reliable, ordered | host sets it on every corpse re-broadcast |
| `EU` | + **`hasMove|speed|direction`** (raw engine channels); NPC `vel` now real | owner → all | GNS reliable-ordered, timestamp-ordered on receipt | older 0.4.2-shaped EU still parses (offline test) |
| `HP` | unchanged format; owner sends on change at **5 Hz** | owner → all | reliable, ordered | puppets accept nothing else |
| `FL|eid|on` | new | player → all | reliable; state, on change + every 5 s | self-state sender check |
| `MINE|sender|owner|ref|proj|shooter|x|y|z|rz|ws|cell` | new | placing machine → all | reliable; re-announced every 15 s (late joiners, arrivals) | host checks `owner == sender` |
| `MINEX|sender|owner|ref` | new | the machine whose copy ended → all | reliable; idempotent (unknown key ignored, ended keys remembered 5 min) | self-state sender check |

Ordering questions:
- **CREATE after UPDATE:** UPDATE never spawns; the self-heal `REQENT` is unchanged.
- **UPDATE after DESTROY:** dropped (`m_destroyedAt`).
- **DEAD before CREATE:** KILL for an unknown entity is ignored; the next `EC` carries `Dead`, and the 5 s `KILL` lays the spawned body down.
- **Duplicate CREATE:** guarded (bound, spawn pending, tombstone).
- **Duplicate DESTROY:** harmless (unbind of an unbound ID).
- **Lost `MINEX`:** that machine's copy stays until its own trigger; its explosion is then an origin explosion and is announced again — no damage duplication, because proxies never take aggressor-less damage.

# Implementation (files)

| File | Change |
|---|---|
| `World/CoSyncPuppetAuthority.{h,cpp}` (new) | combat-entry gate (`0xD00F80`), owner's combat started through task 97 (`0xC46B90`), Speed/Direction setter hooks (`0xD35520`/`0xD355C0`), owner channel read, diagnostics |
| `World/CoSyncEnemySync.{h,cpp}` | `IsGuardedPuppet`/`IsPlayerProxy` (thread-safe), owner-only puppet Health in the ModActorValue hook + `ScopedHealthWrite`, HP on change at 5 Hz |
| `Hooks/CoSyncAnimEventHook.cpp` | puppet graph-input gate; forwarding adds cover/knockdown |
| `Weapons/CoSyncCombatAuthority.cpp` | aggressor-less hits on player proxies refused; SHOT replay `Fire` inside the replay guard |
| `Character/CoSyncHealthSync.cpp` | authoritative writes inside `ScopedHealthWrite` |
| `Character/CoSyncPlayer.{h,cpp}`, `Character/CoSyncPlayerManager.{h,cpp}` | owner's combat → puppet; no per-tick StopCombat when the gate is live; locomotion channels fed every frame; NPC velocity + channels in EU; contact-based timeouts |
| `CoSync/CoSyncEntityState.h` | `lastContactLocalTime` |
| `World/CoSyncNpcWorldSync.cpp` | `Dead` CREATE re-broadcast; corpse tombstones; ID-reuse rule |
| `Networking/CoSyncNet.cpp` | contact stamping (EC/ENS/HP/KILL/CORPSE), `FL`/`MINE`/`MINEX` routing + sender checks, ticks/resets |
| `Networking/Packets_*.h`, `EntitySerialization.h` | EU channel fields, EC `Dead` flag |
| `Character/CoSyncFlashlightSync.{h,cpp}` (new) | flashlight state + engine light builder on proxies |
| `Weapons/CoSyncMineSync.{h,cpp}` (new) | mine trigger gate (`0xE48440`), mine existence/end replication |
| `Core/Version.h` | 0.4.3 |
| `F4se-Data/.../CoSync.ini` | `[Puppet]` switches (all default 1) |

# Verified here vs. needs an in-game session

**VERIFIED (no game session needed):**
- every engine address, argument order and prologue above (IDA; runtime prologue checks);
- the Bug #2 trace and the Bug #6 trace (real logs);
- the Ultralight bitmap/dirty-rect behaviour (harness) and PrismaUI's whole-frame copy (IDA);
- the wire formats (offline round-trip test: new EU tail, 0.4.2-shaped EU, junk rejection, EC `Dead`): **ALL PASSED**;
- build: 0 errors, 0 warnings;
- **main-menu load test** (2026-10-01 17:10, `CoSyncUpdated_37724.log`): `version=0.4.3`, `combat entry hooked at base+0xD00F80`, `Speed/Direction channel setter hooked`, `PuppetAuthority ready: combatGate=1 movementChannels=1`, `EnemySync ready … authoritativeHealth=1`, `Flashlight ready=1`, `mine trigger test hooked at base+0xE48440`, `puppet input gate=1`. 100 s at the main menu, **0 errors, 0 warnings**, normal exit.

**NEEDS TEST (not run — please run with two machines/instances; log lines to look for):**

| # | Test | Expect |
|---|---|---|
| 1 | Raiders fight the host; client watches | client `[PuppetAuthority] entity=… owner fights … engine combat started`; `local combat entries refused=N` in the 10 s line; puppets aim at the host, never switch to the client unless the host's NPC does |
| 2 | Enemy targets client, then host | `ENS target` changes; client puppet re-targets (`started=`) |
| 3 | Walk/run/strafe of NPCs and creatures | `locomotion channel writes replaced: speed=… direction=…` > 0; legs match movement (no sliding, no running in place) |
| 4 | Attacks, reloads, sneaking, cover, stagger | `puppet-local decisions refused=N` in `[AnimEventHook]`; the owner's `AE RX … accepted=1` ratio higher than 0.4.2 |
| 5 | Kill an enemy, wait 60 s next to the corpse | no `TIMEOUT entity=…`, no `EnqueueSpawn` for it, no second `replayed engine kill` |
| 6 | Kill, leave the area, come back | `unbound DEAD … tombstone` then no new living body |
| 7 | Aim at an enemy fighting you | meter steady (no blink); bar = host value; client hits move it only when the host's `HP` arrives (`TX … HP=` on the host) |
| 8 | Aim at an enemy fighting the other player | meter only after you hit it (vanilla latch) |
| 9 | Legendary mutation by client damage | unchanged from 0.4.1 (`replayed cast`) and the meter jumps to the healed value |
| 10 | Creatures (Deathclaw, Mirelurk, dogs), robots, turrets | as 1–7; channel override works for any graph that uses the Speed/Direction channels |
| 11 | Flashlight on/off, in and out of PA, both directions | `[Flashlight] TX local light ON/OFF`; peer `light ON: form 0x0001ED2D` (Pip-Boy) / `0x000B48A0` (headlamp) |
| 12 | Throw a mine; peer walks to it | `[MineSync] TX mine …`; peer `placing a real mine here` + `replica … is live here`; peer hears the beep, sees the HUD mine icon, can disarm; `MINEX` removes it everywhere |
| 13 | Client walks over the host's mine | it beeps and explodes on the CLIENT (`ended here`); host `proxy/puppet mine triggers refused=N`, host removes its copy; one damage on the client |
| 14 | Host stands near a client grenade | host takes damage once; client `aggressor-less hits on player proxies refused=N` |
| 15 | 3–4 players | all of the above per player |

# Open items (honest limits)

1. **Puppet packages still run** (sandbox idles, wandering intention). Their movement intention is now overridden (channels + steering) and their decision inputs are refused, but a package's special idle on a puppet is not blocked: `CoSyncNpcIdleSync` deliberately lets them through because furniture entry/exit uses the same path. The owner's idles are replicated (`NIDL`).
2. **Bug #4 is outside CoSync** (PrismaUI compositor). Evidence and next step above.
3. **Mines are tracked in the cells of the players and of known mines**, not the whole loaded grid. A mine thrown into a cell where no player stands is announced once someone enters that cell (the scan runs at 4 Hz).
4. **Kill credit for explosion kills of host NPCs by a client** stays with the host's explosion copy (the engine builds explosion hits without an aggressor). Weapon kills keep their killer (0.4.1).
5. **Combat target internals**: the gate pins combat entry to the owner's target, and the target is re-asserted once a second. A combat-controller-internal target switch (if one exists outside `sub_140D00F80`) would be corrected within 1 s. **INFERRED**.
6. The enemy meter on a puppet shows while the owner's NPC fights **you** (engine rule: its combat group targets you). If it fights another player, you see it only after hitting it.
