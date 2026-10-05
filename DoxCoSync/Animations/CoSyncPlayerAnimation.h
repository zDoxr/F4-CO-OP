#pragma once

#include <cstdint>
#include <string>

class Actor;
struct AnimVarSnapshot;

// Animation Testing module — isolated from the production animation path.
// Two paths:
//   1. ApplyFromSnapshot — graph-variable named-setter pass (original test).
//   2. CaptureBones / ApplySkeleton — FO4_Wrld-style skeleton bone replication
//      (active path when Testing is enabled).
//
// Bone replication pipeline:
//   CaptureBones(localPlayer) -> g_localCapture (per-frame, from local player tick)
//   ApplySkeleton(proxyActor) -> writes g_localCapture bones to proxy (baked by the re-stamp)
//
// Enabled via "Animations -> Testing" radio in the Mod Config overlay tab.
// Call Init() once at DLL load with the module base (for RVA resolution).

namespace CoSyncPlayerAnimation
{
    // 2026-08-21: raised from 128 alongside the walk-depth change (see
    // kMaxWalkDepth in the .cpp). At depth 12 the walk only ever reached 48
    // nodes so 128 was never the binding limit; at depth 20 the finger chains
    // come into range and a fully-geared human tree can exceed 128, and a DFS
    // that truncates drops whatever happens to come last in traversal order --
    // which varies with equipment. 192 clears it with headroom. Cost is the
    // size of SkeletonCapture (~23KB), which is copied per proxy per frame by
    // GetReceivedSkeleton; measured against a 60Hz two-proxy session that is
    // a few MB/s of memcpy, i.e. noise.
    static constexpr uint32_t MAX_SKELETON_BONES = 192;

    // Per-bone transform captured from the local player's NiNode tree.
    // rot[9] = row-major 3x3 (SIMD pad floats stripped).
    // trans[3] = local translation at NiAVObject+0x60.
    //
    // nameHash is the wire identity of the bone: FNV-1a 32 over the node's
    // name (see HashBoneName in the .cpp). It replaces the old canonical
    // INDEX as the thing that crosses the network, because an index is only
    // meaningful against a list both machines agree on and nothing could
    // guarantee that -- see SerializeSkeletonCapture's comment. `name` is
    // kept for logging/diagnostics only; nothing matches on it.
    struct BoneTransform
    {
        char     name[64];
        uint32_t nameHash;
        float    rot[9];
        float    trans[3];
        float    scale;
    };

    // Snapshot of the local player's full skeleton (up to MAX_SKELETON_BONES bones).
    // recvMs is GetTickCount64() at the moment OnReceiveBones parsed this
    // capture off the wire -- 0 for a locally-produced capture. It drives the
    // FRESH/HOLD/RELEASE tiering in ApplySkeleton so a peer that stops sending
    // eventually hands its proxy back to the normal animation path instead of
    // pinning it to the last pose forever.
    struct SkeletonCapture
    {
        BoneTransform bones[MAX_SKELETON_BONES];
        uint32_t      count;
        bool          valid;
        uint64_t      recvMs;
        // Sender was in first person when this was captured. Their parked
        // third-person graph does not animate the lower body, so only the
        // upper half of this capture is trustworthy -- see ApplySkeleton.
        bool          upperBodyOnly;
    };

    // Anim states (mirrors FO4_Wrld's NPC state model)
    enum class AnimState : uint8_t
    {
        Idle      = 0,
        Walking   = 1,
        Running   = 2,
        Aiming    = 3,
        Firing    = 4,
        Reloading = 5,
        Dead      = 6,
    };

    // Call once at plugin load time (before any Tick/Apply).
    void Init(uintptr_t moduleBase) noexcept;

    bool IsEnabled();
    void SetEnabled(bool enabled);

    // Capture local player's skeleton bones into g_localCapture.
    // Call once per frame from the local player tick (main thread only).
    void CaptureBones(Actor* player) noexcept;

    // Read-only access to the capture CaptureBones() just filled, for
    // SerializeSkeletonCapture. Main thread only, same as CaptureBones.
    const SkeletonCapture& GetLocalCapture() noexcept;

    // Apply g_localCapture bones to proxy actor's skeleton. World transforms
    // are recomputed by ReassertPoseForGraphRoot in the engine's animation
    // update, not here (see the 2026-10-03 note in ApplySkeleton).
    // Replaces ApplyFromSnapshot when Testing is ON (main thread only).
    void ApplySkeleton(Actor* proxy) noexcept;

    // Same as ApplySkeleton(Actor*), but against an explicit capture instead
    // of the local player's own g_localCapture -- this is what lets a proxy
    // be driven by a REMOTE peer's bones instead of mirroring whoever is
    // sitting at this keyboard. Used by the network receive path below.
    // Returns true if at least one bone was actually written -- false means
    // either the capture or the proxy's own node walk came back empty (e.g.
    // proxy 3D not settled yet), and the caller should fall back to the
    // normal graph-var path rather than silently doing nothing this tick.
    bool ApplySkeleton(Actor* proxy, const SkeletonCapture& capture) noexcept;

    // [Animations] bDeltaPositionLocomotion=2 OR =3 in CoSync.ini. Real
    // per-bone pose replication for remote player proxies (see the network
    // functions below), as opposed to =1's derived-locomotion approach for NPC
    // puppets. Both 2 and 3 run the identical pipeline; see IsMinimalMode.
    // Cached; safe to call every frame.
    bool IsBoneReplicationEnabled();

    // [Animations] bDeltaPositionLocomotion=3 -- the clean bundled preset. It
    // is the exact same working first-person pipeline as =2, but as ONE switch:
    // it forces the third-person generated-pose capture source and the engine
    // parked-graph re-tick on (no need for the bCaptureFirstPersonSkeleton /
    // bEngineTickParkedGraph companion keys), and it silences the high-volume
    // experimental diagnostics so only the essential lines remain. =2 is left
    // untouched as the full experimental/diagnostic path. Cached; call freely.
    bool IsMinimalMode();

    // [Animations] bCaptureFirstPersonSkeleton in CoSync.ini (default 1 = on).
    //
    // When on, CaptureBones reads the FIRST-person skeleton
    // (GetActorRootNode(true)) while the local player's camera is in first
    // person, instead of the parked third-person one. Third person is
    // untouched either way, and the receiver needs no change at all: the
    // wire format is keyed on HashBoneName and ApplySkeleton matches by
    // name, and the two rigs use the same names.
    //
    // Why this is sound, measured from the shipped files rather than assumed
    // (Behaviour Graph Studio reading Fallout4 - Animations/Meshes.ba2):
    //
    //   * _1stperson/CharacterAssets/skeleton.hkx is 94 bones,
    //     CharacterAssets/skeleton.hkx is 95. Bones 0..93 are IDENTICAL --
    //     same names, same parent indices, and the reference poses match to
    //     within 0.001 on every body bone. The only 13 differing entries are
    //     Weapon{Bolt,Extra*,Magazine*,Optics*,Trigger}, weapon-attachment
    //     nodes absent from both render NIFs. Index 94 (CamTarget) is third
    //     person only.
    //   * Named non-_skin nodes: 81 in the third-person skeleton.nif, 77 in
    //     the first-person one, and the first-person set is a strict SUBSET.
    //     The only four names it lacks are CPA, CamTargetParent, CamTarget
    //     and CharacterBumper -- not one body bone.
    //   * The first-person locomotion clips are FULL BODY. Decoded from the
    //     shipped hkx: _1stperson/animations/1hm/runforward.hkx drives 74 of
    //     94 tracks, sprintforward.hkx 75, 44pistol/wpnwalkforwardready.hkx
    //     75 -- and MOVING includes COM, LLeg_Thigh/Calf/Foot/Toe1,
    //     RLeg_Thigh/Calf/Foot/Toe1, Spine1, Spine2, Chest, Neck, Head. The
    //     legs are posed in first person; they are merely not drawn.
    //   * Nothing masks them: _1stperson/behaviors/mtbehavior.hkx and
    //     rootbehavior.hkx contain ZERO hkbBoneWeightArray objects, and
    //     gunbehavior.hkx's partitions split WEAPON bones from BODY bones
    //     with the full leg chain on the body side.
    //
    // So in first person the engine is already ticking a full-body graph
    // every frame, on a rig index-identical to the proxy's, while
    // CoSyncFirstPersonGraph spent its effort resurrecting the parked graph
    // beside it. Cached; safe to call every frame.
    bool IsFirstPersonCaptureEnabled();

    // Raw value of the same key, because the modes are not a boolean:
    //   0 = third-person node tree in both cameras (the original behaviour)
    //   1 = first-person NODE TREE. Disproven: that tree is the left-arm +
    //       Pip-Boy view rig, 11 real bones and 68 Pip-Boy model nodes.
    //   2 = first-person GENERATED POSE. Reads the transforms the graph hands
    //       the engine before anything writes a NiNode. The MECHANISM works
    //       and is validated (see below), but it does not solve first person,
    //       for a reason that has nothing to do with the engine:
    //
    //       FALLOUT 4'S FIRST-PERSON ANIMATIONS CONTAIN NO BODY ANIMATION.
    //       Decoded straight out of the shipped hkx, max rotation swing over a
    //       full cycle:
    //                        1P run   1P sprint  1P wpnwalk   3P run
    //         RLeg_Thigh      0.00       0.01       0.00       21.41
    //         LLeg_Calf       0.17       0.22       0.00       32.31
    //         Spine1          0.00       0.00       0.00        3.37
    //         LArm_UpperArm   1.03      17.82       0.00       14.00
    //
    //       The leg tracks EXIST as spline channels -- which is why an earlier
    //       reading of this said "runforward drives 74 of 94 tracks including
    //       both legs" -- but they hold a constant value. RotationAnimated
    //       being true means the channel is present, not that it varies. The
    //       constant the 1P thigh holds, (-0.0398, 0.9858, -0.1472, 0.0698),
    //       is byte-for-byte what the live pose buffer reports.
    //
    //       So a first-person sender has no source of third-person locomotion
    //       anywhere: the 1P assets have none, and the 3P graph is parked
    //       (force-ticking it yields 72 distinct leg values over 484 samples --
    //       sporadic jumps, not a cycle). Mode 2 in first person reproduces the
    //       sender's first-person pose faithfully, which on a third-person body
    //       reads as fists held up at the face and straight legs.
    //
    //       Kept because the machinery is correct and reusable: the pose buffer
    //       carries all 95 bones in THIRD person, including the fingers and
    //       twist bones the node walk never reached (that walk returns 57
    //       nodes, of which only ~29 are real bones and the rest are Pip-Boy
    //       and face-mesh geometry). Extending this to the third-person capture
    //       is a straight fidelity and bandwidth win; first person needs the
    //       variable/event path instead, per ENGINE_ANIM_PIPELINE.md.
    int GetFirstPersonCaptureMode();

    // Tell the capture path whether the local player is in first person. The
    // flag travels with every BONES packet: a first-person sender's parked
    // third-person graph never animates their lower body, so the receiver
    // drives only the upper half from bones and lets the proxy's own graph
    // animate the legs from synced locomotion. Call once per frame.
    void SetLocalFirstPerson(bool inFirstPerson) noexcept;

    // ---- Network bone replication -----------------------------------------
    // CaptureBones() only ever fills the LOCAL player's own g_localCapture --
    // there is no implicit network path connecting it to a remote peer's
    // proxy. These functions are that path: call SerializeSkeletonCapture on
    // the sender after CaptureBones, transmit the result, and feed received
    // packets to OnReceiveBones on every peer (including the sender's own
    // relay-through, matching every other packet type's host-relay pattern).
    //
    // 2026-08-20 — redesigned to match FO4_Wrld's own mature pose-replication
    // pipeline (fw_native/src/native/scene_inject.cpp, capture_and_send_npc_pose
    // / apply_npc_pose_to_actor) rather than CoSync's original from-scratch
    // design. Four changes from the original wire format + apply path, see
    // project-cosync-bone-replication-fix / project-cosync-fp-graph-drive-rvas
    // memory for the full comparison:
    //   1. Wire format is a HASH-KEYED quaternion (4 floats) per bone --
    //      "<fnv1a32 of the bone name>,qx,qy,qz,qw" -- instead of a 9-float
    //      rotation matrix + 3-float translation + scale + a bone NAME STRING
    //      per bone.
    //
    //      2026-08-21 CORRECTION: this was a canonical-INDEX format between
    //      2026-08-20 and now, on the theory that sorting each machine's own
    //      bone-name list alphabetically made two independently-built lists
    //      agree on index->name without synchronization. Sorting does make
    //      two lists agree on ORDER given the same input SET; it does nothing
    //      about the SET differing, which is what actually happens. Measured
    //      live 2026-08-21 (CoSyncUpdated_36724/34592): on ONE machine the
    //      local player's own skeleton walks to 48 nodes while a remote
    //      proxy's walks to 47, and the name match rate is 42/48 -- i.e. two
    //      humanoid actors already disagree on ~5 node names inside a single
    //      process. Since the list could be seeded from EITHER source
    //      (whichever this machine walked first), two peers could trivially
    //      end up with 47- and 46-entry lists, at which point every index
    //      past the first divergence names a DIFFERENT bone and an upper-arm
    //      rotation lands on a thigh. Hashing the name removes the entire
    //      failure class: bone identity now travels with the bone and needs
    //      no agreement of any kind. It also deletes the canonical list, its
    //      lock-in ordering race, and the "can't decode until this machine
    //      has captured once" deadlock the 2026-08-21 proxy-seeding hack
    //      existed to work around.
    //
    //      A bone the sender did not capture this frame is simply absent from
    //      the packet, and the receiver keeps whatever it last had for it --
    //      same outcome as the old qw=2.0 sentinel, without spending 13 bytes
    //      per absent bone to say nothing.
    //   2. Translation is no longer sent/applied for every bone -- only for
    //      COM/Pelvis, exactly like FO4_Wrld's separate "crouch" channel.
    //      FO4_Wrld found that writing translation onto an arbitrary matched
    //      bone stretches the model into whatever coordinate the source bone
    //      happened to be at whenever the two skeletons' assumptions diverge
    //      (their case: a creature's name collision with a human joint; less
    //      likely for CoSync's always-human-to-human proxies, but the wire
    //      format now matches their design either way per user direction).
    //   3. ApplySkeleton no longer re-walks the proxy's whole skeleton via
    //      WalkAllNodes every single call -- see the per-actor pinned bone
    //      cache in the .cpp (mirrors FO4_Wrld's NpcBoneCache / ni_incref /
    //      ni_decref, Build 68.4), rebuilt only when the proxy's root pointer
    //      changes.
    //   4. The applied pose now nlerps toward the latest target and
    //      extrapolates using the previous sample's delta (mirrors FO4_Wrld's
    //      c.42 kNpcPoseSmooth/extrapolation) instead of snapping instantly
    //      on every arrival.
    //
    // Returns an empty string if capture.valid is false.
    std::string SerializeSkeletonCapture(uint32_t entityID, const SkeletonCapture& capture);

    bool IsBonesPacket(const std::string& msg);

    // Parses a "BONES|..." packet and stores it as entityID's latest received
    // capture (overwriting any previous one for that entity) for later
    // ApplySkeleton(proxy, capture) calls. Call from the network receive
    // dispatcher for any message IsBonesPacket() accepts. Stamps recvMs.
    void OnReceiveBones(const std::string& msg);

    // Drops entityID's stored capture (see OnReceiveBones). Call when a
    // player entity departs so a later respawn under the same ID cannot be
    // driven by a pose captured before it left.
    void ForgetReceivedSkeleton(uint32_t entityID);

    // Copies the latest received capture for entityID into `out` (see
    // OnReceiveBones) under lock. Returns false and leaves `out` untouched
    // if none has arrived yet. Copy-out rather than a pointer/reference
    // because OnReceiveBones may run on the network thread while this is
    // called from the game thread.
    bool GetReceivedSkeleton(uint32_t entityID, SkeletonCapture& out);

    // 2026-08-21 -- WRITE-LAST re-stamp. This is the fix for the race the
    // write-order probe measured live: ApplySkeleton runs from F4SE's
    // main-thread task pump, while the proxy's own behaviour graph writes the
    // SAME +0x30 local matrices, so the two were simply fighting. Measured on
    // both machines: of 55 applied bones, ~25 were being overwritten by the
    // engine before the frame was drawn -- and those 25 are the ones the
    // graph actually animates, i.e. exactly the ones that carry the pose.
    //
    // Rather than move the whole apply (tier logic, smoothing state, fallback
    // decision) onto the engine's thread, ApplySkeleton keeps its job and
    // publishes the exact matrices it wrote. This function re-stamps that
    // same set immediately AFTER the engine's own graph update has run, from
    // inside that update, so ours is the last write before the bake. It does
    // no quaternion math and advances no smoothing state, so it cannot
    // double-apply the way a second full ApplySkeleton call would.
    //
    // `graphRootNode` is the graph's own write-target node 0 -- the "Root"
    // bone, which is how a record is matched back to a proxy without needing
    // an Actor* on this path. Returns true if anything was written.
    //
    // MAY RUN OFF THE MAIN THREAD (the engine's animation update is not
    // guaranteed to be main-thread), so the record store has its own lock and
    // deliberately shares no state with the main-thread-only bone cache.
    bool ReassertPoseForGraphRoot(void* graphRootNode) noexcept;

    // How many times the above actually wrote, for diagnostics.
    uint64_t GetReassertCount() noexcept;

    // Diagnostic: read one named bone's local 3x3 rotation off `actor`'s
    // third-person skeleton, using the same SEH-guarded walk CaptureBones and
    // ApplySkeleton already use every frame. Returns false if the actor has no
    // settled 3D or the bone is not present.
    //
    // Exists so callers outside this module can sample a bone WITHOUT poking
    // engine structures themselves. The first two attempts at a first-person
    // leg probe did exactly that -- indexing the animation graph's own array
    // at graph+0x2A0 using graph+0x3C0 as its length -- and crashed both
    // machines twice: that field is the graph's bone count, not the array
    // length, and the slots past the real end read as 0x00000000FFFFFFFF,
    // which is non-null AND inside canonical user space, so neither a null
    // check nor a range check rejects it. This walk starts from
    // GetActorRootNode and follows real parent/child links, so it can only
    // ever visit nodes that are actually in the tree.
    // `firstPerson` selects which of the player's two skeletons to read
    // (default false = third-person, matching every existing call site).
    bool SampleBoneRotation(Actor* actor, const char* boneName, float outRot[9], bool firstPerson = false) noexcept;

    // 2026-08-26 TEMPORARY LIVE DIAGNOSTIC: reported bug is "using a Stimpak
    // in first person doesn't visually replicate to a proxy; third person
    // does." Static IDA decompilation of the engine's equip-visual pipeline
    // (ActorEquipManager::EquipObject and its whole visual-attach chain, four
    // decompilation passes deep) found no first-person/third-person branch
    // anywhere it could reach -- genuine diminishing returns for static
    // analysis. This empirically answers the same question instead: arm a
    // short window (from CoSyncEquipHook when the local player equips an
    // ALCH item) during which CaptureBones logs a witness bone's rotation
    // from BOTH the third-person and first-person skeletons every few
    // frames, tagged with which one capture is currently reading from. If
    // only the first-person bone changes while using a stimpak in first
    // person, the item visual is landing on the view-model skeleton bone-rep
    // never captures from -- confirming the theory with a live measurement
    // instead of another speculative decompilation pass. Remove once the
    // question is answered and any resulting fix is verified.
    void StartAlchEquipDiag() noexcept;

    // Releases this proxy's pinned bone-pointer cache (see the .cpp) and its
    // nlerp smoothing state. Call when a proxy actor is despawned/departs --
    // matches FO4_Wrld's release_npc_bone_cache. Safe to call for an actor
    // with no cache entry (no-op). MAIN THREAD ONLY (may drop the last
    // reference on a cached bone pointer).
    void ReleaseBoneCache(Actor* proxy) noexcept;

    // Legacy graph-variable path (kept for reference / fallback).
    void ApplyFromSnapshot(Actor* actor, const AnimVarSnapshot& snapshot) noexcept;

    // Per-frame tick (currently no-op — capture is driven by CaptureBones).
    void Tick(float deltaTime);

    // Apply anim state to a remote proxy actor.
    void ApplyAnimState(Actor* actor, AnimState state) noexcept;

    // Apply locomotion variables directly.
    void ApplyLocomotion(Actor* actor, float speed, float direction) noexcept;
}
