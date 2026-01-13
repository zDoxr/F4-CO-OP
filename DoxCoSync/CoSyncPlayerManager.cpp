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

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <algorithm>
#include <vector>

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------
CoSyncPlayerManager g_CoSyncPlayerManager;

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

    std::lock_guard<std::mutex> lk(m_inboxMutex);

    InboxItem item{};
    item.type = InboxItem::Type::Create;
    item.create = p;

    m_inbox.push_back(item);
    LOG_INFO("[PlayerMgr] Enqueued CREATE entity=%u", p.entityID);
}

void CoSyncPlayerManager::EnqueueEntityUpdate(const EntityUpdatePacket& p)
{
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

    auto [insIt, _] = m_statesByEntityID.emplace(entityID, std::move(st));
    return insIt->second;
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

    CoSyncEntityState& st = GetOrCreateState(p.entityID);

    st.isLocallyOwned = (p.ownerEntityID == m_localEntityID);

    // Always remember last CREATE
    st.lastCreate = p;
    st.hasCreate = true;
    st.lastUpdateLocalTime = NowSeconds(); // treat CREATE as “alive” signal

    // NPC detection
    st.isNPC = (p.type == CoSyncEntityType::NPC);


    // Authority rule
    st.hostAuthoritative = st.isNPC && CoSyncNet::IsHost();

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

    CoSyncEntityState& st = GetOrCreateState(u.entityID);

    // Update “alive” time even if create hasn’t arrived yet (good for re-ordering)
    st.lastUpdateLocalTime = NowSeconds();
    st.lastUpdate = u;

    if (!st.hasCreate)
        return; // keep state only until CREATE arrives

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

    CoSyncPlayer& player = GetOrCreateByEntityID(p.entityID);
    player.ownerEntityID = p.ownerEntityID;

}



void CoSyncPlayerManager::HostSendNpcUpdates(double now)
{
    // host-only safeguard (callers can also gate)
    if (!CoSyncNet::IsHost())
        return;

    static double s_lastSend = 0.0;
    if (now - s_lastSend < 0.05) // 20 Hz
        return;
    s_lastSend = now;

    for (auto& kv : m_playersByEntityID)
    {
        CoSyncPlayer& pl = *kv.second;
        if (!pl.hasSpawned || !pl.actorRef)
            continue;

        // Determine entity type from state (authoritative)
        auto it = m_statesByEntityID.find(pl.entityID);
        if (it == m_statesByEntityID.end() || !it->second.hasCreate)
            continue;

        const CoSyncEntityState& st = it->second;
        if (st.lastCreate.type != CoSyncEntityType::NPC)
            continue;

        // Only send for NPCs the host is simulating.
        // Your test NPC uses RemoteControlled as "network-controlled on clients".
        if (!EntityCreatePacket::HasFlag(st.lastCreate.spawnFlags, EntityCreatePacket::RemoteControlled))
            continue;

        EntityUpdatePacket u{};
        u.entityID = pl.entityID;
        u.pos = pl.actorRef->pos;
        u.rot = pl.actorRef->rot;
        u.vel = NiPoint3{ 0.f, 0.f, 0.f }; // optional; can compute later
        u.timestamp = now;

        CoSyncTransport::Send(SerializeEntityUpdate(u));

        // Refresh local liveness so host doesn’t timeout its own NPC state
        EnqueueEntityUpdate(u);
    }
}








// -----------------------------------------------------------------------------
// Per-frame tick (GAME THREAD)
// -----------------------------------------------------------------------------
void CoSyncPlayerManager::Tick()
{
	ProcessInbox();
    PumpDeferredSpawns();

    // Apply transforms + smoothing
    const double now = NowSeconds();
    HandleTimeouts(now);

    for (auto& kv : m_playersByEntityID)
    {
        CoSyncPlayer& player = *kv.second;

        if (!player.hasSpawned)
            continue;

        player.ApplyPendingTransformIfAny();
        player.TickSmoothing(now);
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

        // CRITICAL FIX:
        if (!st.isLocallyOwned)
            continue; // never timeout remote-owned entities

        if (st.isNPC && CoSyncNet::IsHost())
            continue; // host owns NPC lifetime


        if (st.lastUpdateLocalTime <= 0.0)
            continue;

        if ((now - st.lastUpdateLocalTime) > kEntityTimeoutSec)
            expired.push_back(st.entityID);
    }


    for (uint32_t entityID : expired)
        DespawnEntity(entityID, "timeout");
}
