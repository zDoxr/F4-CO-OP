# CoSync sync-stability directive — findings and fixes

Directive: "Bug fix first — synchronization, actors, equipment, companions & UI" (9 stages).
Build under investigation: 0.3.2 (commit f362f44). Fixes land as 0.3.3+.

Evidence sources:
* the source tree;
* the latest 0.3.2 session logs `CoSyncUpdated_3012.log` (host) and `CoSyncUpdated_10228.log` (client), 2026-09-28 21:38-21:59, plus earlier 0.3.x logs;
* IDA 9.3 on Fallout4.exe 1.11.240 (working copy of the canonical IDB, headless `idat` scripts).

Labels: **PROVEN** (code + log, or code + IDA), **VERIFIED** (engine behaviour read in the binary), **UNVERIFIED** (not proven; nothing is built on it without saying so). Every fix below is **untested in game** until the user's next test pass.

---

## Stage 1 — character model jitter / flickering

### S1-A. Two writers on every player proxy's position

```text
BUG:        Remote player's model jitters / flickers between positions.
Severity:   High (visible on every proxy, all the time)

SYMPTOMS:   The other player's character model is visually unstable on both
            host and client; worse the further they have walked since they
            last changed cell.

REPRODUCTION:
            Host + client in the same exterior. Client walks in a straight
            line inside one cell. Watch the client's proxy on the host.

ROOT CAUSE: Player proxies were positioned with a raw write:
            PositionRemoteActor set actor->pos and the 3D root NiNode
            directly and never moved the actor's havok character controller.
            Every frame the engine job "ApplyActorFootIKResults" reads each
            high-process actor's character-controller position and writes it
            back into the actor with SetPosition (blending toward it at
            min(15*dt, 1) per frame). The controller stayed where the engine
            last put it (spawn / reveal / last cell change), so each frame
            the engine pulled the proxy toward that stale spot and the CoSync
            tick pushed it back to the network position.

AFFECTED SYSTEMS: player proxies, the client tether, the companion nudge,
            the debug NPC path (all call PositionRemoteActor).

AFFECTED FILES:     CoSync/CoSyncGameAPI.cpp, Character/CoSynclocalplayer.cpp,
                    Character/CoSyncCompanionSync.cpp/.h, Networking/CoSyncNet.cpp
AFFECTED FUNCTIONS: CoSyncGameAPI::PositionRemoteActor, TickTether,
                    TickCompanionFollowNudge

NETWORK FLOW:  EU| (unreliable, ~15-20 Hz) -> PlayerManager inbox ->
               CoSyncPlayer::ApplyUpdate -> m_tfBuffer.
GAME THREAD FLOW:
               CoSync tick = F4SE task pump = engine job "NonRendersafeAI"
               (sub_140BD6170) -> PlayerManager::Tick -> TickSmoothing ->
               ApplyProxyTransform -> PositionRemoteActor.
               Engine: job "ApplyActorFootIKResults" (sub_140BD5C40) runs in
               an earlier job list of the same frame.

ENGINE BEHAVIOR (VERIFIED, IDA 1.11.240):
  Function:   job "ApplyActorFootIKResults"
  Address:    sub_140BD5C40 (job table entry 0x2F30DF0/0x2F30DF8)
  Purpose:    calls sub_140DB3ED0(ProcessLists) -> for every actor in the
              high process list with 3D: sub_140E1BE80(actor), then vtable+0x680.
  Function:   sub_140E1BE80(Actor*)
  Purpose:    charController = sub_140C5CD50(actor) (= process+0x300 ->
              sub_140D347C0); reads its position through vtable+0x1A8,
              scales by 69.991249 (havok -> game units), optionally blends
              from actor->pos toward it by clamp(15*dt), then calls the
              reference SetPosition sub_140513DB0(actor, newPos).
  Function:   Actor::SetPosition(const NiPoint3*, bool updateCharController)
  Address:    Actor vtable 0x25683B0 slot 0xCA = sub_140C60B50
  Behaviour:  sub_140513DB0 (ref pos + 3D update flag), and when the flag is
              set: moves the controller (sub_141894C50), zeroes its velocity
              (vtable+0x1D0 with the zero vector 0x142F3C550), clears two
              controller state fields, writes the 3D root local translation.
  Frame jobs: dumped the job table at 0x2F30B00-0x2F31200 ({func, name}
              pairs). "ApplyActorFootIKResults" is in the list that ends
              before "NonRendersafeAI" (the CoSync tick); "TESAnim" /
              "AnimsAndEffects" are in a later list.

EVIDENCE:   IDA as above. The client tether is a live demonstration that a
            raw actor->pos write does not stick: a 0.3.x client log (19:43)
            "[Tether] P2 exceeded range (107902 units) — pulled to host"
            every 5 s with the distance never changing.

FIX:        PositionRemoteActor now calls the engine's Actor::SetPosition
            (slot 0xCA, updateCharController=true) before its visual root
            write, so the engine's own per-frame controller sync reads back
            the network position: one value, two agreeing writers.

WHY THIS FIX IS CORRECT:
            The engine owns "reference position follows controller"; the
            fix feeds the network position into the controller instead of
            overriding the result downstream. The same slot is already used
            every tick for NPC puppets (SteerPuppetPosition). It also puts
            the collision capsule where the model stands (hits, HP relay),
            which the 1 Hz cell resync previously only approximated.

REGRESSION RISKS:
            * The proxy's own AI (left enabled on player proxies for the
              animation graph) can still add one frame of its own movement
              between ticks; it is reset every tick. UNVERIFIED whether its
              packages ever try to move it.
            * Controller velocity is zeroed each tick; player proxies are
              posed by bone replication, so locomotion speed is not read from
              it. NPC puppets are unchanged (they already used this call).

TEST PLAN:  Host + client, same exterior: walk, run, sprint, strafe within
            one cell and across cell borders; stand still next to each other.
            The proxy must not jitter or flicker. Shoot/punch the proxy while
            it walks: hits must land on the visible body.
```

### S1-B. 1 Hz full engine relocation of proxies in a neighbouring cell

```text
BUG:        Periodic (once a second) hitch of a remote player's model.
Severity:   High

ROOT CAUSE: ResyncActorCellMembership only skipped the engine relocation
            when proxy->parentCell == LOCAL player's parentCell. A remote
            player standing in a neighbouring exterior cell is correctly in
            a different cell, so the proxy got MoveRefrToPosition (controller
            reset, cell re-file, 3D update) every second, forever.
AFFECTED:   CoSync/CoSyncGameAPI.cpp ResyncActorCellMembership_Impl
ENGINE BEHAVIOR (VERIFIED): TESObjectCELL +0x40 bit0 = interior;
            +0x50 = exterior data, [+0] grid X, [+4] grid Y (engine cell-name
            formatter sub_1404C5690, accessors sub_1404C5BE0/sub_1404C5C00).
            4096 units per exterior cell = Creation Engine grid size (the one
            value here not re-derived in IDA; if it were wrong the code falls
            back to exactly the old behaviour).
FIX:        Relocate only when the proxy's cell is not the cell its target
            position belongs to (null, a different interior, or a different
            exterior grid square).
REGRESSION RISKS: none beyond the old behaviour (a false "mismatch" does
            what the old code always did).
TEST PLAN:  Stand in adjacent exterior cells (walk ~40 m apart across a cell
            border). No once-per-second hitch on the proxy.
```

### S1-C. Interpolation timed by arrival instead of by send time

```text
BUG:        Proxy walks at an uneven speed (surges / stalls) even when the
            real player moves steadily.
Severity:   Medium
ROOT CAUSE: m_tfBuffer samples were stamped with their local ARRIVAL time.
            EU is unreliable and the host relays / batches it, so delivery
            jitter became on-screen speed jitter. The sender's own capture
            time (EU timestamp, double seconds, ms precision) was carried but
            only used for the stale-packet check.
FIX:        localTime = senderStamp + offset, where offset follows the
            minimum (arrival - senderStamp) over the last 32 samples (~2 s):
            the least-delayed sample defines the timeline, kInterpDelay
            (120 ms, unchanged) absorbs the rest. Monotonic, re-anchors on a
            >1 s clock jump. No change to the delay or buffer size.
WHY CORRECT: interpolation must be keyed to when states were true, not when
            they were received. NPC ownership handoff creates a NEW entity
            (DESTROY + CREATE), so a per-entity sender clock never changes
            mid-stream.
EVIDENCE:   code (the arrival stamp); the logs only keep 1 in 50 EU so the
            jitter distribution itself is not measured (UNVERIFIED magnitude).
TEST PLAN:  as S1-A; also under packet loss (clumsy/NetLimiter).
```

### S1-D. Inert teleport loops that the S1-A fix would have made live

* **Client tether** (`CoSyncLocalPlayer::TickTether`): teleported the client's own player to the host when >8192 units apart. It was the reported "tether" and a client-authority violation; it only never worked because of the raw write. **Removed.**
* **Companion nudge** (`TickCompanionFollowNudge`): host teleported a companion 300 units behind its owner when >1500 away, every 3 s. Never fired in any 0.3.x log. A teleport loop that would now actually teleport. **Removed**; companion following is Stage 4.

### Checked and ruled out for Stage 1

* **RefreshActor3D (Disable+Enable) storms:** 12 call sites, but only ~10 calls in the whole latest session (spawn, reveal, outfit verify) — not the continuous flicker. Revisited in Stage 3 (equipment).
* **Bone pose fight:** player proxies take 37/93 bones from BONES| (the other 55 are names the proxy rig does not have: fingers/weapon/IK/anim-object helpers) and those 37 are re-stamped post-`BSAnimationGraphManager::Update` (Detour_MgrUpdate -> ReassertProxyPose), so the engine graph does not win the last write. Not a jitter source on current evidence.
* **Actor vtable slot 21 hook:** the proxy is class `Actor` (vtable 0x25683B0; the runtime log's vtbl matches), and slot 21 (sub_140C8CC70) is not a per-frame update: 0 fires for proxies, 1 for the local player all session. The re-stamp path above is the one that runs.
