#include "CoSyncCreatureAnimSync.h"
#include "CoSyncBehaviorGraph.h"
#include "CoSyncAnimationUtils.h"
#include "ConsoleLogger.h"

#include "GameReferences.h"
#include "GameTypes.h"

// UPDATE 2026-06-17: __declspec(noinline) is load-bearing -- a non-noinline
// static __try/__except function risks silent SEH-removal via inlining
// under this project's /GL+/LTCG Release config (see CoSyncDoorHook.cpp's
// SafeReadU32/SafeReadPtr for the fuller writeup and confirmed repro).
__declspec(noinline) static bool HolderGetFloat(Actor* actor, const char* name, float& out)
{
    if (!actor) return false;

    __try
    {

        void* holder = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(static_cast<TESObjectREFR*>(actor)) + 0x48);

        void*** vtablePtr = reinterpret_cast<void***>(holder);
        if (!vtablePtr || !*vtablePtr)
            return false;

        void* fn = (*vtablePtr)[19];
        if (!fn) return false;

        typedef bool(*GetFloatFn)(void*, BSFixedString*, float*);
        BSFixedString bsName(name);
        return reinterpret_cast<GetFloatFn>(fn)(holder, &bsName, &out);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

__declspec(noinline) static bool HolderGetBool(Actor* actor, const char* name, bool& out)
{
    if (!actor) return false;

    __try
    {
        void* holder = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(static_cast<TESObjectREFR*>(actor)) + 0x48);

        void*** vtablePtr = reinterpret_cast<void***>(holder);
        if (!vtablePtr || !*vtablePtr)
            return false;

        void* fn = (*vtablePtr)[21];
        if (!fn) return false;

        typedef bool(*GetBoolFn)(void*, BSFixedString*, bool*);
        BSFixedString bsName(name);
        return reinterpret_cast<GetBoolFn>(fn)(holder, &bsName, &out);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool CoSyncCreatureAnimSync::IsCreatureBehavior(uint32_t varSetSize)
{
    return varSetSize < kHumanoidMinSize;
}

bool CoSyncCreatureAnimSync::SaveCreatureVars(Actor* actor, AnimVarSnapshot& out)
{
    out = AnimVarSnapshot{};
    out.booleans |= kCreatureSnapshotFlag;

    bool anyRead = false;


    float speed = 0.f;
    if (HolderGetFloat(actor, "Speed", speed))
    {
        out.floats[4] = speed;
        anyRead = true;
    }


    float dir = 0.f;
    if (HolderGetFloat(actor, "Direction", dir))
        out.floats[0] = dir;


    float pitch = 0.f;
    if (HolderGetFloat(actor, "Pitch", pitch))
        out.floats[6] = pitch;


    bool sprinting = false;
    const bool sprintResolved = HolderGetBool(actor, "bIsSprinting", sprinting);
    if (sprintResolved && sprinting)
        out.booleans |= (1ULL << 1);


    bool sneaking = false;
    const bool sneakAngleResolved = HolderGetBool(actor, "bIsInSneakAngle", sneaking);
    bool sneakFallbackResolved = false;
    if (!sneakAngleResolved)
        sneakFallbackResolved = HolderGetBool(actor, "iIsInSneak", sneaking);
    if (sneaking)
        out.booleans |= (1ULL << 6);

    // These variable names were never independently confirmed for creature
    // behavior graphs the way TP::Variables was for humanoid ones (creatures
    // may use an entirely different graph/name set, and that can't be
    // checked statically either -- same data-driven limitation). One-time
    // log so a real creature encounter in-game tells us which names actually
    // resolve, instead of this failing silently forever.
    static bool s_nameDiagDone = false;
    if (!s_nameDiagDone)
    {
        s_nameDiagDone = true;
        LOG_INFO("[CreatureAnim] NAME CHECK actor=0x%08X: bIsSprinting=%s "
            "bIsInSneakAngle=%s iIsInSneak(fallback)=%s",
            actor->formID,
            sprintResolved ? "OK" : "FAILED",
            sneakAngleResolved ? "OK" : "FAILED",
            sneakAngleResolved ? "n/a" : (sneakFallbackResolved ? "OK" : "FAILED"));
    }

    // Generic Actor+0x130/+0x134 fields (round 138 IDA verification), not
    // humanoid-graph-specific -- same direct struct read
    // CoSyncAnimVarSystem::SaveAnimationVariables already uses for humanoid
    // actors (actor->actorState.unk08/.flags). Safe to read for any Actor*;
    // needed so TickAnimVars' shared attack/sneak dispatch blocks (which key
    // off actorStateFlags1/2, not graph-specific vars) have real data for
    // creature proxies instead of a stale/zero default.
    __try
    {
        out.actorStateFlags1 = actor->actorState.unk08;
        out.actorStateFlags2 = actor->actorState.flags;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    // TickAnimVars' shared moveStart/moveStop/sprintStart/sprintStop logic
    // keys off integers[15] ("locomotion tier": >0 = moving, >=3 = sprinting)
    // -- a humanoid iState-derived value creature graphs have no equivalent
    // for. Synthesize an equivalent tier from data already captured above so
    // creatures get the same move/sprint event dispatch. The 5.0f "moving"
    // threshold is a first guess -- check it against real creature Speed
    // values via the NAME CHECK/Save diagnostics below on first live test.
    out.integers[15] = sprinting ? 3 : (speed > 5.0f ? 1 : 0);

    out.isValid = anyRead;

    static uint32_t s_logCount = 0;
    if (++s_logCount % 600 == 1)
        LOG_INFO("[CreatureAnim] Save actor=0x%08X speed=%.2f dir=%.2f sprint=%d",
            actor->formID, speed, dir, (int)sprinting);

    return anyRead;
}

bool CoSyncCreatureAnimSync::LoadCreatureVars(Actor* actor, const AnimVarSnapshot& snap)
{
    if (!actor || !snap.isValid) return false;
    if (!(snap.booleans & kCreatureSnapshotFlag)) return false;

    const float speed  = snap.floats[4];
    const float dir    = snap.floats[0];
    const bool  sprint = (snap.booleans & (1ULL << 1)) != 0;
    const bool  sneak  = (snap.booleans & (1ULL << 6)) != 0;


    // Was CoSync::BehaviorGraphHelper::SetFloatVariable/SetBoolVariable, which
    // reaches the graph via actor->middleProcess->unk08[11] (brute-forcing
    // through 64 slots with a loose "looks like a vtable" heuristic if that
    // guess misses) and tries 4 guessed vtable slot indices for the Set call
    // itself, accepting whichever doesn't throw -- much more fragile than
    // necessary. This file's own HolderGetFloat/HolderGetBool above already
    // prove the confirmed-correct actor+0x48 holder mechanism (verified via
    // IDA against Fallout4.exe 1.11.221) works fine for creatures too, so
    // there's no need for the separate path. CoSyncAnimUtils::SetFloat/SetBool
    // now use the real, safe write mechanism (see CoSyncAnimationUtils.cpp).
    //
    // round 28: CoSyncAnimUtils::SetBool/SetFloat call the real engine setter
    // (g_realSetAnimVarBool/Float, CoSyncAnimationUtils.cpp) unconditionally.
    // This crashed live with STATUS_ILLEGAL_INSTRUCTION inside that real
    // setter for "bIsSprinting" on a creature whose own NAME CHECK diagnostic
    // (above, in SaveCreatureVars) had ALREADY logged this exact name as
    // FAILED to resolve for this actor's graph a few lines earlier in the
    // same log. A fail-fast-shaped crash (illegal instruction, not a normal
    // AV) bypasses SEH entirely -- CoSyncAnimUtils::SetBool's own __except IS
    // correctly wrapping the call, it structurally cannot catch this --
    // matching round 23's AnimVarSystem UAF lesson: when a name has no slot
    // in a (creature) graph's much smaller variable table, writing to it
    // corrupts the engine's own lookup/insert state rather than safely
    // no-op'ing. Probe with the same proven-safe HolderGetBool/HolderGetFloat
    // mechanism first -- if the Get doesn't resolve, the graph has no slot
    // for that name, and skip the Set rather than risk it.
    float speedProbe = 0.f;
    if (HolderGetFloat(actor, "Speed", speedProbe))
        CoSyncAnimUtils::SetFloat(actor, "Speed", speed);

    float dirProbe = 0.f;
    if (HolderGetFloat(actor, "Direction", dirProbe))
        CoSyncAnimUtils::SetFloat(actor, "Direction", dir);

    bool sprintProbe = false;
    if (HolderGetBool(actor, "bIsSprinting", sprintProbe))
        CoSyncAnimUtils::SetBool(actor, "bIsSprinting", sprint);

    bool sneakProbe = false;
    if (HolderGetBool(actor, "bIsInSneakAngle", sneakProbe))
        CoSyncAnimUtils::SetBool(actor, "bIsInSneakAngle", sneak);


    if (snap.floats[6] != 0.f)
    {
        float pitchProbe = 0.f;
        if (HolderGetFloat(actor, "Pitch", pitchProbe))
            CoSyncAnimUtils::SetFloat(actor, "Pitch", snap.floats[6]);
    }

    static uint32_t s_logCount = 0;
    if (++s_logCount % 600 == 1)
        LOG_INFO("[CreatureAnim] Load actor=0x%08X speed=%.2f dir=%.2f",
            actor->formID, speed, dir);

    return true;
}
