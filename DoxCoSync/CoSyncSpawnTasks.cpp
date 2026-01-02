#include "CoSyncSpawnTasks.h"

#include "PluginAPI.h"          // F4SETaskInterface
#include "Tasks.h"              // ITaskDelegate

#include "ConsoleLogger.h"
#include "CoSyncPlayerManager.h"

#include "Relocation.h"
#include "GameReferences.h"
#include "GameForms.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// Provided by F4SE
extern RelocPtr<PlayerCharacter*> g_player;

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------
static const F4SETaskInterface* s_taskIFace = nullptr;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static double NowSeconds()
{
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

static TESObjectREFR* GetAnchor()
{
    PlayerCharacter* pc = *g_player;
    return pc ? static_cast<TESObjectREFR*>(pc) : nullptr;
}

// -----------------------------------------------------------------------------
// Main-thread spawn task
// -----------------------------------------------------------------------------
class SpawnRemotePlayerTask final : public ITaskDelegate
{
public:
    SpawnRemotePlayerTask(
        uint32_t entityID,
        uint32_t baseFormID,
        const NiPoint3& pos,
        const NiPoint3& rot)
        : m_entityID(entityID)
        , m_baseFormID(baseFormID)
        , m_pos(pos)
        , m_rot(rot)
    {
    }

    void Run() override
    {
        LOG_DEBUG("[SpawnTask] Run() main-thread id=%u", m_entityID);

        CoSyncPlayer& pl = g_CoSyncPlayerManager.GetOrCreateByEntityID(m_entityID);

        if (pl.hasSpawned && pl.actorRef)
        {
            LOG_DEBUG("[SpawnTask] Already spawned id=%u", m_entityID);
            return;
        }

        TESForm* baseForm = LookupFormByID(m_baseFormID);
        if (!baseForm)
        {
            LOG_ERROR(
                "[SpawnTask] BaseForm not found 0x%08X id=%u",
                m_baseFormID,
                m_entityID
            );
            return;
        }

        TESObjectREFR* anchor = GetAnchor();
        if (!anchor)
        {
            // World readiness & retry logic is handled by PlayerManager
            LOG_WARN("[SpawnTask] Anchor missing; abort spawn id=%u", m_entityID);
            return;
        }

        LOG_INFO(
            "[SpawnTask] SpawnInWorld START id=%u baseForm=0x%08X",
            m_entityID,
            m_baseFormID
        );

        if (!pl.SpawnInWorld(anchor, baseForm))
        {
            LOG_ERROR("[SpawnTask] SpawnInWorld FAILED id=%u", m_entityID);
            return;
        }

        // Apply initial transform AFTER spawn (F4MP behavior)
        pl.lastPacketTime = NowSeconds();
        pl.pendingPos = m_pos;
        pl.pendingRot = m_rot;
        pl.hasPendingTransform = true;
        pl.ApplyPendingTransformIfAny();

        LOG_INFO(
            "[SpawnTask] SpawnInWorld DONE id=%u actor=%p",
            m_entityID,
            pl.actorRef
        );
    }

private:
    uint32_t m_entityID = 0;
    uint32_t m_baseFormID = 0;
    NiPoint3 m_pos{ 0.f, 0.f, 0.f };
    NiPoint3 m_rot{ 0.f, 0.f, 0.f };
};

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
void CoSyncSpawnTasks::Init(const F4SETaskInterface* taskInterface)
{
    s_taskIFace = taskInterface;
    LOG_INFO("[CoSyncSpawnTasks] Init taskIFace=%p", taskInterface);
}

void CoSyncSpawnTasks::EnqueueSpawn(
    uint32_t entityID,
    uint32_t baseFormID,
    const NiPoint3& spawnPos,
    const NiPoint3& spawnRot)
{
    if (!s_taskIFace || !s_taskIFace->AddTask)
    {
        LOG_ERROR(
            "[CoSyncSpawnTasks] Task interface unavailable; cannot spawn id=%u",
            entityID
        );
        return;
    }

    LOG_DEBUG(
        "[CoSyncSpawnTasks] Queue spawn id=%u baseForm=0x%08X",
        entityID,
        baseFormID
    );

    // F4SE deletes the task after Run()
    s_taskIFace->AddTask(
        new SpawnRemotePlayerTask(entityID, baseFormID, spawnPos, spawnRot)
    );
}
