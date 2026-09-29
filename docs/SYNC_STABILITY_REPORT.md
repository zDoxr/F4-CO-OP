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

---

## Stage 2 — entity lifecycle and network state

### S2-A. KILL re-applied to corpses every 5 seconds

```text
BUG:        Corpses re-run the engine death every 5 s on clients.
Severity:   Critical (entity state corruption: repeated death processing)

SYMPTOMS:   Corpse twitch / re-ragdoll; death side effects can repeat.
ROOT CAUSE: The host re-broadcasts CREATE (+ KILL for corpses) for every
            world actor every 5 s so late joiners see corpses
            (NpcWorldSync, kCreateRebroadcastInterval). The KILL receiver was
            not idempotent: every copy ran KillPuppet = SetLifeState(Dying) +
            KillDying on an actor that was already Dead.
AFFECTED FILES:     Character/CoSyncHealthSync.cpp, CoSync/CoSyncGameAPI.cpp/.h
AFFECTED FUNCTIONS: CoSyncHealthSync flush (Kill branch), KillPuppet
NETWORK FLOW:  host NpcWorldSync rebroadcast -> KILL| (reliable) -> client
               HealthSync queue -> flush on the tick.
ENGINE BEHAVIOR (VERIFIED): Actor::SetLifeState = Actor vtable slot 0x105 =
               sub_140C83990; life state = (actor+0x130 >> 17) & 0xF
               (reads/writes mask 0x1E0000). CK enum: 0 alive, 1 dying,
               2 dead, 7 essential-down.
EVIDENCE:   0.3.2 client log: "Killing entity=65537/65538" 440 times in
            18 min, one pair every 5 s from 21:39:45 to 21:58.
FIX:        The Kill branch skips (and just records the kill) when the engine
            already reports the actor dying or dead. Engine state, not a
            bookkeeping flag, so a clone that was freed and respawned alive
            is still killed.
REGRESSION RISKS: none known; a live actor still dies on the first KILL.
TEST PLAN:  Kill an NPC on the host with a client nearby; watch the corpse on
            the client for a minute (no twitching); client log shows one
            "Killing entity" per death.
```

### S2-B. A late update could resurrect a destroyed entity

```text
BUG:        UPDATE after DESTROY re-created entity state and requested a
            re-broadcast of the entity.
Severity:   High (lifecycle: DESTROY -> UPDATE -> REQENT -> CREATE)
ROOT CAUSE: EU travels on the unreliable lane, DESTROY on the reliable one;
            GNS does not order across lanes. ProcessEntityUpdate called
            GetOrCreateState for any entity ID, and with no create on file it
            sent REQENT|, so an in-flight EU landing after its DESTROY created
            a stale state entry and asked peers to re-announce the entity.
FIX:        Destroy tombstones (CoSyncPlayerManager::m_destroyedAt). Every
            DESTROY path records one (manager inbox and the world-NPC path in
            CoSyncNet::OnReceive); updates for a tombstoned entity are dropped
            without touching state; only a new CREATE (either path) clears it.
            Bounded: pruned past 2 minutes once over 256 entries.
WHY CORRECT: CREATE is the only message allowed to bring an entity into
            existence; reconnecting players (same SteamID-derived entity ID)
            always send CREATE first.
REGRESSION RISKS: an entity whose DESTROY is followed by updates with no new
            CREATE stays invisible -- which is the intended rule.
TEST PLAN:  Client disconnects and reconnects; NPC ownership handoff (walk
            away from an NPC your partner is near). Both must reappear.
```

### Audited, no defect found

* **Duplicate CREATE:** ProcessEntityCreate queues a spawn once (`spawnQueued`); a repeat only refreshes the stored create. World-NPC CREATE goes to ClientBindWorldActor, which binds or retries.
* **Out-of-order EU:** dropped by sender timestamp (`u.timestamp < m_lastHostTime`). NPC ownership handoff is DESTROY + a new entity, so one entity never mixes two sender clocks.
* **"STUCK" puppets in NpcSyncDiag (438 lines):** both are the two corpses above; dead NPCs are intentionally not streamed. Diagnostic noise only.
* **Threading:** network thread (GNS) only fills the transport inbox under its mutex; `CoSyncNet::OnReceive`, the manager inbox, and all engine calls run on the CoSync tick (job "NonRendersafeAI"); PrismaUI/WndProc calls are queued to the tick (see HARDENING_REPORT). The only engine-thread callbacks that touch shared CoSync state are the anim-graph re-stamp (own mutex) and the equip/door/quest hooks (enqueue only).

### Found here, fixed in a later stage

* Host revive state loop: "[LocalPlayer] Gave up (revive timeout)" logged 1502 times, twice per frame, from 21:44:24 on (Stage 6).
* Outfit sync re-sends the whole outfit every 10 s and the receiver's verify step ends in `RefreshActor3D` (Disable + Enable) plus a weapon re-equip (Stage 3).

---

## Stage 3 — clothing / armor / weapon desynchronization

### Engine mechanism (VERIFIED, IDA 1.11.240)

```text
Function:  ActorEquipManager::EquipObject     RVA 0xCE5E20 (manager 0x31EE428)
           forceEquip=1 skips the "play an equip animation, equip later"
           branch (sub_140CFAE10) and calls the worker sub_140CE7FB0.
Function:  equip worker                        sub_140CE7FB0
           -> sub_140CAD0A0 (switch on form type; ARMO = 29 = 0x1D)
           -> Actor vtable+0x378 (slot 0x6F) = AddWornItem sub_140CAEBC0
Function:  Actor::AddWornItem                  sub_140CAEBC0
           unequips whatever holds the same biped slots (UnequipObject
           0xCE62C0), adds the item to the biped, and for ARMO calls
           sub_140D363C0(process, 1) and sub_140D363C0(process, 0x10).
Function:  sub_140D363C0(process, flags)        (process = Actor+0x300)
           if (process+0x08) (middle-high data) != null:
               *(uint16*)(mh+0x496) |= flags     -- "update3DModel"
           otherwise does NOTHING.
Consumer:  Actor::Update = Actor vtable slot 0x13 = sub_140C8B5A0
           if update3DModel != 0: actor has 3D -> sub_140D02600 ->
           sub_140D27280 (rebuild the worn 3D), else the flags are cleared
           (a later Load3D builds the biped from the worn items anyway).
Other readers/writers of +0x496: sub_140D31670, sub_140D363E0 (clear bits),
           sub_140D36400 (clear all), sub_140D36420/sub_140D36440 (read),
           sub_140D3FBC0.
```

So the engine rebuilds the visible gear by itself on the next actor update. The one way an equip can land in the inventory with no visual change is an actor without middle-high process data at equip time: the request is silently dropped.

### S3-A. Outfit reconciled against bookkeeping, never against the engine

```text
BUG:        A peer's armor/clothing sometimes never appears until they
            unequip and re-equip it.
Severity:   High
ROOT CAUSE: The receiver deduplicated every OUTFIT (including the 10 s
            re-broadcast meant to self-heal) against s_appliedOutfits, its
            own record of pieces it had once verified. If anything later took
            a piece off the proxy (slot conflict, 3D reload, the proxy base's
            default outfit coming back after Disable/Enable), the record still
            said "applied" and nothing ever re-equipped it -- until the sender
            removed and re-added it, which is the only thing that changed the
            record. Unequips likewise only touched recorded pieces.
FIX:        Every OUTFIT converges the proxy to the sender's list using the
            ENGINE's equipped flags: received pieces not engine-equipped are
            injected + equipped; engine-equipped ARMO not in the list is
            unequipped (not while the proxy is in Power Armor -- PA sync owns
            those pieces until Stage 5).
FILES:      Clothing/CoSyncOutfitSync.cpp (FlushPendingOutfitOps,
            SafeListEquippedArmor)
```

### S3-B. Disable+Enable "biped rebuild" after every verified armor change

```text
BUG:        After an armor change the proxy blinks out and back in, and its
            weapon disappears for about a second.
Severity:   High (model flicker; forced re-equip loop)
ROOT CAUSE: The verify pass ended with RefreshActor3D (Disable + Enable) on
            the assumption that equipping does not update the model. It does
            (above). Enable() drops individually equipped weapons, so each
            armor change also scheduled a weapon re-equip 60 ticks later --
            the same self-inflicted cycle already proven and removed for the
            weapon verify on 2026-08-26.
FIX:        Removed. If an equip or unequip happened while the proxy had no
            middle-high process data, the entity is remembered and the
            engine's own request (sub_140D363C0(process, 0x11), wrapped as
            CoSyncGameAPI::RequestActorModelUpdate) is re-issued once it can
            take effect.
REGRESSION RISK: if some other, unidentified condition also blocks the
            model rebuild, gear would stay invisible where the Disable/Enable
            used to force it. The log line "[OutfitSync] ... worn 3D update
            re-requested" and the VERIFY lines show which path ran.
```

### S3-C. Weapon retry trusted a flag instead of the engine

```text
BUG:        A proxy that lost its weapon (e.g. after a rebuild) stayed
            unarmed until the real player switched weapons.
ROOT CAUSE: The 10 s weapon retry in CoSyncPlayer::ApplyUpdate ran only while
            m_weaponConfirmedEquipped was false; once verified it never
            looked again.
FIX:        Every 10 s the wire weapon is checked against the engine's
            equipped flag on the proxy and re-queued if missing.
```

### S3-D. Unguarded inventory walk in the weapon verify (crash)

ProcessWeaponVerifyQueue walked `actorRef->inventoryList` (and dumped 15 entries on a miss) with no liveness check or SEH -- the pattern that crashed the host in OutfitSync on 2026-09-26 when a rebuilt proxy left a stale pointer. Now uses `CoSyncGameAPI::GetInventoryEquipState` (IsFormStillLive + SEH) and skips a dead actor.

### TEST PLAN (Stage 3)

1. Host and client in one place. Client equips / unequips a hat, a chest piece, gloves, a full outfit, one at a time, then several at once: the host sees each change within ~1 s, with no blink of the model and the weapon staying in hand.
2. Client changes clothes while the host is far away (proxy in low process), then the host walks back: the gear is right when the proxy comes into view (look for "worn 3D update re-requested").
3. Client swaps weapons (pistol, rifle, melee, unarmed): the proxy follows; holster/draw still animate.
4. Teleport / load door / fast travel together: gear and weapon survive the proxy's re-show.

---

## Stage 4 — companions

### What "ownership" means in CoSync (as built, now written down)

| Concept | Meaning | Where it lives |
|---|---|---|
| **FO4 companion relationship** | The vanilla Followers quest alias + teammate flag on *one machine's* actor. On each machine the "player" is that machine's own player. | engine, per machine |
| **Multiplayer owner** | The player whose companion it is. | `CREATE.ownerEntityID` (host: 0 = host) |
| **Network authority / controller** | The machine that runs the companion's AI and streams it (`EU`/`AEV`). Always the owner's machine: that is the only machine where its follow package targets the right player. | NpcWorldSync binding (`isLocallyOwned`) |
| **Visibility** | Everyone in the same location sees it: non-owners get a puppet driven by the owner's stream. | NpcWorldSync puppet |

**Several players can have the same companion.** Each player's companion is their own instance: the host's follower is its real world actor; a client who recruits someone gets its own copy (`CoSyncCompanions::SpawnMine`) that takes over its local Followers quest, announced with `COMPOWN` and shown to everyone as a puppet. Two players can both travel with Piper; each Piper follows, fights for and is controlled by her own player. Followers that came with the host's save are handed back on clients (disabled locally) because the host's stream already shows them. A single actor following two players at once is not possible with the vanilla Followers quest (one alias, one "player" per machine), so it is not attempted.

### S4-A. Companions were eligible for NPC ownership handoff

```text
BUG:        A player's companion can disappear for the other players.
Severity:   Critical (entity lost / wrong authority)
ROOT CAUSE: HostTickOwnershipHandoff (0.3.1) releases any world actor whose
            owner is away (other location or >12000 units) to a nearby player.
            It did not exclude companions:
            * a client's companion copy exists only on that client. Releasing
              it sent DESTROY; no other machine has that actor, so nobody can
              re-claim it, and the client sent COMPOWN only once -- the
              companion was gone for everyone else for the rest of the session.
            * the host's follower released to a client: the client's copy is
              disabled (HandBack) and teammates are skipped by the client scan,
              so it vanished for everyone until the host's 30 s hold expired.
            Both can trigger during any load-door / fast-travel transition,
            where the companion's streamed location briefly differs from its
            owner's.
FIX:        The handoff skips companion entities: COMPOWN-registered entities
            (s_companionEntities) and entities whose host actor is a player
            teammate.
FILES:      World/CoSyncNpcWorldSync.cpp (HostTickOwnershipHandoff,
            HostRegisterClientOwnedActor, Reset)
```

### S4-B. COMPOWN was one-shot

```text
BUG:        A client's companion never comes back for others once its network
            entity is gone (host restart, DESTROY).
FIX:        The client re-announces a companion that has no bound entity 15 s
            after its last announcement (CoSyncCompanions::Tick).
```

### Companion animation — findings (not changed)

* Companion puppets are AI-alive clones steered every tick with `Actor::SetPosition(pos, true)` (vtable 0xCA). VERIFIED: that call passes the zero vector (bytes at 0x142F3C550 are all 0) to the character controller's virtual at +0x1D0 and clears two controller state fields, i.e. it resets the controller's motion state every tick.
* The puppet's locomotion is animated by its own engine AI (TickAnimVars is skipped for puppets), and that AI has no idea the puppet is moving along the owner's path. That is the most likely reason walk/run on companions "is not perfect".
* **UNVERIFIED, so not built:** that controller slot 0x3A (+0x1D0) is a plain linear-velocity setter taking an hkVector4 (on bhkCharProxyController it resolves to sub_1418B4030, whose second argument is dereferenced like an object, not a vector). The fix that follows from this -- give the engine the owner's velocity instead of zero, or drive the puppet's Speed/Direction graph variables from it -- needs this confirmed first.
* Combat, weapon draw and discrete events (fire/reload/throw) already come from the owner (K3, 0.3.1).

### TEST PLAN (Stage 4)

1. Host with Dogmeat/Piper; client nearby: client sees the companion follow the host; host goes through a load door and back -- the companion stays visible to the client the whole time.
2. Client recruits a companion (same one the host has, e.g. both have Piper): both Pipers visible to both players, each following her own player; client walks through doors / fast travels -- the host keeps seeing the client's Piper.
3. Host quits to menu and hosts again with the client rejoining: the client's companion reappears for the host within ~15 s.
