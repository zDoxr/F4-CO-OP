#include "CoSyncNet.h"

#include "ConsoleLogger.h"
#include "CoSyncTransport.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncWorld.h"

#include "Packets_EntityCreate.h"
#include "Packets_EntityUpdate.h"
#include "Packets_EntityDestroy.h"
#include "EntitySerialization.h"

#include "GameReferences.h"

#include <mutex>
#include <unordered_map>
#include <sstream>
#include <string>
#include <functional>

extern RelocPtr<PlayerCharacter*> g_player;

// ============================================================================
// FILE-LOCAL STATE
// ============================================================================
namespace
{
    bool s_initialized = false;
    bool s_isHost = false;
    bool s_sessionActive = false;
    bool s_connected = false;
    bool s_helloSent = false;

    bool s_pendingInit = false;
    bool s_pendingHostFlag = false;

    std::string s_myName = "Player";
    uint64_t    s_mySteamID = 0;
    uint32_t    s_myEntityID = 0;

    bool s_hostCreatePublished = false;

    // ------------------------------------------------------------------------
    // TEST NPC SCHEDULER (ROBUST)
    // ------------------------------------------------------------------------
    bool   s_testNpcPending = false;
    bool   s_testNpcSpawned = false;
    double s_testNpcDeadline = 0.0;   // absolute time when spawn should occur
    double s_testNpcLastDiag = 0.0;   // rate-limit diagnostics
    static bool s_requestArmTestNpc = false;


    constexpr uint32_t kRemotePlayerBaseForm = 0x00000007;

    // Vault-Tec 81 resident (your test base)
    constexpr uint32_t kTestNpcBaseForm = 0x0014890C;
    constexpr uint32_t kTestNpcEntityID = 36865;

    // ------------------------------------------------------------------------
    struct RemotePeer
    {
        uint64_t steamID = 0;
        std::string username;
        uint32_t entityID = 0;
        double lastUpdate = 0.0;
    };

    struct ConnectionPeer
    {
        uint64_t steamID = 0;
        uint32_t entityID = 0;
    };

    std::unordered_map<uint64_t, RemotePeer> s_peers;
    std::unordered_map<HSteamNetConnection, ConnectionPeer> s_connMap;
    std::mutex s_peerMutex;
}

// ============================================================================
// HELPERS (SINGLE DEFINITIONS)
// ============================================================================
static uint64_t HashName(const std::string& s)
{
    return static_cast<uint64_t>(std::hash<std::string>{}(s));
}

static uint32_t EntityIDFromSteamID(uint64_t sid)
{
    return static_cast<uint32_t>(sid & 0xFFFFFFFFu);
}

static bool ParseHello(const std::string& msg, std::string& name, uint64_t& sid)
{
    if (msg.rfind("HELLO|", 0) != 0)
        return false;

    std::stringstream ss(msg.substr(6));
    if (!std::getline(ss, name, '|') || name.empty())
        return false;

    std::string tok;
    if (std::getline(ss, tok, '|'))
    {
        try { sid = std::stoull(tok); }
        catch (...) { sid = 0; }
    }

    if (sid == 0)
        sid = HashName(name);

    return true;
}

// ============================================================================
// HOST BROADCAST HELPERS
// ============================================================================
static void HostBroadcastPlayerCreate(
    uint32_t entityID,
    uint32_t baseForm,
    const NiPoint3& pos,
    const NiPoint3& rot,
    bool enqueueLocal)
{
    EntityCreatePacket p{};
    p.entityID = entityID;
    p.type = CoSyncEntityType::Player;
    p.baseFormID = baseForm;
    p.ownerEntityID = entityID;
    p.spawnFlags =
        EntityCreatePacket::HiddenOnSpawn |
        EntityCreatePacket::RemoteControlled;
    p.spawnPos = pos;
    p.spawnRot = rot;

    CoSyncTransport::Send(SerializeEntityCreate(p));

    if (enqueueLocal)
        g_CoSyncPlayerManager.EnqueueEntityCreate(p);

    LOG_INFO("[CoSyncNet] Host broadcast CREATE entity=%u base=0x%08X local=%d",
        entityID, baseForm, enqueueLocal ? 1 : 0);
}

static void HostPublishHostCreateIfNeeded()
{
    if (!s_isHost || s_hostCreatePublished || !s_connected)
        return;

    const uint32_t eid = EntityIDFromSteamID(CoSyncNet::GetMySteamID());

    HostBroadcastPlayerCreate(
        eid,
        kRemotePlayerBaseForm,
        NiPoint3{ 0.f, 0.f, 0.f },
        NiPoint3{ 0.f, 0.f, 0.f },
        false
    );

    s_hostCreatePublished = true;
}

static void HostBroadcastUpdateInternal(EntityUpdatePacket& u, double now)
{
    u.timestamp = now;
    CoSyncTransport::Send(SerializeEntityUpdate(u));
    g_CoSyncPlayerManager.EnqueueEntityUpdate(u);
}

// ============================================================================
// TEST NPC CONTROL
// ============================================================================
static void ArmTestNpcIfNeeded(double now)
{
    if (!s_isHost || !s_connected)
        return;

    if (s_testNpcSpawned || s_testNpcPending)
        return;

    s_testNpcPending = true;
    s_testNpcDeadline = now + 1.0;

    LOG_INFO("[CoSyncNet] TEST NPC armed base=0x%08X entity=%u",
        kTestNpcBaseForm, kTestNpcEntityID);
}

static void TrySpawnTestNpc(double now)
{
    if (!s_isHost || !s_connected)
        return;

    if (!s_testNpcPending || s_testNpcSpawned)
        return;

    if (now < s_testNpcDeadline)
        return;

    // Preconditions
    if (!CoSyncWorld::IsWorldReady())
    {
        if (now - s_testNpcLastDiag >= 1.0)
        {
            s_testNpcLastDiag = now;
            LOG_WARN("[CoSyncNet] TEST NPC pending: world not ready (deadline passed)");
        }
        return;
    }

    PlayerCharacter* pc = *g_player;
    if (!pc)
    {
        if (now - s_testNpcLastDiag >= 1.0)
        {
            s_testNpcLastDiag = now;
            LOG_WARN("[CoSyncNet] TEST NPC pending: g_player is NULL (deadline passed)");
        }
        return;
    }

    LOG_INFO("[CoSyncNet] TEST NPC SPAWN firing: entity=%u base=0x%08X",
        kTestNpcEntityID, kTestNpcBaseForm);

    CoSyncNet::HostSpawnNpc(
        kTestNpcEntityID,
        kTestNpcBaseForm,
        pc->pos,
        pc->rot
    );

    s_testNpcSpawned = true;
    s_testNpcPending = false;
}

// ============================================================================
// LIFECYCLE
// ============================================================================
void CoSyncNet::Init(bool isHost)
{
    if (s_initialized)
        return;

    s_isHost = isHost;
    s_helloSent = false;
    s_sessionActive = true;
    s_initialized = true;

    s_hostCreatePublished = false;

    s_testNpcPending = false;
    s_testNpcSpawned = false;
    s_testNpcDeadline = 0.0;
    s_testNpcLastDiag = 0.0;

    g_CoSyncPlayerManager.SetLocalEntityID(GetMyEntityID());

    LOG_INFO("[CoSyncNet] Init host=%d localEID=%u", isHost ? 1 : 0, GetMyEntityID());
}

void CoSyncNet::ScheduleInit(bool isHost)
{
    s_pendingInit = true;
    s_pendingHostFlag = isHost;

    CoSyncTransport::SetReceiveCallback(
        [](const std::string& msg, double now, HSteamNetConnection c)
        {
            CoSyncNet::OnReceive(msg, now, c);
        });

    CoSyncTransport::SetConnectedCallback(
        []() { CoSyncNet::OnGNSConnected(); });

    if (CoSyncWorld::IsWorldReady())
    {
        s_pendingInit = false;
        Init(isHost);
    }
}

void CoSyncNet::PerformPendingInit()
{
    if (!s_pendingInit || !CoSyncWorld::IsWorldReady())
        return;

    s_pendingInit = false;
    Init(s_pendingHostFlag);
}

// ============================================================================
// TICK
// ============================================================================
void CoSyncNet::Tick(double now)
{
    // ✅ Pump transport first so RX gets enqueued before PlayerMgr processes inbox
    CoSyncTransport::Tick(now);

    if (s_initialized)
        g_CoSyncPlayerManager.Tick();

    if (!s_initialized)
        return;

    if (s_isHost && s_connected)
        HostPublishHostCreateIfNeeded();
    g_CoSyncPlayerManager.HostSendNpcUpdates(now);

    if (s_requestArmTestNpc)
    {
        s_requestArmTestNpc = false;
        ArmTestNpcIfNeeded(now);
    }

    TrySpawnTestNpc(now);
}


// ============================================================================
// SEND
// ============================================================================
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
    u.pos = pos;
    u.rot = rot;
    u.vel = vel;
    u.timestamp = 0.0;

    CoSyncTransport::Send(SerializeEntityUpdate(u));

    double now = NowSeconds();
    
    static double s_last = 0.0;
    if (now - s_last > 1.0)
    {
        s_last = now;
        LOG_INFO("[CoSyncNet] TX EU entity=%u", entityID);
    }

}

void CoSyncNet::HostSpawnNpc(
    uint32_t entityID,
    uint32_t baseFormID,
    const NiPoint3& pos,
    const NiPoint3& rot)
{
    if (!s_isHost || !s_connected)
        return;

    EntityCreatePacket p{};
    p.entityID = entityID;
    p.type = CoSyncEntityType::NPC;
    p.baseFormID = baseFormID;
    p.ownerEntityID = 0;
    p.spawnFlags = EntityCreatePacket::RemoteControlled;
    p.spawnPos = pos;
    p.spawnRot = rot;

    // Send to clients
    CoSyncTransport::Send(SerializeEntityCreate(p));

    // Enqueue locally so host spawns it too (via PlayerMgr Tick -> SpawnTasks)
    g_CoSyncPlayerManager.EnqueueEntityCreate(p);

    LOG_INFO("[CoSyncNet] HostSpawnNpc queued CREATE entity=%u base=0x%08X",
        entityID, baseFormID);
}

// ============================================================================
// RECEIVE
// ============================================================================
void CoSyncNet::OnReceive(const std::string& msg, double now, HSteamNetConnection conn)
{
    LOG_DEBUG("[CoSyncNet] RX RAW (conn=%u): %s", conn, msg.c_str());

    const bool isHostRole = s_isHost || (s_pendingInit && s_pendingHostFlag);

    // HELLO
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
            if (isHostRole)
                s_connMap[conn] = { sid, eid };
        }

        if (isHostRole)
        {
            HostPublishHostCreateIfNeeded();

            HostBroadcastPlayerCreate(
                eid,
                kRemotePlayerBaseForm,
                NiPoint3{ 0.f, 0.f, 0.f },
                NiPoint3{ 0.f, 0.f, 0.f },
                true
            );

            if (!s_testNpcSpawned && !s_testNpcPending)
            {
                s_requestArmTestNpc = true;
                LOG_INFO("[CoSyncNet] TEST NPC arm requested (will arm on Tick clock)");
            }

        }

        return;
    }

    // UPDATE
    if (IsEntityUpdate(msg))
    {
        EntityUpdatePacket u{};
        if (!DeserializeEntityUpdate(msg, u))
            return;

        if (isHostRole)
            HostBroadcastUpdateInternal(u, now);
        else
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

    // CREATE (clients only)
    if (!isHostRole && IsEntityCreate(msg))
    {
        EntityCreatePacket p{};
        if (DeserializeEntityCreate(msg, p))
            g_CoSyncPlayerManager.EnqueueEntityCreate(p);
        return;
    }
}

// ============================================================================
// IDENTITY
// ============================================================================
uint64_t CoSyncNet::GetMySteamID()
{
    if (!s_mySteamID)
        s_mySteamID = HashName(s_myName);
    return s_mySteamID;
}

uint32_t CoSyncNet::GetMyEntityID()
{
    if (!s_myEntityID)
        s_myEntityID = EntityIDFromSteamID(GetMySteamID());
    return s_myEntityID;
}

const char* CoSyncNet::GetMyName() { return s_myName.c_str(); }
void CoSyncNet::SetMyName(const std::string& n) { s_myName = n; s_mySteamID = 0; s_myEntityID = 0; }
void CoSyncNet::SetMySteamID(uint64_t sid) { s_mySteamID = sid; s_myEntityID = 0; }

bool CoSyncNet::IsHost() { return s_isHost; }
bool CoSyncNet::IsInitialized() { return s_initialized; }
bool CoSyncNet::IsSessionActive() { return s_sessionActive; }
bool CoSyncNet::IsConnected() { return s_connected; }

// ============================================================================
// TRANSPORT CALLBACKS
// ============================================================================
void CoSyncNet::OnGNSConnected()
{
    s_connected = true;
    g_CoSyncPlayerManager.SetLocalEntityID(GetMyEntityID());

    if (s_helloSent)
        return;                // <-- prevents double HELLO

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
    s_connMap.clear();
}

// ============================================================================
// SHUTDOWN
// ============================================================================
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

    s_testNpcPending = false;
    s_testNpcSpawned = false;
    s_testNpcDeadline = 0.0;
    s_testNpcLastDiag = 0.0;

    {
        std::lock_guard<std::mutex> lk(s_peerMutex);
        s_peers.clear();
        s_connMap.clear();
    }

    CoSyncTransport::Shutdown();
}
