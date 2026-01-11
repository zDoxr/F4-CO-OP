// GNS_Session.h
#pragma once

#include <string>
#include <unordered_set>
#include <unordered_map>
#include <cstdint>

#include "steam/steamnetworkingsockets.h"

enum class GNSRole
{
    None,
    Host,
    Client
};

class GNS_Session
{
public:
    static GNS_Session& Get();

    // Startup
    bool StartHost(const char* ip);                   // Host using Hamachi/LAN
    bool StartClient(const std::string& connectStr);  // Client connecting to host

    // Called from global SteamNetworking callback
    void HandleConnectionStatusChanged(const SteamNetConnectionStatusChangedCallback_t* info);

    // Per-frame pump
    void Tick();

    // Send text packet (CoSyncTransport uses this)
    // Host: broadcasts to all connected clients
    // Client: sends to host connection
    void SendText(const std::string& text);

    // Host overlay connection string (“25.x.x.x:48000”)
    std::string GetHostConnectString() const;

    // Optional: basic status helpers
    GNSRole GetRole() const { return m_role; }
    bool IsConnected() const { return m_connected; }

private:
    GNS_Session();

    void ResetSession();
    void CloseConnection(HSteamNetConnection conn, const char* reason);

    // Receive helpers
    void ProcessMessagesOnPollGroup();
    void ProcessMessagesOnConnection(HSteamNetConnection conn);

    // Host connection management
    void Host_TrackPending(HSteamNetConnection conn);
    void Host_PromoteToConnected(HSteamNetConnection conn);
    void Host_RemoveAny(HSteamNetConnection conn);

private:
    GNSRole m_role = GNSRole::None;

    // True when:
    // - Client: connected to host
    // - Host: at least one client is in Connected state
    bool    m_connected = false;

    HSteamListenSocket  m_listenSocket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup  m_pollGroup = k_HSteamNetPollGroup_Invalid;

    // Client mode: single connection to host
    HSteamNetConnection m_serverConn = k_HSteamNetConnection_Invalid;

    // Host mode:
    // - pending: accepted but not yet Connected
    // - connected: fully established clients we can treat as "in-session"
    std::unordered_set<HSteamNetConnection> m_pendingClientConns;
    std::unordered_set<HSteamNetConnection> m_clientConns;
    // Connection -> peer SteamID (tracked on HELLO)
    std::unordered_map<HSteamNetConnection, uint64_t> m_peerSteamIDs;
};
