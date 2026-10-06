# Remote proxy flicker and identity: CoSync 0.5.4

Date: 2026-10-06. Status: **built and deployed (0 errors, 0 C++ warnings). Not yet run in game.**
Evidence comes from the 2026-10-05 22:53–23:03 session (host `CoSyncUpdated_13380.log`, client `CoSyncUpdated_1308.log`), the earlier 22:10–22:34 pair (`6532` / `38320`), and IDA on Fallout4.exe 1.11.240.

## 1. Proxy lifecycle, traced

| Step | Where | Notes |
|---|---|---|
| Connect, HELLO (SteamID, name, eid) | `CoSyncNet` HELLO handler | `eid` is derived from the SteamID. `s_peers[sid] = {name, eid, conn}` |
| CREATE (host broadcasts) | `HostBroadcastPlayerCreate` | Username = HELLO name. A third client learns about the others through `REQENT` → stored CREATE |
| CREATE processed | `CoSyncPlayerManager::ProcessEntityCreate` (game thread, inbox) | **Idempotent.** `st.spawnQueued` admits one spawn per entity. A repeated CREATE only refreshes the name |
| Spawn | `PumpDeferredSpawns` → `CoSyncSpawnTasks` → `CoSyncPlayer::SpawnInWorld` | `if (hasSpawned) return true`, so a watchdog re-enqueue can't make a second actor. Base = the player's own RMPlayer slot |
| UPDATE | `ProcessEntityUpdate` → `ApplyUpdate` → buffer | **Never spawns.** With no CREATE it asks `REQENT` |
| Per frame | `PlayerManager::Tick` → `TickAnimVars` (ApplySkeleton) + `TickSmoothing` (PositionRemoteActor); engine anim update → `ReassertPoseForGraphRoot` | |
| Hide/show | `WorldSync::TickProxyCellVisibility` | Only on a real location mismatch |
| Freed by engine | `ReleaseIfFreed` → watchdog respawn | Only at loads |
| Disconnect / ED | `DespawnEntity` | Slot, bone cache, re-stamp record, registry and marker are all released |

Measured across all four logs: **no spawn/destroy loop and no duplicate proxy.** Each session has 1–3 player-proxy spawns, each one at a real load. Hide→show happened 3–5 times per session, each at a real cell or worldspace change. Network threads only enqueue; all world work runs in the tick.

## 2. Root cause of the flicker

### 2a. The pose re-stamp rebuilt the fade node every frame (main cause)

Every frame, on every bone-replicated **player** proxy (NPC puppets never get this), `ReassertPoseForGraphRoot` called the proxy root's `UpdateDownwardPass` (vtable slot 49) with a **zeroed** 512-byte `NiUpdateData`. On the root, a `BSFadeNode`, that slot is `sub_14217DE70`, and IDA shows it does much more than update transforms:

- It sets `fTimeSinceUpdate` (+0x1AC) to the context time, which was 0.
- It runs controllers at time 0 on nodes that carry flag bit 3.
- It counts the geometry it reaches (context +0x14). When that count differs from the fade node's flattened geometry list (`kGeomArray` +0x168, count +0x178), it **tears the list down and rebuilds it** (`sub_14217ED30`). When the count is 0, it **clears it** (`sub_14217EDC0`). Either way it then recomputes the node's world bound after first zeroing the radius (`sub_14217EE50`).

That list and that bound are what the renderer culls and draws the actor from. They were being rewritten from a fabricated context, on the animation worker, in the middle of the engine's own frame.

A related input: `SafeWriteRotation`/`SafeWriteTranslation` ORed `0x2` into each written bone's `+0x108` flags, believing it was a "dirty" bit. It is actually SELECTIVE_UPDATE: `NiNode`'s pass (`sub_1416BF1E0`) only descends into children that have it set, so it changed which geometry the engine's own pass counts.

A previous session had already rejected slot 53 because BSFadeNode's override "can drop the fade-node geometry cache". Slot 49 contains the same teardown code.

### 2b. CoSync ticked twice per frame, on two threads

F4SE runs permanent tasks after every `MessageQueueProcessTask` (`sub_140C45150`). The engine calls it twice per frame:

- from `Main::Update` (the call at 0xC3488B), on the main thread;
- from the NonRendersafeAI job (`sub_140C365F0`, the call at 0xC3660E), on a worker thread.

The logs show it directly: 600 `TickAnimVars` calls per 5 s against 300 engine animation updates (`restamps`), and `[ThreadDiag]` alternating between one fixed thread and rotating workers. So every proxy write (SetPosition, 1 Hz MoveRefrToPosition, root write, pose write) and every frame-counted delay ran twice per frame, at two points in the frame.

### 2c. `PositionRemoteActor` called the root's slot 52 every tick

That is `sub_1416C8AC0`. It recomputes only the root's own world transform, and first copies the current world transform into the previous-frame transform (+0xC0). It never updated the bones, despite the comment saying "propagate down". What it did do was make the proxy's previous transform equal its current one twice a frame.

## 3. Changes

| File | Change |
|---|---|
| `Animations/CoSyncPlayerAnimation.cpp` | `SafeUpdateDownward` replaced by `BakeSkeletonWorld`/`BakeNodeWorld`: each node below the root gets world = parent world × local through the engine compose (`sub_140344820`), parents before children, and `BSFlattenedBoneTree` (vtable 0x2684FC0) is synced with `sub_1416C7230`. It leaves the root itself alone, along with the previous-frame transforms, bounds, controllers, fade state and the geometry list. The bone writers no longer set flag `0x2`. Logs `bakedNodes=` / `bakeFailures=` |
| `CoSync/CoSyncRuntime.cpp` | One tick per frame. A pump whose stack contains `Main::Update`'s return address (0xC34890) runs the tick. Any other pump runs it only when `Main::Update` won't pump this frame, using the engine's own test (`UI+0x1E0 numPauseGame == 0` and `Main+0x2A == 0`). Includes a starvation fallback and a 30 s pump summary log |
| `CoSync/CoSyncGameAPI.cpp` | `PositionRemoteActor`: the slot-52 call is removed |
| `Character/CoSyncProxyLooks.cpp/.h` | `ApplyDisplayName`: the player's own RMPlayer base FULL is set to their username, at spawn and whenever the name changes |
| `Character/CoSyncPlayerManager.cpp` | Name re-applied when a CREATE changes it. `[CoSyncProxy] CREATE / CREATE again (reusing) / DESTROY` log lines |
| `Character/CoSyncPlayer.cpp` | `[CoSyncProxy] SPAWN` with SteamID, EntityID, Username, ProxyFormID and the proxy pointer |
| `Networking/CoSyncNet.cpp/.h` | `GetSteamIDForEntity` |
| `Core/Version.h`, `CHANGELOG.md` | 0.5.4 |

## 4. Identity

- **"2nd Player"** is the FULL name of CoSync.esp's seven proxy bases (RMPlayer, RMPlayer2…7). It was only ever a display name. The actual identity was already SteamID → entityID (SteamID-derived) → one `CoSyncPlayer` → one registered proxy actor. That chain is unchanged.
- The game shows a base's FULL on the crosshair and activate prompt, the HUD target bar and kill messages. Each remote player owns one base slot, so that FULL is now set to the owner's username. Nameplates and the HUD already used the username.
- **Limit:** an 8th or later remote player shares the fallback base, so it keeps the ESP name rather than showing someone else's.
- **Test setup note:** in the latest logs **both** instances introduce themselves as `gse orca`, which is gbe_fork's default account name. Give each instance its own `account_name` (`configs.user.ini` under each `GseSavePath`) or the two players will look identical.

## 5. Testing done

- Full solution build, v145 toolset: 0 errors, 0 C++ warnings. The DLL is deployed to `Data/F4SE/Plugins`.
- Every new RVA, offset and vtable slot was checked in IDA (sections 2–3).
- **No in-game test yet.** None of the six scenarios has been run, so I am **not** claiming the flicker is gone.

### What to check in the logs on the next test

- `[CoSyncRuntime] tick pumps last 30s: Main::Update=~1800 other=~0 skipped=~1800`, i.e. one tick per frame. The `calls/5s` figure should now match the frame rate (~300), not double it.
- `ApplySkeleton #… restamps=… bakedNodes=<~100+> bakeFailures=0`.
- Each player has exactly one `[CoSyncProxy] SPAWN` per load. Repeated CREATEs show up only as `CREATE again … reusing existing proxy`.
- `[CoSyncProxy] NAME … shown as '<username>'`.
- No `Main::Update pump not seen … ticking on every pump again` warning.

The six scenarios (1 client; 3 clients in every direction; movement and combat; exterior↔interior; disconnect; reconnect) are in the directive. Watch each proxy for several minutes in each one.

## 6. Remaining issues (proxy only)

- A cell change still does a full Disable+Enable per proxy: a single flash when someone changes cell, by design.
- A one-frame "hidden then shown" can happen when both players cross into the same interior and the other player's WS packet arrives a few ms later (22:22:51 / 22:24:09 in the 0.5.3 logs).
- `Pitch` arrives in degrees and is clamped as radians (`[PitchDiag]` clamps every sample). It doesn't affect bone-replicated players, but it is still wrong.
- `PositionRemoteActor` still writes the root's world transform from the tick. The bones catch up at the next engine/animation pass, within the same frame now that the tick runs at the end of `Main::Update`.
- If flicker remains: check `bakeFailures`, then whether the engine's own pass still churns `kGeomArray`. A breakpoint on `sub_14217ED30` / `sub_14217EDC0` for the proxy's root would show it.

## 7. 0.5.5 follow-up: the tick gate is reverted

First in-game run of 0.5.4 (2026-10-06 13:23–13:33, host `9644`, client `16780`). The user reported: "the proxy will randomly stop then starts back up after about 5 seconds".

- **Cause (my 0.5.4 change):** the gate recognised the `Main::Update` pump by stack-walking for return address 0xC34890. At the main menu that works (`Main::Update=1700` per 30 s). In game it almost never does (`Main::Update=0 other=3600`). The in-game ticks come from a fixed thread (22300) alternating with workers, and that thread isn't the one recognised at the menu (18712), so the walk does not reach the engine frame there. Every pump was skipped until 600 had passed (~5 s at 120 pumps/s). The logs show `Main::Update pump not seen for 600 skipped pumps` repeating on both machines, matching the stalls.
- **Fix:** the gate is removed and the tick runs on every pump again (2× per frame, as before 0.5.4). Section 2b still describes a real issue. A proper fix needs a reliable per-frame anchor, not a stack walk.
- **Confirmed working from 0.5.4 in the same run:** `bakedNodes=103/111 bakeFailures=0` on both machines, no crashes, and `[CoSyncProxy] NAME … shown as 'gse orca' (was '2nd Player')`. The proxy respawns at 13:26:06, 13:28:14 and 13:30:22 each followed a real load (engine freed the proxy), not a loop.
