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

---

## Stage 5 — Power Armor

### How the engine does it (VERIFIED unless marked)

```text
Enter:   Papyrus Actor.SwitchToPowerArmor native (sub_1410FBF90) -> engine
         SwitchToPowerArmor sub_140989D90 (8 callers). Needs a PA frame
         FURNITURE ref; merges the frame into the actor (PowerArmorRace,
         ExtraPowerArmor) and equips each piece through
         ActorEquipManager::EquipObject with applyNow=0 -> AddWornItem ->
         update3DModel flags -> Actor::Update rebuilds the 3D (Stage 3 path;
         player proxies have AI enabled, so this runs).
Exit:    SwitchFromPowerArmor sub_14098BE40 (1 argument, the actor; its own
         name string 0x0252B6E8 is referenced at 0x98C371 inside it).
State:   in-PA = ExtraPowerArmor on the actor / PowerArmorRace
         (CoSyncGameAPI::GetActorPowerArmorExtraState, SafeActorInPowerArmorByRace).
Frame:   a FURN ref with the PA keyword; its pieces live in the frame's
         inventory while unworn, in the wearer's while worn.
```

### How CoSync replicates it (as found)

Sender: Papyrus OnSit/OnGetUp (plus a race-based fallback) -> `PA|sender|chassisBase|chassisRef|pos|1|p0..p5` on enter, `...|0` on exit, re-sent on a piece change. Receiver: spawns its own chassis from the base form at the proxy, injects the pieces, calls the native enter, force re-equips the pieces, hides its local copy of the sender's real frame; exit = native eject (with retries), delete the spawned chassis, re-show the real frame, 3D rebuild. Unworn frames' inventories are synced separately (`PAI`/frame mods).

### S5-A. PA was replicated as events only

```text
BUG:        A player who joins while someone is in Power Armor -- or whose
            proxy of them was freed and rebuilt (cell change, reload) -- sees
            them as a normal human.
Severity:   High (permanent desync until the wearer gets out and back in)
ROOT CAUSE: PA| is sent only on enter, exit and piece change; an op that
            arrives while the proxy is not spawned is dropped.
FIX:        The wearer repeats its current PA state every 5 s
            (TickLocalPowerArmorPieces). The receiver treats a repeat with an
            unchanged piece set as a no-op (new CoSyncPlayer::m_paAppliedPieces);
            before, every such message unequipped and re-equipped all six
            pieces. A proxy hidden for being in another location is skipped
            and picks the state up from a later repeat.
PROTOCOL:   unchanged (same PA| message, sent more often while worn).
```

### S5-B. Orphaned chassis / permanently hidden frame

```text
BUG:        A player who disconnects (or whose proxy is freed) while in PA
            leaves an extra PA frame standing in everyone else's world, and
            the real frame the proxy had hidden never comes back.
ROOT CAUSE: m_paSpawnedChassisRefFormID / m_paRealFrameRefFormID were cleaned
            up only by the exit path; DespawnEntity and ReleaseIfFreed never
            looked at them.
FIX:        CoSyncPowerArmorSync::OnProxyRemoved, called from both: deletes the
            spawned chassis, re-enables the hidden real frame, clears the state.
```

### Not reworked in this pass (needs in-game data first)

* **No PA session exists in any log on disk** (all 7 current logs), so the live behaviour of the enter/exit path could not be checked. Everything above is from code + IDA.
* The post-enter `RefreshActor3D` (Disable+Enable 1.2 s after entering) and the unequip+re-equip "edge" loop are the same kind of symptom patch Stage 3 removed for normal armor. With the engine path now understood (deferred equips are applied by Actor::Update on an AI-enabled actor) they are probably unnecessary, but removing them without a PA test log would be a guess.
* Not replicated at all today: fusion core charge, per-piece condition/damage and repair, PA station interactions, moving/transferring a frame between players. Piece condition is stored per inventory stack (extra data) -- the exact field is UNVERIFIED and needs RE before designing its sync.

### TEST PLAN (Stage 5)

1. Host enters a frame; client joins afterwards: the client sees the host in PA within 5 s.
2. Client in PA walks through a load door with the host: the host's proxy of the client is back in PA within 5 s of reappearing.
3. Client disconnects while in PA: no stray frame left on the host, and if the client had entered a world frame, that frame is visible again on the host.
4. Swap a helmet while in PA: the change shows once; no armor flicker every 5 s afterwards.

---

## Stage 6 — revive / downed

### State machine (now explicit)

```text
 Up ──HP<=0 (essential bleedout, life state 7)──► Downed      [DOWN| broadcast, vanilla recovery blocked]
 Downed ──REVIVED for me──► Released (revived)                 [recovery unblocked + health restored at once]
 Downed ──fReviveGiveUpSeconds──► Released (gave up)           [recovery unblocked, engine regen]
 Released ──HP>0 for 5 ticks──► Up                             [REVIVED|me|0 if nobody revived me]
 Real death (life state 1/2) keeps the old full tick gate.
```

Evidence used: session 0.3.2 21:43-21:45 (host 3012 / client 10228). Host went down 21:43:24.820; client started a revive 21:43:42.799 (1 Stimpak); the channel was cancelled 21:43:44.900 "moved out of range"; the host gave up at 21:44:24.828 and then logged the give-up line 1502 times.

### S6-A. Revive channel cancelled by a jumping proxy position

The range check (350 units) reads the downed proxy's `actorRef->pos`, which Stage 1 (S1-A) showed the engine was pulling toward the stale character-controller spot every frame. The downed host was not moving, yet the check failed 2.1 s into a 3 s channel. **Fixed by S1-A** (the proxy's engine position now is the network position). No separate change.

### S6-B. Give-up branch fired every tick

```text
ROOT CAUSE: CoSynclocalplayer Tick: `gaveUp = (now - downedSince) >= giveUp`
            had no latch, so after the timeout it was true on every tick until
            HP rose: SetNoBleedoutRecovery(false) queued into the Papyrus VM and
            a log line, twice per frame (1502 lines). The same missing latch
            also re-ran the give-up path after a successful revive if HP was
            still 0 at the timeout.
FIX:        s_recoveryReleased latch (Downed -> Released once), reset on the
            next down and on recovery.
```

### S6-C. Revive took too long

```text
ROOT CAUSE: A revive only unblocked vanilla recovery; the player then lay on
            the floor until the engine's bleedout regeneration reached its
            recovery threshold, and the revive heal was applied only after
            they had already stood up (respawn debounce).
ENGINE:     GameSettings (layout [vtable][value][name]) fBleedoutDefault 0.2,
            fBleedoutRate 15, fBleedoutMin 5, fBleedoutCheck 1,
            fEssentialDownCombatHealthRegenMult 0.75 (static defaults in the
            exe; Fallout4.esm GMSTs may override). Their exact use by the
            recovery check is UNVERIFIED; the fix does not depend on it beyond
            "health above the threshold ends bleedout at the next check".
FIX:        On revive, RestoreValue(Health, fReviveHealthPercent% of max) is
            queued through the VM together with the unblock (same VM route the
            code already used inside the down window; max = last value this
            machine sent while alive).
```

### S6-D. Revive heal permanently lowered max health

The post-recovery heal used `SetBase(Health, pct * max)`: that rewrites the **base** value, so every revive reduced the player's maximum health. Removed (the heal now happens at revive time via RestoreValue, which restores current health only).

### S6-E. Downed player stopped streaming ("downed animation does not work")

```text
ROOT CAUSE: The dead-player gate returned from the local tick whenever HP<=0,
            which includes essential bleedout. No EU, no BONES, no HP for the
            whole down window (host log: "tx ... unreliable=0" in every 10 s
            summary 21:43:40-21:44:20), so every other machine showed the
            proxy frozen or back in its idle graph -- never the bleedout pose.
            CoSyncLocalPlayer::IsLocallyConfirmedDead() (used by TickHook to
            skip animation capture) had the same over-broad meaning.
FIX:        The gate still stops everything on real death (engine life state
            dying/dead, CoSyncGameAPI::IsDeadOrDying); in bleedout the tick
            continues, so position, animation variables and bones keep
            streaming and the proxy shows the real downed and get-up
            animation. The unverified "SharedGetUp" PlayIdle on the proxy was
            removed (a second animation source on a bone-replicated proxy).
```

### S6-F. "Talk" instead of "Revive"

```text
ENGINE:     TESNPC vtable 0x24D6400 slot 0x60 = sub_140656D60(base, ref,
            BSString* out) builds an actor's crosshair text: "%s\n%s" of
            sub_140513630(ref) (display name) and the verb (sTalk: GMST value
            slot 0x2F22888, read at 0x657192), via BSString printf
            sub_1401F84F0. VERIFIED in IDA.
FIX:        MinHook on sub_140656D60: original runs, then for a downed player
            proxy the text is rewritten to "<name>\nRevive". The set of downed
            proxies is rebuilt every tick from the player manager (own mutex:
            the hook may run on an engine thread), so the prompt goes back to
            normal the moment the player is up.
```

### S6-G. Other players' UI never learned you got up on your own

Only DOWN / REVIVEREQ / REVIVED existed; recovering after the give-up timeout sent nothing, so peers kept you DOWNED (UI tag + Revive prompt) for the rest of the session. The recovered player now sends `REVIVED|<me>|0` (reviver 0 = unaided); receivers clear the state and say "<name> is back on their feet". A duplicate REVIVED (the reviver's own echo) is now ignored.

### S6-H. Revive messages were not sender-checked

Any client could mark any player down or revived. The host now requires: DOWN about yourself; REVIVEREQ as the reviver; REVIVED as the reviver, or about yourself with reviver 0.

### Protocol

`REVIVED|entity|0` is new in meaning (reviver 0); same message shape. Version stays 0.3.3 (both sides must run 0.3.3 anyway).

### TEST PLAN (Stage 6)

1. Get the client downed near the host: host sees the client's bleedout pose (not standing), prompt reads "Revive". Revive with a Stimpak: the client stands up about a second after the channel ends, prompt returns to "Talk"/normal, both UIs clear the DOWNED tag. Client's max HP unchanged (check Pip-Boy).
2. Let the timeout expire (60 s default): one "Gave up" line in the client log; client stands up via vanilla recovery; host's UI clears the tag ("back on their feet").
3. Try reviving while the downed player is still: no "moved out of range" cancellation.

---

## Stage 7 — pickup / carry / debris physics

### Authority model (as built, written down)

| Question | Answer |
|---|---|
| Who owns a carried object? | The machine whose player grabbed it (grab handle at PlayerCharacter+0xB5C, from the engine's GetPlayerGrabbedRef console command), and for 5 s after release (throw tail). |
| Who controls its transform / simulates it? | That machine's own Havok (the vanilla grab). |
| Who broadcasts it? | That machine only: `OBJ|` at 20 Hz with a 0.5-unit deadzone. Idle debris is reported by the host's 2 Hz cell walk (5-unit deadzone). |
| Who applies? | Everyone else, via MoveRefrToPosition; the owner ignores OBJ for objects it owns. |
| After release? | The owner keeps streaming the flight for the 5 s tail; afterwards the object is idle debris again. |

### S7-A. The host echoed objects that another player was carrying

```text
BUG:        A carried/thrown object jitters for other players.
Severity:   High
ROOT CAUSE: When a client carries an object, the host applies its OBJ| and
            relays it to everyone. The host's own 2 Hz cell walk then saw the
            object "moved" (its s_objLastSent baseline was never updated by the
            apply) and broadcast its own copy of the position -- up to 0.5 s
            old. Every machine other than the carrier received two streams for
            the same object and it jumped between them.
FIX:        Applying an OBJ| updates the baseline and marks the object
            remote-driven for 1.5 s; the host's walk skips remote-driven objects.
            The relay already delivers the carrier's stream to everyone, so
            nothing is lost; REQSTATE replay now also has the carrier's final
            position.
FILES:      World/CoSyncWorldSync.cpp (HostTickObjects_Impl,
            ClientTickObjectMoves_Impl)
```

### Findings not changed (need a test to decide)

* On the receivers the object stays a **dynamic** Havok body that is teleported 20 times a second; between teleports local gravity pulls it, so it can still wobble. The engine offers `ObjectReference.SetMotionType` (vanilla Papyrus: Motion_Fixed=0, Motion_Dynamic=1, Motion_Keyframed=2; native registered from sub_14115F4A0). Making remote-driven objects keyframed would stop the wobble, **but** a keyframed body right in front of the carrier's proxy would push that proxy's character controller every frame -- turning object jitter into carrier jitter. Which of the two the engine does was not verifiable offline, so it is left as the next step for a test session.
* The carrier's proxy jitter reported by the user is covered by S1-A (proxy positioned through the engine, so its capsule no longer fights the engine's controller sync).

### TEST PLAN (Stage 7)

1. Three machines if possible (host + 2 clients): client A picks up a box and walks around, then throws it. Host and client B see the box follow smoothly and land in the same spot.
2. Two machines: client carries a box; host sees the carrier walk without jitter. Repeat with the host carrying.
3. Join late after throwing things around: objects are where the thrower left them.
