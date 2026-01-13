#include "CoSyncSpawnTasks.h"

#include "PluginAPI.h"
#include "Tasks.h"
#include "ConsoleLogger.h"

#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncGameAPI.h"
#include "CoSyncNet.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameObjects.h"

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
static TESObjectREFR* GetAnchor()
{
    PlayerCharacter* pc = *g_player;
    return pc ? static_cast<TESObjectREFR*>(pc) : nullptr;
}

static TESNPC* ResolveNPC(UInt32 baseFormID)
{
    TESForm* f = LookupFormByID(baseFormID);
    if (!f)
        return nullptr;

    if (f->formType != TESNPC::kTypeID)
        return nullptr;

    return static_cast<TESNPC*>(f);
}

// -----------------------------------------------------------------------------
// Main-thread spawn task (F4MP-aligned)
// -----------------------------------------------------------------------------
class SpawnRemotePlayerTask final : public ITaskDelegate
{
public:
    SpawnRemotePlayerTask(
        uint32_t entityID,
        uint32_t baseFormID,
        CoSyncEntityType createType,
        uint32_t spawnFlags,
        const NiPoint3& spawnPos,
        const NiPoint3& spawnRot)
        : m_entityID(entityID)
        , m_baseFormID(baseFormID)
        , m_createType(createType)
        , m_spawnFlags(spawnFlags)
        , m_spawnPos(spawnPos)
        , m_spawnRot(spawnRot)
    {
    }

    void Run() override
    {
        LOG_DEBUG(
            "[SpawnTask] Run entity=%u base=0x%08X type=%u flags=0x%08X",
            m_entityID,
            m_baseFormID,
            static_cast<uint32_t>(m_createType),
            m_spawnFlags
        );

        CoSyncPlayer& pl =
            g_CoSyncPlayerManager.GetOrCreateByEntityID(m_entityID);

        // -----------------------------------------------------------------
        // Safety: never double-spawn
        // -----------------------------------------------------------------
        if (pl.hasSpawned && pl.actorRef)
            return;

        // -----------------------------------------------------------------
        // Resolve NPC base (players still use NPC base internally)
        // -----------------------------------------------------------------
        TESNPC* npcBase = ResolveNPC(m_baseFormID);
        if (!npcBase)
        {
            LOG_ERROR(
                "[SpawnTask] BaseForm 0x%08X is not TESNPC (entity=%u)",
                m_baseFormID,
                m_entityID
            );
            return;
        }

        TESObjectREFR* anchor = GetAnchor();
        if (!anchor)
        {
            LOG_WARN("[SpawnTask] Anchor missing (player not ready?)");
            return;
        }

        // -----------------------------------------------------------------
        // Spawn actor
        // -----------------------------------------------------------------
        if (!pl.SpawnInWorld(anchor, npcBase))
        {
            LOG_ERROR(
                "[SpawnTask] SpawnInWorld FAILED entity=%u",
                m_entityID
            );
            return;
        }

        // -----------------------------------------------------------------
        // Initial CREATE placement (single snap)
        // -----------------------------------------------------------------
        CoSyncGameAPI::PositionRemoteActor(
            pl.actorRef,
            m_spawnPos,
            m_spawnRot
        );

        // -----------------------------------------------------------------
        // AI CONTROL RULES (FINAL / LOCKED)
        //
        //  ✔ NPC ONLY
        //  ✔ HOST ONLY
        //  ✔ CREATE-TIME ONLY
        //  ✔ Controlled solely by spawnFlags
        // -----------------------------------------------------------------

        // Players NEVER have AI disabled
        if (m_createType == CoSyncEntityType::Player)
        {
            LOG_DEBUG(
                "[SpawnTask] Entity=%u is Player — AI untouched",
                m_entityID
            );
        }
        else if (
            !CoSyncNet::IsHost() &&                  // ✅ clients only
            m_createType == CoSyncEntityType::NPC &&
            EntityCreatePacket::HasFlag(
                m_spawnFlags,
                EntityCreatePacket::RemoteControlled))
        {
            LOG_INFO("[SpawnTask] Disabling AI for remote-controlled NPC entity=%u (client)", m_entityID);
            CoSyncGameAPI::DisableActorAI(pl.actorRef);
        }
        else if (
            CoSyncNet::IsHost() &&
            m_createType == CoSyncEntityType::NPC)
        {
            LOG_INFO("[SpawnTask] Host NPC entity=%u (AI remains enabled)", m_entityID);
        }


        LOG_INFO(
            "[SpawnTask] Spawn DONE entity=%u actor=%p",
            m_entityID,
            pl.actorRef
        );
    }

private:
    uint32_t m_entityID;
    uint32_t m_baseFormID;
    CoSyncEntityType m_createType;
    uint32_t m_spawnFlags;
    NiPoint3 m_spawnPos;
    NiPoint3 m_spawnRot;
};

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
void CoSyncSpawnTasks::Init(const F4SETaskInterface* taskInterface)
{
    s_taskIFace = taskInterface;
    LOG_INFO(
        "[CoSyncSpawnTasks] Init taskIFace=%p",
        taskInterface
    );
}

void CoSyncSpawnTasks::EnqueueSpawn(
    uint32_t entityID,
    uint32_t baseFormID,
    CoSyncEntityType createType,
    uint32_t spawnFlags,
    const NiPoint3& spawnPos,
    const NiPoint3& spawnRot)
{
    if (!s_taskIFace || !s_taskIFace->AddTask)
    {
        LOG_ERROR(
            "[CoSyncSpawnTasks] Task interface unavailable"
        );
        return;
    }

    s_taskIFace->AddTask(
        new SpawnRemotePlayerTask(
            entityID,
            baseFormID,
            createType,
            spawnFlags,
            spawnPos,
            spawnRot
        )
    );
}
