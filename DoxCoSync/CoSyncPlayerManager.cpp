#include "CoSyncPlayerManager.h"

#include "CoSyncTransport.h"
#include "CoSyncPlayer.h"
#include "ConsoleLogger.h"
#include "CoSyncWorld.h"
#include "CoSyncSpawnTasks.h"
#include "EntitySerialization.h"
#include "CoSyncEntityRegistry.h"
#include "CoSyncEntityTypes.h"
#include "CoSyncNet.h"
#include "CoSyncGameAPI.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <algorithm>
#include <vector>

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------
CoSyncPlayerManager g_CoSyncPlayerManager;


static constexpr uint32_t kLocalRemotePlayerBaseFormID = 0x01001ECC;

// -----------------------------------------------------------------------------
// Host-only debug NPC (authority validation scaffold)
// -----------------------------------------------------------------------------
static constexpr uint32_t kDebugNpcEntityID = 36865;
//static constexpr uint32_t kDebugNpcBaseFormID = 0x01001ECC; // your new CK form


// -----------------------------------------------------------------------------
// Time helper (QPC)
// -----------------------------------------------------------------------------
static double NowSeconds()
{
    static double s_freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
        }();

    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / s_freq;
}

// -----------------------------------------------------------------------------
// Host-only debug NPC helpers
// -----------------------------------------------------------------------------
static EntityCreatePacket MakeHostDebugNpcCreate(uint32_t localEntityID)
{
    EntityCreatePacket p{};

    p.entityID = kDebugNpcEntityID;
    p.ownerEntityID = localEntityID; // host-owned
   // p.baseFormID = //kDebugNpcBaseFormID; //Kinda Broken ATM
    p.type = CoSyncEntityType::NPC;

    // Must be RemoteControlled per debug NPC rules
    p.spawnFlags = EntityCreatePacket::RemoteControlled;

    // NiPoint3 has NO default constructor — explicit init
    p.spawnPos = NiPoint3(0.f, 0.f, 0.f);
    p.spawnRot = NiPoint3(0.f, 0.f, 0.f);

    return p;
}

// Finds a single client player proxy to snap to (first remote Player proxy found).
static CoSyncPlayer* FindAnyRemoteClientPlayerProxy(
    std::unordered_map<uint32_t, std::unique_ptr<CoSyncPlayer>>& playersByEntityID,
    std::unordered_map<uint32_t, CoSyncEntityState>& statesByEntityID,
    uint32_t localEntityID)
{
    for (auto& kv : playersByEntityID)
    {
        CoSyncPlayer& pl = *kv.second;
        if (!pl.hasSpawned || !pl.actorRef)
            continue;

        if (pl.entityID == 0 || pl.entityID == localEntityID)
            continue;

        auto it = statesByEntityID.find(pl.entityID);
        if (it == statesByEntityID.end() || !it->second.hasCreate)
            continue;

        if (it->second.lastCreate.type != CoSyncEntityType::Player)
            continue;

        return &pl;
    }

    return nullptr;
}

// -----------------------------------------------------------------------------
// Network entry points
// -----------------------------------------------------------------------------
void CoSyncPlayerManager::EnqueueEntityCreate(const EntityCreatePacket& p)
{
    // Ignore self CREATE immediately
    if (p.entityID != 0 && p.entityID == m_localEntityID)
    {
        LOG_INFO("[PlayerMgr] Ignoring CREATE for local entity=%u", p.entityID);
        return;
    }

    // Debug NPC is host-only and must never exist on clients
    if (p.entityID == kDebugNpcEntityID && !CoSyncNet::IsHost())
        return;

    std::lock_guard<std::mutex> lk(m_inboxMutex);

    InboxItem item{};
    item.type = InboxItem::Type::Create;
    item.create = p;

    m_inbox.push_back(item);
    LOG_INFO("[PlayerMgr] Enqueued CREATE entity=%u", p.entityID);
}

void CoSyncPlayerManager::EnqueueEntityUpdate(const EntityUpdatePacket& p)
{
    // Debug NPC is host-only and must never exist on clients
    if (p.entityID == kDebugNpcEntityID && !CoSyncNet::IsHost())
        return;

    LOG_DEBUG("[PlayerMgr] Enqueued UPDATE entity=%u", p.entityID);

    std::lock_guard<std::mutex> lk(m_inboxMutex);

    InboxItem item{};
    item.type = InboxItem::Type::Update;
    item.update = p;

    m_inbox.push_back(item);
}

void CoSyncPlayerManager::EnqueueEntityDestroy(const EntityDestroyPacket& p)
{
    if (p.entityID == 0 || p.entityID == m_localEntityID)
        return;

    // Debug NPC is host-only and must never exist on clients
    if (p.entityID == kDebugNpcEntityID && !CoSyncNet::IsHost())
        return;

    std::lock_guard<std::mutex> lk(m_inboxMutex);

    InboxItem item{};
    item.type = InboxItem::Type::Destroy;
    item.destroy = p;

    m_inbox.push_back(item);
    LOG_INFO("[PlayerMgr] Enqueued DESTROY entity=%u", p.entityID);
}

// -----------------------------------------------------------------------------
// Inbox processing (GAME THREAD ONLY)
// -----------------------------------------------------------------------------
void CoSyncPlayerManager::ProcessInbox()
{
    std::deque<InboxItem> inbox;

    {
        std::lock_guard<std::mutex> lk(m_inboxMutex);
        if (m_inbox.empty())
            return;

        inbox.swap(m_inbox);
    }

    for (auto& item : inbox)
    {
        switch (item.type)
        {
        case InboxItem::Type::Create:
            ProcessEntityCreate(item.create);
            break;

        case InboxItem::Type::Update:
            ProcessEntityUpdate(item.update);
            break;

        case InboxItem::Type::Destroy:
            ProcessEntityDestroy(item.destroy);
            break;

        default:
            break;
        }
    }
}

// -----------------------------------------------------------------------------
// State registry (network-only)
// -----------------------------------------------------------------------------
CoSyncEntityState& CoSyncPlayerManager::GetOrCreateState(uint32_t entityID)
{
    auto it = m_statesByEntityID.find(entityID);
    if (it != m_statesByEntityID.end())
        return it->second;

    CoSyncEntityState st{};
    st.entityID = entityID;

    // C++14-safe (no structured bindings)
    auto result = m_statesByEntityID.emplace(entityID, std::move(st));
    return result.first->second;
}

// -----------------------------------------------------------------------------
// Proxy registry (world)
// -----------------------------------------------------------------------------
CoSyncPlayer& CoSyncPlayerManager::GetOrCreateByEntityID(uint32_t entityID)
{
    auto it = m_playersByEntityID.find(entityID);
    if (it != m_playersByEntityID.end())
        return *it->second;

    auto player = std::make_unique<CoSyncPlayer>("Remote");
    player->entityID = entityID;

    CoSyncPlayer& ref = *player;
    m_playersByEntityID.emplace(entityID, std::move(player));

    LOG_INFO("[PlayerMgr] Created CoSyncPlayer entity=%u", entityID);
    return ref;
}

// -----------------------------------------------------------------------------
// CREATE handling (NO SPAWN HERE; only queues)
// -----------------------------------------------------------------------------
void CoSyncPlayerManager::ProcessEntityCreate(const EntityCreatePacket& p)
{
    if (p.entityID == 0 || p.entityID == m_localEntityID)
        return;

    // Debug NPC is host-only and must never exist on clients
    if (p.entityID == kDebugNpcEntityID && !CoSyncNet::IsHost())
        return;

    CoSyncEntityState& st = GetOrCreateState(p.entityID);

    // Local ownership means: "this entity is controlled by me"
    st.isLocallyOwned = (p.ownerEntityID != 0 && p.ownerEntityID == m_localEntityID);

    // Always remember last CREATE
    st.lastCreate = p;
    st.hasCreate = true;
    st.lastUpdateLocalTime = NowSeconds(); // treat CREATE as “alive” signal

    // NPC detection
    st.isNPC = (p.type == CoSyncEntityType::NPC);

    // Authority rule
    st.hostAuthoritative = st.isNPC && CoSyncNet::IsHost();

    // Seed proxy metadata + authoritative transform immediately
    CoSyncPlayer& player = GetOrCreateByEntityID(p.entityID);
    player.ownerEntityID = p.ownerEntityID;
    player.createType = p.type;
    player.isRemoteControlled =
        EntityCreatePacket::HasFlag(p.spawnFlags, EntityCreatePacket::RemoteControlled);

    player.authoritativePos = p.spawnPos;
    player.authoritativeRot = p.spawnRot;

    player.pendingPos = p.spawnPos;
    player.pendingRot = p.spawnRot;
    player.hasPendingTransform = true;

    // Queue spawn only once (avoid duplicates)
    if (!st.spawnQueued)
    {
        st.spawnQueued = true;
        std::lock_guard<std::mutex> lk(m_spawnMutex);
        m_pendingCreates.push_back(p);

        LOG_INFO("[PlayerMgr] Queued CREATE for spawn entity=%u base=0x%08X",
            p.entityID, p.baseFormID);
    }
}

// -----------------------------------------------------------------------------
// UPDATE handling (NO SPAWN EVER)
// -----------------------------------------------------------------------------
void CoSyncPlayerManager::ProcessEntityUpdate(const EntityUpdatePacket& u)
{
    if (u.entityID == 0 || u.entityID == m_localEntityID)
        return;

    // Debug NPC is host-only and must never exist on clients
    if (u.entityID == kDebugNpcEntityID && !CoSyncNet::IsHost())
        return;

    CoSyncEntityState& st = GetOrCreateState(u.entityID);

    // Update “alive” time even if create hasn’t arrived yet (good for re-ordering)
    st.lastUpdateLocalTime = NowSeconds();
    st.lastUpdate = u;

    if (!st.hasCreate)
        return; // keep state only until CREATE arrives

    // Strict authority: host never accepts incoming UPDATEs for NPCs
    if (st.isNPC && CoSyncNet::IsHost())
        return;

    CoSyncPlayer& player = GetOrCreateByEntityID(u.entityID);
    player.ApplyUpdate(u);
}

// -----------------------------------------------------------------------------
// DESTROY handling (NO SPAWN EVER)
// -----------------------------------------------------------------------------
void CoSyncPlayerManager::ProcessEntityDestroy(const EntityDestroyPacket& d)
{
    if (d.entityID == 0 || d.entityID == m_localEntityID)
        return;

    // Debug NPC is host-only and must never exist on clients
    if (d.entityID == kDebugNpcEntityID && !CoSyncNet::IsHost())
        return;

    DespawnEntity(d.entityID, "network destroy");
}

void CoSyncPlayerManager::DespawnEntity(uint32_t entityID, const char* reason)
{
    if (entityID == 0 || entityID == m_localEntityID)
        return;

    {
        std::lock_guard<std::mutex> lk(m_spawnMutex);
        m_pendingCreates.erase(
            std::remove_if(
                m_pendingCreates.begin(),
                m_pendingCreates.end(),
                [entityID](const EntityCreatePacket& p)
                {
                    return p.entityID == entityID;
                }),
            m_pendingCreates.end());
    }

    g_CoSyncEntities.Remove(entityID);

    auto itPlayer = m_playersByEntityID.find(entityID);
    if (itPlayer != m_playersByEntityID.end())
    {
        LOG_INFO("[PlayerMgr] Despawn entity=%u actor=%p (%s)",
            entityID, itPlayer->second ? itPlayer->second->actorRef : nullptr, reason);
        m_playersByEntityID.erase(itPlayer);
    }

    auto itState = m_statesByEntityID.find(entityID);
    if (itState != m_statesByEntityID.end())
        m_statesByEntityID.erase(itState);
}

// -----------------------------------------------------------------------------
// Deferred spawning (≤ 1 per tick — F4MP RULE)
// -----------------------------------------------------------------------------
void CoSyncPlayerManager::PumpDeferredSpawns()
{
    static bool s_warned = false;

    if (!CoSyncWorld::IsWorldReady())
    {
        if (!s_warned)
        {
            s_warned = true;
            LOG_WARN("[PlayerMgr] World not ready; deferring spawns");
        }
        return;
    }

    EntityCreatePacket p{};
    {
        std::lock_guard<std::mutex> lk(m_spawnMutex);
        if (m_pendingCreates.empty())
            return;

        p = m_pendingCreates.front();
        m_pendingCreates.pop_front();
    }

    LOG_INFO("[PlayerMgr] Spawning entity=%u base=0x%08X",
        p.entityID, p.baseFormID);

    CoSyncSpawnTasks::EnqueueSpawn(
        p.entityID,
        p.baseFormID,
        p.type,
        p.spawnFlags,
        p.spawnPos,
        p.spawnRot
    );

    // Maintain ownership on proxy immediately (metadata; spawn occurs in task)
    CoSyncPlayer& player = GetOrCreateByEntityID(p.entityID);
    player.ownerEntityID = p.ownerEntityID;
}

// -----------------------------------------------------------------------------
// Host NPC replication sender (F4MP-aligned)
// -----------------------------------------------------------------------------
void CoSyncPlayerManager::HostSendNpcUpdates(double now)
{
    if (!CoSyncNet::IsHost() || !CoSyncNet::IsConnected())
        return;

    static double s_lastSend = 0.0;
    if ((now - s_lastSend) < 0.05) // 20 Hz
        return;
    s_lastSend = now;

    for (auto& kv : m_playersByEntityID)
    {
        CoSyncPlayer& npc = *kv.second;

        if (npc.entityID == kDebugNpcEntityID)
            continue;

        if (!npc.hasSpawned || !npc.actorRef)
            continue;

        auto it = m_statesByEntityID.find(npc.entityID);
        if (it == m_statesByEntityID.end() || !it->second.hasCreate)
            continue;

        const CoSyncEntityState& st = it->second;

        if (st.lastCreate.type != CoSyncEntityType::NPC)
            continue;

        if (!st.hostAuthoritative)
            continue;

        if (!EntityCreatePacket::HasFlag(st.lastCreate.spawnFlags, EntityCreatePacket::RemoteControlled))
            continue;

        NiPoint3 pos(0.f, 0.f, 0.f);
        NiPoint3 rot(0.f, 0.f, 0.f);

        if (!CoSyncGameAPI::GetActorWorldTransform(npc.actorRef, pos, rot))
            continue;

        EntityUpdatePacket u{};
        u.entityID = npc.entityID;
        u.pos = pos;
        u.rot = rot;
        u.vel = NiPoint3(0.f, 0.f, 0.f);
        u.timestamp = now;

        CoSyncTransport::Send(SerializeEntityUpdate(u));
    }
}

// -----------------------------------------------------------------------------
// Host-only debug NPC hard snap (NO SMOOTHING, NO INTERP, NO VELOCITY)
// -----------------------------------------------------------------------------
static void HostDebugNpcHardSnap(
    std::unordered_map<uint32_t, std::unique_ptr<CoSyncPlayer>>& playersByEntityID,
    std::unordered_map<uint32_t, CoSyncEntityState>& statesByEntityID,
    uint32_t localEntityID)
{
    // Find a remote client player proxy to follow
    CoSyncPlayer* remotePlayer = FindAnyRemoteClientPlayerProxy(
        playersByEntityID,
        statesByEntityID,
        localEntityID);

    if (!remotePlayer || !remotePlayer->actorRef)
        return;

    auto itNpc = playersByEntityID.find(kDebugNpcEntityID);
    if (itNpc == playersByEntityID.end())
        return;

    CoSyncPlayer& npc = *itNpc->second;

    // Absolute guards: debug NPC only, must be spawned, must have actor
    if (npc.entityID != kDebugNpcEntityID)
        return;
    if (!npc.hasSpawned || !npc.actorRef)
        return;

    NiPoint3 clientPos(0.f, 0.f, 0.f);
    NiPoint3 clientRot(0.f, 0.f, 0.f);

    if (!CoSyncGameAPI::GetActorWorldTransform(remotePlayer->actorRef, clientPos, clientRot))
        return;

    npc.authoritativePos = clientPos;
    npc.authoritativeRot = clientRot;

    CoSyncGameAPI::PositionRemoteActor(npc.actorRef, clientPos, clientRot);
}

// -----------------------------------------------------------------------------
// Per-frame tick (GAME THREAD)
// -----------------------------------------------------------------------------
void CoSyncPlayerManager::Tick()
{
    ProcessInbox();

    // Host-only: seed/queue the debug NPC CREATE so it spawns locally via normal queue rules
    if (CoSyncNet::IsHost() &&
        CoSyncNet::IsConnected() &&
        CoSyncWorld::IsWorldReady())
    {
        CoSyncEntityState& dbgSt = GetOrCreateState(kDebugNpcEntityID);

        if (!dbgSt.hasCreate)
        {
            const EntityCreatePacket dbgCreate =
                MakeHostDebugNpcCreate(m_localEntityID);

            ProcessEntityCreate(dbgCreate); // queues spawn only
        }
    }

    PumpDeferredSpawns();

    const double now = NowSeconds();
    HandleTimeouts(now);

    // Host-only: replicate real NPC transforms
    HostSendNpcUpdates(now);

    // Host-only: once debug NPC is spawned, DISABLE AI on host 
    if (CoSyncNet::IsHost() && CoSyncNet::IsConnected())
    {
       

        

        
    }

    for (auto& kv : m_playersByEntityID)
    {
        CoSyncPlayer& ent = *kv.second;

        if (!ent.hasSpawned)
            continue;

        ent.ApplyPendingTransformIfAny();

        // NPCs must NEVER be smoothed/interpolated
        auto it = m_statesByEntityID.find(ent.entityID);
        if (it != m_statesByEntityID.end() && it->second.hasCreate)
        {
            if (it->second.lastCreate.type == CoSyncEntityType::NPC)
                continue;
        }

        ent.TickSmoothing(now);
    }
}

void CoSyncPlayerManager::HandleTimeouts(double now)
{
    constexpr double kEntityTimeoutSec = 15.0;

    std::vector<uint32_t> expired;
    expired.reserve(m_statesByEntityID.size());

    for (const auto& kv : m_statesByEntityID)
    {
        const CoSyncEntityState& st = kv.second;

        if (st.entityID == 0 || st.entityID == m_localEntityID)
            continue;

        if (st.entityID == kDebugNpcEntityID)
            continue;

        if (!st.isLocallyOwned)
            continue;

        if (st.isNPC && CoSyncNet::IsHost())
            continue;

        if (st.lastUpdateLocalTime <= 0.0)
            continue;

        if ((now - st.lastUpdateLocalTime) > kEntityTimeoutSec)
            expired.push_back(st.entityID);
    }

    for (uint32_t entityID : expired)
        DespawnEntity(entityID, "timeout");
}
