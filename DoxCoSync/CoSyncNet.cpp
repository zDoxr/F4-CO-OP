#include "CoSyncNet.h"

#include "ConsoleLogger.h"
#include "PlayerStatePacket.h"
#include "CoSyncTransport.h"
#include "CoSyncPlayerManager.h"

#include <chrono>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <deque>


// Static state
namespace
{
    bool   s_initialized = false;
    bool   s_isHost = false;
    bool   s_sessionActive = false;
    bool   s_pendingInit = false;
    bool   s_pendingHostFlag = false;
    bool   s_connected = false;


    std::mutex s_inboxMutex;
    std::deque<std::string> s_inbox;


    // Identity
    std::string s_myName = "Player";
    uint64_t    s_mySteamID = 0;     // legacy / overlay ID

    // Peer list for overlay/debug
    std::unordered_map<uint64_t, CoSyncNet::RemotePeer> s_peers;

    // Simple time helper
    double GetNowSeconds()
    {
        using clock = std::chrono::steady_clock;
        static auto start = clock::now();
        auto        now = clock::now();
        return std::chrono::duration<double>(now - start).count();
    }
}

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void CoSyncNet::Init(bool host)
{
    if (s_initialized)
        return;

    s_isHost = host;
    s_sessionActive = true;
    s_initialized = true;

    LOG_INFO("[CoSyncNet] Init host=%d", host ? 1 : 0);

    // IMPORTANT:
    //  Transport (InitAsHost / InitAsClient) is driven by overlay/UI.
    //  For host: overlay should have called InitAsHost() already.
    //  For client: overlay should have called InitAsClient() already.
    //
    //  We do NOT re-initialize sockets here to avoid tearing down
    //  existing connections.
}

void CoSyncNet::Shutdown()
{
    if (!s_initialized)
        return;

    LOG_INFO("[CoSyncNet] Shutdown");

    s_initialized = false;
    s_sessionActive = false;
    s_pendingInit = false;
    s_connected = false;

    s_peers.clear();

    CoSyncTransport::Shutdown();
}

// ---------------------------------------------------------------------------
// Deferred Init (called once world is ready via TickHook)
// ---------------------------------------------------------------------------
void CoSyncNet::ScheduleInit(bool host)
{
    s_pendingInit = true;
    s_pendingHostFlag = host;

    LOG_INFO("[CoSyncNet] ScheduleInit host=%d", host ? 1 : 0);

    // Attach receive callback IMMEDIATELY so that *any* packets arriving
    // after Host/Join are processed, even if the world is not yet ready.
    CoSyncTransport::SetReceiveCallback([](const std::string& msg) {
        LOG_DEBUG("[CoSyncTransport] RECV RAW: %s", msg.c_str());
        CoSyncNet::OnReceive(msg);
        });

    LOG_INFO("[CoSyncNet] Transport receive callback set (ScheduleInit)");
}

void CoSyncNet::PerformPendingInit()
{
    if (!s_pendingInit)
        return;

    s_pendingInit = false;

    // Complete logical CoSyncNet init now that the world exists.
    Init(s_pendingHostFlag);
}

// ---------------------------------------------------------------------------
// Tick – network/transport only (player manager is ticked in TickHook)
// ---------------------------------------------------------------------------
void CoSyncNet::Tick(double now)
{
    if (!s_initialized)
        return;

    // Pump underlying sockets / transports (receive/send)
    CoSyncTransport::Tick(now);
    
   
}

// ---------------------------------------------------------------------------
// Outgoing state
// ---------------------------------------------------------------------------
void CoSyncNet::SendLocalPlayerState(const LocalPlayerState& state)
{
    if (!s_initialized || !CoSyncNet::IsSessionActive())
        return;

    std::string out;
    if (!SerializePlayerStateToString(state, out))
        return;

    CoSyncTransport::Send(out);
}

// ---------------------------------------------------------------------------
// Incoming messages
// ---------------------------------------------------------------------------
void CoSyncNet::OnReceive(const std::string& msg)
{
    if (msg.rfind("HELLO|", 0) == 0)
    {
        LOG_INFO("[CoSyncNet] Handshake received: %s", msg.c_str());
        return;
    }

    g_CoSyncPlayerManager.EnqueueIncoming(msg);
}



// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
uint64_t CoSyncNet::GetMySteamID()
{
    if (s_mySteamID == 0)
        s_mySteamID = std::hash<std::string>{}(s_myName);

    return s_mySteamID;
}

const char* CoSyncNet::GetMyName()
{
    return s_myName.c_str();
}

void CoSyncNet::SetMyName(const std::string& name)
{
    s_myName = name;
    s_mySteamID = 0; // force recompute
    LOG_INFO("[CoSyncNet] MyName set to '%s'", s_myName.c_str());
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
bool CoSyncNet::IsInitialized()
{
    return s_initialized;
}

bool CoSyncNet::IsSessionActive()
{
    return s_sessionActive;
}

// ---------------------------------------------------------------------------
// Peers (for overlay / debug)
// ---------------------------------------------------------------------------
const std::unordered_map<uint64_t, CoSyncNet::RemotePeer>& CoSyncNet::GetPeers()
{
    return s_peers;
}

// ---------------------------------------------------------------------------
// GNS connection status
// ---------------------------------------------------------------------------
void CoSyncNet::OnGNSConnected()
{
    s_connected = true;
    LOG_INFO("[CoSyncNet] GNS connected");

    // 🔥 NEW: immediate handshake
    std::string hello = "HELLO|" + std::string(GetMyName());
    CoSyncTransport::Send(hello);
}


bool CoSyncNet::IsConnected()
{
    return s_connected;
}
