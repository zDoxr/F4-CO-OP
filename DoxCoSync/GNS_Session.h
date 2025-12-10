#pragma once

#include <string>
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
    bool StartClient(const std::string& connectStr); // Client connecting to host

    // Called from global SteamNetworking callback
    void HandleConnectionStatusChanged(const SteamNetConnectionStatusChangedCallback_t* info);

    // Per-frame pump
    void Tick();

    // Send text packet (CoSyncTransport uses this)
    void SendText(const std::string& text);

    // Host overlay connection string (“25.x.x.x:48000”)
    std::string GetHostConnectString() const;

private:
    GNS_Session();

    void ProcessConnectionMessages(HSteamNetConnection conn);

    GNSRole m_role;
    bool    m_connected;

    HSteamListenSocket   m_listenSocket;
    HSteamNetPollGroup   m_pollGroup;
    HSteamNetConnection  m_connection;
};
