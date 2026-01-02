#include "CoSyncPlayerManager.h"

#include "ConsoleLogger.h"
#include "CoSyncWorld.h"
#include "CoSyncSpawnTasks.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

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
// Inbox enqueue
// -----------------------------------------------------------------------------
void CoSyncPlayerManager::EnqueueEntityCreate(const EntityCreatePacket& p)
{
    std::lock_guard<std::mutex> lock(m_inboxMutex);
    m_inbox.push_back({ InboxItem::Type::Create, p, {} });

    LOG_INFO("[CoSyncPM] Enqueue CREATE id=%u baseForm=0x%08X", p.entityID, p.baseFormID);
}

void CoSyncPlayerManager::EnqueueEntityUpdate(const EntityUpdatePacket& p)
{
    std::lock_guard<std::mutex> lock(m_inboxMutex);
    m_inbox.push_back({ InboxItem::Type::Update, {}, p });

    LOG_DEBUG("[CoSyncPM] Enqueue UPDATE id=%u pos(%.1f %.1f %.1f)",
        p.entityID, p.pos.x, p.pos.y, p.pos.z);
}

// -----------------------------------------------------------------------------
// ProcessInbox
// - Drains inbox into a local queue (minimizes lock time)
// - NEVER spawns directly inside ProcessInbox
// -----------------------------------------------------------------------------
void CoSyncPlayerManager::ProcessInbox()
{
    std::deque<InboxItem> local;
    {
        std::lock_guard<std::mutex> lock(m_inboxMutex);
        local.swap(m_inbox);
    }

    if (!local.empty())
        LOG_DEBUG("[CoSyncPM] ProcessInbox drained %zu msgs", local.size());

    for (auto& item : local)
    {
        if (item.type == InboxItem::Type::Create)
            ProcessEntityCreate(item.create);
        else
            ProcessEntityUpdate(item.update);
    }
}

// -----------------------------------------------------------------------------
// Player lookup
// IMPORTANT FIX:
//   Initialize lastPacketTime when we first create a slot.
//   Otherwise, entities that never receive UPDATE would "never timeout"
//   if you use any grace logic incorrectly.
// -----------------------------------------------------------------------------
CoSyncPlayer& CoSyncPlayerManager::GetOrCreateByEntityID(uint32_t entityID)
{
    auto it = m_playersByEntityID.find(entityID);
    if (it != m_playersByEntityID.end())
        return it->second;

    auto result = m_playersByEntityID.emplace(entityID, CoSyncPlayer("Remote"));
    CoSyncPlayer& player = result.first->second;
    player.entityID = entityID;

    // Start lifetime timer immediately (prevents “immortal entities”)
    player.lastPacketTime = NowSeconds();

    LOG_INFO("[CoSyncPM] Created CoSyncPlayer slot for entityID=%u", entityID);
    return player;
}

// -----------------------------------------------------------------------------
// CREATE
// - Only CREATE can cause a spawn
// - If world isn't ready, queue to m_pendingCreates (NOT back into inbox)
// - If world is ready, queue a spawn task (deferred via task system)
// - Dedupe: don't queue duplicate pending CREATEs for the same entityID
// -----------------------------------------------------------------------------
void CoSyncPlayerManager::ProcessEntityCreate(const EntityCreatePacket& p)
{
    // Prevent proxy for local entity if configured
    if (m_localEntityID != 0 && p.entityID == m_localEntityID)
    {
        LOG_DEBUG("[CoSyncPM] CREATE ignored for local entity id=%u", p.entityID);
        return;
    }

    if (p.entityID == 0 || p.baseFormID == 0)
    {
        LOG_ERROR("[CoSyncPM] CREATE invalid (entityID=%u baseFormID=0x%08X)", p.entityID, p.baseFormID);
        return;
    }

    CoSyncPlayer& player = GetOrCreateByEntityID(p.entityID);
    if (player.hasSpawned && player.actorRef)
    {
        LOG_DEBUG("[CoSyncPM] CREATE ignored; already spawned id=%u", p.entityID);
        return;
    }

    // World not ready: store CREATE until Tick() can pump it
    if (!CoSyncWorld::IsWorldReady())
    {
        std::lock_guard<std::mutex> lock(m_spawnMutex);

        // Dedupe by entityID so we don’t stack repeated CREATEs
        for (const auto& existing : m_pendingCreates)
        {
            if (existing.entityID == p.entityID)
            {
                LOG_DEBUG("[CoSyncPM] World not ready; CREATE already pending id=%u", p.entityID);
                return;
            }
        }

        m_pendingCreates.push_back(p);

        LOG_WARN("[CoSyncPM] World not ready; deferred CREATE queued id=%u (pending=%zu)",
            p.entityID, m_pendingCreates.size());
        return;
    }

    // World ready: enqueue a spawn task (game thread safe)
    LOG_INFO("[CoSyncPM] Queue spawn id=%u baseForm=0x%08X at (%.2f %.2f %.2f)",
        p.entityID, p.baseFormID,
        p.spawnPos.x, p.spawnPos.y, p.spawnPos.z);

    CoSyncSpawnTasks::EnqueueSpawn(p.entityID, p.baseFormID, p.spawnPos, p.spawnRot);
}

// -----------------------------------------------------------------------------
// UPDATE
// - MUST NEVER spawn
// - Cache last known transform even if not spawned yet (applies after spawn)
// -----------------------------------------------------------------------------
void CoSyncPlayerManager::ProcessEntityUpdate(const EntityUpdatePacket& u)
{
    // Prevent applying updates to local entity if configured
    if (m_localEntityID != 0 && u.entityID == m_localEntityID)
        return;

    if (u.entityID == 0)
        return;

    CoSyncPlayer& player = GetOrCreateByEntityID(u.entityID);

    // Always advance lastPacketTime on UPDATE
    player.lastPacketTime = NowSeconds();

    if (!u.username.empty())
        player.username = u.username;

    // Cache transform always; ApplyPendingTransformIfAny will no-op if not spawned.
    player.pendingPos = u.pos;
    player.pendingRot = u.rot;
    player.pendingVel = u.vel;
    player.hasPendingTransform = true;

    // If actor exists, apply immediately; otherwise it will apply after spawn completes.
    player.ApplyPendingTransformIfAny();
}

// -----------------------------------------------------------------------------
// Deferred spawn pump
// - spawns at most ONE per Tick()
// - avoids spawn storms and makes frame pacing predictable
// -----------------------------------------------------------------------------
void CoSyncPlayerManager::PumpDeferredSpawns()
{
    if (!CoSyncWorld::IsWorldReady())
        return;

    EntityCreatePacket p{};
    {
        std::lock_guard<std::mutex> lock(m_spawnMutex);
        if (m_pendingCreates.empty())
            return;

        p = m_pendingCreates.front();
        m_pendingCreates.pop_front();
    }

    // Local guard
    if (m_localEntityID != 0 && p.entityID == m_localEntityID)
        return;

    if (p.entityID == 0 || p.baseFormID == 0)
        return;

    CoSyncPlayer& player = GetOrCreateByEntityID(p.entityID);
    if (player.hasSpawned && player.actorRef)
        return;

    LOG_INFO("[CoSyncPM] PumpDeferredSpawns -> EnqueueSpawn id=%u baseForm=0x%08X",
        p.entityID, p.baseFormID);

    CoSyncSpawnTasks::EnqueueSpawn(p.entityID, p.baseFormID, p.spawnPos, p.spawnRot);
}

// -----------------------------------------------------------------------------
// Tick
// - pumps deferred spawns
// - handles timeouts
// IMPORTANT FIX:
//   Do NOT use "last = now when lastPacketTime==0" style grace.
//   That creates immortal entities.
//   We set lastPacketTime on slot creation, so timeout logic is stable.
// -----------------------------------------------------------------------------
void CoSyncPlayerManager::Tick()
{
    PumpDeferredSpawns();

    const double now = NowSeconds();

    for (auto it = m_playersByEntityID.begin(); it != m_playersByEntityID.end(); )
    {
        CoSyncPlayer& rp = it->second;

        // 45s timeout like your current standard
        if ((now - rp.lastPacketTime) > 45.0)
        {
            LOG_WARN("[CoSyncPM] Remote entityID=%u timed out", rp.entityID);

            if (rp.actorRef)
            {
                // Soft remove: punt to void. Replace later with proper disable/delete.
                rp.actorRef->pos = NiPoint3{ 0.f, 0.f, -10000.f };
                rp.actorRef = nullptr;
            }

            it = m_playersByEntityID.erase(it);
            continue;
        }

        ++it;
    }
}
