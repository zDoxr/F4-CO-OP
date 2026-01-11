#include "CoSyncNet.h"

#include "ConsoleLogger.h"
#include "CoSyncTransport.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncWorld.h"
#include "TickHook.h"
#include "Packets_EntityCreate.h"
#include "Packets_EntityUpdate.h"
#include "EntitySerialization.h"

#include <mutex>
#include <unordered_map>
#include <sstream>
#include <string>
#include <functional>

namespace
{
    bool s_initialized = false;
    bool s_isHost = false;
    bool s_sessionActive = false;
    bool s_connected = false;

    bool s_pendingInit = false;
    bool s_pendingHostFlag = false;

    std::string s_myName = "Player";
    uint64_t    s_mySteamID = 0;
    uint32_t    s_myEntityID = 0;

    // Host: publish host CREATE once (so clients can spawn host proxy)
    bool s_hostCreatePublished = false;

    std::unordered_map<uint64_t, CoSyncNet::RemotePeer> s_peers;
    std::mutex s_peersMutex;

    // Proxy base used for remote players
    constexpr uint32_t kRemotePlayerBaseForm = 0x00000007;

    
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static uint32_t EntityIDFromSteamID(uint64_t sid)
{
    return static_cast<uint32_t>(sid & 0xFFFFFFFFu);
}

static uint64_t HashName(const std::string& name)
{
    return static_cast<uint64_t>(std::hash<std::string>{}(name));
}

// Parse HELLO messages.
// Accepts:
//   HELLO|name|steamid64
//   HELLO|name                (legacy fallback -> host hashes name)
static bool ParseHello(const std::string& msg, std::string& name, uint64_t& sid)
{
    name.clear();
    sid = 0;

    if (msg.rfind("HELLO|", 0) != 0)
        return false;

    std::stringstream ss(msg.substr(6));

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

    return true;
}

// Host helper: broadcast CREATE and optionally enqueue locally for spawn
static void HostBroadcastCreateInternal(
    uint32_t entityID,
    uint32_t baseFormID,
    const NiPoint3& spawnPos,
    const NiPoint3& spawnRot,
    bool enqueueLocal)
{
    EntityCreatePacket p{};
    p.entityID = entityID;
    p.baseFormID = baseFormID;
    p.spawnPos = spawnPos;
    p.spawnRot = spawnRot;

    // Ownership: the entity controls itself for now (player proxies)
    p.ownerEntityID = entityID;
    p.type = CoSyncEntityType::Player;

    CoSyncTransport::Send(SerializeEntityCreate(p));

    if (enqueueLocal)
        g_CoSyncPlayerManager.EnqueueEntityCreate(p);

    LOG_INFO("[CoSyncNet] Host broadcast CREATE entity=%u base=0x%08X local=%d",
        entityID, baseFormID, enqueueLocal ? 1 : 0);
}

static void HostPublishHostCreateIfNeeded()
{
    if (!s_isHost || s_hostCreatePublished || !s_connected)
        return;

    const uint32_t hostEID = EntityIDFromSteamID(CoSyncNet::GetMySteamID());

    // Send to clients; do NOT spawn local proxy for host
    HostBroadcastCreateInternal(
        hostEID,
        kRemotePlayerBaseForm,
        NiPoint3{ 0.f, 0.f, 0.f },
        NiPoint3{ 0.f, 0.f, 0.f },
        false
    );

    s_hostCreatePublished = true;
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
void CoSyncNet::Init(bool isHost)
{
    if (s_initialized)
        return;

    s_isHost = isHost;
    s_sessionActive = true;
    s_initialized = true;
    s_hostCreatePublished = false;

    // Set local entity ID early so CREATE ignores self deterministically
    g_CoSyncPlayerManager.SetLocalEntityID(GetMyEntityID());

    LOG_INFO("[CoSyncNet] Init host=%d localEID=%u", isHost ? 1 : 0, GetMyEntityID());
}

void CoSyncNet::Shutdown()
{
    if (!s_initialized)
        return;

    LOG_INFO("[CoSyncNet] Shutdown");

    s_initialized = false;
    s_sessionActive = false;
    s_connected = false;
    s_pendingInit = false;

    s_hostCreatePublished = false;

    {
        std::lock_guard<std::mutex> lk(s_peersMutex);
        s_peers.clear();
    }

    CoSyncTransport::Shutdown();
}

void CoSyncNet::ScheduleInit(bool isHost)
{
    s_pendingInit = true;
    s_pendingHostFlag = isHost;

    CoSyncTransport::SetReceiveCallback(
        [](const std::string& msg, double now)
        {
            CoSyncNet::OnReceive(msg, now);
        }
    );

    CoSyncTransport::SetConnectedCallback(
        []()
        {
            CoSyncNet::OnGNSConnected();
        }
    );

    if (CoSyncWorld::IsWorldReady())
    {
        s_pendingInit = false;
        Init(isHost);
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
    if (CoSyncTransport::IsInitialized())
        CoSyncTransport::Tick(now);

    if (!s_initialized)
        return;

    // Host: ensure host CREATE is published once, after connection
    if (s_isHost)
        HostPublishHostCreateIfNeeded();

    // IMPORTANT: PlayerManager::Tick() already calls ProcessInbox()
    g_CoSyncPlayerManager.Tick();
}

// -----------------------------------------------------------------------------
// Sending
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
    u.flags = 0;
    u.pos = pos;
    u.rot = rot;
    u.vel = vel;

    // Client timestamp is not authoritative; host will overwrite upon rebroadcast.
    u.timestamp = 0.0;

    CoSyncTransport::Send(SerializeEntityUpdate(u));
}

static void HostBroadcastUpdateInternal(EntityUpdatePacket u, double now)
{
    // Host-authoritative timestamp
    u.timestamp = now;

    // Broadcast to clients
    CoSyncTransport::Send(SerializeEntityUpdate(u));

    // Apply locally (host spawns client proxies too)
    g_CoSyncPlayerManager.EnqueueEntityUpdate(u);
}

// -----------------------------------------------------------------------------
// Receiving (AUTHORITY SPLIT)
// -----------------------------------------------------------------------------
void CoSyncNet::OnReceive(const std::string& msg, double now)
{
    LOG_DEBUG("[CoSyncNet] RX RAW: %s", msg.c_str());

    // ---------------- HELLO ----------------
    if (msg.rfind("HELLO|", 0) == 0)
    {
        std::string name;
        uint64_t sid = 0;

        if (!ParseHello(msg, name, sid))
        {
            LOG_WARN("[CoSyncNet] HELLO parse failed");
            return;
        }

        LOG_INFO("[CoSyncNet] RX HELLO name='%s' sid=%llu",
            name.c_str(), (unsigned long long)sid);

        uint32_t peerEID = EntityIDFromSteamID(sid);

        {
            std::lock_guard<std::mutex> lk(s_peersMutex);
            auto& peer = s_peers[sid];
            peer.steamID = sid;
            peer.username = name;
            peer.assignedEntityID = peerEID;
            peer.lastUpdateTime = now;
        }

        if (s_isHost)
        {
            // Ensure host CREATE exists for this client (host proxy on clients)
            HostPublishHostCreateIfNeeded();

            // Broadcast peer CREATE and enqueue locally so host spawns proxy for client
            HostBroadcastCreateInternal(
                peerEID,
                kRemotePlayerBaseForm,
                NiPoint3{ 0.f, 0.f, 0.f },
                NiPoint3{ 0.f, 0.f, 0.f },
                true
            );
        }

        return;
    }

    // ---------------- UPDATE ----------------
    if (IsEntityUpdate(msg))
    {
        EntityUpdatePacket u{};
        if (!DeserializeEntityUpdate(msg, u))
            return;

        if (s_isHost)
        {
            // HOST: rebroadcast authoritative update to clients + apply locally
            HostBroadcastUpdateInternal(u, now);
        }
        else
        {
            // CLIENT: accept host update (host is the only rebroadcaster)
            g_CoSyncPlayerManager.EnqueueEntityUpdate(u);
        }

        return;
    }

    // ---------------- CREATE ----------------
    if (!s_isHost && IsEntityCreate(msg))
    {
        EntityCreatePacket p{};
        if (DeserializeEntityCreate(msg, p))
            g_CoSyncPlayerManager.EnqueueEntityCreate(p);
        return;
    }

    // Host ignores CREATE from network by design (host is authoritative)
}

// -----------------------------------------------------------------------------
// Identity
// -----------------------------------------------------------------------------
uint64_t CoSyncNet::GetMySteamID()
{
    if (s_mySteamID == 0)
        s_mySteamID = HashName(s_myName);
    return s_mySteamID;
}

uint32_t CoSyncNet::GetMyEntityID()
{
    if (s_myEntityID == 0)
        s_myEntityID = EntityIDFromSteamID(GetMySteamID());
    return s_myEntityID;
}

const char* CoSyncNet::GetMyName()
{
    return s_myName.c_str();
}

void CoSyncNet::SetMyName(const std::string& name)
{
    s_myName = name;
    s_mySteamID = 0;
    s_myEntityID = 0;
}

bool CoSyncNet::IsHost() { return s_isHost; }
bool CoSyncNet::IsInitialized() { return s_initialized; }
bool CoSyncNet::IsSessionActive() { return s_sessionActive; }
bool CoSyncNet::IsConnected() { return s_connected; }

const std::unordered_map<uint64_t, CoSyncNet::RemotePeer>& CoSyncNet::GetPeers()
{
    return s_peers;
}

// -----------------------------------------------------------------------------
// Transport callbacks
// -----------------------------------------------------------------------------
void CoSyncNet::OnGNSConnected()
{
    s_connected = true;

    // Ensure local ID is set before any inbound CREATE is processed
    g_CoSyncPlayerManager.SetLocalEntityID(GetMyEntityID());

    ResetTickHookSendTimer();

    std::ostringstream ss;
    ss << "HELLO|" << s_myName << "|" << GetMySteamID();

    CoSyncTransport::Send(ss.str());
    LOG_INFO("[CoSyncNet] Sent HELLO name='%s' localEID=%u",
        s_myName.c_str(), GetMyEntityID());
}

void CoSyncNet::OnGNSDisconnected()
{
    s_connected = false;
}




