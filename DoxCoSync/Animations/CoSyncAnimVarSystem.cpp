

#include "CoSyncAnimVarSystem.h"
#include "CoSyncBehaviorGraph.h"
#include "CoSyncCreatureAnimSync.h"
#include "CoSyncAnimationUtils.h"
#include "CoSyncPowerArmorSync.h"
#include "ConsoleLogger.h"
#include "GameReferences.h"
#include "GameFormComponents.h"
#include "GameTypes.h"
#include "BSAnimationGraphManager.h"
#include "BShkbAnimationGraph.h"
#include "hkbBehaviorGraph.h"
#include "hkbVariableValueSet.h"
#include "CoSyncRequirementsLog.h"

#include <cstring>
#include <vector>

extern RelocPtr<PlayerCharacter*> g_player;

// Round 117: GetVariableSetDirect/GetVariableSetForWrite/
// PatchLocalPlayerSpeedFromFP all dispatch through the exact same actor+0x48
// holder vtable slot 4 (GetBSAnimGraph) the engine's own NpcAnimSync uses
// internally to fetch the active graph manager -- the closest CoSync-side
// analog to that call. None of these were breadcrumbed before now.
extern volatile const char* g_lastTickStep;

namespace TP
{

    enum Variables : uint32_t
    {
        kfSpeedWalk = 0,
        kfSpeedRun = 1,
        kDirection = 2,
        kiSyncIdleLocomotion = 3,
        kiState = 8,
        km_bEnablePitchTwistModifier = 10,
        kPitchOffset = 11,
        kPitch = 21,
        kiSyncSprintState = 22,
        kIsSprinting = 23,
        kTurnDelta = 24,
        kisFiring = 28,
        kisReloading = 31,
        kiAttackState = 36,
        kIsAttackReady = 37,
        kTurnDeltaSmoothed = 39,
        kDirectionDamped = 42,
        kbAnimationDriven = 46,
        kisAttackNotReady = 47,
        kVelocityZ = 48,
        kDirectionSmoothed = 49,
        kAimStability = 50,
        kiIsInSneak = 51,
        kisJumping = 52,
        kbAllowRotation = 53,
        kSpeed = 54,
        kbEquipOk = 55,
        kbAimActive = 65,
        kAimHeadingCurrent = 66,
        kAimPitchCurrent = 67,
        kIsNPC = 268,
        kSampledSpeed = 71,
        kiSyncTurnState = 72,
        kbInJumpState = 79,
        kIsStaggering = 81,
        kstaggerMagnitude = 82,
        kiGetUpType = 83,
        kIsSneaking = 85,
        kisMirrored = 86,
        kSpineTwist = 6,
        kiState_Raider_Stumble_Rifle = 105,
        kbNotHeadTrack = 106,
        kbCCSupport = 109,
        kbCCOnStairs = 110,
        kbAllowHeadTracking = 112,
        kbGraphDriven = 113,
        kbGraphDrivenTranslation = 114,
        kbGraphDrivenRotation = 115,
        kiState_NPCSneaking = 118,
        kiState_PlayerDefault = 163,
        kiState_NPCMelee = 167,
        kbGraphWantsFootIK = 171,
        kfik_footplantedgain = 176,
        kIsPlayer = 184,
        kiState_NPCGun = 185,
        kbIsFemale = 186,
        kiState_PlayerMelee = 189,
        kiState_NPCFastWalk = 190,
        kbFreezeSpeedUpdate = 192,
        kbInLandingState = 194,
        kbEnableRoot_IsActiveMod = 195,
        kWalkSpeedMult = 197,
        kJogSpeedMult = 198,
        krunSpeedMult = 199,
        kiSyncLocomotionSpeed = 200,
        kiControlsIdleSync = 201,
        kbPathingInterruptibleIdle = 202,
        kiIsPlayer = 204,
        kSpeedSmoothed = 205,
        kLeftHandIKOn = 207,
        kbIsThrowing = 208,
        kiSyncWalkRun = 209,
        kiSyncJumpState = 211,
        kEnable_bEquipOK = 213,
        kbIsInMT = 216,
        kbFreezeRotationUpdate = 218,
        kDirectionDegrees = 220,
        kiState_NPCBlocking = 222,
        kiSyncForwardBackward = 223,
        kIsBlocking = 224,
        kfDirectAtSavedGain = 225,
        kiSyncForwardBackward00 = 228,
        kfLocomotionWalkPlaybackSpeed = 233,
        kfLocomotionJogPlaybackSpeed = 234,
        kfLocomotionRunPlaybackSpeed = 235,
        kiLocomotionSpeedState = 238,
        kbUseRifleReadyDirectAt = 239,
        kiMeleeState = 240,
        kbEquipOkIsActiveEnabled = 242,
        kiSyncSneakWalkRun = 243,
        kiSyncDirection = 244,
        kfLocomotionSneakWalkPlaybackSpeed = 245,
        kfLocomotionSneakRunPlaybackSpeed = 246,
        kweaponSpeedMult = 248,
        kbIsSneaking = 249,
        kbDisableAttackReady = 251,
        kbSyncDirection = 252,
        kbActorMobilityNotFullyCrippled = 253,
        kbRootRifleEquipOk = 254,
        kbAnimateWeaponBones = 255,
        kbForceIdleStop = 257,
        kiSyncShuffleState = 258,
        kiSyncSightedState = 262,
        kiWeaponChargeMode = 263,
        kReloadSpeedMult = 265,
        kiSyncReadyAlertRelaxed = 266,
        kbUseLeftHandIKDefaults = 270,
        kiSyncGunDown = 271,
        kbAimEnabled = 273,
        kbAimCaptureEnabled = 276,
        kCurrentJumpState = 285,
        kbDisableSpineTracking = 287,
        kfPlaybackMult = 295,
        kbIsInFlavor = 296,
        kiTalkGenerator = 297,
        kstaggerDirection = 298,
    };
}

static const uint32_t kBooleanLookupTable[] = {
    TP::km_bEnablePitchTwistModifier,
    TP::kIsSprinting,
    TP::kisFiring,
    TP::kisReloading,
    TP::kIsAttackReady,
    TP::kisAttackNotReady,
    TP::kiIsInSneak,
    TP::kisJumping,
    TP::kbEquipOk,
    TP::kbInJumpState,
    TP::kIsStaggering,
    TP::kIsSneaking,
    TP::kisMirrored,
    TP::kbNotHeadTrack,
    TP::kbCCSupport,
    TP::kbCCOnStairs,
    TP::kbGraphDriven,
    TP::kbGraphDrivenTranslation,
    TP::kbGraphDrivenRotation,
    TP::kbGraphWantsFootIK,
    TP::kbIsFemale,
    TP::kbIsThrowing,
    TP::kEnable_bEquipOK,
    TP::kIsBlocking,
    TP::kbUseRifleReadyDirectAt,
    TP::kbEquipOkIsActiveEnabled,
    TP::kbIsSneaking,
    TP::kbAimEnabled,
    TP::kbForceIdleStop,
    TP::kbActorMobilityNotFullyCrippled,
    TP::kbSyncDirection,
    TP::kbDisableAttackReady,
    TP::kbAllowHeadTracking,
    TP::kbInLandingState,
    TP::kbIsInFlavor,
    TP::kbAimActive,
    TP::kbAllowRotation,
    TP::kbUseLeftHandIKDefaults,
    TP::kLeftHandIKOn,
    TP::kbEnableRoot_IsActiveMod,
    TP::kbIsInMT,
    TP::kbRootRifleEquipOk,
    TP::kbAimCaptureEnabled,
    TP::kbDisableSpineTracking,
    TP::kIsNPC,
    TP::kIsPlayer,
    TP::kbFreezeSpeedUpdate,
    TP::kbFreezeRotationUpdate,
    TP::kbAnimationDriven,
};
static constexpr int kNumBooleans = 49;

static const uint32_t kFloatLookupTable[] = {
    TP::kDirection,
    TP::kfSpeedRun,
    TP::kfSpeedWalk,
    TP::kSpineTwist,
    TP::kSpeed,
    TP::kPitchOffset,
    TP::kPitch,
    TP::kTurnDelta,
    TP::kDirectionSmoothed,
    TP::kAimStability,
    TP::kSampledSpeed,
    TP::kSpeedSmoothed,
    TP::kReloadSpeedMult,
    TP::kTurnDeltaSmoothed,
    TP::kWalkSpeedMult,
    TP::krunSpeedMult,
    TP::kDirectionDegrees,
    TP::kJogSpeedMult,
    TP::kweaponSpeedMult,
    TP::kfLocomotionWalkPlaybackSpeed,
    TP::kfLocomotionJogPlaybackSpeed,
    TP::kfLocomotionRunPlaybackSpeed,
    TP::kfLocomotionSneakRunPlaybackSpeed,
    TP::kfLocomotionSneakWalkPlaybackSpeed,
    TP::kfik_footplantedgain,
    TP::kVelocityZ,
    TP::kAimHeadingCurrent,
    TP::kAimPitchCurrent,
    TP::kfDirectAtSavedGain,
    TP::kfPlaybackMult,
    TP::kDirectionDamped,
    TP::kstaggerMagnitude,
};
static constexpr int kNumFloats = 32;

static const uint32_t kIntegerLookupTable[] = {
    TP::kiState,
    TP::kiSyncSprintState,
    TP::kiWeaponChargeMode,
    TP::kiAttackState,
    TP::kiGetUpType,
    TP::kiState_Raider_Stumble_Rifle,
    TP::kiState_NPCSneaking,
    TP::kiState_PlayerDefault,
    TP::kiState_NPCMelee,
    TP::kiState_NPCGun,
    TP::kiState_PlayerMelee,
    UINT32_MAX,
    TP::kiControlsIdleSync,
    TP::kiSyncWalkRun,
    TP::kiState_NPCBlocking,
    TP::kiLocomotionSpeedState,
    TP::kiMeleeState,
    TP::kCurrentJumpState,
    TP::kiSyncTurnState,
    TP::kbPathingInterruptibleIdle,
    TP::kiSyncLocomotionSpeed,
    TP::kiSyncShuffleState,
    TP::kiSyncSneakWalkRun,
    TP::kiSyncDirection,
    TP::kiSyncForwardBackward00,
    TP::kiSyncForwardBackward,
    TP::kiSyncIdleLocomotion,
    TP::kiSyncJumpState,
    TP::kiSyncReadyAlertRelaxed,
    TP::kiSyncSightedState,
    TP::kiSyncGunDown,
    TP::kiTalkGenerator,
    TP::kstaggerDirection,
};
static constexpr int kNumIntegers = 33;

struct VariableSetResult
{
    uint32_t* data;
    uint32_t  size;
    // round 23: manager is the live BSAnimationGraphManager reference this
    // data/size pair points into. GetVariableSetDirect/GetVariableSetForWrite
    // no longer Release() it themselves on success -- BSAnimationGraphManager
    // ::Release() (see BsAnimationGraphManager.h) is a REAL refcount
    // decrement that destructs the manager (and everything it owns,
    // including this data pointer) at zero. Releasing before the caller is
    // done reading/writing data is a genuine use-after-free: for a freshly
    // registered actor (first time this exact code path runs for it -- e.g.
    // the first world NPC ever processed in a session) our reference can
    // easily be the last one, so the manager is destructed immediately, and
    // every subsequent varSet.data[idx] write/read corrupts already-freed
    // memory. A write into freed-but-still-mapped memory doesn't fault
    // (so __except can't catch it) -- it just corrupts the heap, which
    // surfaces later as a heap-corruption fail-fast (RaiseFailFastException),
    // a kill that bypasses SEH/VEH entirely: a silent crash with zero trace.
    // Every caller MUST call manager->Release() itself exactly once, after
    // the last point it touches data, on every exit path (including early
    // returns) -- not via an RAII destructor, since this project's
    // /GL+/LTCG build has repeatedly shown SEH (__try/__except) and C++
    // object destructors in the same function are unreliable together.
    BSAnimationGraphManager* manager = nullptr;
};

// UPDATE 2026-06-17 (round 13): real animation behavior graphs have on the
// order of 50-300 named variables (kNumBooleans=49/kNumFloats=32/
// kNumIntegers=33 above, kAnimVarCheckTable ~116 entries). If
// animationVariables is read while the graph is still mid-construction
// (confirmed possible now that the NpcAnimSync_Hook RVA fix lets this code
// run for the first time ever this session, during world load, for the
// player's own graph) `size` can come back as raw uninitialized memory --
// not zero (already rejected below), just absurdly large. Unbounded, that
// flows into `std::vector<uint32_t> snapshot(varSet.data, varSet.data +
// varSet.size)` in VerifyVariableIndices, which throws std::bad_alloc for a
// multi-gigabyte request -- an uncaught C++ exception, not a Win32 SEH
// access violation, so it never reaches CoSync_VEH or any __except here: it
// goes straight to std::terminate()/abort(), a silent crash with no log line
// at all. This generous bound (10x the largest real table above) rejects
// that case the same way the existing size==0 check rejects an empty one.
static constexpr uint32_t kMaxSaneVariableSetSize = 4096;

// 2026-08-21 -- noinline added after a live crash traced through a fresh
// PDB-symbolized VEH stack (DoxCoSync.dll+0x2A8F3/+0x284BA area) landed
// inside this function's own call chain during the first live test of the
// re-enabled first-person drive + the new ClearParkedGraphFreezeFlags call
// path (both new this session, both exercise this and GetVariableSetForWrite
// below far more heavily -- and on the rapidly camera-switching LOCAL
// PLAYER, not just settled NPC puppets -- than either function had ever
// been exercised before). Neither this function nor GetVariableSetForWrite
// had __declspec(noinline) despite both being small `static` functions
// wrapping a raw vtable dispatch in __try/__except -- EXACTLY the failure
// mode already found and fixed once in CoSyncPlayerAnimation.cpp (see
// [[project-cosync-fp-graph-drive-rvas]] pt.20 / SafeReadSkelRoot's own
// comment there): MSVC's /GL+/LTCG can inline a small SEH-wrapped helper
// into its callers and silently drop the SEH scope table, so the guard
// stops actually catching anything. That lesson was never applied to this
// file. Fixed here on both functions.
__declspec(noinline) static bool GetVariableSetDirect(Actor* actor, VariableSetResult& out)
{
    LOG_REQUIREMENT("GetVariableSetDirect", "Needs a valid actor with a working animation graph. Reads its animation variables directly; guarded so a bad graph can't crash the game.");
    out.data = nullptr;
    out.size = 0;

    if (!actor) return false;

    // Round 118/119: never touch the graph manager while the actor's 3D is
    // not loaded AND settled (see matching gate in GetVariableSetForWrite).
    if (!CoSyncAnimUtils::IsGraphReady(actor)) return false;

    __try
    {

        TESObjectREFR* refr = static_cast<TESObjectREFR*>(actor);
        void* animHolder = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(refr) + 0x48);

        void*** vtable = reinterpret_cast<void***>(animHolder);
        if (!vtable || !*vtable) return false;

        typedef bool(__thiscall* GetBSAnimGraphFn)(void*, BSAnimationGraphManager**);
        GetBSAnimGraphFn getBSAnimGraph = reinterpret_cast<GetBSAnimGraphFn>((*vtable)[4]);

        g_lastTickStep = "AnimVarSystem:GetVariableSetDirect:pre-GetBSAnimGraph";
        BSAnimationGraphManager* pManager = nullptr;
        if (!getBSAnimGraph(animHolder, &pManager) || !pManager)
            return false;
        g_lastTickStep = "AnimVarSystem:GetVariableSetDirect:post-GetBSAnimGraph";

        // FIXED round 96: this used to branch on formID (player -> graph[0],
        // everyone else -> graph[animationGraphIndex]), the same bug already
        // found and fixed in GetVariableSetForWrite below ("CONFIRMED BUG:
        // animationGraphIndex != 0 for proxy" -- that fix forces graph[0]
        // unconditionally for every actor type, since a proxy's
        // animationGraphIndex can point at its invisible FP graph instead of
        // the TP graph everyone else actually sees it through). This read
        // path was never updated to match, and it's NOT a dead/diagnostic
        // path: CoSyncPlayer::TickAnimVars's no-network-snapshot fallback
        // calls SaveAnimationVariables (-> this function) directly on the
        // remote proxy actor, then carries the result into
        // LoadAnimationVariables (-> GetVariableSetForWrite, graph[0]).
        // Reading the wrong graph here while writing the right one there
        // means every snapshot field NOT explicitly overwritten by that
        // fallback (i.e. everything except speed/pitch/locomotion state)
        // got carried from the wrong graph instance into the right one,
        // clobbering graph[0]'s real values with stale/unrelated data from
        // graph[animationGraphIndex] whenever that index was nonzero.
        const BShkbAnimationGraph* pGraph = pManager->animationGraphs.Get(0);

        if (!pGraph)
        {
            pManager->Release();
            return false;
        }

        if (!pGraph->behaviorGraph)
        {
            pManager->Release();
            return false;
        }

        auto* pVariableSet = pGraph->behaviorGraph->animationVariables;
        if (!pVariableSet || !pVariableSet->data || pVariableSet->size == 0 ||
            pVariableSet->size > kMaxSaneVariableSetSize)
        {
            pManager->Release();
            return false;
        }

        out.data = pVariableSet->data;
        out.size = pVariableSet->size;

        // round 23: do NOT Release() here -- out.data points into memory
        // owned by pManager, and Release() can destruct it immediately (see
        // VariableSetResult::manager comment above). Hand the live reference
        // to the caller instead; the caller now owns exactly one Release()
        // call, to be made after its last use of out.data/out.size.
        out.manager = pManager;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


// See GetVariableSetDirect's own comment above -- same noinline fix, same reason.
__declspec(noinline) static bool GetVariableSetForWrite(Actor* actor, VariableSetResult& out)
{
    LOG_REQUIREMENT("GetVariableSetForWrite", "Needs a valid actor with a working animation graph. Opens its animation variables for writing; guarded so a bad graph can't crash the game.");

    out.data = nullptr;
    out.size = 0;

    if (!actor) return false;

    // Round 118: the first anim-var write to a freshly spawned remote proxy
    // (3D not yet loaded: loadedData=0/rootNode=0 in SpawnDiag) hard-froze
    // the main thread with no fault/VEH/WER — consistent with spinning on a
    // mid-construction graph lock. Defer all graph access until the engine
    // has loaded the actor's 3D; callers already treat false as "skip this
    // tick" and will retry next tick. Throttled log doubles as a watchdog:
    // if this keeps printing for the same actor, its 3D never loads (the
    // "invisible NPC" symptom) and spawn-side 3D loading needs forcing.
    // Round 119: upgraded from Is3DLoaded to IsGraphReady — the 02:36 test
    // froze at the first engine SetInt 0.5s AFTER 3D loaded (loader thread
    // still owned the graph-manager spin lock); a grace period after 3D load
    // rides that out.
    if (!CoSyncAnimUtils::IsGraphReady(actor))
    {
        static uint32_t s_no3dLogCount = 0;
        if ((s_no3dLogCount++ % 200) == 0)
            LOG_WARN("[AnimVarSystem] Holding off on animation writes: actor=0x%08X isn't ready yet (seen #%u times)",
                actor->formID, s_no3dLogCount);
        return false;
    }

    __try
    {
        TESObjectREFR* refr = static_cast<TESObjectREFR*>(actor);
        void* animHolder = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(refr) + 0x48);

        void*** vtable = reinterpret_cast<void***>(animHolder);
        if (!vtable || !*vtable) return false;

        typedef bool(__thiscall* GetBSAnimGraphFn)(void*, BSAnimationGraphManager**);
        GetBSAnimGraphFn getBSAnimGraph = reinterpret_cast<GetBSAnimGraphFn>((*vtable)[4]);

        g_lastTickStep = "AnimVarSystem:GetVariableSetForWrite:pre-GetBSAnimGraph";
        BSAnimationGraphManager* pManager = nullptr;
        if (!getBSAnimGraph(animHolder, &pManager) || !pManager)
            return false;
        g_lastTickStep = "AnimVarSystem:GetVariableSetForWrite:post-GetBSAnimGraph";

        {
            static bool s_graphDiagDone = false;
            if (!s_graphDiagDone && actor->formID != 0x14)
            {
                s_graphDiagDone = true;
                const uint32_t graphCount = pManager->animationGraphs.size;
                const uint32_t activeIdx  = pManager->animationGraphIndex;
                LOG_INFO("[AnimVarSystem] Graph check actor=0x%08X: graphCount=%u activeIdx=%u"
                    " — writing to graph 0",
                    actor->formID, graphCount, activeIdx);
                if (activeIdx != 0)
                    LOG_WARN("[AnimVarSystem] This proxy's active graph index was %u instead of 0!"
                        " Earlier writes were going to the wrong (invisible) graph — now forcing graph 0.",
                        activeIdx);
            }
        }


        const BShkbAnimationGraph* pGraph = pManager->animationGraphs.Get(0);

        if (!pGraph || !pGraph->behaviorGraph)
        {
            pManager->Release();
            return false;
        }

        auto* pVariableSet = pGraph->behaviorGraph->animationVariables;
        if (!pVariableSet || !pVariableSet->data || pVariableSet->size == 0 ||
            pVariableSet->size > kMaxSaneVariableSetSize)
        {
            pManager->Release();
            return false;
        }

        out.data = pVariableSet->data;
        out.size = pVariableSet->size;

        // round 23: see matching comment in GetVariableSetDirect -- caller
        // now owns the single Release() call for this manager.
        out.manager = pManager;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// See header comment (CoSyncAnimVarSystem.h). noinline: raw pointer writes
// under __try, same rule as every other SEH helper in this codebase (a
// small function's __try/__except silently stops catching if the optimizer
// inlines it into its caller and drops the SEH scope table). Out-of-line
// qualified definition, matching this file's own established pattern for
// every other CoSyncAnimVarSystem:: public function (IsGraphReady,
// SaveAnimationVariables, LoadAnimationVariables all follow this same
// `ReturnType CoSyncAnimVarSystem::Name(...)` form rather than a nested
// `namespace CoSyncAnimVarSystem { }` block).
__declspec(noinline) void CoSyncAnimVarSystem::ClearParkedGraphFreezeFlags(Actor* actor) noexcept
{
    VariableSetResult varSet{};
    if (!GetVariableSetForWrite(actor, varSet))
        return;

    __try
    {
        if (TP::kbAnimationDriven        < varSet.size) varSet.data[TP::kbAnimationDriven]        = 0;
        if (TP::kbAllowRotation          < varSet.size) varSet.data[TP::kbAllowRotation]          = 0;
        if (TP::kbGraphDrivenTranslation < varSet.size) varSet.data[TP::kbGraphDrivenTranslation] = 0;
        if (TP::kbGraphDrivenRotation    < varSet.size) varSet.data[TP::kbGraphDrivenRotation]    = 0;
        if (TP::kbFreezeSpeedUpdate      < varSet.size) varSet.data[TP::kbFreezeSpeedUpdate]      = 0;
        if (TP::kbFreezeRotationUpdate   < varSet.size) varSet.data[TP::kbFreezeRotationUpdate]   = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (varSet.manager) varSet.manager->Release();
}

enum class AnimVarKind { Bool, Float, Int };

struct AnimVarCheckEntry
{
    const char* name;
    uint32_t    expectedIndex;
    AnimVarKind kind;
};

// Generated from the TP::Variables enum above (script-derived, not hand
// transcribed, to avoid errors across 116 entries) by stripping the leading
// 'k' -- a convention independently confirmed against ~50 already-in-use
// string literals elsewhere in this file and in CoSyncCreatureAnimSync.cpp
// (kiState -> "iState", km_bEnablePitchTwistModifier ->
// "m_bEnablePitchTwistModifier", kIsNPC -> "IsNPC", all exact matches to
// production call sites). Kind (Bool/Float/Int) comes from whichever of the
// three kXxxLookupTable arrays above each entry actually appears in -- the
// real, already-used type -- falling back to name-prefix convention only for
// the handful of entries not present in any of those tables.
static const AnimVarCheckEntry kAnimVarCheckTable[] = {
    { "fSpeedWalk", TP::kfSpeedWalk, AnimVarKind::Float },
    { "fSpeedRun", TP::kfSpeedRun, AnimVarKind::Float },
    { "Direction", TP::kDirection, AnimVarKind::Float },
    { "iSyncIdleLocomotion", TP::kiSyncIdleLocomotion, AnimVarKind::Int },
    { "iState", TP::kiState, AnimVarKind::Int },
    { "m_bEnablePitchTwistModifier", TP::km_bEnablePitchTwistModifier, AnimVarKind::Bool },
    { "PitchOffset", TP::kPitchOffset, AnimVarKind::Float },
    { "Pitch", TP::kPitch, AnimVarKind::Float },
    { "iSyncSprintState", TP::kiSyncSprintState, AnimVarKind::Int },
    { "IsSprinting", TP::kIsSprinting, AnimVarKind::Bool },
    { "TurnDelta", TP::kTurnDelta, AnimVarKind::Float },
    { "isFiring", TP::kisFiring, AnimVarKind::Bool },
    { "isReloading", TP::kisReloading, AnimVarKind::Bool },
    { "iAttackState", TP::kiAttackState, AnimVarKind::Int },
    { "IsAttackReady", TP::kIsAttackReady, AnimVarKind::Bool },
    { "TurnDeltaSmoothed", TP::kTurnDeltaSmoothed, AnimVarKind::Float },
    { "DirectionDamped", TP::kDirectionDamped, AnimVarKind::Float },
    { "bAnimationDriven", TP::kbAnimationDriven, AnimVarKind::Bool },
    { "isAttackNotReady", TP::kisAttackNotReady, AnimVarKind::Bool },
    { "VelocityZ", TP::kVelocityZ, AnimVarKind::Float },
    { "DirectionSmoothed", TP::kDirectionSmoothed, AnimVarKind::Float },
    { "AimStability", TP::kAimStability, AnimVarKind::Float },
    { "iIsInSneak", TP::kiIsInSneak, AnimVarKind::Bool },
    { "isJumping", TP::kisJumping, AnimVarKind::Bool },
    { "bAllowRotation", TP::kbAllowRotation, AnimVarKind::Bool },
    { "Speed", TP::kSpeed, AnimVarKind::Float },
    { "bEquipOk", TP::kbEquipOk, AnimVarKind::Bool },
    { "bAimActive", TP::kbAimActive, AnimVarKind::Bool },
    { "AimHeadingCurrent", TP::kAimHeadingCurrent, AnimVarKind::Float },
    { "AimPitchCurrent", TP::kAimPitchCurrent, AnimVarKind::Float },
    { "IsNPC", TP::kIsNPC, AnimVarKind::Bool },
    { "SampledSpeed", TP::kSampledSpeed, AnimVarKind::Float },
    { "iSyncTurnState", TP::kiSyncTurnState, AnimVarKind::Int },
    { "bInJumpState", TP::kbInJumpState, AnimVarKind::Bool },
    { "IsStaggering", TP::kIsStaggering, AnimVarKind::Bool },
    { "staggerMagnitude", TP::kstaggerMagnitude, AnimVarKind::Float },
    { "iGetUpType", TP::kiGetUpType, AnimVarKind::Int },
    { "IsSneaking", TP::kIsSneaking, AnimVarKind::Bool },
    { "isMirrored", TP::kisMirrored, AnimVarKind::Bool },
    { "SpineTwist", TP::kSpineTwist, AnimVarKind::Float },
    { "iState_Raider_Stumble_Rifle", TP::kiState_Raider_Stumble_Rifle, AnimVarKind::Int },
    { "bNotHeadTrack", TP::kbNotHeadTrack, AnimVarKind::Bool },
    { "bCCSupport", TP::kbCCSupport, AnimVarKind::Bool },
    { "bCCOnStairs", TP::kbCCOnStairs, AnimVarKind::Bool },
    { "bAllowHeadTracking", TP::kbAllowHeadTracking, AnimVarKind::Bool },
    { "bGraphDriven", TP::kbGraphDriven, AnimVarKind::Bool },
    { "bGraphDrivenTranslation", TP::kbGraphDrivenTranslation, AnimVarKind::Bool },
    { "bGraphDrivenRotation", TP::kbGraphDrivenRotation, AnimVarKind::Bool },
    { "iState_NPCSneaking", TP::kiState_NPCSneaking, AnimVarKind::Int },
    { "iState_PlayerDefault", TP::kiState_PlayerDefault, AnimVarKind::Int },
    { "iState_NPCMelee", TP::kiState_NPCMelee, AnimVarKind::Int },
    { "bGraphWantsFootIK", TP::kbGraphWantsFootIK, AnimVarKind::Bool },
    { "fik_footplantedgain", TP::kfik_footplantedgain, AnimVarKind::Float },
    { "IsPlayer", TP::kIsPlayer, AnimVarKind::Bool },
    { "iState_NPCGun", TP::kiState_NPCGun, AnimVarKind::Int },
    { "bIsFemale", TP::kbIsFemale, AnimVarKind::Bool },
    { "iState_PlayerMelee", TP::kiState_PlayerMelee, AnimVarKind::Int },
    { "iState_NPCFastWalk", TP::kiState_NPCFastWalk, AnimVarKind::Int },
    { "bFreezeSpeedUpdate", TP::kbFreezeSpeedUpdate, AnimVarKind::Bool },
    { "bInLandingState", TP::kbInLandingState, AnimVarKind::Bool },
    { "bEnableRoot_IsActiveMod", TP::kbEnableRoot_IsActiveMod, AnimVarKind::Bool },
    { "WalkSpeedMult", TP::kWalkSpeedMult, AnimVarKind::Float },
    { "JogSpeedMult", TP::kJogSpeedMult, AnimVarKind::Float },
    { "runSpeedMult", TP::krunSpeedMult, AnimVarKind::Float },
    { "iSyncLocomotionSpeed", TP::kiSyncLocomotionSpeed, AnimVarKind::Int },
    { "iControlsIdleSync", TP::kiControlsIdleSync, AnimVarKind::Int },
    { "bPathingInterruptibleIdle", TP::kbPathingInterruptibleIdle, AnimVarKind::Int },
    { "iIsPlayer", TP::kiIsPlayer, AnimVarKind::Int },
    { "SpeedSmoothed", TP::kSpeedSmoothed, AnimVarKind::Float },
    { "LeftHandIKOn", TP::kLeftHandIKOn, AnimVarKind::Bool },
    { "bIsThrowing", TP::kbIsThrowing, AnimVarKind::Bool },
    { "iSyncWalkRun", TP::kiSyncWalkRun, AnimVarKind::Int },
    { "iSyncJumpState", TP::kiSyncJumpState, AnimVarKind::Int },
    { "Enable_bEquipOK", TP::kEnable_bEquipOK, AnimVarKind::Bool },
    { "bIsInMT", TP::kbIsInMT, AnimVarKind::Bool },
    { "bFreezeRotationUpdate", TP::kbFreezeRotationUpdate, AnimVarKind::Bool },
    { "DirectionDegrees", TP::kDirectionDegrees, AnimVarKind::Float },
    { "iState_NPCBlocking", TP::kiState_NPCBlocking, AnimVarKind::Int },
    { "iSyncForwardBackward", TP::kiSyncForwardBackward, AnimVarKind::Int },
    { "IsBlocking", TP::kIsBlocking, AnimVarKind::Bool },
    { "fDirectAtSavedGain", TP::kfDirectAtSavedGain, AnimVarKind::Float },
    { "iSyncForwardBackward00", TP::kiSyncForwardBackward00, AnimVarKind::Int },
    { "fLocomotionWalkPlaybackSpeed", TP::kfLocomotionWalkPlaybackSpeed, AnimVarKind::Float },
    { "fLocomotionJogPlaybackSpeed", TP::kfLocomotionJogPlaybackSpeed, AnimVarKind::Float },
    { "fLocomotionRunPlaybackSpeed", TP::kfLocomotionRunPlaybackSpeed, AnimVarKind::Float },
    { "iLocomotionSpeedState", TP::kiLocomotionSpeedState, AnimVarKind::Int },
    { "bUseRifleReadyDirectAt", TP::kbUseRifleReadyDirectAt, AnimVarKind::Bool },
    { "iMeleeState", TP::kiMeleeState, AnimVarKind::Int },
    { "bEquipOkIsActiveEnabled", TP::kbEquipOkIsActiveEnabled, AnimVarKind::Bool },
    { "iSyncSneakWalkRun", TP::kiSyncSneakWalkRun, AnimVarKind::Int },
    { "iSyncDirection", TP::kiSyncDirection, AnimVarKind::Int },
    { "fLocomotionSneakWalkPlaybackSpeed", TP::kfLocomotionSneakWalkPlaybackSpeed, AnimVarKind::Float },
    { "fLocomotionSneakRunPlaybackSpeed", TP::kfLocomotionSneakRunPlaybackSpeed, AnimVarKind::Float },
    { "weaponSpeedMult", TP::kweaponSpeedMult, AnimVarKind::Float },
    { "bIsSneaking", TP::kbIsSneaking, AnimVarKind::Bool },
    { "bDisableAttackReady", TP::kbDisableAttackReady, AnimVarKind::Bool },
    { "bSyncDirection", TP::kbSyncDirection, AnimVarKind::Bool },
    { "bActorMobilityNotFullyCrippled", TP::kbActorMobilityNotFullyCrippled, AnimVarKind::Bool },
    { "bRootRifleEquipOk", TP::kbRootRifleEquipOk, AnimVarKind::Bool },
    { "bAnimateWeaponBones", TP::kbAnimateWeaponBones, AnimVarKind::Bool },
    { "bForceIdleStop", TP::kbForceIdleStop, AnimVarKind::Bool },
    { "iSyncShuffleState", TP::kiSyncShuffleState, AnimVarKind::Int },
    { "iSyncSightedState", TP::kiSyncSightedState, AnimVarKind::Int },
    { "iWeaponChargeMode", TP::kiWeaponChargeMode, AnimVarKind::Int },
    { "ReloadSpeedMult", TP::kReloadSpeedMult, AnimVarKind::Float },
    { "iSyncReadyAlertRelaxed", TP::kiSyncReadyAlertRelaxed, AnimVarKind::Int },
    { "bUseLeftHandIKDefaults", TP::kbUseLeftHandIKDefaults, AnimVarKind::Bool },
    { "iSyncGunDown", TP::kiSyncGunDown, AnimVarKind::Int },
    { "bAimEnabled", TP::kbAimEnabled, AnimVarKind::Bool },
    { "bAimCaptureEnabled", TP::kbAimCaptureEnabled, AnimVarKind::Bool },
    { "CurrentJumpState", TP::kCurrentJumpState, AnimVarKind::Int },
    { "bDisableSpineTracking", TP::kbDisableSpineTracking, AnimVarKind::Bool },
    { "fPlaybackMult", TP::kfPlaybackMult, AnimVarKind::Float },
    { "bIsInFlavor", TP::kbIsInFlavor, AnimVarKind::Bool },
    { "iTalkGenerator", TP::kiTalkGenerator, AnimVarKind::Int },
    { "staggerDirection", TP::kstaggerDirection, AnimVarKind::Int },
};

void CoSyncAnimVarSystem::VerifyVariableIndices(Actor* actor)
{
    LOG_REQUIREMENT("CoSyncAnimVarSystem::VerifyVariableIndices", "Needs a valid actor with a readable animation variable set. Writes test values into the actor's real graph to check them — this is risky (has crashed the game when run on the live player) so it's currently disabled everywhere it's called.");
    if (!actor) return;

    VariableSetResult varSet;
    if (!GetVariableSetDirect(actor, varSet) || varSet.size == 0)
    {
        LOG_WARN("[AnimVerify] Couldn't read the animation variables for actor=0x%08X", actor->formID);
        return;
    }

    // UPDATE 2026-06-17 (round 14): added per-step tracing. The previous
    // theory (varSet.size read as uninitialized garbage, feeding an unbounded
    // std::vector ctor -> uncaught std::bad_alloc -> silent abort bypassing
    // VEH) was NOT confirmed by data -- the round-13 bound fix on
    // GetVariableSetDirect made no observable difference, meaning size was
    // already sane both times. Whatever silently kills/hangs this (still zero
    // VEH output, zero further log lines) is happening somewhere else in this
    // function or in the Get/Set calls below. Logging size + a marker before
    // and after each per-entry step so the next run pinpoints the exact call.
    LOG_INFO("[AnimVerify] Variable set has %u entries, starting %zu checks", varSet.size,
        sizeof(kAnimVarCheckTable) / sizeof(kAnimVarCheckTable[0]));

    std::vector<uint32_t> snapshot(varSet.data, varSet.data + varSet.size);

    // round 23: last use of varSet.data above -- release now, before any of
    // the Get/SetXxx calls below get a chance to re-enter the engine and
    // drop the last other reference to this manager out from under us.
    if (varSet.manager) { varSet.manager->Release(); varSet.manager = nullptr; }

    LOG_INFO("[AnimVerify] Built a snapshot with %zu entries", snapshot.size());

    int okCount = 0, mismatchCount = 0, ambiguousCount = 0, failCount = 0;

    int checkIdx = 0;
    for (const auto& chk : kAnimVarCheckTable)
    {
        LOG_INFO("[AnimVerify] [%d] checking '%s' (expected index=%u, kind=%d)...",
            checkIdx, chk.name, chk.expectedIndex, (int)chk.kind);
        ++checkIdx;

        bool gotOriginal = false;
        float origF = 0.f;
        int   origI = 0;
        bool  origB = false;

        switch (chk.kind)
        {
        case AnimVarKind::Float: gotOriginal = CoSyncAnimUtils::GetFloat(actor, chk.name, origF); break;
        case AnimVarKind::Int:   gotOriginal = CoSyncAnimUtils::GetInt(actor, chk.name, origI);   break;
        case AnimVarKind::Bool:  gotOriginal = CoSyncAnimUtils::GetBool(actor, chk.name, origB);  break;
        }

        LOG_INFO("[AnimVerify] [%d] '%s' read attempt returned %d", checkIdx - 1, chk.name, gotOriginal ? 1 : 0);

        if (!gotOriginal)
        {
            LOG_WARN("[AnimVerify] '%s' (expected index=%u): couldn't read it — name not found",
                chk.name, chk.expectedIndex);
            ++failCount;
            continue;
        }

        bool setOk = false;
        switch (chk.kind)
        {
        case AnimVarKind::Float: setOk = CoSyncAnimUtils::SetFloat(actor, chk.name, 123456.75f); break;
        case AnimVarKind::Int:   setOk = CoSyncAnimUtils::SetInt(actor, chk.name, 123456);        break;
        case AnimVarKind::Bool:  setOk = CoSyncAnimUtils::SetBool(actor, chk.name, !origB);       break;
        }

        int realIdx = -1, numChanged = 0;
        if (setOk)
        {
            VariableSetResult after;
            if (GetVariableSetDirect(actor, after) && after.size == snapshot.size())
            {
                for (uint32_t i = 0; i < after.size; ++i)
                {
                    if (after.data[i] != snapshot[i])
                    {
                        realIdx = static_cast<int>(i);
                        ++numChanged;
                    }
                }
            }
            if (after.manager) { after.manager->Release(); after.manager = nullptr; }
        }

        // Restore immediately regardless of what the probe found.
        switch (chk.kind)
        {
        case AnimVarKind::Float: CoSyncAnimUtils::SetFloat(actor, chk.name, origF); break;
        case AnimVarKind::Int:   CoSyncAnimUtils::SetInt(actor, chk.name, origI);   break;
        case AnimVarKind::Bool:  CoSyncAnimUtils::SetBool(actor, chk.name, origB);  break;
        }

        // Refresh the baseline snapshot for the next iteration's diff.
        VariableSetResult refreshed;
        if (GetVariableSetDirect(actor, refreshed) && refreshed.size == snapshot.size())
            snapshot.assign(refreshed.data, refreshed.data + refreshed.size);
        if (refreshed.manager) { refreshed.manager->Release(); refreshed.manager = nullptr; }

        if (!setOk)
        {
            LOG_WARN("[AnimVerify] '%s' (expected index=%u): read fine but couldn't write it back",
                chk.name, chk.expectedIndex);
            ++failCount;
        }
        else if (numChanged != 1)
        {
            LOG_WARN("[AnimVerify] '%s' (expected index=%u): unclear result, %d slots changed",
                chk.name, chk.expectedIndex, numChanged);
            ++ambiguousCount;
        }
        else if (static_cast<uint32_t>(realIdx) == chk.expectedIndex)
        {
            ++okCount;
        }
        else
        {
            LOG_ERROR("[AnimVerify] '%s': mismatch — our table says index=%u but the engine's "
                "own name lookup found index=%d",
                chk.name, chk.expectedIndex, realIdx);
            ++mismatchCount;
        }
    }

    LOG_INFO("[AnimVerify] === Done for actor=0x%08X: %d ok, %d mismatched, %d unclear, %d failed (of %zu total) ===",
        actor->formID, okCount, mismatchCount, ambiguousCount, failCount,
        sizeof(kAnimVarCheckTable) / sizeof(kAnimVarCheckTable[0]));
}

// First-person graph variable indices, read straight off the variable-name
// table in hkbBehaviorGraphStringData:
//   Fallout4 - Animations\Meshes\Actors\Character\_1stPerson\Behaviors\RootBehavior.hkx
//     0 fSpeedWalk   1 fSpeedRun   2 Speed   3 Direction
//    10 IsFirstPerson                       75 bRenderFirstPersonInWorld
//    89 iState_PlayerSneakMelee             90 SpeedSmoothed
//
// The third-person table (Behaviors\RaiderRootBehavior.hkx) is the same list
// with Speed NOT present at index 2, so every entry past index 1 sits one slot
// lower there: Direction=2, iState=8, speedDamped=27 (lowercase s -- the
// first-person spelling is SpeedDamped), DirectionSmoothed=49, Speed=54.
// These constants are first-person ONLY; applying them to a proxy's
// third-person graph reads Direction where Speed is meant.
constexpr uint32_t kFPVar_Speed           = 2;
constexpr uint32_t kFPVar_Direction       = 3;
constexpr uint32_t kFPVar_SpeedSmoothed   = 90;
constexpr uint32_t kFPVar_IsFirstPerson   = 10;
constexpr uint32_t kFPVar_RenderFPInWorld = 75;

// noinline: same audit/fix as GetVariableSetDirect above (small static
// function wrapping a raw pointer read in __try/__except).
__declspec(noinline) static void PatchLocalPlayerSpeedFromFP(Actor* actor, AnimVarSnapshot& snap)
{
    LOG_REQUIREMENT("PatchLocalPlayerSpeedFromFP", "Only runs for the local player. Falls back to reading the first-person graph's raw data directly if the normal path isn't ready; guarded so that's safe even if the graph isn't set up yet.");
    if (!actor || actor->formID != 0x14) return;

    float vtblSpeed = 0.0f;
    const bool gotVtblSpeed = CoSyncAnimUtils::GetFloat(actor, "Speed", vtblSpeed);

    if (gotVtblSpeed && vtblSpeed > 0.5f)
    {

        static int s_vtblDiag = 0;
        if (++s_vtblDiag % 300 == 1)
            LOG_INFO("[AnimVarSave] First-person speed reading: %.2f  previous snapshot=%.2f  check#=%d",
                vtblSpeed, snap.floats[4], s_vtblDiag);

        snap.floats[4]  = vtblSpeed;
        snap.floats[10] = vtblSpeed;

        float vtblSmoothed = 0.0f;
        if (CoSyncAnimUtils::GetFloat(actor, "SpeedSmoothed", vtblSmoothed) && vtblSmoothed > 0.0f)
            snap.floats[11] = vtblSmoothed;
        else
            snap.floats[11] = vtblSpeed;

        return;
    }

    __try
    {
        TESObjectREFR* refr = static_cast<TESObjectREFR*>(actor);
        void* animHolder = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(refr) + 0x48);
        void*** vtbl = reinterpret_cast<void***>(animHolder);
        if (!vtbl || !*vtbl) return;

        typedef bool(__thiscall* GetBSAnimGraphFn)(void*, BSAnimationGraphManager**);
        auto getBSAnimGraph = reinterpret_cast<GetBSAnimGraphFn>((*vtbl)[4]);

        g_lastTickStep = "AnimVarSystem:PatchLocalPlayerSpeedFromFP:pre-GetBSAnimGraph";
        BSAnimationGraphManager* mgr = nullptr;
        if (!getBSAnimGraph(animHolder, &mgr) || !mgr) return;
        g_lastTickStep = "AnimVarSystem:PatchLocalPlayerSpeedFromFP:post-GetBSAnimGraph";

        const BShkbAnimationGraph* fpGraph = mgr->animationGraphs.Get(1);

        if (fpGraph && fpGraph->behaviorGraph)
        {
            const auto* fpVS = fpGraph->behaviorGraph->animationVariables;
            if (fpVS && fpVS->data && fpVS->size > 4)
            {


                static bool s_fpDirectDiagDone = false;
                if (!s_fpDirectDiagDone)
                {
                    s_fpDirectDiagDone = true;
                    const float dSpeed = fpVS->size > kFPVar_Speed
                        ? *reinterpret_cast<const float*>(&fpVS->data[kFPVar_Speed]) : -1.f;
                    const float dDir   = fpVS->size > kFPVar_Direction
                        ? *reinterpret_cast<const float*>(&fpVS->data[kFPVar_Direction]) : -1.f;
                    const float dSmth  = fpVS->size > kFPVar_SpeedSmoothed
                        ? *reinterpret_cast<const float*>(&fpVS->data[kFPVar_SpeedSmoothed]) : -1.f;
                    // Index 89 is iState_PlayerSneakMelee, an int state enum -- kept in
                    // the log purely so a session can be compared against the pre-fix
                    // builds that read SpeedSmoothed from here and got an int
                    // reinterpreted as a denormal float.
                    const float dOld89 = fpVS->size > 89
                        ? *reinterpret_cast<const float*>(&fpVS->data[89]) : -1.f;

                    // The graph's own first/third-person state. Speed and Direction are
                    // only driven while the first-person graph is actually active, so
                    // these two say whether a near-zero reading means "standing still"
                    // or "this graph isn't being driven at all".
                    const int isFP = fpVS->size > kFPVar_IsFirstPerson
                        ? *reinterpret_cast<const int*>(&fpVS->data[kFPVar_IsFirstPerson]) : -1;
                    const int renderFP = fpVS->size > kFPVar_RenderFPInWorld
                        ? *reinterpret_cast<const int*>(&fpVS->data[kFPVar_RenderFPInWorld]) : -1;

                    LOG_WARN("[AnimVarSave] First-person fallback check: size=%u  "
                        "Speed[2]=%.3f  Direction[3]=%.3f  SpeedSmoothed[90]=%.3f  "
                        "(old idx89=%.3f)  IsFirstPerson[10]=%d  RenderFirstPersonInWorld[75]=%d  "
                        "vtblSpeed=%.3f  gotVtbl=%d — a near-zero speed with IsFirstPerson=0 just means the "
                        "first-person graph isn't active, not that these indices are wrong",
                        fpVS->size, dSpeed, dDir, dSmth, dOld89, isFP, renderFP,
                        vtblSpeed, gotVtblSpeed ? 1 : 0);


                    LOG_INFO("[AnimVarSave] First-person variable scan (first 20 as float):");
                    for (uint32_t i = 0; i < 20 && i < fpVS->size; ++i)
                    {
                        float v = *reinterpret_cast<const float*>(&fpVS->data[i]);
                        if (v > 5.0f || v < -0.1f)
                            LOG_INFO("  first-person var[%u] = %.3f", i, v);
                    }
                }

                const float d2 = *reinterpret_cast<const float*>(&fpVS->data[kFPVar_Speed]);
                if (d2 > 0.5f)
                {
                    snap.floats[4]  = d2;
                    snap.floats[10] = d2;
                    if (fpVS->size > kFPVar_SpeedSmoothed)
                        snap.floats[11] = *reinterpret_cast<const float*>(&fpVS->data[kFPVar_SpeedSmoothed]);
                    if (fpVS->size > kFPVar_Direction)
                    {
                        const float d3 = *reinterpret_cast<const float*>(&fpVS->data[kFPVar_Direction]);
                        if (d3 != 0.0f)
                        {
                            snap.floats[0] = d3;
                            snap.floats[8] = d3;
                        }
                    }
                }
            }
        }

        mgr->Release();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool CoSyncAnimVarSystem::IsGraphReady(Actor* actor)
{
    LOG_REQUIREMENT("CoSyncAnimVarSystem::IsGraphReady", "Needs a valid actor. This IS the readiness check — it just reports whether the actor's animation variables can be read right now.");
    if (!actor) return false;
    VariableSetResult varSet;
    // round 23: this discards .data/.size entirely, but GetVariableSetDirect
    // still hands back a live manager reference (out.manager) on success --
    // release it immediately, since this function never touches varSet.data.
    bool ready = GetVariableSetDirect(actor, varSet);
    if (varSet.manager) { varSet.manager->Release(); varSet.manager = nullptr; }
    return ready;
}

bool CoSyncAnimVarSystem::SaveAnimationVariables(Actor* actor, AnimVarSnapshot& outSnapshot)
{
    LOG_REQUIREMENT("CoSyncAnimVarSystem::SaveAnimationVariables", "Needs a valid actor with a readable animation variable set. Assumes the graph's variable layout matches our known index table; the raw reads are guarded so a mismatch can't crash the game.");
    g_lastTickStep = "AnimVarSystem:SaveAnimationVariables:entry";
    if (!actor) return false;

    VariableSetResult varSet;
    if (!GetVariableSetDirect(actor, varSet))
        return false;

    // DISABLED 2026-06-17 (round 14): this diagnostic writes a deliberate
    // out-of-range sentinel value (123456.75f / 123456 / !origB) into the
    // LIVE player character's real animation variable, once for each of
    // ~116 table entries, purely to observe which array slot changes. Two
    // consecutive live tests died silently at exactly this call -- no VEH
    // crash report, no further log line, not even the first entry's result
    // -- consistent with the engine's own code reacting badly to one of
    // those sentinel writes (e.g. if some variable is used as a direct array
    // index or enum elsewhere in engine logic) in a way that bypasses both
    // SEH and VEH entirely (a hard abort, not a catchable AV). This is a
    // one-time self-verification of already-believed-correct hardcoded
    // indices, not required for any real sync functionality -- not worth
    // destabilizing the player's live game state to run. Disabled outright
    // rather than chasing which specific one of 116 writes is unsafe.
    {
        static bool s_indexVerifyDone = false;
        if (false && !s_indexVerifyDone && actor->formID == 0x14)
        {
            s_indexVerifyDone = true;
            LOG_INFO("[AnimVerify] Running the one-time variable-index check...");
            CoSyncAnimVarSystem::VerifyVariableIndices(actor);
        }
    }

    // 2026-08-01: a Power-Armor-active humanoid's graph was confirmed live
    // to report a variable-set size small enough to trip IsCreatureBehavior
    // -- silently routing the whole PA session through the creature path
    // (which never runs LoadPowerArmorLocomotion or the full humanoid
    // snapshot). Check sender-side PA state FIRST and skip the size
    // heuristic entirely for a confirmed-PA actor, real actors only
    // (formID 0 covers proxies/synthetic refs, which can't be in PA).
    const bool knownInPowerArmor =
        actor->formID != 0 && CoSyncPowerArmorSync::IsActorKnownInPowerArmor(actor->formID);

    if (!knownInPowerArmor && CoSyncCreatureAnimSync::IsCreatureBehavior(varSet.size))
    {
        // round 23: bail point holds a live manager reference (varSet never
        // touches .data again on this path) -- release before returning.
        bool creatureOk = CoSyncCreatureAnimSync::SaveCreatureVars(actor, outSnapshot);
        if (varSet.manager) { varSet.manager->Release(); varSet.manager = nullptr; }
        return creatureOk;
    }


    outSnapshot = AnimVarSnapshot{};

    __try
    {

        outSnapshot.booleans = 0;
        for (int i = 0; i < kNumBooleans; ++i)
        {
            uint32_t idx = kBooleanLookupTable[i];
            if (idx < varSet.size && varSet.data[idx] != 0)
                outSnapshot.booleans |= (1ULL << i);
        }


        for (int i = 0; i < kNumFloats; ++i)
        {
            uint32_t idx = kFloatLookupTable[i];
            if (idx < varSet.size)
                outSnapshot.floats[i] = *reinterpret_cast<float*>(&varSet.data[idx]);
        }


        for (int i = 0; i < kNumIntegers; ++i)
        {
            uint32_t idx = kIntegerLookupTable[i];
            if (idx < varSet.size)
                outSnapshot.integers[i] = varSet.data[idx];
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[AnimVarSystem] Crashed while reading animation variables for saving — caught it");
        if (varSet.manager) { varSet.manager->Release(); varSet.manager = nullptr; }
        return false;
    }


    static int diagTick = 0;
    ++diagTick;
    const bool doSaveDiag = (diagTick == 50 || diagTick == 200 || diagTick % 1800 == 0);
    if (doSaveDiag && varSet.data && varSet.size > 300)
    {
        const float speed   = *reinterpret_cast<float*>(&varSet.data[TP::kSpeed]);
        const float dir     = *reinterpret_cast<float*>(&varSet.data[TP::kDirection]);
        const float pitch   = *reinterpret_cast<float*>(&varSet.data[TP::kPitch]);
        LOG_INFO("[AnimVarSave] t=%d player(0x%08X) snapshot: "
            "speed=%.2f dir=%.2f pitch=%.2f iState=%u "
            "bools=0x%llX graphDriven=%u freezeSpd=%u jumpState=%u landing=%u "
            "NPCFastWalk=%u size=%u",
            diagTick, actor->formID,
            speed, dir, pitch,
            varSet.data[TP::kiState],
            (unsigned long long)outSnapshot.booleans,
            varSet.data[TP::kbGraphDriven],
            varSet.data[TP::kbFreezeSpeedUpdate],
            varSet.data[TP::kCurrentJumpState],
            varSet.data[TP::kbInLandingState],
            varSet.data[TP::kiState_NPCFastWalk],
            varSet.size);
    }

    // round 23: last touch of varSet.data/varSet.manager in this function --
    // everything below reads actor->actorState/middleProcess directly, not
    // through this manager. Release now rather than holding it across
    // PatchLocalPlayerSpeedFromFP, which does its own separate
    // GetVariableSetDirect call on the FP graph and shouldn't run while we're
    // still holding an unrelated TP-graph reference open.
    if (varSet.manager) { varSet.manager->Release(); varSet.manager = nullptr; }

    __try
    {
        outSnapshot.actorStateFlags1 = actor->actorState.unk08;
        outSnapshot.actorStateFlags2 = actor->actorState.flags;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}


    __try
    {
        auto middleProcess = actor->middleProcess;
        if (middleProcess)
        {
            float* dirPtr = reinterpret_cast<float*>(
                reinterpret_cast<uintptr_t>(middleProcess) + 0xC0);
            outSnapshot.direction = *dirPtr;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    g_lastTickStep = "AnimVarSystem:SaveAnimationVariables:PatchLocalPlayerSpeedFromFP";
    PatchLocalPlayerSpeedFromFP(actor, outSnapshot);

    g_lastTickStep = "AnimVarSystem:SaveAnimationVariables:exit";
    outSnapshot.isValid = true;
    return true;
}
bool CoSyncAnimVarSystem::LoadAnimationVariables(Actor* actor, const AnimVarSnapshot& snapshot)
{
    LOG_REQUIREMENT("CoSyncAnimVarSystem::LoadAnimationVariables", "Needs a valid actor and a valid snapshot, plus a writable animation variable set for that actor. Assumes the graph's layout matches our index table. The iState value must go through the safe setter, not a raw write, to avoid a known engine crash.");
    g_lastTickStep = "AnimVarSystem:LoadAnimationVariables:entry";
    if (!actor || !snapshot.isValid) return false;

    if (snapshot.booleans & kCreatureSnapshotFlag)
        return CoSyncCreatureAnimSync::LoadCreatureVars(actor, snapshot);

    VariableSetResult varSet;
    if (!GetVariableSetForWrite(actor, varSet))
        return false;

    // Round 119: both live tests died with zero trace (no VEH/SEH/WER/dmp)
    // somewhere between GetVariableSetForWrite's one-shot GRAPH DIAG line and
    // the next log line of this function — a window covering the raw data[]
    // writes, the engine named-setter block, and the manager Release. Every
    // log line is fflush'd, so phase markers on the first few proxy loads
    // pinpoint the exact dying phase in the next repro.
    static uint32_t s_phaseDiagCalls = 0;
    const bool phaseDiag = (++s_phaseDiagCalls <= 5);
#define ANIM_PHASE(tag) do { if (phaseDiag) \
    LOG_INFO("[AnimLoadPhase] call#%u actor=0x%08X %s", s_phaseDiagCalls, actor->formID, tag); } while (0)

    ANIM_PHASE("varset-ok");

    // Round 131: A-pose persists with AI enabled → suspicion moves to process
    // registration. FO4 only samples an actor's behavior graph onto its bones
    // when the actor is in the AI process lists (middleProcess non-null,
    // high-ish process level). Compare the proxy's process state against the
    // local player's on the first few loads.
    if (phaseDiag)
    {
        PlayerCharacter* lp = *g_player;
        LOG_INFO("[AnimLoadPhase] call#%u actor=0x%08X middleProcess=%p (local player's=%p)",
            s_phaseDiagCalls, actor->formID,
            static_cast<void*>(actor->middleProcess),
            lp ? static_cast<void*>(lp->middleProcess) : nullptr);
    }

    uint32_t sv_npcSneaking = 0, sv_npcBlocking = 0;
    __try
    {
        if (TP::kiState_NPCSneaking < varSet.size) sv_npcSneaking = varSet.data[TP::kiState_NPCSneaking];
        if (TP::kiState_NPCBlocking < varSet.size) sv_npcBlocking = varSet.data[TP::kiState_NPCBlocking];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Derive proxy NPC attack type from snapshot rather than restoring stale
    // proxy values. snapshot.integers[3] = kiAttackState (non-zero any attack),
    // snapshot.integers[16] = kiMeleeState (non-zero only for melee attacks).
    uint32_t sv_npcGun   = (snapshot.integers[3] > 0 && snapshot.integers[16] == 0) ? 1u : 0u;
    uint32_t sv_npcMelee = (snapshot.integers[3] > 0 && snapshot.integers[16] >  0) ? 1u : 0u;

    ANIM_PHASE("raw-writes-begin");
    __try
    {

        for (int i = 0; i < kNumBooleans; ++i)
        {
            uint32_t idx = kBooleanLookupTable[i];
            if (idx < varSet.size)
                varSet.data[idx] = (snapshot.booleans & (1ULL << i)) ? 1 : 0;
        }


        for (int i = 0; i < kNumFloats; ++i)
        {
            uint32_t idx = kFloatLookupTable[i];
            if (idx < varSet.size)
                *reinterpret_cast<float*>(&varSet.data[idx]) = snapshot.floats[i];
        }


        for (int i = 0; i < kNumIntegers; ++i)
        {
            uint32_t idx = kIntegerLookupTable[i];
            // kiState is excluded here -- it's the one variable in this table
            // documented to have actually crashed the engine when raw-written
            // (the NPCFastWalk=190 case). It's now written exclusively via
            // the safe CoSyncAnimUtils::SetInt call further down this
            // function; writing it here too (with the *remote peer's* raw
            // synced value, immediately clobbered a few lines later by the
            // locally-recomputed one) would be a redundant unsafe write to
            // the exact slot that's known to be risky, for no behavioral
            // benefit since the later call overwrites it regardless.
            if (idx < varSet.size && idx != TP::kiState)
                varSet.data[idx] = snapshot.integers[i];
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[AnimVarSystem] Crashed while writing animation variables — caught it");
        // round 23: bail point holds a live manager reference -- release
        // before returning, or this leaks the manager's refcount forever
        // (which would itself eventually prevent the manager from ever being
        // destructed/replaced, a slow real-engine-state bug on top of the
        // UAF this whole round is fixing).
        if (varSet.manager) { varSet.manager->Release(); varSet.manager = nullptr; }
        return false;
    }
    ANIM_PHASE("raw-writes-done");

    ANIM_PHASE("identity-begin");
    __try
    {

        const uint32_t pre_graphDriven    = (TP::kbGraphDriven          < varSet.size) ? varSet.data[TP::kbGraphDriven]          : 0;
        const uint32_t pre_graphDrivenRot = (TP::kbGraphDrivenRotation  < varSet.size) ? varSet.data[TP::kbGraphDrivenRotation]  : 0;
        const uint32_t pre_freezeSpd      = (TP::kbFreezeSpeedUpdate    < varSet.size) ? varSet.data[TP::kbFreezeSpeedUpdate]    : 0;
        const uint32_t pre_jumpState      = (TP::kCurrentJumpState      < varSet.size) ? varSet.data[TP::kCurrentJumpState]      : 0;
        const uint32_t pre_landing        = (TP::kbInLandingState       < varSet.size) ? varSet.data[TP::kbInLandingState]       : 0;
        const uint32_t pre_fastWalk       = (TP::kiState_NPCFastWalk    < varSet.size) ? varSet.data[TP::kiState_NPCFastWalk]    : 0;
        const uint32_t pre_kiState        = (TP::kiState                < varSet.size) ? varSet.data[TP::kiState]                : 0;
        const float    pre_kSpeed         = (TP::kSpeed                < varSet.size) ? *reinterpret_cast<float*>(&varSet.data[TP::kSpeed]) : 0.f;

        if (TP::kIsNPC    < varSet.size) varSet.data[TP::kIsNPC]    = 1;
        if (TP::kIsPlayer < varSet.size) varSet.data[TP::kIsPlayer]  = 0;
        if (TP::kiIsPlayer< varSet.size) varSet.data[TP::kiIsPlayer] = 0;

        if (TP::kbGraphDrivenTranslation < varSet.size) varSet.data[TP::kbGraphDrivenTranslation] = 0;
        if (TP::kbGraphDrivenRotation    < varSet.size) varSet.data[TP::kbGraphDrivenRotation]    = 0;
        if (TP::kbAllowRotation          < varSet.size) varSet.data[TP::kbAllowRotation]          = 0;

        if (TP::km_bEnablePitchTwistModifier < varSet.size) varSet.data[TP::km_bEnablePitchTwistModifier] = 1;

        if (TP::kbNotHeadTrack      < varSet.size) varSet.data[TP::kbNotHeadTrack]      = 1;
        if (TP::kbAllowHeadTracking < varSet.size) varSet.data[TP::kbAllowHeadTracking] = 0;


        if (TP::kbDisableSpineTracking< varSet.size) varSet.data[TP::kbDisableSpineTracking] = 1;

        if (TP::kbFreezeSpeedUpdate    < varSet.size) varSet.data[TP::kbFreezeSpeedUpdate]    = 0;
        if (TP::kbFreezeRotationUpdate < varSet.size) varSet.data[TP::kbFreezeRotationUpdate] = 0;

        if (TP::kbAnimationDriven    < varSet.size) varSet.data[TP::kbAnimationDriven]    = 0;
        if (TP::kbAnimateWeaponBones < varSet.size) varSet.data[TP::kbAnimateWeaponBones] = 1;

        // Restored 2026-06-17: was hardcoded to 0u because writing kiState=190
        // (NPCFastWalk) raw into varSet.data[] confirmed-crashed the engine on
        // its next per-actor Update -- the raw memcpy write skips whatever
        // real state-transition machinery the engine's own SetVariableInt
        // path runs alongside it. That real path is now known (traced via IDA
        // for the AnimationUtils SetFloat/Int/Bool fix above: get the active
        // graph manager via holder vtable slot 4, lock it, iterate every
        // active graph instance, call the real non-virtual setter on each --
        // CoSyncAnimUtils::SetInt now does exactly this). Using the safe API
        // for just this one historically-crashing write instead of the raw
        // array poke: same effect, but self-validating (fails safely if the
        // name lookup misses, rather than blindly trusting a possibly-wrong
        // TP::Variables index) and runs the real transition machinery the
        // direct write was skipping.
        const uint32_t npcKiState = (snapshot.floats[4] > 30.0f) ? 190u : 0u;
        // Round 122: the engine named-setter far-call froze the main thread in
        // FOUR straight live sessions (02:16/02:36/15:22/15:28) — the last one
        // with the mgr spin lock successfully pre-acquired (round-121 try-lock,
        // no "lock busy" logged), so the hang is deeper inside the worker
        // (string interning / per-graph name lookup). It has never once
        // completed. The "raw kiState write crashes the engine" history that
        // justified it dates from the fake-actor era (pre spawn fix) and is
        // tainted evidence. Raw bounded write instead; if the engine's next
        // Update genuinely objects, the (now-proven) VEH will catch it with an
        // exact RVA we can hook — strictly better than a silent freeze.
        ANIM_PHASE("raw-iState-write");
        if (TP::kiState < varSet.size)
            varSet.data[TP::kiState] = npcKiState;

        if (TP::kiState_NPCFastWalk < varSet.size)
        {
            if (npcKiState == 0u)
            {

                varSet.data[TP::kiState_NPCFastWalk] = 13u;
            }
            else if (pre_fastWalk == 13u)
            {

                varSet.data[TP::kiState_NPCFastWalk] = 0u;
            }

        }

        if (TP::kiState_NPCGun      < varSet.size) varSet.data[TP::kiState_NPCGun]      = sv_npcGun;
        if (TP::kiState_NPCSneaking < varSet.size) varSet.data[TP::kiState_NPCSneaking] = sv_npcSneaking;
        if (TP::kiState_NPCMelee    < varSet.size) varSet.data[TP::kiState_NPCMelee]    = sv_npcMelee;
        if (TP::kiState_NPCBlocking < varSet.size) varSet.data[TP::kiState_NPCBlocking] = sv_npcBlocking;

        {
            // Round 138 found the (flags >> 29) & 7 extraction read undocumented
            // bits (see CoSyncPlayer.cpp's player-side weapon-state fix) and
            // corrected it to the SDK's own ActorState accessor: (flags >> 1) & 7,
            // drawn when >= 3 (kWeaponState_Drawn). That fix was never propagated
            // here, so this raw-write pass -- which unlike the block below IS live
            // every tick -- was silently deriving weaponDrawn from the wrong bits
            // for every NPC puppet.
            const uint32_t weaponStateRaw = (snapshot.actorStateFlags2 >> 1) & 0x7u;
            const bool weaponDrawn = (weaponStateRaw >= 3u);

            // Aim: engage upper-body aiming when weapon is drawn.
            // kAimHeadingCurrent/kAimPitchCurrent are already written from
            // the snapshot float array above; keep bAimCaptureEnabled false
            // so the engine doesn't overwrite them from hardware input.
            if (TP::kbAimEnabled            < varSet.size) varSet.data[TP::kbAimEnabled]             = weaponDrawn ? 1u : 0u;
            if (TP::kbAimCaptureEnabled     < varSet.size) varSet.data[TP::kbAimCaptureEnabled]      = 0;
            if (TP::kbAimActive             < varSet.size) varSet.data[TP::kbAimActive]              = 0;
            if (TP::kbUseRifleReadyDirectAt < varSet.size) varSet.data[TP::kbUseRifleReadyDirectAt]  = 0;
            if (TP::kbRootRifleEquipOk      < varSet.size) varSet.data[TP::kbRootRifleEquipOk]       = weaponDrawn ? 1u : 0u;
            if (TP::kEnable_bEquipOK        < varSet.size) varSet.data[TP::kEnable_bEquipOK]         = weaponDrawn ? 1u : 0u;
            if (TP::kbEquipOkIsActiveEnabled< varSet.size) varSet.data[TP::kbEquipOkIsActiveEnabled] = weaponDrawn ? 1u : 0u;
            if (TP::kbGraphWantsFootIK      < varSet.size) varSet.data[TP::kbGraphWantsFootIK]       = 0;

            const bool isMeleeing = (snapshot.integers[16] > 0);
            if (TP::kiSyncReadyAlertRelaxed < varSet.size)
                varSet.data[TP::kiSyncReadyAlertRelaxed] = (weaponDrawn || isMeleeing) ? 1u : 0u;
        }

        if (pre_graphDrivenRot || pre_freezeSpd || pre_jumpState || pre_landing)
        {
            static uint32_t s_staleCount = 0;
            ++s_staleCount;
            if (s_staleCount <= 5 || s_staleCount % 600 == 0)
                LOG_WARN("[AnimVarSystem] actor=0x%08X: cleared out-of-date snapshot state "
                    "(graphDrivenRot=%u freezeSpd=%u jumpState=%u landing=%u) "
                    "[stale#%u]",
                    actor->formID,
                    pre_graphDrivenRot, pre_freezeSpd,
                    pre_jumpState, pre_landing,
                    s_staleCount);

            // Recovery: after clearing stale transitional state the actor's anim
            // graph may be left in A-pose. Fire IdleForceDefaultState so the
            // behavior graph finds its default idle. Rate-limited per actor to
            // avoid spamming the event every frame.
            CoSyncAnimUtils::SendEvent(actor, "IdleForceDefaultState");
        }

        if (snapshot.floats[4] > 5.0f)
        {
            static uint32_t s_moveDiag = 0;
            ++s_moveDiag;
            if (s_moveDiag <= 20 || s_moveDiag % 50 == 0)
            {
                const float graphSpd = (TP::kSpeed < varSet.size)
                    ? *reinterpret_cast<float*>(&varSet.data[TP::kSpeed]) : -1.f;
                LOG_INFO("[AnimVarSystem] Movement update actor=0x%08X move#%u: "
                    "recvSpd=%.1f graphSpd=%.1f(was %.1f) "
                    "kiState=%u idleLoco=%u "
                    "locoState=%u graphDriven=%u fastWalk=%u(was %u)",
                    actor->formID, s_moveDiag,
                    snapshot.floats[4], graphSpd, pre_kSpeed,
                    (TP::kiState               < varSet.size) ? varSet.data[TP::kiState]               : 99u,
                    (TP::kiSyncIdleLocomotion  < varSet.size) ? varSet.data[TP::kiSyncIdleLocomotion]  : 99u,
                    (TP::kiLocomotionSpeedState< varSet.size) ? varSet.data[TP::kiLocomotionSpeedState] : 99u,
                    (TP::kbGraphDriven         < varSet.size) ? varSet.data[TP::kbGraphDriven]         : 99u,
                    (TP::kiState_NPCFastWalk   < varSet.size) ? varSet.data[TP::kiState_NPCFastWalk]   : 99u,
                    pre_fastWalk);
            }
        }

        static uint32_t s_loadDiagCount = 0;
        ++s_loadDiagCount;
        if (s_loadDiagCount <= 2 || s_loadDiagCount % 600 == 0)
        {
            const float spd  = (TP::kSpeed     < varSet.size) ? *reinterpret_cast<float*>(&varSet.data[TP::kSpeed])     : -1.f;
            const float dir  = (TP::kDirection < varSet.size) ? *reinterpret_cast<float*>(&varSet.data[TP::kDirection]) : 0.f;
            const float ptch = (TP::kPitch     < varSet.size) ? *reinterpret_cast<float*>(&varSet.data[TP::kPitch])     : 0.f;
            LOG_INFO("[AnimVarSystem] actor=0x%08X applying load[%u] varSize=%u: "
                "speed=%.1f dir=%.2f pitch=%.2f "
                "kiState=%u(was %u) locoState=%u idleLoco=%u iSyncWalkRun=%u "
                "graphDriven=%u gDrvRot=%u freezeSpd=%u inLanding=%u jumpState=%u "
                "IsNPC=%u IsPlayer=%u allowHead=%u disableSpine=%u pitchTwist=%u",
                actor->formID, s_loadDiagCount, varSet.size,
                spd, dir, ptch,
                (TP::kiState              < varSet.size) ? varSet.data[TP::kiState]              : 99u,
                pre_kiState,
                (TP::kiLocomotionSpeedState<varSet.size) ? varSet.data[TP::kiLocomotionSpeedState]: 99u,
                (TP::kiSyncIdleLocomotion < varSet.size) ? varSet.data[TP::kiSyncIdleLocomotion] : 99u,
                (TP::kiSyncWalkRun        < varSet.size) ? varSet.data[TP::kiSyncWalkRun]        : 99u,
                (TP::kbGraphDriven        < varSet.size) ? varSet.data[TP::kbGraphDriven]        : 99u,
                (TP::kbGraphDrivenRotation< varSet.size) ? varSet.data[TP::kbGraphDrivenRotation]: 99u,
                (TP::kbFreezeSpeedUpdate  < varSet.size) ? varSet.data[TP::kbFreezeSpeedUpdate]  : 99u,
                (TP::kbInLandingState     < varSet.size) ? varSet.data[TP::kbInLandingState]     : 99u,
                (TP::kCurrentJumpState    < varSet.size) ? varSet.data[TP::kCurrentJumpState]    : 99u,
                (TP::kIsNPC               < varSet.size) ? varSet.data[TP::kIsNPC]               : 99u,
                (TP::kIsPlayer            < varSet.size) ? varSet.data[TP::kIsPlayer]            : 99u,
                (TP::kbAllowHeadTracking  < varSet.size) ? varSet.data[TP::kbAllowHeadTracking]  : 99u,
                (TP::kbDisableSpineTracking<varSet.size) ? varSet.data[TP::kbDisableSpineTracking]: 99u,
                (TP::km_bEnablePitchTwistModifier<varSet.size) ? varSet.data[TP::km_bEnablePitchTwistModifier] : 99u);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[AnimVarSystem] Crashed while applying NPC identity overrides — caught it");
    }
    ANIM_PHASE("identity-done");

    // round 23: THE fix. This was the last point in the function that
    // touches varSet.data -- everything from here on reads only from the
    // local `snapshot` parameter and writes exclusively through the safe
    // CoSyncAnimUtils::SetFloat/SetBool/SetInt API (which resolves and
    // releases its own manager reference internally per call, not this
    // one). The removed code used to call pManager->Release() inside
    // GetVariableSetForWrite itself, before ANY of this function's ~80 raw
    // varSet.data[idx] writes above had run -- for a newly-registered actor
    // whose graph manager has no other live reference, that Release() can
    // destruct the manager (BSAnimationGraphManager::Release() is a real
    // InterlockedDecrement-to-zero dtor call, see BsAnimationGraphManager.h)
    // immediately, turning every write above into a write into already-freed
    // memory. Freed-but-mapped writes don't fault, so __except never saw
    // them; they just corrupted the heap, which surfaces later as
    // RaiseFailFastException -- a kill that bypasses SEH/VEH entirely,
    // matching the exact "silent crash, zero trace" symptom from tonight's
    // test (the first time this code path ever ran live for a non-player
    // actor). Releasing here instead, once data is truly done being touched.
    if (varSet.manager) { varSet.manager->Release(); varSet.manager = nullptr; }
    ANIM_PHASE("manager-released");

    // Round 122: entire engine named-setter pass disabled — see the
    // raw-iState-write comment above. Every variable it set is already
    // written raw (bounds-checked) by the lookup-table + identity blocks
    // above; this pass was a redundant second write through the far-call
    // worker that hangs. Re-enable deliberately (with a hang dump in hand)
    // if a specific variable turns out to need the engine's transition
    // machinery.
    static constexpr bool kUseEngineNamedSetters = false;
    if (kUseEngineNamedSetters)
    {
        ANIM_PHASE("named-setters-begin");

        const float snapSpeed     = snapshot.floats[4];
        const float snapDir       = snapshot.floats[0];
        const float snapDirSmth   = snapshot.floats[8];
        const float snapDirDamped = snapshot.floats[30];
        const float snapSpeedSmth = snapshot.floats[11];
        const float snapPitch     = snapshot.floats[6];
        const float snapTurnDelta = snapshot.floats[7];
        const float snapTurnSmth  = snapshot.floats[13];
        const float snapStaggerMag= snapshot.floats[31];

        const bool  sneaking      = (snapshot.booleans & (1ULL << 11)) != 0;
        const bool  sprinting     = (snapshot.booleans & (1ULL <<  1)) != 0;
        const bool  blocking      = (snapshot.booleans & (1ULL << 23)) != 0;
        const bool  jumping       = (snapshot.booleans & (1ULL <<  7)) != 0;
        const bool  inJumpState   = (snapshot.booleans & (1ULL <<  9)) != 0;
        const bool  inLanding     = (snapshot.booleans & (1ULL << 33)) != 0;
        const bool  moving        = snapSpeed > 5.0f;
        const uint32_t wsRaw2     = (snapshot.actorStateFlags2 >> 1) & 0x7u;
        const bool  weaponDrawn2  = (wsRaw2 >= 3u);

        const int   syncDir       = static_cast<int>(snapshot.integers[23]);
        const int   syncSneak     = static_cast<int>(snapshot.integers[22]);
        const int   syncSprint    = static_cast<int>(snapshot.integers[1]);
        const int   syncWalkRun   = static_cast<int>(snapshot.integers[13]);
        const int   locoTier      = static_cast<int>(snapshot.integers[15]);
        const int   syncLocoSpd   = static_cast<int>(snapshot.integers[20]);
        const int   isBackward    = static_cast<int>(snapshot.integers[25]);
        const int   syncGunDown   = static_cast<int>(snapshot.integers[30]);
        const int   syncAlert     = static_cast<int>(snapshot.integers[28]);
        const int   syncSighted   = static_cast<int>(snapshot.integers[29]);
        const int   talkGen       = static_cast<int>(snapshot.integers[31]);
        const int   staggerDir    = static_cast<int>(snapshot.integers[32]);
        const int   npcKiState    = (snapSpeed > 30.0f) ? 190 : 0;

        CoSyncAnimUtils::SetFloat(actor, "Speed",                 snapSpeed);
        CoSyncAnimUtils::SetFloat(actor, "SpeedSmoothed",         snapSpeedSmth);
        CoSyncAnimUtils::SetFloat(actor, "SampledSpeed",          snapSpeed);
        CoSyncAnimUtils::SetFloat(actor, "Direction",             snapDir);
        CoSyncAnimUtils::SetFloat(actor, "DirectionSmoothed",     snapDirSmth);
        CoSyncAnimUtils::SetFloat(actor, "DirectionDamped",       snapDirDamped);
        CoSyncAnimUtils::SetFloat(actor, "DirectionDegrees",      snapDir * 57.2957795f);
        // Removed: was CoSyncAnimUtils::SetFloat(actor, "speedDamped", snapDirDamped) --
        // fed direction-damped data into a variable named for speed, and there's no
        // captured/computed damped-speed value anywhere in AnimVarSnapshot to put there
        // instead (no separate slot for it, and "speedDamped" isn't in TP::Variables
        // either, so it was never covered by the bulk sync table). Looked like a
        // copy-paste artifact from the DirectionDamped line just above it -- removing
        // rather than guessing a replacement value, since sending wrong data to a named
        // variable (if it's even real) is worse than not touching it at all.
        CoSyncAnimUtils::SetFloat(actor, "TurnDelta",             snapTurnDelta);
        CoSyncAnimUtils::SetFloat(actor, "TurnDeltaSmoothed",     snapTurnSmth);
        CoSyncAnimUtils::SetFloat(actor, "Pitch",                 snapPitch);
        CoSyncAnimUtils::SetFloat(actor, "staggerMagnitude",      snapStaggerMag);
        // Aim/ADS spine pitch + heading. Both are captured on the wire
        // (floats[26]/[27]) but were only ever written through the raw indexed
        // data[] path, which this runtime has shown going dead for several
        // vars -- so the proxy's sighted/aim pose never pitched toward the
        // source's aim point. Drive them by name (re-resolves per call,
        // no-ops for a graph that lacks the name) so ADS tracks correctly.
        CoSyncAnimUtils::SetFloat(actor, "AimHeadingCurrent",     snapshot.floats[26]);
        CoSyncAnimUtils::SetFloat(actor, "AimPitchCurrent",       snapshot.floats[27]);

        CoSyncAnimUtils::SetBool(actor, "bIsSneaking",              sneaking);
        CoSyncAnimUtils::SetBool(actor, "IsSneaking",               sneaking);
        CoSyncAnimUtils::SetBool(actor, "bSyncDirection",           moving);
        CoSyncAnimUtils::SetBool(actor, "IsNPC",                    true);
        CoSyncAnimUtils::SetBool(actor, "IsPlayer",                 false);


        CoSyncAnimUtils::SetBool(actor, "bAnimationDriven",         false);
        CoSyncAnimUtils::SetBool(actor, "bAllowRotation",           false);
        CoSyncAnimUtils::SetBool(actor, "bGraphDrivenTranslation",  false);
        CoSyncAnimUtils::SetBool(actor, "bGraphDrivenRotation",     false);
        CoSyncAnimUtils::SetBool(actor, "bFreezeSpeedUpdate",       false);
        CoSyncAnimUtils::SetBool(actor, "bFreezeRotationUpdate",    false);


        CoSyncAnimUtils::SetBool(actor, "bAnimateWeaponBones",      true);


        CoSyncAnimUtils::SetBool(actor, "bAimEnabled",              weaponDrawn2);
        CoSyncAnimUtils::SetBool(actor, "bAimCaptureEnabled",       false);
        CoSyncAnimUtils::SetBool(actor, "bAimActive",               false);
        CoSyncAnimUtils::SetBool(actor, "bUseRifleReadyDirectAt",   false);
        CoSyncAnimUtils::SetBool(actor, "bRootRifleEquipOk",        weaponDrawn2);
        CoSyncAnimUtils::SetBool(actor, "bEquipOkIsActiveEnabled",  weaponDrawn2);
        CoSyncAnimUtils::SetBool(actor, "bGraphWantsFootIK",        false);


        CoSyncAnimUtils::SetBool(actor, "bNotHeadTrack",                true);
        CoSyncAnimUtils::SetBool(actor, "bAllowHeadTracking",           false);
        CoSyncAnimUtils::SetBool(actor, "bDisableSpineTracking",        true);
        CoSyncAnimUtils::SetBool(actor, "m_bEnablePitchTwistModifier",  true);


        CoSyncAnimUtils::SetBool(actor, "bInJumpState",     inJumpState);
        CoSyncAnimUtils::SetBool(actor, "bInLandingState",  inLanding);

        CoSyncAnimUtils::SetInt(actor, "iSyncIdleLocomotion",       moving ? 1 : 0);


        CoSyncAnimUtils::SetInt(actor, "iSyncDirection",            syncDir);


        CoSyncAnimUtils::SetInt(actor, "iSyncForwardBackward",      isBackward);
        CoSyncAnimUtils::SetInt(actor, "iSyncForwardBackward00",    isBackward);


        CoSyncAnimUtils::SetInt(actor, "iSyncWalkRun",              syncWalkRun);
        CoSyncAnimUtils::SetInt(actor, "iSyncSprintState",          syncSprint);
        CoSyncAnimUtils::SetInt(actor, "iLocomotionSpeedState",     locoTier);
        CoSyncAnimUtils::SetInt(actor, "iSyncLocomotionSpeed",      syncLocoSpd);


        CoSyncAnimUtils::SetInt(actor, "iSyncSneakWalkRun",         syncSneak);

        CoSyncAnimUtils::SetInt(actor, "iSyncTurnState",            1);


        CoSyncAnimUtils::SetInt(actor, "iSyncGunDown",              syncGunDown);
        // Pass the real posture through instead of the old `> 1 ? 1` cap. The
        // graph's weapon-up/ready pose is iSyncReadyAlertRelaxed == 2; capping
        // at 1 forced the proxy permanently into alert/relaxed and it could
        // never raise the weapon to the ready carry. Clamp only to the valid
        // 0..2 range so a garbage capture can't push an out-of-range state.
        {
            int alert = syncAlert < 0 ? 0 : (syncAlert > 2 ? 2 : syncAlert);
            CoSyncAnimUtils::SetInt(actor, "iSyncReadyAlertRelaxed", alert);
        }
        CoSyncAnimUtils::SetInt(actor, "iSyncSightedState",         syncSighted);


        CoSyncAnimUtils::SetInt(actor, "iState",                    npcKiState);


        CoSyncAnimUtils::SetInt(actor, "iIsPlayer",                 0);


        CoSyncAnimUtils::SetInt(actor, "iTalkGenerator",            talkGen);
        CoSyncAnimUtils::SetInt(actor, "staggerDirection",          staggerDir);


        static uint32_t s_namePas = 0;
        if (++s_namePas % 300 == 1)
            LOG_INFO("[AnimVarSys] Name-based pass #%u actor=0x%08X: "
                "spd=%.1f dir=%.2f(deg=%.0f) sneak=%d sprint=%d "
                "syncDir=%d fwdBack=%d walkRun=%d tier=%d npcState=%d sighted=%d gunDown=%d",
                s_namePas, actor->formID,
                snapSpeed, snapDir, snapDir * 57.295f,
                sneaking?1:0, sprinting?1:0,
                syncDir, isBackward, syncWalkRun, locoTier,
                npcKiState, syncSighted, syncGunDown);
    }

    ANIM_PHASE("named-setters-done");

    __try
    {
        auto middleProcess = actor->middleProcess;
        if (middleProcess)
        {
            float* dirPtr = reinterpret_cast<float*>(
                reinterpret_cast<uintptr_t>(middleProcess) + 0xC0);
            *dirPtr = snapshot.direction;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    ANIM_PHASE("exit");
#undef ANIM_PHASE
    g_lastTickStep = "AnimVarSystem:LoadAnimationVariables:exit";
    return true;
}

// 2026-07-31: resolves a variable NAME to its index in a SPECIFIC graph's
// value-set by reading the graph's own name->index hash table directly,
// entirely read-only, with NO call into the far-call engine setter chain
// (CoSyncAnimUtils::SetFloat/Int/Bool) that round 122 found hangs and never
// completes. IDA-traced against the canonical Fallout4_disk.i64 by
// decompiling the real per-graph setter (sub_141326660, reached from
// SetGraphVariableFloat/0x818DE0's call chain):
//   BShkbAnimationGraph+0x370 = hashmap struct pointer
//     hashmap+0x84  (132) = int32 bucket count
//     hashmap+0x90  (144) = sentinel/chain-end pointer
//     hashmap+0xA0  (160) = bucket/entry array base, 24-byte stride
//       entry+0x00 = key pointer (BSFixedString::data / StringCache::Entry*)
//       entry+0x08 = int32 resolved variable index
//       entry+0x10 = next pointer (collision chain)
// Deliberately does NOT replicate the engine's own hash function (unverified,
// and a wrong guess there would just silently miss entries) -- instead brute-
// force scans every bucket's own chain for a pointer-identity match against a
// freshly interned BSFixedString for `name` (interning itself, via the
// separate ctor RVA 0x167BEC0, is unrelated to and unaffected by the round-122
// hang and is already used safely throughout this codebase). Worst case of a
// wrong offset/layout assumption is "index not found" (safe no-op) or an
// SEH-caught fault reading unmapped memory -- never a hang, since there is no
// lock acquisition or engine call anywhere in this function.
// noinline: same audit/fix as GetVariableSetDirect above (small static
// function wrapping a raw pointer/vtable read in __try/__except).
__declspec(noinline) static bool ResolveGraphVariableIndex(const BShkbAnimationGraph* graph, const char* name, uint32_t& outIndex)
{
    if (!graph || !name) return false;

    __try
    {
        BSFixedString key(name);
        const void* keyPtr = key.data;
        if (!keyPtr) return false;

        const uint8_t* graphBytes = reinterpret_cast<const uint8_t*>(graph);
        const uint8_t* mapBase = *reinterpret_cast<const uint8_t* const*>(graphBytes + 0x370);
        if (!mapBase) return false;

        const int32_t  bucketCount = *reinterpret_cast<const int32_t*>(mapBase + 0x84);
        const uint8_t* sentinel    = *reinterpret_cast<const uint8_t* const*>(mapBase + 0x90);
        const uint8_t* bucketBase  = *reinterpret_cast<const uint8_t* const*>(mapBase + 0xA0);
        if (bucketCount <= 0 || bucketCount > 1000000 || !bucketBase) return false;

        for (int32_t i = 0; i < bucketCount; ++i)
        {
            const uint8_t* entry = bucketBase + 24ll * i;
            int hops = 0;
            while (entry && entry != sentinel && hops < 64)
            {
                const void* entryKey = *reinterpret_cast<const void* const*>(entry);
                if (entryKey == keyPtr)
                {
                    outIndex = *reinterpret_cast<const uint32_t*>(entry + 0x08);
                    return true;
                }
                entry = *reinterpret_cast<const uint8_t* const*>(entry + 0x10);
                ++hops;
            }
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

namespace
{
    struct PaVar { const char* name; float fval; bool isFloat; };
}

bool CoSyncAnimVarSystem::LoadPowerArmorLocomotion(Actor* actor, const AnimVarSnapshot& snapshot)
{
    LOG_REQUIREMENT("CoSyncAnimVarSystem::LoadPowerArmorLocomotion", "Needs a valid actor and a valid snapshot. Looks up each variable's index by name, then writes it the same safe, direct way already proven for the regular human graph — never through the disabled engine setter.");
    if (!actor || !snapshot.isValid) return false;
    if (!CoSyncAnimUtils::IsGraphReady(actor)) return false;

    const float snapSpeed     = snapshot.floats[4];
    const float snapDir       = snapshot.floats[0];
    const float snapDirSmth   = snapshot.floats[8];
    const float snapDirDamped = snapshot.floats[30];
    const float snapSpeedSmth = snapshot.floats[11];
    const float snapPitch     = snapshot.floats[6];
    const float snapTurnDelta = snapshot.floats[7];
    const float snapTurnSmth  = snapshot.floats[13];

    const bool  sneaking      = (snapshot.booleans & (1ULL << 11)) != 0;
    const bool  moving        = snapSpeed > 5.0f;

    const int   syncDir       = static_cast<int>(snapshot.integers[23]);
    const int   syncSneak     = static_cast<int>(snapshot.integers[22]);
    const int   syncSprint    = static_cast<int>(snapshot.integers[1]);
    const int   syncWalkRun   = static_cast<int>(snapshot.integers[13]);
    const int   locoTier      = static_cast<int>(snapshot.integers[15]);
    const int   syncLocoSpd   = static_cast<int>(snapshot.integers[20]);
    const int   isBackward    = static_cast<int>(snapshot.integers[25]);

    __try
    {
        TESObjectREFR* refr = static_cast<TESObjectREFR*>(actor);
        void* animHolder = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(refr) + 0x48);
        void*** vtable = reinterpret_cast<void***>(animHolder);
        if (!vtable || !*vtable) return false;

        typedef bool(__thiscall* GetBSAnimGraphFn)(void*, BSAnimationGraphManager**);
        GetBSAnimGraphFn getBSAnimGraph = reinterpret_cast<GetBSAnimGraphFn>((*vtable)[4]);

        BSAnimationGraphManager* pManager = nullptr;
        if (!getBSAnimGraph(animHolder, &pManager) || !pManager)
            return false;

        const BShkbAnimationGraph* pGraph = pManager->animationGraphs.Get(0);
        if (!pGraph || !pGraph->behaviorGraph)
        {
            pManager->Release();
            return false;
        }

        auto* pVariableSet = pGraph->behaviorGraph->animationVariables;
        if (!pVariableSet || !pVariableSet->data || pVariableSet->size == 0 ||
            pVariableSet->size > kMaxSaneVariableSetSize)
        {
            pManager->Release();
            return false;
        }

        uint32_t* data = pVariableSet->data;
        const uint32_t size = pVariableSet->size;
        uint32_t resolvedCount = 0, attemptCount = 0;

        auto writeFloat = [&](const char* name, float v)
        {
            ++attemptCount;
            uint32_t idx;
            if (ResolveGraphVariableIndex(pGraph, name, idx) && idx < size)
            {
                *reinterpret_cast<float*>(&data[idx]) = v;
                ++resolvedCount;
            }
        };
        auto writeInt = [&](const char* name, int v)
        {
            ++attemptCount;
            uint32_t idx;
            if (ResolveGraphVariableIndex(pGraph, name, idx) && idx < size)
            {
                data[idx] = static_cast<uint32_t>(v);
                ++resolvedCount;
            }
        };
        auto writeBool = [&](const char* name, bool v) { writeInt(name, v ? 1 : 0); };

        writeFloat("Speed",             snapSpeed);
        writeFloat("SpeedSmoothed",     snapSpeedSmth);
        writeFloat("SampledSpeed",      snapSpeed);
        writeFloat("Direction",         snapDir);
        writeFloat("DirectionSmoothed", snapDirSmth);
        writeFloat("DirectionDamped",   snapDirDamped);
        writeFloat("DirectionDegrees",  snapDir * 57.2957795f);
        writeFloat("TurnDelta",         snapTurnDelta);
        writeFloat("TurnDeltaSmoothed", snapTurnSmth);
        writeFloat("Pitch",             snapPitch);

        writeBool("bIsSneaking",    sneaking);
        writeBool("IsSneaking",     sneaking);
        writeBool("bSyncDirection", moving);

        writeInt("iSyncIdleLocomotion",   moving ? 1 : 0);
        writeInt("iSyncDirection",        syncDir);
        writeInt("iSyncForwardBackward",  isBackward);
        writeInt("iSyncForwardBackward00",isBackward);
        writeInt("iSyncWalkRun",          syncWalkRun);
        writeInt("iSyncSprintState",      syncSprint);
        writeInt("iLocomotionSpeedState", locoTier);
        writeInt("iSyncLocomotionSpeed",  syncLocoSpd);
        writeInt("iSyncSneakWalkRun",     syncSneak);
        writeInt("iSyncTurnState",        1);

        pManager->Release();

        static uint32_t s_paPass = 0;
        if (++s_paPass % 60 == 1)
            LOG_UPDATED("[AnimVarSys] Power armor pass #%u actor=0x%08X: resolved=%u/%u varSetSize=%u spd=%.1f dir=%.2f sneak=%d walkRun=%d tier=%d",
                s_paPass, actor->formID, resolvedCount, attemptCount, size,
                snapSpeed, snapDir, sneaking ? 1 : 0, syncWalkRun, locoTier);

        return resolvedCount > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_UPDATED_WARN("[AnimVarSys] Crashed loading power armor locomotion for actor=0x%08X — caught it", actor->formID);
        return false;
    }
}

// 2026-08-02 decisive diagnostic. Host-owned raiders A-pose on the HOST's own
// screen, and every snapshot the host captures from them reads speed=0.0. Two
// mutually exclusive explanations were left standing and guessing between them
// is what produced this project's revert cycles:
//   (A) the hardcoded TP:: index table is wrong for THESE graphs, so we read a
//       slot that isn't Speed and always see 0 -- the actor is fine;
//   (B) the actor really is frozen, and 0.0 is a correct read.
// This resolves the variable BY NAME out of the graph's own name->index hash
// map (the mechanism already proven at resolved=23/23 during the PA work) and
// logs the resolved index and value beside the hardcoded one. Different indices
// => (A). Same index and both 0 => (B). Purely read-only, no engine call.
// NOTE: SaveAnimationVariables' own [AnimVarSave] diagnostic is gated on
// varSet.size > 300 and these NPC graphs are 299, so it has NEVER fired for
// them -- which is exactly why this was invisible until now.
void CoSyncAnimVarSystem::DiagnoseCaptureIndices(Actor* actor, uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncAnimVarSystem::DiagnoseCaptureIndices", "Needs a valid actor. Read-only — just inspects the graph the same way DumpGraphVariableNames does. Guarded, and always releases what it opens.");
    if (!actor) return;

    __try
    {
        TESObjectREFR* refr = static_cast<TESObjectREFR*>(actor);
        void* animHolder = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(refr) + 0x48);
        void*** vtable = reinterpret_cast<void***>(animHolder);
        if (!vtable || !*vtable) return;

        typedef bool(__thiscall* GetBSAnimGraphFn)(void*, BSAnimationGraphManager**);
        GetBSAnimGraphFn getBSAnimGraph = reinterpret_cast<GetBSAnimGraphFn>((*vtable)[4]);

        BSAnimationGraphManager* pManager = nullptr;
        if (!getBSAnimGraph(animHolder, &pManager) || !pManager) return;

        const BShkbAnimationGraph* pGraph = pManager->animationGraphs.Get(0);
        if (!pGraph) { pManager->Release(); return; }
        if (!pGraph->behaviorGraph) { pManager->Release(); return; }

        auto* pVarSet = pGraph->behaviorGraph->animationVariables;
        if (!pVarSet || !pVarSet->data || pVarSet->size == 0 ||
            pVarSet->size > kMaxSaneVariableSetSize)
        {
            pManager->Release();
            return;
        }

        struct Probe { const char* name; uint32_t hardIdx; };
        const Probe probes[] = {
            { CoSyncAnimUtils::Vars::Speed,     TP::kSpeed     },
            { CoSyncAnimUtils::Vars::Direction, TP::kDirection },
        };

        for (const Probe& p : probes)
        {
            uint32_t resolved = 0xFFFFFFFFu;
            const bool ok = ResolveGraphVariableIndex(pGraph, p.name, resolved);

            float hardVal = 0.f;
            if (p.hardIdx < pVarSet->size)
                hardVal = *reinterpret_cast<const float*>(&pVarSet->data[p.hardIdx]);

            float resVal = 0.f;
            if (ok && resolved < pVarSet->size)
                resVal = *reinterpret_cast<const float*>(&pVarSet->data[resolved]);

            LOG_UPDATED("[CaptureDiag] entity=%u actor=0x%08X size=%u '%s': hardIdx=%u val=%.2f | resolved=%s idx=%d val=%.2f | %s",
                entityID, actor->formID, pVarSet->size, p.name,
                p.hardIdx, hardVal,
                ok ? "YES" : "NO", ok ? (int)resolved : -1, resVal,
                (ok && resolved != p.hardIdx) ? "*** INDEX MISMATCH ***" : "indices agree");
        }

        pManager->Release();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_UPDATED_WARN("[CaptureDiag] Crashed checking actor=0x%08X — caught it", actor ? actor->formID : 0);
    }
}

void CoSyncAnimVarSystem::DumpGraphVariableNames(Actor* actor)
{
    LOG_REQUIREMENT("CoSyncAnimVarSystem::DumpGraphVariableNames", "Needs a valid actor. Read-only diagnostic — walks the graph's variable table and prints every name it finds, instead of checking against a known list.");
    if (!actor) return;

    __try
    {
        TESObjectREFR* refr = static_cast<TESObjectREFR*>(actor);
        void* animHolder = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(refr) + 0x48);
        void*** vtable = reinterpret_cast<void***>(animHolder);
        if (!vtable || !*vtable) return;

        typedef bool(__thiscall* GetBSAnimGraphFn)(void*, BSAnimationGraphManager**);
        GetBSAnimGraphFn getBSAnimGraph = reinterpret_cast<GetBSAnimGraphFn>((*vtable)[4]);

        BSAnimationGraphManager* pManager = nullptr;
        if (!getBSAnimGraph(animHolder, &pManager) || !pManager) return;

        const BShkbAnimationGraph* pGraph = pManager->animationGraphs.Get(0);
        if (!pGraph) { pManager->Release(); return; }

        const uint8_t* graphBytes = reinterpret_cast<const uint8_t*>(pGraph);
        const uint8_t* mapBase = *reinterpret_cast<const uint8_t* const*>(graphBytes + 0x370);
        if (!mapBase) { pManager->Release(); return; }

        const int32_t  bucketCount = *reinterpret_cast<const int32_t*>(mapBase + 0x84);
        const uint8_t* sentinel    = *reinterpret_cast<const uint8_t* const*>(mapBase + 0x90);
        const uint8_t* bucketBase  = *reinterpret_cast<const uint8_t* const*>(mapBase + 0xA0);
        if (bucketCount <= 0 || bucketCount > 1000000 || !bucketBase)
        {
            pManager->Release();
            return;
        }

        LOG_UPDATED("[AnimVarSys] Power armor graph dump actor=0x%08X bucketCount=%d", actor->formID, bucketCount);
        int printed = 0;
        for (int32_t i = 0; i < bucketCount && printed < 4000; ++i)
        {
            const uint8_t* entry = bucketBase + 24ll * i;
            int hops = 0;
            while (entry && entry != sentinel && hops < 64)
            {
                __try
                {
                    const void* entryKeyPtr = *reinterpret_cast<const void* const*>(entry);
                    const uint32_t idx = *reinterpret_cast<const uint32_t*>(entry + 0x08);
                    if (entryKeyPtr && idx < 2000)
                    {
                        auto* se = reinterpret_cast<StringCache::Entry*>(const_cast<void*>(entryKeyPtr));
                        const char* name = se->Get<char>();
                        if (name && name[0] >= 0x20 && name[0] < 0x7F)
                        {
                            LOG_UPDATED("[AnimVarSys]   [%u] '%s'", idx, name);
                            ++printed;
                        }
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}

                entry = *reinterpret_cast<const uint8_t* const*>(entry + 0x10);
                ++hops;
            }
        }
        LOG_UPDATED("[AnimVarSys] Power armor graph dump done actor=0x%08X printed=%d", actor->formID, printed);

        pManager->Release();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_UPDATED_WARN("[AnimVarSys] Crashed dumping graph variable names for actor=0x%08X — caught it", actor->formID);
    }
}

// PowerArmor diagnostic (round <PA graph-index diagnostic>): DumpGraphVariableNames
// and LoadPowerArmorLocomotion/ResolveGraphVariableIndex all hardcode
// animationGraphs.Get(0) to find the active graph, while the real engine
// event dispatch (CoSyncAnimUtils::SendEvent) goes through the holder's own
// index-aware internal resolution instead -- this project already knows
// (round 96, see the GRAPH DIAG/CONFIRMED BUG log in GetVariableSetForWrite
// above) that animationGraphIndex can legitimately point away from slot 0.
// If SwitchToPowerArmor changes the active index for a PA proxy, everything
// reading Get(0) here could be looking at a different graph object than the
// one SendEvent actually targets -- which would fully explain PAMoveStart/
// PowerArmorEnter coming back accepted=0 every time. Read-only, one-shot per
// entity (call site owns the dedup), same access pattern as
// GetVariableSetForWrite/DumpGraphVariableNames above -- do NOT act on this
// data without a real test result; see the call sites in CoSyncPlayer.cpp.
void CoSyncAnimVarSystem::DumpGraphIndexDiag(Actor* actor, const char* context)
{
    LOG_REQUIREMENT("CoSyncAnimVarSystem::DumpGraphIndexDiag", "Needs a valid actor. Read-only diagnostic that accesses the graph the same safe way the other functions in this file do. Guarded.");
    if (!actor) return;

    __try
    {
        TESObjectREFR* refr = static_cast<TESObjectREFR*>(actor);
        void* animHolder = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(refr) + 0x48);
        void*** vtable = reinterpret_cast<void***>(animHolder);
        if (!vtable || !*vtable) return;

        typedef bool(__thiscall* GetBSAnimGraphFn)(void*, BSAnimationGraphManager**);
        GetBSAnimGraphFn getBSAnimGraph = reinterpret_cast<GetBSAnimGraphFn>((*vtable)[4]);

        BSAnimationGraphManager* pManager = nullptr;
        if (!getBSAnimGraph(animHolder, &pManager) || !pManager) return;

        const uint32_t graphCount = pManager->animationGraphs.size;
        const uint32_t activeIdx  = pManager->animationGraphIndex;
        LOG_WARN("[PA graph check] context=%s actor=0x%08X: graphCount=%u activeIdx=%u"
            " (direct writes always target graph 0; sending an event picks its own graph — "
            "if activeIdx isn't 0, an event might land on a different graph than what we read)",
            context, actor->formID, graphCount, activeIdx);

        pManager->Release();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_UPDATED_WARN("[AnimVarSys] Crashed checking graph index for actor=0x%08X (%s) — caught it", actor->formID, context);
    }
}

void CoSyncAnimVarSystem::DumpVariables(Actor* actor, const char* label)
{
    LOG_REQUIREMENT("CoSyncAnimVarSystem::DumpVariables", "Needs a valid actor with a working animation graph — this just saves its current variables and prints them.");
    if (!actor) return;

    AnimVarSnapshot snap;
    if (!SaveAnimationVariables(actor, snap))
    {
        LOG_ERROR("[AnimVarSystem] Couldn't save variables to dump (%s)", label ? label : "");
        return;
    }

    LOG_INFO("[AnimVarSystem] === DUMP %s ===", label ? label : "");
    LOG_INFO("  Booleans: 0x%016llX", (unsigned long long)snap.booleans);
    LOG_INFO("  Floats: Speed=%.2f Dir=%.2f DirDamped=%.2f Turn=%.2f Pitch=%.2f SpeedSmooth=%.2f StaggerMag=%.2f",
        snap.floats[4], snap.floats[0], snap.floats[30], snap.floats[7], snap.floats[6], snap.floats[11], snap.floats[31]);
    LOG_INFO("  Ints: iState=%u iSprint=%u iAttack=%u iLocoSpeed=%u iIdleLoco=%u iSighted=%u iGunDown=%u iTalkGen=%u iStaggerDir=%u",
        snap.integers[0], snap.integers[1], snap.integers[3], snap.integers[20], snap.integers[26],
        snap.integers[29], snap.integers[30], snap.integers[31], snap.integers[32]);
    LOG_INFO("  ActorState: flags1=0x%08X flags2=0x%08X dir=%.2f",
        snap.actorStateFlags1, snap.actorStateFlags2, snap.direction);
}

size_t AnimVarSnapshot::Serialize(uint8_t* buffer, size_t bufferSize) const
{
    LOG_REQUIREMENT("AnimVarSnapshot::Serialize", "Doesn't touch the game — just needs a buffer big enough to hold the data.");
    const size_t needed = SerializedSize();
    if (bufferSize < needed) return 0;

    uint8_t* ptr = buffer;

    memcpy(ptr, &booleans, sizeof(booleans)); ptr += sizeof(booleans);
    memcpy(ptr, floats, sizeof(floats)); ptr += sizeof(floats);
    memcpy(ptr, integers, sizeof(integers)); ptr += sizeof(integers);
    memcpy(ptr, &actorStateFlags1, sizeof(actorStateFlags1)); ptr += sizeof(actorStateFlags1);
    memcpy(ptr, &actorStateFlags2, sizeof(actorStateFlags2)); ptr += sizeof(actorStateFlags2);
    memcpy(ptr, &direction, sizeof(direction)); ptr += sizeof(direction);

    return static_cast<size_t>(ptr - buffer);
}

size_t AnimVarSnapshot::Deserialize(const uint8_t* buffer, size_t bufferSize)
{
    LOG_REQUIREMENT("AnimVarSnapshot::Deserialize", "Doesn't touch the game — just needs a buffer with enough data in it to read.");
    const size_t needed = SerializedSize();
    if (bufferSize < needed) return 0;

    const uint8_t* ptr = buffer;

    memcpy(&booleans, ptr, sizeof(booleans)); ptr += sizeof(booleans);
    memcpy(floats, ptr, sizeof(floats)); ptr += sizeof(floats);
    memcpy(integers, ptr, sizeof(integers)); ptr += sizeof(integers);
    memcpy(&actorStateFlags1, ptr, sizeof(actorStateFlags1)); ptr += sizeof(actorStateFlags1);
    memcpy(&actorStateFlags2, ptr, sizeof(actorStateFlags2)); ptr += sizeof(actorStateFlags2);
    memcpy(&direction, ptr, sizeof(direction)); ptr += sizeof(direction);

    isValid = true;
    return static_cast<size_t>(ptr - buffer);
}
