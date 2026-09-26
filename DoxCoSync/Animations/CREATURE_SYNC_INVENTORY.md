# Creature / enemy sync inventory — Fallout4.esm (1.11.221)

Generated 2026-08-03 from the user's own dumper
(`C:\Users\hotto\Documents\FalloutDumperOutput\Plugins\Fallout4.esm\records`),
not from a wiki. Source records: `RACE` (45), `NPC_` (3015).

Method: every `NPC_` was joined to its `RNAM` (race), then to that race's
`ANAM` (skeleton path). "hostile" counts NPCs whose `AIDT` byte 0 (aggression)
is >= 1 (`1=Aggressive, 2=VeryAggressive, 3=Frenzied`).

> **Caveat on the hostile column — read this before trusting it.**
> It *undercounts*. Many creature NPCs (Mirelurk and RadRoach most visibly,
> which show 0 despite obviously being hostile in game) carry a `TPLT`
> template and inherit AI Data from it instead of storing their own `AIDT`.
> Treat the column as "confirmed hostile", never as "the others are safe".
> **Species presence, skeleton, and NPC count are exact; aggression is a floor.**

## Why the skeleton column is the one that matters

Sync cost is driven by the **behaviour graph**, and the graph follows the
skeleton — not the race. That produces the single most useful fact here:

- **1998 of 3015 NPCs (66%) already use `Character\CharacterAssets\skeleton.nif`** —
  HumanRace, SynthGen1, SynthGen2, SynthGen2Valentine, Ghoul, GhoulChild,
  HumanChild. These ride the humanoid sync that already works.
- The remaining **1017 NPCs span 31 distinct non-human skeletons**, each with
  its own animation-variable vocabulary. That is the actual work.
- Confirmed-hostile NPCs on those non-human skeletons: **585**.

Note `CoSyncCreatureAnimSync` currently detects "creature" only as
`varSetSize < kHumanoidMinSize (290)` and blind-copies the whole variable set.
It has no per-race handling at all — and since build 220-221 adopted AI-alive
puppets (which skip `TickAnimVars` because it fought native animation), that
path may be effectively dead for enemies. Verify before building on it.

## Full inventory, ordered by confirmed-hostile count

| Race | NPCs | hostile | Skeleton |
|---|---:|---:|---|
| HumanRace | 1558 | 834 | Character *(covered)* |
| SuperMutantRace | 134 | 119 | Supermutant |
| HandyRace | 120 | 89 | Robot |
| SynthGen1Race | 118 | 69 | Character *(covered)* |
| GhoulRace | 74 | 48 | Character *(covered)* |
| FeralGhoulRace | 76 | 47 | FeralGhoul |
| SynthGen2RaceValentine | 166 | 44 | Character *(covered)* |
| SynthGen2Race | 42 | 41 | Character *(covered)* |
| ProtectronRace | 47 | 38 | Robot |
| AssaultronRace | 33 | 24 | Robot |
| SentryBotRace | 42 | 23 | CreateABot |
| DeathclawRace | 36 | 22 | Deathclaw |
| YaoGuaiRace | 19 | 18 | YaoGuai |
| RadScorpionRace | 28 | 17 | Radscorpion |
| MoleratRace | 28 | 16 | Molerat |
| BloodbugRace | 29 | 15 | Mosquito |
| VertibirdRace | 18 | 14 | Vertibird |
| RaiderDogRace | 16 | 14 | Dogmeat |
| StingwingRace | 26 | 13 | Stingwing |
| FeralGhoulGlowingRace | 22 | 13 | FeralGhoul |
| RadStagRace | 14 | 13 | RadStag |
| BloatflyRace | 23 | 12 | Bloatfly |
| SupermutantBehemothRace | 14 | 11 | SuperMutantBehemoth |
| MirelurkHunterRace | 22 | 10 | MirelurkHunter |
| MirelurkKingRace | 17 | 9 | MirelurkKing |
| ViciousDogRace | 36 | 7 | Dogmeat |
| TurretBubbleRace | 27 | 7 | Turret (Mounted) |
| FEVHoundRace | 12 | 6 | FEVHound |
| TurretTripodRace | 57 | 5 | Turret (Standing) |
| MirelurkQueenRace | 8 | 5 | MirelurkQueen |
| GorillaRace | 6 | 5 | Gorilla |
| TurretWorkshopRace | 5 | 5 | Turret (Workshop) |
| LibertyPrimeRace | 4 | 4 | LibertyPrime |
| AlienRace | 2 | 2 | Alien |
| DogmeatRace | 2 | 1 | Dogmeat |
| PowerArmorRace | 1 | 1 | PowerArmor |
| HumanChildRace | 39 | 0 | Character *(covered)* |
| **MirelurkRace** | 34 | 0 † | Mirelurk |
| **RadRoachRace** | 19 | 0 † | RadRoach |
| BrahminRace | 16 | 0 † | Brahmin |
| EyeBotRace | 14 | 0 | EyeBot |
| CatRace | 10 | 0 | Cat |
| GhoulChildRace | 1 | 0 | Character *(covered)* |

† template-inherited aggression — hostile in practice, see caveat above.

`HumanRaceSubGraphData` and `DefaultRace` exist as RACE records but have zero
NPCs and are not sync targets.

## Suggested tiering

Ordered by (confirmed hostile x how often you actually meet them), and
grouped so each tier shares skeletons — one tier is one coherent chunk of work.

- **Tier 1 — the Commonwealth's bread and butter (4 skeletons, ~400 NPCs).**
  SuperMutant(+Behemoth), FeralGhoul(+Glowing), Deathclaw, Dogmeat-skeleton
  dogs (ViciousDog/RaiderDog/Dogmeat). Behemoth shares nothing with
  SuperMutant despite the name — separate skeleton, budget it separately.
- **Tier 2 — robots (3 skeletons, ~242 NPCs).** Robot (Mr Handy / Protectron /
  Assaultron all share it — best NPC-per-skeleton ratio in the game),
  CreateABot (SentryBot), EyeBot. High value for the effort.
- **Tier 3 — turrets (3 skeletons, 89 NPCs).** Mounted / Standing / Workshop.
  Mostly stationary, so position sync is trivial and only the yaw/pitch aim
  and fire state actually need streaming. Cheap win, and required for #4
  (settlement sharing) anyway.
- **Tier 4 — critters (8 skeletons).** Radscorpion, Molerat, Mosquito
  (Bloodbug), Stingwing, Bloatfly, RadRoach, YaoGuai, RadStag.
- **Tier 5 — mirelurks (4 separate skeletons).** Mirelurk, MirelurkHunter,
  MirelurkKing, MirelurkQueen — four distinct graphs for one "family", so this
  tier is more expensive than its NPC count suggests.
- **Tier 6 — set pieces / passives.** Vertibird (also a mount — needs its own
  design), LibertyPrime, Alien, Gorilla, Brahmin, Cat, FEVHound, PowerArmor.

## Related records already located for other work

- `EXPL` 238 records, `PROJ` 139 — the source data for item #9 (explosion sync).
- `LVLN` 228 — levelled-actor lists; relevant if spawn *selection* ever has to
  agree across peers rather than being host-decided.
