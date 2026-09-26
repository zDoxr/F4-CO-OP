#pragma once

#include <string>
#include <cstdint>
#include "NiTypes.h"

namespace CoSyncPowerArmorSync
{

    bool IsPowerArmor(const std::string& msg);


    // `chassisPos` is the REAL chassis ref's world position at the moment of
    // the enter/exit event (radians/game-units, straight off akPowerArmor->pos)
    // -- see NotifyPowerArmor's own comment for why this exists.
    std::string SerializePowerArmor(
        uint32_t        senderEntityID,
        uint32_t        chassisBaseFormID,
        uint32_t        chassisRefFormID,
        bool            entering,
        const uint32_t  pieceFormIDs[6],
        const NiPoint3& chassisPos);


    // 2026-08-26: `chassisPos` added so every OTHER client can find ITS OWN
    // local representation of the real-world frame the sender interacted
    // with -- not the fake clone we spawn for the proxy attach (that part
    // already works and is unrelated). The chassis ref's formID is almost
    // never directly resolvable cross-machine (dynamic 0xFF ref for anything
    // workshop-built or console-spawned; only a handful of CK-placed frames
    // share a formID across every client) -- position + base form is the
    // one thing every client CAN agree on, since the same physical object
    // (or an already-synced copy of it, spawned by the existing generic
    // workshop/world-object scan for any dynamic ref of a buildable form
    // type -- FURN=42 is already in that list) sits at a matching position
    // in every client's own loaded world. Full circle: pre-placed, workshop-
    // built, and console-spawned frames are all found the same way.
    void NotifyPowerArmor(
        uint32_t        senderEntityID,
        uint32_t        chassisBaseFormID,
        uint32_t        chassisRefFormID,
        bool            entering,
        const uint32_t  pieceFormIDs[6],
        const NiPoint3& chassisPos);


    void OnReceivePowerArmor(const std::string& msg);


    void FlushPendingPowerArmorOps();

    // Sender-side PA state tracking, keyed by the REAL actor's own formID
    // (not an entity ID -- this runs before/independent of any network
    // send). Lets CoSyncAnimVarSystem::SaveAnimationVariables force the
    // humanoid capture path for an actor confirmed in Power Armor, instead
    // of trusting the variable-set-size heuristic (kHumanoidMinSize) alone
    // -- PA's active graph was confirmed (2026-08-01 live test) to report a
    // small enough variable-set size to be misclassified as a creature,
    // which silently skipped all PA-specific sync (LoadPowerArmorLocomotion,
    // named-setter variable writes) for the whole PA session.
    void MarkActorPowerArmorState(uint32_t actorFormID, bool inPowerArmor);
    bool IsActorKnownInPowerArmor(uint32_t actorFormID);

    // Phase 8: "PAMODS|sender|ref|base|x|y|z|ITEM:MOD/MOD,..." -- the object
    // mods (paint, jetpack, linings) on an unworn frame's pieces; receivers
    // attach them to their copy of that frame (retrying while pieces arrive).
    bool IsPowerArmorMods(const std::string& msg);

    // Local player currently occupies a Power Armor frame (race check).
    bool IsLocalPlayerInPowerArmor();
    // Ejects the local player (native SwitchFromPowerArmor). Returns the frame
    // ref it was in (0 if unknown). The exit completes over the next frames.
    uint32_t ExitLocalPlayerPowerArmor();
    void OnReceivePowerArmorMods(const std::string& msg);

    // 2026-09-05: "PAI|sender|ref|base|x|y|z|fid:count,..." -- inventory
    // snapshot of an UNWORN power armor frame (pieces / fusion cores swapped
    // at a station or via the transfer menu). Both roles publish changes for
    // frames near them; receivers diff their local copy of the frame and
    // AddItem/RemoveItem to match. Worn frames are excluded (their pieces
    // travel in the PA| enter packet instead).
    bool IsPowerArmorInventory(const std::string& msg);
    void OnReceivePowerArmorInventory(const std::string& msg);

    // Clears session state (frame signatures, pending ops, fallback timers).
    void Reset();
}
