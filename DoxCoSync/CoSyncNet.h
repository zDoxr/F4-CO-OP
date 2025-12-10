#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>

#include "LocalPlayerState.h"

// High-level CoSync networking layer that sits on top of
// GNS_Session + CoSyncTransport and feeds CoSyncPlayerManager.
namespace CoSyncNet
{
    // A remote peer on the LAN/Hamachi network.
    // NOTE: steamID is kept for backwards compatibility but is
    //       now effectively a generic LAN/Hamachi numeric ID.
    struct RemotePeer
    {
        uint64_t         steamID = 0;    // can be hash(username) or other ID
        std::string      username;
        LocalPlayerState lastState;
        double           lastUpdateTime = 0.0;
    };

    // ------------------------------------------------------
    // Initialization / shutdown
    // ------------------------------------------------------

    // Init immediately (normally called after world loaded).
    // 'host' = true → host; false → client.
    void Init(bool host);

    // Shut down networking runtime and clear peer list.
    void Shutdown();

    // Schedules Init(host) to run once the world is loaded.
    // TickHook will call PerformPendingInit() to complete it.
    void ScheduleInit(bool host);

    // Called from TickHook after WORLD LOADED to perform
    // the pending Init, if any.
    void PerformPendingInit();

    // Per-frame network tick (CoSyncNet-level; not sockets only).
    void Tick(double now);

    // ------------------------------------------------------
    // Outgoing player-state replication
    // ------------------------------------------------------
    void SendLocalPlayerState(const LocalPlayerState& state);

    // ------------------------------------------------------
    // Incoming network packet callback (called by transport)
    // ------------------------------------------------------
    void OnReceive(const std::string& msg);

    // ------------------------------------------------------
    // Identity (LAN/Hamachi)
    // ------------------------------------------------------

    // Returns a numeric ID for this client (hash of username or 0 if unset).
    uint64_t    GetMySteamID();

    // Returns our username (for packet field 8).
    const char* GetMyName();

    // CoSyncNet.h
    void SetMyName(const std::string& name);


    bool IsInitialized();

    // Overall session flag (host or client active)
    bool IsSessionActive();

    // ------------------------------------------------------
    // Peer list (for overlay/debug)
    // ------------------------------------------------------
    const std::unordered_map<uint64_t, RemotePeer>& GetPeers();

    // Called from GNS_Session when socket connection is established.
    void OnGNSConnected();

    bool IsConnected();
}
