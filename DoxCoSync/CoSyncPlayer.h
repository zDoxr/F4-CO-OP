#pragma once

#include <string>
#include <cstdint>

#include "NiTypes.h"
#include "GameReferences.h"
#include "LocalPlayerState.h"

// -----------------------------------------------------------------------------
// CoSyncPlayer
//
// F4MP-aligned remote entity proxy
//
// Represents ONE remote network entity.
// Owns exactly ONE Actor* spawned via PlaceAtMe.
// Never authoritative. Never local.
//
// RULES (MATCHES F4MP):
//  - Spawn ONLY via CREATE
//  - UPDATE never spawns
//  - Void-spawn first
//  - Transforms buffered until actor exists
// -----------------------------------------------------------------------------
class CoSyncPlayer
{
public:
    explicit CoSyncPlayer(const std::string& name);

    // -------------------------------------------------------------------------
    // Spawn (CREATE → game thread only)
    // -------------------------------------------------------------------------
    bool SpawnInWorld(TESObjectREFR* anchor, TESForm* baseForm);

    // -------------------------------------------------------------------------
    // Replication
    // -------------------------------------------------------------------------
    void ApplyPendingTransformIfAny();
    void UpdateFromState(const LocalPlayerState& state);

public:
    // -------------------------------------------------------------------------
    // Identity
    // -------------------------------------------------------------------------
    uint32_t    entityID = 0;
    std::string username;

    // -------------------------------------------------------------------------
    // Authority
    // -------------------------------------------------------------------------
    bool isLocalPlayer = false; // ALWAYS false for CoSyncPlayer

    // -------------------------------------------------------------------------
    // Actor proxy
    // -------------------------------------------------------------------------
    Actor* actorRef = nullptr;
    bool   hasSpawned = false;

    // -------------------------------------------------------------------------
    // Buffered transform (UPDATE before spawn safe)
    // -------------------------------------------------------------------------
    bool     hasPendingTransform = false;
    NiPoint3 pendingPos{ 0.f, 0.f, 0.f };
    NiPoint3 pendingRot{ 0.f, 0.f, 0.f };
    NiPoint3 pendingVel{ 0.f, 0.f, 0.f }; // informational only

    // -------------------------------------------------------------------------
    // Timing
    // -------------------------------------------------------------------------
    double lastPacketTime = 0.0;

    // Debug / future
    LocalPlayerState lastState{};
};
