#include "CoSyncPlayerManager.h"

#include "CoSyncPlayer.h"
#include "ConsoleLogger.h"
#include "CoSyncWorld.h"
#include "CoSyncSpawnTasks.h"
#include "CoSyncEntityRegistry.h"

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
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
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
    if (p.entityID == 0)
        return;

    if (p.entityID == m_localEntityID)
    {
        LOG_DEBUG("[PlayerMgr] Ignoring CREATE for local entity");
        return;
    }

    // Record CREATE into state registry (F4MP style)
    {
        CoSyncEntityState& st = GetOrCreateState(p.entityID);
        st.hasCreate = true;
        st.lastCreate = p;
        st.lastUpdateLocalTime = NowSeconds();
    }

    // Queue for deferred spawn (≤1 per tick)
    {
        std::lock_guard<std::mutex> lk(m_spawnMutex);
        m_pendingCreates.push_back(p);
    }

    LOG_INFO("[PlayerMgr] Queued CREATE for spawn entity=%u base=0x%08X",
        p.entityID, p.baseFormID);
}

// -----------------------------------------------------------------------------
// UPDATE handling (NO SPAWN EVER)
// -----------------------------------------------------------------------------
void CoSyncPlayerManager::ProcessEntityUpdate(const EntityUpdatePacket& u)
{
    if (u.entityID == 0)
        return;

    if (u.entityID == m_localEntityID)
        return;

    // Record UPDATE into state registry (F4MP style)
    {
        CoSyncEntityState& st = GetOrCreateState(u.entityID);
        st.hasUpdate = true;
        st.lastUpdate = u;
        //st.username = u.username;
        st.lastUpdateLocalTime = NowSeconds();
    }

    // Keep your existing behavior: cache update into proxy slot too.
    // (Later we can make CoSyncPlayer read from CoSyncEntityState only.)
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
        p.spawnPos,
        p.spawnRot
    );

    CoSyncPlayer& player = GetOrCreateByEntityID(p.entityID);
    player.ownerEntityID = p.ownerEntityID;

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

        if (st.lastUpdateLocalTime <= 0.0)
            continue;

        if ((now - st.lastUpdateLocalTime) > kEntityTimeoutSec)
            expired.push_back(st.entityID);
    }

    for (uint32_t entityID : expired)
        DespawnEntity(entityID, "timeout");
}
