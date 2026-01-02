#include "CoSyncNet.h"

#include "ConsoleLogger.h"
#include "PlayerStatePacket.h"
#include "CoSyncTransport.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncWorld.h"

#include "Packets_EntityCreate.h"
#include "Packets_EntityUpdate.h"
#include "EntitySerialization.h"

#include <mutex>
#include <unordered_map>
#include <string>
#include <sstream>
#include <cstdint>

namespace
{
    bool s_initialized = false;
    bool s_isHost = false;
    bool s_sessionActive = false;

    bool s_pendingInit = false;
    bool s_pendingHostFlag = false;

    bool s_connected = false;

    std::string s_myName = "Player";
    uint64_t    s_mySteamID = 0;

    // Host: have we already published our own CREATE to clients?
    bool s_hostCreatePublished = false;

    // Peers (diagnostic)
    std::unordered_map<uint64_t, CoSyncNet::RemotePeer> s_peers;
    std::mutex s_peersMutex;

    // Host authority table:
    // maps peerSteamID -> assigned entityID (deterministic in this version)
    std::unordered_map<uint64_t, uint32_t> s_peerEntity;
    std::mutex s_peerEntityMutex;

    // Proxy base used for remote players (your current choice)
    constexpr uint32_t kRemotePlayerBaseForm = 0x0014890C;
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static uint32_t EntityIDFromSteamID64(uint64_t sid)
{
    return static_cast<uint32_t>(sid & 0xFFFFFFFFu);
}

static uint64_t HashNameToSteamID64(const std::string& name)
{
    return static_cast<uint64_t>(std::hash<std::string>{}(name));
}

// Parse HELLO messages.
// Accepts:
//   HELLO|name|steamid64
//   HELLO|name                (legacy fallback -> host hashes name)
static bool ParseHello(const std::string& msg, std::string& outName, uint64_t& outSteamID)
{
    outName.clear();
    outSteamID = 0;

    if (msg.rfind("HELLO|", 0) != 0)
        return false;

    std::string payload = msg.substr(6);
    std::stringstream ss(payload);

    std::string tokName;
    std::getline(ss, tokName, '|');
    if (tokName.empty())
        return false;

    outName = tokName;

    std::string tokID;
    if (std::getline(ss, tokID, '|') && !tokID.empty())
    {
        // steamid64 provided
        try
        {
            outSteamID = std::stoull(tokID);
        }
        catch (...)
        {
            outSteamID = 0;
        }
    }

    if (outSteamID == 0)
    {
        // Legacy hello fallback
        outSteamID = HashNameToSteamID64(outName);
    }

    return true;
}

static void Host_SendCreate(uint32_t entityID, uint32_t ownerEntityID, bool enqueueLocal)
{
    EntityCreatePacket p{};
    p.entityID = entityID;
    p.type = CoSyncEntityType::Player;
    p.baseFormID = kRemotePlayerBaseForm;
    p.ownerEntityID = ownerEntityID;
    p.spawnPos = { 0.f, 0.f, 0.f };
    p.spawnRot = { 0.f, 0.f, 0.f };

    CoSyncTransport::Send(SerializeEntityCreate(p));

    // Host: enqueue locally ONLY when we explicitly want a local proxy (rare)
    if (enqueueLocal)
        g_CoSyncPlayerManager.EnqueueEntityCreate(p);

    LOG_INFO("[CoSyncNet] Host broadcast CREATE entityID=%u baseForm=0x%08X (local=%d)",
        p.entityID, p.baseFormID, enqueueLocal ? 1 : 0);
}


static void Host_PublishHostCreateToAllIfNeeded()
{
    if (!s_isHost || s_hostCreatePublished)
        return;

    const uint32_t hostEID = EntityIDFromSteamID64(CoSyncNet::GetMySteamID());

    // Send to clients; do NOT spawn local proxy
    Host_SendCreate(hostEID, hostEID, false);

    s_hostCreatePublished = true;
}


// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
void CoSyncNet::Init(bool host)
{
    if (s_initialized)
        return;

    s_isHost = host;
    s_sessionActive = true;
    s_initialized = true;

    s_hostCreatePublished = false;

    LOG_INFO("[CoSyncNet] Init host=%d", host ? 1 : 0);
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

    s_hostCreatePublished = false;

    {
        std::lock_guard<std::mutex> lk(s_peersMutex);
        s_peers.clear();
    }
    {
        std::lock_guard<std::mutex> lk(s_peerEntityMutex);
        s_peerEntity.clear();
    }

    CoSyncTransport::Shutdown();
}

void CoSyncNet::ScheduleInit(bool host)
{
    s_pendingInit = true;
    s_pendingHostFlag = host;

    LOG_INFO("[CoSyncNet] ScheduleInit host=%d", host ? 1 : 0);

    // F4MP-style: receive path only enqueues/records; never spawns directly
    CoSyncTransport::SetReceiveCallback([](const std::string& msg)
        {
            CoSyncNet::OnReceive(msg);
        });

    if (CoSyncWorld::IsWorldReady())
    {
        s_pendingInit = false;
        Init(host);
    }
}

void CoSyncNet::PerformPendingInit()
{
    if (!s_pendingInit)
        return;

    if (!CoSyncWorld::IsWorldReady())
        return;

    s_pendingInit = false;
    Init(s_pendingHostFlag);
}

// -----------------------------------------------------------------------------
// Tick
// -----------------------------------------------------------------------------
void CoSyncNet::Tick(double now)
{
    if (!s_initialized)
        return;

    CoSyncTransport::Tick(now);

    // Game-thread processing
    g_CoSyncPlayerManager.ProcessInbox();
    g_CoSyncPlayerManager.Tick();
}

// -----------------------------------------------------------------------------
// Sending (LocalPlayerState legacy path)
// -----------------------------------------------------------------------------
void CoSyncNet::SendLocalPlayerState(const LocalPlayerState& state)
{
    if (!s_initialized || !s_sessionActive)
        return;

    std::string out;
    if (SerializePlayerStateToString(state, out))
        CoSyncTransport::Send(out);
}

// -----------------------------------------------------------------------------
// Entity protocol: UPDATE ONLY here (clients never CREATE)
// -----------------------------------------------------------------------------
void CoSyncNet::SendMyEntityUpdate(
    uint32_t entityID,
    const NiPoint3& pos,
    const NiPoint3& rot,
    const NiPoint3& vel)
{
    if (!s_initialized || !s_connected)
        return;

    EntityUpdatePacket u{};
    u.entityID = entityID;
    u.username = s_myName; // debug label
    u.pos = pos;
    u.rot = rot;
    u.vel = vel;
    u.timestamp = 0.0;

    CoSyncTransport::Send(SerializeEntityUpdate(u));
}

// -----------------------------------------------------------------------------
// Receiving (AUTHORITY SPLIT)
// -----------------------------------------------------------------------------
void CoSyncNet::OnReceive(const std::string& msg)
{
    // ----------------------------
    // HELLO handshake
    // ----------------------------
    if (msg.rfind("HELLO|", 0) == 0)
    {
        std::string peerName;
        uint64_t peerSteamID = 0;

        if (!ParseHello(msg, peerName, peerSteamID))
        {
            LOG_WARN("[CoSyncNet] HELLO parse failed");
            return;
        }

        LOG_INFO("[CoSyncNet] RX HELLO name='%s' sid=%llu",
            peerName.c_str(), (unsigned long long)peerSteamID);

        // Record peer (diagnostic)
        {
            std::lock_guard<std::mutex> lk(s_peersMutex);
            auto& rp = s_peers[peerSteamID];
            rp.steamID = peerSteamID;
            rp.username = peerName;
            rp.lastUpdateTime = 0.0;
        }

        if (s_isHost)
        {
            // Ensure we have an entityID for this peer (deterministic)
            const uint32_t peerEntityID = EntityIDFromSteamID64(peerSteamID);
            {
                std::lock_guard<std::mutex> lk(s_peerEntityMutex);
                s_peerEntity[peerSteamID] = peerEntityID;
            }

            // 1) Publish host CREATE (so new client can spawn host proxy)
            // 2) Publish peer CREATE (so everyone spawns new client proxy)
            Host_PublishHostCreateToAllIfNeeded();
            Host_SendCreate(peerEntityID, peerEntityID, true);

            LOG_INFO("[CoSyncNet] Host assigned peer entityID=%u for sid=%llu",
                peerEntityID, (unsigned long long)peerSteamID);
        }

        return;
    }

    // ----------------------------
    // HOST: handle client UPDATE
    // ----------------------------
    if (s_isHost && IsEntityUpdate(msg))
    {
        EntityUpdatePacket u{};
        if (!DeserializeEntityUpdate(msg, u))
            return;

        // Optional: host can sanity-check that the entityID belongs to a known peer.
        // We do not hard-drop here to keep iteration simple.
        // Rebroadcast authoritative update to all clients, and feed local manager too.
        CoSyncTransport::Send(SerializeEntityUpdate(u));
        g_CoSyncPlayerManager.EnqueueEntityUpdate(u);
        return;
    }

    // ----------------------------
    // CLIENT: receive host packets
    // ----------------------------
    if (!s_isHost && IsEntityCreate(msg))
    {
        EntityCreatePacket p{};
        if (DeserializeEntityCreate(msg, p))
            g_CoSyncPlayerManager.EnqueueEntityCreate(p);
        return;
    }

    if (!s_isHost && IsEntityUpdate(msg))
    {
        EntityUpdatePacket u{};
        if (DeserializeEntityUpdate(msg, u))
            g_CoSyncPlayerManager.EnqueueEntityUpdate(u);
        return;
    }

    // Unknown message types ignored for now
}

// -----------------------------------------------------------------------------
// Identity
// -----------------------------------------------------------------------------
uint64_t CoSyncNet::GetMySteamID()
{
    if (s_mySteamID == 0)
        s_mySteamID = HashNameToSteamID64(s_myName);

    return s_mySteamID;
}

const char* CoSyncNet::GetMyName()
{
    return s_myName.c_str();
}

void CoSyncNet::SetMyName(const std::string& name)
{
    s_myName = name;
    s_mySteamID = 0;
}

bool CoSyncNet::IsInitialized() { return s_initialized; }
bool CoSyncNet::IsSessionActive() { return s_sessionActive; }
bool CoSyncNet::IsConnected() { return s_connected; }

const std::unordered_map<uint64_t, CoSyncNet::RemotePeer>& CoSyncNet::GetPeers()
{
    return s_peers;
}

// -----------------------------------------------------------------------------
// Connection callback from GNS
// -----------------------------------------------------------------------------
void CoSyncNet::OnGNSConnected()
{
    s_connected = true;

    // F4MP-style: announce presence immediately once transport is up.
    // Include steam/hash id so host can deterministically assign entityID.
    {
        const uint64_t sid = GetMySteamID();

        std::ostringstream ss;
        ss << "HELLO|" << s_myName << "|" << sid;

        CoSyncTransport::Send(ss.str());
        LOG_INFO("[CoSyncNet] Sent HELLO name='%s' sid=%llu",
            s_myName.c_str(), (unsigned long long)sid);
    }

    // If we are host, we can publish our host CREATE once a client exists.
    // (We also publish on receiving HELLO.)
}
