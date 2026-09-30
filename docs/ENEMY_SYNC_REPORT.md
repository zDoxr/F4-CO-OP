# CoSync — Enemy Synchronization: Research Report (3 passes) and Architecture

Directive: "Enemies must be actually multiplayer-synchronized" (2026-09-30).

**Status.** This report was written **before** any code change. Implementation followed it (section 11). Nothing described here has been tested in game yet (section 12).

## Target runtime (correction to the directive)

The directive names Fallout 4 **1.11.191** with F4SE **0.7.7**. The installed game is different:
- `Fallout4.exe` FileVersion **1.11.240.0**;
- `f4se_1_11_240.dll` version **0.7.9**.

CoSync has been 1.11.240-only since P8 (`e41dbe7`), and every hook is prologue-checked against that build. **All addresses below are 1.11.240 RVAs** (image base `0x140000000`), read in IDA 9.3 on the 1.11.240 IDB. They are **not valid for 1.11.191**; on any other build the prologue checks fail and the features switch themselves off.

## Evidence labels

- **VERIFIED**: read in IDA (decompiled or disassembled), in the ESM, or in compiled Papyrus.
- **STRONGLY SUPPORTED**: the structure is read, but the full semantics are not traced.
- **INFERRED**: reasoned, not read.
- **NEEDS TEST**: only a real game session can settle it.

## Tools used

- IDA 9.3 headless on the 1.11.240 IDB, 24 scripted batches.
- A new Fallout4.esm indexer (every record plus subrecords, with zlib handled).
- A new BA2 extractor.
- A new Fallout 4 `.pex` disassembler. The vanilla legendary scripts were read as bytecode, not guessed from names.

---

# PASS 1 — DISCOVERY: how Fallout 4 represents and runs enemies

## 1.1 Who owns enemy AI state

| Question | Answer | Evidence |
|---|---|---|
| Object that owns AI state | `Actor` plus its `AIProcess` at `Actor+0x300`. The combat group comes from `Actor` vfunc `0xF2` (`sub_140CCF3D0`). | VERIFIED: every combat native reads through them (below). |
| Current package | `AIProcess+0x20` (package procedure type at `+0x24`). `sub_140C69490` = GetCurrentPackage, called by the console commands at `0x5CCFA0` and `0x5CD0A0`. | VERIFIED. This corrects a first reading that took it for the combat target. |
| **Current combat target** | **`Actor+0x380`**, a `BSPointerHandle`. The Papyrus `GetCombatTarget` native `sub_1410F8E00` decodes exactly this handle. | VERIFIED on 1.11.240. CoSync's `0x380` was verified on 1.11.221 only; it holds on .240. |
| All combat targets | Actor vfunc `0xF2` gives the combat group. Target array at `+0x08` (0xD0-byte entries, handle at `+0`), count at `+0x18`. `GetAllCombatTargets` native `sub_1410F8B30`. | VERIFIED |
| **Combat state 0/1/2** | `sub_140CCF410(actor)`: <ul><li>not `IsInCombat` (vfunc `0xFE`) → 0;</li><li>combat group `+0x98` ≠ 0 → **2 (searching)**;</li><li>otherwise 1.</li></ul> Used by the `GetCombatState` native `sub_1410F8DF0` and the condition function at `0x5A9E60`. | VERIFIED |
| Target selection / switching | Done inside the combat controller and combat group: the AI picks a target and writes it to `Actor+0x380`. Selection is **not** replicated. Its **result** is. | STRONGLY SUPPORTED (only the result field was traced) |

## 1.2 Attack → hit → damage → death pipeline

```
Attack decision (combat behaviour tree, AI)       -- local to the simulating machine
 ├─ ranged: TESObjectWEAP::Fire   sub_1404799B0    (aim = AIProcess+0xBC)   [0.3.5 SHOT]
 ├─ melee:  graph hit frame → HitData
 └─ explosion / projectile impact → HitData
      │
      ▼
Actor::DoHitMe(target, HitData&)   sub_140CD2CA0   (also task-executor case 84)
 ├─ TESHitEvent send               sub_14052D0B0(HitData*)  at 0xCD2E3A   → Papyrus OnHit
 ├─ damage calc (resists, perks: entry points 0x73/0x4C …)
 └─ ModActorValue(target, 2, Health, -dmg, aggressor)   sub_140CC9B70   at 0xCD3709
          │  (also: magic value modifiers sub_140BA0150 pass the caster as attacker;
          │   the ActorValueOwner slot-6 thunk sub_140CC9B40 passes attacker = 0)
          ▼
      if avi == Health && delta < 0 → Actor vfunc 0x110  CheckForDeath(attacker)   sub_140CD2AE0
          │  !IsDead(0) (vfunc 0xC0, life state) && Health <= 0
          ▼
      Kill core  sub_140C61800(actor, killer, float dmg, bool silent, bool ragdollInstant)
          │  writes killer handle → Actor+0x384
          │  queues task 8: sub_140C463C0(qword_1432FF808, …) → executor sub_140C4D320
          ▼
      KillImpl = Actor vfunc 0x117   sub_140CA00C0(actor, killer, dmg, silent, ragdollInstant)
          ├─ life state → Dying (SetLifeState vfunc 0x105)
          ├─ death animation: TESActionData(ActionDeath) → sub_140E13D70 (graph action);
          │   falls back to the second default action, then ragdoll;
          │   ragdollInstant → sub_140C59400
          ├─ kill credit when killer = PlayerCharacter or a teammate
          │   (perk entry point 9, sub_140CA71B0)
          └─ TESDeathEvent (dying) sub_14052C6F0(victim, killer@+0x384)   → Papyrus OnDying/OnDeath
```

All of the above is **VERIFIED**. The HitData layout comes from its init `sub_140E37510`:
- `+0x40` aggressor handle;
- `+0x44` target handle;
- `+0x48` source ref;
- `+0x90`/`+0x94` damage.

That confirms 0.3.5's DoHitMe gate reads the right field.

**Where the authoritative state lives:**

| State | Location |
|---|---|
| Health | Health ActorValue (`0x2D4`), read through ActorValueOwner `Actor+0x58` |
| Death / life state | `Actor+0x130` bits 17–20: 0 alive, 1 dying, 2 dead, 7 essential-down, 8 bleedout |
| Killer | `Actor+0x384` |
| Target | `Actor+0x380` |

Bleedout (`IsBleedingOut` native `sub_1410F9920`) is **life state 7 or 8**.

`Actor.Kill`/`KillSilent` (`sub_1410F9E60`/`sub_1410F9E90` → `sub_141108D30`) are thin wrappers. They check invulnerability, reset essential-down to alive, and then run the **same kill core**.

## 1.3 Death animation

- **How it is picked.** `KillImpl` performs the default action `ActionDeath` (AACT `0x489ED`) on the actor's **own behaviour graph** (VERIFIED). So the animation is chosen by the local graph and its conditions, and ragdoll is the fallback.
- **Consequence.** A remote machine gets the real, race- and animation-mod-correct death by running the engine's **kill core** on its copy. It needs no forced graph variables.
- **Other actions exist as AACT forms:**
  - Stagger: `ActionStaggerStart 0x138D2`.
  - Knockdown: `ActionKnockDown 0xD1FDC`.
  - Bleedout: `ActionBleedoutStart/Stop 0x13EC9/0x13ECA`.
  - Instant ragdoll: `ActionRagdollInstant 0x9BB4E`.
- **Dismemberment.** It is a HitData-driven step (`sub_140C88E70`, also the `Actor.Dismember` native `sub_1410F83E0`) and is not part of the kill itself.

## 1.4 Legendary enemies (the real system)

| Aspect | Engine / data reality | Evidence |
|---|---|---|
| Identity | **Keyword `EncTypeLegendary` (KYWD 0x1CF26C)** on the resolved NPC template. `LegendaryNPCKeyword_DO` (DFOB 0x2478FA) points to it for the engine (HUD star). There is **no LegendaryRank AV**: `0x1DFDFB`, which CoSync used, is a **REFR** in Fallout4.esm, and no AVIF with that name exists. | VERIFIED (ESM). **CoSync bug**: `LGD` could never be sent. |
| Decided when | At spawn. Leveled lists such as `LCharRaiderLegendary` mix legendary and non-legendary templates by level (`EncRaider01Template` … `EncRaider03Legendary`). The chosen template carries the keyword, one ability spell (`AbLegendary2xDmg`/`Radioactive`/`Poison`/`Accuracy`/`Explode`) and a perk. `LegendaryChanceQuest` only sets the `ChanceLegendary` global (10–20 % by player level). | VERIFIED (ESM + `QF_LegendaryChanceQuest_00165570.pex`) |
| Mutation trigger | `AbLegendaryScript` (ActiveMagicEffect on the ability):<ul><li>**OnEffectStart**: `RegisterForHitEvent(self)`.</li><li>**OnHit**: if `GetValuePercentage(Health) < HealthThreshhold` and not `PoweredUp`, it calls `LegendaryPowerUp.Cast(target, target)`. If its combat target is the player it shows `LegendaryPowerUpMsg` and plays a sound. It then sets `PoweredUp = true`, otherwise it re-registers.</li></ul> | VERIFIED (`AbLegendaryScript.pex` disassembled) |
| Threshold | **0.5** for all four mutating abilities | VERIFIED (VMAD of MGEF 0x1FA243/0x1FE6AD/0x1FE6B1/0x1FE6B5/0x1FE6BB) |
| Mutation effect | Power-up spell, e.g. `Legendary2xDmg` (0x1FA245) = `RestoreHealthGeneric` (magnitude 9999, a full heal) + `Legendary2xDmgEffect` (duration 900 s, shader `Legendary2xDmgEffectFXS`). The others are `LegendaryRadioactive` 0x1FE6AE, `LegendaryPoison` 0x1FE6B4 and `LegendaryAccuracy` 0x1FE6BA. | VERIFIED (ESM) |
| Cast path | Papyrus `Spell.Cast` native **`sub_14113F810`**. It builds a `CastFunctor {spell, source handle, target handle}` and queues it (`sub_1410EB260(qword_1430EB308)`); the cast runs later on the game thread. | VERIFIED |
| Legendary item | `AbLegendaryCreatureItem` → `LegendaryCreatureItemEffectScript.OnEffectStart` → `LegendaryItemQuest.GenerateLegendaryItem(actor, …)`. The item is added **to the actor's inventory when the effect starts**. | VERIFIED (`.pex`) |
| Explode variant | `AbLegendaryExplodeScript.OnDeath`: `PlaceAtMe(DeathExplosion)` → `LegendaryEncExplosion` (0x249F36) | VERIFIED |
| Persisted? | `PoweredUp` is a script variable on the active effect, so it is saved with the actor on the host. | INFERRED (standard Papyrus persistence) |

**What this means for the network.**
- Mutation is **not an engine state flag**. It is a **script decision** driven by `OnHit`, and its result is a **self-cast of a spell**.
- Two consequences for CoSync:
  1. Damage a client deals is applied on the host with `ForceCurrent` (the slot-6 thunk, no hit event). That **never raises `OnHit`**, so an enemy damaged only by clients can **never mutate** (Pass 2, G3).
  2. The machine to replicate from is the host's `Spell.Cast` of the power-up. **Every** clone of a legendary template on a client carries the same ability script and could mutate locally on its own.

## 1.5 Boss classification

- **There is no universal runtime "boss" flag. VERIFIED.**
- What exists:
  - **Location Ref Type `Boss`** (LCRT **0x3956**) on the **placed reference** (XLRT). **373** ACHRs in Fallout4.esm carry it. `BossLocRefTypeList` (FLST 0xB6EE9, via DFOB `BossLocRefTypeList_DO`) = { Boss, SettlementLeader, SettlementRobot }.
    - The location "cleared" logic and the workshop scripts use it; scripts can also set it at runtime (`SetLocRefType`).
    - Engine test: `ObjectReference.HasLocRefType` native `sub_141158090` → `sub_1405645C0(ref, lcrt)`.
  - Boss **templates** and leveled lists (`LCharRaiderBoss`, `LCharGunnerBoss`, …, `Outfit_*_Boss`, loot `*_Boss`). The ability `BossAbility01` (SPEL 0xD3FC6) gives +25 health, +10 damage resist and +2 perception.
  - `MUSzCombatBoss*` combat music (presentation only).
- **What matters for synchronization:**
  - The boss **template** travels with the base form: the puppet is built from the same static template.
  - The **LocRefType** is a property of the host's reference. It is replicated as a classification bit, for display and diagnostics only.

## 1.6 Other classifications (all ESM keywords, VERIFIED)

| Class | Keyword |
|---|---|
| NPC | `ActorTypeNPC` 0x13794 |
| Creature | `ActorTypeCreature` 0x13795 |
| Animal | `ActorTypeAnimal` 0x13798 |
| Robot | `ActorTypeRobot` 0x2CB73 |
| Turret | `ActorTypeTurret` 0xB2BF3 |
| Super mutant | `ActorTypeSuperMutant` 0x6D7B6 |
| Ghoul | `ActorTypeFeralGhoul` 0x6B4F2 |
| Legendary | `EncTypeLegendary` 0x1CF26C |

- **Unique/named**: the ACBS Unique flag on the base.
- **Power armor enemy**: the ExtraPowerArmor (`0xBB`) state CoSync already reads.

The engine keyword test is `Form.HasKeyword` native **`sub_141143FC0`**. It goes through `sub_141144EE0`, which walks the reference and base keyword forms.

## 1.7 Safe to replicate vs. let the engine simulate

**Replicate (authoritative results):**
- existence;
- transform;
- equipped weapon and draw state;
- combat state and target;
- each shot (existing);
- damage **as hit reports**;
- health;
- life state;
- death with **killer** and ragdoll flag;
- legendary / boss / type classification;
- script self-casts (mutation).

**Let every machine's engine do (presentation):**
- locomotion clips from velocity;
- the death animation (the local `ActionDeath`);
- hit reactions from local hits;
- projectile flight and impact effects;
- the mutation shader and heal (by replaying the same cast);
- the ragdoll physics.

**Never replicate:**
- package evaluation;
- pathfinding;
- target-selection internals;
- per-tick AI;
- graph variables of the death.

---

# PASS 2 — VALIDATION against the real CoSync code

## 2.1 What CoSync already has (read, not assumed)

| Area | Where | What it does |
|---|---|---|
| Transport | `CoSyncTransport` / `GNS_Session` | GNS. `Send` is reliable by default, and reliable messages are ordered per connection. The host relays. The network thread only queues into the inbox. |
| Game-thread inbox | `CoSyncNet::Tick` → `OnReceive` | All routing runs in the tick (engine job "NonRendersafeAI"), plus per-module `OnReceive*` → mutex queue → `FlushPending*`. |
| Sender checks (P5/N7) | `CoSyncNet.cpp` `SelfStateType`/`HostSenderOwns` | The first field must be the sender or an NPC the sender owns. **KILL, DMG and LGD were NOT checked** (DMG by design). |
| Entity registry | `g_CoSyncEntities`, `CoSyncPlayerManager` states, `NpcWorldSync` `s_eidToActor` | World actors are IDs 0x10000–0x1FFFF, allocated only by the host. |
| CREATE / UPDATE / DESTROY | `Packets_Entity*.h`, `EntitySerialization.h` | CREATE is the spawn trigger. UPDATE (`EU`) only updates. **VERIFIED: UPDATE never spawns** (`EnqueueEntityUpdate` into an existing state). DESTROY carries a Dead flag. |
| Enemy discovery | `NpcWorldSync::TickScanWorldActors` | Walks ProcessLists High and MiddleHigh plus the cell. It registers, re-broadcasts CREATE every 5 s, and sends KILL for dead actors. |
| Authority | `NpcWorldSync::HostTickOwnershipHandoff` | **Host-authoritative wherever the host's engine simulates the NPC.** A client owns an NPC only outside the host's loaded area (an engine limit: actors are simulated only near a player), and the host takes it back as soon as it is near. PEERLEFT releases a client's NPCs. |
| Transform | `EU` (~15 Hz, rotation cap 24/tick, 20480 u), interpolated 120 ms behind | unchanged |
| Combat flag + target | `EU.isCombatActive` / `combatTargetEntityID` from `Actor+0x380` | **falls back to the "nearest player" when the real target is not synced (fabricated)** |
| Shots | `CoSyncCombatAuthority` SHOT + `Fire` hook | shooter's machine only; replayed through the engine |
| Hit authority | `Detour_DoHitMe` | refuses hits whose aggressor is simulated elsewhere |
| Damage by clients | `HealthSync::TickDamageDetection` | **HP-delta polling of puppets every 0.15 s → `DMG|eid|amount`**, with no attacker. The owner applies it with `SafeForceDamage` (slot-6 thunk, **attacker 0, no hit event**). |
| Health | `HP` from the owner (1 Hz scan + 4 s baseline) | the authoritative correction |
| Death | `KILL|eid` from the owner's **poll** (1 Hz scan, plus a second poll in `HostSendNpcHealth`) | receiver: `KillPuppet` = `SetLifeState(Dying)` + `KillDying` (vfunc 0xCB) |
| Puppet protection | the runtime-essential bit (`uiFlags 0x40000`) on non-owned puppets | local lethal damage becomes essential-down (7) instead of death |
| Legendary | `LGD|eid|rank` from the "LegendaryRank" AV | **the AV does not exist, so it never fires** |
| Corpse | `CORPSE` after 4 s | unchanged |

**Authority check.** The directive's premise ("host authoritative for NPCs") is **verified, with one explicit, existing exception**: client ownership outside the host's loaded area. That is an **engine limit, not a design choice**, and it is kept.

**No authority migration is added.** The existing handoff is the only one, and every rule below is phrased as "**owner** = the machine simulating it (the host whenever it can)".

## 2.2 Gaps found (each proven from the code or IDA)

| # | Gap | Consequence |
|---|---|---|
| G1 | `KILL` is not sender-checked; the host relays it and applies it to **its own real NPC** (`KillPuppet(proxy->actorRef)`, where the host's `actorRef` is the real actor) | **Any client can kill any host enemy**, which is client-authoritative death |
| G2 | KILL carries no killer; the receiver bypasses `KillImpl` | Client corpses skip `ActionDeath` (no native death animation), `OnDeath` and kill credit. The host's own client-caused kills have killer **None** (slot-6 thunk, attacker 0). |
| G3 | Client damage is applied as `ForceCurrent` | No `TESHitEvent` → no `OnHit` → **legendary mutation never triggers for client damage**, and quest `OnHit` scripts never see it. |
| G4 | `LGD` uses REFR 0x1DFDFB as an AVIF | Legendary state is never replicated; mutations aren't either. |
| G5 | `combatTargetEntityID` is fabricated as the nearest player | "Enemy X → target Player 2" can be false on every client |
| G6 | Death is detected by polling (1 Hz), twice | Up to 1 s of latency, duplicate sends, and no killer |
| G7 | A puppet's own AI and ability scripts can **self-cast** (mutate) locally | A client-local mutation diverges from the host (health, shader, damage buff) |
| G8 | Runtime-essential puppets that take lethal local damage enter **essential-down (life state 7)** | The enemy kneels on one client while it fights on the host |
| G9 | `DMG` for world NPCs is an aggregated HP delta, fed by any local HP drop (hence the combat gate) | Wrong attribution. Double counting is possible when several machines see the same local drop. |

## 2.3 Where enemy sync belongs (no second architecture)

- **One new module, `World/CoSyncEnemySync`,** sits beside `CoSyncCombatAuthority`, using:
  - the same transport (`CoSyncTransport::Send/SendExcept`);
  - the same inbox path (`CoSyncNet::OnReceive` → queue → game-thread flush);
  - the same sender-check table;
  - the same entity IDs.
- **What it adds:** four engine hooks (prologue-checked, all-or-nothing per hook) and three small messages.
- **What stays:** CREATE/UPDATE/DESTROY, EU, SHOT, HP and CORPSE are unchanged, apart from the targeted fixes G1/G5 in place.

---

# PASS 3 — ADVERSARIAL REVIEW (and the corrections it forced)

The first design (after Pass 2) was:
- hook `KillImpl` to capture deaths;
- make the receiver call the kill core;
- capture client damage in `DoHitMe`;
- send an `EMUT` event for mutations.

Pass 3 broke it in these places:

| Attack on the design | What broke | Correction |
|---|---|---|
| A puppet's local lethal damage calls the kill core → `sub_140C463C0` marks the actor "kill queued" (`sub_140C61B90`) **before** the task runs | A gate at `KillImpl` would leave that mark set, and the later authoritative kill would be **dropped** by `!sub_140C61B60` | Gate at the **kill core entry** (`sub_140C61800`), before any side effect. Capture stays at `KillImpl`. |
| The replayed kill is a queued task; the PlayerManager tick **re-sets runtime-essential** every frame | `KillImpl` runs one frame later and `SetLifeState(Dying)` becomes **essential-down** (7): a kneeling puppet instead of a death | The tick no longer re-arms essential on an entity with an applied KILL. The replay clears the bit first. There is a 3 s watchdog falling back to the old `KillPuppet`. |
| Damage capture in `DoHitMe` | Misses DOT (fire/poison/cryo value modifiers), which never pass through `DoHitMe` | Capture at **`ModActorValue` `sub_140CC9B70`**, the single choke point. `DoHitMe` passes the aggressor, and magic effects pass the caster (VERIFIED). |
| Environmental damage (fire hazard, falling) on a puppet, attacker 0 | Every client would report it, so the host applies it N times | Report **only** when the attacker is simulated **here** (the local player or an NPC this machine owns). Attacker 0 is never reported: the owner's own engine applies environmental damage to its real actor. |
| CoSync's own HP corrections (`ForceDamage`) pass through `ModActorValue` | They would be reported back as damage | They run with attacker 0 (not reported), plus a thread-local "applying" guard. |
| Hooks run on the main, job and VM threads; `NpcWorldSync` maps are game-thread-only | A data race if a hook resolves entity IDs | Hooks only test a **thread-safe guard set** (maintained by the tick where the essential bit is managed) and push `{handle, formID, value}` under a mutex. The tick resolves entities. |
| `EMUT` event lost for a late joiner, or the player out of range | A mutated enemy looks un-mutated | Mutation is **state**, not an event: `ENS.casts` lists the spells the owner's scripts self-cast; it is applied once per puppet and re-sent in the 5 s snapshot. |
| Only legendary mutations are captured | Other scripted self-casts (special enemy behaviour) would be missing or local | Capture **every** Papyrus self-cast by an owned world actor (throttled 4/s, last 4 kept). **Refuse every** Papyrus self-cast on a puppet unless CoSync replays it. |
| A client-local `AbLegendaryScript` on a puppet | Local mutation, i.e. divergence (G7) | Refused by the same `Spell.Cast` gate (`source == target == guarded puppet`). |
| Two clients report the enemy dead | — | Clients cannot: KILL is accepted **only from the owner** (sender check), and puppets cannot die locally (kill-core gate). |
| Two players damage at once | — | Each reports only its own damage (`EHIT` with its own attacker). The owner applies them in arrival order on its game thread; health is single-sourced and credit goes to whichever hit crossed 0 (the engine's `CheckForDeath(attacker)`). |
| The same `EHIT` twice (relay, retry) | Double damage | Per-attacker sequence numbers; the owner drops seq ≤ last. |
| `EHIT` after death | Damage to a corpse | The owner drops hits on dead or dying targets. |
| DEATH before DAMAGE | — | One owner, one reliable ordered channel, so HP is always before KILL. HP arriving after KILL is ignored on a dead or killed puppet (new guard). |
| TARGET before the target's CREATE | — | Targets are entity IDs, resolved lazily each use. ENS and EU never spawn anything, and an unresolved target is simply "not visible here". |
| A target switching 5× per second | Floods | ENS is **state** with at most 5 sends/s/entity; trailing edge, latest value wins. |
| Attack 300 ms late | — | Shots over 1 s old are dropped (0.3.5). Damage never depends on the replayed shot. |
| Late join during a 10-enemy fight | — | CREATE+KILL (5 s), ENS snapshot (5 s), HP baseline (4 s) and the EU stream: converged within ~5 s. |
| Spawn while a client loads | — | The inbox keeps events (cap 65536, evicts EU/BONES first). CREATE re-broadcast plus the deferred spawn watchdog (existing). |
| Unload / cell reset / respawn | — | Departure → DESTROY(Dead flag) as before. The ENS state for that ID is dropped. A reset or respawned actor is a new discovery with a new ID. |
| A different animation or mod setup | A different death clip | The gameplay state is identical; the clip is the local graph's choice (`ActionDeath`). The 0.4.0 content sync makes the setups the same. |
| Custom legendary / boss mods | — | Keyword-based (mods reuse `EncTypeLegendary` for the HUD star); cast-based mutation is generic; boss via `BossLocRefTypeList` membership (mods can extend the FLST). FormIDs go through `CoSyncFormIdMap` (host load order canonical). |
| 20+ enemies, 4–8 players | Bandwidth | The only per-tick stream stays EU (capped). Enemy additions are per event: EHIT ≈ 40 B per damaging hit (at most 60/s per attacker), KILL per death, ENS ≈ 70 B per change and per 5 s. 64 enemies ≈ 0.9 KB/s snapshot. |

---

# 5. Authoritative enemy state (what is replicated)

| Field | Source on the owner | Wire | Receiver |
|---|---|---|---|
| Entity ID, base form, ref, location | registry | CREATE (existing) | spawn or bind |
| Transform, weapon, draw, anim | engine | EU (existing) | puppet |
| **Combat state 0/1/2** | `sub_140CCF410` | **ENS** | stored; drives nothing new |
| **Target entity** (truthful; 0 = none or unsynced) | `Actor+0x380` → entity | **ENS** (+ EU, now truthful) | stored, resolved lazily |
| **Class flags**: legendary, boss, unique, essential, robot, turret, creature, humanoid, power armor | keywords / LocRefType / ACBS / ExtraPowerArmor | **ENS** | stored, logged |
| **Self-casts** (the mutation spell list, max 4) | `Spell.Cast` hook | **ENS** | same engine cast on the puppet, once |
| Life state (0/1/2/7/8) | `Actor+0x130` | **ENS** | stored. Death is applied by KILL; bleedout is display only (see open items) |
| Health | Health AV | HP (existing) | correction |
| **Death + killer + ragdoll flag** | `KillImpl` hook | **KILL** (extended) | engine kill core on the puppet |
| **Hit** (target, attacker, damage) | `ModActorValue` hook on a puppet | **EHIT** (non-owner → owner) | owner: `TESHitEvent` + `ModActorValue(attacker)` |

# 6. Packet model

| Message | Direction | Reliability | Ordering / idempotence |
|---|---|---|---|
| `CREATE` / `DESTROY` / `EU` / `HP` / `SHOT` / `CORPSE` | unchanged | unchanged | unchanged |
| `KILL|eid|killer|flags` | owner → all | reliable | final. Receivers skip if already dead. Only the owner may send it (host check). |
| `EHIT|target|attacker|seq|damage|flags` | the attacker's machine → host → owner | reliable | per-attacker sequence number. Host check: the sender must own `attacker`. |
| `ENS|eid|ver|combat|target|class|life|casts` | owner → all (host relays) | reliable | per-entity version, monotonic. On change (≤ 5/s) and every 5 s. |
| `LGD` | **retired** (broken since it was written) | — | — |

# 7. Event and state flow

```
SPAWN (host engine)
 → REGISTER   (NpcWorldSync scan)            CREATE (+ENS snapshot)      clients: bind or spawn puppet
 → ACTIVE     EU stream, HP                  puppets steered
 → COMBAT     ENS combat=1 / target=eid      (EU flag + truthful target)
 → ATTACK     SHOT (ranged) / AE graph inputs (melee)     puppet replays; damage refused (DoHitMe gate)
 → DAMAGE
     host-simulated attacker: host engine only (DoHitMe) → HP
     client-simulated attacker: client's puppet takes it locally (prediction)
        → EHIT → host: TESHitEvent (OnHit) + ModActorValue(attacker)
        → HP (the correction)
 → LEGENDARY  host OnHit → AbLegendaryScript → Spell.Cast (hook) → ENS casts → clients cast the same spell
 → DEATH      host CheckForDeath(attacker) → kill core → KillImpl (hook) → KILL|eid|killer|ragdoll
              clients: kill core(puppet, killer→local actor) → KillImpl → ActionDeath animation,
              TESDeathEvent; XP only where killer = the local player
 → CORPSE     CORPSE after 4 s (existing)
 → DESTROY    departure, with the Dead flag (existing)
```

# 8. IDA findings (1.11.240) — used by the implementation

| Function | RVA | Convention / arguments | Returns | Owner | Callers / xrefs | Verified by |
|---|---|---|---|---|---|---|
| Kill core | `0xC61800` | `(Actor* victim, Actor* killer, float dmg [xmm2], bool silent [r9b], bool ragdollInstant)` | ptr (ignored) | Actor | 19: CheckForDeath `0xCD2AE0`, the `Kill` worker `0x1108D30`, `0xD3C440`, `0xC63BC0`, … | decompile + disassembly (`movaps xmm6,xmm2`, `movzx esi,r9b`); prologue `48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 40` |
| KillImpl | `0xCA00C0` | vfunc `0x117`; `(Actor*, Actor* killer, float, bool, bool)` | — | Actor (vtable `0x25683B0`) | task case 8, `0xD78A30` | vtable slot read; task-8 decompile; prologue `44 88 4C 24 20 48 89 54 24 10 55 53 56` |
| ModActorValue | `0xCC9B70` | `(Actor*, int mode, ActorValueInfo*, float delta, Actor* attacker)` | — | Actor | 11 incl. DoHitMe `0xCD3709`, magic `0xBA0150`, thunk `0xCC9B40` | decompile (calls vfunc `0x110` with the attacker); prologue `48 89 5C 24 18 48 89 6C 24 20 57 41 56 41 57` |
| CheckForDeath | vfunc `0x110` = `0xCD2AE0` | `(Actor*, Actor* attacker)` | — | Actor | `0x5BF2A0`, `0xCC9B70`, `0xCCAB40`, `0x11543B0` (vcall scan) | decompile |
| TESHitEvent send | `0x52D0B0` | `(HitData*)` | — | ScriptEventSource | DoHitMe `0xCD2E3A` | decompile; prologue `40 53 48 81 EC 30 01 00 00` |
| HitData ctor / init / dtor | `0xE36380` / `0xE37510` / `0xE366A0` | ctor `(HitData*)`; init `(HitData*, Actor* aggressor, Actor* target, float dmg)`; dtor `(HitData*)` | — | HitData (0xE0) | 18 callers, incl. `Actor.Dismember` `0x10F83E0` (which uses init with null actors) | decompile |
| Spell.Cast native | `0x113F810` | `(vm, stackId, SpellItem*, REFR* source, REFR* target)` | bool | GameScript | registered in `0x113FAC0` | registration disassembly + decompile (latent functor) |
| Form.HasKeyword native | `0x1143FC0` | `(vm, stackId, TESForm*, BGSKeyword*)` | bool | GameScript | `0x1144480` | registration disassembly + decompile |
| REFR HasLocRefType | `0x5645C0` (via the native `0x1158090`) | `(REFR*, BGSLocationRefType*)` | bool | REFR | native `0x1158090` | decompile |
| GetCombatState impl | `0xCCF410` | `(Actor*)` | 0/1/2 | Actor | native `0x10F8DF0`, condition `0x5A9E60` | decompile |
| Combat target | `Actor+0x380` | handle | — | Actor | the `GetCombatTarget` native `0x10F8E00` | decompile |
| Killer | `Actor+0x384` | handle | — | Actor | written by the kill core, read by `KillImpl` | decompile |
| Life state | `Actor+0x130` bits 17..20 | — | — | Actor | `IsBleedingOut` `0x10F9920`, `IsDead` vfunc `0xC0` | decompile |

# 9. Failure handling (summary)

- **A hook fails its prologue check:** that feature is off, logged once, and the old behaviour remains:
  - kill gate/capture → poll KILL + `KillPuppet`;
  - hit capture → `TickDamageDetection` keeps covering world NPCs;
  - cast hook → no mutation replication.
- **Every engine call CoSync makes is SEH-isolated.**
- **An engine kill replay that has not produced a death after 3 s** falls back to `KillPuppet`, logged.

# 10. Three-pass conclusion

## PASS 1 findings
- The engine's authoritative enemy state is:
  - `Actor+0x380` (target);
  - `+0x384` (killer);
  - `+0x130` (life state);
  - the Health AV;
  - the combat group (searching).
- Death is one pipeline: kill core → task 8 → `KillImpl` (`ActionDeath` animation, death event, credit).
- Hits raise `TESHitEvent` inside `DoHitMe`.
- Legendary mutation is a **script** (`AbLegendaryScript.OnHit` < 50 % → `Spell.Cast(power-up)`), identified by the keyword `EncTypeLegendary`.
- "Boss" is a **LocRefType on the reference** plus boss templates. It is not an engine flag.

## PASS 2 validation
- CoSync already has the right skeleton: host authority where the host simulates, CREATE-only spawning, SHOT, a DoHitMe gate, HP and KILL.
- Pass 1 showed nine concrete gaps (G1–G9). The worst:
  - clients could kill host enemies (**G1**);
  - client damage cannot trigger legendary mutation (**G3**);
  - legendary sync never worked (**G4**);
  - death skipped the engine's `KillImpl`, i.e. no native death animation or killer (**G2**).

## PASS 3 adversarial results
Pass 3 moved:
- the kill gate from `KillImpl` to the **kill core** (to avoid a stuck "kill queued" mark);
- damage capture from `DoHitMe` to **`ModActorValue`** (DOT);
- mutation from an **event** to **state** (late join, lost packets).

It also added:
- a per-attacker hit sequence;
- a thread-safe puppet guard set;
- the essential-bit race fix;
- a replay watchdog;
- a rule that only damage from attackers simulated **here** is reported.

---

# 11. Implementation (after the three passes)

- **Version:** 0.4.1 (network 401). This is a patch bump for new messages, following 0.3.3/0.3.5 practice, so peers must match.

**New files: `World/CoSyncEnemySync.{h,cpp}`**
- Four hooks:
  - kill-core gate;
  - `KillImpl` capture;
  - `ModActorValue` hit capture;
  - `Spell.Cast` capture and gate.
- `EHIT` / `ENS` / extended `KILL`.
- The owner-side hit application (`TESHitEvent` + `ModActorValue` with the attacker).
- Engine-kill replay with a watchdog.
- Cast replay.
- Classification.
- A 10 s diagnostic line: `[EnemySync] last 10 s: …`.

**Changed files**

| File | Change |
|---|---|
| `CoSyncNet.cpp` | Routes `EHIT`/`ENS`. Sender checks for `KILL`/`ENS` (owner only) and `EHIT` (sender owns the attacker). Calls `EnemySync::Tick`/`Reset`. |
| `CoSyncHealthSync.cpp/.h` | `KILL|eid|killer|flags`. The world-NPC kill uses the engine replay. HP is ignored for killed puppets. Hit polling for world NPCs is off while the hit hook is live. |
| `CoSyncNpcWorldSync.cpp` | Poll KILL only if not already announced by the hook. The broken `LGD` send is removed. |
| `CoSyncPlayerManager.cpp` | Poll KILL deduplicated. The puppet guard set is maintained with the essential bit (not re-armed after KILL). **Truthful combat target** (the nearest-player fabrication is removed). |
| `CoSync_Plugin.cpp` | `CoSyncEnemySync::Install()` |

# 12. Verified by static analysis vs. requires real multiplayer testing

**VERIFIED BY CODE / STATIC ANALYSIS**
- Every engine address, argument order and prologue above.
- The legendary logic, threshold and power-up spells (the game's own bytecode and data).
- The boss and type classification data.
- KILL can no longer come from a non-owner (sender check in code).
- UPDATE, ENS and EHIT never create entities (code paths).
- No hook touches game-thread maps: they only use the guard set and queues.
- Release build: 0 errors (section 13).

**REQUIRES REAL FALLOUT 4 MULTIPLAYER TESTING (all NOT RUN)**
1. One normal enemy attacks one player: host `ENS … combat=1 target=<player>`, and no duplicate damage.
2. An enemy switches between two players: `ENS target` changes on each client (`[EnemySync] RX ENS`).
3. Several enemies attack different players.
4. The client damages an enemy: client `TX EHIT`, host `applied EHIT … hitEvent=1`, then HP back.
5. Several players damage one enemy: per-attacker sequences, and a single health line.
6. An enemy damages a player: the existing `DMG` path, once.
7. An enemy kills a player: the existing revive/DOWN path.
8. A player kills an enemy: host `TX KILL … killer=<client eid>`; the client's death animation (`replayed engine kill`); **XP only on that client**.
9. Normal death animation (not an instant ragdoll).
10. Melee death.
11. Ranged death.
12. Explosive death (`ragdoll=1`).
13. A legendary enemy: ENS class shows `legendary`.
14. Legendary mutation caused by **client** damage: host `captured self-cast … Legendary2xDmg`, and clients `replayed cast` (shader plus full heal on both).
15. A legendary killed after mutation.
16. A boss (e.g. `DN009_GunnerBossREF`): class `boss`.
17. A named unique enemy.
18. A creature.
19. A robot.
20. A turret.
21. Several deaths at once.
22. Late join during combat: ENS and KILL snapshots within 5 s.
23. A player disconnects during combat: the host retargets and ENS follows.
24. The host disconnects: the session ends (no migration).
25. Enemy unload and reload.
26. A cell transition.
27. A modded enemy.
28. A custom enemy weapon.
29. A custom enemy animation.
30. 20+ enemies in combat: check the `[EnemySync]` byte counters.

# 13. Open items (honest limits)

- **Bleedout / essential-down of enemies** is replicated as state (ENS `life`) but not re-enacted on puppets. It needs an engine-verified entry for `ActionBleedoutStart` on a remote actor.
- **Dismemberment and limb damage** are not replicated. The host's `DoHitMe` decides them locally; the engine entry exists (`sub_140C88E70`) for a later pass.
- **Client damage uses the client's own difficulty multiplier** (the number the puppet's `DoHitMe` produced). The host applies that number (INFERRED equivalent when both play at the same difficulty).
- **The ragdoll impulse direction** isn't replicated. `CORPSE` settles the position.
- **The host `TESHitEvent` carries no weapon or projectile** (`akSource`/`akProjectile` = None). Vanilla `AbLegendaryScript` doesn't read them; a mod script filtering on weapon wouldn't fire.
- **Script casts on *other* targets** (an enemy script casting at a player) remain host-local.
