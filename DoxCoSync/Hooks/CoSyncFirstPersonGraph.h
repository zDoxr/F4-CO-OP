#pragma once

#include <cstdint>

// Keeps the local player's PARKED third-person animation graph ticking while
// the camera is in first person. Without this, BSAnimationGraphManager only
// ever updates graphs[activeGraph] -- entering first person completely stops
// ticking the third-person graph (it freezes on its default bind/T-pose, not
// just "stale"), which is what CoSyncPlayerAnimation's bone-replication capture
// (CaptureBones, mode 2) was reading. Ported from FO4_Wrld's
// fw_native/src/hooks/first_person_graph.cpp (a separate, independently
// mature FO4 co-op project) -- see the CoSync memory entry
// "cosync-fp-graph-drive-rvas" for the full RE trail and struct-offset
// cross-checks behind every address used here. All RVAs below are re-derived
// against THIS build's Fallout4.exe; none of FO4_Wrld's own raw addresses are
// reused directly (they target a different game build).
//
// Always installs its hooks at plugin load (near-zero cost when idle: a
// couple of pointer/flag checks per frame). The actual extra per-frame work
// is gated live behind CoSyncPlayerAnimation::IsBoneReplicationEnabled() (ini
// [Animations] bDeltaPositionLocomotion=2) AND CoSyncNet::IsConnected() --
// solo play and every other bone-replication mode cost nothing extra, and
// the ini can be toggled without a restart.
namespace CoSyncFirstPersonGraph
{
    // Call once at plugin load with the Fallout4.exe module base (module
    // base + RVA, exactly the same pattern as CoSyncPlayerAnimation::Init).
    bool Install(uintptr_t moduleBase);

    // Diagnostics -- surfaced in the overlay/log the same way
    // CoSyncPlayerAnimation exposes its own capture stats.
    bool     IsDriving() noexcept;      // at least one driven frame has run
    uint64_t GetDriveCount() noexcept;
    uint64_t GetSehCount() noexcept;

    // True while the local player's camera is currently in first person.
    // Derived from the SAME signal (BSAnimationGraphManager::activeGraph)
    // the drive itself uses, observed live inside the Update hook every
    // frame -- not a new/separate mechanism, so it inherits everything
    // already verified about that signal this session (PC_SKELETON_SYNC
    // firing timing, driveCount behavior, transition logging all lined up
    // with it throughout testing).
    bool IsSenderInFirstPerson() noexcept;

    // The capture/send gate, ported from FO4_Wrld's own shipped design
    // (fw_native/src/native/scene_inject.cpp): "if in first person AND the
    // drive is NOT confirmed running, don't capture -- hold whatever was
    // last captured instead of shipping a broken/stub pose." Always true in
    // third person (unchanged, pre-existing behavior). Their own account:
    // sending a stub pose is what deformed remote proxies before this gate
    // existed; a stale-but-once-valid capture looks correct (frozen) rather
    // than wrong, and resumes the instant the drive confirms itself running
    // (usually within one frame of entering first person, per driveCount
    // reaching 1 almost immediately in every test this session).
    bool ShouldCaptureNow() noexcept;

    // ---- Generated-pose snapshot -------------------------------------------
    //
    // The pose every graph computes for EVERY bone in its animation skeleton,
    // before anything writes a NiNode. This is what makes first-person bone
    // capture possible at all: the first-person 3D has no lower-body nodes, so
    // the legs are computed and discarded -- but they are computed. Measured
    // live, first person, with the parked-graph re-tick disabled: RLeg_Thigh
    // in the active (first-person) graph's buffer changed on 666 of 705
    // consecutive 4 Hz samples, median step 0.903 deg, while the parked
    // third-person graph sat at median step 0.000 deg.
    //
    // Reached the way BShkbAnimationGraph::GraphPoseApply reaches it:
    //   genOut = *(void**)(graph + 0x1F8)
    //   tracks = *(void**)genOut
    //   pose   = tracks + *(int16*)(tracks + 52)
    //   count  = *(uint16*)(graph + 0x3C0)
    //
    // Indexed by ANIMATION SKELETON bone index, and the first- and third-
    // person rigs are the same rig for indices 0..93 (third person only
    // appends CamTarget at 94), so one index means the same bone either way.
    // Transforms are PARENT-LOCAL -- verified by a parked graph reading back
    // exactly the shipped skeleton's reference rotations.
    struct PoseSnapshotBone
    {
        float rot[4];   // quaternion xyzw
        float trans[3];
        float scale;
    };

    // Copies the most recent snapshot. Returns the bone count, 0 if none is
    // available. Safe to call from anywhere: the snapshot is taken inside the
    // graph-manager update (the only place the buffer is valid) and read back
    // from this copy, never from the engine's own memory.
    int GetPoseSnapshot(PoseSnapshotBone* out, int maxBones) noexcept;

    // Same, but always from graphs[0] -- the THIRD-person graph -- regardless
    // of which graph is active. In first person that graph is parked, so this
    // is only meaningful while the engine re-tick is arming it
    // ([Animations] bEngineTickParkedGraph=1); with the re-tick off it reads
    // the skeleton's reference pose. It exists because first-person clips carry
    // no body animation at all, so the third-person graph is the only possible
    // source of a real walk for a first-person sender.
    int GetThirdPersonPoseSnapshot(PoseSnapshotBone* out, int maxBones) noexcept;

    // Which graph the snapshot came from, and how old it is in frames.
    bool     PoseSnapshotWasFirstPerson() noexcept;
    uint64_t PoseSnapshotAge() noexcept;
}
