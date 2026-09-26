

#pragma once

#include <cstdint>
#include <vector>
#include <cstring>
#include "CoSyncRequirementsLog.h"

class Actor;

struct AnimVarSnapshot
{
    uint64_t booleans = 0;

    static constexpr int NUM_FLOATS = 32;
    static constexpr int NUM_INTS   = 33;

    float    floats[NUM_FLOATS] = {};
    uint32_t integers[NUM_INTS] = {};


    uint32_t actorStateFlags1 = 0;
    uint32_t actorStateFlags2 = 0;


    float direction = 0.0f;


    bool isValid = false;


    bool operator==(const AnimVarSnapshot& other) const
    {
        LOG_REQUIREMENT("AnimVarSnapshot::operator==", "Doesn't touch the game. Just needs a valid snapshot to compare against.");
        if (booleans != other.booleans) return false;
        if (memcmp(floats, other.floats, sizeof(floats)) != 0) return false;
        if (memcmp(integers, other.integers, sizeof(integers)) != 0) return false;
        return true;
    }

    bool operator!=(const AnimVarSnapshot& other) const { LOG_REQUIREMENT("AnimVarSnapshot::operator!=", "Doesn't touch the game. Just needs a valid snapshot -- passes the comparison to operator==."); return !(*this == other); }

    size_t Serialize(uint8_t* buffer, size_t bufferSize) const;


    size_t Deserialize(const uint8_t* buffer, size_t bufferSize);


    static constexpr size_t SerializedSize()
    {
        return sizeof(uint64_t)
            + sizeof(float) * NUM_FLOATS
            + sizeof(uint32_t) * NUM_INTS
            + sizeof(uint32_t) * 2
            + sizeof(float);
    }
};

namespace CoSyncAnimVarSystem
{

    bool SaveAnimationVariables(Actor* actor, AnimVarSnapshot& outSnapshot);

    bool LoadAnimationVariables(Actor* actor, const AnimVarSnapshot& snapshot);

    // PA-safe locomotion sync: the proxy's behavior graph while in Power Armor
    // is an entirely different graph than the human one LoadAnimationVariables
    // targets, with its own variable index table -- raw varSet.data[idx]
    // writes using human TP::Variables indices would corrupt it, which is why
    // TickAnimVars skips LoadAnimationVariables entirely while in PA. This
    // writes ONLY through the by-name engine setter (CoSyncAnimUtils::Set*),
    // which re-resolves the correct index per graph on every call and simply
    // no-ops for a name the active graph doesn't have -- safe against any
    // graph, unlike a raw indexed write. Deliberately narrower than the full
    // named-setter set in LoadAnimationVariables (which is disabled there for
    // performance/hang reasons under high-frequency human-actor load): only
    // called for the rare PA-proxy case, so the cost of the extra named calls
    // doesn't matter here.
    bool LoadPowerArmorLocomotion(Actor* actor, const AnimVarSnapshot& snapshot);

    bool IsGraphReady(Actor* actor);

    // 2026-08-21 -- for CoSyncFirstPersonGraph's manually-driven third-person
    // graph (bDeltaPositionLocomotion=2's "drive" experiment). Clears
    // bAnimationDriven/bAllowRotation/bGraphDrivenTranslation/
    // bGraphDrivenRotation/bFreezeSpeedUpdate/bFreezeRotationUpdate to false
    // -- the SAME flag set + values LoadAnimationVariables already uses for
    // driven NPC puppets (proven safe/working there) -- but via
    // GetVariableSetForWrite's raw indexed write, which resolves
    // animationGraphs.Get(0) UNCONDITIONALLY (always the third-person graph
    // slot, never influenced by which graph is currently active). This is
    // NOT the holder-based SetGraphVariable* API (that dispatches through
    // IAnimationGraphManagerHolder and loops ALL of an actor's graphs,
    // which is what [[feedback-no-graph-variable-writes-in-bone-replication]]
    // forbids on the sender's local player -- it previously bled into their
    // real, rendered first-person graph too, "messing up functionality").
    // This write physically cannot reach graph slot 1 (first-person): it
    // never resolves through the holder at all, only ever touches slot 0's
    // own raw variable-set array directly. Does NOT touch IsPlayer/IsNPC/
    // iIsPlayer (unlike LoadAnimationVariables' NPC-puppet block) -- this is
    // the real player, that identity must stay untouched. Safe to call
    // every driven frame; cheap no-op if the graph isn't ready yet.
    void ClearParkedGraphFreezeFlags(Actor* actor) noexcept;

    void DumpVariables(Actor* actor, const char* label = "");

    // Diagnostic only: for every named variable in TP::Variables, probes the
    // live behavior graph (via the confirmed-safe CoSyncAnimUtils Get/Set
    // calls) to find which array slot the engine's own name->index hash
    // lookup actually resolves that name to, and compares it against the
    // hardcoded index in TP::Variables. These indices are assigned by
    // Bethesda's behavior-graph compiler from the .hkx data files, not by
    // code in Fallout4.exe, so they can't be checked via static/IDA analysis
    // -- this is the only way to get a real answer. Restores every value it
    // touches immediately after probing. Logs one line per mismatch/failure
    // plus a final summary line.
    void VerifyVariableIndices(Actor* actor);

    // Diagnostic only: walks the ACTIVE graph's own name->index hash table
    // (same read-only mechanism as LoadPowerArmorLocomotion's
    // ResolveGraphVariableIndex) and logs every {index, name} pair it finds by
    // extracting the interned string content directly from each entry's key
    // pointer. Unlike VerifyVariableIndices this doesn't probe a hardcoded
    // name list -- it dumps what's ACTUALLY there, which is the only way to
    // find out what a graph with an unknown/different naming convention (e.g.
    // the Power Armor graph) really exposes.
    void DumpGraphVariableNames(Actor* actor);

    // Read-only. For a HOST-OWNED NPC, resolves Speed/Direction by name out of
    // the graph's own name->index map and logs that index+value against the
    // hardcoded TP:: index+value. Decides between "our index table is wrong for
    // this graph" and "the actor really is frozen" -- the two explanations left
    // standing for the host-side NPC A-pose. Throttled by the caller.
    void DiagnoseCaptureIndices(Actor* actor, uint32_t entityID);

    // Diagnostic only: logs the anim holder's active graph index/count for
    // `actor` under `context` (e.g. "PowerArmorEnter", "PAMoveStart"). See
    // the CoSyncAnimVarSystem.cpp definition for the full rationale -- this
    // exists to confirm or rule out a graph-object mismatch as the cause of
    // PA-specific animation events being universally rejected by the engine.
    void DumpGraphIndexDiag(Actor* actor, const char* context);
}
