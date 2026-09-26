#include "CoSyncPlayerAnimation.h"
#include "CoSyncAnimVarSystem.h"
#include "CoSyncAnimationUtils.h"
#include "CoSyncFirstPersonGraph.h"
#include "CoSyncPowerArmorSync.h"
#include "ConsoleLogger.h"
#include "GameReferences.h"
#include "GameTypes.h"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// RVAs — Fallout4.exe 1.11.240.0 (disk, not memory-dump)
//
// 2026-08-21: this header used to say 1.11.221. That was wrong and actively
// misleading -- the shipping install, the F4SE 0.7.9 that loads us, and every
// live log are all 1.11.240.0, and CoSyncFirstPersonGraph.cpp's own RVA block
// has said so since it was written. The addresses below were re-checked
// against 1.11.240 directly: all three graph-variable setters are genuine
// function starts at the stated sizes.
// ============================================================================

// ── Confirmed: at_func_start=True, size≥24 in disk IDB ──────────────────────
//
// IAnimationGraphManagerHolder::SetGraphVariable*
// Signature: bool(void* holder /*actor+0x48*/, BSFixedString* name, T value)
// Dispatch: holder vtbl slot4 → AddRef BSAnimationGraphManager →
//           loops all graphs, hash-lookup name → index, bounded store.
//
// FO4_Wrld used 0x818D60/D80/DA0 — first two are MID-FUNCTION (sub_140818D10
// starts at 0x818D10). Only 0x818DA0 is a real function start, but it is the
// BOOL setter, not float. All three values below are IDA-confirmed starts.
static constexpr uintptr_t RVA_SET_GRAPH_VAR_BOOL  = 0x008190B0; // sub_140818DA0 size=24
static constexpr uintptr_t RVA_SET_GRAPH_VAR_INT   = 0x008190D0; // sub_140818DC0 size=24
static constexpr uintptr_t RVA_SET_GRAPH_VAR_FLOAT = 0x008190F0; // sub_140818DE0 size=25

// IAnimationGraphManagerHolder lives at actor + 0x48 (confirmed CoSyncAnimationUtils.cpp
// round 117 + FO4_Wrld offsets.h IANIMGRAPHHOLDER_OFF).
static constexpr size_t HOLDER_OFF = 0x48;

// ── TODO: verify semantic identity in IDA before use ────────────────────────
//
// These are the ENCLOSING FUNCTION STARTS found by IDA around the addresses
// FO4_Wrld listed. FO4_Wrld's offsets were all mid-function (confirmed disk IDB).
// Open Fallout4_disk.i64, navigate to each address, decompile, confirm the
// function matches the description, then remove this TODO block.

// BSFixedString minter: interns a literal into the global string cache.
//   FO4_Wrld claimed 0x167BDC0 — IDA found that is 0x90 into sub_14167BD30.
//   Real function start: 0x167BD30 (size=181). Confirm: should take
//   (BSFixedString* dest, const char* src) and call the interning hash walker.
//   TODO: decompile sub_14167BD30, confirm signature. If confirmed, set below.
static constexpr uintptr_t RVA_BSFIXEDSTRING_MAKE = 0x0167C050; // TODO: confirm sub_14167BD30

// Actor::MoveTo — atomically updates pos + NIF world + Havok + anim graph + cell.
//   FO4_Wrld claimed 0xC60BE0 — IDA found that is 0x110 into sub_140C60AD0.
//   Real function start: 0xC60AD0 (size=496). Confirm: should accept
//   (Actor* this, float x, float y, float z, float yaw_rad, int doProcessUpdate).
//   TODO: decompile sub_140C60AD0, confirm MoveTo signature.
static constexpr uintptr_t RVA_ACTOR_MOVETO = 0x00C60E60; // TODO: confirm sub_140C60AD0

// Actor::TickMovementController — per-actor movement wrapper.
//   FO4_Wrld claimed 0xC65E20 — IDA found that is 0x50 into sub_140C65DD0.
//   Real function start: 0xC65DD0 (size=467). Confirm: should take
//   (Actor* this, float dt), dispatching to MovementControllerNPC::Tick via vt[8].
//   TODO: decompile sub_140C65DD0, confirm it is the movement wrapper.
static constexpr uintptr_t RVA_MOVEMENT_TICK = 0x00C66160; // TODO: confirm sub_140C65DD0

// Actor::Update_PerFrame — main per-actor AI tick funnel.
//   FO4_Wrld claimed 0xC636A0 — IDA found that is 0x10 into sub_140C63690.
//   Real function start: 0xC63690 (size=31). NOTE: size=31 is very small;
//   this may be a thunk or stub, not the full Update_PerFrame body.
//   TODO: decompile sub_140C63690, check if it tail-calls the real body.
static constexpr uintptr_t RVA_UPDATE_PERFRAME = 0x00C63A20; // TODO: confirm sub_140C63690

// ============================================================================
// Skeleton bone replication — UpdateDownwardPass
// ============================================================================
//
// This used to be an RVA: 0x016C8450, described in-tree as
// "NiAVObject::UpdateDownwardPass, IDA-confirmed, size=127".
//
// 2026-08-21: that address is a real function start, which is why it survived
// review, but it is NOT UpdateDownwardPass. Disassembled against the live
// build (1.11.240.0) it is NiAVObject VTABLE SLOT 31 (offset 0xF8), a boolean
// object-comparison in the IsEqual family: it compares two objects' hidden
// flag bit at +0x108, their +0x30 local transforms, and their +0x100
// collision objects, and returns bool in al. It never touches children and
// cannot recompute a world transform.
//
// Worse than useless: SafeUpdateDownward passed a ZEROED 512-byte stack
// buffer as the second argument, and the comparison path treats that argument
// as a live NiObject --
//
//     0x016BACB0   test rdx, rdx        ; non-null buffer, guard passes
//                  mov  rax, [rdx]      ; rax = buffer[0] = 0
//     0x016BACC9   call qword [rax+10h] ; -> 0xC0000005
//
// -- which is exactly the access violation this codebase has been chasing.
// Live proof from the 2026-08-21 session: the host log holds 3220
// "[CoSync-VEH] CRASH ... RIP=Fallout4.exe+0x16BACC9" lines and 3220
// "UpdateDownwardPass SEH on proxy" lines; the client holds 3184 and 3184.
// Equal to the unit on both machines -- every AV in the session was this one
// call, once per frame per proxy, caught by SEH and therefore non-fatal but
// costing a full VEH dispatch + log write at 60Hz. And the world-transform
// rebake this feature depends on had never once actually run.
//
// The real function is VTABLE SLOT 49 (offset 0x188). NiNode implements it at
// 0x016BF1E0 (verified: reads child count at +0x132, children array at +0x128,
// tests each child's +0x108 bit 0x2 before descending, recurses through
// [vt+0x188]/[vt+0x190]); BSFadeNode -- which is what GetActorRootNode
// actually returns -- OVERRIDES it at 0x0217DE70. So hard-coding NiNode's
// address would silently skip the override. Dispatch through the object's own
// vtable instead: no RVA to re-derive on the next game patch, and the correct
// implementation is selected by the object itself.
//
// Note the arity too: the real signature takes THREE arguments,
// void(NiAVObject*, NiUpdateData*, uint32_t flags) -- the old typedef declared
// two, leaving r8d as whatever the caller happened to leave behind.
static constexpr size_t NIAV_VTBL_UPDATE_DOWNWARD_PASS = 49;

// ============================================================================
// Function pointer types
// ============================================================================

using SetGraphVarBoolFn      = bool (*)(void* holder, BSFixedString* name, char  value);
using SetGraphVarIntFn       = bool (*)(void* holder, BSFixedString* name, int   value);
using SetGraphVarFloatFn     = bool (*)(void* holder, BSFixedString* name, float value);
using UpdateDownwardPassFn   = void (__fastcall*)(void* node, void* update_data, uint32_t flags);

// ============================================================================
// Module state
// ============================================================================

static std::atomic<bool> s_enabled{false};
static std::atomic<bool> s_initialized{false};

static SetGraphVarBoolFn    g_setBool          = nullptr;
static SetGraphVarIntFn     g_setInt           = nullptr;
static SetGraphVarFloatFn   g_setFloat         = nullptr;

// Global skeleton capture filled each frame by CaptureBones(), consumed by
// ApplySkeleton(). Both are called on the main thread, no locking needed.
static CoSyncPlayerAnimation::SkeletonCapture g_localCapture = {};

// Moved up from its old home next to SetLocalFirstPerson: CaptureBones now
// needs to CLEAR this when it builds a full-body capture from the generated
// pose, and CaptureBones is defined earlier in the file.
static std::atomic<bool> g_localFirstPerson{false};

// ============================================================================
// Animation variable names
// Canonical names per FO4_Wrld + strings.csv verification. Case-sensitive.
// iIsInSneak is INT (not bool) — drive with g_setInt, NOT g_setBool.
// ============================================================================

// Speed-sampled locomotion (float). 0=idle, 100=walk, 200=run.
static const char* k_SpeedSampled = "SpeedSampled";
// Direction (float, degrees). 0 = forward.
static const char* k_Direction    = "Direction";
// Locomotion state booleans
static const char* k_IsRunning    = "IsRunning";
static const char* k_IsSprinting  = "IsSprinting";
// Aiming / combat
static const char* k_bAimActive   = "bAimActive";
// Sneak — INT (0=stand, 1=sneak). Use g_setInt.
static const char* k_iIsInSneak   = "iIsInSneak";
// Dead
static const char* k_IsDead       = "IsDead";

// ============================================================================
// Helpers
// ============================================================================

static void* GetHolder(Actor* actor) noexcept
{
    if (!actor) return nullptr;
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(actor) + HOLDER_OFF);
}

// SEH-caged setter helpers — safe to call on the main thread.

// __declspec(noinline) is load-bearing: without it the optimizer can inline
// this __try/__except body into its (few) call sites and silently drop the
// SEH scope table, exactly as documented for CoSyncAnimUtils::GetFloat and
// confirmed live for CoSyncPlayerAnimation::SafeReadSkelRoot (2026-08-20, see
// [[project-cosync-fp-graph-drive-rvas]] pt.20) -- the guard stops actually
// catching anything and the fault propagates past the try block entirely.
__declspec(noinline) static void SafeSetBool(void* holder, const char* name, bool value) noexcept
{
    if (!g_setBool || !holder || !name) return;
    __try
    {
        BSFixedString key(name);
        g_setBool(holder, &key, value ? 1 : 0);
        CALL_MEMBER_FN(&key, Release)();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[PlayerAnim] SafeSetBool(%s) crashed and was caught", name);
    }
}

__declspec(noinline) static void SafeSetInt(void* holder, const char* name, int value) noexcept
{
    if (!g_setInt || !holder || !name) return;
    __try
    {
        BSFixedString key(name);
        g_setInt(holder, &key, value);
        CALL_MEMBER_FN(&key, Release)();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[PlayerAnim] SafeSetInt(%s) crashed and was caught", name);
    }
}

__declspec(noinline) static void SafeSetFloat(void* holder, const char* name, float value) noexcept
{
    if (!g_setFloat || !holder || !name) return;
    __try
    {
        BSFixedString key(name);
        g_setFloat(holder, &key, value);
        CALL_MEMBER_FN(&key, Release)();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[PlayerAnim] SafeSetFloat(%s) crashed and was caught", name);
    }
}

// ============================================================================
// Public API
// ============================================================================

namespace CoSyncPlayerAnimation
{

void Init(uintptr_t moduleBase) noexcept
{
    if (s_initialized.exchange(true)) return;

    g_setBool        = reinterpret_cast<SetGraphVarBoolFn   >(moduleBase + RVA_SET_GRAPH_VAR_BOOL);
    g_setInt         = reinterpret_cast<SetGraphVarIntFn    >(moduleBase + RVA_SET_GRAPH_VAR_INT);
    g_setFloat       = reinterpret_cast<SetGraphVarFloatFn  >(moduleBase + RVA_SET_GRAPH_VAR_FLOAT);

    LOG_INFO("[PlayerAnim] Init — RVAs resolved:"
             "  Bool=0x%08X  Int=0x%08X  Float=0x%08X"
             "  UpdateDownwardPass=vtable slot %zu (no RVA)",
             (unsigned)RVA_SET_GRAPH_VAR_BOOL,
             (unsigned)RVA_SET_GRAPH_VAR_INT,
             (unsigned)RVA_SET_GRAPH_VAR_FLOAT,
             NIAV_VTBL_UPDATE_DOWNWARD_PASS);
    LOG_INFO("[PlayerAnim] TODO RVAs (need IDA semantic confirm):"
             "  BSFSMake=0x%08X  MoveTo=0x%08X"
             "  MovementTick=0x%08X  UpdatePerFrame=0x%08X",
             (unsigned)RVA_BSFIXEDSTRING_MAKE,
             (unsigned)RVA_ACTOR_MOVETO,
             (unsigned)RVA_MOVEMENT_TICK,
             (unsigned)RVA_UPDATE_PERFRAME);
}

bool IsEnabled()
{
    return s_enabled.load(std::memory_order_relaxed);
}

void SetEnabled(bool enabled)
{
    s_enabled.store(enabled, std::memory_order_relaxed);
    LOG_INFO("[PlayerAnim] %s", enabled ? "ENABLED" : "disabled");
}

void ApplyAnimState(Actor* actor, AnimState state) noexcept
{
    if (!s_initialized.load(std::memory_order_acquire)) return;
    void* holder = GetHolder(actor);
    if (!holder) return;

    // Mirror of FO4_Wrld apply_npc_state_to_actor anim dispatch.
    switch (state)
    {
        case AnimState::Idle:
            SafeSetFloat(holder, k_SpeedSampled, 0.0f);
            SafeSetFloat(holder, k_Direction,    0.0f);
            SafeSetBool (holder, k_IsRunning,    false);
            SafeSetBool (holder, k_IsSprinting,  false);
            SafeSetBool (holder, k_bAimActive,   false);
            SafeSetInt  (holder, k_iIsInSneak,   1);
            SafeSetBool (holder, k_IsDead,       false);
            break;

        case AnimState::Walking:
            SafeSetFloat(holder, k_SpeedSampled, 100.0f);
            SafeSetFloat(holder, k_Direction,    0.0f);
            SafeSetBool (holder, k_IsRunning,    false);
            SafeSetBool (holder, k_IsSprinting,  false);
            break;

        case AnimState::Running:
            SafeSetFloat(holder, k_SpeedSampled, 200.0f);
            SafeSetFloat(holder, k_Direction,    0.0f);
            SafeSetBool (holder, k_IsRunning,    true);
            SafeSetBool (holder, k_IsSprinting,  false);
            break;

        case AnimState::Aiming:
            SafeSetFloat(holder, k_SpeedSampled, 0.0f);
            SafeSetBool (holder, k_bAimActive,   true);
            SafeSetBool (holder, k_IsRunning,    false);
            break;

        case AnimState::Firing:
            SafeSetBool (holder, k_bAimActive,   true);
            SafeSetBool (holder, k_IsRunning,    false);
            break;

        case AnimState::Reloading:
            // TODO: add reload-specific variables if needed
            SafeSetBool (holder, k_bAimActive,   true);
            break;

        case AnimState::Dead:
            SafeSetFloat(holder, k_SpeedSampled, 0.0f);
            SafeSetBool (holder, k_IsRunning,    false);
            SafeSetBool (holder, k_bAimActive,   false);
            SafeSetBool (holder, k_IsDead,       true);
            break;
    }
}

void ApplyLocomotion(Actor* actor, float speed, float direction) noexcept
{
    if (!s_initialized.load(std::memory_order_acquire)) return;
    void* holder = GetHolder(actor);
    if (!holder) return;

    SafeSetFloat(holder, k_SpeedSampled, speed);
    SafeSetFloat(holder, k_Direction,    direction);
    SafeSetBool (holder, k_IsRunning,    speed >= 150.0f);
    SafeSetBool (holder, k_IsSprinting,  false);
}

void ApplyFromSnapshot(Actor* actor, const AnimVarSnapshot& snapshot) noexcept
{
    if (!s_initialized.load(std::memory_order_acquire)) return;
    if (!actor || !snapshot.isValid) return;

    void* holder = GetHolder(actor);
    if (!holder) return;

    // Mirror of CoSyncAnimVarSystem::LoadAnimationVariables named-setter pass
    // (the block guarded by kUseEngineNamedSetters, disabled since round 122).
    // Difference: uses SafeSet* which calls the IDA-confirmed setter RVAs
    // (0x818DA0/DC0/DE0) instead of the old wrong RVAs that hung.

    const float snapSpeed     = snapshot.floats[4];
    const float snapDir       = snapshot.floats[0];
    const float snapDirSmth   = snapshot.floats[8];
    const float snapDirDamped = snapshot.floats[30];
    const float snapSpeedSmth = snapshot.floats[11];
    const float snapPitch     = snapshot.floats[6];
    const float snapTurnDelta = snapshot.floats[7];
    const float snapTurnSmth  = snapshot.floats[13];
    const float snapStaggerMag= snapshot.floats[31];

    const bool  sneaking    = (snapshot.booleans & (1ULL << 11)) != 0;
    const bool  inJumpState = (snapshot.booleans & (1ULL <<  9)) != 0;
    const bool  inLanding   = (snapshot.booleans & (1ULL << 33)) != 0;
    const bool  moving      = snapSpeed > 5.0f;

    const uint32_t wsRaw       = (snapshot.actorStateFlags2 >> 29) & 0x7u;
    const bool     weaponDrawn = (wsRaw == 2u || wsRaw == 4u || wsRaw == 6u);

    const int syncDir      = static_cast<int>(snapshot.integers[23]);
    const int syncSneak    = static_cast<int>(snapshot.integers[22]);
    const int syncSprint   = static_cast<int>(snapshot.integers[1]);
    const int syncWalkRun  = static_cast<int>(snapshot.integers[13]);
    const int locoTier     = static_cast<int>(snapshot.integers[15]);
    const int syncLocoSpd  = static_cast<int>(snapshot.integers[20]);
    const int isBackward   = static_cast<int>(snapshot.integers[25]);
    const int syncGunDown  = static_cast<int>(snapshot.integers[30]);
    const int syncAlert    = static_cast<int>(snapshot.integers[28]);
    const int syncSighted  = static_cast<int>(snapshot.integers[29]);
    const int talkGen      = static_cast<int>(snapshot.integers[31]);
    const int staggerDir   = static_cast<int>(snapshot.integers[32]);
    const int npcKiState   = (snapSpeed > 30.0f) ? 190 : 0;

    // ── Floats ────────────────────────────────────────────────────────────────
    SafeSetFloat(holder, "Speed",              snapSpeed);
    SafeSetFloat(holder, "SpeedSmoothed",      snapSpeedSmth);
    SafeSetFloat(holder, "SampledSpeed",       snapSpeed);
    SafeSetFloat(holder, "Direction",          snapDir);
    SafeSetFloat(holder, "DirectionSmoothed",  snapDirSmth);
    SafeSetFloat(holder, "DirectionDamped",    snapDirDamped);
    SafeSetFloat(holder, "DirectionDegrees",   snapDir * 57.2957795f);
    SafeSetFloat(holder, "TurnDelta",          snapTurnDelta);
    SafeSetFloat(holder, "TurnDeltaSmoothed",  snapTurnSmth);
    SafeSetFloat(holder, "Pitch",              snapPitch);
    SafeSetFloat(holder, "staggerMagnitude",   snapStaggerMag);

    // Booleans
    SafeSetBool(holder, "bIsSneaking",               sneaking);
    SafeSetBool(holder, "IsSneaking",                sneaking);
    SafeSetBool(holder, "bSyncDirection",            moving);
    SafeSetBool(holder, "IsNPC",                     true);
    SafeSetBool(holder, "IsPlayer",                  false);
    SafeSetBool(holder, "bAnimationDriven",          false);
    SafeSetBool(holder, "bAllowRotation",            false);
    SafeSetBool(holder, "bGraphDrivenTranslation",   false);
    SafeSetBool(holder, "bGraphDrivenRotation",      false);
    SafeSetBool(holder, "bFreezeSpeedUpdate",        false);
    SafeSetBool(holder, "bFreezeRotationUpdate",     false);
    SafeSetBool(holder, "bAnimateWeaponBones",       true);
    SafeSetBool(holder, "bAimEnabled",               weaponDrawn);
    SafeSetBool(holder, "bAimCaptureEnabled",        false);
    SafeSetBool(holder, "bAimActive",                false);
    SafeSetBool(holder, "bUseRifleReadyDirectAt",    false);
    SafeSetBool(holder, "bRootRifleEquipOk",         weaponDrawn);
    SafeSetBool(holder, "bEquipOkIsActiveEnabled",   weaponDrawn);
    SafeSetBool(holder, "bGraphWantsFootIK",         false);
    SafeSetBool(holder, "bNotHeadTrack",             true);
    SafeSetBool(holder, "bAllowHeadTracking",        false);
    SafeSetBool(holder, "bDisableSpineTracking",     true);
    SafeSetBool(holder, "m_bEnablePitchTwistModifier", true);
    SafeSetBool(holder, "bInJumpState",              inJumpState);
    SafeSetBool(holder, "bInLandingState",           inLanding);

    // Integers
    SafeSetInt(holder, "iSyncIdleLocomotion",    moving ? 1 : 0);
    SafeSetInt(holder, "iSyncDirection",         syncDir);
    SafeSetInt(holder, "iSyncForwardBackward",   isBackward);
    SafeSetInt(holder, "iSyncForwardBackward00", isBackward);
    SafeSetInt(holder, "iSyncWalkRun",           syncWalkRun);
    SafeSetInt(holder, "iSyncSprintState",       syncSprint);
    SafeSetInt(holder, "iLocomotionSpeedState",  locoTier);
    SafeSetInt(holder, "iSyncLocomotionSpeed",   syncLocoSpd);
    SafeSetInt(holder, "iSyncSneakWalkRun",      syncSneak);
    SafeSetInt(holder, "iSyncTurnState",         1);
    SafeSetInt(holder, "iSyncGunDown",           syncGunDown);
    SafeSetInt(holder, "iSyncReadyAlertRelaxed", syncAlert > 1 ? 1 : syncAlert);
    SafeSetInt(holder, "iSyncSightedState",      syncSighted);
    SafeSetInt(holder, "iState",                 npcKiState);
    SafeSetInt(holder, "iIsPlayer",              0);
    SafeSetInt(holder, "iTalkGenerator",         talkGen);
    SafeSetInt(holder, "staggerDirection",       staggerDir);

    static uint32_t s_applyCount = 0;
    if (++s_applyCount % 300 == 1)
        LOG_INFO("[PlayerAnim] ApplyFromSnapshot #%u actor=0x%08X: "
                 "spd=%.1f dir=%.2f sneak=%d weaponDrawn=%d npcKiState=%d",
                 s_applyCount, actor->formID,
                 snapSpeed, snapDir,
                 sneaking ? 1 : 0, weaponDrawn ? 1 : 0, npcKiState);
}

void Tick(float deltaTime)
{
    (void)deltaTime;
}

// ============================================================================
// Skeleton bone replication — FO4_Wrld pipeline
// ============================================================================
//
// NiAVObject memory layout:
//   +0x10  NiFixedString* (ptr to pool_entry; actual C string at pool_entry+0x18)
//   +0x28  parent NiNode*
//   +0x30  local rotation NiMatrix3 (3 x NiPoint4, SIMD-padded: 12 floats, use [0-2,4-6,8-10])
//   +0x60  local translation vec3 (3 floats)
//   +0x6C  local scale float
//   +0x108 flags uint64 (bit 0x2 = dirty, triggers UpdateDownwardPass recompute)
// NiNode children:
//   +0x128 void** children array
//   +0x132 uint16_t child count
//
// All raw-pointer accesses are wrapped in SEH helpers to guard against invalid
// actor references or partially-unloaded skeletons.

// ── SEH helpers (no C++ objects with destructors in these functions) ──────────

// __declspec(noinline) is load-bearing here, not decorative: this function
// was NOT noinline as of 2026-08-20, and a ground-truth PDB-resolved VEH
// crash stack (repeat #3121+, RIP=Fallout4.exe+0x16BACC9, a null vtable
// dispatch reached via actor->GetActorRootNode) landed exactly at
// ApplySkeleton+0x50 -- inside where this call is inlined, not inside a
// call to a separate SafeReadSkelRoot frame. The __try/__except below never
// ran (no matching log line ever appeared for the thousands of occurrences
// in one session): the compiler inlined this into ApplySkeleton/WalkCapture
// and dropped the SEH scope table, the exact same failure mode already
// diagnosed and fixed with noinline in CoSyncAnimUtils::GetFloat. See
// [[project-cosync-fp-graph-drive-rvas]] pt.20 for the full trace.
//
// 2026-08-24 -- `firstPerson` added, defaulting to false so every existing
// caller (SampleBoneRotation's diagnostics, ApplySkeleton's proxy walk) keeps
// the exact third-person behavior it had. ONLY CaptureBones passes true, and
// only for the LOCAL player while their own camera is in first person -- a
// proxy is never in first person and must always be walked as third person.
__declspec(noinline) static void* SafeReadSkelRoot(Actor* actor, bool firstPerson = false) noexcept
{
    __try {
        // REFR_LOADED_3D_OFF was a hand-guessed raw offset that does NOT match
        // the SDK-verified TESObjectREFR layout (sizeof(TESObjectREFR)==0x110;
        // the loaded-3D root is reached via unkF0->rootNode, a DOUBLE
        // indirection, not a single pointer at +0xB78). That bogus offset was
        // reading some other consistently-present-but-unrelated field, which
        // is why WalkCapture always found exactly 20 "bones" on the sender
        // (not real skeleton data) while WalkAllNodes found 0 on proxies (the
        // same field was null/different there) -- the whole feature was never
        // touching a real skeleton.
        //
        // Use GetActorRootNode(false) (vtable slot 0x8B, explicit third
        // person) rather than GetObjectRootNode() (0x8C): live testing after
        // the first fix showed GetObjectRootNode() on the LOCAL player
        // specifically still returns a small (~20-node), completely unnamed
        // substructure -- almost certainly the first-person arm/pipboy rig,
        // not the 3rd person body -- while the exact same call on a remote
        // PROXY (never the local player) correctly returned the real named
        // body skeleton (skeleton.nif/Root/COM/Pelvis/LLeg_Thigh/...).
        // Copying first-person arm transforms onto a 3rd-person body by
        // index is exactly what produced the reported corruption (inverted
        // body, pipboy arm stretched a car's length out, legs folded inside
        // the torso). GetActorRootNode(bool firstPerson) is the SDK's
        // explicit primitive for this (same call PapyrusObjectReference.cpp
        // uses for its own GetPosition natives).
        //
        // 2026-08-24 -- the "~20-node, completely unnamed substructure" that
        // GetObjectRootNode() returned above was blamed on "almost certainly
        // the first-person arm/pipboy rig", and that inference is what pinned
        // every capture path in this project to third person. It is wrong on
        // two counts. GetObjectRootNode is vtable slot 0x8C; GetActorRootNode
        // is 0x8B -- different calls. And the real first-person skeleton is
        // not a 20-node arm rig: _1stperson/CharacterAssets/skeleton.nif has
        // 113 node blocks, 77 of them named non-_skin, and that set is a
        // strict SUBSET of third person's 77-of-81 by name. Whatever that
        // unnamed 20-node thing was, it was not this. See
        // IsFirstPersonCaptureEnabled() in the header for the full
        // measurement trail behind passing `true` here.
        //
        // The corruption that comment describes (inverted body, pipboy arm
        // stretched out, legs folded into the torso) was a BY-INDEX apply
        // against a mismatched tree. The wire format has since been rebuilt
        // around HashBoneName and ApplySkeleton matches by name, so that
        // failure mode is structurally gone -- a name that does not exist on
        // the proxy is simply skipped.
        return static_cast<void*>(actor->GetActorRootNode(firstPerson));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

__declspec(noinline) static const char* SafeReadBoneName(void* node) noexcept
{
    __try {
        const void* pool_entry = *reinterpret_cast<const void**>(
            reinterpret_cast<uint8_t*>(node) + 0x10);
        if (!pool_entry) return nullptr;
        return reinterpret_cast<const char*>(
            reinterpret_cast<const uint8_t*>(pool_entry) + 0x18);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Confirmed 2026-08-20 via FO4_Wrld's own hard-won lesson (write_local_3x3's
// "DETACH PROBE", scene_inject.cpp): a node removed from the scene graph
// (~NiNode / NiNode::DetachChild) has its parent pointer at +0x28 nulled,
// but its vtable/refcount/name are left untouched -- meaning a stale cached
// proxy-node pointer we hold can point at memory that's still fully mapped
// and writable (SEH catches nothing) yet no longer part of any live tree,
// or has since been reallocated for something else entirely. Their own
// words: "the check must be semantic, not exceptional." CoSync's proxy
// writer (SafeWriteRotation/Translation below) never had this guard --
// walking WalkAllNodes fresh every ApplySkeleton call sidesteps the worst
// of it (a genuinely-detached node wouldn't be found via a live children-
// pointer walk from the current root), but a node that was live at walk
// time and got detached/recycled in between the walk and the write later
// in the same call is exactly the risk this closes.
__declspec(noinline) static bool IsNodeAttached(void* node) noexcept
{
    __try {
        return *reinterpret_cast<void* const*>(
            reinterpret_cast<const uint8_t*>(node) + 0x28) != nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ============================================================================
// 2026-08-20 -- pinned per-proxy bone cache (FO4_Wrld Build 68.4 port)
// ============================================================================
//
// Ported from FO4_Wrld's ni_incref/ni_decref (scene_inject.cpp) -- the ONLY
// code that frees a NiNode is its scalar deleting destructor, reachable
// solely through vtable slot 1 (NiRefObject::DeleteThis), invoked solely from
// `if (InterlockedDecrement(obj+0x08) == 0) vt[1](obj)` sites throughout the
// engine. Holding one extra reference via +0x08 (the universal NiRefObject
// refcount field -- already relied on implicitly everywhere in this codebase
// that touches a live Ni object) therefore makes it IMPOSSIBLE for the engine
// to free a cached bone pointer out from under us between cache rebuilds,
// replacing "probably still there" (IsNodeAttached's parent-nonnull check,
// still kept as cheap defense-in-depth below) with "physically cannot have
// been freed." noinline for the same reason every other SEH helper in this
// file needs it -- see SafeReadSkelRoot's comment above.
static constexpr size_t NIAV_REFCOUNT_OFF = 0x08;

__declspec(noinline) static void NiIncRef(void* obj) noexcept
{
    if (!obj) return;
    __try {
        _InterlockedIncrement(reinterpret_cast<volatile long*>(
            reinterpret_cast<uint8_t*>(obj) + NIAV_REFCOUNT_OFF));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static void NiDecRef(void* obj) noexcept
{
    if (!obj) return;
    __try {
        const long prev = _InterlockedExchangeAdd(
            reinterpret_cast<volatile long*>(
                reinterpret_cast<uint8_t*>(obj) + NIAV_REFCOUNT_OFF), -1);
        if (prev == 1)
        {
            using DtorFn = void(__fastcall*)(void*);
            void** vt = *reinterpret_cast<void***>(obj);
            reinterpret_cast<DtorFn>(vt[1])(obj);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Per-proxy cache: name->node lookup built once per 3D-root change instead
// of every ApplySkeleton call (was: full WalkAllNodes, up to 128 nodes/depth
// 12, EVERY tick for EVERY proxy). Every stored pointer is pinned (NiIncRef'd)
// on build and released (NiDecRef'd) on rebuild/ReleaseBoneCache -- mirrors
// FO4_Wrld's NpcBoneCache (scene_inject.cpp ~12390).
struct BoneCacheEntry
{
    // One pinned proxy node plus the two per-bone decisions that need the
    // node's NAME to make. Resolved once at rebuild time because a received
    // capture only carries name hashes -- by apply time the names are gone.
    struct CachedBone
    {
        void* node      = nullptr;
        bool  translate = false;  // COM/Pelvis: the only bones that carry translation
        bool  upperBody = false;  // drives HOLD-tier gating + smoothing factor
    };

    void*                                     root = nullptr; // GetActorRootNode(false) when built
    std::unordered_map<uint32_t, CachedBone>  nodesByHash;    // pinned, keyed by HashBoneName

    // nlerp state: last-applied + prev/cur target quaternions per bone,
    // for the smoothing pass in ApplySkeleton (mirrors FO4_Wrld's c.42
    // kNpcPoseSmooth + extrapolation). Keyed by the SAME hash as nodesByHash.
    struct SmoothState
    {
        float appliedQuat[4] = { 0.f, 0.f, 0.f, 1.f };
        float curQuat[4]     = { 0.f, 0.f, 0.f, 1.f };
        float prevQuat[4]    = { 0.f, 0.f, 0.f, 1.f };
        uint64_t curSetMs    = 0;    // GetTickCount64() when curQuat last changed
        uint64_t prevSetMs   = 0;    // GetTickCount64() when curQuat became prevQuat
        bool  hasPrev        = false;
        bool  primed         = false; // false -> first apply snaps, no glide-in

        // Write-order probe (see ApplySkeleton): the exact matrix we last
        // wrote to this node, and whether it was still there when we came
        // back a frame later.
        float lastWrote[9]   = {};
        bool  hasLastWrote   = false;
        bool  lastSurvived   = false;
    };
    std::unordered_map<uint32_t, SmoothState> smooth;
    uint64_t lastTouchedMs = 0;         // for the age-based sweep below
    void*    graphRootNode = nullptr;   // the "Root" bone -- key of the reassert record below
};

// ============================================================================
// 2026-08-21 -- write-last re-stamp store (see ReassertPoseForGraphRoot)
// ============================================================================
//
// Deliberately NOT part of BoneCacheEntry. g_boneCaches is main-thread-only by
// construction and several of its operations rehash; the engine's animation
// update, where the re-stamp runs, is not guaranteed to be the main thread.
// This is a separate, self-contained snapshot with its own lock: the main
// thread publishes finished matrices into it, the engine's thread reads them
// back out. Nothing else is shared.
struct PoseWrite
{
    void* node = nullptr;
    float rot[9] = {};
    bool  hasTrans = false;
    float trans[3] = {};
};

struct ReassertRecord
{
    void*                  skelRoot = nullptr; // BSFadeNode, for UpdateDownwardPass
    uint32_t               formID   = 0;
    uint64_t               stampMs  = 0;       // last ApplySkeleton publish
    std::vector<PoseWrite> writes;
};

static std::mutex                                g_reassertMutex;
static std::unordered_map<void*, ReassertRecord> g_reassertByGraphRoot;
static std::atomic<uint64_t>                     g_reassertCount{0};

// If ApplySkeleton stops publishing (RELEASE tier, proxy despawned, mode
// turned off), the record goes stale and the re-stamp stops on its own --
// no extra bookkeeping, and no risk of a released proxy being pinned to a
// pose by a hook the release path forgot to tell. ApplySkeleton republishes
// every frame, so anything older than a few frames means it has stopped.
static constexpr uint64_t kReassertMaxAgeMs = 100;

static std::unordered_map<Actor*, BoneCacheEntry> g_boneCaches; // main-thread only, no lock needed

static void ReleaseBoneCacheEntry(BoneCacheEntry& entry) noexcept
{
    // Drop the re-stamp record FIRST: it holds raw copies of these node
    // pointers, and the NiDecRef below can be the last reference. Doing it in
    // the other order leaves a window where the engine's thread could write
    // through a pointer this thread just freed.
    if (entry.graphRootNode)
    {
        std::lock_guard<std::mutex> lk(g_reassertMutex);
        g_reassertByGraphRoot.erase(entry.graphRootNode);
        entry.graphRootNode = nullptr;
    }

    for (auto& kv : entry.nodesByHash)
        if (kv.second.node) NiDecRef(kv.second.node);
    entry.nodesByHash.clear();
    entry.smooth.clear();
    entry.root = nullptr;
}

// Self-throttled ~250ms age sweep (FO4_Wrld's sweep_npc_bone_caches, Build
// 68.4): event-driven release alone (ReleaseBoneCache, called from proxy
// despawn) can miss a handoff -- a sweep cannot miss an event because it does
// not depend on one. Any entry not touched (rebuilt-or-confirmed-current) by
// ApplySkeleton in the last 2s is assumed to belong to a departed proxy.
static void SweepStaleBoneCaches() noexcept
{
    static uint64_t s_nextSweepMs = 0;
    const uint64_t now = GetTickCount64();
    if (now < s_nextSweepMs) return;
    s_nextSweepMs = now + 250;

    for (auto it = g_boneCaches.begin(); it != g_boneCaches.end(); )
    {
        if (now - it->second.lastTouchedMs > 2000)
        {
            ReleaseBoneCacheEntry(it->second);
            it = g_boneCaches.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

__declspec(noinline) static bool SafeReadRotation(void* node, float out[9]) noexcept
{
    __try {
        const float* p = reinterpret_cast<const float*>(
            reinterpret_cast<uint8_t*>(node) + 0x30);
        out[0]=p[0]; out[1]=p[1]; out[2]=p[2];
        out[3]=p[4]; out[4]=p[5]; out[5]=p[6];
        out[6]=p[8]; out[7]=p[9]; out[8]=p[10];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static bool SafeReadTranslation(void* node, float out[3]) noexcept
{
    __try {
        const float* p = reinterpret_cast<const float*>(
            reinterpret_cast<uint8_t*>(node) + 0x60);
        out[0]=p[0]; out[1]=p[1]; out[2]=p[2];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static bool SafeReadScale(void* node, float& out) noexcept
{
    __try {
        out = *reinterpret_cast<const float*>(
            reinterpret_cast<uint8_t*>(node) + 0x6C);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static void SafeWriteRotation(void* node, const float rot[9]) noexcept
{
    __try {
        float* p = reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(node) + 0x30);
        p[0]=rot[0]; p[1]=rot[1]; p[2]=rot[2]; p[3]=0.0f;
        p[4]=rot[3]; p[5]=rot[4]; p[6]=rot[5]; p[7]=0.0f;
        p[8]=rot[6]; p[9]=rot[7]; p[10]=rot[8]; p[11]=0.0f;
        *reinterpret_cast<uint64_t*>(
            reinterpret_cast<uint8_t*>(node) + 0x108) |= 0x2ULL;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static void SafeWriteTranslation(void* node, const float trans[3]) noexcept
{
    __try {
        float* p = reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(node) + 0x60);
        p[0]=trans[0]; p[1]=trans[1]; p[2]=trans[2];
        *reinterpret_cast<uint64_t*>(
            reinterpret_cast<uint8_t*>(node) + 0x108) |= 0x2ULL;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Valid canonical Windows user-space pointer: 0x10000 <= v < 0x0001_0000_0000_0000.
// Leaf NiAVObject nodes (NiTriShape etc.) have non-NiNode data at +0x128 — reading
// their "children" field yields non-canonical garbage like 0xBD1E3B99B35D04BD that
// would fault. Reject those before touching them so no exception is ever raised.
static bool IsValidUserPtr(const void* p) noexcept
{
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000ULL && v < 0x0001000000000000ULL;
}

// FNV-1a 32. This is the bone's identity on the wire and the key of the
// per-proxy node cache. Chosen over the old canonical index because it needs
// no agreement between machines at all (see the header's wire-format note),
// and over the raw name string because a name costs 10-20 bytes per bone
// against 8 hex digits. Collision odds across ~50 bones in a 2^32 space are
// about 3 in ten million -- and a collision would mis-drive one bone, not
// shift every bone after it, which is what the index scheme did on any
// disagreement. Empty/unnamed nodes hash to 0 and are treated as "no
// identity" everywhere below.
static uint32_t HashBoneName(const char* name) noexcept
{
    if (!name || !name[0]) return 0u;
    uint32_t h = 2166136261u;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p)
    {
        h ^= *p;
        h *= 16777619u;
    }
    return h ? h : 1u; // never collide with the "no identity" sentinel
}

// Upper body vs lower body, decided from the node's own name at cache-build
// time. Two things depend on it: the HOLD tier drives only the upper body, and
// the smoothing factor is split (FO4_Wrld kNpcPoseSmoothUpper/Lower).
//
// Fallout 4's human skeleton names every leg joint with a "Leg" infix
// (LLeg_Thigh, RLeg_Calf, LLeg_Foot ...), so the reliable test is to call out
// the lower body explicitly and treat everything else -- spine, neck, head,
// both arm chains, weapon and camera nodes -- as upper. COM and Pelvis count
// as lower: they are the root of the walk cycle, and releasing them during a
// HOLD is what lets the proxy's own graph resume locomotion cleanly.
static bool IsUpperBodyBoneName(const char* name) noexcept
{
    if (!name || !name[0]) return true;
    static const char* const kLower[] = {
        "Leg", "Thigh", "Calf", "Foot", "Toe", "Pelvis", "COM"
    };
    for (const char* frag : kLower)
        if (std::strstr(name, frag) != nullptr)
            return false;
    return true;
}

// Returns true if name ends with "_skin" (skin anchor stubs — skip these).
// 2026-08-24 -- case-INSENSITIVE. The vanilla third-person skeleton.nif
// spells one of these "Chest_Rear_Skin" with a capital S (verified against
// the shipped mesh: 48 of its 50 skin nodes are "_skin", that one is "_Skin").
// The old exact-lowercase compare let it and its entire subtree through the
// filter and onto the wire every frame.
static bool EndsWithSkin(const char* name) noexcept
{
    if (!name) return false;
    size_t len = strlen(name);
    return len >= 5
        && name[len-5] == '_'
        && (name[len-4] == 's' || name[len-4] == 'S')
        && (name[len-3] == 'k' || name[len-3] == 'K')
        && (name[len-2] == 'i' || name[len-2] == 'I')
        && (name[len-1] == 'n' || name[len-1] == 'N');
}

// Roots of attached MODEL hierarchies -- geometry, not skeleton. Skip the
// node and everything under it, exactly as for a skin stub.
//
// 2026-08-24, from the live capture inventory. This was costing real wire
// bytes in BOTH camera modes and had nothing to do with poses:
//   third person, 57 captured -- ~10 nodes under PipboyRoot (Screen,
//     c_screen, Screen:0, ScreenGlowEffect01, PipboyBodyLowPlayer:0..8) and
//     ~11 under BSFaceGenNiNodeSkinned (Beard01, HairMale01,
//     HairMale01_Hairline, MaleHeadHuman, MouthShadowMale,
//     MaleEyesHumanHazel/Lashes/AO/Wet, MaleNeckGore). Only about 29 of the
//     57 were ever real bones.
//   first person, 79 captured -- 68 of them the entire Pip-Boy model
//     (PipboyHinge, PipboyClasp01-03, AdjustKnob, ModeKnob, TuneKnob,
//     ScrollKnob, SelectButton, LightButton, RadioNeedle, RadNeedle,
//     EjectButton, TapeDeck01/02, TapREF, TapeDeckLid, PowerButton,
//     ScreenStealthBoy, ScreenDust, and every *_mesh:N under them).
//
// Worse than waste: several of these names (Screen, c_screen, Screen:0,
// ScreenGlowEffect01) appear in BOTH inventories, so name-hash matching was
// happily writing the sender's Pip-Boy sub-mesh transforms onto the proxy's
// own Pip-Boy. PipboyBone itself is a genuine skeleton bone (it is in the
// shipped rig at index 25, parented to LArm_ForeArm3) and is deliberately
// NOT in this list -- it is the attachment point, and it should keep moving.
static bool IsModelSubtreeRoot(const char* name) noexcept
{
    if (!name || !name[0]) return false;
    return std::strcmp(name, "PipboyRoot") == 0
        || std::strcmp(name, "BSFaceGenNiNodeSkinned") == 0;
}

// Runtime attachment nodes named after a FormID, e.g. " (0001ED3D)[0]/ (00021B3B)".
// They sit between a skeleton bone and an attached model. The name embeds
// instance-specific FormIDs, so its hash is not stable identity between two
// machines -- capture it and you are shipping bytes keyed on a name the
// receiver can never match. Skip the NODE but keep descending: on the third-
// person tree these are what the head and Pip-Boy hang from, and the
// IsModelSubtreeRoot test below them does the real pruning.
static bool IsFormIdAttachNode(const char* name) noexcept
{
    return name && std::strchr(name, '(') != nullptr;
}

// Animation-skeleton bone order, straight out of the shipped rig
// (meshes/actors/character/CharacterAssets/skeleton.hkx, read with Behaviour
// Graph Studio's own HkxBinaryReader). The generated-pose buffer is indexed by
// this order and carries no names, so this table is what turns a pose index
// into the name-hash identity the wire format already uses.
//
// The first-person rig (_1stperson/CharacterAssets/skeleton.hkx) is the SAME
// list for indices 0..93 -- same names, same parent indices, and reference
// poses matching to within 0.001 on every body bone; third person only appends
// CamTarget at 94. So one table serves both graphs, and the runtime bone
// counts confirm it: the pose probe reads bones=95 off the third-person graph
// and bones=94 off the first-person one.
//
// Index 78 and 79 are "Camera" and "Camera Control"; the shipped file spells
// the second one with a space, and IsCameraNode drops both anyway.
static const char* const kAnimBoneNames[] = {
    "Root","COM","Pelvis","LLeg_Thigh","LLeg_Calf","LLeg_Foot",
    "RLeg_Thigh","RLeg_Calf","RLeg_Foot","Spine1","Spine2","Chest",
    "Neck","Head","LArm_Collarbone","LArm_UpperArm","LArm_ForeArm1","LArm_ForeArm2",
    "LArm_ForeArm3","LArm_Hand","RArm_Collarbone","RArm_UpperArm","RArm_ForeArm1","RArm_ForeArm2",
    "RArm_ForeArm3","PipboyBone","RArm_Hand","WeaponLeft","Weapon","WeaponBolt",
    "WeaponExtra1","WeaponExtra2","WeaponExtra3","WeaponMagazine","WeaponMagazineChild1","WeaponMagazineChild2",
    "WeaponMagazineChild3","WeaponMagazineChild4","WeaponMagazineChild5","WeaponOptics1","WeaponOptics2","WeaponTrigger",
    "LArm_UpperTwist1","LArm_UpperTwist2","RArm_UpperTwist1","RArm_UpperTwist2","LLeg_Toe1","RLeg_Toe1",
    "LArm_Finger11","LArm_Finger12","LArm_Finger13","LArm_Finger21","LArm_Finger22","LArm_Finger23",
    "LArm_Finger31","LArm_Finger32","LArm_Finger33","LArm_Finger41","LArm_Finger42","LArm_Finger43",
    "LArm_Finger51","LArm_Finger52","LArm_Finger53","RArm_Finger11","RArm_Finger12","RArm_Finger13",
    "RArm_Finger21","RArm_Finger22","RArm_Finger23","RArm_Finger31","RArm_Finger32","RArm_Finger33",
    "RArm_Finger41","RArm_Finger42","RArm_Finger43","RArm_Finger51","RArm_Finger52","RArm_Finger53",
    "Camera","Camera Control","AnimObjectA","AnimObjectB","WeaponIKTargetL","WeaponIKTargetR",
    "WeaponIKTargetLMirror","WeaponIKTargetRMirror","AnimObjectL1","AnimObjectL2","AnimObjectL3","AnimObjectR1",
    "AnimObjectR2","AnimObjectR3","L_RibHelper","R_RibHelper","CamTarget",
};
static constexpr int kAnimBoneCount = static_cast<int>(sizeof(kAnimBoneNames) / sizeof(kAnimBoneNames[0]));

// Quaternion (xyzw) -> 3x3, written into the same row-major layout
// SafeReadRotation produces from NiMatrix3's rows.
//
// `transposed` exists because the handedness/row-vs-column convention is the
// one thing here that cannot be settled by reading the engine's structures --
// it only shows up when the two representations are compared against each
// other. ValidatePoseSnapshot scores both against the live node matrices in
// third person, where both sources exist, and says which one is right rather
// than guessing and shipping a mirrored skeleton.
static void QuatToMatrix(const float q[4], bool transposed, float m[9]) noexcept
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x*x, yy = y*y, zz = z*z;
    const float xy = x*y, xz = x*z, yz = y*z;
    const float wx = w*x, wy = w*y, wz = w*z;

    float r[9];
    r[0] = 1.0f - 2.0f*(yy + zz);  r[1] = 2.0f*(xy - wz);         r[2] = 2.0f*(xz + wy);
    r[3] = 2.0f*(xy + wz);         r[4] = 1.0f - 2.0f*(xx + zz);  r[5] = 2.0f*(yz - wx);
    r[6] = 2.0f*(xz - wy);         r[7] = 2.0f*(yz + wx);         r[8] = 1.0f - 2.0f*(xx + yy);

    if (!transposed) { for (int i = 0; i < 9; ++i) m[i] = r[i]; return; }
    m[0] = r[0]; m[1] = r[3]; m[2] = r[6];
    m[3] = r[1]; m[4] = r[4]; m[5] = r[7];
    m[6] = r[2]; m[7] = r[5]; m[8] = r[8];
}

// Nodes that exist on both rigs but mean nothing on a proxy. "Camera" and
// "Camera Control" ARE driven by the shipped first-person locomotion clips
// (both appear in runforward.hkx's moving-track set), so capturing from the
// first-person skeleton would start putting them on the wire where the
// third-person capture never did. They steer the local player's view, not a
// body; sending them is pure waste and applying them to a proxy is worse.
static bool IsCameraNode(const char* name) noexcept
{
    if (!name) return false;
    return std::strcmp(name, "Camera") == 0
        || std::strcmp(name, "Camera Control") == 0
        || std::strcmp(name, "CamTarget") == 0
        || std::strcmp(name, "CamTargetParent") == 0;
}

// ── Recursive capture walk ────────────────────────────────────────────────────
//
// Children access uses a single inline __try per stack frame. Any fault
// (garbage pointer, bad count, unmapped page) aborts THIS NODE's children
// in one catch — not per-child — so a garbage count never drives thousands
// of VEH firings. Depth capped at 12; NiNode child count capped at 32.

// The +0x30 local matrix is rotation*scale, not rotation alone -- each row's
// own magnitude carries whatever non-uniform scale is baked into it (this is
// separate from the uniform scale scalar at +0x6C SafeReadScale reads).
// Confirmed live-relevant 2026-08-20 by reading FO4_Wrld's own capture code
// (fw_native/src/native/scene_inject.cpp, "STRIP SCALE BEFORE THE
// QUATERNION"): the engine deliberately COLLAPSES certain 3rd-person bones
// (scales them toward zero on specific axes) while the camera is in first
// person, so they render invisibly rather than intersecting the camera.
// Sending that scale-contaminated matrix un-normalized produces a degenerate
// rotation on the receiving proxy -- their own account: "that is what
// deformed the ghost's head into a cone the moment the sender entered first
// person: the engine's 1P trick collapses the 3P head bone ... and we
// faithfully replicated the collapse." CoSync's own capture never stripped
// this. Returns false (bone should be treated as unreadable this frame, same
// as an SEH/read failure) when any row is too close to zero to recover a
// rotation from; normalizes each row in place otherwise.
static bool NormalizeRotationRows(float m[9]) noexcept
{
    const float r0 = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    const float r1 = std::sqrt(m[3]*m[3] + m[4]*m[4] + m[5]*m[5]);
    const float r2 = std::sqrt(m[6]*m[6] + m[7]*m[7] + m[8]*m[8]);
    if (!(r0 > 1e-3f) || !(r1 > 1e-3f) || !(r2 > 1e-3f))
        return false;
    m[0] /= r0; m[1] /= r0; m[2] /= r0;
    m[3] /= r1; m[4] /= r1; m[5] /= r1;
    m[6] /= r2; m[7] /= r2; m[8] /= r2;
    return true;
}

// Shepperd's method: row-major 3x3 -> quaternion (qx,qy,qz,qw). Ported
// verbatim from FO4_Wrld's scene_inject.cpp mat3_to_quat -- standard,
// build-independent math, no engine offsets involved.
static void Mat3ToQuat(const float m[9], float q[4]) noexcept
{
    const float trace = m[0] + m[4] + m[8];
    float qw, qx, qy, qz;
    if (trace > 0) {
        const float s = 0.5f / std::sqrt(trace + 1.0f);
        qw = 0.25f / s;
        qx = (m[7] - m[5]) * s;
        qy = (m[2] - m[6]) * s;
        qz = (m[3] - m[1]) * s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = 2.0f * std::sqrt(1.0f + m[0] - m[4] - m[8]);
        qw = (m[7] - m[5]) / s;
        qx = 0.25f * s;
        qy = (m[1] + m[3]) / s;
        qz = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        const float s = 2.0f * std::sqrt(1.0f + m[4] - m[0] - m[8]);
        qw = (m[2] - m[6]) / s;
        qx = (m[1] + m[3]) / s;
        qy = 0.25f * s;
        qz = (m[5] + m[7]) / s;
    } else {
        const float s = 2.0f * std::sqrt(1.0f + m[8] - m[0] - m[4]);
        qw = (m[3] - m[1]) / s;
        qx = (m[2] + m[6]) / s;
        qy = (m[5] + m[7]) / s;
        qz = 0.25f * s;
    }
    q[0] = qx; q[1] = qy; q[2] = qz; q[3] = qw;
}

// Quaternion (qx,qy,qz,qw) -> row-major 3x3 rotation. Ported verbatim from
// FO4_Wrld's scene_inject.cpp quat_to_mat3.
static void QuatToMat3(const float q[4], float m[9]) noexcept
{
    const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    const float xx = qx*qx, yy = qy*qy, zz = qz*qz;
    const float xy = qx*qy, xz = qx*qz, yz = qy*qz;
    const float wx = qw*qx, wy = qw*qy, wz = qw*qz;
    m[0] = 1.0f - 2.0f*(yy + zz);
    m[1] = 2.0f*(xy - wz);
    m[2] = 2.0f*(xz + wy);
    m[3] = 2.0f*(xy + wz);
    m[4] = 1.0f - 2.0f*(xx + zz);
    m[5] = 2.0f*(yz - wx);
    m[6] = 2.0f*(xz - wy);
    m[7] = 2.0f*(yz + wx);
    m[8] = 1.0f - 2.0f*(xx + yy);
}

// Shortest-path nlerp of `cur` toward `target` by fraction t, normalized.
// If the dot product is negative, negate target first (quaternions q/-q
// represent the same rotation; nlerp-ing the "wrong side" spins the long
// way around). Falls back to `target` verbatim if the result degenerates.
static void NlerpQuat(const float cur[4], const float target[4], float t, float out[4]) noexcept
{
    float tgt[4] = { target[0], target[1], target[2], target[3] };
    const float dot = cur[0]*tgt[0] + cur[1]*tgt[1] + cur[2]*tgt[2] + cur[3]*tgt[3];
    if (dot < 0.0f) { tgt[0] = -tgt[0]; tgt[1] = -tgt[1]; tgt[2] = -tgt[2]; tgt[3] = -tgt[3]; }
    for (int i = 0; i < 4; ++i) out[i] = cur[i] + (tgt[i] - cur[i]) * t;
    const float len = std::sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2] + out[3]*out[3]);
    if (len > 1e-6f) { out[0] /= len; out[1] /= len; out[2] /= len; out[3] /= len; }
    else { out[0] = target[0]; out[1] = target[1]; out[2] = target[2]; out[3] = target[3]; }
}

// ============================================================================
// 2026-08-21 -- the canonical joint-name list is GONE
// ============================================================================
//
// What used to live here: a per-machine list of bone names, built once from
// the first skeleton this process happened to walk, sorted alphabetically and
// then frozen, whose INDEX was the bone's identity on the wire. Plus
// LockInCanonicalNames / EnsureCanonicalNames / EnsureCanonicalNamesFromProxy
// / SnapshotCanonicalNames and a mutex, plus a public
// TrySeedCanonicalFromActor() that CoSyncPlayer::TickAnimVars had to call on
// every proxy every tick to work around a decode deadlock the scheme created.
//
// All of it existed to make two independently-built lists agree, and it could
// not. Sorting fixes ORDER for a shared SET; the sets are not shared. Measured
// live on ONE machine: local player walks 48 nodes, a proxy walks 47, 42 names
// match. See the wire-format note in CoSyncPlayerAnimation.h for the full trace.
//
// Bone identity now travels with the bone as a name hash (HashBoneName), so
// there is nothing left to agree on, nothing to seed, and nothing to deadlock
// on. ~90 lines deleted, one public function removed.

// Depth ceiling for both skeleton walks.
//
// 2026-08-21: raised from 12 to 20. The capture root is the BSFadeNode at
// depth 0 with "Root" as its child at depth 1 (confirmed live by the FP-drive
// diagnostic's `writeTargetNode0 ... name='Root' ... sameTree(depth)=1`), so
// an arm chain reaches Collarbone at 7, UpperArm 8, ForeArm 9, ForeTwist 10,
// Hand 11 -- and the finger joints at 12+ were being silently dropped. The
// live numbers from the 2026-08-21 session say the same thing from the other
// side: the engine's own graph reported `boneCount=95` for the very skeleton
// CaptureBones walked to only 48 nodes in the same frame. 20 clears the
// deepest human finger chain with room to spare; MAX_SKELETON_BONES (128) and
// the per-node 32-child cap still bound the total work.
static constexpr int kMaxWalkDepth = 20;

// Walk the player skeleton and capture every non-skin node in DFS order.
// Only NAMED nodes are captured: the bone's name hash is its identity on the
// wire and in the proxy cache, so an unnamed node has nothing to match on.
// bones[0] is the root BSFadeNode (world-space actor position); callers that
// write to the proxy must skip index 0 to avoid teleporting the proxy.
__declspec(noinline) static void WalkCapture(void* node, int depth,
                        CoSyncPlayerAnimation::BoneTransform* bones,
                        uint32_t& count) noexcept
{
    if (!node || depth > kMaxWalkDepth || count >= CoSyncPlayerAnimation::MAX_SKELETON_BONES)
        return;

    const char* name = SafeReadBoneName(node);
    if (EndsWithSkin(name)) return;        // skip skin nodes + their subtrees
    if (IsModelSubtreeRoot(name)) return;  // skip attached model geometry too

    CoSyncPlayerAnimation::BoneTransform& bt = bones[count];
    bool ok  = SafeReadRotation(node, bt.rot);
    ok      &= SafeReadTranslation(node, bt.trans);
    // A collapsed/scaled-to-near-zero bone is not a read failure (the memory
    // is valid), but its rotation is unrecoverable -- skip adding it to this
    // frame's capture rather than send a degenerate matrix. Since
    // ApplySkeleton matches by name and only touches bones present in the
    // packet, omitting it here means the proxy simply keeps whatever it last
    // had for that bone -- the same "receiver keeps last good value" outcome
    // FO4_Wrld gets from their sentinel value, without needing wire-format
    // changes.
    if (ok) ok = NormalizeRotationRows(bt.rot);
    // IsCameraNode / IsFormIdAttachNode: drop the node itself but keep
    // descending -- unlike a skin or model node these are not roots of a
    // subtree we want to lose.
    if (ok && !IsCameraNode(name) && !IsFormIdAttachNode(name))
    {
        if (name && name[0] != '\0')
            strncpy_s(bt.name, sizeof(bt.name), name, _TRUNCATE);
        else
            bt.name[0] = '\0';
        bt.nameHash = HashBoneName(bt.name);
        if (!SafeReadScale(node, bt.scale)) bt.scale = 1.0f;
        ++count;
    }

    __try
    {
        void** children  = *reinterpret_cast<void***>(
            reinterpret_cast<uint8_t*>(node) + 0x128);

        if (!IsValidUserPtr(children)) return;

        uint32_t nch = *reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(node) + 0x132);
        if (nch > 32u) nch = 32u;

        for (uint32_t i = 0; i < nch && count < CoSyncPlayerAnimation::MAX_SKELETON_BONES; ++i)
        {
            void* child = children[i];
            if (child && IsValidUserPtr(child))
                WalkCapture(child, depth + 1, bones, count);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Walk the proxy skeleton and collect node pointers + names. DFS order is no
// longer relied on to line up with the sender's order (see ApplySkeleton --
// matching is by name now, since two skeletons' traversal order/count can
// legitimately differ frame to frame with equipped gear), but `names` lets
// the caller build a name->node lookup in one pass.
__declspec(noinline) static void WalkAllNodes(void* node, int depth, void** nodes, const char** names, uint32_t& count) noexcept
{
    if (!node || depth > kMaxWalkDepth || count >= CoSyncPlayerAnimation::MAX_SKELETON_BONES) return;

    const char* name = SafeReadBoneName(node);
    if (EndsWithSkin(name)) return;

    // Mirror WalkCapture's validation: only include nodes where rotation+translation read
    float tmpR[9], tmpT[3];
    if (SafeReadRotation(node, tmpR) && SafeReadTranslation(node, tmpT))
    {
        nodes[count] = node;
        names[count] = name;
        ++count;
    }

    __try
    {
        void** children = *reinterpret_cast<void***>(
            reinterpret_cast<uint8_t*>(node) + 0x128);
        if (!IsValidUserPtr(children)) return;

        uint32_t nch = *reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(node) + 0x132);
        if (nch > 32u) nch = 32u;

        for (uint32_t i = 0; i < nch && count < CoSyncPlayerAnimation::MAX_SKELETON_BONES; ++i)
        {
            void* child = children[i];
            if (child && IsValidUserPtr(child))
                WalkAllNodes(child, depth + 1, nodes, names, count);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Public API ────────────────────────────────────────────────────────────────

// See the call site's own comment in CaptureBones. noinline for the usual
// reason (raw __try in a small function).
__declspec(noinline) static void LogRootParentDiag(void* root) noexcept
{
    __try
    {
        void* parent = *reinterpret_cast<void* const*>(
            reinterpret_cast<const uint8_t*>(root) + 0x28);
        const char* rootName = SafeReadBoneName(root);
        LOG_INFO("[PlayerAnim] ROOT PARENT DIAG: root=%p name='%s' parent(+0x28)=%p",
            root, (rootName && rootName[0]) ? rootName : "<unnamed>", parent);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[PlayerAnim] ROOT PARENT DIAG: crashed while reading the root bone's parent, caught safely");
    }
}

// Score the generated-pose snapshot against the node walk that just ran.
//
// Only meaningful in THIRD person, where both sources describe the same
// skeleton at the same instant: the node tree has every bone, and the pose
// buffer has every bone. If the index->name table and the quaternion
// convention are right, the two must agree to within float noise. If they
// don't, this says so before any of it reaches the wire -- which is the check
// that was missing from every earlier attempt in this file.
static void ValidatePoseSnapshot() noexcept
{
    using Snap = CoSyncFirstPersonGraph::PoseSnapshotBone;
    static Snap snap[192];
    const int n = CoSyncFirstPersonGraph::GetPoseSnapshot(snap, 192);
    if (n <= 0) return;

    // name hash -> captured node matrix, from this frame's own walk
    std::unordered_map<uint32_t, const float*> byHash;
    for (uint32_t i = 0; i < g_localCapture.count; ++i)
        byHash[g_localCapture.bones[i].nameHash] = g_localCapture.bones[i].rot;

    float worst[2] = { 0.0f, 0.0f };
    const char* worstBone[2] = { "-", "-" };
    int compared = 0;

    const int lim = (n < kAnimBoneCount) ? n : kAnimBoneCount;
    for (int i = 0; i < lim; ++i)
    {
        const char* nm = kAnimBoneNames[i];
        if (IsCameraNode(nm)) continue;
        auto it = byHash.find(HashBoneName(nm));
        if (it == byHash.end()) continue;   // bone has no node (expected in FP)
        ++compared;
        for (int t = 0; t < 2; ++t)
        {
            float m[9];
            QuatToMatrix(snap[i].rot, t != 0, m);
            float e = 0.0f;
            for (int k = 0; k < 9; ++k)
            {
                const float d = std::fabs(m[k] - it->second[k]);
                if (d > e) e = d;
            }
            if (e > worst[t]) { worst[t] = e; worstBone[t] = nm; }
        }
    }

    if (compared == 0) return;
    LOG_INFO("[PlayerAnim] [pose-check] snapshotBones=%d compared=%d  "
             "direct: maxErr=%.4f (%s)  transposed: maxErr=%.4f (%s)  -> %s",
        n, compared, worst[0], worstBone[0], worst[1], worstBone[1],
        (worst[0] <= worst[1]) ? "DIRECT wins" : "TRANSPOSED wins");
}

// Build the capture from the graph's generated pose instead of walking nodes.
//
// This is the whole point of the exercise. The node tree cannot supply a
// first-person lower body -- that 3D does not exist -- but the pose buffer
// carries every bone in the animation skeleton, legs included, because the
// graph computes them whether or not anything renders them.
//
// The quaternion convention is TRANSPOSED, settled by measurement rather than
// argument: in third person, where both the node tree and the pose buffer
// describe the same active graph in the same frame, the transposed conversion
// matched the live node matrices with maxErr=0.0000 across 25 bones while the
// direct one sat at ~1.99. That same result also validates kAnimBoneNames --
// a misaligned table would have failed both conventions, not one.
//
// bones[0] is left as a zeroed placeholder so real bones begin at index 1,
// matching the node walk (where index 0 is the root BSFadeNode and every
// consumer skips it).
static uint32_t BuildCaptureFromPoseSnapshot(bool fromThirdPersonGraph) noexcept
{
    using Snap = CoSyncFirstPersonGraph::PoseSnapshotBone;
    static Snap snap[MAX_SKELETON_BONES];
    const int n = fromThirdPersonGraph
        ? CoSyncFirstPersonGraph::GetThirdPersonPoseSnapshot(snap, MAX_SKELETON_BONES)
        : CoSyncFirstPersonGraph::GetPoseSnapshot(snap, MAX_SKELETON_BONES);
    if (n <= 0) return 0;

    uint32_t out = 0;
    g_localCapture.bones[out] = {};   // placeholder, skipped downstream
    ++out;

    const int lim = (n < kAnimBoneCount) ? n : kAnimBoneCount;
    for (int i = 0; i < lim && out < MAX_SKELETON_BONES; ++i)
    {
        const char* nm = kAnimBoneNames[i];
        if (IsCameraNode(nm)) continue;

        CoSyncPlayerAnimation::BoneTransform& bt = g_localCapture.bones[out];
        QuatToMatrix(snap[i].rot, /*transposed*/ true, bt.rot);
        if (!NormalizeRotationRows(bt.rot)) continue;

        strncpy_s(bt.name, sizeof(bt.name), nm, _TRUNCATE);
        bt.nameHash = HashBoneName(bt.name);
        bt.trans[0] = snap[i].trans[0];
        bt.trans[1] = snap[i].trans[1];
        bt.trans[2] = snap[i].trans[2];
        bt.scale    = (snap[i].scale > 0.0001f) ? snap[i].scale : 1.0f;
        ++out;
    }
    return out;
}

// Defined further down; forward declared here so CaptureBones can call it.
static void TickAlchEquipDiag(bool wantFp) noexcept;

void CaptureBones(Actor* player) noexcept
{
    if (!s_initialized.load(std::memory_order_acquire)) return;
    if (!player) return;

    // 2026-08-21 -- cache the local player's own root pointer instead of
    // calling SafeReadSkelRoot (actor->GetActorRootNode(false), a real
    // engine vtable dispatch) fresh every single frame. Live-traced via a
    // PDB-symbolized VEH stack + the linker's own .map file (ground truth,
    // no IDA base-address ambiguity this time): with the first-person drive
    // active, this call was faulting on a null nested vtable dispatch
    // (RAX=0, `call qword ptr [rax+10h]`) INSIDE the engine's own
    // GetActorRootNode implementation on nearly every call while the local
    // player is in first person -- 2000-2700 SEH-caught (non-fatal, the
    // __except already in SafeReadSkelRoot handles it -- game never
    // actually crashed) faults per session. The root NiNode pointer for a
    // given actor essentially never changes identity between frames (only
    // a full 3D reload changes it, the same assumption ApplySkeleton's own
    // pinned bone cache already relies on) -- so there was never a good
    // reason to re-resolve it via the engine every frame in the first
    // place.
    //
    // CORRECTED same session: the first version of this validated the
    // cached pointer with IsNodeAttached (parent-nonnull) before reuse --
    // measured live to have ZERO effect on the crash rate, because
    // IsNodeAttached checks a node's PARENT pointer, and the skeleton
    // ROOT node (what's cached here) legitimately has no parent at all --
    // it's the top of the tree. That check therefore failed unconditionally
    // for the one pointer this cache ever stores, so CaptureBones fell
    // through to the risky fresh GetActorRootNode call exactly as often as
    // before the "fix." Replaced with a plain time-based refresh: reuse the
    // cached pointer unconditionally for up to 1 second, then pay for one
    // fresh (risky) resolve. A full 3D reload (equip change, cell
    // transition) is the only thing that actually changes this pointer, so
    // worst case this walks a slightly-stale-but-still-mapped tree for up
    // to a second after such an event -- WalkCapture's own SEH guards every
    // individual read regardless, so a genuinely freed root just yields an
    // empty capture that frame, not a crash.
    //
    // 2026-08-24 -- the cache is now keyed on WHICH skeleton we want as well
    // as on time. A camera switch changes the answer instantly and a 1-second
    // stale window would mean up to a second of walking the wrong tree (or a
    // freed one) after every POV toggle, so s_cachedFp forces an immediate
    // re-resolve on change rather than waiting for the timer.
    //
    // Note this also explains, and should fix, the 2000-2700 SEH-caught
    // faults per session logged inside the engine's own GetActorRootNode
    // "while the local player is in first person": we were asking for the
    // third-person root of a player whose third-person 3D the engine had
    // parked. Asking for the one that is actually live should stop that at
    // the source instead of catching it.
    const bool wantFp = IsFirstPersonCaptureEnabled()
                     && CoSyncFirstPersonGraph::IsSenderInFirstPerson();

    // 2026-08-26: also force-invalidate on a Power Armor transition, not just
    // a camera switch. Entering/exiting PA is the same class of "full 3D
    // reload" event the 1-second timer above was already documented as
    // relying on to not walk a stale root -- it just wasn't checked. Keyed on
    // the LOCAL player's own formID via CoSyncPowerArmorSync's existing
    // sender-side PA state tracker (already proven safe/no-engine-call).
    const bool wantPa = CoSyncPowerArmorSync::IsActorKnownInPowerArmor(player->formID);

    static void* s_cachedRoot = nullptr;
    static uint64_t s_cacheExpiresMs = 0;
    static bool s_cachedFp = false;
    static bool s_cachedPa = false;
    void* root = nullptr;
    const uint64_t nowMs = GetTickCount64();
    if (s_cachedRoot && nowMs < s_cacheExpiresMs && s_cachedFp == wantFp && s_cachedPa == wantPa)
    {
        root = s_cachedRoot;
    }
    else
    {
        if (s_cachedFp != wantFp)
            LOG_INFO("[PlayerAnim] capture source -> %s person skeleton", wantFp ? "FIRST" : "THIRD");
        if (s_cachedPa != wantPa)
            LOG_INFO("[PlayerAnim] capture source -> Power Armor %s", wantPa ? "ENTER" : "EXIT");

        root = SafeReadSkelRoot(player, wantFp);
        s_cachedRoot = root;
        s_cachedFp = wantFp;
        s_cachedPa = wantPa;
        s_cacheExpiresMs = nowMs + 1000;

        // 2026-08-21 DIAGNOSTIC, one-shot -- settles live, directly, whether
        // the captured root genuinely has no NiNode parent (this file's own
        // assumption, used to justify dropping the parent-check from the
        // cache above) or whether it's actually nested under something else
        // at runtime. Delegated to its own noinline function -- CaptureBones
        // itself has other locals requiring unwind (the witness-log block
        // below uses std::string), and MSVC's C2712 forbids mixing that with
        // a raw __try in the same function.
        static std::atomic<bool> s_rootParentLogged{false};
        if (root && !s_rootParentLogged.exchange(true, std::memory_order_relaxed))
            LogRootParentDiag(root);
    }
    if (!root) return;

    g_localCapture.count = 0;
    g_localCapture.valid = false;
    g_localCapture.recvMs = 0; // locally produced, never stale

    // Mode 2: first person captures the GENERATED POSE, not the node tree.
    // Falls through to the node walk if no snapshot is available this frame,
    // so a missed update degrades to the old behaviour rather than to nothing.
    // Mode 3 reads the THIRD-person graph's pose instead of the active one.
    // That is the only source of a real walk for a first-person sender: the
    // shipped first-person clips hold the whole body at a constant pose
    // (RLeg_Thigh swings 0.00 deg over a full 1P run cycle, against 21.41 deg
    // in the third-person clip), so mode 2 faithfully reproduces a pose with
    // no locomotion in it. Requires bEngineTickParkedGraph=1, or the parked
    // third-person graph just sits at its reference pose.
    bool fromPose = false;
    const int fpMode = GetFirstPersonCaptureMode();
    if (wantFp && (fpMode == 2 || fpMode == 3))
    {
        const uint32_t n = BuildCaptureFromPoseSnapshot(fpMode == 3);
        if (n > 1)
        {
            g_localCapture.count = n;
            fromPose = true;
            // The pose buffer carries the WHOLE skeleton, so this capture is
            // not upper-body-only even though the camera is in first person.
            // Leaving the flag set would make ApplySkeleton drop exactly the
            // legs this path exists to deliver.
            g_localFirstPerson.store(false, std::memory_order_relaxed);
        }
    }

    if (!fromPose)
        WalkCapture(root, 0, g_localCapture.bones, g_localCapture.count);

    g_localCapture.valid = (g_localCapture.count > 0);

    // Live diagnostic window (see StartAlchEquipDiag) -- reads the REAL
    // g_localCapture this frame just filled (via WalkCapture or
    // BuildCaptureFromPoseSnapshot, whichever fpMode is active), not a fresh
    // GetActorRootNode(true) node-tree walk -- that tree is NOT what mode 3
    // (this codebase's default) captures from, and per IsFirstPersonCaptureEnabled's
    // own measured-and-documented finding it does not even contain a right arm
    // or hand at all ("left-arm + Pip-Boy view rig"), so sampling it would
    // silently test the wrong thing regardless of camera state.
    TickAlchEquipDiag(wantFp);

    // 2026-08-24 -- one-shot inventory per capture source. "captured N bones"
    // alone cannot distinguish "the whole body" from "79 nodes that happen to
    // be arms, fingers and weapon parts", and that distinction is the entire
    // question in first person. Logged once per source per session, in chunks
    // so nothing truncates it the way the BONES| transport line truncates.
    {
        static int s_dumpedFor = -1;
        const int srcKey = wantFp ? 1 : 0;
        if (s_dumpedFor != srcKey && g_localCapture.count > 0)
        {
            s_dumpedFor = srcKey;
            LOG_INFO("[PlayerAnim] [inventory] %s person source: %u bones",
                wantFp ? "FIRST" : "THIRD", g_localCapture.count);
            std::string line;
            for (uint32_t i = 0; i < g_localCapture.count; ++i)
            {
                line += g_localCapture.bones[i].name[0] ? g_localCapture.bones[i].name : "<unnamed>";
                line += ' ';
                if ((i % 10) == 9 || i + 1 == g_localCapture.count)
                {
                    LOG_INFO("[PlayerAnim] [inventory]   %s", line.c_str());
                    line.clear();
                }
            }
        }
    }

    static uint32_t s_capCount = 0;
    if (++s_capCount % 300 == 1)
        LOG_INFO("[PlayerAnim] CaptureBones #%u: captured %u bones from player",
                 s_capCount, g_localCapture.count);

    // Cross-check the generated-pose snapshot against the node walk.
    //
    // THIRD PERSON ONLY, and gated on the CAMERA rather than on wantFp. With
    // bCaptureFirstPersonSkeleton=0 wantFp is always false, so keying on it
    // would have let this run in first person too -- where the node walk reads
    // the PARKED third-person tree (frozen at bind pose) while the snapshot
    // comes off the ACTIVE first-person graph (animating). Those two describe
    // different things, and comparing them would produce large errors that say
    // nothing whatsoever about the quaternion convention.
    //
    // In third person both sources are the same active graph at the same
    // instant, which is the only configuration where disagreement is
    // attributable to the conversion. PoseSnapshotWasFirstPerson() is checked
    // too, so a snapshot left over from across a camera switch cannot sneak in.
    if (s_capCount % 300 == 1
        && !CoSyncFirstPersonGraph::IsSenderInFirstPerson()
        && !CoSyncFirstPersonGraph::PoseSnapshotWasFirstPerson())
        ValidatePoseSnapshot();

    // 2026-08-20 diagnostic: the transport log line for BONES| packets only
    // ever shows bone[0] (the world root), which is truncated by CoSync's
    // own logging before any leg/arm bone becomes visible -- no amount of
    // log querying can answer "are the legs actually moving" from that
    // source. Log two specific named witness bones directly from the
    // capture instead, one leg (RLeg_Thigh) and one upper-body (LArm_
    // UpperArm, matching FO4_Wrld's own witness bone choice in their [pose-
    // 1p] diagnostic), so consecutive log lines can be diffed by eye for
    // real frame-to-frame rotation change.
    //
    // TIGHTENED 2026-08-20 pt.2: confirmed real motion at 2s sampling, but
    // user reports the RESULT still doesn't look smooth (unlike 3rd person,
    // which uses this exact same 15Hz-send/no-interpolation pipeline and
    // looks fine). That points at jump SIZE, not correctness -- sampling
    // this coarse can't distinguish "smooth motion sampled sparsely" from
    // "genuinely large/uneven steps between updates". Matched to the
    // network send's own ~67ms (15Hz) cadence so what's logged here is
    // apples-to-apples with what actually goes out over the wire each tick.
    {
        static uint64_t s_nextWitnessMs = 0;
        const uint64_t nowMs = GetTickCount64();
        if (!IsMinimalMode() && nowMs >= s_nextWitnessMs)
        {
            s_nextWitnessMs = nowMs + 67;
            const CoSyncPlayerAnimation::BoneTransform* leg = nullptr;
            const CoSyncPlayerAnimation::BoneTransform* arm = nullptr;
            for (uint32_t i = 0; i < g_localCapture.count; ++i)
            {
                const auto& b = g_localCapture.bones[i];
                if (!leg && strcmp(b.name, "RLeg_Thigh") == 0) leg = &b;
                if (!arm && strcmp(b.name, "LArm_UpperArm") == 0) arm = &b;
            }
            auto fmt = [](const CoSyncPlayerAnimation::BoneTransform* b) -> std::string
            {
                if (!b) return "<not found>";
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                    "[%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f]",
                    b->rot[0], b->rot[1], b->rot[2], b->rot[3], b->rot[4],
                    b->rot[5], b->rot[6], b->rot[7], b->rot[8]);
                return buf;
            };
            // driveCount alongside the pose so a stutter (drive stalling a
            // few frames then jumping) is distinguishable from genuinely
            // large motion: steady ~4/interval at this 67ms cadence (60fps
            // drive / 15Hz sample) means the drive itself isn't the cause;
            // an irregular/bursty delta here would point at the drive loop
            // (lock contention, an intermittent early-out) instead of the
            // pose data itself.
            //
            // 2026-08-25 -- CAPTURE-CONTEXT enrichment (read-only, no writes).
            // The pose matrices alone can't tell "player jogs but the captured
            // graph plays a sprint/backpedal clip" from "player really is
            // sprinting". These extra fields make that call directly:
            //   cam     -- FP/TP, the sender's live camera (IsSenderInFirstPerson).
            //   path    -- which capture branch produced THIS pose: poseTP =
            //              third-person pose snapshot (mode 3, the parked graph
            //              re-tick), poseFP = active first-person graph pose
            //              (mode 2), walk = the NiNode tree walk fallback.
            //   capN    -- bones in the capture; legs -- is RLeg_Thigh present.
            //   realSpd -- game u/s from world-position delta, INDEPENDENT of any
            //              graph variable, so it is ground truth for the gait.
            //   gVars   -- the ACTIVE graph's own locomotion vars, read BY NAME so
            //              the correct per-graph index resolves (FP and TP graphs
            //              differ). NOTE these are the active graph's; in first
            //              person the CAPTURED legs come from the PARKED graph
            //              (path=poseTP), which these do not read -- realSpd vs
            //              the leg matrix is the cross-check there.
            const bool  camFp = CoSyncFirstPersonGraph::IsSenderInFirstPerson();
            const char* path  = fromPose ? (fpMode == 3 ? "poseTP" : "poseFP") : "walk";

            static NiPoint3 s_lastPos{};
            static uint64_t s_lastPosMs = 0;
            static bool     s_posValid  = false;
            float realSpd = 0.f;
            if (player)
            {
                const NiPoint3 p = player->pos;
                if (s_posValid && nowMs > s_lastPosMs)
                {
                    const float dt = static_cast<float>(nowMs - s_lastPosMs) / 1000.f;
                    if (dt > 0.f)
                    {
                        const float dx = p.x - s_lastPos.x;
                        const float dy = p.y - s_lastPos.y;
                        realSpd = std::sqrt(dx * dx + dy * dy) / dt;
                    }
                }
                s_lastPos = p; s_lastPosMs = nowMs; s_posValid = true;
            }

            float gSpeed = 0.f, gDir = 0.f;
            int   gLoco = -1, gSprint = -1;
            CoSyncAnimUtils::GetFloat(player, "Speed",                 gSpeed);
            CoSyncAnimUtils::GetFloat(player, "Direction",             gDir);
            CoSyncAnimUtils::GetInt  (player, "iLocomotionSpeedState", gLoco);
            CoSyncAnimUtils::GetInt  (player, "iSyncSprintState",      gSprint);

            LOG_INFO("[PlayerAnim] [witness] cam=%s path=%s capN=%u legs=%c realSpd=%.0f "
                "gVars[Speed=%.0f Dir=%.2f loco=%d sprint=%d] driveCount=%llu "
                "RLeg_Thigh=%s | LArm_UpperArm=%s",
                camFp ? "FP" : "TP", path, g_localCapture.count, leg ? 'Y' : 'N', realSpd,
                gSpeed, gDir, gLoco, gSprint,
                static_cast<unsigned long long>(CoSyncFirstPersonGraph::GetDriveCount()),
                fmt(leg).c_str(), fmt(arm).c_str());
        }
    }
}

void ApplySkeleton(Actor* proxy) noexcept
{
    ApplySkeleton(proxy, g_localCapture);
}

const CoSyncPlayerAnimation::SkeletonCapture& GetLocalCapture() noexcept
{
    return g_localCapture;
}

// Kill-switch for the nlerp/extrapolation smoothing pass below (FO4_Wrld
// c.42, kNpcPoseSmooth). Flip to false to fall back to instant-snap-per-
// arrival (the original CoSync behavior) if this ever regresses anything --
// matches the project's established constexpr-killswitch convention
// (kAttemptFirstPersonDrive, kGhostCrouchEnabled, etc.).
static constexpr bool  kBonePoseSmoothingEnabled = true;
// Split per body region (FO4_Wrld kNpcPoseSmoothUpper / kNpcPoseSmoothLower):
// the aim and arm chain is near-snap so weapon pointing tracks the sender,
// while the legs glide more slowly to hide the 15Hz sample grain.
static constexpr float kBonePoseSmoothUpper      = 0.70f; // FO4_Wrld kNpcPoseSmoothUpper
static constexpr float kBonePoseSmoothLower      = 0.45f; // FO4_Wrld kNpcPoseSmoothLower
static constexpr float kBonePoseMaxExtrap        = 1.5f;  // FO4_Wrld kNpcPoseMaxExtrap

// Isolated in its own function (no C++ objects with destructors in scope) --
// MSVC's C2712 forbids a raw __try/__except directly inside a function that
// also has locals requiring unwinding (ApplySkeleton now does, via the
// std::unordered_map/std::string bone cache). Same reasoning as every other
// SEH helper in this file; noinline per the established GetFloat/
// SafeReadSkelRoot lesson (see that function's own comment above).
// The update_data buffer is a NiUpdateData. Slot 49 reads AND WRITES the dword
// at update_data+0x10 (it folds the node's own +0x108 bit 3 into bit 0 of that
// field, then tests it to decide whether to run NiTimeControllers), so the
// buffer must be writable -- a stack array is. 512 bytes is far more than the
// struct needs and keeps any internal read in bounds. Zero-initialised means
// time 0.0, matching what FO4_Wrld passes.
__declspec(noinline) static void SafeUpdateDownward(void* root, uint32_t proxyFormID) noexcept
{
    uint8_t update_data[512] = {};
    __try
    {
        void** vt = *reinterpret_cast<void***>(root);
        auto fn = reinterpret_cast<UpdateDownwardPassFn>(vt[NIAV_VTBL_UPDATE_DOWNWARD_PASS]);
        fn(root, update_data, 0u);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[PlayerAnim] UpdateDownwardPass crashed and was caught on proxy 0x%08X", proxyFormID);
    }
}

bool ReassertPoseForGraphRoot(void* graphRootNode) noexcept
{
    if (!s_initialized.load(std::memory_order_acquire)) return false;
    if (!graphRootNode) return false;

    std::lock_guard<std::mutex> lk(g_reassertMutex);
    auto it = g_reassertByGraphRoot.find(graphRootNode);
    if (it == g_reassertByGraphRoot.end()) return false;

    ReassertRecord& rec = it->second;
    if (rec.writes.empty()) return false;

    // Stale record => ApplySkeleton has stopped publishing for this proxy
    // (RELEASE tier, despawn, mode off). Stop re-stamping rather than pinning
    // a pose nobody is maintaining any more.
    const uint64_t now = GetTickCount64();
    if (now - rec.stampMs > kReassertMaxAgeMs)
        return false;

    // Raw re-stamp: no quaternion math, no smoothing advance, no tier logic --
    // just put back the exact matrices the main thread already decided on,
    // now that the engine's graph has had its turn.
    for (const PoseWrite& w : rec.writes)
    {
        if (!w.node) continue;
        if (!IsNodeAttached(w.node)) continue;
        SafeWriteRotation(w.node, w.rot);
        if (w.hasTrans) SafeWriteTranslation(w.node, w.trans);
    }

    if (rec.skelRoot)
        SafeUpdateDownward(rec.skelRoot, rec.formID);

    g_reassertCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

uint64_t GetReassertCount() noexcept
{
    return g_reassertCount.load(std::memory_order_relaxed);
}

bool SampleBoneRotation(Actor* actor, const char* boneName, float outRot[9], bool firstPerson) noexcept
{
    if (!s_initialized.load(std::memory_order_acquire)) return false;
    if (!actor || !boneName || !boneName[0]) return false;

    void* root = SafeReadSkelRoot(actor, firstPerson);
    if (!root) return false;

    // Same walk ApplySkeleton uses for its cache rebuild -- every node it can
    // reach came from a live children array, and every read inside it is
    // SEH-caged. Static buffers because this is main-thread-only diagnostics,
    // matching the existing WalkAllNodes call sites in this file.
    static void* s_nodes[CoSyncPlayerAnimation::MAX_SKELETON_BONES];
    static const char* s_names[CoSyncPlayerAnimation::MAX_SKELETON_BONES];
    uint32_t count = 0;
    WalkAllNodes(root, 0, s_nodes, s_names, count);

    const uint32_t wanted = HashBoneName(boneName);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!s_names[i] || !s_names[i][0]) continue;
        if (HashBoneName(s_names[i]) != wanted) continue;
        return SafeReadRotation(s_nodes[i], outRot);
    }
    return false;
}

// See the header comment on StartAlchEquipDiag for what this is and why.
static std::atomic<int> s_alchDiagFramesLeft{0};

void StartAlchEquipDiag() noexcept
{
    s_alchDiagFramesLeft.store(90, std::memory_order_relaxed);
    LOG_INFO("[AlchDiag] armed -- sampling RArm_Hand/LArm_Hand on both skeletons for 90 frames");
}

// Called once per frame from CaptureBones, AFTER g_localCapture is filled for
// this frame, while a diagnostic window is armed. Read-only -- just scans the
// buffer CaptureBones already built (via WalkCapture or
// BuildCaptureFromPoseSnapshot -- whichever is actually live) and logs a
// couple of arm bones' rotation, tagged with the capture source. This is the
// REAL data that gets serialized and sent to peers, so it directly answers
// "did the stimpak pose make it into what we transmit" regardless of which
// underlying mechanism produced it.
static void TickAlchEquipDiag(bool wantFp) noexcept
{
    const int framesLeft = s_alchDiagFramesLeft.load(std::memory_order_relaxed);
    if (framesLeft <= 0) return;
    s_alchDiagFramesLeft.store(framesLeft - 1, std::memory_order_relaxed);

    if ((framesLeft % 10) != 0 && framesLeft != 90) return;

    const uint32_t wantR = HashBoneName("RArm_Hand");
    const uint32_t wantL = HashBoneName("LArm_UpperArm");
    const CoSyncPlayerAnimation::BoneTransform* rArm = nullptr;
    const CoSyncPlayerAnimation::BoneTransform* lArm = nullptr;
    for (uint32_t i = 0; i < g_localCapture.count; ++i)
    {
        if (!rArm && g_localCapture.bones[i].nameHash == wantR) rArm = &g_localCapture.bones[i];
        if (!lArm && g_localCapture.bones[i].nameHash == wantL) lArm = &g_localCapture.bones[i];
        if (rArm && lArm) break;
    }

    LOG_INFO("[AlchDiag] framesLeft=%d capturingFP=%d capturedCount=%u | RArm_Hand %s [%.3f %.3f %.3f] | LArm_UpperArm %s [%.3f %.3f %.3f]",
        framesLeft, wantFp ? 1 : 0, g_localCapture.count,
        rArm ? "present" : "ABSENT", rArm ? rArm->rot[0] : 0.f, rArm ? rArm->rot[1] : 0.f, rArm ? rArm->rot[2] : 0.f,
        lArm ? "present" : "ABSENT", lArm ? lArm->rot[0] : 0.f, lArm ? lArm->rot[1] : 0.f, lArm ? lArm->rot[2] : 0.f);
}

// FRESH / HOLD / RELEASE tiering (FO4_Wrld kNpcPoseFreshMs / kNpcPoseHoldMs).
//
// 2026-08-21: CoSync had only the FRESH ceiling, and only inside the
// extrapolation math -- nothing ever stopped applying a pose. Since
// ApplySkeleton returns true for any still-valid capture, and s_recvSkeletons
// entries were never aged out, one BONES packet was enough to take a proxy
// off the graph-variable path PERMANENTLY: a peer who went first person
// (ShouldCaptureNow holds, so no new captures leave their machine) or simply
// stopped sending left their proxy pinned to its last received frame with no
// route back to normal animation.
//
//   FRESH   (<= 200ms)  drive every matched bone, with extrapolation.
//   HOLD    (<= 500ms)  drive the upper body only; the legs are released back
//                       to the proxy's own graph, which is still running, so
//                       it keeps walking instead of skating on a frozen pose.
//   RELEASE (>  500ms)  return false. TickAnimVars' existing fallback picks it
//                       up and the proxy animates normally again.
static constexpr uint64_t kPoseFreshMs = 200;
static constexpr uint64_t kPoseHoldMs  = 500;

bool ApplySkeleton(Actor* proxy, const CoSyncPlayerAnimation::SkeletonCapture& capture) noexcept
{
    if (!s_initialized.load(std::memory_order_acquire)) return false;
    if (!proxy || !capture.valid) return false;

    const uint64_t nowMs = GetTickCount64();

    // recvMs == 0 means a locally-produced capture (the IsEnabled() local
    // mirror path, which has no network component and so cannot go stale).
    const uint64_t poseAge = (capture.recvMs == 0) ? 0 : (nowMs - capture.recvMs);
    if (poseAge > kPoseHoldMs)
        return false;                                 // RELEASE
    // HOLD tier only. The first-person flag still travels on the wire (see
    // upperBodyOnly) but no longer changes the apply: the hybrid that used it
    // was reverted, so a first-person sender's pose is applied whole, exactly
    // as before.
    const bool holding = (poseAge > kPoseFreshMs);

    void* root = SafeReadSkelRoot(proxy);
    if (!root) return false;

    // 2026-08-20 -- pinned per-proxy cache (FO4_Wrld NpcBoneCache port, see
    // BoneCacheEntry above) replaces the old "fresh WalkAllNodes every call"
    // approach. Rebuilt ONLY when the proxy's root pointer changes (equip or
    // gear-driven 3D reload, cell transition, etc.) instead of once per tick
    // per proxy. Matching is by bone-name HASH (2026-08-21, was the name
    // string); the per-bone body-region and translation flags are resolved
    // here too, once per rebuild, off the PROXY's own node names -- a received
    // capture carries only hashes, so this is the last place names exist to
    // classify from.
    BoneCacheEntry& entry = g_boneCaches[proxy];
    if (entry.root != root)
    {
        ReleaseBoneCacheEntry(entry);

        static void* s_proxyNodes[CoSyncPlayerAnimation::MAX_SKELETON_BONES];
        static const char* s_proxyNames[CoSyncPlayerAnimation::MAX_SKELETON_BONES];
        uint32_t nodeCount = 0;
        WalkAllNodes(root, 0, s_proxyNodes, s_proxyNames, nodeCount);

        for (uint32_t j = 1; j < nodeCount; ++j)
        {
            const char* nm = s_proxyNames[j];
            if (!nm || !nm[0]) continue;
            void* node = s_proxyNodes[j];
            if (!node) continue;
            const uint32_t h = HashBoneName(nm);
            if (h == 0u) continue;
            if (entry.nodesByHash.find(h) != entry.nodesByHash.end()) continue; // first wins
            NiIncRef(node); // pin -- the engine physically cannot free this out from under us now

            BoneCacheEntry::CachedBone cb;
            cb.node      = node;
            cb.translate = (std::strcmp(nm, "COM") == 0 || std::strcmp(nm, "Pelvis") == 0);
            cb.upperBody = IsUpperBodyBoneName(nm);
            entry.nodesByHash[h] = cb;
        }
        entry.root = root;

        // The graph's own write-target node 0 is the "Root" bone (confirmed
        // live by the FP-drive diagnostic's writeTargetNode0 name='Root'
        // sameTree(depth)=1). That node is how ReassertPoseForGraphRoot maps
        // an engine graph update back to this proxy without an Actor*.
        {
            auto rootIt = entry.nodesByHash.find(HashBoneName("Root"));
            entry.graphRootNode = (rootIt != entry.nodesByHash.end()) ? rootIt->second.node : nullptr;
        }

        LOG_INFO("[PlayerAnim] ApplySkeleton: proxy 0x%08X cache REBUILT nodeCount=%u pinned=%zu source boneCount=%u graphRoot=%p",
                 proxy->formID, nodeCount, entry.nodesByHash.size(), capture.count, entry.graphRootNode);
        if (!entry.graphRootNode)
            LOG_WARN("[PlayerAnim] proxy 0x%08X has no 'Root' bone -- write-last re-stamp cannot be keyed for it",
                     proxy->formID);
    }

    // Scratch for this frame's published write set. Main-thread only (see
    // ApplySkeleton's own contract), swapped into the shared record under lock
    // at the end so the lock is never held across the apply loop.
    static std::vector<PoseWrite> s_publish;
    s_publish.clear();

    static uint32_t s_applyCount = 0;
    ++s_applyCount;

    // Apply source[i] to whichever CACHED proxy node has the same name hash.
    // Skip index 0 (root BSFadeNode/skeleton.nif): its translation is the
    // world-space actor position, which WorldSync owns -- writing it would
    // teleport the proxy. A source bone with no identity, or none matching a
    // cached node, is skipped rather than guessed at.
    uint32_t applied = 0;
    uint32_t missed  = 0;
    for (uint32_t i = 1; i < capture.count; ++i)
    {
        const CoSyncPlayerAnimation::BoneTransform& bt = capture.bones[i];
        if (bt.nameHash == 0u) continue;

        auto nodeIt = entry.nodesByHash.find(bt.nameHash);
        if (nodeIt == entry.nodesByHash.end()) { ++missed; continue; }
        const BoneCacheEntry::CachedBone& cb = nodeIt->second;

        // HOLD tier: leave the lower body to the proxy's own graph.
        if (holding && !cb.upperBody) continue;

        // 2026-08-24 -- first-person sender: apply the UPPER BODY ONLY.
        //
        // Un-reverts the hybrid this file's own comment above says was
        // dropped. It was dropped while first person shipped a frozen pose
        // off the parked third-person graph, so it could not have helped --
        // gating a stale pose down to half a stale pose is still stale. Now
        // that first person captures the live first-person tree, the upper
        // half is real and the gate does what it was written for.
        //
        // Two independent reasons the lower half must not be applied from a
        // first-person capture:
        //   * It is not there. Live inventory: 79 bones captured in first
        //     person with RLeg_Thigh among the names NOT present, versus 57
        //     in third person where it is present and moving. The first-
        //     person 3D the engine instantiates has no lower-body NiNodes at
        //     all -- the graph poses those bones internally (the shipped
        //     first-person clips do drive them), but with no node to write
        //     them into, nothing downstream can read them back.
        //   * COM and Pelvis in a first-person tree are anchored to the
        //     CAMERA, not to the body, and they are the only two bones this
        //     pipeline sends translation for. Copying them onto a proxy
        //     displaces the whole actor. IsUpperBodyBoneName already
        //     classifies COM and Pelvis as lower body, so this one test
        //     covers both hazards; "Root" is named explicitly because it
        //     matches none of that function's fragments but carries whole-
        //     body orientation just the same.
        if (capture.upperBodyOnly
            && (!cb.upperBody || std::strcmp(bt.name, "Root") == 0))
            continue;

        void* target = cb.node;
        // Cheap defense-in-depth alongside the pin (see BoneCacheEntry's own
        // comment) -- if this ever fails for a pinned pointer, skip the write
        // for this bone this frame; the next root-change rebuild recovers it
        // rather than risking a corrupt write.
        if (!IsNodeAttached(target)) continue;

        float targetQuat[4];
        Mat3ToQuat(bt.rot, targetQuat);

        BoneCacheEntry::SmoothState& sm = entry.smooth[bt.nameHash];
        if (!sm.primed)
        {
            std::memcpy(sm.appliedQuat, targetQuat, sizeof(targetQuat));
            std::memcpy(sm.curQuat, targetQuat, sizeof(targetQuat));
            sm.curSetMs = nowMs;
            sm.primed = true;
        }
        else if (targetQuat[0] != sm.curQuat[0] || targetQuat[1] != sm.curQuat[1] ||
                 targetQuat[2] != sm.curQuat[2] || targetQuat[3] != sm.curQuat[3])
        {
            std::memcpy(sm.prevQuat, sm.curQuat, sizeof(sm.curQuat));
            sm.prevSetMs = sm.curSetMs;
            std::memcpy(sm.curQuat, targetQuat, sizeof(targetQuat));
            sm.curSetMs = nowMs;
            sm.hasPrev = true;
        }

        float rotOut[9];
        if (kBonePoseSmoothingEnabled)
        {
            // Extrapolate `cur` forward by the prev->cur sample's own delta,
            // scaled by how much of that same interval has elapsed since -- so
            // the nlerp always chases a MOVING target instead of converging
            // and then freezing between sparse network arrivals (FO4_Wrld
            // c.42; this is what actually fixed their staleness, not the nlerp
            // blend alone).
            //
            // Bounded by a freshness ceiling as well as the per-sample factor
            // clamp: without it, while the SENDER holds a pose `elapsed` grows
            // unbounded, `tExtrap` saturates at its max and STAYS there, and
            // the proxy converges onto a fixed but WRONGLY-extrapolated pose
            // (whatever the last real prev->cur swing predicted 1.5x further
            // along) instead of the real last-good pose. Past that window,
            // stop extrapolating and chase `cur` as-is -- a clean, correct
            // hold. The HOLD tier never extrapolates at all.
            float chaseTarget[4] = { sm.curQuat[0], sm.curQuat[1], sm.curQuat[2], sm.curQuat[3] };
            if (sm.hasPrev && !holding)
            {
                const uint64_t elapsed = nowMs - sm.curSetMs;
                if (elapsed <= kPoseFreshMs)
                {
                    const uint64_t period = (sm.curSetMs > sm.prevSetMs) ? (sm.curSetMs - sm.prevSetMs) : 33;
                    float tExtrap = period > 0 ? static_cast<float>(elapsed) / static_cast<float>(period) : 0.f;
                    if (tExtrap > kBonePoseMaxExtrap) tExtrap = kBonePoseMaxExtrap;
                    if (tExtrap > 0.f)
                        NlerpQuat(sm.prevQuat, sm.curQuat, 1.0f + tExtrap, chaseTarget);
                }
            }
            // Split smoothing (FO4_Wrld kNpcPoseSmoothUpper/Lower): the aim and
            // arm chain wants to be near-snap so weapon pointing tracks, while
            // the legs want a slower glide to hide the 15Hz sample grain.
            const float smooth = cb.upperBody ? kBonePoseSmoothUpper : kBonePoseSmoothLower;
            NlerpQuat(sm.appliedQuat, chaseTarget, smooth, sm.appliedQuat);
            QuatToMat3(sm.appliedQuat, rotOut);
        }
        else
        {
            std::memcpy(rotOut, bt.rot, sizeof(rotOut));
        }

        // 2026-08-21 WRITE-ORDER PROBE -- the one question the static trace
        // could not answer. ApplySkeleton runs from F4SE's main-thread task
        // pump (AddTaskPermanent -> MessageQueueProcessTask, reached from
        // Main::Update at 0x00C3488B), while the proxy's own behaviour graph
        // is ALSO writing these same +0x30 matrices every frame -- so we are
        // racing it, not overriding it, and whichever writes last before the
        // scene bake is what renders. Read the matrix back immediately before
        // writing and compare against what we left there last frame: if it
        // still matches, our write survived and the ordering is already right;
        // if it has been replaced, the engine's graph is winning and this
        // needs a post-original detour on the graph's own pose-apply
        // (0x01321030 -- two callers, both inside
        // BSAnimationGraphManager::Update) instead. Reported below as one
        // throttled line per proxy so the answer is unambiguous in the log.
        if (sm.hasLastWrote)
        {
            float current[9];
            if (SafeReadRotation(target, current))
            {
                bool survived = true;
                for (int k = 0; k < 9; ++k)
                    if (current[k] != sm.lastWrote[k]) { survived = false; break; }
                sm.lastSurvived = survived;
            }
        }

        SafeWriteRotation(target, rotOut);
        std::memcpy(sm.lastWrote, rotOut, sizeof(rotOut));
        sm.hasLastWrote = true;

        // Translation restricted to COM/Pelvis only -- FO4_Wrld's own "crouch
        // channel" design. Writing translation onto an arbitrary matched bone
        // is what stretched their ghost when a skeleton mismatch slipped
        // through; every other bone here is rotation-only.
        if (cb.translate)
            SafeWriteTranslation(target, bt.trans);

        // Publish exactly what we just wrote, for the write-last re-stamp.
        {
            PoseWrite pw;
            pw.node = target;
            std::memcpy(pw.rot, rotOut, sizeof(rotOut));
            pw.hasTrans = cb.translate;
            if (cb.translate) std::memcpy(pw.trans, bt.trans, sizeof(pw.trans));
            s_publish.push_back(pw);
        }

        ++applied;
    }

    entry.lastTouchedMs = nowMs;
    SweepStaleBoneCaches();

    if (applied > 0)
        SafeUpdateDownward(root, proxy->formID);

    // 2026-08-24 -- RECEIVER-SIDE PROXY WITNESS. The sender-side witness proves
    // the capture streams a real walk (41 distinct RLeg_Thigh poses), the tier
    // is FRESH and OURS SURVIVES 55/55 -- yet the proxy's lower body looks
    // wrong. No log has ever shown what the PROXY actually holds. This reads
    // two named bones straight off (a) the received wire capture and (b) the
    // proxy's own live node AFTER the downward-pass recompute, so consecutive
    // lines diff by eye: if WIRE moves but PROXY is frozen the loss is in the
    // apply/graph-fight; if WIRE is frozen the loss is upstream (send timing);
    // if both move but look wrong it is a coordinate/bind-space issue.
    {
        static uint64_t s_nextRxWitnessMs = 0;
        if (!IsMinimalMode() && nowMs >= s_nextRxWitnessMs)
        {
            s_nextRxWitnessMs = nowMs + 500;
            // Walk the WHOLE leg + arm chain top-to-bottom. For each bone report
            // one of: OK (proxy matches wire within tol), LAG (proxy differs from
            // wire -- smoothing or graph-fight), MISS (bone not on proxy skeleton,
            // so it never replicates -> everything below it breaks), or NOWIRE
            // (bone not in the received capture at all -> sender never sent it).
            // A single MISS/NOWIRE high in the chain explains a broken lower half
            // even while RLeg_Thigh itself reads OK.
            static const char* const kChain[] = {
                "COM", "Pelvis",
                "LLeg_Thigh", "LLeg_Calf", "LLeg_Foot",
                "RLeg_Thigh", "RLeg_Calf", "RLeg_Foot",
                "SPINE1", "Spine1", "Chest", "Neck", "Head",
                "LArm_UpperArm", "LArm_ForeArm1", "LArm_Hand",
                "RArm_UpperArm", "RArm_ForeArm1", "RArm_Hand",
            };
            auto findWire = [&](uint32_t h) -> const CoSyncPlayerAnimation::BoneTransform* {
                for (uint32_t i = 0; i < capture.count; ++i)
                    if (capture.bones[i].nameHash == h) return &capture.bones[i];
                return nullptr;
            };
            char line[900]; size_t off = 0;
            for (const char* nm : kChain)
            {
                const uint32_t h = HashBoneName(nm);
                const CoSyncPlayerAnimation::BoneTransform* w = findWire(h);
                auto it = entry.nodesByHash.find(h);
                float pr[9] = {}; bool gotP = (it != entry.nodesByHash.end()) && SafeReadRotation(it->second.node, pr);
                const char* tag;
                if (!w && it == entry.nodesByHash.end()) tag = "ABSENT";   // neither side has it (name variant)
                else if (!w)                            tag = "NOWIRE";   // proxy has it, wire doesn't
                else if (it == entry.nodesByHash.end()) tag = "MISS";     // wire has it, proxy doesn't
                else if (!gotP)                         tag = "NOREAD";
                else {
                    float d = 0.f; for (int k = 0; k < 9; ++k) { float e = w->rot[k]-pr[k]; d += e*e; }
                    tag = (d < 0.02f) ? "OK" : "LAG";
                }
                if (std::strcmp(tag, "ABSENT") == 0) continue; // skip name variants not on this skeleton
                int wr = std::snprintf(line+off, sizeof(line)-off, " %s=%s", nm, tag);
                if (wr <= 0 || off + wr >= sizeof(line) - 1) break;
                off += static_cast<size_t>(wr);
            }
            // WORLD-TRANSFORM probe: the user reports T-pose legs while the
            // LOCAL rotations above read OK -- so the question is whether
            // UpdateDownwardPass actually propagated the local rotations into
            // the WORLD transform that renders and skins the mesh. NiAVObject
            // world NiTransform is at +0x70 (verified vs CommonLibF4:
            // sizeof==0x120, local@0x30, world@0x70), world translate at +0xA0.
            // Log the WORLD translation of a mid and low leg bone: if these
            // stay constant while the sender walks, the world transform is
            // frozen at bind == T-pose render, and the local writes never reach
            // the mesh (UpdateDownwardPass not propagating for this proxy).
            auto worldTrans = [&](const char* nm, float out[3]) -> bool {
                auto it = entry.nodesByHash.find(HashBoneName(nm));
                if (it == entry.nodesByHash.end() || !it->second.node) return false;
                bool ok = false;
                __try {
                    const float* wt = reinterpret_cast<const float*>(
                        reinterpret_cast<uint8_t*>(it->second.node) + 0xA0);
                    out[0]=wt[0]; out[1]=wt[1]; out[2]=wt[2]; ok = true;
                } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
                return ok;
            };
            float wThigh[3]={}, wFoot[3]={}, wHand[3]={};
            bool gT = worldTrans("RLeg_Thigh", wThigh);
            bool gF = worldTrans("RLeg_Foot", wFoot) || worldTrans("RLeg_Calf", wFoot);
            bool gH = worldTrans("LArm_Hand", wHand) || worldTrans("LArm_ForeArm1", wHand);
            std::snprintf(line+off, sizeof(line)-off,
                " || WORLDpos Thigh=%s[%.1f %.1f %.1f] Foot=%s[%.1f %.1f %.1f] Hand=%s[%.1f %.1f %.1f]",
                gT?"":"?", wThigh[0],wThigh[1],wThigh[2],
                gF?"":"?", wFoot[0],wFoot[1],wFoot[2],
                gH?"":"?", wHand[0],wHand[1],wHand[2]);
            LOG_INFO("[PlayerAnim] [rx-chain] proxy 0x%08X tier=%s captureN=%u cached=%zu |%s",
                proxy->formID, holding ? "HOLD" : "FRESH",
                capture.count, entry.nodesByHash.size(), line);
        }
    }

    if (entry.graphRootNode)
    {
        std::lock_guard<std::mutex> lk(g_reassertMutex);
        ReassertRecord& rec = g_reassertByGraphRoot[entry.graphRootNode];
        rec.skelRoot = root;
        rec.formID   = proxy->formID;
        rec.stampMs  = nowMs;
        rec.writes.swap(s_publish);   // O(1); s_publish keeps the old buffer to refill next frame
    }

    {
        static uint64_t s_nextProbeMs = 0;
        if (!IsMinimalMode() && nowMs >= s_nextProbeMs)
        {
            s_nextProbeMs = nowMs + 2000;
            uint32_t survived = 0, checked = 0;
            for (const auto& kv : entry.smooth)
            {
                if (!kv.second.hasLastWrote) continue;
                ++checked;
                if (kv.second.lastSurvived) ++survived;
            }
            // checked==0 on the very first apply (nothing written yet to
            // compare against) -- say so rather than reporting a verdict the
            // sample cannot support.
            const char* verdict = (checked == 0)                 ? "NO SAMPLE YET"
                                : (survived * 2u >= checked)     ? "OURS SURVIVES"
                                                                 : "ENGINE OVERWRITES US";
            LOG_INFO("[PlayerAnim] [write-order] proxy 0x%08X: %u/%u bones still held our last write (%s)"
                     " | applied=%u missed=%u tier=%s poseAge=%llums restamps=%llu",
                     proxy->formID, survived, checked, verdict,
                     applied, missed, holding ? "HOLD" : "FRESH",
                     static_cast<unsigned long long>(poseAge),
                     static_cast<unsigned long long>(g_reassertCount.load(std::memory_order_relaxed)));
        }
    }

    if (s_applyCount % 300 == 1)
        LOG_INFO("[PlayerAnim] ApplySkeleton #%u: applied %u/%u to proxy 0x%08X (cached=%zu missed=%u)",
                 s_applyCount, applied, capture.count, proxy->formID, entry.nodesByHash.size(), missed);

    return applied > 0;
}

void ReleaseBoneCache(Actor* proxy) noexcept
{
    if (!proxy) return;
    auto it = g_boneCaches.find(proxy);
    if (it == g_boneCaches.end()) return;
    ReleaseBoneCacheEntry(it->second);
    g_boneCaches.erase(it);
}

// ============================================================================
// Network bone replication
// ============================================================================

// Raw bDeltaPositionLocomotion value, cached at file scope so IsMinimalMode
// can read it too. -1 = not yet read. 2 = full experimental bone replication,
// 3 = the same pipeline as a clean bundled preset (see IsMinimalMode).
static int g_boneRepModeCache = -1;

bool IsBoneReplicationEnabled()
{
    LOG_REQUIREMENT("(global)::IsBoneReplicationEnabled", "Reads an optional settings file. If it's missing, it just falls back to off, which keeps the old behavior.");
    if (g_boneRepModeCache >= 0)
        return g_boneRepModeCache == 2 || g_boneRepModeCache == 3;

    char exePath[MAX_PATH] = {};
    if (::GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0)
    {
        g_boneRepModeCache = 0;
        return false;
    }

    std::string ini(exePath);
    const size_t slash = ini.find_last_of("\\/");
    ini = (slash == std::string::npos) ? std::string() : ini.substr(0, slash + 1);
    ini += "Data\\F4SE\\Plugins\\CoSync.ini";

    // 2026-08-26 -- bone replication is now the MAIN player animation system,
    // so the default (key absent) is the minimal preset (3), not off. The ini
    // key is retained only as an override (e.g. =2 for full diagnostics, =0 to
    // force the legacy graph-var path off entirely for players).
    g_boneRepModeCache = ::GetPrivateProfileIntA("Animations", "bDeltaPositionLocomotion", 3, ini.c_str());
    LOG_INFO("[PlayerAnim] bDeltaPositionLocomotion=%d -> boneReplication=%s%s (from '%s')",
        g_boneRepModeCache,
        (g_boneRepModeCache == 2 || g_boneRepModeCache == 3) ? "ON" : "off",
        (g_boneRepModeCache == 3) ? " (minimal preset)" : "",
        ini.c_str());
    return g_boneRepModeCache == 2 || g_boneRepModeCache == 3;
}

// bDeltaPositionLocomotion=3: the clean bundled first-person preset. Same
// pipeline as =2 -- this only forces the capture source + engine re-tick on
// (GetFirstPersonCaptureMode / IsFirstPersonCaptureEnabled / IsEngineTickEnabled
// key off it) and silences the high-volume experimental diagnostics.
bool IsMinimalMode()
{
    IsBoneReplicationEnabled();   // ensure the value is read + cached once
    return g_boneRepModeCache == 3;
}

bool IsFirstPersonCaptureEnabled()
{
    LOG_REQUIREMENT("(global)::IsFirstPersonCaptureEnabled", "Reads the same settings file as bone replication. If it's missing, it defaults to on -- this only matters in first person, which doesn't currently work anyway, so 'on' is the safe default.");
    // Minimal preset bundles first-person capture on -- no companion key needed.
    if (IsMinimalMode()) return true;
    static int s_cached = -1;
    if (s_cached >= 0)
        return s_cached != 0;

    char exePath[MAX_PATH] = {};
    if (::GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0)
    {
        s_cached = 1;
        return true;
    }

    std::string ini(exePath);
    const size_t slash = ini.find_last_of("\\/");
    ini = (slash == std::string::npos) ? std::string() : ini.substr(0, slash + 1);
    ini += "Data\\F4SE\\Plugins\\CoSync.ini";

    // 2026-08-24 -- default flipped 1 -> 0. DISPROVEN BY LIVE INVENTORY.
    //
    // The file-side reasoning was right and is unchanged: the first-person
    // ANIMATION skeleton really is 94 bones index-identical to third person's
    // 95, and the shipped first-person clips really do drive 74-75 of 94
    // tracks including both legs. What that reasoning skipped is that the
    // Havok animation skeleton is NOT the instantiated NiNode tree.
    //
    // GetActorRootNode(true) on the local player returns, measured:
    //   79 nodes = 11 real skeleton bones + 68 Pip-Boy MODEL nodes
    //   Root COM SPINE1 SPINE2 Chest LArm_Collarbone LArm_UpperArm
    //   LArm_ForeArm1 LArm_ForeArm2 LArm_ForeArm3 PipboyBone
    //   then PipboyRoot, PipboyHinge, PipboyClasp01-03, PipboyBody,
    //   AdjustKnob, ModeKnob, TuneKnob, ScrollKnob, SelectButton,
    //   LightButton, RadioNeedle, TapeDeck01/02, Screen, ... and stops.
    // No LArm_Hand. No right arm at all. No Neck, no Head, no Pelvis, no
    // legs. It is the left-arm + Pip-Boy view rig, and it is strictly LESS
    // than the parked third-person tree, which at least has all 57 named
    // body nodes even though the graph is not ticking them.
    //
    // This is also, finally, what the old "~20-node unnamed substructure"
    // comment on SafeReadSkelRoot was looking at. It reads as 79 here only
    // because kMaxWalkDepth was later raised 12 -> 20, which is exactly deep
    // enough to descend into the Pip-Boy mesh hierarchy.
    //
    // Left reachable at 1 because the finding is worth being able to
    // reproduce in one line, not because it is a useful mode.
    s_cached = ::GetPrivateProfileIntA("Animations", "bCaptureFirstPersonSkeleton", 0, ini.c_str());
    LOG_INFO("[PlayerAnim] bCaptureFirstPersonSkeleton=%d -> firstPersonCapture=%s (from '%s')",
        s_cached, (s_cached == 2) ? "GENERATED POSE" : ((s_cached != 0) ? "node tree" : "off"),
        ini.c_str());
    return s_cached != 0;
}

int GetFirstPersonCaptureMode()
{
    // Minimal preset forces mode 3 (third-person generated-pose capture) -- the
    // only source proven to carry a real first-person walk.
    if (IsMinimalMode()) return 3;
    // Same cached key; IsFirstPersonCaptureEnabled does the reading and the
    // logging, so calling it here keeps one source of truth for both.
    IsFirstPersonCaptureEnabled();
    static int s_mode = -1;
    if (s_mode < 0)
    {
        char exePath[MAX_PATH] = {};
        if (::GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) { s_mode = 0; return 0; }
        std::string ini(exePath);
        const size_t slash = ini.find_last_of("\\/");
        ini = (slash == std::string::npos) ? std::string() : ini.substr(0, slash + 1);
        ini += "Data\\F4SE\\Plugins\\CoSync.ini";
        s_mode = ::GetPrivateProfileIntA("Animations", "bCaptureFirstPersonSkeleton", 0, ini.c_str());
    }
    return s_mode;
}

// Wire format (2026-08-21):
//
//   BONES|<entityID>|<boneCount>|<hash8>,<qx>,<qy>,<qz>,<qw>|...|<trCount>|<hash8>,<x>,<y>,<z>;...
//
// <hash8> is HashBoneName written as 8 lowercase hex digits -- the bone's
// identity, carried with the bone, needing no agreement between the two
// machines. That replaced a canonical INDEX, which needed both sides to have
// built the identical name list and demonstrably could not guarantee it (see
// the wire-format note in CoSyncPlayerAnimation.h for the measurements).
//
// Absent bones are simply not in the packet; the receiver keeps whatever it
// last had for them. The old format spent 13 bytes per absent bone on a
// qw=2.0 sentinel to say the same thing.
// Set by the local player tick each frame; travels with the packet so the
// receiver knows the lower half of this pose is not real.

void SetLocalFirstPerson(bool inFirstPerson) noexcept
{
    g_localFirstPerson.store(inFirstPerson, std::memory_order_relaxed);
}

std::string SerializeSkeletonCapture(uint32_t entityID, const CoSyncPlayerAnimation::SkeletonCapture& capture)
{
    if (!capture.valid || capture.count == 0)
        return std::string();

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4);

    std::ostringstream body;
    body << std::fixed << std::setprecision(4);
    uint32_t written = 0;

    std::ostringstream trss;
    trss << std::fixed << std::setprecision(4);
    uint32_t trCount = 0;

    // Index 0 is the root BSFadeNode -- its transform is the actor's world
    // position, which WorldSync owns. Never send it.
    for (uint32_t i = 1; i < capture.count; ++i)
    {
        const CoSyncPlayerAnimation::BoneTransform& bt = capture.bones[i];
        if (bt.nameHash == 0u) continue;

        float q[4];
        Mat3ToQuat(bt.rot, q);

        char hashBuf[16];
        std::snprintf(hashBuf, sizeof(hashBuf), "%08x", bt.nameHash);

        body << "|" << hashBuf << ","
             << q[0] << "," << q[1] << "," << q[2] << "," << q[3];
        ++written;

        // Restricted translation channel -- COM/Pelvis only. FO4_Wrld's
        // "crouch channel" design: writing translation onto an arbitrary
        // matched bone is what stretched their model when two skeletons'
        // assumptions diverged, so every other bone is rotation-only.
        if (std::strcmp(bt.name, "COM") == 0 || std::strcmp(bt.name, "Pelvis") == 0)
        {
            if (trCount > 0) trss << ";";
            trss << hashBuf << "," << bt.trans[0] << "," << bt.trans[1] << "," << bt.trans[2];
            ++trCount;
        }
    }

    if (written == 0)
        return std::string();

    ss << "BONES|" << entityID << "|" << written << "|" << (g_localFirstPerson ? 1 : 0) << body.str()
       << "|" << trCount << "|" << trss.str();

    return ss.str();
}

bool IsBonesPacket(const std::string& msg)
{
    return msg.rfind("BONES|", 0) == 0;
}

namespace
{
    std::mutex s_recvSkelMutex;
    std::unordered_map<uint32_t, CoSyncPlayerAnimation::SkeletonCapture> s_recvSkeletons;
}

void OnReceiveBones(const std::string& msg)
{
    LOG_REQUIREMENT("(global)::OnReceiveBones", "Needs a well-formed skeleton-data message. Bad or cut-off data is dropped without touching what's already stored for that entity. Each bone carries its own ID, so a packet can be read as soon as it arrives, no other state needed.");
    if (!IsBonesPacket(msg))
        return;

    const char* p = msg.c_str() + 6; // skip "BONES|"
    char* e = nullptr;
    const uint32_t entityID = static_cast<uint32_t>(std::strtoul(p, &e, 10));
    if (!e || *e != '|' || entityID == 0)
        return;
    p = e + 1;

    const uint32_t n = static_cast<uint32_t>(std::strtoul(p, &e, 10));
    if (!e || *e != '|' || n == 0 || n > CoSyncPlayerAnimation::MAX_SKELETON_BONES - 1)
        return;
    p = e + 1;

    const uint32_t fpFlag = static_cast<uint32_t>(std::strtoul(p, &e, 10));
    if (!e || *e != '|') return;
    p = e + 1;

    CoSyncPlayerAnimation::SkeletonCapture cap{};
    cap.upperBodyOnly = (fpFlag != 0);
    cap.bones[0].name[0]  = '\0';   // root placeholder -- ApplySkeleton always skips index 0
    cap.bones[0].nameHash = 0u;
    cap.count = n + 1;

    for (uint32_t i = 0; i < n; ++i)
    {
        CoSyncPlayerAnimation::BoneTransform& bt = cap.bones[i + 1];

        const uint32_t hash = static_cast<uint32_t>(std::strtoul(p, &e, 16));
        if (e == p || *e != ',') return;
        p = e + 1;

        float q[4];
        for (int k = 0; k < 4; ++k)
        {
            q[k] = std::strtof(p, &e);
            if (e == p) return;
            if (k < 3)
            {
                if (*e != ',') return;
            }
            else
            {
                if (*e != '|') return;
            }
            p = e + 1;
        }

        bt.name[0]  = '\0';   // names do not cross the wire; the hash is the identity
        bt.nameHash = hash;
        bt.trans[0] = bt.trans[1] = bt.trans[2] = 0.0f;
        bt.scale    = 1.0f;
        QuatToMat3(q, bt.rot);
    }

    const uint32_t trCount = static_cast<uint32_t>(std::strtoul(p, &e, 10));
    if (!e || *e != '|') return;
    p = e + 1;

    for (uint32_t i = 0; i < trCount; ++i)
    {
        const uint32_t hash = static_cast<uint32_t>(std::strtoul(p, &e, 16));
        if (e == p || *e != ',') return;
        p = e + 1;

        float tx, ty, tz;
        tx = std::strtof(p, &e); if (e == p || *e != ',') return; p = e + 1;
        ty = std::strtof(p, &e); if (e == p || *e != ',') return; p = e + 1;
        tz = std::strtof(p, &e); if (e == p) return;
        p = (*e == ';') ? e + 1 : e;

        for (uint32_t j = 1; j < cap.count; ++j)
        {
            if (cap.bones[j].nameHash == hash)
            {
                cap.bones[j].trans[0] = tx;
                cap.bones[j].trans[1] = ty;
                cap.bones[j].trans[2] = tz;
                break;
            }
        }
    }

    cap.valid  = true;
    cap.recvMs = GetTickCount64();   // drives ApplySkeleton's FRESH/HOLD/RELEASE tiering

    std::lock_guard<std::mutex> lk(s_recvSkelMutex);
    s_recvSkeletons[entityID] = cap;
}

bool GetReceivedSkeleton(uint32_t entityID, CoSyncPlayerAnimation::SkeletonCapture& out)
{
    std::lock_guard<std::mutex> lk(s_recvSkelMutex);
    auto it = s_recvSkeletons.find(entityID);
    if (it == s_recvSkeletons.end())
        return false;
    out = it->second;
    return true;
}

void ForgetReceivedSkeleton(uint32_t entityID)
{
    std::lock_guard<std::mutex> lk(s_recvSkelMutex);
    s_recvSkeletons.erase(entityID);
}

} // namespace CoSyncPlayerAnimation
