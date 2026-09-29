# CoSync 0.3.4 — Sync Rework Report

Directive: "Deep Bug-Fix, Reverse-Engineering & Multiplayer Synchronization Pass" (2026-09-29).
Build: 0.3.4 (network 304), Release x64, built clean with `/p:PlatformToolset=v145`, deployed.
**Nothing in this report has been tested in game yet.** Every claim is marked **VERIFIED** (engine code read in IDA, or logged ground truth) or **INFERRED** (reasoned, not proven).

Evidence used throughout:

| Log | Build | Role | Notes |
|---|---|---|---|
| `CoSyncUpdated_3012.log` | 0.3.2 | host | last passing test (2026-09-28 21:37–21:59) |
| `CoSyncUpdated_10228.log` | 0.3.2 | client | same session |
| `CoSyncUpdated_40280.log` | 0.3.3 | host | regressed test (2026-09-28 23:40–23:55) |
| `CoSyncUpdated_19284.log` | 0.3.3 | client | same session |

IDA: Fallout4.exe 1.11.240 (working copy of the canonical IDB), headless `idat` + scripts. RVAs below are relative to 0x140000000.

User report used to scope Phase 3 (answered in this session): the problem is seen **on the client, looking at the host**; the host's character **slides without walking** and **actions don't play**; the weapon that disappears is **the other player's gun**.

---

## 1. Root-cause reports

### RC1 — Remote player "slides, no walk, actions don't play" (the host as seen by the client)

**What was broken.** Player proxies are animated by bone replication: the owner captures its skeleton at 15 Hz and the receiver writes those rotations onto the proxy's bones (`CoSyncPlayerAnimation::ApplySkeleton`). For the rest of the session, nothing written there reached the visible skeleton. The proxy only played its own idle graph while being moved.

**Why.** `ApplySkeleton` caches the proxy's bone nodes and pins them with `NiIncRef`. It rebuilt the cache **only when the 3D root pointer changed**. When a proxy is hidden and re-shown, its 3D is torn down and rebuilt, and the freed root's memory can be reused for the new root. The pointer compare then still matches, and the cache keeps writing into the old skeleton. That skeleton is detached from the scene but kept alive by our own pins, so nothing crashes and nothing moves on screen.

**Evidence (VERIFIED, logs).**
- 0.3.3 client, host proxy `0xFF000FC6`:
  - `23:45:43.950 cache REBUILT nodeCount=93` — the cache is built once.
  - `23:45:43.953` `RefreshActor3D` + `HIDDEN` (the client entered the Vault 111 editor cell).
  - `23:45:46.984` `RefreshActor3D`.
  - `23:46:15.234` `RefreshActor3D` + `SHOWN (full refresh)`.
  - **No further `cache REBUILT` for the rest of the session**, while `ApplySkeleton #… applied 36/93` kept counting writes.
- 0.3.3 host, client proxy `0xFF00100C`: after the same show cycle the cache *was* rebuilt, but 1.4 s after the re-show while the 3D was still loading: `23:46:16.665 cache REBUILT nodeCount=60 pinned=59`. Every apply afterwards read `applied 25/93 (cached=59 missed=67)`. It never re-walked, so the client was also partly broken on the host's screen.
- 0.3.2 (passing test): neither side went through a hide/show cycle after the cache was built, so the weakness never triggered.

**Code path.** `CoSyncPlayerManager::Tick` → `CoSyncPlayer::TickAnimVars` → `CoSyncPlayerAnimation::ApplySkeleton` → cache check `entry.root != root` (the old code); write-last re-stamp in `ReassertPoseForGraphRoot` (keyed by the cached "Root" bone).

**Fix (`Animations/CoSyncPlayerAnimation.cpp`).**
1. The 3D root is pinned (`NiIncRef`) for as long as it is cached, so a replacement 3D can never reuse its address.
2. The cached "Root" bone must still hang off the current root (new `IsNodeUnder`, which walks parent pointers at +0x28).
3. Once a second the skeleton is re-counted. A different count means it was cached mid-load, or gear changed the tree, so the cache is rebuilt.

Every rebuild now logs its reason.

### RC2 — First-person sender: legs slide (INFERRED contributor)

A first-person capture is applied to the **upper body only**. `capture.upperBodyOnly` is gated in `ApplySkeleton` because the first-person 3D has no lower-body nodes, and COM/Pelvis are camera-anchored. The legs are left to the proxy's own locomotion graph.

In 0.3.3, `PositionRemoteActor` moves player proxies with `Actor::SetPosition(pos, true)` every frame. VERIFIED in IDA (§2.1): that call zeroes the character controller's linear velocity. The puppet path already hands the owner's velocity back afterwards (0.3.3 follow-up); the player path never did.

**Fix:** player proxies now get `SetActorControllerVelocity(owner's velocity)` after positioning, with the same freshness rule as puppets.

**Still unverified:** the exact engine consumer that turns the controller's motion into the `Speed` graph input for an actor whose AI isn't pathing. §2.2 shows `Speed` comes from the movement controller's output, not straight from the character controller.

### RC3 — "The weapon model appears for a few seconds, then becomes invisible" (the other player's gun)

**What was broken.** A proxy's weapon was drawn when its owner drew it. A few seconds later the proxy was empty-handed, and it stayed that way until the owner holstered and drew again.

**Why (VERIFIED, IDA §2.4).** A proxy is a real engine `Actor` with its AI running. The engine's middle-high AI process update (`sub_140D12010`) **holsters the weapon of any non-player actor** that has it drawn while it has no combat target and no alert. It does this by calling `Actor::DrawWeaponMagicHands(false)` (Actor vtable slot 0xC9 = `sub_140C5D5A0`) itself. Fallout 4 draws no model for a holstered weapon.

CoSync only re-drew on an **edge** of the owner's state (`TickAnimVars`: `weapState != m_prevWeaponState`). Once the AI had holstered the weapon, nothing noticed. NPC puppets had the same problem: their combat is stopped on every update, so the AI holstered the weapon the owner's combat edge had just drawn.

**Not a visibility problem.** The engine really did change the weapon state. Forcing the model visible would have hidden that.

**Fix.**
- New `Weapons/CoSyncProxyWeaponAuthority` hooks `sub_140C5D5A0` (MinHook). For a registered proxy it **refuses calls from the AI update** (return address inside `sub_140D12010`) that contradict the owner's replicated state. CoSync's own calls (`CoSyncEquipHook::DrawWeaponEngine`, wrapped in `ScopedOwnCall`) always pass. Calls from any other engine path are allowed, and counted with their caller RVA (`[WeaponAuthority]` 10 s summary). If something else still fights the state, the next log will name it.
- The replicated state is fed every tick from the owner's weapon state (players, NPC proxies), or from the owner's combat state (puppets).
- `TickAnimVars` now converges on the **state**: if the owner has the weapon drawn and the engine has the proxy's weapon fully holstered for more than 1 s, it draws once through the engine. Skipped while a scheduled draw is pending or the proxy is in PA.
- `PlayerCharacter` overrides slot 0xC9 (`sub_140DA13D0`), so the local player is never touched by the hook.

### RC4 — Power Armor drops off for everyone ~2 s after entering

**Evidence (VERIFIED, logs).**
- 0.3.3 client: `23:52:22.048 TX PA … entering=1`, then `23:52:23.801 TX PA … entering=0` + `FALLBACK exit detected via PowerArmorRace (Papyrus OnGetUp did not report)`. The real exit came at `23:52:36.645` through Papyrus (camera leaves furniture state 9). The same thing happened again at 23:52:51.879 → 23:52:53.650.
- 0.3.3 host: it received the false exit and ejected the proxy (`Exit PA … deferring … retry`).

**Why.** `TickLocalPowerArmorFallback` compares "known in PA" against the race. "Known" is set by the enter notification the moment the player sits in the frame. The race only becomes PowerArmorRace when the enter animation completes. For the whole animation the two disagree, and after the 1.5 s grace the fallback broadcast an exit.

**Fix (`Armor/CoSyncPowerArmorSync.cpp`).** An explicit Entering phase. The fallback may only claim a missed exit after the race **has been seen** as PowerArmorRace in this PA session **and** the engine's own `ExtraPowerArmor` marker on the player is gone.

### RC5 — Power Armor exit: redundant rebuild and eject hammering

**Evidence (VERIFIED, logs).**
- Host `23:52:36.682`: a second exit for a proxy already out read `ExtraPowerArmor 0 -> 0`, then ran `SwitchFromPowerArmor + full 3D rebuild` again.
- `23:52:23.834`–`23:52:27.882`: the native eject was re-called every second while `ExtraPowerArmor` stayed 1 for about 4 s.

**Fix.**
- An exit for a proxy that isn't in PA (no chassis, marker clear) is a logged no-op.
- The retry loop now **waits** for the engine's own exit sequence, and re-issues the eject only after 5 s without progress. It still gives up after 10 attempts without tearing down a merged chassis.

### RC6 — NPC copies that never appear (STUCK for a whole session)

**Evidence (VERIFIED, logs).**
- 0.3.3 client: entities 65536 and 65538 (base `0x28669`, creatures) logged `STUCK … graphReadyNow=0 neverApplied=1` 110 times each, i.e. no 3D for the whole session.
- Both were spawned at `23:45:21` while the client was in the Vault 111 editor cell (`0x16D8`).

**Why.**
- A proxy is created in the cell the local player is in at that moment.
- Puppets and players are then moved with `SetPosition`, which never changes cell.
- The cell resync (`ResyncActorCellMembership` → `MoveRefrToPosition`) only runs when a transform update arrives. Idle or dead NPCs send none.
- The puppet 2000-unit snap never fired, because the coordinates were already right; only the cell was wrong.

The copies stayed filed in an interior at exterior coordinates.

**Fix (`World/CoSyncWorldSync.cpp`, `CoSync/CoSyncGameAPI.cpp`).** On every local transition, and every 2 s, each visible CoSync-spawned proxy is checked. If its cell doesn't hold its position, it is re-filed through the same engine move (`RefileMisplacedActor`, at most once a second per actor). `IsRegisteredRemoteActor` (new, `Hooks/TickHook`) guarantees a machine's own real NPCs are never moved.

### RC7 — NPC alive for one player, dead for another (authority gap)

**Why (VERIFIED, code).** `InitNpcProxy` clears essential/protected on NPC proxies, so a client's own shots could bring a puppet to 0 HP and **kill it locally**, before the owner had decided anything.

**Engine fact used (VERIFIED 2026-08-18, recorded in `CoSyncGameAPI.cpp`).** `Actor::SetLifeState` (vtable 0x105) reads the runtime essential bit `uiFlags 0x40000`. While it's set, a requested Dying transition becomes essential-down (life state 7), and `KillDying` no-ops.

**Fix.** A world NPC this machine does **not** own now carries the runtime essential bit (`SetRuntimeEssential`). Local damage can down it but not kill it. The owner's `KILL` → `KillPuppet` already clears the bit and kills. A puppet this machine takes ownership of gets the bit cleared.

**INFERRED:** recovery from essential-down when the owner's HP broadcast stays positive relies on the engine's normal bleedout recovery.

### RC8 — Downed player hit with a stream of relayed damage

**Evidence.** 0.3.3 host: `TX DMG entity=902260615 amount=5.3 (player proxy, hp 0.0->-5.3)` 272 times, several per second, while the client was downed.

**Fix (`Character/CoSyncHealthSync.cpp`).** A player proxy already at 0 HP (its owner reports 0 while in bleedout) no longer relays further drops.

### RC9 — Caught crash in a Power Armor diagnostic

**Evidence.** 0.3.3 host: `23:52:22.816 CRASH 0xC0000005 READ 0x9` at `DoxCoSync.dll+0x21750`, resolved with the build's `.map` to `CoSyncAnimVarSystem::DumpGraphVariableNames+0x290` ← `CoSyncPlayer::TickAnimVars+0x584`. It was walking the PA graph's variable hash buckets.

**Fix.** The delayed dump call was removed. It was a diagnostic only.

---

## 2. IDA findings (Fallout4.exe 1.11.240)

### 2.1 `Actor::SetPosition` — `sub_140C60B50` (Actor vtable slot 0xCA) — VERIFIED
- `sub_140513DB0(actor, pos)` writes the reference position. If it changed, it calls `sub_1404FFBF0` and vfunc +0x68 with `2` (change flag).
- With `updateCharController`:
  - The controller (`sub_140D347C0(process)`) is moved with `sub_141894C50` (game units × 0.0142875 → Havok, vfunc +0x1A0 with 1).
  - Its **linear velocity is set to the zero vector** (vfunc +0x1D0 = slot 0x3A, which is SetLinearVelocity per the 0.3.3 notes), and `sub_1418806D0(ctrl, zero)`.
  - It clears `ctrl+0x300` bit 0x80 and `ctrl+0x328`.
- Then it writes the 3D root's local translation (vfunc +0x460 = slot 0x8C).

### 2.2 Locomotion graph inputs — VERIFIED
- `sub_140CA9EA0` registers the actor's animation-graph data channels: `BSTAnimationGraphDataChannel<Actor, float, ActorSpeedChannel>` for "Speed", plus Direction, Pitch, Roll, TurnDelta, PitchDelta, WeaponSpeed, iWantBlock, TimeDelta, bCCSupport, fCCNormDotUp, bCCOnStairs, TurnDeltaSmoothed, SpeedSmoothed and DirectionDegrees.
- The Speed channel (`sub_140CC3B10` → `sub_140DF3710`) polls `sub_140CA69A0(actor)` = `process->middleHigh + 0x428` (setter `sub_140D35520`). It keeps its cached value instead when graph-var #4 or `sub_140CA5760` is set.
- The Direction channel (`sub_140DF3C40`) polls `sub_140CA69E0` = `middleHigh + 0xC0` (setter `sub_140D355C0`).
- The writer is `sub_140C66910` (movement update, called from `sub_140C66160`):
  - Speed comes from the movement controller at `actor+0x318` (vfunc +0x38; `MovementControllerNPC` vtable `0x256FB28`, `MovementControllerAI` `0x289C3E0`, both slot 7 = `sub_141E449F0`, which runs the movement-handler chain).
  - When graph bool #9 is set (animation-driven), it uses the "Speed" graph variable × scale instead.
- **Not proven:** which movement handler reads the character-controller velocity. See RC2.

### 2.3 Worn-model update flags — VERIFIED
- `sub_140D363C0(process, flags)` ORs `middleHigh+0x496`. The readers are `sub_140D36420`/`sub_140D36440`, and it is cleared by `sub_140D363E0`/`sub_140D36400`.
- The consumer is `sub_140D02600` → `sub_140D27280` (DoUpdate3dModel):
  - 0x0C → queue head data (`AIProcess::PendingActorHeadData`).
  - 0x20 → full 3D reload path (vfuncs +0x440/+0x430).
  - 0x01 → worn biped path (`sub_140510180`, `sub_140D319A0`, vfunc +0x7D8 or +0x740).
  - 0x10 → `sub_1404FF8C0` + `sub_140C9AFE0`.
  - 0x100 → vfunc +0x908.
- So the flags 0x11 that `RequestActorModelUpdate` uses rebuild worn gear on the **existing** skeleton, not a skeleton reload. That ruled out one RC1 hypothesis.

### 2.4 Weapon draw / holster — VERIFIED
- `Actor` vtable `0x25683B0`. Slot 0xC9 = `sub_140C5D5A0` (DrawWeaponMagicHands). `PlayerCharacter` slot 0xC9 = `sub_140DA13D0`. RTTI has no `Character` class in this build: NPCs are `Actor`.
- The weapon state is `(actor+0x134 >> 1) & 7`: 0 holstered, 3 drawn; 1/2 and 4/5 are the transitions. The draw/holster actions are the DefaultObjects at +0x1E0 / +0x1E8, performed by `sub_140DC0250`. The state change goes through `actor+0x128` vfunc +0x110.
- `sub_140D12010` (middle-high AI process update) calls `[vtable+0x648]` four times. The holster (`…(a2, 0)`, LABEL_286) runs when all of these hold:
  - the actor is not the player (`a2 != qword_1432DD370`);
  - `sub_140CF1E60` is false (read as "in combat": INFERRED from its use);
  - the bytes at `+0x573` / `+0x58B` of the object at `process+0x10` are clear (alert/state flags: INFERRED);
  - the player-proximity/keyword test passes (`a2[180] & 0x4000000` …);
  - the weapon state bits are `>= 6`, i.e. drawn or later.
- A scan for `call [reg+648h]` found 40+ other callers (furniture, PA, death, combat …). That's why the hook only refuses calls from `sub_140D12010` and reports any other contradicting caller.

### 2.5 `ApplyActorFootIKResults` — `sub_140E1BE80` — VERIFIED (partially)
It reads the controller position (vfunc +0x1A8, × 69.99), gets foot-IK data from the active graph (`sub_140E1C880`), blends the actor position toward it at `min(15·dt, 1)`, and writes it with `sub_140513DB0`. This is why 0.3.3 moves the controller together with the proxy.

### 2.6 Other confirmed facts used
- GNS `ISteamNetworkingSockets::GetConnectionRealTimeStatus` → `m_nPing` (vendored headers). Used for the UI ping.
- `SetLifeState` + runtime essential/protected bits (`uiFlags 0x40000`/`0x80000`): verified 2026-08-18, reused for RC7.

---

## 3. Architecture changes

| Change | Kind | Where |
|---|---|---|
| `CoSyncProxyWeaponAuthority` (hook on `sub_140C5D5A0`, replicated drawn state per proxy, SRW-locked map, 10 s diagnostics) | **new system** | `Weapons/` |
| Bone cache validity: pinned root, attachment check, 1 Hz recount, rebuild reason | modified | `Animations/CoSyncPlayerAnimation.cpp` |
| Player-proxy controller velocity hand-back | modified | `Character/CoSyncPlayer.cpp` |
| Weapon state convergence (state, not edge) | modified | `Character/CoSyncPlayer.cpp` |
| PA sender Entering phase (race confirmation + `ExtraPowerArmor`) | modified | `Armor/CoSyncPowerArmorSync.cpp` |
| PA exit idempotent, engine-paced eject retry | modified | `Armor/CoSyncPowerArmorSync.cpp` |
| Proxy re-file pass (`RefileMisplacedActor`, `IsRegisteredRemoteActor`) | new functions | `CoSync/CoSyncGameAPI`, `Hooks/TickHook`, `World/CoSyncWorldSync.cpp` |
| NPC death authority (`SetRuntimeEssential`) | new function + ownership rule | `CoSync/CoSyncGameAPI`, `Character/CoSyncPlayerManager.cpp` |
| Downed-proxy damage relay gate | modified | `Character/CoSyncHealthSync.cpp` |
| Per-connection ping (`GNS_Session::GetPingMs`, `RosterEntry::pingMs`) | new API | `Networking/` |
| PrismaUI multiplayer menu redesign | UI | `views/DoxCoSync/multiplayer.{html,css,js}` |
| `DumpGraphVariableNames` call after PA enter | **removed** | `Character/CoSyncPlayer.cpp` |

- **Packets:** no wire-format change. The UI state JSON gains `roster[].ping` (local only). The version bump to 0.3.4 (network 304) is for behaviour.
- **State:**
  - PA sender: Unoccupied → Entering (notification, race not yet swapped) → Occupied (race confirmed) → exit (Papyrus, or confirmed fallback).
  - Proxy weapon: replicated drawn/holstered, enforced against the AI.
  - NPC puppet: owner-alive / essential-down / dead, where only the owner can move it to dead.
- **Ownership:** unchanged hand-off rules. What changed is what ownership *means* for death (RC7).

---

## 4. Multiplayer behaviour (authority model after 0.3.4)

| Thing | Authority | Replicated | Local simulation | Interpolated |
|---|---|---|---|---|
| Player position/rotation | the player's own machine | EU (unreliable, ~15 Hz) | none | yes (sender clock + 120 ms) |
| Player pose | owner | BONES (unreliable, 15 Hz) | proxy graph for parts not in the capture (legs of a first-person sender) | per-bone nlerp + short extrapolation |
| Player weapon drawn/holstered | owner | EU anim vars | **refused** if the proxy's AI contradicts it | — |
| Player equipment | owner | OUTFIT (event + 10 s repeat), IMODS | converged to the engine's equipped set | — |
| Power Armor | wearer | PA (on change + 5 s repeat while worn), PAI/PAMODS | frame clone + engine enter/exit on the proxy | — |
| World NPC transform/combat | NPC owner (host by default; hand-off to a nearby client) | EU, combat state/target | puppet with AI alive, combat stopped, weapon state enforced | yes |
| World NPC death | **owner only** | KILL (reliable, idempotent) | non-owners: essential-down at most | — |
| World NPC damage | owner applies | DMG from non-owners | — | — |
| Corpses | owner | CREATE+KILL repeat, corpse position | — | — |

**Lost or out-of-order packets:**
- EU is timestamp-ordered; older samples are dropped (`u.timestamp < m_lastHostTime`).
- A destroyed entity cannot be resurrected by a late EU (tombstones, 0.3.3).
- KILL is idempotent against the engine life state.
- The PA state repeats every 5 s, and repeated exits are now no-ops.
- A lost BONES frame is superseded by the next one, and the HOLD/RELEASE tiers cover gaps.

**Thread model (Phase 7 audit, VERIFIED in code).**
- The GNS background thread only receives and queues (`CoSyncTransport` inbox: mutex, hard cap, evicts superseded state first, logs dropped events).
- The game thread drains the inbox in `CoSyncTransport::Tick`, parses in `CoSyncNet::OnReceive`, and subsystems apply queued ops in their `Flush*` calls. The CoSync tick runs as the engine job "NonRendersafeAI".
- The only engine code that runs off the game thread is the new weapon hook. It only reads an SRW-locked map and never touches CoSync state.

---

## 5. Regression analysis (Phase 9)

- **Window:** `4b7168a` (0.3.2 test matrix, passed) … `af77f1f` (0.3.3, regressed). All 0.3.3 commits were diffed. Log message frequencies of the two host logs and the two client logs were compared.
- **Ruled out by evidence:**
  - Host-side capture: identical BONES sizes and capture rates, first-person graph drive unchanged.
  - Bone apply counts on the client: 36/93 vs 37/93.
  - The FormID translation: inactive, load orders matched.
  - The 0x11 model update as a skeleton reload (IDA §2.3).
- **Found:**
  - RC1 (stale cache). The weakness dates from the 2026-08-20 cache. It was *triggered* in the 0.3.3 session by the hide/show cycle of the client's character-editor join.
  - RC2 was introduced by `238f5d5` ("position proxies through the engine": player proxies' controller velocity zeroed every frame, never restored).
  - RC3 (AI holster) is older than 0.3.3. It is more visible now that proxies stay in the world.
- **Not reproducible offline:** whether the 0.3.2 test simply never went through a proxy hide/show. The 0.3.2 logs show no `HIDDEN`/`SHOWN` for the player proxies after their cache was built.

---

## 6. Test matrix (to run in game, host ↔ client both directions)

All rows are **NOT RUN (needs in-game test)**. The offline checks that *were* done:
- the solution builds clean;
- `multiplayer.js` parses (esprima);
- the new menu was rendered in headless Edge with sample state (layout, status strip, ping colours);
- every RVA used was re-read in IDA this session.

| Area | Test | Watch in the log | Expected |
|---|---|---|---|
| Animation | Join through the character editor, then walk/run/sprint/crouch/jump in **third** person (both directions) | `[PlayerAnim] … cache REBUILT (3D root replaced)` after `SHOWN` | walking legs and upper-body actions visible |
| Animation | Same in **first** person | `[TickAnimVars]`; proxy moves | upper body replicated, legs walk (RC2, inferred) |
| Animation | Attack, reload, switch weapons, Pip-Boy | — | visible on the proxy |
| Weapon | Draw a weapon and stand still 30 s, out of combat | `[WeaponAuthority] … refused N AI holster/draw call(s)` | the gun stays visible |
| Weapon | Holster / draw / switch | `weaponDraw` / `weaponSheathe` | follows the owner |
| Weapon | Anything else still makes it vanish | `[WeaponAuthority] … OUTSIDE the AI update … (last caller base+0x…)` | report the RVA |
| Enemies | Fight with the host's enemies nearby | puppet `draws weapon` and stays armed | weapons out during the fight |
| Enemies | Join through the editor near NPCs | NpcSyncDiag no longer STUCK | NPCs visible |
| Enemies | Client shoots an enemy to 0 HP | host `TX KILL`, client `Killing entity` | dies on both, never dead-here/alive-there |
| Power Armor | Enter a frame and watch from the other machine for 30 s | no `FALLBACK exit` on the wearer | stays in PA |
| Power Armor | Exit | `Exit PA … not re-issued (waiting…)` then `confirmed out` | a single rebuild, normal body |
| Power Armor | Two players in two frames at once | — | both correct |
| Revive | Get downed near enemies | no `TX DMG … hp 0.0->` spam | revive works once |
| UI | Open Multiplayer (main menu and pause menu), host, join, factions, voice | — | new terminal layout; ping in the player list |

---

## 7. Open items (NEEDS INVESTIGATION / user decisions)

1. **RC2 consumer (INFERRED).** If a first-person host's legs still slide after 0.3.4, the lower body needs a state-driven driver. That's the "hybrid" that was reverted at the user's call on 2026-08-22. This directive's "replicate state, not frames" supports bringing it back; the user needs to decide.
2. **NPC authority model.** This directive says the host is authoritative for world NPCs. 0.3.1 introduced dynamic ownership hand-off to the nearest client (the user's call in directive 2: multi-targeting, no standing puppets). 0.3.4 keeps one owner at a time and makes death owner-only. Removing the hand-off is a user decision, not done here.
3. **PA exit still ends in one Disable+Enable** (`FinishPaExit` → `RefreshActor3D`) to restore the body after the race changes back. It's not proven that the engine's own exit sequence restores an NPC's 3D by itself.
4. The hook refuses only the verified AI caller. If another engine path turns out to fight the replicated weapon state, the diagnostic names it; extend the refusal only after reading that caller.
