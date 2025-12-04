#include "GNS_Session.h"
#include "ConsoleLogger.h"
#include "CoSyncNet.h"

#include <string>
#include <cstring>

#include "steam/steamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"

static ISteamNetworkingSockets* gSockets()
{
    return SteamNetworkingSockets();
}

// ------------------------------------------------------------
// Singleton
// ------------------------------------------------------------
GNS_Session& GNS_Session::Get()
{
    static GNS_Session inst;
    return inst;
}

// ------------------------------------------------------------
// HOST START
// ------------------------------------------------------------
bool GNS_Session::StartHost()
{
    m_role = GNSRole::Host;
    m_connected = false;

    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.SetIPv4(0, 27020);  // Listen on all interfaces

    SteamNetworkingConfigValue_t opt{};
    opt.SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, 5000);

    m_listenSocket = gSockets()->CreateListenSocketIP(addr, 1, &opt);

    if (m_listenSocket == k_HSteamListenSocket_Invalid)
    {
        LOG_ERROR("[GNS] Host FAILED: could not create listen socket");
        return false;
    }

    LOG_INFO("[GNS] Host listening on 0.0.0.0:27020");
    return true;
}

// ------------------------------------------------------------
// CLIENT START
// ------------------------------------------------------------
bool GNS_Session::StartClient(const std::string& connectString)
{
    m_role = GNSRole::Client;
    m_connected = false;

    size_t colon = connectString.find(':');
    if (colon == std::string::npos)
    {
        LOG_ERROR("[GNS] Invalid connect string: %s", connectString.c_str());
        return false;
    }

    std::string ip = connectString.substr(0, colon);
    int port = std::stoi(connectString.substr(colon + 1));

    SteamNetworkingIPAddr addr;
    addr.Clear();
    if (!addr.ParseString(ip.c_str()))
    {
        LOG_ERROR("[GNS] Invalid IP in connect string: %s", ip.c_str());
        return false;
    }
    addr.m_port = (uint16)port;

    SteamNetworkingConfigValue_t opt{};
    opt.SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, 5000);

    m_connection = gSockets()->ConnectByIPAddress(addr, 1, &opt);

    if (m_connection == k_HSteamNetConnection_Invalid)
    {
        LOG_ERROR("[GNS] Connect FAILED: %s", connectString.c_str());
        return false;
    }

    LOG_INFO("[GNS] Connecting to host at %s", connectString.c_str());
    return true;
}


// ------------------------------------------------------------
// TICK — process messages + connection state
// ------------------------------------------------------------
void GNS_Session::Tick()
{
    ISteamNetworkingSockets* sock = gSockets();

    if (m_connection != k_HSteamNetConnection_Invalid)
    {
        // ---- Receive all messages ----
        SteamNetworkingMessage_t* msgs[16];
        int count = sock->ReceiveMessagesOnConnection(m_connection, msgs, 16);

        for (int i = 0; i < count; i++)
        {
            std::string text((char*)msgs[i]->m_pData, msgs[i]->m_cbSize);
            msgs[i]->Release();

            LOG_INFO("[GNS] Received msg: %s", text.c_str());

            // Forward to CoSyncNet for PlayerState + gameplay
            CoSyncNet::OnReceive(text);
        }

        // ---- Check connection status ----
        SteamNetConnectionRealTimeStatus_t stat{};
        sock->GetConnectionRealTimeStatus(m_connection, &stat, 0, nullptr);

        if (stat.m_eState == k_ESteamNetworkingConnectionState_Connected)
        {
            if (!m_connected)
            {
                m_connected = true;
                LOG_INFO("[GNS] Connection established!");
            }
        }
    }
}

// ------------------------------------------------------------
// HOST: Get connect string for overlay
// ------------------------------------------------------------
std::string GNS_Session::GetHostConnectString() const
{
    if (m_listenSocket == k_HSteamListenSocket_Invalid)
        return "";

    SteamNetworkingIPAddr addr;
    if (!gSockets()->GetListenSocketAddress(m_listenSocket, &addr))
        return "";

    char buff[64];
    addr.ToString(buff, sizeof(buff), false);
    return std::string(buff);
}

// ------------------------------------------------------------
// Send text message to peer
// ------------------------------------------------------------
void GNS_Session::SendText(const std::string& text)
{
    if (m_connection == k_HSteamNetConnection_Invalid)
        return;

    gSockets()->SendMessageToConnection(
        m_connection,
        text.c_str(),
        text.size(),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
}
