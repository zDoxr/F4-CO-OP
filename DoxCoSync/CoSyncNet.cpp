// CoSyncNet.cpp
#include "CoSyncNet.h"

#include "ConsoleLogger.h"
#include "CoSyncTransport.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncWorld.h"
#include "CoSyncLocalPlayer.h"

#include "Packets_EntityCreate.h"
#include "Packets_EntityUpdate.h"
#include "Packets_EntityDestroy.h"
#include "EntitySerialization.h"

#include <mutex>
#include <unordered_map>
#include <sstream>
#include <string>
#include <functional>

// ============================================================================
// FILE-LOCAL STATE
// ============================================================================
namespace
{
    bool s_initialized = false;
    bool s_isHost = false;
    bool s_connected = false;
    bool s_helloSent = false;

    bool s_pendingInit = false;
    bool s_pendingHostFlag = false;

    std::string s_myName = "Player";
    uint64_t    s_mySteamID = 0;
    uint32_t    s_myEntityID = 0;

    bool s_hostCreatePublished = false;

    // Optional: track known peers (for debug)
    struct RemotePeer
    {
        uint64_t steamID = 0;
        std::string username;
        uint32_t entityID = 0;
        double lastSeen = 0.0;
    };

    std::unordered_map<uint64_t, RemotePeer> s_peers;
    std::mutex s_peerMutex;
}

// ============================================================================
// HELPERS
// ============================================================================
static uint64_t HashName(const std::string& s)
{
    return static_cast<uint64_t>(std::hash<std::string>{}(s));
}

static uint32_t EntityIDFromSteamID(uint64_t sid)
{
    return static_cast<uint32_t>(sid & 0xFFFFFFFFu);
}

static bool ParseHello(const std::string& msg, std::string& outName, uint64_t& outSid)
{
    outName.clear();
    outSid = 0;

    if (msg.rfind("HELLO|", 0) != 0)
        return false;

    std::stringstream ss(msg.substr(6)); // after "HELLO|"

    if (!std::getline(ss, outName, '|') || outName.empty())
        return false;

    std::string tok;
    if (std::getline(ss, tok, '|') && !tok.empty())
    {
        try { outSid = std::stoull(tok); }
        catch (...) { outSid = 0; }
    }

    if (outSid == 0)
        outSid = HashName(outName);

    return outSid != 0;
}

// ============================================================================
// HOST: CREATE BROADCAST (PLAYER BASE RESOLVED LOCALLY; baseFormID=0)
// ============================================================================
// IMPORTANT: This is how you remove kRemotePlayerBaseForm from the network.
// Players do NOT need baseFormID sent; each side can resolve its own player base.
static void HostBroadcastPlayerCreate(
    uint32_t entityID,
    const NiPoint3& pos,
    const NiPoint3& rot,
    bool enqueueLocal)
{
    EntityCreatePacket p{};
    p.entityID = entityID;
    p.type = CoSyncEntityType::Player;

    // 🔒 base resolved locally in SpawnTask/PlayerMgr (do NOT send Player base)
    p.baseFormID = 0;

    // player owns itself (fine for now)
    p.ownerEntityID = entityID;

    // keep semantics consistent
    p.spawnFlags = EntityCreatePacket::HiddenOnSpawn | EntityCreatePacket::RemoteControlled;

    p.spawnPos = pos;
    p.spawnRot = rot;

    LOG_INFO("[CoSyncNet] TX CREATE Player entity=%u enqueueLocal=%d",
        entityID, enqueueLocal ? 1 : 0);

    CoSyncTransport::Send(SerializeEntityCreate(p));

    if (enqueueLocal)
        g_CoSyncPlayerManager.EnqueueEntityCreate(p);
}

static void HostPublishHostCreateIfNeeded()
{
    if (!s_isHost || s_hostCreatePublished == true || !s_connected)
        return;

    const uint32_t eid = EntityIDFromSteamID(CoSyncNet::GetMySteamID());

    HostBroadcastPlayerCreate(
        eid,
        NiPoint3{ 0.f, 0.f, 0.f },
        NiPoint3{ 0.f, 0.f, 0.f },
        false
    );

    s_hostCreatePublished = true;
}

// ============================================================================
// LIFECYCLE
// ============================================================================
void CoSyncNet::Init(bool isHost)
{
    if (s_initialized)
        return;

    s_initialized = true;
    s_isHost = isHost;

    // Reset session flags
    s_helloSent = false;
    s_hostCreatePublished = false;

    // Local identity must be set in PlayerMgr early
    g_CoSyncPlayerManager.SetLocalEntityID(GetMyEntityID());

    // Initialize local player movement tracking
    CoSyncLocalPlayer::Init();

    LOG_INFO("[CoSyncNet] Init host=%d localEID=%u",
        isHost ? 1 : 0, GetMyEntityID());
}

void CoSyncNet::ScheduleInit(bool isHost)
{
    // CRITICAL: Role must be known before any inbound HELLO is processed.
    s_pendingInit = true;
    s_pendingHostFlag = isHost;

    // Wire callbacks immediately.
    CoSyncTransport::SetReceiveCallback(
        [](const std::string& msg, double now, HSteamNetConnection c)
        {
            CoSyncNet::OnReceive(msg, now, c);
        });

    CoSyncTransport::SetConnectedCallback(
        []()
        {
            CoSyncNet::OnGNSConnected();
        });

    // If world already ready, init now. Otherwise, wait for PerformPendingInit.
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

void CoSyncNet::Shutdown()
{
    if (!s_initialized && !s_pendingInit)
        return;

    LOG_INFO("[CoSyncNet] Shutdown");

    s_initialized = false;
    s_pendingInit = false;

    s_connected = false;
    s_isHost = false;

    s_helloSent = false;
    s_hostCreatePublished = false;

    {
        std::lock_guard<std::mutex> lk(s_peerMutex);
        s_peers.clear();
    }

    CoSyncLocalPlayer::Shutdown();
    CoSyncTransport::Shutdown();
}

// ============================================================================
// STATE
// ============================================================================
bool CoSyncNet::IsHost() { return s_isHost; }
bool CoSyncNet::IsInitialized() { return s_initialized; }
bool CoSyncNet::IsConnected() { return s_connected; }

// ============================================================================
// IDENTITY
// ============================================================================
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

    // keep PlayerMgr aligned
    g_CoSyncPlayerManager.SetLocalEntityID(GetMyEntityID());
}

void CoSyncNet::SetMySteamID(uint64_t sid)
{
    s_mySteamID = sid;
    s_myEntityID = 0;

    g_CoSyncPlayerManager.SetLocalEntityID(GetMyEntityID());
}

// ============================================================================
// TICK
// ============================================================================
void CoSyncNet::Tick(double now)
{
    // 1) Always pump transport first (enqueues inbound)
    CoSyncTransport::Tick(now);

    // 2) Ensure delayed init happens as soon as world ready
    PerformPendingInit();

    // 3) PlayerMgr tick applies inbox + spawn pump
    if (s_initialized)
        g_CoSyncPlayerManager.Tick();

    if (!s_initialized)
        return;

    // Host publishes its own create after connection is live.
    if (s_isHost && s_connected)
        HostPublishHostCreateIfNeeded();

    // Host NPC authority updates are handled in PlayerMgr (your existing code)
    g_CoSyncPlayerManager.HostSendNpcUpdates(now);
}

// ============================================================================
// SEND
// ============================================================================
void CoSyncNet::SendMyEntityUpdate(
    uint32_t entityID,
    const NiPoint3& pos,
    const NiPoint3& rot,
    const NiPoint3& vel,
    double now)
{
    if (!s_initialized || !s_connected)
        return;

    EntityUpdatePacket u{};
    u.entityID = entityID;
    u.pos = pos;
    u.rot = rot;
    u.vel = vel;
    u.timestamp = now;

    CoSyncTransport::Send(SerializeEntityUpdate(u));
}

void CoSyncNet::HostSpawnNpc(
    uint32_t entityID,
    uint32_t baseFormID,
    const NiPoint3& pos,
    const NiPoint3& rot)
{
    if (!s_initialized || !s_isHost || !s_connected)
        return;

    EntityCreatePacket p{};
    p.entityID = entityID;
    p.type = CoSyncEntityType::NPC;
    p.baseFormID = baseFormID; // MUST be a real TESNPC baseForm
    p.ownerEntityID = 0;
    p.spawnFlags = EntityCreatePacket::RemoteControlled;
    p.spawnPos = pos;
    p.spawnRot = rot;

    LOG_INFO("[CoSyncNet] TX CREATE NPC entity=%u base=0x%08X", entityID, baseFormID);

    CoSyncTransport::Send(SerializeEntityCreate(p));

    // Host must enqueue locally too (so host sees the NPC)
    g_CoSyncPlayerManager.EnqueueEntityCreate(p);
}

// ============================================================================
// RECEIVE
// ============================================================================
void CoSyncNet::OnReceive(const std::string& msg, double now, HSteamNetConnection conn)
{
    // Host-role must be known even before Init completes.
    const bool isHostRole = s_isHost || (s_pendingInit && s_pendingHostFlag);

    // HELLO (client -> host only)
    if (msg.rfind("HELLO|", 0) == 0)
    {
        std::string name;
        uint64_t sid = 0;
        if (!ParseHello(msg, name, sid))
            return;

        const uint32_t eid = EntityIDFromSteamID(sid);

        {
            std::lock_guard<std::mutex> lk(s_peerMutex);
            s_peers[sid] = { sid, name, eid, now };
        }

        LOG_INFO("[CoSyncNet] RX HELLO name='%s' sid=%llu eid=%u (isHostRole=%d)",
            name.c_str(),
            (unsigned long long)sid,
            eid,
            isHostRole ? 1 : 0);

        if (isHostRole)
        {
            LOG_INFO("[CoSyncNet] Processing HELLO from client eid=%u", eid);

            // 1. Create for the joining client
            HostBroadcastPlayerCreate(
                eid,
                NiPoint3{ 0.f, 0.f, 0.f },
                NiPoint3{ 0.f, 0.f, 0.f },
                true
            );

            LOG_INFO("[CoSyncNet] Sent CREATE for client entity=%u", eid);

            // 2. ALWAYS send host's CREATE to the new client
            // This ensures the client can see the host!
            const uint32_t hostEID = GetMyEntityID();

            HostBroadcastPlayerCreate(
                hostEID,
                NiPoint3{ 0.f, 0.f, 0.f },
                NiPoint3{ 0.f, 0.f, 0.f },
                false
            );

            s_hostCreatePublished = true;

            LOG_INFO("[CoSyncNet] Sent CREATE for host entity=%u (BIDIRECTIONAL FIX)", hostEID);
        }

        return;
    }

    // UPDATE
    if (IsEntityUpdate(msg))
    {
        EntityUpdatePacket u{};
        if (!DeserializeEntityUpdate(msg, u))
            return;

        // F4MP rule: only host re-broadcasts; clients just enqueue
        if (isHostRole)
        {
            // Host rebroadcast (authoritative fanout)
            u.timestamp = now;
            CoSyncTransport::Send(SerializeEntityUpdate(u));
        }

        g_CoSyncPlayerManager.EnqueueEntityUpdate(u);
        return;
    }

    // DESTROY
    if (IsEntityDestroy(msg))
    {
        EntityDestroyPacket d{};
        if (DeserializeEntityDestroy(msg, d))
            g_CoSyncPlayerManager.EnqueueEntityDestroy(d);
        return;
    }

    // CREATE (host sends; clients receive)
    if (IsEntityCreate(msg))
    {
        EntityCreatePacket p{};
        if (!DeserializeEntityCreate(msg, p))
            return;

        // Host should not re-enqueue creates it originated unless you explicitly want it;
        // but enqueue is safe on client and host.
        g_CoSyncPlayerManager.EnqueueEntityCreate(p);
        return;
    }

    (void)conn;
}

// ============================================================================
// TRANSPORT CALLBACKS
// ============================================================================
void CoSyncNet::OnGNSConnected()
{
    s_connected = true;

    // Always keep local ID in sync
    g_CoSyncPlayerManager.SetLocalEntityID(GetMyEntityID());

    // Host NEVER sends HELLO.
    if (s_isHost || (s_pendingInit && s_pendingHostFlag))
    {
        LOG_INFO("[CoSyncNet] Host transport connected. localEID=%u", GetMyEntityID());
        return;
    }

    // Client sends HELLO once
    if (s_helloSent)
        return;

    s_helloSent = true;

    std::ostringstream ss;
    ss << "HELLO|" << s_myName << "|" << GetMySteamID();
    CoSyncTransport::Send(ss.str());

    LOG_INFO("[CoSyncNet] Sent HELLO name='%s' localEID=%u",
        s_myName.c_str(), GetMyEntityID());
}

void CoSyncNet::OnGNSDisconnected()
{
    s_connected = false;
    s_helloSent = false;
    s_hostCreatePublished = false;
}
