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

    bool StartHost();
    bool StartClient(const std::string& connectString);

    void Tick();

    bool IsConnected() const { return m_connected; }
    bool IsHost() const { return m_role == GNSRole::Host; }
    bool IsClient() const { return m_role == GNSRole::Client; }

    std::string GetHostConnectString() const;

    /// ✔ You MUST HAVE THIS
    void SendText(const std::string& text);

private:
    GNS_Session() = default;

    GNSRole m_role = GNSRole::None;
    bool    m_connected = false;

    HSteamListenSocket   m_listenSocket = k_HSteamListenSocket_Invalid;
    HSteamNetConnection  m_connection = k_HSteamNetConnection_Invalid;
};
