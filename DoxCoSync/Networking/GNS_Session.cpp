
#include "GNS_Session.h"

#include "ConsoleLogger.h"
#include "CoSyncTransport.h"
#include "CoSyncNet.h"
#include "CoSyncMessageHelpers.h"
#include "EntitySerialization.h"
#include "CoSyncSessionConfig.h"
#include "CoSyncSteamAPI.h"

#include "steam/steamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"

#include <functional>
#include <cstring>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <sstream>
#include <chrono>

static void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);
static bool g_callbacksRegistered = false;

namespace
{
    uint64_t HashName(const std::string& name)
    {
        return static_cast<uint64_t>(std::hash<std::string>{}(name));
    }

    bool ParseHelloSteamID(const std::string& msg, uint64_t& sid)
    {
        sid = 0;

        if (!IsHello(msg))
            return false;

        std::stringstream ss(msg.substr(6));
        std::string name;
        if (!std::getline(ss, name, '|') || name.empty())
            return false;

        std::string tok;
        if (std::getline(ss, tok, '|') && !tok.empty())
        {
            try { sid = std::stoull(tok); }
            catch (...) { sid = 0; }
        }

        if (sid == 0)
            sid = HashName(name);

        return sid != 0;
    }

    uint32_t EntityIDFromSteamID(uint64_t sid)
    {
        return static_cast<uint32_t>(sid & 0xFFFFFFFFu);
    }
}

// Which library the session runs on: the bundled open-source GNS (LAN /
// direct IP) or Steam's own ISteamNetworkingSockets (P2P + SDR relay). The two
// share the vendored C++ interface -- every vtable slot proven identical
// (see CoSyncSteamAPI.h).
static std::atomic<bool> g_steamBackend{ false };

static ISteamNetworkingSockets* gSockets()
{
    return g_steamBackend.load() ? CoSyncSteamAPI::Sockets() : SteamNetworkingSockets();
}

static ISteamNetworkingUtils* gUtils()
{
    return g_steamBackend.load() ? CoSyncSteamAPI::Utils() : SteamNetworkingUtils();
}

// Steam sessions: virtual port every CoSync host listens on.
static constexpr int kSteamVirtualPort = 0;


static bool g_steamCallbacksRegistered = false;

static void EnsureCallbacksRegistered()
{
    if (g_steamBackend.load())
    {
        if (g_steamCallbacksRegistered)
            return;
        // Valve's DLL: function-pointer path, dispatched by the network
        // thread's sock->RunCallbacks() exactly like the open-source library.
        // gbe_fork: only Steam's callback system (dispatched when the game
        // runs SteamAPI_RunCallbacks). Both are registered; the status handler
        // drops repeats of the same (connection, state).
        auto* utils = gUtils();
        if (utils)
            utils->SetGlobalCallback_SteamNetConnectionStatusChanged(OnSteamNetConnectionStatusChanged);
        const bool cb = CoSyncSteamAPI::RegisterConnectionStatus(OnSteamNetConnectionStatusChanged);
        g_steamCallbacksRegistered = true;
        LOG_INFO("[GNS] Steam connection-status callbacks registered (fn-ptr=%d callback=%d)",
            utils ? 1 : 0, cb ? 1 : 0);
        return;
    }

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

GNS_Session::~GNS_Session()
{
    StopNetworkThread();
}

void GNS_Session::StartNetworkThread()
{
    if (m_networkThreadRunning.load())
        return;

    m_networkThreadRunning.store(true);
    m_networkThread = std::thread(&GNS_Session::NetworkThreadProc, this);
    LOG_INFO("[GNS] Background network thread started");
}

void GNS_Session::StopNetworkThread()
{
    if (!m_networkThreadRunning.load())
        return;

    m_networkThreadRunning.store(false);
    if (m_networkThread.joinable())
        m_networkThread.join();

    LOG_INFO("[GNS] Background network thread stopped");
}

void GNS_Session::NetworkThreadProc()
{
    while (m_networkThreadRunning.load(std::memory_order_relaxed))
    {
        auto* sock = gSockets();
        if (sock)
        {

            sock->RunCallbacks();

            ProcessMessagesOnPollGroup();

            std::vector<HSteamNetConnection> connsSnapshot;
            {
                std::lock_guard<std::mutex> lk(m_connsMutex);
                if (m_role == GNSRole::Client &&
                    m_serverConn != k_HSteamNetConnection_Invalid)
                {
                    connsSnapshot.push_back(m_serverConn);
                }
                else if (m_role == GNSRole::Host)
                {
                    connsSnapshot.insert(connsSnapshot.end(),
                        m_clientConns.begin(), m_clientConns.end());
                }
            }
            for (auto conn : connsSnapshot)
                ProcessMessagesOnConnection(conn);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

void GNS_Session::ResetSession()
{

    StopNetworkThread();

    auto* sock = gSockets();
    if (!sock)
        return;


    if (m_listenSocket != k_HSteamListenSocket_Invalid)
    {
        sock->CloseListenSocket(m_listenSocket);
        m_listenSocket = k_HSteamListenSocket_Invalid;
    }

    {
        std::lock_guard<std::mutex> lk(m_connsMutex);


        if (m_serverConn != k_HSteamNetConnection_Invalid)
        {
            sock->CloseConnection(m_serverConn, 0, "reset", false);
            m_serverConn = k_HSteamNetConnection_Invalid;
        }


        for (auto conn : m_pendingClientConns)
            sock->CloseConnection(conn, 0, "reset", false);
        m_pendingClientConns.clear();

        for (auto conn : m_clientConns)
            sock->CloseConnection(conn, 0, "reset", false);
        m_clientConns.clear();

        m_peerSteamIDs.clear();
    }

    // CoSync_Main::Shutdown() calls GameNetworkingSockets_Kill(); a poll group
    // created before that is invalid after the next GNS_Init(). Recreate it
    // for every session so "stop hosting, host again" works.
    if (m_pollGroup != k_HSteamNetPollGroup_Invalid)
        sock->DestroyPollGroup(m_pollGroup);
    m_pollGroup = sock->CreatePollGroup();
    if (m_pollGroup == k_HSteamNetPollGroup_Invalid)
        LOG_ERROR("[GNS] Failed to (re)create poll group");

    m_role = GNSRole::None;
    m_connected.store(false, std::memory_order_relaxed);
    m_boundPort.store(0);
    m_clientState.store(GNSClientState::Idle);
    m_clientEndReason.store(0);
    m_clientWasConnected.store(false);
}

void GNS_Session::CloseConnection(HSteamNetConnection conn, const char* reason)
{
    auto* sock = gSockets();
    if (!sock || conn == k_HSteamNetConnection_Invalid)
        return;

    sock->CloseConnection(conn, 0, reason ? reason : "closed", false);

    if (m_role == GNSRole::Client)
    {
        std::lock_guard<std::mutex> lk(m_connsMutex);
        if (conn == m_serverConn)
            m_serverConn = k_HSteamNetConnection_Invalid;
        m_connected.store(false, std::memory_order_relaxed);
    }
    else if (m_role == GNSRole::Host)
    {
        Host_RemoveAny(conn);


        std::lock_guard<std::mutex> lk(m_connsMutex);
        m_connected.store(!m_clientConns.empty(), std::memory_order_relaxed);
    }
}

bool GNS_Session::StartHost(const char* ip)
{
    ResetSession();

    const bool steam = ip && std::strcmp(ip, "steam") == 0;
    if (!SelectBackend(steam))
        return false;

    m_role = GNSRole::Host;
    m_connected = false;

    EnsureCallbacksRegistered();

    auto* sock = gSockets();
    if (!sock)
    {
        LOG_ERROR("[GNS] StartHost FAILED: sockets null");
        return false;
    }

    if (steam)
    {
        SteamNetworkingConfigValue_t sopts[2]{};
        sopts[0].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, 30000);   // 0.5.9: relay routes can take longer the first time
        sopts[1].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, 10000);
        m_listenSocket = sock->CreateListenSocketP2P(kSteamVirtualPort, 2, sopts);
        if (m_listenSocket == k_HSteamListenSocket_Invalid)
        {
            LOG_ERROR("[GNS] Host FAILED: CreateListenSocketP2P (Steam)");
            return false;
        }
        LOG_INFO("[GNS] Steam host listening (P2P virtual port %d, SteamID %llu)",
            kSteamVirtualPort, (unsigned long long)CoSyncSteamAPI::MySteamID());
        StartNetworkThread();
        return true;
    }

    // Accept "", "ip", ":port" or "ip:port" -- port defaults to kGamePortBase.
    std::string ipStr = ip ? ip : "";
    uint16       basePort = CoSyncSessionConfig::kGamePortBase;
    {
        const size_t colon = ipStr.find(':');
        if (colon != std::string::npos)
        {
            try { basePort = static_cast<uint16>(std::stoi(ipStr.substr(colon + 1))); }
            catch (...) {}
            ipStr = ipStr.substr(0, colon);
        }
    }

    // Empty / 0.0.0.0 = all interfaces. A cleared address is GNS's "any"
    // (like INADDR_ANY), so Ethernet, Wi-Fi and any other LAN adapter all
    // accept connections -- no adapter has to be chosen (this replaces the
    // old Hamachi-IP bind).
    SteamNetworkingIPAddr addr;
    addr.Clear();
    const bool anyInterface = ipStr.empty() || ipStr == "0.0.0.0";
    if (!anyInterface && !addr.ParseString(ipStr.c_str()))
    {
        LOG_ERROR("[GNS] Invalid host IP: %s", ipStr.c_str());
        return false;
    }

    SteamNetworkingConfigValue_t opts[2]{};
    opts[0].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, 15000);
    // P6 (2026-09-26): was 300000 (5 min, no recorded reason, from the first
    // GNS integration). A crashed peer's proxy then stayed frozen and its slot
    // counted toward the 8-player cap for five minutes. Loading screens don't
    // need it: GNS services keepalives on its own thread, and the 26 Sep logs
    // show messages arriving throughout 8-32 s game-thread stalls. 10000 is
    // GNS's own default (csteamnetworkingsockets.cpp), set explicitly so it
    // doesn't depend on the DLL build.
    opts[1].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, 10000);

    for (int attempt = 0; attempt < CoSyncSessionConfig::kGamePortTries; ++attempt)
    {
        const uint16 port = static_cast<uint16>(basePort + attempt);
        addr.m_port = port;
        LOG_INFO("[GNS] Attempting bind on %s:%u...", anyInterface ? "<all interfaces>" : ipStr.c_str(), (unsigned)port);

        m_listenSocket = sock->CreateListenSocketIP(addr, 2, opts);
        if (m_listenSocket != k_HSteamListenSocket_Invalid)
        {
            m_boundPort.store(port);
            break;
        }
        LOG_WARN("[GNS] Port %u unavailable, trying the next one", (unsigned)port);
    }

    if (m_listenSocket == k_HSteamListenSocket_Invalid)
    {
        LOG_ERROR("[GNS] Host FAILED: CreateListenSocketIP (ports %u-%u)",
            (unsigned)basePort, (unsigned)(basePort + CoSyncSessionConfig::kGamePortTries - 1));
        return false;
    }

    LOG_INFO("[GNS] Host socket created successfully (port %u)", (unsigned)m_boundPort.load());
    StartNetworkThread();
    return true;
}

bool GNS_Session::StartClient(const std::string& connectString)
{
    ResetSession();

    const bool steam = connectString.rfind("steam:", 0) == 0;
    if (!SelectBackend(steam))
        return false;

    m_role = GNSRole::Client;
    m_connected = false;

    EnsureCallbacksRegistered();

    auto* sock = gSockets();
    if (!sock)
    {
        LOG_ERROR("[GNS] StartClient FAILED: sockets null");
        return false;
    }

    if (steam)
    {
        uint64_t hostID = 0;
        try { hostID = std::stoull(connectString.substr(6)); }
        catch (...) { hostID = 0; }
        if (hostID == 0)
        {
            LOG_ERROR("[GNS] Invalid Steam connect string: %s", connectString.c_str());
            return false;
        }
        SteamNetworkingIdentity ident;
        ident.Clear();
        ident.SetSteamID64(hostID);
        SteamNetworkingConfigValue_t sopts[2]{};
        sopts[0].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, 30000);   // 0.5.9: relay routes can take longer the first time
        sopts[1].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, 10000);
        LOG_INFO("[GNS] Connecting over Steam to host %llu", (unsigned long long)hostID);
        m_clientState.store(GNSClientState::Connecting);
        m_serverConn = sock->ConnectP2P(ident, kSteamVirtualPort, 2, sopts);
        if (m_serverConn == k_HSteamNetConnection_Invalid)
        {
            LOG_ERROR("[GNS] Steam ConnectP2P FAILED (host %llu)", (unsigned long long)hostID);
            m_clientState.store(GNSClientState::Closed);
            return false;
        }
        {
            // The host's identity is Steam-verified; bind it now.
            std::lock_guard<std::mutex> lk(m_connsMutex);
            m_peerSteamIDs[m_serverConn] = hostID;
        }
        if (m_pollGroup != k_HSteamNetPollGroup_Invalid)
            sock->SetConnectionPollGroup(m_serverConn, m_pollGroup);
        StartNetworkThread();
        return true;
    }


    const size_t colon = connectString.find(':');
    if (colon == std::string::npos)
    {
        LOG_ERROR("[GNS] Invalid connect string: %s", connectString.c_str());
        return false;
    }

    const std::string ip = connectString.substr(0, colon);

    // UPDATE 2026-06-18 (round 23 audit): was a bare std::stoi with no
    // try/catch -- a malformed/empty port (e.g. a trailing colon with nothing
    // after it) throws std::invalid_argument, an uncaught C++ exception that
    // bypasses SEH entirely and aborts the process (same bug class as the
    // round-13 AnimVerify bad_alloc crash). The other two stoi/stoull calls in
    // this exact file (lines ~42, ~252) already use try/catch; this one didn't.
    int port = 0;
    try
    {
        port = std::stoi(connectString.substr(colon + 1));
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[GNS] Invalid port in connect string '%s': %s", connectString.c_str(), e.what());
        return false;
    }

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
    // P6 (2026-09-26): was 300000 (5 min, no recorded reason, from the first
    // GNS integration). A crashed peer's proxy then stayed frozen and its slot
    // counted toward the 8-player cap for five minutes. Loading screens don't
    // need it: GNS services keepalives on its own thread, and the 26 Sep logs
    // show messages arriving throughout 8-32 s game-thread stalls. 10000 is
    // GNS's own default (csteamnetworkingsockets.cpp), set explicitly so it
    // doesn't depend on the DLL build.
    opts[1].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, 10000);

    LOG_INFO("[GNS] Connecting to %s", connectString.c_str());

    m_clientState.store(GNSClientState::Connecting);
    m_serverConn = sock->ConnectByIPAddress(addr, 2, opts);
    if (m_serverConn == k_HSteamNetConnection_Invalid)
    {
        LOG_ERROR("[GNS] Connect FAILED");
        m_clientState.store(GNSClientState::Closed);
        return false;
    }

    if (m_pollGroup != k_HSteamNetPollGroup_Invalid)
        sock->SetConnectionPollGroup(m_serverConn, m_pollGroup);

    StartNetworkThread();
    return true;
}

void GNS_Session::Host_TrackPending(HSteamNetConnection conn)
{
    if (conn == k_HSteamNetConnection_Invalid)
        return;

    std::lock_guard<std::mutex> lk(m_connsMutex);
    m_pendingClientConns.insert(conn);
    LOG_INFO("[GNS] Host pending clients=%zu connected clients=%zu",
        m_pendingClientConns.size(), m_clientConns.size());
}

void GNS_Session::Host_PromoteToConnected(HSteamNetConnection conn)
{
    if (conn == k_HSteamNetConnection_Invalid)
        return;

    std::lock_guard<std::mutex> lk(m_connsMutex);
    m_pendingClientConns.erase(conn);
    m_clientConns.insert(conn);
    LOG_INFO("[GNS] Host CONNECTED clients=%zu (pending=%zu)",
        m_clientConns.size(), m_pendingClientConns.size());
}

void GNS_Session::Host_RemoveAny(HSteamNetConnection conn)
{
    if (conn == k_HSteamNetConnection_Invalid)
        return;

    std::lock_guard<std::mutex> lk(m_connsMutex);
    m_pendingClientConns.erase(conn);
    m_clientConns.erase(conn);
    LOG_INFO("[GNS] Host tracking clients=%zu (pending=%zu)",
        m_clientConns.size(), m_pendingClientConns.size());
}

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

        uint64_t sid = 0;
        if (ParseHelloSteamID(text, sid))
        {
            // m_peerSteamIDs is read/erased under m_connsMutex elsewhere in this
            // file (HandleConnectionStatusChanged) -- this write must take the
            // same lock, otherwise it's an unsynchronized concurrent mutation
            // of the same std::unordered_map (this function runs on both the
            // background network thread and, redundantly, the main thread via
            // Tick() -- see the Tick()-side fix below for the actual root cause).
            BindPeerSteamID(conn, sid);
        }

        CoSyncTransport::ForwardMessage(text, conn);
    }
}

// P5/N7 (2026-09-26): the identity a connection gives in its first HELLO is
// the one the host checks every entity message against, so a later HELLO must
// not switch it to another player's Steam ID. (Without Steam authentication the
// first claim itself is still unverified -- IP_AllowWithoutAuth=1.)
void GNS_Session::BindPeerSteamID(HSteamNetConnection conn, uint64_t sid)
{
    std::lock_guard<std::mutex> lk(m_connsMutex);
    auto it = m_peerSteamIDs.find(conn);
    if (it == m_peerSteamIDs.end())
    {
        m_peerSteamIDs.emplace(conn, sid);
        return;
    }
    if (it->second != sid)
        LOG_WARN("[GNS] conn=%u sent a HELLO as %llu but identified as %llu -- keeping the first identity",
            conn, (unsigned long long)sid, (unsigned long long)it->second);
}

uint64_t GNS_Session::GetPeerSteamID(HSteamNetConnection conn) const
{
    std::lock_guard<std::mutex> lk(m_connsMutex);
    auto it = m_peerSteamIDs.find(conn);
    return it != m_peerSteamIDs.end() ? it->second : 0;
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
        ISteamNetworkingMessage* m = msgs[i];
        if (!m)
            continue;

        const HSteamNetConnection conn = m->m_conn;

        std::string text(
            reinterpret_cast<const char*>(m->m_pData),
            static_cast<size_t>(m->m_cbSize)
        );

        m->Release();

        uint64_t sid = 0;
        if (ParseHelloSteamID(text, sid) && conn != k_HSteamNetConnection_Invalid)
            BindPeerSteamID(conn, sid);

        CoSyncTransport::ForwardMessage(text, conn);
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
            // Hard player cap (host included). Rejecting BEFORE accept sends
            // the reason code to the client, which shows "SESSION FULL".
            {
                size_t inUse = 0;
                {
                    std::lock_guard<std::mutex> lk(m_connsMutex);
                    inUse = m_pendingClientConns.size() + m_clientConns.size();
                }
                if (inUse + 1 >= static_cast<size_t>(CoSyncSessionConfig::kMaxPlayers))
                {
                    LOG_WARN("[GNS] Rejecting connection %u: session full (%zu clients + host = %d max)",
                        info->m_hConn, inUse, CoSyncSessionConfig::kMaxPlayers);
                    sock->CloseConnection(info->m_hConn, CoSyncSessionConfig::kEndReason_SessionFull,
                        "session_full", false);
                    return;
                }
            }

            LOG_INFO("[GNS] Incoming connection %u — accepting", info->m_hConn);

            // Steam sessions: the peer's SteamID is verified by Steam, so it
            // is the connection's identity from the start (first-wins, see
            // BindPeerSteamID -- its HELLO must then name the same ID).
            if (g_steamBackend.load())
            {
                const uint64_t verified = info->m_info.m_identityRemote.GetSteamID64();
                if (verified == 0)
                {
                    LOG_WARN("[GNS] Rejecting Steam connection %u: no Steam identity", info->m_hConn);
                    sock->CloseConnection(info->m_hConn, 0, "no_identity", false);
                    return;
                }
                BindPeerSteamID(info->m_hConn, verified);
                LOG_INFO("[GNS] Steam connection %u is SteamID %llu", info->m_hConn, (unsigned long long)verified);
            }

            if (sock->AcceptConnection(info->m_hConn) != k_EResultOK)
            {
                LOG_ERROR("[GNS] AcceptConnection FAILED for %u", info->m_hConn);
                sock->CloseConnection(info->m_hConn, 0, nullptr, false);
                return;
            }

            if (m_pollGroup != k_HSteamNetPollGroup_Invalid)
                sock->SetConnectionPollGroup(info->m_hConn, m_pollGroup);


            Host_TrackPending(info->m_hConn);
        }
        break;

    case k_ESteamNetworkingConnectionState_Connected:
        if (m_role == GNSRole::Client)
        {
            bool expected = false;
            if (info->m_hConn == m_serverConn &&
                m_connected.compare_exchange_strong(expected, true,
                    std::memory_order_acq_rel))
            {
                LOG_INFO("[GNS] Client connected!");
                m_clientState.store(GNSClientState::Connected);
                m_clientWasConnected.store(true);

            }
        }
        else if (m_role == GNSRole::Host)
        {
            Host_PromoteToConnected(info->m_hConn);
            bool expected2 = false;
            if (m_connected.compare_exchange_strong(expected2, true,
                    std::memory_order_acq_rel))
            {
                LOG_INFO("[GNS] Host has at least one CONNECTED client!");
            }
        }
        break;

    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
    {
        LOG_WARN("[GNS] Connection closed (conn=%u, state=%d endReason=%d reason='%s')",
            info->m_hConn, (int)info->m_info.m_eState, info->m_info.m_eEndReason,
            info->m_info.m_szEndDebug ? info->m_info.m_szEndDebug : "");

        if (m_role == GNSRole::Client && info->m_hConn == m_serverConn)
        {
            m_clientEndReason.store(info->m_info.m_eEndReason);
            m_clientState.store(GNSClientState::Closed);
        }

        uint64_t peerSteamID = 0;
        bool     identityStillConnected = false;
        {
            std::lock_guard<std::mutex> lk(m_connsMutex);
            auto it = m_peerSteamIDs.find(info->m_hConn);
            if (it != m_peerSteamIDs.end())
                peerSteamID = it->second;
            m_peerSteamIDs.erase(info->m_hConn);
            // N5 (2026-09-26): the same identity can be live on another
            // connection (a player reconnecting before this one timed out).
            // Announcing this one's departure would destroy that live
            // player's proxy on every machine.
            if (peerSteamID != 0)
                for (const auto& kv : m_peerSteamIDs)
                    if (kv.second == peerSteamID) { identityStillConnected = true; break; }
        }

        CloseConnection(info->m_hConn, "closed");

        if (m_role == GNSRole::Host && peerSteamID != 0 && identityStillConnected)
        {
            LOG_WARN("[GNS] conn=%u closed, but its identity %llu is still connected on another connection -- not announcing a departure",
                info->m_hConn, (unsigned long long)peerSteamID);
        }
        else if (m_role == GNSRole::Host && peerSteamID != 0)
        {

            EntityDestroyPacket d{};
            d.entityID = EntityIDFromSteamID(peerSteamID);
            const std::string destroyMsg = SerializeEntityDestroy(d);
            CoSyncTransport::Send(destroyMsg);
            CoSyncTransport::ForwardMessage(destroyMsg, info->m_hConn);

            // Local-only notice (conn = Invalid, so a remote peer can never
            // forge it): exactly this one player left. CoSyncNet removes just
            // that peer -- the session and everyone else stay up.
            CoSyncTransport::ForwardMessage("PEERLEFT|" + std::to_string(peerSteamID),
                k_HSteamNetConnection_Invalid);
        }
        else if (m_role == GNSRole::Client)
        {


            CoSyncTransport::ForwardMessage("DISCONNECT|host_closed",
                k_HSteamNetConnection_Invalid);
        }
        break;
    }
    }
}

void GNS_Session::Tick()
{
    // StartHost()/StartClient() unconditionally start the background network
    // thread, which already calls RunCallbacks() + ProcessMessagesOnPollGroup()
    // + ProcessMessagesOnConnection() continuously every ~16ms (see
    // NetworkThreadProc). This main-thread Tick() used to call the exact same
    // functions on the exact same connections every frame -- not a fallback
    // for "thread isn't running" (it's always running once connected), just
    // redundant. Two threads concurrently calling the SDK's receive/callback
    // functions on the same connection, plus two threads writing
    // m_peerSteamIDs without both sides taking the lock, is what produced a
    // real unsynchronized-map-write bug (now also fixed at the write sites
    // directly). Removing the redundant call here removes the race at its
    // root instead of only patching one symptom of it.
    if (m_networkThreadRunning.load(std::memory_order_relaxed))
        return;

    auto* sock = gSockets();
    if (!sock)
        return;

    sock->RunCallbacks();

    if (!m_connected)
        return;


    ProcessMessagesOnPollGroup();


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


void GNS_Session::SendText(const std::string& text, bool reliable)
{
    auto* sock = gSockets();
    if (!sock || !m_connected.load(std::memory_order_relaxed))
        return;

    const void* data = text.data();
    const uint32 cb  = static_cast<uint32>(text.size());
    const int sendFlags = reliable ? k_nSteamNetworkingSend_Reliable
                                   : k_nSteamNetworkingSend_Unreliable;

    if (m_role == GNSRole::Client)
    {

        HSteamNetConnection serverConn;
        {
            std::lock_guard<std::mutex> lk(m_connsMutex);
            serverConn = m_serverConn;
        }
        if (serverConn == k_HSteamNetConnection_Invalid)
            return;
        sock->SendMessageToConnection(serverConn, data, cb,
            sendFlags, nullptr);
        return;
    }


    std::vector<HSteamNetConnection> clients;
    {
        std::lock_guard<std::mutex> lk(m_connsMutex);
        clients.assign(m_clientConns.begin(), m_clientConns.end());
    }
    for (auto conn : clients)
        sock->SendMessageToConnection(conn, data, cb,
            sendFlags, nullptr);
}

void GNS_Session::SendTextExcept(const std::string& text, HSteamNetConnection except, bool reliable)
{
    if (m_role != GNSRole::Host || except == k_HSteamNetConnection_Invalid)
    {
        SendText(text, reliable);
        return;
    }
    auto* sock = gSockets();
    if (!sock || !m_connected.load(std::memory_order_relaxed))
        return;

    std::vector<HSteamNetConnection> clients;
    {
        std::lock_guard<std::mutex> lk(m_connsMutex);
        clients.assign(m_clientConns.begin(), m_clientConns.end());
    }
    const int sendFlags = reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
    for (auto conn : clients)
        if (conn != except)
            sock->SendMessageToConnection(conn, text.data(), static_cast<uint32>(text.size()), sendFlags, nullptr);
}

void GNS_Session::KickClient(HSteamNetConnection conn, int reason, const char* debug)
{
    auto* sock = gSockets();
    if (!sock || m_role != GNSRole::Host || conn == k_HSteamNetConnection_Invalid)
        return;

    uint64_t peerSteamID = 0;
    {
        std::lock_guard<std::mutex> lk(m_connsMutex);
        auto it = m_peerSteamIDs.find(conn);
        if (it != m_peerSteamIDs.end())
            peerSteamID = it->second;
        m_peerSteamIDs.erase(conn);
        // N5: don't announce a departure for an identity that is still live
        // on another connection.
        for (const auto& kv : m_peerSteamIDs)
            if (peerSteamID != 0 && kv.second == peerSteamID) { peerSteamID = 0; break; }
    }

    LOG_WARN("[GNS] Kicking client conn=%u (reason=%d '%s')", conn, reason, debug ? debug : "");
    // A locally closed connection gets no status callback, so do the per-peer
    // bookkeeping the ClosedByPeer path would have done.
    sock->CloseConnection(conn, reason, debug, false);
    Host_RemoveAny(conn);
    {
        std::lock_guard<std::mutex> lk(m_connsMutex);
        m_connected.store(!m_clientConns.empty(), std::memory_order_relaxed);
    }

    if (peerSteamID != 0)
    {
        EntityDestroyPacket d{};
        d.entityID = EntityIDFromSteamID(peerSteamID);
        const std::string destroyMsg = SerializeEntityDestroy(d);
        CoSyncTransport::Send(destroyMsg);
        CoSyncTransport::ForwardMessage(destroyMsg, k_HSteamNetConnection_Invalid);
        CoSyncTransport::ForwardMessage("PEERLEFT|" + std::to_string(peerSteamID),
            k_HSteamNetConnection_Invalid);
    }
}

void GNS_Session::LeaveWithReason(int reason, const char* debug)
{
    auto* sock = gSockets();
    if (!sock || m_role != GNSRole::Client)
        return;

    HSteamNetConnection conn;
    {
        std::lock_guard<std::mutex> lk(m_connsMutex);
        conn = m_serverConn;
        m_serverConn = k_HSteamNetConnection_Invalid;
    }
    if (conn == k_HSteamNetConnection_Invalid)
        return;

    LOG_WARN("[GNS] Leaving host (reason=%d '%s')", reason, debug ? debug : "");
    sock->CloseConnection(conn, reason, debug, false);
    m_connected.store(false, std::memory_order_relaxed);
    m_clientEndReason.store(reason);
    m_clientState.store(GNSClientState::Closed);
    CoSyncTransport::ForwardMessage(std::string("DISCONNECT|") + (debug ? debug : "left"),
        k_HSteamNetConnection_Invalid);
}

bool GNS_Session::IsClientConnected(HSteamNetConnection conn) const
{
    std::lock_guard<std::mutex> lk(m_connsMutex);
    return m_clientConns.count(conn) != 0 || m_pendingClientConns.count(conn) != 0;
}

int GNS_Session::GetConnectedClientCount() const
{
    std::lock_guard<std::mutex> lk(m_connsMutex);
    return static_cast<int>(m_clientConns.size());
}

int GNS_Session::GetPingMs(HSteamNetConnection conn) const
{
    auto* sock = gSockets();
    if (!sock)
        return -1;
    if (conn == k_HSteamNetConnection_Invalid)
        conn = m_serverConn;
    if (conn == k_HSteamNetConnection_Invalid)
        return -1;
    SteamNetConnectionRealTimeStatus_t st{};
    if (sock->GetConnectionRealTimeStatus(conn, &st, 0, nullptr) != k_EResultOK)
        return -1;
    return st.m_nPing >= 0 ? st.m_nPing : -1;
}

void GNS_Session::SendTextTo(HSteamNetConnection conn, const std::string& text, bool reliable)
{
    auto* sock = gSockets();
    if (!sock || conn == k_HSteamNetConnection_Invalid)
        return;
    const EResult r = sock->SendMessageToConnection(conn, text.data(), static_cast<uint32>(text.size()),
        reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable, nullptr);
    if (r != k_EResultOK)
        LOG_WARN("[GNS] SendTextTo conn=%u failed (%d, %zu bytes)", conn, (int)r, text.size());
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
    if (!info)
        return;
    {
        // Steam sessions register two delivery paths (see
        // EnsureCallbacksRegistered); handle each (connection, state) once.
        static std::mutex s_seenMutex;
        static std::unordered_map<HSteamNetConnection, int> s_lastState;
        std::lock_guard<std::mutex> lk(s_seenMutex);
        const int st = static_cast<int>(info->m_info.m_eState);
        auto it = s_lastState.find(info->m_hConn);
        if (it != s_lastState.end() && it->second == st)
            return;
        s_lastState[info->m_hConn] = st;
        if (s_lastState.size() > 4096)
            s_lastState.clear();
    }
    GNS_Session::Get().HandleConnectionStatusChanged(info);
}

bool GNS_Session::SelectBackend(bool steam)
{
    if (steam && !CoSyncSteamAPI::Init())
    {
        LOG_ERROR("[GNS] Steam networking unavailable -- can't start a Steam session");
        return false;
    }
    if (g_steamBackend.load() == steam && m_pollGroup != k_HSteamNetPollGroup_Invalid)
    {
        m_steamBackend.store(steam);
        return true;
    }

    if (auto* old = gSockets())
        if (m_pollGroup != k_HSteamNetPollGroup_Invalid)
            old->DestroyPollGroup(m_pollGroup);
    m_pollGroup = k_HSteamNetPollGroup_Invalid;

    g_steamBackend.store(steam);
    m_steamBackend.store(steam);

    if (steam)
    {
        CoSyncSteamAPI::InitRelay();
        // Same buffers as the open-source setup (GNS_Core.cpp). 0.5.9: rate
        // floor = Valve's default 128 KB/s (was 512 KB/s). The floor is sent
        // even when the link can't carry it, so on a slow home upload a high
        // floor turned into packet loss; the rate still climbs to what the
        // connection measures.
        if (auto* u = gUtils())
        {
            u->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendBufferSize, 32 * 1024 * 1024);
            u->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_RecvBufferSize, 32 * 1024 * 1024);
            u->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_RecvBufferMessages, 16384);
            u->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendRateMin, 128 * 1024);
            u->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendRateMax, 8 * 1024 * 1024);
        }
    }
    CoSyncNet::SetSteamIdentity(steam);

    if (auto* sock = gSockets())
        m_pollGroup = sock->CreatePollGroup();
    if (m_pollGroup == k_HSteamNetPollGroup_Invalid)
    {
        LOG_ERROR("[GNS] Failed to create a poll group on the %s backend", steam ? "Steam" : "open-source");
        return false;
    }
    LOG_INFO("[GNS] Backend: %s", steam ? "Steam (P2P + relay)" : "open-source GNS (LAN / direct IP)");
    return true;
}
