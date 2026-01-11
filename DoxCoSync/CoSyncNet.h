#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>

#include "NiTypes.h"
#include "LocalPlayerState.h"

// -----------------------------------------------------------------------------
// CoSyncNet
//
// RESPONSIBILITIES (STRICT):
//  - Session lifecycle (host/client)
//  - Network send / receive
//  - Authority routing
//  - Packet parsing + enqueue ONLY
//
// HARD RULES:
//  - NEVER spawns actors
//  - NEVER moves actors
//  - NEVER touches the world
//
// World interaction lives in:
//   - CoSyncPlayerManager
//   - CoSyncSpawnTasks
// -----------------------------------------------------------------------------
class CoSyncNet
{
public:
    // -------------------------------------------------------------------------
    // RemotePeer
    //
    // Diagnostic + bookkeeping ONLY.
    // NOT authoritative world state.
    // -------------------------------------------------------------------------
    struct RemotePeer
    {
        uint64_t steamID = 0;
        std::string username;

        // Debug / UI only
        LocalPlayerState lastState{};
        double lastUpdateTime = 0.0;

        // Deterministic entity assignment (host-side)
        uint32_t assignedEntityID = 0;
    };

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------
    static void Init(bool isHost);
    static void Shutdown();

    // Deferred init until world is ready
    static void ScheduleInit(bool isHost);
    static void PerformPendingInit();

    // Per-frame tick (game thread only)
    // Does NOT touch world state
    static void Tick(double now);

    // -------------------------------------------------------------------------
    // Sending
    // -------------------------------------------------------------------------

    // CLIENT → HOST
    // Sends ONLY transform updates
    static void SendMyEntityUpdate(
        uint32_t entityID,
        const NiPoint3& pos,
        const NiPoint3& rot,
        const NiPoint3& vel
    );

    // -------------------------------------------------------------------------
    // Receiving
    // -------------------------------------------------------------------------

    // Called ONLY by CoSyncTransport receive callback
    // Must NEVER touch the world
    static void OnReceive(const std::string& msg, double now);

    // -------------------------------------------------------------------------
    // Identity
    // -------------------------------------------------------------------------
    static uint64_t GetMySteamID();
    static uint32_t GetMyEntityID();

    static const char* GetMyName();
    static void SetMyName(const std::string& name);

    static bool IsHost();

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------
    static bool IsInitialized();
    static bool IsSessionActive();
    static bool IsConnected();

    // Diagnostics (NOT authoritative)
    static const std::unordered_map<uint64_t, RemotePeer>& GetPeers();

    // -------------------------------------------------------------------------
    // Transport callbacks
    // -------------------------------------------------------------------------
    static void OnGNSConnected();
    static void OnGNSDisconnected();
};
