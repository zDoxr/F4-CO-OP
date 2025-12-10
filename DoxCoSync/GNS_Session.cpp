#include "GNS_Session.h"
#include "ConsoleLogger.h"
#include "CoSyncTransport.h"
#include "CoSyncNet.h"
#include "steam/steamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"
#include "TickHook.h"

static void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);
static bool g_callbacksRegistered = false;

// Shortcut to sockets instance
static ISteamNetworkingSockets* gSockets()
{
    return SteamNetworkingSockets();
}

// Register global connection-status callback
static void EnsureCallbacksRegistered()
{
    if (g_callbacksRegistered)
        return;

    auto* utils = SteamNetworkingUtils();
    if (!utils)
    {
        LOG_ERROR("[GNS] Failed to get networking utils");
        return;
    }

    utils->SetGlobalCallback_SteamNetConnectionStatusChanged(OnSteamNetConnectionStatusChanged);
    g_callbacksRegistered = true;

    LOG_INFO("[GNS] Connection-status callback registered");
}

GNS_Session& GNS_Session::Get()
{
    static GNS_Session inst;
    return inst;
}

GNS_Session::GNS_Session()
    : m_role(GNSRole::None)
    , m_connected(false)
    , m_listenSocket(k_HSteamListenSocket_Invalid)
    , m_pollGroup(k_HSteamNetPollGroup_Invalid)
    , m_connection(k_HSteamNetConnection_Invalid)
{
    auto* sock = gSockets();
    if (sock)
    {
        m_pollGroup = sock->CreatePollGroup();
        if (m_pollGroup == k_HSteamNetPollGroup_Invalid)
            LOG_ERROR("[GNS] Failed to create poll group");
        else
            LOG_INFO("[GNS] Poll group created");
    }
}

bool GNS_Session::StartHost(const char* ip)
{
    m_role = GNSRole::Host;
    m_connected = false;
    m_connection = k_HSteamNetConnection_Invalid;

    EnsureCallbacksRegistered();

    auto* sock = gSockets();
    if (!sock)
    {
        LOG_ERROR("[GNS] StartHost FAILED: sockets null");
        return false;
    }

    SteamNetworkingIPAddr addr;
    addr.Clear();

    if (!addr.ParseString(ip))
    {
        LOG_ERROR("[GNS] Invalid host IP: %s", ip);
        return false;
    }

    addr.m_port = 48000;

    SteamNetworkingConfigValue_t opts[2]{};
    opts[0].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, 15000);
    opts[1].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, 300000);

    LOG_INFO("[GNS] Attempting bind on %s:48000...", ip);

    m_listenSocket = sock->CreateListenSocketIP(addr, 2, opts);
    if (m_listenSocket == k_HSteamListenSocket_Invalid)
    {
        LOG_ERROR("[GNS] Host FAILED: CreateListenSocketIP");
        return false;
    }

    LOG_INFO("[GNS] Host socket created successfully");
    return true;
}

bool GNS_Session::StartClient(const std::string& connectString)
{
    m_role = GNSRole::Client;
    m_connected = false;
    m_connection = k_HSteamNetConnection_Invalid;

    EnsureCallbacksRegistered();

    auto* sock = gSockets();
    if (!sock)
    {
        LOG_ERROR("[GNS] StartClient FAILED: sockets null");
        return false;
    }

    // Parse "ip:port"
    const size_t colon = connectString.find(':');
    if (colon == std::string::npos)
    {
        LOG_ERROR("[GNS] Invalid connect string: %s", connectString.c_str());
        return false;
    }

    const std::string ip = connectString.substr(0, colon);
    const int port = std::stoi(connectString.substr(colon + 1));

    SteamNetworkingIPAddr addr;
    addr.Clear();

    if (!addr.ParseString(ip.c_str()))
    {
        LOG_ERROR("[GNS] Invalid IP: %s", ip.c_str());
        return false;
    }

    addr.m_port = static_cast<uint16>(port);

    SteamNetworkingConfigValue_t opts[2]{};
    opts[0].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, 15000);
    opts[1].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, 300000);

    LOG_INFO("[GNS] Connecting to %s", connectString.c_str());

    m_connection = sock->ConnectByIPAddress(addr, 2, opts);
    if (m_connection == k_HSteamNetConnection_Invalid)
    {
        LOG_ERROR("[GNS] Connect FAILED");
        return false;
    }

    if (m_pollGroup != k_HSteamNetPollGroup_Invalid)
        sock->SetConnectionPollGroup(m_connection, m_pollGroup);

    return true;
}

void GNS_Session::ProcessConnectionMessages(HSteamNetConnection conn)
{
    auto* sock = gSockets();
    if (!sock)
        return;

    ISteamNetworkingMessage* msgs[32];
    int count = sock->ReceiveMessagesOnConnection(conn, msgs, 32);

    if (count < 0)
        return;

    for (int i = 0; i < count; i++)
    {
        std::string text(
            reinterpret_cast<char*>(msgs[i]->m_pData),
            static_cast<size_t>(msgs[i]->m_cbSize)
        );

        msgs[i]->Release();

        // Forward to CoSyncTransport → CoSyncNet::OnReceive
        CoSyncTransport::ForwardMessage(text);
    }
}

void GNS_Session::HandleConnectionStatusChanged(const SteamNetConnectionStatusChangedCallback_t* info)
{
    auto* sock = gSockets();
    if (!sock)
        return;

    switch (info->m_info.m_eState)
    {
    case k_ESteamNetworkingConnectionState_Connecting:
        if (m_role == GNSRole::Host && info->m_info.m_hListenSocket == m_listenSocket)
        {
            LOG_INFO("[GNS] Incoming connection %u — accepting", info->m_hConn);

            if (sock->AcceptConnection(info->m_hConn) != k_EResultOK)
            {
                LOG_ERROR("[GNS] AcceptConnection FAILED for %u", info->m_hConn);
                sock->CloseConnection(info->m_hConn, 0, nullptr, false);
                return;
            }

            if (m_pollGroup != k_HSteamNetPollGroup_Invalid)
                sock->SetConnectionPollGroup(info->m_hConn, m_pollGroup);

            m_connection = info->m_hConn;
        }
        break;

    case k_ESteamNetworkingConnectionState_Connected:
        if (info->m_hConn == m_connection && !m_connected)
        {
            
            m_connected = true;
            LOG_INFO("[GNS] Connection established!");

            CoSyncNet::OnGNSConnected();
        }
        break;

    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        if (info->m_hConn == m_connection)
        {
            LOG_WARN("[GNS] Connection closed (conn=%u)", info->m_hConn);
            sock->CloseConnection(info->m_hConn, 0, nullptr, false);
            m_connection = k_HSteamNetConnection_Invalid;
            m_connected = false;
        }
        break;
    }
}

void GNS_Session::Tick()
{
    auto* sock = gSockets();
    if (!sock)
        return;
	

    sock->RunCallbacks();
	

    if (m_connection != k_HSteamNetConnection_Invalid && m_connected)
        ProcessConnectionMessages(m_connection);
	
}

void GNS_Session::SendText(const std::string& text)
{
    if (!m_connected || m_connection == k_HSteamNetConnection_Invalid)
        return;

    gSockets()->SendMessageToConnection(
        m_connection,
        text.c_str(),
        static_cast<uint32>(text.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
}

std::string GNS_Session::GetHostConnectString() const
{
    if (m_listenSocket == k_HSteamListenSocket_Invalid)
        return "";

    auto* sock = gSockets();
    if (!sock)
        return "";

    SteamNetworkingIPAddr addr;
    if (!sock->GetListenSocketAddress(m_listenSocket, &addr))
        return "";

    char buffer[64];
    addr.ToString(buffer, sizeof(buffer), false);
    return buffer;
}

static void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info)
{
    GNS_Session::Get().HandleConnectionStatusChanged(info);
}
