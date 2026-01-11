// GNS_Session.cpp
#include "GNS_Session.h"

#include "ConsoleLogger.h"
#include "CoSyncTransport.h"
#include "CoSyncNet.h"

#include "steam/steamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"

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

void GNS_Session::ResetSession()
{
    auto* sock = gSockets();
    if (!sock)
        return;

    // Close host listen socket
    if (m_listenSocket != k_HSteamListenSocket_Invalid)
    {
        sock->CloseListenSocket(m_listenSocket);
        m_listenSocket = k_HSteamListenSocket_Invalid;
    }

    // Close client connection
    if (m_serverConn != k_HSteamNetConnection_Invalid)
    {
        sock->CloseConnection(m_serverConn, 0, "reset", false);
        m_serverConn = k_HSteamNetConnection_Invalid;
    }

    // Close host connections (pending + connected)
    for (auto conn : m_pendingClientConns)
        sock->CloseConnection(conn, 0, "reset", false);
    m_pendingClientConns.clear();

    for (auto conn : m_clientConns)
        sock->CloseConnection(conn, 0, "reset", false);
    m_clientConns.clear();

    m_role = GNSRole::None;
    m_connected = false;
}

void GNS_Session::CloseConnection(HSteamNetConnection conn, const char* reason)
{
    auto* sock = gSockets();
    if (!sock || conn == k_HSteamNetConnection_Invalid)
        return;

    sock->CloseConnection(conn, 0, reason ? reason : "closed", false);

    if (m_role == GNSRole::Client)
    {
        if (conn == m_serverConn)
            m_serverConn = k_HSteamNetConnection_Invalid;

        m_connected = false;
    }
    else if (m_role == GNSRole::Host)
    {
        Host_RemoveAny(conn);

        // Host is "connected" if it still has any fully connected clients
        m_connected = !m_clientConns.empty();
    }
}

bool GNS_Session::StartHost(const char* ip)
{
    ResetSession();

    m_role = GNSRole::Host;
    m_connected = false;

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
    ResetSession();

    m_role = GNSRole::Client;
    m_connected = false;

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

    m_serverConn = sock->ConnectByIPAddress(addr, 2, opts);
    if (m_serverConn == k_HSteamNetConnection_Invalid)
    {
        LOG_ERROR("[GNS] Connect FAILED");
        return false;
    }

    if (m_pollGroup != k_HSteamNetPollGroup_Invalid)
        sock->SetConnectionPollGroup(m_serverConn, m_pollGroup);

    return true;
}

// -----------------------------
// Host connection management
// -----------------------------
void GNS_Session::Host_TrackPending(HSteamNetConnection conn)
{
    if (conn == k_HSteamNetConnection_Invalid)
        return;

    // Pending means: accepted/being established, but not yet in Connected state
    m_pendingClientConns.insert(conn);

    LOG_INFO("[GNS] Host pending clients=%zu connected clients=%zu",
        m_pendingClientConns.size(), m_clientConns.size());
}

void GNS_Session::Host_PromoteToConnected(HSteamNetConnection conn)
{
    if (conn == k_HSteamNetConnection_Invalid)
        return;

    // Remove from pending if present
    auto it = m_pendingClientConns.find(conn);
    if (it != m_pendingClientConns.end())
        m_pendingClientConns.erase(it);

    // Add to connected set
    m_clientConns.insert(conn);

    LOG_INFO("[GNS] Host CONNECTED clients=%zu (pending=%zu)",
        m_clientConns.size(), m_pendingClientConns.size());
}

void GNS_Session::Host_RemoveAny(HSteamNetConnection conn)
{
    if (conn == k_HSteamNetConnection_Invalid)
        return;

    auto itP = m_pendingClientConns.find(conn);
    if (itP != m_pendingClientConns.end())
        m_pendingClientConns.erase(itP);

    auto itC = m_clientConns.find(conn);
    if (itC != m_clientConns.end())
        m_clientConns.erase(itC);

    LOG_INFO("[GNS] Host tracking clients=%zu (pending=%zu)",
        m_clientConns.size(), m_pendingClientConns.size());
}

// -----------------------------
// Receive processing
// -----------------------------
void GNS_Session::ProcessMessagesOnConnection(HSteamNetConnection conn)
{
    auto* sock = gSockets();
    if (!sock)
        return;

    ISteamNetworkingMessage* msgs[32];
    int count = sock->ReceiveMessagesOnConnection(conn, msgs, 32);
    if (count <= 0)
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

void GNS_Session::ProcessMessagesOnPollGroup()
{
    auto* sock = gSockets();
    if (!sock || m_pollGroup == k_HSteamNetPollGroup_Invalid)
        return;

    ISteamNetworkingMessage* msgs[32];
    int count = sock->ReceiveMessagesOnPollGroup(m_pollGroup, msgs, 32);
    if (count <= 0)
        return;

    for (int i = 0; i < count; i++)
    {
        std::string text(
            reinterpret_cast<char*>(msgs[i]->m_pData),
            static_cast<size_t>(msgs[i]->m_cbSize)
        );

        msgs[i]->Release();

        CoSyncTransport::ForwardMessage(text);
    }
}

// -----------------------------
// Connection status callback
// -----------------------------
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

            // Track as pending ONLY (do not set session "connected" yet)
            Host_TrackPending(info->m_hConn);
        }
        break;

    case k_ESteamNetworkingConnectionState_Connected:
        if (m_role == GNSRole::Client)
        {
            if (info->m_hConn == m_serverConn && !m_connected)
            {
                m_connected = true;
                LOG_INFO("[GNS] Client connected!");
                CoSyncNet::OnGNSConnected();
            }
        }
        else if (m_role == GNSRole::Host)
        {
            // Promote this conn to the connected set
            Host_PromoteToConnected(info->m_hConn);

            // Host session becomes "connected" when first client reaches Connected state
            if (!m_connected)
            {
                m_connected = true;
                LOG_INFO("[GNS] Host has at least one CONNECTED client!");
                CoSyncNet::OnGNSConnected();
            }
        }
        break;

    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        LOG_WARN("[GNS] Connection closed (conn=%u, state=%d)", info->m_hConn, (int)info->m_info.m_eState);
        CloseConnection(info->m_hConn, "closed");
        break;

    default:
        break;
    }
}

void GNS_Session::Tick()
{
    auto* sock = gSockets();
    if (!sock)
        return;

    sock->RunCallbacks();

    if (!m_connected)
        return;

    // Primary path
    ProcessMessagesOnPollGroup();

    // Fallbacks / redundancy
    if (m_role == GNSRole::Client)
    {
        if (m_serverConn != k_HSteamNetConnection_Invalid)
            ProcessMessagesOnConnection(m_serverConn);
    }
    else if (m_role == GNSRole::Host)
    {
        for (auto conn : m_clientConns)
            ProcessMessagesOnConnection(conn);
    }
}


void GNS_Session::SendText(const std::string& text)
{
    auto* sock = gSockets();
    if (!sock || !m_connected)
        return;

    const void* data = text.data();
    const uint32 cb = static_cast<uint32>(text.size());

    if (m_role == GNSRole::Client)
    {
        if (m_serverConn == k_HSteamNetConnection_Invalid)
            return;

        sock->SendMessageToConnection(
            m_serverConn,
            data,
            cb,
            k_nSteamNetworkingSend_Reliable,
            nullptr
        );
        return;
    }

    // Host: broadcast to all fully connected clients only
    for (auto conn : m_clientConns)
    {
        sock->SendMessageToConnection(
            conn,
            data,
            cb,
            k_nSteamNetworkingSend_Reliable,
            nullptr
        );
    }
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
