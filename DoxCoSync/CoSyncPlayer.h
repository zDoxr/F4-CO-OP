#pragma once

#include <string>
#include <cstdint>
#include <deque>

#include "NiTypes.h"
#include "GameReferences.h"
#include "LocalPlayerState.h"
#include "Packets_EntityUpdate.h"
#include "CoSyncEntityTypes.h"


// -----------------------------------------------------------------------------
// CoSyncPlayer
//
// World proxy for a remote or host-simulated entity.
// This class NEVER owns authority decisions — it only applies them.
// -----------------------------------------------------------------------------
class CoSyncPlayer
{
public:
    explicit CoSyncPlayer(const std::string& name);

    // ---------------------------------------------------------------------
    // Spawn (GAME THREAD ONLY)
    // ---------------------------------------------------------------------
    bool SpawnInWorld(TESObjectREFR* anchor, TESNPC* npcBase);

    // ---------------------------------------------------------------------
    // Network replication
    // ---------------------------------------------------------------------
    void ApplyUpdate(const EntityUpdatePacket& u);

    // Apply cached transform if UPDATE arrived before spawn
    void ApplyPendingTransformIfAny();

    // Per-frame interpolation (players only)
    void TickSmoothing(double now);

public:
    // ---------------------------------------------------------------------
    // Identity
    // ---------------------------------------------------------------------
    uint32_t    entityID = 0;
    uint32_t    ownerEntityID = 0;
    std::string username;

    // ---------------------------------------------------------------------
    // Creation metadata (from CREATE packet)
    // ---------------------------------------------------------------------
    CoSyncEntityType createType = CoSyncEntityType::Player;
    bool isRemoteControlled = false;

    // ---------------------------------------------------------------------
    // Authority flags
    // ---------------------------------------------------------------------
    bool isLocalPlayer = false;

    bool IsOwner(uint32_t localEntityID) const
    {
        return ownerEntityID != 0 && ownerEntityID == localEntityID;
    }

    // ---------------------------------------------------------------------
    // Actor proxy
    // ---------------------------------------------------------------------
    Actor* actorRef = nullptr;
    bool   hasSpawned = false;

    // ---------------------------------------------------------------------
    // Authoritative transform (HOST-OWNED ENTITIES ONLY)
    //
    // Used for:
    //   - Host-only debug NPC hard snapping
    //   - Future AI / NPC simulation
    // ---------------------------------------------------------------------
    NiPoint3 authoritativePos{ 0.f, 0.f, 0.f };
    NiPoint3 authoritativeRot{ 0.f, 0.f, 0.f };

    // ---------------------------------------------------------------------
    // Pending transform (UPDATE-before-spawn)
    // ---------------------------------------------------------------------
    bool     hasPendingTransform = false;
    NiPoint3 pendingPos{ 0.f, 0.f, 0.f };
    NiPoint3 pendingRot{ 0.f, 0.f, 0.f };
    NiPoint3 pendingVel{ 0.f, 0.f, 0.f };

    // ---------------------------------------------------------------------
    // F4MP-style transform buffer (players only)
    // ---------------------------------------------------------------------
    struct NetTransform
    {
        NiPoint3 pos{ 0.f, 0.f, 0.f };
        NiPoint3 rot{ 0.f, 0.f, 0.f };
        float    scale = 1.0f;
        double   hostTime = 0.0; // authoritative host timestamp
    };

    std::deque<NetTransform> m_tfBuffer;

    // ---------------------------------------------------------------------
    // Timing
    // ---------------------------------------------------------------------
    double m_lastHostTime = 0.0;
    double m_lastRecvLocalTime = 0.0;
    bool   m_hasAny = false;

    // ---------------------------------------------------------------------
    // Diagnostics / debugging
    // ---------------------------------------------------------------------
    double lastPacketTime = 0.0;
    LocalPlayerState lastState{};
};
