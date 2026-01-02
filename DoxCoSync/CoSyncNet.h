#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>

#include "NiTypes.h"
#include "LocalPlayerState.h"

// -----------------------------------------------------------------------------
// CoSyncNet
//
// F4MP-aligned responsibilities:
//   - Session lifecycle
//   - Network send / receive
//   - Authority routing (host vs client)
//   - Serialization + enqueue ONLY
//
// RULES:
//   - NEVER spawns actors
//   - NEVER moves actors
//   - NEVER touches the world
//
// All world interaction lives in:
//   CoSyncPlayerManager + CoSyncSpawnTasks
// -----------------------------------------------------------------------------
class CoSyncNet
{
public:
    // -------------------------------------------------------------------------
    // RemotePeer (diagnostic / bookkeeping only)
    // NOT authoritative for world state
    // -------------------------------------------------------------------------
    struct RemotePeer
    {
        uint64_t steamID = 0;
        std::string username;
        LocalPlayerState lastState{};
        double lastUpdateTime = 0.0;
    };

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------
    static void Init(bool host);
    static void Shutdown();

    // Deferred init until world is ready
    static void ScheduleInit(bool host);
    static void PerformPendingInit();

    // Per-frame tick (game thread only)
    static void Tick(double now);

    // -------------------------------------------------------------------------
    // Sending
    // -------------------------------------------------------------------------

    // Legacy full-state packet (optional / debug)
    static void SendLocalPlayerState(const LocalPlayerState& state);

    // CLIENT → HOST
    // Send transform updates ONLY
    // CREATE is host-authoritative and never sent by clients
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
    static void OnReceive(const std::string& msg);

    // -------------------------------------------------------------------------
    // Identity
    // -------------------------------------------------------------------------
    static uint64_t GetMySteamID();
    static const char* GetMyName();
    static void SetMyName(const std::string& name);

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------
    static bool IsInitialized();
    static bool IsSessionActive();
    static bool IsConnected();

    // Optional diagnostics (NOT authoritative)
    static const std::unordered_map<uint64_t, RemotePeer>& GetPeers();

    // -------------------------------------------------------------------------
    // Transport callback
    // -------------------------------------------------------------------------
    static void OnGNSConnected();
};
