# FO4 1.11.221 Animation Pipeline — Verified Reference (Round 145, 2026-07-09)

Ground truth: `Fallout4_disk.i64` (RTTI scan + Hex-Rays, headless idat.exe).
Raw decompilations: `logs\ida_dumps\anim_pipeline_disk.txt`, `anim_pipeline_disk2.txt`.
Full report + sync architecture: https://claude.ai/code/artifact/a1b48252-48ca-48eb-8b7d-3837500c28cf

## THE HANG, SOLVED (rounds 119/121/122)

The engine named-setter far-calls froze because the dump-IDB RVAs were skewed:
`0x818D60` and `0x818D80` are **mid-function** inside `sub_140818D10` in the real
exe (executing them = garbage frame -> parked in the engine's unbounded lock
spin), and `0x818DA0` is the **bool** setter, not float. Real thunks:

| Function                     | Thunk    | Worker   | Per-graph | Signature |
|------------------------------|----------|----------|-----------|-----------|
| SetGraphVariableBool         | 0x818DA0 | 0x81B5E0 | 0x1326580 | bool(holder, BSFixedString*, bool) |
| SetGraphVariableInt          | 0x818DC0 | 0x81AE90 | 0x1326740 | bool(holder, BSFixedString*, int) |
| SetGraphVariableFloat        | 0x818DE0 | 0x81B050 | 0x1326660 | bool(holder, BSFixedString*, float) |
| GetGraphVarVector (active)   | 0x818D90 | 0x81AB50 | 0x1326B80 | bool(holder, BSFixedString*, float[3]*) |

holder = actor + 0x48 (IAnimationGraphManagerHolder).
=> `CoSyncAnimationUtils.cpp` g_realSetAnimVar* RelocAddrs must be repointed and
the round-122 `EngineSettersDisabled` kill-switch removed. Keep the bounded
try-lock + IsGraphReady gates as safety.

## Worker protocol (confirmed byte-level)

1. holder->vtbl[4] -> AddRef'd BSAnimationGraphManager (refcount mgr+0x08;
   release: dec, slot0 dtor (this,1) at zero).
2. Lock: owner tid mgr+0xC8, count mgr+0xCC. 10000 spins then Sleep(1) forever,
   re-entrant by tid. (Our TryAcquireMgrLock replica is exactly right.)
3. Loop ALL graphs: BSTSmallArray at mgr+0x40 (cap/discriminator), +0x48 (data),
   +0x50 (count). Player's mgr holds BOTH 1st- and 3rd-person graphs — setters
   write all graphs; getters (holder slots 19/20/21) read the ACTIVE one
   (mgr+0xD8 activeGraphIndex). This is the "FP indices shifted in NG" trap.

## Per-graph setter = hash lookup + 32-bit store. Nothing else.

```
BShkbAnimationGraph:
  +0x370  BSTHashMap<BSFixedString, int>   variable name -> index
  +0x378  -> chain to hkbVariableValueSet:  +0x10 words[], +0x18 count
```

No binding refresh, no side effects — bindings are pulled on next graph update.
Raw data[] writes are functionally IDENTICAL to the engine path given correct
indices. The +0x370 map can be read once at graph load to resolve indices at
runtime and retire the hardcoded TP::Variables tables.

**LIVE-CONFIRMED (build 145 session, 2026-07-09 20:51):** the corrected setter
thunks completed on the first try (`ok=11 fail=0`, T2 round 4) — the round-122
hang is dead. Also ground-truthed: the +0x370 map's key qword is a
**StringCache::Entry\***, not a char\*. Entry layout (from the engine's own
c_str() resolution bytes): state flags at +0x08 (0x4000 = extern), externData
chain at +0x10, chars at +0x18. Poisoned chains carry the known sentinel
externData=0xFFFFFFFFFFFFFFF7 (the SafeGetTag/SafeMakeKey hazard).

## Event pipeline

- Code -> graph: holder vtbl slot 1 = NotifyAnimationGraph impl **0x819B50**
  (fetches mgr via slot 4, dispatch **0x130EBB0**). Unknown names silently drop;
  vocabulary = HKXExtract behavior-graph string tables (round 139 allowlist).
- Graph -> code: BSTEventSink<BSAnimationGraphEvent> at actor+0x38, slot 1
  ReceiveEvent = **0xCAB610** (actor = sink - 0x38 — our hook math confirmed).
  Internally: per-actor response-handler map walk 0xCC8360 (weaponFire, hit
  frames, footsteps), global filtered queue, Papyrus RegisterForAnimationEvent
  relay 0xE0F850 (list qword_1431E3398).

## Vtables (RTTI-derived)

| Class | Vtable RVA |
|---|---|
| IAnimationGraphManagerHolder (base) | 0x2518A50 |
| Actor main / +0x38 sink / +0x48 holder / +0x128 ActorState | 0x25602C0 / 0x2560C90 / 0x2560CC0 / 0x2560E88 |
| Actor holder impls: slot4 / slot19 / slot20 / slot21 | 0xCB28A0 / 0xCB2AE0 / 0xCB2A80 / 0xCB2A20 |
| PlayerCharacter main / +0x48 holder | 0x2564848 / 0x2565268 |
| BSAnimationGraphManager / BShkbAnimationGraph | 0x2626330 / 0x2626B48 |
| hkbBehaviorGraph / hkbVariableValueSet | 0x26334F0 / 0x26334C8 |
| hkbStateMachine / hkbClipGenerator | 0x26352A0 / 0x2635C18 |
| MovementControllerNPC | 0x2567A38 |
| AnimationFileManagerSingleton | 0x26281E0 |

## Papyrus native registration anchors (for future derivations)

- ObjectReference natives (Set/GetAnimationVariableBool/Int/Float): **0x115F150**
- Actor natives (PlayIdle, PlayIdleAction, ChangeAnimArchetype, ChangeAnimFlavor,
  ChangeAnimFaceArchetype, PlaySubGraphAnimation): **0x10FCB20**
  -> decompile PlayIdleAction's body here to derive TESActionData construction +
  mediator execute for the action-replay tier (reload / furniture / power armor).

## Recommended sync architecture (summary — full rationale in the artifact)

Hybrid, four tiers (this is what the engine itself does, and what Skyrim
Together Reborn independently converged on):

1. **Movement + ActorState dwords** 15-20 Hz unreliable — self-healing baseline.
2. **Anim events** (round-139 allowlist) edge-triggered, reliable-ordered.
3. **~8-12 continuous vars** (Speed, Direction, AimPitch/HeadingCurrent, iState,
   TurnDelta) piggybacked on tier 1, deadband-filtered.
4. **Gameplay-action RPCs** (equip, fire, reload, furniture/PA, kill, stagger
   impulse) executed through native engine paths on the proxy.

Full 299-var AnimVarSnapshot: spawn/teleport keyframe ONLY, not per tick.
NEVER transmit: bone poses, root-motion deltas, graph-internal state, raw
variable indices, ragdoll bone states.
