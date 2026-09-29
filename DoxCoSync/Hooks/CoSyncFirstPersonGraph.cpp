#include "CoSyncFirstPersonGraph.h"

#include "GameReferences.h"
#include "GameTypes.h"
#include "GameCamera.h"
#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncPlayerAnimation.h"
#include "CoSyncAnimVarSystem.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "MinHook.h"

extern PlayerCharacter* CoSyncGetPlayer();

// Defined further down in this file, inside the module's own namespace.
// Forward-declared so the gate diagnostic can print camera state and graph
// state on one line instead of forcing timestamp correlation across two.
namespace CoSyncFirstPersonGraph { bool IsLocalPlayerInFirstPerson() noexcept; }

// ============================================================================
// RVAs -- Fallout4.exe 1.11.240 (disk), re-derived from FO4_Wrld's 1.11.191
// first_person_graph.cpp via headless IDA. See memory "cosync-fp-graph-drive-
// rvas" for the full trail: most of this code region shifted by a constant
// +0x3F0 between the two builds (confirmed across 9 independently-decompiled
// functions), the PlayerCharacter-class functions did NOT follow that shift
// and were found separately (PC_SKELETON_SYNC by a small manual offset scan,
// PopulateGraphVariables by a string cross-reference search). Every RVA below
// was independently decompile-verified against the actual 1.11.240 binary,
// not guessed from FO4_Wrld's stale addresses.
// ============================================================================

namespace
{
    // 2026-08-20: DISABLED PER EXPLICIT DIRECTION. Every diagnostic this
    // session eventually came back clean (correct write target, correct
    // tree, real changing bone values, live-varying active-node count), but
    // the actual RESULT still didn't look as smooth as third person, and
    // user's own call: "First person is broken... force Third person the
    // whole time to fake First person and possibly bypass it breaking."
    // Third person's own replication (same capture/send/apply pipeline,
    // same 15Hz rate, same no-interpolation) already works reliably --
    // rather than keep iterating on the manual-drive machinery (activation
    // sequencing, forced context, foot IK, locomotion vars, all revisited
    // this session), the simplest and lowest-risk fix is to stop attempting
    // it. With kAttemptFirstPersonDrive=false, DriveThirdPersonGraph never
    // runs, so IsDriving() is permanently false, which collapses
    // ShouldCaptureNow()'s existing gate (see the .h) to exactly "always
    // hold the last known-good pose in first person, always capture
    // normally in third person" -- the proxy freezes on the sender's last
    // real 3rd-person pose instead of shipping a driven-but-imperfect one,
    // and resumes the instant they return to third person. Every OTHER
    // hook's special-case logic (SetActiveGraph revive/undo,
    // GraphDeactivate suppression, PC_SKELETON_SYNC suppression) is gated
    // on drive-state that can now never trigger, so they fall through to
    // vanilla behavior automatically -- no need to also revert them
    // separately. Flip back to true to resume the drive experiment; all of
    // its code is untouched and intact below.
    //
    // 2026-08-21 RE-ENABLED per explicit user direction ("don't stop till
    // 100% of the pipe is complete"). Everything downstream of first-person
    // detection was fixed today (IsLocalPlayerInFirstPerson via PlayerCamera
    // replaces the broken shared-hook cache; the whole capture/apply/wire/
    // smoothing pipeline was rewritten and confirmed working in third
    // person). None of that touches this drive machinery directly, but the
    // RECEIVING side now nlerps + extrapolates instead of snapping, which
    // may change how any remaining drive imperfections actually look --
    // worth a fresh, informed look rather than trusting pt.17-18's verdict
    // from before all of today's fixes landed. All drive code below is
    // untouched and intact.
    // 2026-08-21 pt.7 -- DISABLED, superseded by kForceThirdPersonGraph.
    //
    // The manual drive is the whole "tick the PARKED third-person graph
    // ourselves" experiment. It was made to work in every measurable respect
    // this session -- events reach it and move its state machine
    // (MoveStart/SprintStart logged, activeNodes 29..62), the live speed is
    // handed over, the active-node refresh is wired, the pose-apply runs with
    // all 95 bones -- and the legs still would not animate, because a parked
    // graph is missing things the engine only ever does for the ACTIVE graph
    // (locomotion variable population, clip playbackSpeed, node-list upkeep).
    // Each one we replaced by hand revealed another.
    //
    // Rather than keep reimplementing the engine's own per-frame driver, the
    // new approach stops the graph being parked at all: see
    // kForceThirdPersonGraph. All of the drive code below is left intact and
    // reachable by flipping this back to true.
    // 2026-08-21 pt.9 -- RE-ENABLED. Disabling this was the wrong call, and
    // FO4_Wrld's own changelog for the same bug (v0.6.5, "First-person ghost
    // animation") says why: the manual drive IS their fix. A non-SELECTED
    // graph is never ticked by the engine (one index at mgr+0xD8 chooses), so
    // something has to tick it, and that is this.
    //
    // What CoSync was missing is one item from their list, not the approach:
    // "Keep it alive across camera switches. Reactivation rebuilds the
    // active-node list from the root, i.e. RESETS THE STATE MACHINE --
    // measured live as a drop from 59 active nodes to 19. The deactivate on
    // the way into first person is suppressed so a walking body stays
    // walking." See kPinTpGraphAcrossSwitch below.
    // 2026-08-22 -- OFF. Six mechanisms were built, measured and disproven on
    // the parked graph this session (active-node refresh, live speed hand-off,
    // clip playback speeds, the iSyncIdleLocomotion switch, pinning the graph
    // activated, pinning the selector). The drive faithfully reproduces the
    // engine's own flush/generate/pose-apply sequence -- verified instruction
    // by instruction against BSAnimationGraphManager::Update -- and the pose
    // still comes out with a static lower body. It costs CPU and log volume
    // for no measured benefit, so it is off until there is a reason to try
    // again. All of its code is intact behind this flag.
    // 2026-08-23 -- ON. Reverting to the reference implementation EXACTLY.
    // Every divergence from it that this project accumulated is now gated
    // behind kExactReferenceActivation below.
    //
    // 2026-08-24 -- OFF (Experiment B). Exp A's live test proved the manual
    // drive CANNOT advance the parked graph's per-node clocks on 1.11.240:
    // during steady FP walking (Speed=350, idleLoco=1) RLeg_Thigh stayed
    // byte-identical AND SpeedSmoothed (a time-integrated damper) sat at 43
    // vs 297 in 3P with the same Speed -- i.e. the Havok step receives dt
    // (graph timers 0x350/0x354 reset every frame) but does not propagate it
    // to the node clocks on a hand-driven parked graph. The engine's OWN
    // per-frame flow advances them perfectly (3P works). So stop reproducing
    // the tick by hand and let the engine do it: see kEngineTickParkedGraph.
    // The whole manual-drive machinery below is intact behind this flag.
    constexpr bool kAttemptFirstPersonDrive = false;

    // -- BSAnimationGraphManager / BShkbAnimationGraph (the +0x3F0 region) --
    constexpr uintptr_t RVA_MGR_UPDATE        = 0x0130E630; // BSAnimationGraphManager::Update
    constexpr uintptr_t RVA_MGR_NOTIFY        = 0x0130EED0; // BSAnimationGraphManager::NotifyAnimationGraph
    constexpr uintptr_t RVA_SET_ACTIVE_GRAPH  = 0x0130F1E0; // BSAnimationGraphManager::SetActiveGraph
    constexpr uintptr_t RVA_GRAPH_EVENT_FLUSH = 0x01321290; // flush queued events onto one graph
    constexpr uintptr_t RVA_GRAPH_UPDATE      = 0x01320920; // generate (HKB step, no bone write yet)
    constexpr uintptr_t RVA_GRAPH_POSE_APPLY  = 0x01321030; // write generated pose to NiNodes
    constexpr uintptr_t RVA_GRAPH_ACTIVATE    = 0x01320820; // revive a parked graph (idempotent)
    constexpr uintptr_t RVA_GRAPH_DEACTIVATE  = 0x01326760; // resets the graph's HKB state to initial (T-pose)
    constexpr uintptr_t RVA_GRAPH_SEND_EVENT  = 0x013213D0; // send ONE event to one graph
    // Dispatched internally by the engine via vtable+0x98 inside
    // GraphUpdate/GraphPoseApply -- resolved here for DIAGNOSTIC logging only.
    constexpr uintptr_t RVA_GRAPH_PHYS_PRED   = 0x01328130;
    // 2026-08-19: fresh full decompile of SetActiveGraph (not the earlier
    // partial/summarized read) shows GraphActivate is never called alone in
    // the real engine flow -- the incoming graph gets a PREP call right
    // before it and a COMMIT call right after, both skipped by the v1 port
    // (which only ever calls GraphActivate in isolation). Plausible
    // explanation for a graph that "generates" and "writes bones" every
    // frame (both independently confirmed live) yet produces a BYTE-FOR-
    // BYTE FROZEN pose for 15+ continuous seconds: an incompletely-finished
    // activation (e.g. an active-node list never actually populated).
    constexpr uintptr_t RVA_GRAPH_ACTIVATE_PREP   = 0x0052A800; // sub_14052A800(newGraph+104, mgr) -- called BEFORE GraphActivate
    constexpr uintptr_t RVA_MGR_ACTIVATE_COMMIT   = 0x013AA730; // sub_1413AA730(mgr+136, &newGraphPtr) -- called AFTER GraphActivate

    // -- PlayerCharacter class (does NOT follow the +0x3F0 shift) --
    constexpr uintptr_t RVA_PC_SKELETON_SYNC   = 0x00D7DE10; // copies FP arms onto the 3P body every frame
    constexpr uintptr_t RVA_PC_POPULATE_VARS   = 0x00CA9A40; // PlayerCharacter::PopulateGraphVariables

    // REVERTED 2026-08-20: briefly used CoSyncPlayerAnimation.cpp's graph-
    // variable-bool setter to poke bHumanoidFootIKDisable and 6 locomotion-
    // drive flags (bAnimationDriven/bAllowRotation/bGraphDrivenTranslation/
    // bGraphDrivenRotation/bFreezeSpeedUpdate/bFreezeRotationUpdate),
    // confirmed present in RaiderRootBehavior.hkx via `strings`. Undone per
    // explicit direction: `bDeltaPositionLocomotion=2` (this whole feature's
    // ini gate) is documented as REAL per-bone skeleton replication
    // specifically INSTEAD OF deriving Speed/Direction and touching the
    // proxy's own graph-variable-driven animation -- SetGraphVarBool
    // dispatches through the holder to ALL of the actor's graphs, so this
    // was mutating the SENDER's actual live animation state (gameplay-
    // visible, not just replication data), directly contradicting that
    // design. This whole feature must stay a pure bone-transform capture/
    // apply pipeline -- no graph-variable writes of any kind.

    // -- Struct offsets (decimal in code, matching FO4_Wrld's own verified
    //    table -- these are stable across builds; only code RVAs move) --
    constexpr size_t MGR_ARRAY_FLAGS_OFF = 0x40;
    constexpr size_t MGR_ARRAY_DATA_OFF  = 0x48;
    constexpr size_t MGR_ARRAY_SIZE_OFF  = 0x50;
    constexpr size_t MGR_EVENT_ARRAY_OFF = 0x10;
    constexpr size_t MGR_LOCK_OWNER_OFF  = 0xC8;
    constexpr size_t MGR_LOCK_COUNT_OFF  = 0xCC;
    constexpr size_t MGR_ACTIVE_OFF      = 0xD8;

    constexpr size_t GRAPH_HKB_WRAPPER_OFF = 0x378; // -> hkbBehaviorGraph wrapper
    constexpr size_t HKB_ACTIVE_BYTE_OFF   = 0x1AA;  // wrapper+0x1AA: graph is live
    // Struct offset only (trusted from FO4_Wrld's own table, cross-context
    // with other confirmed offsets in this same wrapper struct) -- m_active
    // Nodes pointer + count, diagnostic-only. If the fix below is wrong,
    // this number settles the active-node-list theory directly: 0 or a
    // permanently-unchanging small number across many driven frames would
    // mean the state machine genuinely has nothing to advance regardless of
    // dt, matching FO4_Wrld's own "round 7" failure description.
    constexpr size_t HKB_ACTIVE_NODES_OFF  = 0xE0;   // wrapper+0xE0: m_activeNodes ptr; count at [ptr+8]
    // graph+0x3C3: when non-zero the pose worker (0x0131E030) wipes the
    // character event queue and returns without running handleEvents, so the
    // graph's own clip/transition events are discarded every frame. CoSync
    // never writes it; logged here to find out whether the engine leaves it
    // set on a parked graph.
    constexpr size_t GRAPH_MUTE_BYTE_OFF = 0x3C3;
    constexpr size_t GRAPH_EVENT_PENDING_OFF = 0x3CA; // GraphSendEvent refuses while set

    // ── 2026-08-21: HKB active-node-list refresh ────────────────────────────
    //
    // The piece this drive was knowingly missing ("needs 4 more RVAs this pass
    // never located"). Its absence was measured live, not guessed at: on a
    // client that sat in first person for a whole session with the drive
    // running (driveCount 2320, RAN=YES, boneWriteBranch=TAKEN, boneCount=95
    // every frame) the graph's activeNodes count NEVER MOVED off 57, and
    // RLeg_Thigh was byte-for-byte identical across all 458 witness samples.
    // The other machine, whose graph was being driven by the engine itself,
    // cycled through 37/55/57/62/67 active nodes and 313 distinct leg poses.
    //
    // That is exactly the failure the refresh prevents: the timestep gets
    // fanned out over a STALE node list, so the clip clock stops while
    // generate still writes a full pose. Procedural modifiers (aim, look)
    // keep moving the arms -- which is why the arms looked alive and the legs
    // did not -- but every clip-driven joint, i.e. the entire walk cycle,
    // freezes. The earlier note that this "only matters after a FRESH revive"
    // understated it: in first person it never recovers.
    //
    // The engine does this itself before its own Update whenever the dirty
    // byte is set. Its call site (0x0147F232) is where two of these four came
    // from, ground-truthed rather than shifted:
    //
    //     cmp  byte [r14+1ACh], 0        ; hkb nodes-dirty
    //     je   skip
    //     xor  r8d, r8d
    //     mov  rdx, rdi                  ; hkbContext*
    //     mov  rcx, r14                  ; hkbBehaviorGraph*
    //     call 0x0148EF90                ; updateActiveNodes(hkb, ctx, 0)
    //     mov  rdx, rdi
    //     mov  rcx, r14
    //     call 0x01480B40                ; updateSyncInfo(hkb, ctx)
    //
    // The context ctor/dtor pair was then confirmed at the SAME +0x420 delta
    // from FO4_Wrld's 1.11.191 values (vs the +0x3F0 that holds across the
    // 0x130-0x132 graph region -- this part of the binary moved differently,
    // which is why a blanket shift produced mid-function addresses). Both are
    // exact function starts, they share call sites, and they verify
    // semantically: the ctor stores the character at ctx+0x00, zeroes
    // ctx+0x08..0x48, AddRefs the world into ctx+0x50 and stores arg4 at
    // ctx+0x58; the dtor reads ctx+0x50 back and releases the same refcount.
    constexpr uintptr_t RVA_HKB_CTX_CTOR   = 0x013A9E30; // hkbContext::hkbContext(ctx, character, world, nullptr)
    constexpr uintptr_t RVA_HKB_CTX_DTOR   = 0x013A9EB0; // hkbContext::~hkbContext(ctx)
    constexpr uintptr_t RVA_HKB_UPD_ACTIVE = 0x0148EF90; // hkbBehaviorGraph::updateActiveNodes(hkb, ctx, 0)
    constexpr uintptr_t RVA_HKB_UPD_SYNC   = 0x01480B40; // hkbBehaviorGraph::updateSyncInfo(hkb, ctx)

    // Verified in the same pass, from the engine's own ctor call site:
    //   cmp qword [rdi+3B8h], r14   -> physics-world holder
    //   mov r8,   qword [rdi+378h]  -> hkb wrapper (matches GRAPH_HKB_WRAPPER_OFF)
    //   lea rdx,  [rdi+1C8h]        -> INLINE hkbCharacter (lea, not mov)
    constexpr size_t GRAPH_PHYS_WORLD_OFF     = 0x3B8;
    std::atomic<uintptr_t> g_moduleBase{0};

    constexpr size_t GRAPH_HKB_CHARACTER_OFF  = 0x1C8;

    // ── hkbGeneratorOutput: the pose the graph computed, BEFORE anything
    //    writes a NiNode ────────────────────────────────────────────────────
    //
    // 2026-08-24. This is the answer to "the first-person tree has no lower
    // body": stop reading NiNodes. Every graph computes a full pose for every
    // bone in its animation skeleton and hands it to the engine as an
    // hkbGeneratorOutput; only the bones that HAVE a node ever get written
    // out. The rest are computed and dropped. The first-person locomotion
    // clips do drive the legs (verified from the shipped hkx: runforward.hkx
    // drives 74 of 94 tracks including LLeg_/RLeg_ Thigh, Calf, Foot, Toe1),
    // so in first person those transforms exist in this buffer every frame.
    //
    // Layout ground-truthed two independent ways that agree exactly:
    //   1. Decompiled hkbPoseStoringGeneratorOutputListener's own notify
    //      method (RVA 0x19E3C10) -- the engine's own "take the generator
    //      output and store the pose" code. It reads numTracks at tracks+4,
    //      requires it > 2, tests bit 0x10 of tracks+60, copies
    //      48 * (int16)tracks+50 bytes from tracks + (int16)tracks+52, and
    //      then reads worldFromModel from tracks + (int16)tracks+20.
    //   2. The shipped Havok class metadata for
    //      hkbPoseStoringGeneratorOutputListenerStoredPose, which independently
    //      says pose is an ARRAY<QSTRANSFORM> at +24 (size +32, capacity +36),
    //      worldFromModel a QSTRANSFORM at +48, isPoseValid at +96 -- exactly
    //      the destinations that decompilation writes to.
    //
    // tracks+50 / +52 / +60 decode as TrackHeader[2] at tracks+0x10+2*0x10,
    // i.e. headers start at +0x10 with a 16-byte stride, numData at +2,
    // dataOffset at +4, flags at +12. Track 2 is the pose (48-byte
    // hkQsTransform each); track 0 is worldFromModel.
    // CORRECTED 2026-08-24 pt.2. The first attempt reached the generator
    // output through hkbCharacter (graph+0x1C8, then +0x48) because the Havok
    // class metadata puts generatorOutput there. Live, that read produced a
    // heap pointer whose first qword was an IMAGE address -- a vtable, not the
    // heap-allocated tracks the layout predicts -- and numTracks came back as
    // 32759. Right structure, wrong route to it.
    //
    // BShkbAnimationGraph::GraphPoseApply (the symbol map's "real bone
    // writer", RVA 0x1321030) shows the route the engine itself uses:
    //
    //   sub_1413998F0(
    //       **(_QWORD **)(a1 + 504) + *(__int16 *)(**(_QWORD **)(a1 + 504) + 52),
    //       a1 + 672,                          // NiAVObject* array  == +0x2A0
    //       *(unsigned __int16 *)(a1 + 960),   // bone count         == +0x3C0
    //       a1 + 864);
    //
    // a1+672 and a1+960 are GRAPH_NODE_ARRAY_OFF and GRAPH_BONE_COUNT_OFF,
    // both already in this file from earlier independent work -- which is a
    // strong check that this decompilation is the function it claims to be.
    //
    //   genOut = *(void**)(graph + 0x1F8)         (hkbGeneratorOutput*)
    //   tracks = *(void**)genOut                   (first member, as before)
    //   pose   = tracks + *(int16*)(tracks + 52)   (TrackHeader[2].dataOffset)
    //   count  = *(uint16*)(graph + 0x3C0)
    //
    // tracks+52 decodes as headers at +0x10, stride 16, dataOffset at +4 for
    // track 2 -- the same arithmetic hkbPoseStoringGeneratorOutputListener
    // uses, so the header layout below was right all along; only the pointer
    // that reaches `tracks` was wrong.
    //
    // GraphPoseApply has two branches: when graph+966 ("update due") is set it
    // works through graph+912 instead. Both are tried here, +0x1F8 first.
    constexpr size_t GRAPH_GENOUT_PRIMARY_OFF  = 0x1F8; // direct-node-writer path
    constexpr size_t GRAPH_GENOUT_FALLBACK_OFF = 0x390; // 'update due' path
    constexpr size_t GENOUT_TRACKS_OFF            = 0x00; // hkbGeneratorOutput::m_tracks
    constexpr size_t TRACKS_NUMTRACKS_OFF         = 0x04;
    constexpr size_t TRACKS_HEADERS_OFF           = 0x10;
    constexpr size_t TRACK_HEADER_STRIDE          = 0x10;
    constexpr size_t TRACKHDR_NUMDATA_OFF         = 0x02;
    constexpr size_t TRACKHDR_DATAOFF_OFF         = 0x04;
    constexpr size_t TRACKHDR_FLAGS_OFF           = 0x0C;
    constexpr int    TRACK_POSE                   = 2;
    constexpr size_t QSTRANSFORM_SIZE             = 48;   // trans16 + rot16 + scale16
    constexpr size_t QSTRANSFORM_ROT_OFF          = 16;

    // Animation-skeleton bone indices, from the shipped rig. Identical in the
    // first- and third-person skeletons (both are the same rig for indices
    // 0..93; third person only appends CamTarget at 94), so one index means
    // the same bone in either graph's pose buffer.
    constexpr int BONE_RLEG_THIGH   = 6;
    constexpr int BONE_LARM_UPPERARM = 15;
    constexpr size_t HKB_NODES_DIRTY_OFF      = 0x1AC; // read straight off 0x0147F232
    constexpr size_t PHYS_HOLDER_WORLD_OFF    = 0x30;  // holder -> hkpWorld

    // ── 2026-08-21: locomotion hand-off, live 1P graph -> parked 3P graph ───
    //
    // THE first-person defect, after three wrong suspects were eliminated by
    // measurement this session:
    //   - the HKB active-node list is NOT stale (the refresh above fired ZERO
    //     times on both machines: the dirty byte is never set on a parked
    //     graph, and updateActiveNodes gates internally on +0x1AD as well);
    //   - dt reaches the graph fine (BShkbAnimationGraph::Update does
    //     `movss xmm9,[rdx]`, reading it from exactly where the drive writes
    //     it, so the clip clock has real time);
    //   - PC_SKELETON_SYNC is not overwriting us (it fired ONCE per session,
    //     not per frame -- one "allowed through (#0)" across ~7900 frames).
    //
    // The graph is ticking, with correct time, writing all 95 bones -- and
    // playing a STANDING IDLE, because nothing ever tells it the player is
    // moving. activeNodes pinned at 57 is a state machine sitting in one
    // state, not a stale list; RLeg_Thigh byte-identical across 803 samples
    // is a standing idle's legs, which genuinely do not move. The arms looked
    // alive only because procedural aim/look modifiers still track the camera.
    //
    // The parked graph needs its two locomotion scalars. They must come from
    // the ENGINE, not from a positional delta: FO4_Wrld tried deriving them
    // from frame-to-frame displacement and disabled it 2026-08-06 because
    // this drive only runs while the camera is in first person, so the
    // displacement spans gaps dt does not account for (their live numbers:
    // one client 5953 where 100-200 was expected, another a constant 0).
    // Their note names the fix they never got to -- "the clean source is the
    // engine's own movement speed rather than a positional delta". That
    // source is the LIVE first-person graph's own variable slots, which the
    // engine updates every frame and which CoSyncAnimVarSystem already reads
    // for exactly this reason (kFPVar_Speed / kFPVar_Direction).
    //
    // WHY THIS IS NOT THE FORBIDDEN GRAPH-VARIABLE WRITE: the standing rule
    // is against the HOLDER-level setters (SetGraphVariable*), which dispatch
    // through actor+0x48 and broadcast to EVERY graph the actor owns -- that
    // is what mutated the sender's own live first-person animation, i.e. real
    // gameplay state, not just replication data. The setter used here is
    // graph-scoped: its own first instruction resolves the target through
    // `[graph+0x378]`, THAT graph's wrapper. It writes the parked
    // third-person graph and nothing else. The live 1P graph the player is
    // actually looking through is never touched, and the parked graph's only
    // consumer is CaptureBones. Confirmed by disassembly, not by name.
    //
    // Kill-switch: set kDriveLocomotionVars=false to revert to the exact
    // pre-2026-08-21 behaviour (parked graph ticks, plays a standing idle).
    // 2026-08-22 -- RE-ENABLED, now on proof rather than theory.
    //
    // The post-generate leg probe finally ran (174 samples) and read the leg
    // bone straight out of the skeleton immediately after generate+apply. It
    // returns the SAME frozen matrix the capture-time witness sees:
    //   [leg-probe] RLeg_Thigh=[-0.987 -0.099 -0.126 | -0.058 0.953 -0.296 | ...]
    //   [witness]   RLeg_Thigh 1007/1148 identical
    // so nothing is overwriting the legs after the graph runs -- the graph
    // GENERATES a static leg, while active (tpActivated=1), driven
    // (driveCount 5036), transitioning (20 MOVED events, activeNodes 39..59)
    // and receiving correct dt. Arms move (61% distinct) because they come
    // from procedural aim/look layers, not from locomotion clips.
    //
    // The unpacked behaviour asset says why: in Locomotion_8wayBlend.hkb,
    // EVERY locomotion clip's playbackSpeed is bound to
    // fLocomotionWalkPlaybackSpeed / fLocomotionRunPlaybackSpeed. At 0 the
    // clip generator holds its local time -- the pose is re-written to the
    // skeleton every frame and never advances. That is this exact signature,
    // and it is the same root cause behind FO4_Wrld's own documented residue
    // ("the behaviour scales its locomotion clips from variables that nothing
    // writes into a parked graph"); theirs read stale rather than zero, so
    // they saw a wrong rate where CoSync sees no motion.
    //
    // Scope is unchanged and still inside the project's rule: the setters used
    // are graph-scoped (they resolve the target through [graph+0x378]), so
    // only the parked third-person graph is touched and the live first-person
    // graph driving the player's own view is not.
    // OFF with the drive above -- these only ever fed the parked graph.
    constexpr bool kDriveLocomotionVars = false;
    constexpr uintptr_t RVA_GRAPH_SET_VAR_FLOAT = 0x01326980; // sub_141326980; 1.11.191 0x01326590 +0x3F0
                                                              // (graph, BSFixedString*, float) -> bool

    // Live 1P graph variable slots -- same indices CoSyncAnimVarSystem's
    // PatchLocalPlayerSpeedFromFP already reads, verified there against the
    // graph's own variable-name table.
    // 2026-08-21 pt.2 -- READ SLOT 90, NOT SLOT 2.
    //
    // Slot 2 ("Speed") is the damped/instantaneous scalar and it reads 0.000
    // while the player is genuinely moving. Caught red-handed in the same
    // frame by AnimVarSystem's own FP diagnostic on a host that was running:
    //
    //   Speed[2]=0.000  Direction[3]=0.500  SpeedSmoothed[90]=264.944
    //   IsFirstPerson[10]=1
    //
    // That is the "speedDamped case trap" already recorded for this graph's
    // index map. Reading slot 2 alone made the hand-off flicker between 0.00
    // and 350.87 across consecutive samples, so the parked graph was told
    // "idle, idle, run, run" and flickered between states instead of settling
    // into a walk cycle -- which is why the first attempt moved activeNodes
    // (proof the write lands) without producing usable animation.
    //
    // Slot 90 is the live value. Slot 2 is kept only as a fallback for the
    // case where 90 is the dormant one; whichever has real magnitude wins.
    constexpr uint32_t kFPVar_Speed         = 2;
    constexpr uint32_t kFPVar_SpeedSmoothed = 90;
    constexpr uint32_t kFPVar_Direction     = 3;
    constexpr size_t   HKB_ANIM_VARS_OFF    = 0x110; // hkbBehaviorGraph::animationVariables
    constexpr size_t   VVS_DATA_OFF         = 0x10;  // hkbVariableValueSet::data
    constexpr size_t   VVS_SIZE_OFF         = 0x18;  // hkbVariableValueSet::size

    // Written by NAME into the parked graph. "SpeedSampled" is what FO4_Wrld
    // identified as the scalar the locomotion clips actually scale from;
    // "Speed" is written too because CoSync's own index map for the
    // third-person behaviour (RaiderRootBehavior.hkx) lists Speed, not
    // SpeedSampled. The setter returns false for a name the graph does not
    // have, so writing both costs one failed lookup and the first-run log
    // below reports which one actually landed.
    // ── 2026-08-21 pt.6: what the locomotion blend ACTUALLY reads ───────────
    //
    // Established by unpacking the real behaviour assets with
    // BehaviourGraphStudio's hkxpack (Actors\Character\Behaviors\*.hkx ->
    // XML), instead of inferring from symptoms. Locomotion_8wayBlend.hkb --
    // the graph that owns the walk cycle -- declares exactly 9 variables, and
    // its hkbVariableBindingSet entries say what each one drives:
    //
    //     startStateId    <- iLocomotionSpeedState     (picks Walk/Run/WalkRun)
    //     blendParameter  <- Speed                     (blends WITHIN a state)
    //     fBlendParameter <- Direction                 (8-way axis)
    //     playbackSpeed   <- fLocomotionWalkPlaybackSpeed   (on EVERY walk clip)
    //     playbackSpeed   <- fLocomotionRunPlaybackSpeed    (on EVERY run clip)
    //     startStateId    <- iSyncRunDirection
    //
    // Two things follow, and both explain the measurements:
    //
    // 1. Speed is ONLY a blend parameter. It cannot move a state machine, so
    //    writing it alone -- which is what pt.1 did -- can never make a parked
    //    graph walk. That is why the write landed (Speed=ok) and nothing
    //    animated.
    //
    // 2. Every locomotion CLIP takes its playbackSpeed from a variable. On the
    //    live graph the engine recomputes those each frame; on a PARKED graph
    //    nothing does, so they sit at whatever they were -- and a clip whose
    //    playbackSpeed is 0 is written to the skeleton every frame while its
    //    local clock never advances. That is exactly the observed signature:
    //    RAN=YES, boneCount=95, pose applied, and RLeg_Thigh byte-identical
    //    for 332 of 409 first-person samples, while the arms (procedural
    //    aim/look nodes, not clip-driven) kept moving.
    //
    // Variable indices below are read straight out of RaiderRootBehavior.hkx's
    // own variableNames table -- the same table that independently confirms
    // this project's existing map (Direction=2, Speed=54).
    constexpr uint32_t kTPVar_Direction        = 2;
    constexpr uint32_t kTPVar_Speed            = 54;
    constexpr uint32_t kTPVar_WalkPlayback     = 233; // fLocomotionWalkPlaybackSpeed
    constexpr uint32_t kTPVar_JogPlayback      = 234; // fLocomotionJogPlaybackSpeed
    constexpr uint32_t kTPVar_RunPlayback      = 235; // fLocomotionRunPlaybackSpeed
    constexpr uint32_t kTPVar_LocoSpeedState   = 238; // iLocomotionSpeedState

    // Graph-scoped setters, same family as the float one -- all three resolve
    // the target through [graph+0x378], that graph's own wrapper, so none of
    // them can reach the live first-person graph.
    //   0x013268A0  movzx r14d, r8b   -> bool
    //   0x01326980  movaps xmm6, xmm2 -> float
    //   0x01326A60  mov   r14d, r8d   -> int
    constexpr uintptr_t RVA_GRAPH_SET_VAR_INT = 0x01326A60;

    // ── 2026-08-21 pt.7: KEEP THE ANIMATION SYSTEM IN THIRD PERSON ─────────
    //
    // The camera still switches to first person; the ANIMATION GRAPH does not.
    // BSAnimationGraphManager::SetActiveGraph is refused when it tries to move
    // the player's manager to the first-person graph, so activeGraph (mgr+0xD8)
    // stays on the third-person one and the engine's own per-frame driver
    // keeps updating it in full -- real locomotion variables, real clip
    // playback speeds, real active-node upkeep, real events, real dt.
    //
    // That is the entire point: every failure this session came from the
    // third-person graph being PARKED while the camera was in first person,
    // and from us trying to reproduce by hand what the engine does for the
    // active graph. Nothing is reproduced by hand here -- the graph simply
    // never stops being the active one, so CaptureBones reads a genuinely
    // engine-animated skeleton in both camera modes.
    //
    // Scope: player's manager only (the two-graph one), and only while
    // bDeltaPositionLocomotion=2 and connected. Solo play and every other mode
    // are untouched.
    //
    // KNOWN COST, to confirm live: the first-person arm rig is driven by the
    // first-person graph, so with that graph never activated the local
    // player's own viewmodel arms may hold a static pose. Whether they freeze,
    // fall back to the third-person body, or are simply not drawn is a
    // question for the test, not the disassembly. Flip this to false to
    // restore vanilla graph switching.
    // 2026-08-21 pt.9 -- OFF. Measured live: pinning the selector froze the
    // WHOLE skeleton in first person (488/488 witness samples byte-identical,
    // arms included), because entering first person also DEACTIVATES the
    // third-person graph via a separate path and every per-graph entry point
    // early-outs on hkb+0x1AA. Worse, it starves the first-person graph, so
    // the local player's own viewmodel stops animating -- which is why
    // FO4_Wrld deliberately let the switch happen and drove the parked graph
    // instead. Kept only as a documented dead end.
    constexpr bool kForceThirdPersonGraph = false;

    // THE missing ingredient. The third-person graph pointer is cached every
    // frame while it is still selected, so its deactivation can be suppressed
    // at the moment the camera goes first person -- BEFORE the drive has run
    // even once. CoSync's existing guard keyed on g_drivenGraph, which the
    // drive only sets after it has run, so the very first deactivate always
    // got through: graph reset to its initial state, drive revives it, state
    // machine back at the start, legs idle. Suppressing it means the graph
    // carries its live walking state straight through the switch.
    //
    // The trip BACK to third person is deliberately NOT suppressed (the
    // existing g_allowDeactivate path hands it over deactivated) so the
    // engine's own activation runs in full, including the ragdoll bind.
    constexpr bool kPinTpGraphAcrossSwitch = true;

    // ── 2026-08-23: MATCH THE REFERENCE ACTIVATION EXACTLY ─────────────────
    //
    // CoSync's activation path calls two functions the working reference
    // implementation does not: GraphActivatePrep (sub_14052A800) before
    // GraphActivate, and MgrActivateCommit (sub_1413AA730) after. They were
    // added on a theory that SetActiveGraph's full sequence was needed for a
    // "complete" activation. The reference calls GraphActivate ALONE and its
    // legs animate; ours calls all three and they do not.
    //
    // That matters because of something the reference states explicitly about
    // this exact path: reactivation "rebuilds the active-node list from the
    // root, i.e. RESETS THE STATE MACHINE -- measured live as a drop from 59
    // active nodes to 19." A more thorough activation is not obviously better
    // here; it is more reset. After matching every other step and still
    // getting a static pose, the remaining honest move is to stop diverging
    // from the one implementation known to work and re-add differences only
    // with evidence.
    //
    // true  = GraphActivate alone (reference behaviour)
    // false = the prep/commit sandwich this project added
    constexpr bool kExactReferenceActivation = true;

    // ── 2026-08-23: UNCULL THE THIRD-PERSON BODY WHILE DRIVING ─────────────
    //
    // The last MEASURED difference between the state that works and the state
    // that does not. Everything else tried this session was a theory; this is
    // a number: rootCulled=1 in 17/17 first-person samples, and the body is
    // not culled in third person where replication works.
    //
    // NiAVObject+0x108 bit 0 is APP_CULLED. The forced update context already
    // beats the LOD throttle that reads it (boneCount went 0 -> 95 once the
    // drive was on), so the throttle is not the remaining problem -- but the
    // culled bit is consulted by more than the throttle, and the engine's own
    // animation work for a hidden body may legitimately skip producing
    // anything regardless of what the context asks for.
    //
    // Cleared on the ROOT only, only while the drive is running, and restored
    // immediately after the pose is generated so the body is never visible for
    // a rendered frame. If the local player briefly sees their own body in
    // first person, this is why -- set to false to revert.
    //
    // 2026-08-24 -- OFF (Experiment A). The reference implementation does not
    // uncull; the forced context (null camera + ctx[0x1A]=1) already takes the
    // full-generate branch regardless of the culled bit (disassembly-verified:
    // GraphUpdate branches on ctx+0x08==null and ctx[0x1A], not on the NiNode
    // cull flag). Toggling the cull bit mid-generate is an unproven divergence;
    // remove it so the test isolates the ClearParkedGraphFreezeFlags change.
    constexpr bool kUncullBodyWhileDriving = false;

    // ── 2026-08-23: LET THE ENGINE TICK THE PARKED GRAPH ────────────────────
    //
    // The variable-diff run settled what the parked graph is missing, and it
    // is not data. Measured on the same graph in both camera modes:
    //
    //   inFP=0  Speed=350.87 SpdSmooth=350.87 idleLoco=1 locoState=1 walkPb=1.44
    //   inFP=1  Speed=235.71 SpdSmooth=235.71 idleLoco=1 locoState=1 walkPb=1.47
    //
    // The engine populates the third-person graph's locomotion variables
    // identically while the camera is in first person -- every value the six
    // previous attempts wrote by hand was already there. Same run: muteByte=0
    // throughout (graph+0x3C3 is clear) and tpActivated=1 in 26 of 28
    // first-person samples, so the graph is activated and unmuted too.
    //
    // The ONLY thing it lacks is a tick: BSAnimationGraphManager::Update runs
    // graphs[mgr+0xD8] and nothing else. The hand-rolled drive did tick it,
    // with those same correct variables, and still produced a static lower
    // body -- so our reproduction of the engine's tick differs from the real
    // one in some way that reading the instructions has not revealed.
    //
    // So stop reproducing it. Point the manager's active index at the
    // third-person graph and call the engine's OWN Update, then restore the
    // index. The engine then ticks that graph with its own context, its own
    // LOD handling, its own node-list upkeep and its own root-motion path --
    // nothing hand-rolled anywhere.
    //
    // Ordering is deliberate: the NORMAL update runs FIRST so the
    // first-person graph the player is actually looking through keeps
    // consuming its events exactly as vanilla, and the extra third-person
    // tick runs afterwards on whatever remains. The reverse order would let
    // our extra tick eat the player's own animation events.
    //
    // KNOWN RISK, the reason this is behind its own flag: the manager's
    // per-frame bookkeeping runs twice for the player. If footsteps, weapon
    // fire or other event-driven effects start double-firing, this is why.
    // OFF while we test the exact-reference drive -- running both would make
    // the result unreadable, which is the mistake that cost several sessions.
    //
    // 2026-08-24 -- ON (Experiment B). The manual drive is now OFF
    // (kAttemptFirstPersonDrive=false), so ONLY this runs and the result is
    // readable. This is the whole point of Exp B: the engine's own MGR_UPDATE
    // is the one thing that provably advances the parked graph's per-node
    // clocks (it does so in 3P every frame), and the hand-rolled drive
    // demonstrably cannot on 1.11.240. Companion wiring: this path now bumps
    // g_engineTickCount and sets g_forcedRevive so (a) ShouldCaptureNow's
    // IsDriving() gate still fires in FP and (b) the PC_SKELETON_SYNC 1P->3P
    // overwrite is suppressed while ticking, and (c) the return-to-3P undo
    // hands graph 0 back deactivated so the engine's activation runs in full.
    constexpr bool kEngineTickParkedGraph = true;

    // LocomotionRoot's own state ids, from the same unpacked graph:
    //   0 = MeleeWalkState, 1 = MeleeRunState, 2 = MeleeWalkRunState
    constexpr int kLocoState_Walk = 0;
    constexpr int kLocoState_Run  = 1;

    const char* const kLocoVarNames[] = {
        "SpeedSampled", "Speed", "Direction",
        "fLocomotionWalkPlaybackSpeed",
        "fLocomotionJogPlaybackSpeed",
        "fLocomotionRunPlaybackSpeed",
    };
    BSFixedString g_locoVarStr[6];
    bool g_locoVarsReady = false;

    // 2026-08-22 -- the integer channel, from the unpacked MTBehavior.hkb
    // binding table rather than from guesswork:
    //
    //   startStateId           <- iSyncIdleLocomotion   x11
    //   selectedGeneratorIndex <- iSyncIdleLocomotion   x1
    //   boneWeights            <- iSyncIdleLocomotion   x1
    //   startStateId           <- iSyncSprintState      x5
    //   startStateId           <- iSyncLocomotionSpeed  x1
    //
    // iSyncIdleLocomotion is THE idle-vs-locomotion switch: eleven separate
    // state machines take their start state from it, and it additionally
    // drives a blend's boneWeights -- which is how a graph ends up animating
    // the upper body while the locomotion bones stay masked out. That is
    // precisely the measured signature (arms 76% distinct, legs 74% pinned to
    // one pose), and none of the previous rounds wrote it.
    //
    // Left at 0 the whole locomotion half of the behaviour never starts, no
    // matter how correct Speed, Direction, the playback speeds or the clip
    // clock are -- all of which this session confirmed were already fine
    // (playbackSpeed read 0.503/0.736 before we touched it, dt 0.008..0.024,
    // readback Speed=149.42).
    //
    // Written by NAME, so a variable the graph does not have simply reports
    // NO-VAR instead of misfiring.
    const char* const kLocoIntNames[] = {
        "iSyncIdleLocomotion",     // 0 = idle, 1 = locomotion
        "iLocomotionSpeedState",   // LocomotionRoot start state: 0 Walk, 1 Run
        "iSyncWalkRun",
        "iSyncLocomotionSpeed",
        "iSyncSprintState",
    };
    BSFixedString g_locoIntStr[5];
    constexpr size_t GRAPH_UPDATE_DUE_SENTINEL_OFF = 0x3A8; // Update's last write in the gated block
    // Diagnostics-only fields (which branch GraphPoseApply takes depends on
    // these; per its own decompile, the DIRECT NiNode bone-write only
    // happens when GRAPH_UPDATE_DUE_BYTE_OFF is CLEAR and the physics
    // predicate is false -- otherwise it dispatches through a different,
    // non-bone-writing path).
    constexpr size_t GRAPH_UPDATE_DUE_BYTE_OFF = 0x3C6; // graph+966
    constexpr size_t GRAPH_GEN_DUE_BYTE_OFF    = 0x3C5; // graph+965
    constexpr size_t GRAPH_BONE_COUNT_OFF      = 0x3C0; // graph+960, uint16
    // CORRECTED 2026-08-19 (twice): the field previously logged as "root
    // node" was never a NiNode pointer at all. Decompiled the actual bone
    // writer `sub_1413998F0(a1, a2, a3)` directly (3 args, not 4 -- an
    // earlier transcription of the call site had a stray extra arg) and it
    // shows: `a1` is a flat array of computed quaternion+scale pose data
    // (source, 48 bytes/bone stride) -- comparing it to any NiNode* was
    // always comparing apples to oranges, regardless of the pointer-chase
    // bug fixed earlier. The REAL destination is `a2`: an array of
    // NiAVObject* pointers (`v7 = *(QWORD*)(*(QWORD*)a2 + 16*i)`), each
    // written at `v7+48` -- i.e. node+0x30, the SAME local-rotation offset
    // CoSyncPlayerAnimation.cpp's own SafeReadRotation/SafeWriteRotation
    // already use. This array lives at graph+672 (0x2A0) -- cross-confirmed
    // by FO4_Wrld's own probe comment ("arrData(0x2A0)") independently
    // landing on the identical offset. This is the pointer that actually
    // matters: does entry 0 of THIS array resolve to a real, named bone on
    // the actor's actual skeleton?
    constexpr size_t GRAPH_NODE_ARRAY_OFF = 0x2A0; // graph+672, NiAVObject* array (the writer's real a2)
    constexpr size_t GRAPH_ACTIVATE_PREP_ARG_OFF = 0x68; // graph+104, first arg to the prep call
    constexpr size_t MGR_ACTIVATE_COMMIT_ARG_OFF = 0x88; // mgr+136, first arg to the commit call

    constexpr size_t ACTOR_HOLDER_OFF = 0x48; // IAnimationGraphManagerHolder @ actor+0x48

    constexpr uint32_t THIRD_PERSON_GRAPH = 0;
    constexpr uint32_t FIRST_PERSON_GRAPH = 1;

    // ------------------------------------------------------------------
    // Function pointer types
    // ------------------------------------------------------------------
    using MgrUpdateFn       = void    (__fastcall*)(void* mgr, void* updateData);
    using MgrNotifyFn       = uint8_t (__fastcall*)(void* mgr, const void* event);
    using SetActiveGraphFn  = uint8_t (__fastcall*)(void* mgr, uint32_t newIndex);
    using GraphEventFlushFn = void    (__fastcall*)(void* graph, void* eventArrayPtr);
    // REVERTED 2026-08-19: briefly "fixed" this to 3 args (graph, ctx, dt)
    // reading FO4_Wrld's own comment as describing THIS function's calling
    // convention. Re-checked their actual call site (first_person_graph.cpp
    // line 657): `g_graph_update(tp_graph, ctx)` -- exactly 2 args, same as
    // here. Their "xmm2 third argument" comment describes an INTERNAL call
    // GraphUpdate itself makes to a nested hkbBehaviorGraph::update -- it
    // reads dt from ITS OWN ctx+0x00 (which we already set correctly) and
    // forwards it onward internally; outside callers never touch xmm2. The
    // frozen-pose symptom this was meant to explain has a different cause.
    using GraphUpdateFn     = void    (__fastcall*)(void* graph, void* updateData);
    using GraphPoseApplyFn  = void    (__fastcall*)(void* graph, void* scratch);
    using GraphActivateFn   = void    (__fastcall*)(void* graph);
    using GraphDeactivateFn = uint8_t (__fastcall*)(void* graph, void* scrapOut);
    using GraphSendEventFn  = uint8_t (__fastcall*)(void* graph, const void* event, void* sinkOrNull);
    using PopulateVarsFn    = void    (__fastcall*)(void* holder, void** graphPtrPtr);
    // TWO args, not one -- confirmed 2026-08-19 via live-crash root-cause
    // (raw disasm of 0x140D7DE10): `mov rbx, rdx / mov rdi, rcx / call
    // sub_140CA9880` reads BOTH incoming registers before its first call.
    // a2 is a pointer-to-graph-manager-pointer (`v5 = *a2`); the callee
    // dereferences it unconditionally (`if (*a2 && ...)`). A single-arg
    // detour that only forwards a1 leaves RDX holding whatever this
    // function's own locals last put there when calling through to the
    // original -- a guaranteed null/garbage deref the first time the
    // original actually runs (i.e. the very first un-suppressed call).
    using PcSkeletonSyncFn  = void    (__fastcall*)(void* a1, void* a2);
    using GraphPhysPredFn   = uint8_t (__fastcall*)(void* graph);
    // Signatures read directly off SetActiveGraph's own disassembly (not
    // guessed): both take exactly 2 pointer args, return value unused by
    // the caller in the real engine flow either.
    using GraphActivatePrepFn = void (__fastcall*)(void* graphPlus104, void* mgr);
    using MgrActivateCommitFn = void (__fastcall*)(void* mgrPlus136, void* graphPtrSlot);

    MgrUpdateFn       g_origMgrUpdate      = nullptr;
    MgrNotifyFn       g_origMgrNotify      = nullptr;
    SetActiveGraphFn  g_origSetActiveGraph = nullptr;
    GraphDeactivateFn g_origGraphDeactivate = nullptr;
    PcSkeletonSyncFn  g_origSkeletonSync   = nullptr;

    GraphEventFlushFn g_graphEventFlush = nullptr;
    GraphUpdateFn     g_graphUpdate     = nullptr;
    GraphPoseApplyFn  g_graphPoseApply  = nullptr;

    using HkbCtxCtorFn   = void* (__fastcall*)(void* ctx, void* character, void* world, void* arg4);
    using HkbCtxDtorFn   = void  (__fastcall*)(void* ctx);
    using HkbUpdActiveFn = void  (__fastcall*)(void* hkb, void* ctx, uint32_t flags);
    using HkbUpdSyncFn   = void  (__fastcall*)(void* hkb, void* ctx);

    HkbCtxCtorFn   g_hkbCtxCtor   = nullptr;
    HkbCtxDtorFn   g_hkbCtxDtor   = nullptr;
    HkbUpdActiveFn g_hkbUpdActive = nullptr;
    HkbUpdSyncFn   g_hkbUpdSync   = nullptr;

    // Graph-scoped, unlike the holder-level setters. See kDriveLocomotionVars.
    using GraphSetVarFloatFn = uint8_t (__fastcall*)(void* graph, BSFixedString* name, float value);
    using GraphSetVarIntFn   = uint8_t (__fastcall*)(void* graph, BSFixedString* name, int   value);
    GraphSetVarFloatFn g_graphSetVarFloat = nullptr;
    GraphSetVarIntFn   g_graphSetVarInt   = nullptr;

    std::atomic<uint64_t> g_nodeRefreshCount{0};
    std::atomic<uint64_t> g_locoWriteCount{0};
    std::atomic<uint64_t> g_eventsMirrored{0};
    std::atomic<uint64_t> g_mirrorSkipped{0};
    std::atomic<uint64_t> g_fpSwitchBlocked{0};
    // The player's third-person graph, cached while it is still the active one
    // so the deactivate hook can recognise it later (that hook only receives a
    // graph pointer, with no manager to look it up from).
    std::atomic<void*>    g_pinnedTpGraph{nullptr};
    std::atomic<uint64_t> g_deactBlocked{0};

    // Reads one float slot out of a graph's own hkbVariableValueSet.
    // noinline + SEH per this file's established convention -- the 1P graph
    // can be mid-rebuild, and every pointer in the chain is engine-owned.
    __declspec(noinline) bool ReadGraphVarFloat(void* graph, uint32_t index, float& out) noexcept
    {
        __try
        {
            auto* wrapper = *reinterpret_cast<uint8_t**>(
                reinterpret_cast<uint8_t*>(graph) + GRAPH_HKB_WRAPPER_OFF);
            if (!wrapper) return false;
            auto* vvs = *reinterpret_cast<uint8_t**>(wrapper + HKB_ANIM_VARS_OFF);
            if (!vvs) return false;
            auto* data = *reinterpret_cast<uint32_t**>(vvs + VVS_DATA_OFF);
            const uint32_t size = *reinterpret_cast<uint32_t*>(vvs + VVS_SIZE_OFF);
            if (!data || index >= size) return false;
            out = *reinterpret_cast<const float*>(&data[index]);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    GraphActivateFn   g_graphActivate   = nullptr;
    GraphSendEventFn  g_graphSendEvent  = nullptr;
    PopulateVarsFn    g_populateVars    = nullptr;
    GraphActivatePrepFn g_graphActivatePrep = nullptr;
    MgrActivateCommitFn g_mgrActivateCommit = nullptr;
    GraphPhysPredFn   g_graphPhysPred   = nullptr; // diagnostics only -- the engine dispatches this internally

    // The wake-up events that move a freshly-revived graph's Havok state
    // machine out of its initial (T-pose) state, per FO4_Wrld's own live
    // measurement (logging active-node count around every mirrored event
    // showed these are the two that actually move it).
    const char* const kWakeEvents[] = { "g_archetypeBaseStateStartInstant", "MoveStop" };
    BSFixedString g_wakeEventStr[2];
    bool g_wakeEventsReady = false;

    std::atomic<bool>     g_enabled{true};
    std::atomic<bool>     g_installed{false};
    std::atomic<uint64_t> g_driveCount{0};
    std::atomic<uint64_t> g_sehCount{0};
    // Experiment B (2026-08-24): counts frames the ENGINE re-ticked the parked
    // graph (kEngineTickParkedGraph). Read by IsDriving()/PC_SKELETON_SYNC
    // suppression so the capture gate and overwrite-suppression work with the
    // hand-rolled drive turned off.
    std::atomic<uint64_t> g_engineTickCount{0};

    // Identity guard for the driven graph -- only ever compared, never
    // dereferenced outside a live drive call, so a stale pointer after the
    // graph goes away is harmless (matches FO4_Wrld's own round-8 note).
    void*             g_drivenGraph = nullptr;
    std::atomic<bool> g_forcedRevive{false};
    std::atomic<bool> g_allowDeactivate{false};

    // ------------------------------------------------------------------
    // The engine's own recursive spinlock idiom (BSAnimationGraphManager
    // +0xC8 owner tid / +0xCC count) -- transcribed from the decompiled
    // Update/GraphEventFlush/GraphPoseApply prologues, all of which open
    // with this exact sequence.
    // ------------------------------------------------------------------
    void MgrLock(uint8_t* mgr) noexcept
    {
        auto* owner = reinterpret_cast<volatile long*>(mgr + MGR_LOCK_OWNER_OFF);
        auto* count = reinterpret_cast<volatile long*>(mgr + MGR_LOCK_COUNT_OFF);
        const DWORD tid = GetCurrentThreadId();
        if (static_cast<DWORD>(*owner) == tid)
        {
            _InterlockedIncrement(count);
            return;
        }
        unsigned spins = 0;
        while (_InterlockedCompareExchange(count, 1, 0))
        {
            Sleep(spins >= 0x2710 ? 1 : 0);
            if (spins < 0x2710) ++spins;
        }
        *owner = static_cast<long>(tid);
        MemoryBarrier();
    }

    void MgrUnlock(uint8_t* mgr) noexcept
    {
        auto* owner = reinterpret_cast<volatile long*>(mgr + MGR_LOCK_OWNER_OFF);
        auto* count = reinterpret_cast<volatile long*>(mgr + MGR_LOCK_COUNT_OFF);
        if (*count == 1)
        {
            *owner = 0;
            MemoryBarrier();
            _InterlockedCompareExchange(count, 0, 1);
        }
        else
        {
            _InterlockedDecrement(count);
        }
    }

    // Resolve graphs[idx] from the BSTSmallArray at mgr+0x40/+0x48, exactly
    // the way the engine itself does (confirmed against every decompiled
    // caller): `data = mgr+0x48; if ((int32)flags >= 0) data = *data;`
    void* GraphAt(uint8_t* mgr, uint32_t idx) noexcept
    {
        const auto flags = *reinterpret_cast<const int32_t*>(mgr + MGR_ARRAY_FLAGS_OFF);
        auto** data = reinterpret_cast<void**>(mgr + MGR_ARRAY_DATA_OFF);
        if (flags >= 0) data = *reinterpret_cast<void***>(data);
        return data[idx];
    }

    void* GetHkbWrapper(uint8_t* graph) noexcept
    {
        return *reinterpret_cast<void**>(graph + GRAPH_HKB_WRAPPER_OFF);
    }

    bool IsGraphActive(uint8_t* graph) noexcept
    {
        void* hkb = GetHkbWrapper(graph);
        if (!hkb) return false;
        return *(reinterpret_cast<uint8_t*>(hkb) + HKB_ACTIVE_BYTE_OFF) != 0;
    }

    // Read one graph's computed pose. Returns false if anything about the
    // chain looks wrong -- this runs inside the engine's own update, so it is
    // SEH-caged and noinline like every other raw reader in this file.
    //
    // LIFETIME: generatorOutput is only meaningful during/just after the
    // graph's update. Call this from inside the MgrUpdate hook and nowhere
    // else; do not cache the pointer across frames.
    // [Animations] bEngineTickParkedGraph in CoSync.ini, default 1.
    //
    // Exists so the parked-graph re-tick can be turned off WITHOUT also
    // changing the capture source. Those two were entangled through
    // IsFirstPersonCaptureEnabled(), which meant the pose-buffer probe ran
    // with graph0 being force-ticked by us -- and graph0's legs duly moved,
    // which is our own doing, not the engine's. graph1 is driven by the
    // engine's normal update and should be unaffected either way, but that is
    // an assumption until it is measured with this off.
    bool IsEngineTickEnabled() noexcept
    {
        // Minimal preset (bDeltaPositionLocomotion=3) bundles the re-tick on --
        // no companion key needed.
        if (CoSyncPlayerAnimation::IsMinimalMode()) return true;
        static int s_cached = -1;
        if (s_cached >= 0) return s_cached != 0;

        char exePath[MAX_PATH] = {};
        if (::GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) { s_cached = 1; return true; }
        std::string ini(exePath);
        const size_t slash = ini.find_last_of("\\/");
        ini = (slash == std::string::npos) ? std::string() : ini.substr(0, slash + 1);
        ini += "Data\\F4SE\\Plugins\\CoSync.ini";
        s_cached = ::GetPrivateProfileIntA("Animations", "bEngineTickParkedGraph", 1, ini.c_str());
        LOG_INFO("[FPGraph] bEngineTickParkedGraph=%d -> parked-graph re-tick %s",
            s_cached, s_cached ? "ARMED" : "off");
        return s_cached != 0;
    }

    bool IsValidUserPtr(const void* p) noexcept; // defined just below GetHolder

    struct GraphPose
    {
        const uint8_t* data  = nullptr; // first hkQsTransform
        int            count = 0;       // bones in the pose
        bool           valid = false;

        // 2026-08-24 -- every raw value the read walked through, so a failure
        // says WHERE it failed instead of just "no". The first version of this
        // returned a bare bool and the answer was unreadable: both graphs came
        // back empty in BOTH camera modes, including third person where the
        // pose demonstrably exists, which means the chain or its timing is
        // wrong -- and a bool cannot tell those apart.
        const void* charPtr   = nullptr;
        const void* genOut    = nullptr;
        const void* tracks    = nullptr;
        int32_t     numTracks = -1;
        int32_t     flags     = -1;
        int32_t     numData   = -1;
        int32_t     dataOff   = -1;
        const char* stopped   = "ok";
    };

    __declspec(noinline) bool ReadGraphPose(uint8_t* graph, GraphPose& out) noexcept
    {
        __try
        {
            if (!IsValidUserPtr(graph)) { out.stopped = "graph ptr"; return false; }
            out.charPtr = graph + GRAPH_HKB_CHARACTER_OFF; // logged for reference only

            auto* genOut = *reinterpret_cast<uint8_t**>(graph + GRAPH_GENOUT_PRIMARY_OFF);
            if (!IsValidUserPtr(genOut))
                genOut = *reinterpret_cast<uint8_t**>(graph + GRAPH_GENOUT_FALLBACK_OFF);
            out.genOut = genOut;
            if (!IsValidUserPtr(genOut)) { out.stopped = "generatorOutput null/bad"; return false; }

            auto* tracks = *reinterpret_cast<uint8_t**>(genOut + GENOUT_TRACKS_OFF);
            out.tracks = tracks;
            if (!IsValidUserPtr(tracks)) { out.stopped = "m_tracks null/bad"; return false; }

            const int32_t numTracks = *reinterpret_cast<int32_t*>(tracks + TRACKS_NUMTRACKS_OFF);
            out.numTracks = numTracks;
            if (numTracks <= TRACK_POSE || numTracks > 64) { out.stopped = "numTracks"; return false; }

            uint8_t* hdr = tracks + TRACKS_HEADERS_OFF + TRACK_POSE * TRACK_HEADER_STRIDE;
            const int8_t flags = *reinterpret_cast<int8_t*>(hdr + TRACKHDR_FLAGS_OFF);
            out.flags = flags;

            const int16_t numData = *reinterpret_cast<int16_t*>(hdr + TRACKHDR_NUMDATA_OFF);
            const int16_t dataOff = *reinterpret_cast<int16_t*>(hdr + TRACKHDR_DATAOFF_OFF);
            out.numData = numData;
            out.dataOff = dataOff;

            // The engine's own gate, replicated rather than reinterpreted:
            // notifyGeneratorOutput uses the pose only when bit 0x10 is clear.
            // Checked AFTER recording numData/dataOff so a flag-only rejection
            // is distinguishable from a garbage header.
            if ((flags & 0x10) != 0) { out.stopped = "flags bit 0x10 set"; return false; }
            if (numData <= 0 || numData > 256) { out.stopped = "numData range"; return false; }
            if (dataOff <= 0) { out.stopped = "dataOffset range"; return false; }

            out.data  = tracks + dataOff;
            out.count = numData;
            out.valid = true;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_sehCount.fetch_add(1, std::memory_order_relaxed);
            out.stopped = "SEH";
            return false;
        }
    }

    // Rotation quaternion of one bone in a pose buffer, by animation-skeleton
    // index. Separate + SEH-caged for the same reason as everything else here.
    __declspec(noinline) bool PoseBoneRot(const GraphPose& p, int bone, float outQ[4]) noexcept
    {
        __try
        {
            if (!p.valid || bone < 0 || bone >= p.count) return false;
            const auto* q = reinterpret_cast<const float*>(
                p.data + static_cast<size_t>(bone) * QSTRANSFORM_SIZE + QSTRANSFORM_ROT_OFF);
            outQ[0] = q[0]; outQ[1] = q[1]; outQ[2] = q[2]; outQ[3] = q[3];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Log both of the player's graphs' pose buffers side by side.
    //
    // The single question this answers: in FIRST person, does graph 1 (the
    // one the engine is actually ticking) carry a moving RLeg_Thigh? If it
    // does, the legs were there the whole time and every previous attempt was
    // reading the wrong side of the pipeline. If its pose count comes back
    // short, or RLeg_Thigh sits byte-identical while walking, that is a real
    // answer too and it closes the approach out for good.
    // Raw dword read, SEH-caged in its own frame so callers that use C++
    // objects are not forced into C2712 (no raw __try beside an unwindable
    // local -- the same constraint LogRootParentDiag exists for).
    __declspec(noinline) bool SafeReadU32(const void* p, size_t off, uint32_t& out) noexcept
    {
        __try
        {
            out = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p) + off);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }


    // One-shot raw window around the two structures the pose read walks. The
    // pointer found at generatorOutput+0 came back as 0x7FF7xxxxxxxx -- inside
    // the loaded exe image, i.e. a VTABLE, not the heap-allocated m_tracks the
    // layout predicts. Logging it as an RVA identifies the object's type
    // outright: look the RVA up in the IDB and the class name falls out. That
    // is a far better use of a round trip than another guess at an offset.
    __declspec(noinline) void DumpQwords(const char* label, const void* p, int n) noexcept
    {
        if (!IsValidUserPtr(p)) return;
        const uintptr_t modBase = g_moduleBase.load(std::memory_order_relaxed);
        for (int i = 0; i < n; i += 4)
        {
            uint64_t q[4] = {};
            bool okAll = true;
            for (int k = 0; k < 4; ++k)
            {
                uint32_t lo = 0, hi = 0;
                const size_t off = static_cast<size_t>(i + k) * 8;
                if (!SafeReadU32(p, off, lo) || !SafeReadU32(p, off + 4, hi)) { okAll = false; break; }
                q[k] = (static_cast<uint64_t>(hi) << 32) | lo;
            }
            if (!okAll) return;
            char note[128] = "";
            for (int k = 0; k < 4; ++k)
            {
                if (modBase && q[k] > modBase && q[k] < modBase + 0x10000000ULL)
                {
                    _snprintf_s(note, sizeof(note), _TRUNCATE,
                                " <- +0x%X is IMAGE RVA 0x%llX",
                                (i + k) * 8, (unsigned long long)(q[k] - modBase));
                    break;
                }
            }
            LOG_INFO("[FPGraph] [dump] %s +0x%02X: %016llX %016llX %016llX %016llX%s",
                label, i * 8,
                (unsigned long long)q[0], (unsigned long long)q[1],
                (unsigned long long)q[2], (unsigned long long)q[3], note);
        }
    }

    void FormatQuat(const float q[4], char* buf, size_t n) noexcept
    {
        _snprintf_s(buf, n, _TRUNCATE, "[%.4f %.4f %.4f %.4f]",
                    q[0], q[1], q[2], q[3]);
    }

    // `when` is "pre" (before the engine's own update this frame) or "post"
    // (after it). Both are sampled in the same frame so a generatorOutput that
    // the engine populates and then clears is distinguishable from one that is
    // never populated at all -- Havok documents the field as valid only during
    // the update, and "always null after" is exactly what a post-only probe
    // would report either way.
    // ---- snapshot storage --------------------------------------------------
    //
    // Written only inside Detour_MgrUpdate (where generatorOutput is live),
    // read by anyone. A spinning seqlock would be overkill: the writer is the
    // main thread inside the update and every reader this codebase has is also
    // the main thread, so a plain buffer plus a published count is enough. The
    // count is stored last and read first, so a torn read yields fewer bones
    // rather than garbage ones.
    constexpr int MAX_SNAPSHOT_BONES = 128;
    CoSyncFirstPersonGraph::PoseSnapshotBone g_snapBones[MAX_SNAPSHOT_BONES];
    std::atomic<int>      g_snapCount{0};
    std::atomic<bool>     g_snapWasFp{false};
    std::atomic<uint64_t> g_snapFrame{0};

    // Second buffer, always taken from graphs[THIRD_PERSON_GRAPH] regardless of
    // which graph is active.
    //
    // 2026-08-24 pt.4. First person has no body animation of its own -- the
    // shipped 1P clips hold the legs at a constant pose -- so the only place a
    // real third-person walk can come from on a first-person sender is the
    // third-person graph itself. That graph is parked in first person, which is
    // what the engine re-tick exists to fix, and the re-tick was previously
    // judged by reading NiNodes... which are culled in first person
    // (rootCulled=1 in 17/17 samples). So a working drive and a broken drive
    // looked identical from the outside.
    //
    // This buffer is the missing instrument: it reads what the graph COMPUTED,
    // which the cull cannot touch. The note on kUncullBodyWhileDriving already
    // records "boneCount went 0 -> 95 once the drive was on", so the tick does
    // wake the graph; the open question was only ever whether the pose it then
    // produces is a real cycle.
    CoSyncFirstPersonGraph::PoseSnapshotBone g_snapTpBones[MAX_SNAPSHOT_BONES];
    std::atomic<int>      g_snapTpCount{0};

    // Copy one graph's generated pose into the given buffer.
    __declspec(noinline) void TakePoseSnapshotInto(uint8_t* graph,
                                                   CoSyncFirstPersonGraph::PoseSnapshotBone* dst,
                                                   std::atomic<int>& outCount) noexcept
    {
        GraphPose pose;
        if (!ReadGraphPose(graph, pose)) { outCount.store(0, std::memory_order_release); return; }

        int n = pose.count;
        if (n > MAX_SNAPSHOT_BONES) n = MAX_SNAPSHOT_BONES;

        __try
        {
            for (int i = 0; i < n; ++i)
            {
                const auto* t = reinterpret_cast<const float*>(
                    pose.data + static_cast<size_t>(i) * QSTRANSFORM_SIZE);
                auto& b = dst[i];
                b.trans[0] = t[0]; b.trans[1] = t[1]; b.trans[2] = t[2];
                b.rot[0]   = t[4]; b.rot[1]   = t[5]; b.rot[2] = t[6]; b.rot[3] = t[7];
                b.scale    = t[8];
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_sehCount.fetch_add(1, std::memory_order_relaxed);
            outCount.store(0, std::memory_order_release);
            return;
        }
        outCount.store(n, std::memory_order_release);
    }

    // Copy the active graph's generated pose into g_snapBones.
    __declspec(noinline) void TakePoseSnapshot(uint8_t* graph, bool inFP) noexcept
    {
        GraphPose pose;
        if (!ReadGraphPose(graph, pose)) return;

        int n = pose.count;
        if (n > MAX_SNAPSHOT_BONES) n = MAX_SNAPSHOT_BONES;

        __try
        {
            for (int i = 0; i < n; ++i)
            {
                const auto* t = reinterpret_cast<const float*>(
                    pose.data + static_cast<size_t>(i) * QSTRANSFORM_SIZE);
                auto& b = g_snapBones[i];
                // hkQsTransform: translation xyz(w) | rotation xyzw | scale xyz(w)
                b.trans[0] = t[0]; b.trans[1] = t[1]; b.trans[2] = t[2];
                b.rot[0]   = t[4]; b.rot[1]   = t[5]; b.rot[2] = t[6]; b.rot[3] = t[7];
                b.scale    = t[8];
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_sehCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        g_snapWasFp.store(inFP, std::memory_order_relaxed);
        g_snapFrame.fetch_add(1, std::memory_order_relaxed);
        g_snapCount.store(n, std::memory_order_release); // published last
    }

    // Settle the quaternion->matrix convention without needing a camera switch
    // or a name table.
    //
    // GraphPoseApply writes pose[i] into nodeArray[i] -- graph+0x2A0, count at
    // graph+0x3C0. So for any i where a node exists, the pose buffer and that
    // node describe the SAME bone in the SAME frame on the SAME graph, index-
    // aligned by construction. Converting pose[i]'s quaternion and comparing
    // against the node's own local NiMatrix3 tests the conversion directly.
    //
    // Works in first person: the upper-body indices (Root, COM, Spine1/2,
    // Chest, the left arm chain) do have nodes there, and those are exactly
    // the ones the first-person tree carries. Bones with no node are skipped.
    //
    // Scores BOTH conventions and reports which wins, because row-vs-column
    // handedness is the one thing that cannot be read out of a struct.
    __declspec(noinline) void ValidateAgainstNodeArray(uint8_t* graph, const GraphPose& pose) noexcept
    {
        float worst[2] = { 0.0f, 0.0f };
        int   worstIdx[2] = { -1, -1 };
        int   compared = 0;

        __try
        {
            const uint16_t n = *reinterpret_cast<uint16_t*>(graph + GRAPH_BONE_COUNT_OFF);
            auto** nodes = *reinterpret_cast<void***>(graph + GRAPH_NODE_ARRAY_OFF);
            if (!IsValidUserPtr(nodes) || n == 0) return;

            const int lim = (n < pose.count) ? n : pose.count;
            for (int i = 0; i < lim; ++i)
            {
                void* node = nodes[i];
                if (!IsValidUserPtr(node)) continue;

                // NiAVObject +0x30: NiMatrix3, 3 rows of 4 floats (SIMD pad)
                const auto* nm = reinterpret_cast<const float*>(
                    reinterpret_cast<uint8_t*>(node) + 0x30);
                const float nodeM[9] = { nm[0], nm[1], nm[2],
                                         nm[4], nm[5], nm[6],
                                         nm[8], nm[9], nm[10] };

                const auto* t = reinterpret_cast<const float*>(
                    pose.data + static_cast<size_t>(i) * QSTRANSFORM_SIZE);
                const float q[4] = { t[4], t[5], t[6], t[7] };

                // reject junk rows rather than let them dominate the score
                const float len0 = nodeM[0]*nodeM[0] + nodeM[1]*nodeM[1] + nodeM[2]*nodeM[2];
                if (len0 < 0.9f || len0 > 1.1f) continue;

                ++compared;
                for (int c = 0; c < 2; ++c)
                {
                    const float x = q[0], y = q[1], z = q[2], w = q[3];
                    const float xx = x*x, yy = y*y, zz = z*z;
                    const float xy = x*y, xz = x*z, yz = y*z;
                    const float wx = w*x, wy = w*y, wz = w*z;
                    float r[9] = {
                        1.0f - 2.0f*(yy+zz), 2.0f*(xy - wz),      2.0f*(xz + wy),
                        2.0f*(xy + wz),      1.0f - 2.0f*(xx+zz), 2.0f*(yz - wx),
                        2.0f*(xz - wy),      2.0f*(yz + wx),      1.0f - 2.0f*(xx+yy) };
                    if (c == 1)
                    {
                        float tr[9] = { r[0], r[3], r[6], r[1], r[4], r[7], r[2], r[5], r[8] };
                        for (int k = 0; k < 9; ++k) r[k] = tr[k];
                    }
                    float e = 0.0f;
                    for (int k = 0; k < 9; ++k)
                    {
                        const float d = r[k] - nodeM[k];
                        const float ad = d < 0 ? -d : d;
                        if (ad > e) e = ad;
                    }
                    if (e > worst[c]) { worst[c] = e; worstIdx[c] = i; }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_sehCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (compared == 0) return;
        LOG_INFO("[FPGraph] [quat-check] poseBones=%d compared=%d  "
                 "direct maxErr=%.4f (bone %d)  transposed maxErr=%.4f (bone %d)  -> %s",
            pose.count, compared, worst[0], worstIdx[0], worst[1], worstIdx[1],
            (worst[0] <= worst[1]) ? "DIRECT wins" : "TRANSPOSED wins");
    }

    void SnapshotActiveGraph(void* mgrV) noexcept
    {
        uint32_t size = 0, activeIdx = 0;
        if (!SafeReadU32(mgrV, MGR_ARRAY_SIZE_OFF, size)) return;
        if (size != 2) return; // player only
        if (!SafeReadU32(mgrV, MGR_ACTIVE_OFF, activeIdx)) return;
        if (activeIdx > 1) return;

        void* g = GraphAt(reinterpret_cast<uint8_t*>(mgrV), activeIdx);
        if (!IsValidUserPtr(g)) return;

        TakePoseSnapshot(reinterpret_cast<uint8_t*>(g),
                         CoSyncFirstPersonGraph::IsLocalPlayerInFirstPerson());

        // Always also snapshot graphs[0], the third-person graph, whether or
        // not it is the active one. In first person it is the only possible
        // source of a real third-person walk (see g_snapTpBones).
        void* tp = GraphAt(reinterpret_cast<uint8_t*>(mgrV), THIRD_PERSON_GRAPH);
        if (IsValidUserPtr(tp))
            TakePoseSnapshotInto(reinterpret_cast<uint8_t*>(tp), g_snapTpBones, g_snapTpCount);
        else
            g_snapTpCount.store(0, std::memory_order_release);

        // Convention check (ValidateAgainstNodeArray) DISABLED 2026-09-25.
        // It walks graph+0x2A0 as a flat node array bounded by graph+0x3C0 --
        // the exact premise ProbeLegBone's notes below prove wrong (0x3C0 is the
        // bone COUNT; slots past the real entries read 0xFFFFFFFF, which passes
        // IsValidUserPtr). First live host+client test: node=0xFFFFFFFF, AV at
        // +0x30 once per second on BOTH machines (caught by its __except, but
        // logged by the VEH as CRASH with three minidumps each). Diagnostic
        // only -- the pose snapshot and replication above never used its result.
    }

    void ProbePoseBuffers(void* mgrV, const char* when) noexcept
    {
        uint32_t size = 0;
        if (!SafeReadU32(mgrV, MGR_ARRAY_SIZE_OFF, size)) return;
        if (size != 2) return; // player only: everyone else has one graph

        static uint64_t s_nextMs = 0;
        static bool     s_armed  = false;
        const uint64_t now = GetTickCount64();
        const bool isPre = (when[0] == 'p' && when[1] == 'r');
        if (isPre)
        {
            if (now < s_nextMs) return;
            s_nextMs = now + 250;
            s_armed  = true;
        }
        else
        {
            if (!s_armed) return;
            s_armed = false;
        }

        uint32_t activeIdx = 0;
        if (!SafeReadU32(mgrV, MGR_ACTIVE_OFF, activeIdx)) return;

        auto* mgr = reinterpret_cast<uint8_t*>(mgrV);
        const int inFP = CoSyncFirstPersonGraph::IsLocalPlayerInFirstPerson() ? 1 : 0;

        for (uint32_t idx = 0; idx < 2; ++idx)
        {
            void* g = GraphAt(mgr, idx);
            if (!IsValidUserPtr(g)) continue;

            GraphPose pose;
            if (!ReadGraphPose(reinterpret_cast<uint8_t*>(g), pose))
            {
                LOG_EVERY_MS(5000, LOG_INFO("[FPGraph] [pose] inFP=%d graph%u%s STOPPED at '%s' | "
                         "graph=%p char=%p genOut=%p tracks=%p numTracks=%d "
                         "flags=0x%X numData=%d dataOff=%d",
                    inFP, idx, (idx == activeIdx) ? "(ACTIVE)" : "        ",
                    pose.stopped, g, pose.charPtr, pose.genOut, pose.tracks,
                    pose.numTracks, pose.flags, pose.numData, pose.dataOff));
                continue;
            }

            float leg[4] = {}, arm[4] = {};
            char legBuf[64] = "<oob>", armBuf[64] = "<oob>";
            if (PoseBoneRot(pose, BONE_RLEG_THIGH, leg))    FormatQuat(leg, legBuf, sizeof(legBuf));
            if (PoseBoneRot(pose, BONE_LARM_UPPERARM, arm)) FormatQuat(arm, armBuf, sizeof(armBuf));

            LOG_EVERY_MS(5000, LOG_INFO("[FPGraph] [pose] inFP=%d graph%u%s bones=%d "
                     "RLeg_Thigh=%s LArm_UpperArm=%s",
                inFP, idx, (idx == activeIdx) ? "(ACTIVE)" : "        ", pose.count,
                legBuf, armBuf));
        }
    }

    void* GetHolder(Actor* actor) noexcept
    {
        if (!actor) return nullptr;
        return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(actor) + ACTOR_HOLDER_OFF);
    }

    // Canonical user-space pointer test, same rule CoSyncPlayerAnimation uses for
    // its own skeleton walks. A garbage slot in an engine array reads as
    // something like 0x00000000FFFFFFFF, which is non-null and therefore
    // sails past a plain null check straight into an access violation.
    bool IsValidUserPtr(const void* p) noexcept
    {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v >= 0x10000ULL && v < 0x0001000000000000ULL;
    }

    // Post-generate leg sample. See the call site for what it is measuring.
    //
    // 2026-08-21 pt.5 -- THIRD VERSION. The first two read the animation
    // graph's own destination array at graph+0x2A0, using graph+0x3C0 (=95) as
    // its length, and crashed both machines twice. Ground truth from the second
    // VEH report: RIP bytes `48 8B 4E 10` (mov rcx,[rsi+10h]) with
    // RSI=0x00000000FFFFFFFF, faulting at 0x10000000F.
    //
    // Two compounding mistakes, both mine. graph+0x3C0 is the graph's BONE
    // COUNT, not the length of that array -- the array holds far fewer real
    // entries, and the slots past the end read as 0xFFFFFFFF. And 0xFFFFFFFF
    // passes BOTH a null check and a canonical-user-space range check
    // (0x10000 <= 0xFFFFFFFF < 0x1000000000000), so no amount of hardening on
    // that approach was going to make it safe; the premise was wrong, not the
    // guard. The 0xFFFFFFFF at index 1-2 most likely means that pointer leads
    // to a struct with a count/flags field, not to a flat NiAVObject* array.
    //
    // This version does not touch engine structures here at all. It asks
    // CoSyncPlayerAnimation to sample the bone using the SAME skeleton walk that
    // CaptureBones and ApplySkeleton have run every frame for weeks without
    // faulting -- one that starts at GetActorRootNode and follows real
    // parent/child links, so it can only ever reach nodes genuinely in the
    // tree.
    void ProbeLegBone() noexcept
    {
        PlayerCharacter* player = CoSyncGetPlayer();
        if (!player) return;

        float rot[9] = {};
        if (!CoSyncPlayerAnimation::SampleBoneRotation(
                reinterpret_cast<Actor*>(player), "RLeg_Thigh", rot))
        {
            LOG_WARN("[FPGraph] [leg-probe] RLeg_Thigh not found on the local 3P skeleton");
            return;
        }

        LOG_INFO("[FPGraph] [leg-probe] post-generate RLeg_Thigh="
                 "[%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f]",
            rot[0], rot[1], rot[2], rot[3], rot[4], rot[5], rot[6], rot[7], rot[8]);
    }

    // Write-last re-stamp of a remote proxy's replicated bone pose, called
    // post-original from Detour_MgrUpdate (see the call site for why here).
    // Resolves the ACTIVE graph's write-target node 0 -- the "Root" bone --
    // and hands it to CoSyncPlayerAnimation, which matches it against the proxy
    // that published a pose for that node. Every read is SEH-caged: this runs
    // for every actor in the cell, including ones with a half-built graph.
    // noinline for the reason documented on every other SEH helper in this
    // project (MSVC can inline a __try body and silently drop the scope table).
    __declspec(noinline) void ReassertProxyPose(void* mgrV) noexcept
    {
        void* node0 = nullptr;
        __try
        {
            auto* mgr = reinterpret_cast<uint8_t*>(mgrV);
            const auto size = *reinterpret_cast<const uint32_t*>(mgr + MGR_ARRAY_SIZE_OFF);
            if (size == 0 || size > 8) return;

            auto active = *reinterpret_cast<const uint32_t*>(mgr + MGR_ACTIVE_OFF);
            if (active >= size) active = 0;

            void* graph = GraphAt(mgr, active);
            if (!graph) return;

            void* arrBase = *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(graph) + GRAPH_NODE_ARRAY_OFF);
            if (!IsValidUserPtr(arrBase)) return;

            // Element 0 only. This is the one slot of that array proven to
            // hold a real node ('Root', confirmed live by the drive's own
            // writeTargetNode0 diagnostic); indexing further into it is what
            // crashed the leg probe twice.
            node0 = *reinterpret_cast<void**>(arrBase);
            if (!IsValidUserPtr(node0)) node0 = nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }

        if (!node0) return;
        CoSyncPlayerAnimation::ReassertPoseForGraphRoot(node0);
    }

    // Defined at file scope further down (direct PlayerCamera read).
    // Forward-declared here so the gate diagnostic can report camera state
    // on the same line as the graph state.
    // (file-scope; see below)
    // Swap the manager's active-graph index to the third-person graph, run the
    // engine's own Update, then put the index back. Player's manager only, and
    // only while the camera is in first person -- in third person that graph is
    // already the active one and this would be a pointless double update.
    // 2026-08-25 -- LOCOMOTION. Now that we own the parked-graph tick and have
    // proven writes to it survive to the generated pose (the strafe fix), feed
    // it the FULL locomotion state the engine gives the LIVE third-person graph,
    // so the captured pose matches the real gait instead of a wrong clip:
    //   Direction (8-way axis)      -> forward/back/strafe            [Speed idx?]
    //   Speed / SpeedSampled        -> blend within the state
    //   iLocomotionSpeedState       -> walk vs run
    //   iSyncSprintState            -> sprint only when truly sprinting
    //   iSyncIdleLocomotion         -> idle vs moving
    // Without these the parked graph in first person blends a wrong clip: a jog
    // shown as a full sprint, a forward run leaning wrong, strafe as a slide.
    // Everything is derived from the LOCAL player's real movement (world-
    // position delta for speed + heading, actor facing for the axis) and set on
    // the parked graph pointer only -- never the sender's live first-person
    // view. Own noinline function so the __try does not force object unwinding
    // onto the caller (C2712).
    //   Tuning knobs, kept isolated for live adjustment:
    //     kStrafeSign  -- multiply Direction by -1 if strafe L/R come out swapped
    //     kRunSpeed    -- game u/s at which run replaces walk
    //     kSprintSpeed -- game u/s at which the sprint clip is selected
    __declspec(noinline) void WriteParkedGraphLocomotion(uint8_t* mgr) noexcept
    {
        if (!g_locoVarsReady || !g_graphSetVarFloat || !g_graphSetVarInt) return;

        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc) return;

        constexpr float kStrafeSign  = 1.0f;   // flip to -1.0f if L/R come out swapped
        constexpr float kMoveSpeed   = 20.0f;  // idle -> moving
        constexpr float kRunSpeed    = 150.0f; // walk -> run
        constexpr float kSprintSpeed = 400.0f; // run  -> sprint (real sprint ~450)

        static float    s_lastX = 0.0f, s_lastY = 0.0f;
        static uint64_t s_lastMs = 0;
        static bool     s_have   = false;
        static float    s_speedSmoothed = 0.0f;

        const uint64_t nowMs = GetTickCount64();
        const float px = pc->pos.x;
        const float py = pc->pos.y;

        float dir      = 0.0f;
        float rawSpeed = 0.0f;
        if (s_have && nowMs > s_lastMs)
        {
            const float dt = static_cast<float>(nowMs - s_lastMs) / 1000.0f;
            const float dx = px - s_lastX;
            const float dy = py - s_lastY;
            if (dt > 0.0f)
                rawSpeed = sqrtf(dx * dx + dy * dy) / dt;
            if (rawSpeed > 600.0f) rawSpeed = 600.0f;   // clamp interpolation spikes
            if (rawSpeed > kMoveSpeed)                  // heading only meaningful when moving
            {
                const float kPi = 3.14159265358979f;
                float rel = atan2f(dx, dy) - pc->rot.z; // Bethesda yaw: CW from +Y
                while (rel >  kPi) rel -= 2.0f * kPi;
                while (rel < -kPi) rel += 2.0f * kPi;
                dir = rel * kStrafeSign;
            }
        }
        s_lastX = px; s_lastY = py; s_lastMs = nowMs; s_have = true;

        // Smooth so a per-frame spike can't flip the state (sprint chatter).
        s_speedSmoothed += (rawSpeed - s_speedSmoothed) * 0.30f;
        const float speed  = s_speedSmoothed;
        const bool  moving = speed > kMoveSpeed;
        const bool  sprint = speed > kSprintSpeed;
        const int   tier   = (speed >= kRunSpeed) ? 1 : 0;   // 0 walk, 1 run

        __try
        {
            void* g = GraphAt(mgr, THIRD_PERSON_GRAPH);
            if (IsValidUserPtr(g))
            {
                g_graphSetVarFloat(g, &g_locoVarStr[0], speed);          // SpeedSampled
                g_graphSetVarFloat(g, &g_locoVarStr[1], speed);          // Speed
                g_graphSetVarFloat(g, &g_locoVarStr[2], dir);            // Direction
                g_graphSetVarInt  (g, &g_locoIntStr[0], moving ? 1 : 0); // iSyncIdleLocomotion
                g_graphSetVarInt  (g, &g_locoIntStr[1], tier);           // iLocomotionSpeedState
                g_graphSetVarInt  (g, &g_locoIntStr[4], sprint ? 1 : 0); // iSyncSprintState
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    __declspec(noinline) void EngineTickParkedGraph(void* mgrV, void* updateData) noexcept
    {
        uint32_t saved = 0;
        uint8_t* mgr = nullptr;
        __try
        {
            mgr = reinterpret_cast<uint8_t*>(mgrV);
            if (*reinterpret_cast<uint32_t*>(mgr + MGR_ARRAY_SIZE_OFF) != 2) return;
            saved = *reinterpret_cast<uint32_t*>(mgr + MGR_ACTIVE_OFF);
            if (saved == THIRD_PERSON_GRAPH) return;   // already ticked normally
            *reinterpret_cast<uint32_t*>(mgr + MGR_ACTIVE_OFF) = THIRD_PERSON_GRAPH;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }

        // 2026-08-25 -- STRAFE. Feed the parked graph the movement Direction
        // BEFORE the engine ticks it, so its 8-way locomotion blend selects the
        // strafe/backpedal clip instead of always the forward clip. Same input
        // the engine gives the LIVE third-person graph (where strafe already
        // replicates correctly through pure bone capture); in first person that
        // graph is parked and never receives a lateral Direction, which is why
        // the legs walk forward while the body slides sideways. See
        // WriteParkedGraphLocomotion -- kept in its own noinline function so its
        // __try does not force object unwinding onto this one (C2712).
        WriteParkedGraphLocomotion(mgr);

        // Outside the __try: g_origMgrUpdate is a real engine call and must not
        // sit inside a scope whose unwind we are also relying on. This is the
        // engine's REAL per-frame tick applied to graph 0: event dispatch,
        // channel flush, generate (which advances every per-node clock), and
        // the NiNode pose-apply -- the exact code that animates the walk cycle
        // in third person, now run for the parked graph while in first person.
        g_origMgrUpdate(mgrV, updateData);

        __try { *reinterpret_cast<uint32_t*>(mgr + MGR_ACTIVE_OFF) = saved; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        // Companion state (Experiment B): mark that the parked graph is being
        // driven this frame so ShouldCaptureNow()'s IsDriving() gate captures
        // in first person, and so the PC_SKELETON_SYNC 1P->3P bone copy is
        // suppressed before it can overwrite the pose we just generated.
        g_engineTickCount.fetch_add(1, std::memory_order_relaxed);
        // Reuse the existing sticky-revive undo: on the way back to third
        // person, Detour_SetActiveGraph will hand graph 0 back DEACTIVATED so
        // the engine's own activation (ragdoll bind + pending-flag clear) runs
        // in full instead of hitting GraphActivate's already-active early-out.
        g_forcedRevive.store(true, std::memory_order_relaxed);

        const auto n = g_engineTickCount.load(std::memory_order_relaxed) - 1;
        if (n == 0 || (n % 600) == 0)
            LOG_INFO("[FPGraph] engine-ticked the parked third-person graph (#%llu), "
                     "index restored to %u", static_cast<unsigned long long>(n), saved);
    }

    // Read the third-person body root's cull state and bone count.
    //
    // 2026-08-23: the one mechanism from FO4_Wrld's write-up that has never
    // been measured here. Their account of why a parked graph generates
    // nothing: "the distance-LOD throttle resolves a hidden third-person body
    // to 'generate nothing'", and separately "the third-person body is
    // APP_CULLED in first person". If the root carries the culled bit while
    // the camera is in first person, then EVERY approach tried so far --
    // including having the engine itself tick the graph -- runs into the same
    // wall, because the throttle is computed from the body's visibility rather
    // than from anything about the graph.
    //
    // NiAVObject+0x108 bit 0 is the app-culled flag (confirmed from
    // NiAVObject::ctor, which tests `byte [rbx+0x108] & 1` and propagates a
    // related bit up the parent chain).
    __declspec(noinline) int ReadThirdPersonRootCulled() noexcept
    {
        __try
        {
            PlayerCharacter* player = CoSyncGetPlayer();
            if (!player) return -1;
            void* root = static_cast<void*>(player->GetActorRootNode(false));
            if (!IsValidUserPtr(root)) return -1;
            const uint64_t flags = *reinterpret_cast<const uint64_t*>(
                reinterpret_cast<const uint8_t*>(root) + 0x108);
            return (flags & 0x1ULL) ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    // 2026-08-25 -- FURNITURE/CAMERA diagnostic (read-only, logs only when the
    // camera state changes). Pins why first-person pitch locks after standing
    // from furniture: is the camera stuck in a Furniture/Tween state, or does it
    // return to a ThirdPerson state instead of FirstPerson, and how do our
    // inFP/actorState track it across the sit->stand transition. Camera-state
    // enum (GameCamera.h): FirstPerson=0, TweenMenu, ThirdPerson1, ThirdPerson2,
    // Furniture, ... actorState dwords at Actor+0x130/0x134 carry the
    // sit/furniture bits. Own noinline function so its __try does not force
    // object unwinding onto the caller (C2712).
    __declspec(noinline) void LogCameraStateDiagOnChange() noexcept
    {
        int camId = -1;
        __try
        {
            PlayerCamera* cam = *g_playerCamera;
            if (cam && cam->cameraState)
                camId = cam->GetCameraStateId(cam->cameraState);
            else
                camId = -3; // no camera / no state
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { camId = -2; }

        static int s_lastCamId = -999;
        if (camId == s_lastCamId) return;
        const int prev = s_lastCamId;
        s_lastCamId = camId;

        uint32_t as0 = 0, as1 = 0;
        __try
        {
            if (PlayerCharacter* pc = CoSyncGetPlayer())
            {
                as0 = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(pc) + 0x130);
                as1 = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(pc) + 0x134);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        LOG_UPDATED("[FPGraph] [cam-state] CHANGED %d -> %d (FirstPerson=%d Furniture=%d) "
                    "actorState0=0x%08X actorState1=0x%08X",
            prev, camId,
            camId == PlayerCamera::kCameraState_FirstPerson ? 1 : 0,
            camId == PlayerCamera::kCameraState_Furniture   ? 1 : 0,
            as0, as1);
    }

    __declspec(noinline) void TrackPlayerThirdPersonGraph(void* mgrV) noexcept
    {
        __try
        {
            auto* mgr = reinterpret_cast<uint8_t*>(mgrV);
            if (*reinterpret_cast<uint32_t*>(mgr + MGR_ARRAY_SIZE_OFF) != 2) return; // player only

            void* tp = GraphAt(mgr, THIRD_PERSON_GRAPH);
            if (!IsValidUserPtr(tp)) return;
            g_pinnedTpGraph.store(tp, std::memory_order_relaxed);

            static uint64_t s_nextMs = 0;
            const uint64_t now = GetTickCount64();
            if (now < s_nextMs) return;
            s_nextMs = now + 2000;

            const uint32_t activeIdx = *reinterpret_cast<uint32_t*>(mgr + MGR_ACTIVE_OFF);
            int activeByte = -1;
            if (void* hkb = GetHkbWrapper(reinterpret_cast<uint8_t*>(tp)))
                activeByte = *(reinterpret_cast<uint8_t*>(hkb) + HKB_ACTIVE_BYTE_OFF);

            // inFP is on this line deliberately: the whole question is whether
            // tpActivated stays 1 WHILE the camera is first person. Splitting
            // that across two log lines is what made the earlier sessions need
            // timestamp correlation to read at all.
            const int inFP = CoSyncFirstPersonGraph::IsLocalPlayerInFirstPerson() ? 1 : 0;
            LOG_INFO("[FPGraph] [3P-gates] inFP=%d activeGraphIdx=%u (0=3P) tpActivated=%d "
                     "muteByte=%d rootCulled=%d boneCount=%u driveCount=%llu blockedDeactivates=%llu",
                inFP, activeIdx, activeByte,
                (int)*(reinterpret_cast<uint8_t*>(tp) + GRAPH_MUTE_BYTE_OFF),
                ReadThirdPersonRootCulled(),
                (unsigned)*reinterpret_cast<uint16_t*>(
                    reinterpret_cast<uint8_t*>(tp) + GRAPH_BONE_COUNT_OFF),
                static_cast<unsigned long long>(g_driveCount.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_deactBlocked.load(std::memory_order_relaxed)));

            // ── 2026-08-23: THE DIFF ────────────────────────────────────────
            //
            // Stop theorising about which variable matters and just read the
            // same graph in both camera modes. graphs[0] IS the third-person
            // graph in both: in third person the engine drives it and the legs
            // animate; in first person it is parked and they do not. Anything
            // the engine populates for an ACTIVE graph and not for a parked one
            // shows up as a difference between these two lines. Whatever that
            // turns out to be is the thing to write -- measured, not guessed.
            //
            // Read-only, by index, through the SEH-guarded reader. Indices are
            // RaiderRootBehavior.hkx's own variableNames table, which the live
            // readbacks already cross-checked (Speed=54, Direction=2).
            {
                struct V { const char* n; uint32_t i; bool isInt; };
                static const V kVars[] = {
                    {"Speed",        54,  false}, {"Dir",          2,   false},
                    {"SpdSmooth",    205, false}, {"SampledSpd",   71,  false},
                    {"idleLoco",     3,   true }, {"walkRun",      209, true },
                    {"locoSpd",      200, true }, {"sprint",       22,  true },
                    {"locoState",    238, true }, {"fwdBack",      223, true },
                    {"syncDir",      244, true }, {"bSyncDir",     252, true },
                    {"turnState",    72,  true }, {"runDir",       87,  true },
                    {"walkPb",       233, false}, {"jogPb",        234, false},
                    {"runPb",        235, false}, {"walkMult",     197, false},
                    {"jogMult",      198, false}, {"runMult",      199, false},
                };
                char buf[512]; size_t off = 0;
                for (const V& v : kVars)
                {
                    float f = 0.f;
                    if (!ReadGraphVarFloat(tp, v.i, f)) continue;
                    const int written = v.isInt
                        ? std::snprintf(buf + off, sizeof(buf) - off, " %s=%d",
                              v.n, *reinterpret_cast<const int*>(&f))
                        : std::snprintf(buf + off, sizeof(buf) - off, " %s=%.2f", v.n, f);
                    if (written <= 0 || off + written >= sizeof(buf) - 1) break;
                    off += static_cast<size_t>(written);
                }
                LOG_INFO("[FPGraph] [varsnap] inFP=%d |%s", inFP, buf);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_sehCount.fetch_add(1, std::memory_order_relaxed); }
    }

    // ------------------------------------------------------------------
    // SEH-caged worker: manually drives the PARKED third-person graph for
    // one frame. Called from the Update hook right after the engine's own
    // Update has run (so the local player's own first-person arms tick
    // normally, untouched). Mirrors FO4_Wrld's drive_third_person_graph,
    // minus the two pieces they either disabled or marked as later polish:
    //   - the position-delta SpeedSampled/Direction estimate (their own
    //     2026-08-06 note: "unsound here... made the walk worse, not
    //     better" -- never re-enabled upstream either)
    //   - the HKB active-node-list refresh (needs 4 more RVAs this pass
    //     never located; only matters to avoid a frozen clip immediately
    //     after a FRESH revive, not for the drive to work at all)
    // ------------------------------------------------------------------
    bool DriveThirdPersonGraph(void* mgrV, void* updateData) noexcept
    {
        __try
        {
            auto* mgr = reinterpret_cast<uint8_t*>(mgrV);

            // The engine's own "is this the player" test: a manager with
            // exactly two graphs. Every NPC has one graph and is skipped.
            const auto size = *reinterpret_cast<const uint32_t*>(mgr + MGR_ARRAY_SIZE_OFF);
            if (size != 2) return false;

            const auto active = *reinterpret_cast<const uint32_t*>(mgr + MGR_ACTIVE_OFF);
            if (active != FIRST_PERSON_GRAPH) return false;

            void* tpGraph = GraphAt(mgr, THIRD_PERSON_GRAPH);
            if (!tpGraph) return false;
            auto* g = reinterpret_cast<uint8_t*>(tpGraph);
            g_forcedRevive.store(true, std::memory_order_relaxed);
            g_drivenGraph = tpGraph;

            // (1) REVIVE. SetActiveGraph deactivated this graph's HKB state
            // machine on the way into first person; every call below
            // early-outs on the wrapper's "is live" byte. Activating is
            // idempotent (no-ops when already live).
            //
            // Full prep -> activate -> commit sequence, not GraphActivate
            // alone -- read directly off SetActiveGraph's own disassembly
            // 2026-08-19 after GraphActivate-alone produced a graph that
            // "generated" and "wrote bones" every single driven frame
            // (independently confirmed at each stage) yet emitted a byte-
            // for-byte IDENTICAL pose for 15+ continuous seconds live. The
            // real engine never calls GraphActivate in isolation -- it
            // always brackets it with sub_14052A800(graph+104, mgr) right
            // before and sub_1413AA730(mgr+136, &graphPtr) right after, all
            // under the manager lock. Whatever those two finish (plausibly:
            // populating the active-node list GraphActivate's own vtable
            // call alone doesn't fully build) was the missing piece.
            if (!IsGraphActive(g))
            {
                if (!g_graphActivate) return false;
                MgrLock(mgr);
                if (!kExactReferenceActivation && g_graphActivatePrep)
                    g_graphActivatePrep(g + GRAPH_ACTIVATE_PREP_ARG_OFF, mgr);
                g_graphActivate(tpGraph);
                if (!kExactReferenceActivation && g_mgrActivateCommit)
                {
                    void* gp = tpGraph;
                    g_mgrActivateCommit(mgr + MGR_ACTIVATE_COMMIT_ARG_OFF, &gp);
                }
                MgrUnlock(mgr);
                if (!IsGraphActive(g)) return false; // refused to wake

                // A freshly-activated behavior sits in its INITIAL state
                // (T-pose) until something moves the state machine. Raise
                // the two events that FO4_Wrld's live measurement showed
                // actually do that, so the ghost doesn't need the player to
                // draw a weapon before it leaves T-pose.
                if (g_wakeEventsReady && g_graphSendEvent)
                {
                    MgrLock(mgr);
                    for (auto& s : g_wakeEventStr)
                        g_graphSendEvent(tpGraph, &s, nullptr);
                    MgrUnlock(mgr);
                }
            }

            // (2) Re-prime the locomotion snapshot EVERY driven frame, not
            // just at the camera switch -- FO4_Wrld's own reported residue
            // from priming once was "no strafing animation, and an
            // occasional walk where a run belongs" because the parked
            // graph never learns the player changed direction/speed after
            // the switch instant.
            if (g_populateVars)
            {
                PlayerCharacter* player = CoSyncGetPlayer();
                if (player)
                {
                    void* holder = GetHolder(player);
                    void* gp = tpGraph;
                    g_populateVars(holder, &gp);

                    // 2026-08-24 -- DISABLED (Experiment A). This forced
                    // bAnimationDriven/bAllowRotation/bGraphDrivenTranslation/
                    // bGraphDrivenRotation/bFreezeSpeedUpdate/
                    // bFreezeRotationUpdate to 0 on the parked graph EVERY
                    // driven frame, immediately AFTER PopulateGraphVariables
                    // (which had just set them to the engine's own values for
                    // a moving player). The proven-working reference
                    // (FO4_Wrld first_person_graph.cpp) calls
                    // PopulateGraphVariables and then touches NOTHING else --
                    // it never clears these flags, and its legs animate.
                    // Overwriting bAnimationDriven/bGraphDriven* to 0 is the
                    // exact shape of "procedural upper body stays alive,
                    // clip-driven lower body dies", which is the measured
                    // frozen-legs signature. Removing this divergence is the
                    // whole point of Experiment A. Restore only with evidence.
                    // CoSyncAnimVarSystem::ClearParkedGraphFreezeFlags(player);
                }
            }

            // (3) FORCED update context. Reusing the engine's own per-frame
            // context means reusing the camera pointer + LOD word it
            // computed for the FIRST-person graph: GraphUpdate then runs
            // its distance/LOD throttle against the parked graph's root
            // node, and a hidden (APP_CULLED) third-person body resolves to
            // "generate nothing" -- the pose-apply that follows then has
            // zero bones to write. Null camera pointer + force-full-LOD
            // byte takes the generate branch unconditionally instead, at
            // the full bone count. Layout per the engine's own per-frame
            // driver, not its (dt=0, static-prime) bind path.
            uint8_t ctx[0x40] = {};
            const float dt = *reinterpret_cast<const float*>(updateData);
            *reinterpret_cast<float*>(ctx + 0x00) = dt;
            *reinterpret_cast<void**>(ctx + 0x08) = nullptr; // no camera = force
            // 2026-08-21 -- byte-for-byte verified against a fresh IDA decompile
            // of ALL THREE real callers of BSAnimationGraphManager::Update
            // (including sub_140E10740, the primary per-frame BATCH driver that
            // loops every live actor's own graph manager -- the actual "engine's
            // per-frame driver" this comment already claimed to match, now
            // actually confirmed instead of guessed). Real bytes at 0x18-0x1D:
            // int32@0x18 = 0x01000000 (bytes 00,00,01,00) then int16@0x1C = 257
            // (bytes 01,01). ctx[0x1B] was previously set to 1 -- WRONG, the
            // real value is 0. Also confirmed the "null camera" bytes at 0x08-
            // 0x17 aren't a special force-trick at all: EVERY real caller,
            // including the batch driver for ordinary live/rendered actors,
            // passes zero there too.
            *reinterpret_cast<uint16_t*>(ctx + 0x18) = 0;    // no bone cap
            ctx[0x1A] = 1; // force full LOD (skips the distance throttle)
            // Back to the reference value. Proven irrelevant either way (it is
            // only read inside a block already skipped when ctx[0x1A] != 0,
            // verified at 0x01320A9B), but the point of this build is zero
            // unexplained divergence from the implementation that works.
            ctx[0x1B] = 1;
            ctx[0x1C] = 1;
            // REVERTED 2026-08-20: briefly forced to 0 as an unverified
            // guess at disabling "the IK/foot block" (FO4_Wrld's label,
            // downstream effect never actually confirmed by decompile this
            // session). Superseded by a real fix: RaiderRootBehavior.hkx
            // (the actual asset) names the real foot-IK kill switch as a
            // graph variable, `bHumanoidFootIKDisable`, now set directly
            // below every driven frame via the same setter CoSyncPlayerAnimation
            // already uses. Restored to 1, matching normal engine behavior
            // for a live, visible character -- this raw byte was never the
            // right lever.
            ctx[0x1D] = 1; // allow the IK/foot block, as the engine does

            // Sentinel: Update's own last write inside its gated block is
            // `graph+0x3A8 = updateData+0x00`. Poison it first; if it comes
            // back holding our dt after GraphUpdate, every gate passed and
            // the call really executed -- if it stays poisoned, nothing ran
            // and everything else this tick is noise. Cheap and caught the
            // exact "ran 3600 times a minute and changed nothing" mistake
            // upstream hit early on.
            *reinterpret_cast<float*>(g + GRAPH_UPDATE_DUE_SENTINEL_OFF) = -12345.0f;

            // ── REFRESH THE ACTIVE NODE LIST ────────────────────────────────
            // Must run BEFORE the generate step below, exactly where the
            // engine's own graph-bind path does it (see the RVA block at the
            // top of this file for the call site the addresses came from).
            // Without it the timestep is fanned out over a stale node list:
            // generate still writes all 95 bones, but the clip clock never
            // advances, so the walk cycle freezes while procedural aim/look
            // keeps the arms moving. That is the exact first-person symptom
            // measured on 2026-08-21 (activeNodes pinned at 57, RLeg_Thigh
            // byte-identical across 458 samples).
            //
            // Gated on the same dirty byte the engine gates on, so a graph
            // that does not need it pays one byte read. Every pointer is
            // null-checked: a missing physics world just means the context is
            // built without one, which is what the engine itself does when
            // the graph has no world attached.
            if (g_hkbCtxCtor && g_hkbCtxDtor && g_hkbUpdActive && g_hkbUpdSync)
            {
                auto* hkbW = reinterpret_cast<uint8_t*>(GetHkbWrapper(g));
                if (hkbW && *(hkbW + HKB_NODES_DIRTY_OFF))
                {
                    void* physHolder = *reinterpret_cast<void**>(g + GRAPH_PHYS_WORLD_OFF);
                    void* world = physHolder
                        ? *reinterpret_cast<void**>(
                              reinterpret_cast<uint8_t*>(physHolder) + PHYS_HOLDER_WORLD_OFF)
                        : nullptr;

                    // hkbContext is a stack temporary here, same as the
                    // engine's own use. 0x80 covers the 0x60 the ctor
                    // actually touches with room to spare.
                    uint8_t hctx[0x80] = {};
                    g_hkbCtxCtor(hctx, g + GRAPH_HKB_CHARACTER_OFF, world, nullptr);
                    g_hkbUpdActive(hkbW, hctx, 0);
                    g_hkbUpdSync(hkbW, hctx);
                    g_hkbCtxDtor(hctx);   // releases the world ref the ctor took

                    const auto n = g_nodeRefreshCount.fetch_add(1, std::memory_order_relaxed);
                    if (n == 0 || (n % 600) == 0)
                        LOG_INFO("[FPGraph] active-node list refreshed (#%llu) world=%p",
                            static_cast<unsigned long long>(n), world);
                }
            }

            // Locomotion hand-off, read here so the values are this frame's.
            // Source is the LIVE first-person graph -- the engine's own
            // per-frame numbers, not a positional delta (see the long note on
            // kDriveLocomotionVars for why that distinction is the whole
            // fix). Applied below, inside the manager lock, between the flush
            // and the update: that is the only window where a written value
            // reaches the clip generator intact this frame.
            float fpSpeed = 0.0f, fpDir = 0.0f;
            float rawSpeed2 = 0.0f, rawSpeed90 = 0.0f;
            bool  haveLoco = false;
            if (kDriveLocomotionVars && g_locoVarsReady && g_graphSetVarFloat)
            {
                void* fpGraph = GraphAt(mgr, FIRST_PERSON_GRAPH);
                if (fpGraph && ReadGraphVarFloat(fpGraph, kFPVar_Direction, fpDir))
                {
                    const bool got2  = ReadGraphVarFloat(fpGraph, kFPVar_Speed,         rawSpeed2);
                    const bool got90 = ReadGraphVarFloat(fpGraph, kFPVar_SpeedSmoothed, rawSpeed90);
                    if (got2 || got90)
                    {
                        // Whichever slot has real magnitude is the live one;
                        // see the kFPVar_SpeedSmoothed comment for why slot 2
                        // alone is not trustworthy.
                        const float s2  = got2  ? std::fabs(rawSpeed2)  : 0.0f;
                        const float s90 = got90 ? std::fabs(rawSpeed90) : 0.0f;
                        fpSpeed  = (s90 >= s2) ? rawSpeed90 : rawSpeed2;
                        haveLoco = true;
                    }
                }
            }

            // Temporarily clear APP_CULLED on the third-person root -- see
            // kUncullBodyWhileDriving. Restored right after the pose apply
            // below, inside the same lock, so no rendered frame ever sees it.
            void*   uncullRoot  = nullptr;
            uint64_t savedFlags = 0;
            if (kUncullBodyWhileDriving)
            {
                if (PlayerCharacter* pc = CoSyncGetPlayer())
                {
                    void* r = static_cast<void*>(pc->GetActorRootNode(false));
                    if (IsValidUserPtr(r))
                    {
                        auto* f = reinterpret_cast<uint64_t*>(
                            reinterpret_cast<uint8_t*>(r) + 0x108);
                        savedFlags = *f;
                        if (savedFlags & 0x1ULL)
                        {
                            *f = savedFlags & ~0x1ULL;
                            uncullRoot = r;
                        }
                    }
                }
            }

            MgrLock(mgr);
            if (g_graphEventFlush) g_graphEventFlush(tpGraph, mgr + MGR_EVENT_ARRAY_OFF);

            if (haveLoco)
            {
                // Graph-scoped setter: writes tpGraph and nothing else. The
                // first-person graph driving the player's own arms is never
                // touched -- that is what separates this from the holder-level
                // setters the project rules out.
                const uint8_t okSampled = g_graphSetVarFloat(tpGraph, &g_locoVarStr[0], fpSpeed);
                const uint8_t okSpeed   = g_graphSetVarFloat(tpGraph, &g_locoVarStr[1], fpSpeed);
                const uint8_t okDir     = g_graphSetVarFloat(tpGraph, &g_locoVarStr[2], fpDir);

                // The clip clocks. Every locomotion clip's playbackSpeed is
                // bound to one of these (proved from the unpacked asset), and
                // nothing populates them on a parked graph -- at 0 the clip is
                // re-written to the skeleton every frame while its local time
                // never advances, which is the frozen-legs signature exactly.
                // 1.0 = play at the authored rate.
                // Sample BEFORE writing: this is the number the whole theory
                // rests on. If it reads 0.000 here, the clips were frozen.
                float wasWalkPb = -1.f, wasRunPb = -1.f;
                ReadGraphVarFloat(tpGraph, kTPVar_WalkPlayback, wasWalkPb);
                ReadGraphVarFloat(tpGraph, kTPVar_RunPlayback,  wasRunPb);

                const uint8_t okWalkPb = g_graphSetVarFloat(tpGraph, &g_locoVarStr[3], 1.0f);
                const uint8_t okJogPb  = g_graphSetVarFloat(tpGraph, &g_locoVarStr[4], 1.0f);
                const uint8_t okRunPb  = g_graphSetVarFloat(tpGraph, &g_locoVarStr[5], 1.0f);

                // The state selector. Speed only blends WITHIN a state; this
                // is what picks Walk vs Run (LocomotionRoot state ids).
                const float absSpeed = std::fabs(fpSpeed);
                const bool  moving   = absSpeed > 5.0f;
                const int   runTier  = (absSpeed >= 150.0f) ? kLocoState_Run : kLocoState_Walk;
                const int intVals[5] = {
                    moving ? 1 : 0,   // iSyncIdleLocomotion  -- the master switch
                    runTier,          // iLocomotionSpeedState
                    runTier,          // iSyncWalkRun
                    runTier,          // iSyncLocomotionSpeed
                    0,                // iSyncSprintState
                };
                uint8_t okState = 0;
                char intResults[64]; intResults[0] = 0;
                if (g_graphSetVarInt)
                {
                    size_t off = 0;
                    for (size_t i = 0; i < 5; ++i)
                    {
                        const uint8_t ok = g_graphSetVarInt(tpGraph, &g_locoIntStr[i], intVals[i]);
                        if (i == 0) okState = ok;   // iSyncIdleLocomotion is the one that matters
                        if (off < sizeof(intResults) - 2)
                            off += static_cast<size_t>(std::snprintf(intResults + off,
                                sizeof(intResults) - off, "%s", ok ? "1" : "-"));
                    }
                }

                // Read the parked graph's own values back so this is
                // self-diagnosing: if the legs still do not move, this line
                // says whether the playback speeds were the problem.
                float curWalkPb = -1.f, curRunPb = -1.f, curSpeed = -1.f, curState = -1.f;
                ReadGraphVarFloat(tpGraph, kTPVar_WalkPlayback,   curWalkPb);
                ReadGraphVarFloat(tpGraph, kTPVar_RunPlayback,    curRunPb);
                ReadGraphVarFloat(tpGraph, kTPVar_Speed,          curSpeed);
                ReadGraphVarFloat(tpGraph, kTPVar_LocoSpeedState, curState);

                // Both raw slots are logged, not just the chosen one: if the
                // walk still looks wrong, this line says immediately whether
                // the source flickered or the write failed.
                const auto n = g_locoWriteCount.fetch_add(1, std::memory_order_relaxed);
                if (n == 0 || (n % 600) == 0)
                    LOG_INFO("[FPGraph] loco hand-off (#%llu): 1P Speed[2]=%.2f Smoothed[90]=%.2f -> used %.2f "
                             "dir=%.2f | wrote [Sampled=%s Speed=%s Dir=%s WalkPb=%s JogPb=%s RunPb=%s State=%s] "
                             "| 3P readback Speed=%.2f WalkPb=%.3f RunPb=%.3f StateRaw=%d",
                        static_cast<unsigned long long>(n), rawSpeed2, rawSpeed90, fpSpeed, fpDir,
                        okSampled ? "ok" : "NO-VAR", okSpeed ? "ok" : "NO-VAR", okDir ? "ok" : "NO-VAR",
                        okWalkPb ? "ok" : "NO-VAR", okJogPb ? "ok" : "NO-VAR", okRunPb ? "ok" : "NO-VAR",
                        intResults,
                        curSpeed, curWalkPb, curRunPb,
                        *reinterpret_cast<const int*>(&curState));
                if (n == 0 || (n % 600) == 0)
                    LOG_INFO("[FPGraph]   playbackSpeed BEFORE our write: walk=%.3f run=%.3f "
                             "(0.000 == clips frozen, which is the frozen-legs cause)",
                        wasWalkPb, wasRunPb);
            }

            if (g_graphUpdate)     g_graphUpdate(tpGraph, ctx);

            // ── 2026-08-21 pt.3: POST-GENERATE LEG PROBE ────────────────────
            //
            // Everything upstream now measures clean and first person is still
            // wrong, so stop guessing and read the one value that separates
            // the two remaining explanations. Confirmed working already:
            // events reach the parked graph and move it (MoveStart/SprintStart
            // logged, activeNodes 29..62), the live speed is written
            // (SpeedSmoothed[90]=74.87 -> Speed=ok), the pose-apply runs
            // (RAN=YES, boneCount=95), and the receiver applies all 55 bones
            // with our write surviving (55/55 OURS SURVIVES, tier=FRESH).
            // Yet the SENDER's own captured RLeg_Thigh sits at one identical
            // bind pose for 332 of 409 first-person samples while the arm is
            // 78% distinct.
            //
            // So either the generated pose itself has static legs (fix: the
            // locomotion blend needs more than Speed), or the pose is fine and
            // something overwrites the legs between here and CaptureBones
            // (fix: find that writer). Sampling the leg bone HERE -- straight
            // out of the writer's own destination array, immediately after
            // generate -- distinguishes them: compare this line against the
            // [witness] line's RLeg_Thigh from the same moment.
            //   same, and both bind pose  -> graph is generating static legs
            //   different                 -> something overwrites after us
            {
                static uint64_t s_nextLegProbeMs = 0;
                const uint64_t nowMs = GetTickCount64();
                if (nowMs >= s_nextLegProbeMs)
                {
                    s_nextLegProbeMs = nowMs + 500;
                    ProbeLegBone();
                }
            }

            // GraphPoseApply's own decompile shows TWO branches gated on
            // graph+966 ("updateDue"): SET dispatches through a generic
            // listener/observer notifier (sub_141320F80 -> sub_140525900,
            // confirmed by decompile to be a plain "notify all registered
            // listeners" event dispatcher -- NOT a bone writer); CLEAR takes
            // the direct path that calls the real NiNode writer
            // (sub_1413998F0), gated only by the physics predicate. This
            // graph's updateDue is permanently 1 in steady state (confirmed
            // live, 2500+ consecutive driven frames, never 0) -- meaning
            // every prior test was silently taking the notify-only branch
            // and never writing a single bone, no matter how many times
            // GraphUpdate itself ran. Force it clear for just this one
            // call (save/restore around it, since GraphUpdate's own
            // generate-dispatch branch needs it SET to run, and other code
            // may depend on its persisted value) so THIS call takes the
            // real bone-write branch.
            const uint8_t updateDueBeforeApply = *(g + GRAPH_UPDATE_DUE_BYTE_OFF);
            *(g + GRAPH_UPDATE_DUE_BYTE_OFF) = 0;
            // MUST be nullptr, not a real buffer -- confirmed by decompile
            // 2026-08-19 (live crash root-cause): GraphPoseApply's tail call
            // `sub_14131E030(a1, v17, a2)` gates ALL of its dangerous logic
            // behind `if (a3)` (a3 = this argument) -- non-null wakes a
            // queue-walking path that indexes a graph-side array using data
            // this manually-driven graph never populates the way the real
            // engine flow does, and crashes on a garbage/out-of-range index
            // (confirmed: the crash call stack bottomed out inside exactly
            // this function). The direct NiNode bone-writer branch inside
            // GraphPoseApply itself never touches this argument at all, so
            // nullptr costs nothing -- it only disarms the unrelated,
            // unsafe-for-us tail path. This is also what FO4_Wrld's own
            // (differently-built) code passes.
            if (g_graphPoseApply) g_graphPoseApply(tpGraph, nullptr);

            if (uncullRoot)
            {
                auto* f = reinterpret_cast<uint64_t*>(
                    reinterpret_cast<uint8_t*>(uncullRoot) + 0x108);
                *f = savedFlags;   // put the culled bit back before anything renders
                static std::atomic<uint64_t> s_uncull{0};
                const auto n = s_uncull.fetch_add(1, std::memory_order_relaxed);
                if (n == 0 || (n % 600) == 0)
                    LOG_INFO("[FPGraph] un-culled the 3P body for the generate pass (#%llu)",
                        static_cast<unsigned long long>(n));
            }
            *(g + GRAPH_UPDATE_DUE_BYTE_OFF) = updateDueBeforeApply;
            MgrUnlock(mgr);

            static uint64_t s_nextLogMs = 0;
            const uint64_t now = GetTickCount64();
            if (now >= s_nextLogMs)
            {
                s_nextLogMs = now + 5000;
                const bool ran = (*reinterpret_cast<float*>(g + GRAPH_UPDATE_DUE_SENTINEL_OFF) != -12345.0f);
                const uint8_t genDue = *(g + GRAPH_GEN_DUE_BYTE_OFF);
                const uint16_t boneCount = *reinterpret_cast<uint16_t*>(g + GRAPH_BONE_COUNT_OFF);
                const uint8_t physPred = g_graphPhysPred ? g_graphPhysPred(tpGraph) : 0xFF;

                // THE real decisive check, corrected: read the writer's
                // actual destination array (graph+0x2A0, confirmed by
                // decompiling sub_1413998F0 itself -- its a2, not a1, is the
                // NiAVObject* array it writes into) and pull out entry 0's
                // pointer + bone name, the same way CoSyncPlayerAnimation's own
                // SafeReadBoneName does. A real, recognizable bone name here
                // (e.g. "COM", "Root", "Pelvis") proves the array points
                // into the actor's real skeleton; garbage/empty means it
                // doesn't, regardless of any raw pointer's value.
                void* node0Ptr = nullptr;
                const char* node0Name = nullptr;
                __try
                {
                    void* arrBase = *reinterpret_cast<void**>(g + GRAPH_NODE_ARRAY_OFF);
                    if (arrBase)
                    {
                        node0Ptr = *reinterpret_cast<void**>(arrBase);
                        if (node0Ptr)
                        {
                            void* poolEntry = *reinterpret_cast<void**>(
                                reinterpret_cast<uint8_t*>(node0Ptr) + 0x10);
                            if (poolEntry)
                                node0Name = reinterpret_cast<const char*>(
                                    reinterpret_cast<uint8_t*>(poolEntry) + 0x18);
                        }
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) { node0Ptr = reinterpret_cast<void*>(uintptr_t(-1)); node0Name = "<SEH>"; }

                void* captureRoot = nullptr;
                if (PlayerCharacter* player = CoSyncGetPlayer())
                {
                    __try { captureRoot = static_cast<void*>(player->GetActorRootNode(false)); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { captureRoot = reinterpret_cast<void*>(uintptr_t(-1)); }
                }

                int32_t activeNodeCount = -1;
                __try
                {
                    void* hkbW = GetHkbWrapper(g);
                    if (hkbW)
                    {
                        void* actNodes = *reinterpret_cast<void**>(
                            reinterpret_cast<uint8_t*>(hkbW) + HKB_ACTIVE_NODES_OFF);
                        if (actNodes)
                            activeNodeCount = *reinterpret_cast<int32_t*>(
                                reinterpret_cast<uint8_t*>(actNodes) + 8);
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) { activeNodeCount = -2; }

                // Walk UP the parent chain from the write target (NiAVObject
                // parent pointer @ +0x28, per CoSyncPlayerAnimation.cpp's own
                // documented layout) to see whether it's actually inside the
                // SAME tree captureRoot anchors, or a genuinely separate
                // skeleton copy the writer updates that CaptureBones' walk
                // never reaches. 'Root' being a real bone name doesn't
                // settle this by itself -- a stale/detached second copy of
                // the skeleton would produce an equally real-looking name.
                int ancestorDepth = -1; // -1 = not found within the walk
                void* ancestor = node0Ptr;
                if (node0Ptr && node0Ptr != reinterpret_cast<void*>(uintptr_t(-1)) && captureRoot)
                {
                    __try
                    {
                        for (int depth = 0; depth < 12 && ancestor; ++depth)
                        {
                            if (ancestor == captureRoot) { ancestorDepth = depth; break; }
                            ancestor = *reinterpret_cast<void**>(
                                reinterpret_cast<uint8_t*>(ancestor) + 0x28);
                        }
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) { ancestorDepth = -2; /* SEH mid-walk */ }
                }

                // updateDue is force-cleared before every PoseApply call now
                // (see above) -- physPred==0 is the only remaining gate on
                // whether the NiNode bone writer actually ran this frame.
                //
                // 2026-08-24 -- THE CLIP-CLOCK TIMERS (0x350/0x354). Restored
                // from FO4_Wrld's own post-drive diagnostic (CoSync had dropped
                // it). Disassembly of GraphUpdate (generate) on 1.11.240 proves
                // the per-node timestep is `min(-timer0, dt)` CLAMPED TO >= 0,
                // and the healthy steady state after a consumed step is
                // timer0==0 / timer1==-FLT_MAX (the engine resets them to that
                // at 0x141320C48). If the legs are static while dt is real,
                // this line says whether the timestep is actually reaching the
                // clip: a timer0 that stays positive means min(-timer0,dt)<0 is
                // clamped to 0 every frame -> generate writes 95 bones but the
                // clip clock never advances -> exactly the frozen-legs
                // signature. This is the single measurement that separates "the
                // clip isn't ticking" from "the clip ticks but the pose is
                // wrong". Read straight off the graph we just drove.
                float tmr0 = -99.f, tmr1 = -99.f;
                __try {
                    tmr0 = *reinterpret_cast<const float*>(g + 0x350);
                    tmr1 = *reinterpret_cast<const float*>(g + 0x354);
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                LOG_INFO("[FPGraph] drive: RAN=%s dt=%.5f driveCount=%llu | "
                    "naturalUpdateDue=%u genDue=%u boneWriteBranch=%s activeNodes=%d | "
                    "boneCount=%u physPred=%u | timer0(0x350)=%.5f timer1(0x354)=%.3e | "
                    "writeTargetNode0=%p name='%s' captureRoot=%p "
                    "sameTree(depth)=%d",
                    ran ? "YES" : "NO-OP", dt,
                    static_cast<unsigned long long>(g_driveCount.load(std::memory_order_relaxed)),
                    updateDueBeforeApply, genDue,
                    (physPred == 0) ? "TAKEN" : "SKIPPED(physPred)", activeNodeCount,
                    boneCount, physPred,
                    tmr0, static_cast<double>(tmr1),
                    node0Ptr, node0Name ? node0Name : "<null>", captureRoot,
                    ancestorDepth);
            }

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_sehCount.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    // Mirror one animation event onto the parked third-person graph, so its
    // state machine keeps receiving moveStart/turn/etc instead of looping
    // whatever state it was parked in. sinkOrNull=NULL is the entire safety
    // story: with it null there is zero outward gameplay traffic from this
    // call (no duplicated footsteps/hit frames/weapon-fire/script events) --
    // the engine's own idiom for a silent, internal-only event send.
    uint8_t __fastcall Detour_NotifyAnimationGraph(void* mgrV, const void* event)
    {
        const uint8_t r = g_origMgrNotify ? g_origMgrNotify(mgrV, event) : 0;
        if (!g_enabled.load(std::memory_order_relaxed)) return r;
        if (!mgrV || !event || !g_graphSendEvent) return r;
        if (!CoSyncPlayerAnimation::IsBoneReplicationEnabled()) return r;
        if (!CoSyncNet::IsConnected()) return r;

        __try
        {
            // Each early-out is counted with a reason. "Mirroring never fired"
            // and "mirroring fired and did nothing" need completely different
            // fixes, and until now the log could not tell them apart.
            auto skip = [&](const char* why) {
                const auto s = g_mirrorSkipped.fetch_add(1, std::memory_order_relaxed);
                if (s == 0 || (s % 3000) == 0)
                    LOG_INFO("[FPGraph] event mirror SKIPPED (#%llu): %s",
                        static_cast<unsigned long long>(s), why);
            };

            auto* mgr = reinterpret_cast<uint8_t*>(mgrV);
            if (*reinterpret_cast<uint32_t*>(mgr + MGR_ARRAY_SIZE_OFF) != 2) { skip("not the player (graph count != 2)"); return r; }
            if (*reinterpret_cast<uint32_t*>(mgr + MGR_ACTIVE_OFF) != FIRST_PERSON_GRAPH) { skip("camera is third person"); return r; }

            void* tpGraph = GraphAt(mgr, THIRD_PERSON_GRAPH);
            if (!tpGraph) { skip("no third-person graph"); return r; }
            auto* g = reinterpret_cast<uint8_t*>(tpGraph);
            if (!IsGraphActive(g)) { skip("parked graph not revived (hkb+0x1AA == 0)"); return r; }
            if (*(g + GRAPH_EVENT_PENDING_OFF)) { skip("event already pending on parked graph"); return r; }

            // Read the active-node count either side of the send. This is how
            // FO4_Wrld identified which events actually move a parked state
            // machine, and it is the missing half of the first-person
            // diagnosis: writing Speed into the graph is a BLEND scalar, but
            // a locomotion state machine leaves idle on an EVENT. Measured
            // 2026-08-21, the host's own first-person segments held one
            // identical leg pose for 366 of 486 samples EVEN IN FRAMES WHERE
            // the hand-off had just written Speed=350.87 successfully -- so
            // "does any event reach this graph, and does it move it" is now
            // the open question, and this answers it without another guess.
            auto activeNodeCount = [](uint8_t* graph) -> int32_t {
                void* hkb = GetHkbWrapper(graph);
                if (!hkb) return -1;
                void* an = *reinterpret_cast<void**>(
                    reinterpret_cast<uint8_t*>(hkb) + HKB_ACTIVE_NODES_OFF);
                return an ? *reinterpret_cast<int32_t*>(
                    reinterpret_cast<uint8_t*>(an) + 8) : -1;
            };
            const int32_t before = activeNodeCount(g);

            MgrLock(mgr);
            g_graphSendEvent(tpGraph, event, nullptr);
            MgrUnlock(mgr);

            const int32_t after = activeNodeCount(g);
            const auto n = g_eventsMirrored.fetch_add(1, std::memory_order_relaxed);
            if (after != before)
            {
                // An event that changed the active-node count is one that
                // actually moved the state machine. Name it: that is the
                // event worth raising deliberately at wake-up.
                const char* nm = "?";
                auto* pool = *reinterpret_cast<char* const*>(event);
                if (pool) nm = pool + 0x18;
                LOG_INFO("[FPGraph] event '%s' MOVED the parked graph: activeNodes %d -> %d (mirrored #%llu)",
                    nm, before, after, static_cast<unsigned long long>(n));
            }
            else if (n == 0 || (n % 600) == 0)
            {
                LOG_INFO("[FPGraph] events mirrored to parked graph: #%llu (activeNodes steady at %d)",
                    static_cast<unsigned long long>(n), before);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_sehCount.fetch_add(1, std::memory_order_relaxed);
        }
        return r;
    }

    // Undo the "sticky revive" before the engine switches back to third
    // person, and re-prime locomotion at the instant of switching INTO
    // first person.
    //
    // GraphActivate early-outs when the graph is ALREADY active. Since this
    // module leaves the parked third-person graph revived, the engine's own
    // SetActiveGraph(0) on the way back to third person would hit that
    // early-out and skip whatever ragdoll-bind/pending-flag work the real
    // activation does. Hand the graph back genuinely deactivated first (via
    // the real, un-hooked GraphDeactivate) so that activation runs in full.
    uint8_t __fastcall Detour_SetActiveGraph(void* mgrV, uint32_t newIdx)
    {
        // Refuse the move to the first-person graph -- see kForceThirdPersonGraph.
        if (kForceThirdPersonGraph && newIdx == FIRST_PERSON_GRAPH && mgrV
            && CoSyncPlayerAnimation::IsBoneReplicationEnabled() && CoSyncNet::IsConnected())
        {
            bool isPlayerMgr = false;
            __try
            {
                isPlayerMgr = (*reinterpret_cast<uint32_t*>(
                    reinterpret_cast<uint8_t*>(mgrV) + MGR_ARRAY_SIZE_OFF) == 2);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { isPlayerMgr = false; }

            if (isPlayerMgr)
            {
                const auto n = g_fpSwitchBlocked.fetch_add(1, std::memory_order_relaxed);
                if (n == 0 || (n % 50) == 0)
                    LOG_INFO("[FPGraph] blocked switch to the first-person graph (#%llu) -- "
                             "animation stays third person, camera does not",
                        static_cast<unsigned long long>(n));
                return 1; // report success; activeGraph is left on the 3P graph
            }
        }

        if (g_forcedRevive.load(std::memory_order_relaxed)
            && newIdx == THIRD_PERSON_GRAPH && mgrV && g_origGraphDeactivate)
        {
            __try
            {
                auto* mgr = reinterpret_cast<uint8_t*>(mgrV);
                if (*reinterpret_cast<uint32_t*>(mgr + MGR_ARRAY_SIZE_OFF) == 2)
                {
                    void* tpGraph = GraphAt(mgr, THIRD_PERSON_GRAPH);
                    if (tpGraph)
                    {
                        g_allowDeactivate.store(true, std::memory_order_relaxed);
                        g_origGraphDeactivate(tpGraph, nullptr);
                        g_allowDeactivate.store(false, std::memory_order_relaxed);
                        LOG_INFO("[FPGraph] handing the 3P graph back DEACTIVATED so the engine's own activation runs in full");
                    }
                }
                g_forcedRevive.store(false, std::memory_order_relaxed);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_sehCount.fetch_add(1, std::memory_order_relaxed);
            }
        }

        const uint8_t r = g_origSetActiveGraph ? g_origSetActiveGraph(mgrV, newIdx) : 0;

        if (g_enabled.load(std::memory_order_relaxed) && newIdx == FIRST_PERSON_GRAPH
            && mgrV && g_populateVars
            && CoSyncPlayerAnimation::IsBoneReplicationEnabled() && CoSyncNet::IsConnected())
        {
            __try
            {
                auto* mgr = reinterpret_cast<uint8_t*>(mgrV);
                if (*reinterpret_cast<uint32_t*>(mgr + MGR_ARRAY_SIZE_OFF) == 2)
                {
                    void* tpGraph = GraphAt(mgr, THIRD_PERSON_GRAPH);
                    PlayerCharacter* player = CoSyncGetPlayer();
                    if (tpGraph && player)
                    {
                        void* holder = GetHolder(player);
                        void* gp = tpGraph;
                        g_populateVars(holder, &gp);
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_sehCount.fetch_add(1, std::memory_order_relaxed);
            }
        }

        return r;
    }

    // Keep the driven third-person graph from being deactivated on the way
    // INTO first person: deactivate resets its HKB state machine, and the
    // re-activation this module then performs would hand it back as a
    // newborn (T-pose) instead of a body that was already walking. Our own
    // undo path above calls the original directly with the allow-flag set,
    // so the trip back to third person still deactivates it properly.
    uint8_t __fastcall Detour_GraphDeactivate(void* graph, void* scrap)
    {
        if (graph && graph == g_drivenGraph
            && g_enabled.load(std::memory_order_relaxed)
            && !g_allowDeactivate.load(std::memory_order_relaxed)
            && CoSyncPlayerAnimation::IsBoneReplicationEnabled())
        {
            return 0;
        }

        // 2026-08-21 pt.8 -- the other half of kForceThirdPersonGraph.
        //
        // Blocking SetActiveGraph alone was not enough, and the measurement
        // said so loudly: with only that in place, first person froze the
        // WHOLE skeleton (488/488 witness samples byte-identical, arms as well
        // as legs) where previously at least the arms moved. Reason: entering
        // first person DEACTIVATES the third-person graph through this
        // separate path, and BShkbAnimationGraph::Update early-outs on
        // `cmp byte [wrapper+0x1AA], 0` -- a deactivated graph generates
        // nothing at all, no matter which index is "active".
        //
        // So the graph has to be kept both selected AND activated.
        if (graph && kPinTpGraphAcrossSwitch
            && graph == g_pinnedTpGraph.load(std::memory_order_relaxed)
            && g_enabled.load(std::memory_order_relaxed)
            && !g_allowDeactivate.load(std::memory_order_relaxed)
            && CoSyncPlayerAnimation::IsBoneReplicationEnabled()
            && CoSyncNet::IsConnected())
        {
            const auto n = g_deactBlocked.fetch_add(1, std::memory_order_relaxed);
            if (n == 0 || (n % 50) == 0)
                LOG_INFO("[FPGraph] blocked DEACTIVATE of the third-person graph (#%llu)",
                    static_cast<unsigned long long>(n));
            return 0;
        }
        return g_origGraphDeactivate ? g_origGraphDeactivate(graph, scrap) : 0;
    }

    // Suppress the engine's first-person -> third-person skeleton copy
    // while we are driving the third-person graph, so the pose just
    // generated survives to CaptureBones instead of being overwritten one
    // call later. The third-person body is APP_CULLED locally in first
    // person, so nothing is lost on this screen by skipping it.
    void __fastcall Detour_PcSkeletonSync(void* a1, void* a2)
    {
        static std::atomic<uint64_t> s_suppressed{0};
        static std::atomic<uint64_t> s_allowed{0};

        if (g_enabled.load(std::memory_order_relaxed)
            && (g_driveCount.load(std::memory_order_relaxed) > 0
                || g_engineTickCount.load(std::memory_order_relaxed) > 0)
            && CoSyncPlayerAnimation::IsBoneReplicationEnabled())
        {
            const auto n = s_suppressed.fetch_add(1, std::memory_order_relaxed);
            if (n == 0 || (n % 3600) == 0)
                LOG_INFO("[FPGraph] PC_SKELETON_SYNC SUPPRESSED (#%llu) -- 1P->3P bone copy skipped this frame",
                    static_cast<unsigned long long>(n));
            return; // skip the overwrite entirely
        }

        const auto n = s_allowed.fetch_add(1, std::memory_order_relaxed);
        if (n == 0 || (n % 3600) == 0)
            LOG_INFO("[FPGraph] PC_SKELETON_SYNC allowed through (#%llu) enabled=%d driveCount=%llu boneReplEnabled=%d",
                static_cast<unsigned long long>(n), g_enabled.load(std::memory_order_relaxed),
                static_cast<unsigned long long>(g_driveCount.load(std::memory_order_relaxed)),
                CoSyncPlayerAnimation::IsBoneReplicationEnabled() ? 1 : 0);
        if (g_origSkeletonSync) g_origSkeletonSync(a1, a2);
    }

    void __fastcall Detour_MgrUpdate(void* mgr, void* updateData)
    {
        // Engine first, untouched: the active graph (1P while the player is
        // in first person) gets its normal frame, so the local player's own
        // arms and weapon keep animating exactly as vanilla.
        if (mgr && CoSyncPlayerAnimation::IsBoneReplicationEnabled())
        {
            ProbePoseBuffers(mgr, "pre");
            LogCameraStateDiagOnChange();   // read-only; logs only on camera-state change
        }

        if (g_origMgrUpdate) g_origMgrUpdate(mgr, updateData);

        // Second, engine-driven tick of the PARKED third-person graph. See
        // kEngineTickParkedGraph. Runs after the normal update so the active
        // graph keeps first claim on the event queue.
        //
        // 2026-08-24 -- suppressed while bCaptureFirstPersonSkeleton is on.
        // The whole point of Experiment B was to make the PARKED third-person
        // graph produce a pose worth capturing; if the capture now reads the
        // first-person graph the engine is already ticking, there is nothing
        // left for this to feed. Running both at once is exactly the mistake
        // this file's own notes say cost several sessions ("running both
        // would make the result unreadable"), and it carries a known risk --
        // the manager's per-frame bookkeeping runs twice for the player, so
        // footsteps and weapon fire can double-fire. One experiment at a time.
        // Mode-aware: modes 1 and 2 read a first-person source and have no use
        // for the parked graph, so the re-tick is suppressed to keep one
        // experiment running at a time. Mode 3 reads the THIRD-person graph and
        // is meaningless without it -- suppressing it there would leave that
        // graph sitting at its reference pose and look exactly like failure.
        const int fpCaptureMode = CoSyncPlayerAnimation::GetFirstPersonCaptureMode();
        if (kEngineTickParkedGraph && mgr && updateData && g_origMgrUpdate
            && IsEngineTickEnabled()
            && (fpCaptureMode == 0 || fpCaptureMode == 3)
            && g_enabled.load(std::memory_order_relaxed)
            && CoSyncPlayerAnimation::IsBoneReplicationEnabled()
            && CoSyncNet::IsConnected())
        {
            EngineTickParkedGraph(mgr, updateData);
        }

        if (!mgr || !updateData) return;
        if (!g_enabled.load(std::memory_order_relaxed)) return;
        // Ini gate: [Animations] bDeltaPositionLocomotion=2 only.
        if (!CoSyncPlayerAnimation::IsBoneReplicationEnabled()) return;
        // Solo play costs nothing: without a peer there is no ghost to animate.
        if (!CoSyncNet::IsConnected()) return;

        // 2026-08-21 REMOVED: this used to also refresh a cached
        // "am I in first person" flag here from this SAME mgr/size/active
        // read, for ShouldCaptureNow()/IsSenderInFirstPerson() to consume
        // later. Proven broken by live diagnostic: Detour_MgrUpdate is a
        // MinHook detour on the SHARED engine function
        // BSAnimationGraphManager::Update, invoked for EVERY actor's own
        // graph manager (not just the local player's) -- the size==2 filter
        // below (unchanged, still correct, matches FO4_Wrld's own
        // detour_graph_mgr_update byte-for-byte) reliably identifies the
        // ONE call among many that's really the player's, but caching that
        // single call's result into a GLOBAL flag and reading it back from
        // an entirely different call site (ShouldCaptureNow, on a different
        // per-frame chain) meant every OTHER actor's manager update in
        // between (size==1, isFP=false) clobbered it back to false first.
        // A live test's own heartbeat sample proved this directly: four
        // 3-second-spaced samples returned FOUR DIFFERENT manager pointers,
        // and ShouldCaptureNow logged zero transitions all session despite
        // PC_SKELETON_SYNC independently confirming the sender DID enter
        // first person. See CoSyncPlayerAnimation-adjacent
        // project-cosync-bone-replication-fix memory for the full trace.
        // Replaced with IsLocalPlayerInFirstPerson() (below), a direct
        // PlayerCamera read -- inherently player-scoped (one singleton, no
        // cross-actor ambiguity possible), no caching needed. Ported from
        // FO4_Wrld's OWN real signal (engine_calls.cpp
        // local_player_in_first_person reads PlayerCamera+0x28 directly),
        // not from their detour's inline size==2 probe, which was only ever
        // used inline in their code too -- never cached/read elsewhere.

        // 2026-08-21 -- WRITE-LAST re-stamp for remote player proxies.
        //
        // This detour fires for EVERY actor's graph manager, post-original,
        // i.e. immediately after that actor's graph has written its generated
        // pose into the NiNodes (BSAnimationGraphManager::Update reaches the
        // pose writer at 0x01321030 internally). That makes it the correct --
        // and, since the animation update is not guaranteed to be the main
        // thread, the only reliable -- place to put CoSync's bone-replication
        // write so it lands LAST.
        //
        // Why it was needed: the write-order probe added to ApplySkeleton
        // measured the race directly on both machines. Of 55 applied bones,
        // ~25 were being overwritten by the engine before the frame was
        // drawn, and those 25 are precisely the ones the graph animates. The
        // apply itself stays where it is (main thread, F4SE task pump, where
        // the tier logic and smoothing state live); this only re-stamps the
        // finished matrices, so it cannot double-advance anything.
        //
        // Keyed by the graph's own write-target node 0 -- the "Root" bone --
        // because this path has a graph manager, not an Actor*. Cheap for
        // every non-proxy actor in the cell: one pointer read and one failed
        // hash lookup.
        ReassertProxyPose(mgr);

        // Cache the player's third-person graph and report the two gates that
        // decide whether it produces a pose at all: the graph's own activated
        // byte (hkb+0x1AA, which Update tests before generating anything) and
        // the manager's selected index. Both must hold for
        // kForceThirdPersonGraph to do what it claims.
        TrackPlayerThirdPersonGraph(mgr);

        // 2026-08-24 -- read the pose the graphs actually computed, not the
        // nodes they happened to write. Runs here because generatorOutput is
        // only live during the update. Log-only, rate-limited to 4 Hz, and it
        // touches nothing -- safe to leave in while it is answering the
        // question. Gated so it costs nothing outside bone-replication mode.
        if (mgr && CoSyncPlayerAnimation::IsBoneReplicationEnabled())
        {
            // Snapshot FIRST: generatorOutput is only valid inside the update,
            // and CaptureBones runs later from the player tick, where it is
            // already gone. Everything downstream reads the copy.
            SnapshotActiveGraph(mgr);
            ProbePoseBuffers(mgr, "post");
        }

        if (!kAttemptFirstPersonDrive) return;

        if (DriveThirdPersonGraph(mgr, updateData))
        {
            g_driveCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    bool InstallDetour(const char* name, uintptr_t moduleBase, uintptr_t rva, void* detour, void** original)
    {
        void* target = reinterpret_cast<void*>(moduleBase + rva);

        MH_STATUS st = MH_CreateHook(target, detour, original);
        if (st != MH_OK)
        {
            LOG_ERROR("[FPGraph] MH_CreateHook(%s) failed: %d (target=%p)", name, (int)st, target);
            return false;
        }
        st = MH_EnableHook(target);
        if (st != MH_OK)
        {
            LOG_ERROR("[FPGraph] MH_EnableHook(%s) failed: %d", name, (int)st);
            return false;
        }
        LOG_INFO("[FPGraph] %s hooked at base+0x%zX (%p)", name, rva, target);
        return true;
    }
}

namespace CoSyncFirstPersonGraph
{

bool Install(uintptr_t moduleBase)
{
    if (!moduleBase) return false;
    g_moduleBase.store(moduleBase, std::memory_order_relaxed);

    g_graphEventFlush = reinterpret_cast<GraphEventFlushFn>(moduleBase + RVA_GRAPH_EVENT_FLUSH);
    g_graphUpdate     = reinterpret_cast<GraphUpdateFn>(moduleBase + RVA_GRAPH_UPDATE);
    g_graphPoseApply  = reinterpret_cast<GraphPoseApplyFn>(moduleBase + RVA_GRAPH_POSE_APPLY);

    g_hkbCtxCtor   = reinterpret_cast<HkbCtxCtorFn  >(moduleBase + RVA_HKB_CTX_CTOR);
    g_hkbCtxDtor   = reinterpret_cast<HkbCtxDtorFn  >(moduleBase + RVA_HKB_CTX_DTOR);
    g_hkbUpdActive = reinterpret_cast<HkbUpdActiveFn>(moduleBase + RVA_HKB_UPD_ACTIVE);
    g_hkbUpdSync   = reinterpret_cast<HkbUpdSyncFn  >(moduleBase + RVA_HKB_UPD_SYNC);
    LOG_INFO("[FPGraph] HKB active-node refresh resolved: ctxCtor=0x%08X ctxDtor=0x%08X "
             "updActive=0x%08X updSync=0x%08X (1.11.240, +0x420 from 1.11.191)",
             (unsigned)RVA_HKB_CTX_CTOR, (unsigned)RVA_HKB_CTX_DTOR,
             (unsigned)RVA_HKB_UPD_ACTIVE, (unsigned)RVA_HKB_UPD_SYNC);
    g_graphActivate   = reinterpret_cast<GraphActivateFn>(moduleBase + RVA_GRAPH_ACTIVATE);
    g_graphSendEvent  = reinterpret_cast<GraphSendEventFn>(moduleBase + RVA_GRAPH_SEND_EVENT);
    g_populateVars    = reinterpret_cast<PopulateVarsFn>(moduleBase + RVA_PC_POPULATE_VARS);
    g_graphPhysPred   = reinterpret_cast<GraphPhysPredFn>(moduleBase + RVA_GRAPH_PHYS_PRED);
    g_graphActivatePrep = reinterpret_cast<GraphActivatePrepFn>(moduleBase + RVA_GRAPH_ACTIVATE_PREP);
    g_mgrActivateCommit = reinterpret_cast<MgrActivateCommitFn>(moduleBase + RVA_MGR_ACTIVATE_COMMIT);

    g_graphSetVarFloat = reinterpret_cast<GraphSetVarFloatFn>(moduleBase + RVA_GRAPH_SET_VAR_FLOAT);
    g_graphSetVarInt   = reinterpret_cast<GraphSetVarIntFn  >(moduleBase + RVA_GRAPH_SET_VAR_INT);

    __try
    {
        for (size_t i = 0; i < 6; ++i)
            g_locoVarStr[i] = BSFixedString(kLocoVarNames[i]);
        for (size_t i = 0; i < 5; ++i)
            g_locoIntStr[i] = BSFixedString(kLocoIntNames[i]);
        g_locoVarsReady = true;
        LOG_INFO("[FPGraph] locomotion hand-off %s: setter=0x%08X vars('%s','%s','%s')",
            kDriveLocomotionVars ? "ARMED" : "disabled (kill-switch)",
            (unsigned)RVA_GRAPH_SET_VAR_FLOAT,
            kLocoVarNames[0], kLocoVarNames[1], kLocoVarNames[2]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_locoVarsReady = false;
        LOG_WARN("[FPGraph] Failed to set up movement variable names -- hand-off disabled");
    }

    __try
    {
        for (size_t i = 0; i < 2; ++i)
            g_wakeEventStr[i] = BSFixedString(kWakeEvents[i]);
        g_wakeEventsReady = true;
        LOG_INFO("[FPGraph] set up wake-up event names ('%s', '%s')", kWakeEvents[0], kWakeEvents[1]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_wakeEventsReady = false;
        LOG_WARN("[FPGraph] could not set up wake-up event names -- the player's body double will need a manual action (e.g. drawing a weapon) to leave T-pose at spawn");
    }

    MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[FPGraph] MH_Initialize failed: %d", (int)mhInit);
        return false;
    }

    // Round 8 order (matches FO4_Wrld): deactivate guard before the update
    // hook, so g_origGraphDeactivate is valid before the drive can ever run.
    bool ok = true;
    ok &= InstallDetour("GraphDeactivate", moduleBase, RVA_GRAPH_DEACTIVATE,
        reinterpret_cast<void*>(&Detour_GraphDeactivate), reinterpret_cast<void**>(&g_origGraphDeactivate));

    ok &= InstallDetour("PC_SKELETON_SYNC", moduleBase, RVA_PC_SKELETON_SYNC,
        reinterpret_cast<void*>(&Detour_PcSkeletonSync), reinterpret_cast<void**>(&g_origSkeletonSync));

    // Secondary hooks: if either fails to install, the core drive still
    // runs -- it just keeps looping the parked state's clip instead of
    // reacting to new events / dropping the sticky-revive undo.
    InstallDetour("NotifyAnimationGraph", moduleBase, RVA_MGR_NOTIFY,
        reinterpret_cast<void*>(&Detour_NotifyAnimationGraph), reinterpret_cast<void**>(&g_origMgrNotify));

    InstallDetour("SetActiveGraph", moduleBase, RVA_SET_ACTIVE_GRAPH,
        reinterpret_cast<void*>(&Detour_SetActiveGraph), reinterpret_cast<void**>(&g_origSetActiveGraph));

    // THE decisive hook.
    ok &= InstallDetour("BSAnimationGraphManager::Update", moduleBase, RVA_MGR_UPDATE,
        reinterpret_cast<void*>(&Detour_MgrUpdate), reinterpret_cast<void**>(&g_origMgrUpdate));

    g_installed.store(ok, std::memory_order_relaxed);
    LOG_INFO("[FPGraph] install %s (gated behind [Animations] bDeltaPositionLocomotion=2)",
        ok ? "complete" : "FAILED on a required hook");
    return ok;
}

bool IsDriving() noexcept
{
    // "Driving" now means EITHER the hand-rolled drive OR the engine re-tick
    // (Experiment B) has run at least once -- ShouldCaptureNow() consults this
    // to keep capturing in first person, and with the manual drive off only
    // the engine-tick counter climbs.
    return g_installed.load(std::memory_order_relaxed)
        && g_enabled.load(std::memory_order_relaxed)
        && (g_driveCount.load(std::memory_order_relaxed) > 0
            || g_engineTickCount.load(std::memory_order_relaxed) > 0);
}

uint64_t GetDriveCount() noexcept { return g_driveCount.load(std::memory_order_relaxed); }
uint64_t GetSehCount() noexcept   { return g_sehCount.load(std::memory_order_relaxed); }

// 2026-08-21 -- reliable, player-scoped first-person signal. Replaces the
// removed g_inFirstPersonCached mechanism (see Detour_MgrUpdate's own
// comment for the full trace of why it was unreliable -- clobbered by
// every OTHER actor's graph-manager update in between the player's own
// update and the point this gets read). PlayerCamera is a genuine engine
// singleton: there is exactly one, it always belongs to the local player,
// so reading it directly has no cross-actor ambiguity to begin with -- no
// caching needed, cheap enough to call fresh every time.
//
// Uses ThirdParty/Pap/GameCamera.h's SDK-maintained PlayerCamera definition
// (g_playerCamera RelocPtr + PlayerCamera::GetCameraStateId), NOT a custom
// RVA -- GetCameraStateId is OUR OWN compiled code (a plain pointer-identity
// scan of PlayerCamera::cameraStates[], GameCamera.cpp), so this needs no
// vtable/RTTI comparison and no new engine offset beyond what the SDK
// already resolves. Mirrors the INTENT of FO4_Wrld's own real signal
// (engine_calls.cpp local_player_in_first_person, which reads
// PlayerCamera+0x28 and compares vtables) without reusing any of their
// numeric RVAs.
bool IsLocalPlayerInFirstPerson() noexcept
{
    __try
    {
        PlayerCamera* cam = *g_playerCamera;

        // One-shot sanity log -- this codebase has a documented precedent
        // (GameGlobals.cpp's g_player) of an SDK RelocPtr going stale after
        // an engine update and silently reading null forever. Cheap enough
        // to check every call; only LOGS once so it can't spam.
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true, std::memory_order_relaxed))
        {
            LOG_INFO("[FPGraph] IsLocalPlayerInFirstPerson: g_playerCamera=%p cameraState=%p stateId=%d",
                static_cast<void*>(cam),
                cam ? static_cast<void*>(cam->cameraState) : nullptr,
                (cam && cam->cameraState) ? cam->GetCameraStateId(cam->cameraState) : -1);
        }

        if (!cam || !cam->cameraState) return false;
        return cam->GetCameraStateId(cam->cameraState) == PlayerCamera::kCameraState_FirstPerson;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool IsSenderInFirstPerson() noexcept
{
    return IsLocalPlayerInFirstPerson();
}

// Ported from FO4_Wrld's own shipped gate (scene_inject.cpp): capture always
// in third person; in first person, only once the drive is confirmed
// actually running. See the header comment for the full rationale.
int GetPoseSnapshot(PoseSnapshotBone* out, int maxBones) noexcept
{
    if (!out || maxBones <= 0) return 0;
    const int n = g_snapCount.load(std::memory_order_acquire);
    if (n <= 0) return 0;
    const int copy = (n < maxBones) ? n : maxBones;
    for (int i = 0; i < copy; ++i) out[i] = g_snapBones[i];
    return copy;
}

int GetThirdPersonPoseSnapshot(PoseSnapshotBone* out, int maxBones) noexcept
{
    if (!out || maxBones <= 0) return 0;
    const int n = g_snapTpCount.load(std::memory_order_acquire);
    if (n <= 0) return 0;
    const int copy = (n < maxBones) ? n : maxBones;
    for (int i = 0; i < copy; ++i) out[i] = g_snapTpBones[i];
    return copy;
}

bool PoseSnapshotWasFirstPerson() noexcept
{
    return g_snapWasFp.load(std::memory_order_relaxed);
}

uint64_t PoseSnapshotAge() noexcept
{
    return g_snapFrame.load(std::memory_order_relaxed);
}

bool ShouldCaptureNow() noexcept
{
    const bool boneReplOn = CoSyncPlayerAnimation::IsBoneReplicationEnabled();
    const bool connected  = CoSyncNet::IsConnected();
    const bool inFP       = IsLocalPlayerInFirstPerson();
    const bool driving    = IsDriving();

    bool result;
    if (!boneReplOn || !connected)
        result = true; // this gate only applies to the bone-replication drive path
    else if (!inFP)
        result = true; // third person: always capture, unchanged from before
    else if (CoSyncPlayerAnimation::IsFirstPersonCaptureEnabled())
        // 2026-08-24 -- first person now captures the FIRST-person skeleton,
        // which the engine ticks itself every frame. There is no drive to wait
        // on and nothing to hold: this gate exists purely to avoid shipping a
        // stub pose off a parked graph, and we are no longer reading one.
        result = true;
    else if (g_snapCount.load(std::memory_order_acquire) > 0)
        // 2026-08-24 pt.3 -- a generated-pose snapshot exists, so the premise
        // behind the `driving` gate no longer holds.
        //
        // That gate asks "is the parked third-person graph being driven?",
        // because the only pose worth capturing in first person used to come
        // from reviving that graph. It is wired to the drive/engine-tick
        // counters, which meant turning the engine re-tick OFF (to isolate the
        // pose probe) also silently stopped CaptureBones from running at all
        // -- measured: 816 [pose] samples and ZERO CaptureBones calls in the
        // same session, so bone replication was entirely dead in first person
        // for that run and the pose-check never had a chance to fire.
        //
        // The snapshot does not care whether anything is driving the parked
        // graph; it comes off whichever graph the engine is ticking anyway.
        // Having one is sufficient reason to capture.
        result = true;
    else
        result = driving;

    // 2026-08-20 diagnostic, CORRECTED: the first version of this logged on
    // a 3s timer -- a live test came back with inFP=0 on every single
    // sample, but with only ~11-15 samples over the whole session that's
    // just as likely to mean "sparse periodic sampling missed every brief
    // first-person window" as "the gate is broken". Switched to logging on
    // every TRANSITION (inFP changing value) instead, which cannot miss a
    // window regardless of how short it is, plus a slower heartbeat while
    // continuously in first person (matching FO4_Wrld's own [pose-1p]
    // diagnostic pattern: log on every camera transition, and periodically
    // while it stays in first person).
    static bool s_prevInFP = false;
    static uint64_t s_nextHeartbeatMs = 0;
    const uint64_t nowMs = GetTickCount64();
    const bool transitioned = (inFP != s_prevInFP);
    if (transitioned || (inFP && nowMs >= s_nextHeartbeatMs))
    {
        s_prevInFP = inFP;
        s_nextHeartbeatMs = nowMs + 3000;
        LOG_INFO("[FPGraph] ShouldCaptureNow%s: boneReplOn=%d connected=%d inFP=%d driving=%d -> %s",
            transitioned ? " (TRANSITION)" : "",
            boneReplOn, connected, inFP, driving, result ? "CAPTURE" : "HOLD");
    }
    return result;
}

}
