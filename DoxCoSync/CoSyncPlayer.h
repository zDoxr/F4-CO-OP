#pragma once

#include <string>
#include <cstdint>
#include <deque>

#include "NiTypes.h"
#include "GameReferences.h"
#include "LocalPlayerState.h"
#include "Packets_EntityUpdate.h"   // explicit, do NOT pull from PlayerManager

// -----------------------------------------------------------------------------
// CoSyncPlayer
//
// F4MP-aligned remote entity proxy
//
// RESPONSIBILITIES:
//   ✔ Owns exactly ONE Actor* proxy
//   ✔ Applies authoritative UPDATE packets
//   ✔ Smooths movement (client-side only)
//   ✔ NEVER sends network data
//   ✔ NEVER decides authority
//
// HARD RULES:
//   - isLocalPlayer is ALWAYS false
//   - Spawning happens ONLY via CREATE
//   - UPDATE never causes spawn
// -----------------------------------------------------------------------------
class CoSyncPlayer
{
public:
    explicit CoSyncPlayer(const std::string& name);

    // -------------------------------------------------------------------------
    // Spawn (GAME THREAD ONLY)
    // Called ONLY by CoSyncSpawnTasks
    // -------------------------------------------------------------------------
    bool SpawnInWorld(TESObjectREFR* anchor, TESNPC* npcBase);

    // -------------------------------------------------------------------------
    // Network replication (GAME THREAD)
    // -------------------------------------------------------------------------
    void ApplyUpdate(const EntityUpdatePacket& u);

    // Applies cached transform if UPDATE arrived before spawn
    void ApplyPendingTransformIfAny();

    // Per-frame interpolation (client-side smoothing)
    void TickSmoothing(double now);

public:
    // -------------------------------------------------------------------------
    // Identity
    // -------------------------------------------------------------------------
    uint32_t    entityID = 0;
    uint32_t    ownerEntityID = 0;
    std::string username;

    // -------------------------------------------------------------------------
    // Authority
    // -------------------------------------------------------------------------
    // ALWAYS false for CoSyncPlayer
    bool isLocalPlayer = false;

    // -------------------------------------------------------------------------
    // Actor proxy
    // -------------------------------------------------------------------------
    Actor* actorRef = nullptr;
    bool   hasSpawned = false;

    // -------------------------------------------------------------------------
    // Buffered transform (UPDATE-before-spawn safety)
    // -------------------------------------------------------------------------
    bool     hasPendingTransform = false;
    NiPoint3 pendingPos{ 0.f, 0.f, 0.f };
    NiPoint3 pendingRot{ 0.f, 0.f, 0.f };
    NiPoint3 pendingVel{ 0.f, 0.f, 0.f }; // informational only

    // -----------------------------------------------------------------------------
// F4MP-style transform buffer
// -----------------------------------------------------------------------------
    struct NetTransform
    {
        NiPoint3 pos{ 0.f, 0.f, 0.f };
        NiPoint3 rot{ 0.f, 0.f, 0.f };
        float    scale = 1.0f;

        double   t = 0.0; // host-time (authoritative timeline)
    };

    std::deque<NetTransform> m_tfBuffer;

    // Buffer tuning (F4MP-ish)
    double m_lastHostT = 0.0;
    bool   m_hasAny = false;


    // -------------------------------------------------------------------------
    // Timing / telemetry
    // -------------------------------------------------------------------------
    double lastPacketTime = 0.0;

    // -------------------------------------------------------------------------
    // Debug / future expansion
    // -------------------------------------------------------------------------
    LocalPlayerState lastState{};




    bool IsOwner(uint32_t localEntityID) const
    {
        return ownerEntityID != 0 && ownerEntityID == localEntityID;
    }
};
