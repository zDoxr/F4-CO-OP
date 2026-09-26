


#include "CoSyncSpawnTasks.h"

#include "CoSyncRequirementsLog.h"
#include "PluginAPI.h"
#include "Tasks.h"
#include "ConsoleLogger.h"

#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncGameAPI.h"
#include "CoSyncNpcWorldSync.h"
#include "CoSyncNet.h"
#include "CoSyncPapyrusHelper.h"
#include "TickHook.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "CoSyncProxyLooks.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>


extern RelocPtr<PlayerCharacter*> g_player;
extern PlayerCharacter* CoSyncGetPlayer();

static const F4SETaskInterface* s_taskIFace = nullptr;

static TESObjectREFR* GetAnchor()
{
    LOG_REQUIREMENT("CoSyncSpawnTasks::GetAnchor", "Needs a save to already be loaded, so there is a real player to anchor to. The caller must check the result for null.");
    PlayerCharacter* pc = CoSyncGetPlayer();
    return pc ? static_cast<TESObjectREFR*>(pc) : nullptr;
}

static TESNPC* ResolveNPC(UInt32 baseFormID)
{
    LOG_REQUIREMENT("CoSyncSpawnTasks::ResolveNPC", "Needs a real, non-zero base form ID that actually looks up to an NPC record. Guarded against a crash if the lookup returns a bad pointer.");
    LOG_INFO("[SpawnTask] ResolveNPC START: baseFormID=0x%08X", baseFormID);

    if (baseFormID == 0)
    {
        LOG_ERROR("[SpawnTask] ResolveNPC: baseFormID is 0!");
        return nullptr;
    }

    __try
    {
        TESForm* f = LookupFormByID(baseFormID);
        if (!f)
        {
            LOG_ERROR("[SpawnTask] ResolveNPC: LookupFormByID returned NULL for 0x%08X", baseFormID);
            return nullptr;
        }

        LOG_INFO("[SpawnTask] ResolveNPC: Form found, formType=%u (expecting %u for TESNPC)",
            f->formType, TESNPC::kTypeID);

        if (f->formType != TESNPC::kTypeID)
        {
            LOG_ERROR("[SpawnTask] ResolveNPC: Form 0x%08X is not TESNPC (type=%u)",
                baseFormID, f->formType);
            return nullptr;
        }

        TESNPC* npc = static_cast<TESNPC*>(f);
        LOG_INFO("[SpawnTask] ResolveNPC SUCCESS: npc=0x%p", npc);
        return npc;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[SpawnTask] ResolveNPC CRASHED! Exception: 0x%08X baseFormID=0x%08X",
            GetExceptionCode(), baseFormID);
        return nullptr;
    }
}

static uint32_t ResolveBaseForSpawn(uint32_t baseFormID, CoSyncEntityType createType, uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncSpawnTasks::ResolveBaseForSpawn", "For a player entity with no base form ID, needs the remote player's base form lookup to succeed. If that lookup fails or crashes, it falls back to the game's default player NPC (form 0x7).");
    LOG_INFO("[SpawnTask] ResolveBaseForSpawn START: entity=%u baseFormID=0x%08X type=%d",
        entityID, baseFormID, (int)createType);


    if (createType == CoSyncEntityType::Player)
    {
        if (baseFormID == 0)
        {
            LOG_INFO("[SpawnTask] Player entity with baseFormID=0, resolving locally...");


            // Phase 7d: every remote player gets their OWN proxy base (one of
            // CoSync.esp's seven RMPlayer records) carrying their face/body.
            if (const uint32_t slotBase = CoSyncProxyLooks::BaseFormForEntity(entityID))
            {
                if (TESNPC* npc = ResolveNPC(slotBase))
                {
                    CoSyncProxyLooks::ApplyToBaseBeforeSpawn(entityID, npc);
                    return slotBase;
                }
            }

            __try
            {
                LOG_INFO("[SpawnTask] Calling CoSyncPapyrusHelper::GetRemotePlayerBase()...");
                TESNPC* playerBase = CoSyncPapyrusHelper::GetRemotePlayerBase();

                if (!playerBase)
                {
                    LOG_ERROR("[SpawnTask] GetRemotePlayerBase() returned NULL!");
                    LOG_ERROR("[SpawnTask] >>> CoSync.esp is MISSING or NOT LOADED on THIS machine — "
                              "the remote player will use the default Player NPC (0x7): wrong look, "
                              "no essential flag, broken equips. Install/enable CoSync.esp here! <<<");
                    LOG_WARN("[SpawnTask] Using FALLBACK player base 0x00000007");
                    return 0x00000007;
                }

                uint32_t resolvedFormID = playerBase->formID;
                LOG_INFO("[SpawnTask] Successfully resolved to 0x%08X", resolvedFormID);
                return resolvedFormID;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                LOG_ERROR("[SpawnTask] CRASH in GetRemotePlayerBase()! Exception: 0x%08X",
                    GetExceptionCode());
                LOG_WARN("[SpawnTask] Using EMERGENCY FALLBACK player base 0x00000007");
                return 0x00000007;
            }
        }

        LOG_INFO("[SpawnTask] Player entity with baseFormID=0x%08X, using as-is", baseFormID);
        return baseFormID;
    }


    // Round 138: NPC-type spawns use the REAL NPC base from the packet when
    // it resolves locally — the old unconditional override to the RMPlayer
    // proxy body is exactly what produced the "phantom human standing on
    // Codsworth" ghosts (round 135). This path now only runs for the client
    // spawn-fallback (a world NPC the client's own save doesn't have loaded),
    // and the clone must look like the actual NPC.
    if (baseFormID != 0)
    {
        TESNPC* realBase = ResolveNPC(baseFormID);
        if (realBase)
        {
            LOG_INFO("[SpawnTask] NPC entity, using REAL base 0x%08X from packet", baseFormID);
            return baseFormID;
        }
        LOG_UPDATED_WARN("[SpawnTask] NPC base 0x%08X not resolvable on this machine — NOT spawning a proxy-bodied stand-in",
            baseFormID);
    }
    return 0;
}

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
        LOG_REQUIREMENT("SpawnRemotePlayerTask::SpawnRemotePlayerTask", "Just stores the arguments it is given. No game state involved.");
        LOG_INFO("[SpawnTask] Constructor: entity=%u base=0x%08X", entityID, baseFormID);
    }

    void Run() override
    {
        LOG_REQUIREMENT("SpawnRemotePlayerTask::Run", "Needs a loaded player to anchor to, a valid NPC base form resolved, and the actual spawn to succeed -- each step is checked before moving to the next one.");
        LOG_INFO(
            "[SpawnTask] Run START entity=%u base=0x%08X type=%s flags=0x%08X spawnPos=(%.1f,%.1f,%.1f)",
            m_entityID,
            m_baseFormID,
            m_createType == CoSyncEntityType::Player ? "Player" : "NPC",
            m_spawnFlags,
            m_spawnPos.x, m_spawnPos.y, m_spawnPos.z
        );

        CoSyncPlayer& pl = g_CoSyncPlayerManager.GetOrCreateByEntityID(m_entityID);

        // SpawnInWorld branches on pl.createType; the world-actor bind path
        // never sets it, so it must be stamped from the task before spawning.
        pl.createType = m_createType;

        if (pl.hasSpawned && pl.actorRef)
        {
            LOG_WARN("[SpawnTask] Already spawned entity=%u", m_entityID);
            return;
        }

        LOG_INFO("[SpawnTask] Getting anchor...");
        TESObjectREFR* anchor = GetAnchor();
        if (!anchor)
        {
            LOG_WARN("[SpawnTask] Anchor missing (player not ready?) entity=%u", m_entityID);
            return;
        }
        LOG_INFO("[SpawnTask] Anchor OK: 0x%p", anchor);

        // 2026-08-25 -- DO NOT SPAWN INTO A CELL THAT IS STILL LOADING.
        // When both players teleport, the client receives the host's proxy
        // EntityCreate while its OWN cell is mid-stream: the local player's
        // parentCell is NULL, CreateReferenceAtLocation places the proxy into a
        // half-loaded cell (log: "parentCell is NULL"), and a few seconds later
        // the client's game thread hangs on the load screen while the render
        // thread keeps presenting -- the host, which never places a proxy into
        // its own loading cell, finishes fine. Gate on the anchor's (local
        // player's) parentCell and re-enqueue until the cell is streamed in.
        // One retry per task pump (per frame); naturally throttled, drains the
        // instant the load completes.
        if (!anchor->parentCell)
        {
            static uint32_t s_deferLog = 0;
            if ((s_deferLog++ % 120) == 0)
                LOG_UPDATED("[SpawnTask] entity=%u deferred -- local cell still loading "
                            "(parentCell NULL), retrying next frame", m_entityID);
            CoSyncSpawnTasks::EnqueueSpawn(
                m_entityID, m_baseFormID, m_createType, m_spawnFlags, m_spawnPos, m_spawnRot);
            return;
        }


        LOG_INFO("[SpawnTask] Resolving base form...");
        const uint32_t resolvedBase = ResolveBaseForSpawn(m_baseFormID, m_createType, m_entityID);

        if (resolvedBase == 0)
        {
            LOG_ERROR("[SpawnTask] Resolved baseFormID is 0 (entity=%u type=%u) - cannot spawn",
                m_entityID, (uint32_t)m_createType);
            return;
        }
        LOG_INFO("[SpawnTask] Resolved base: 0x%08X (packet had 0x%08X)", resolvedBase, m_baseFormID);


        LOG_INFO("[SpawnTask] Resolving TESNPC...");
        TESNPC* npcBase = ResolveNPC(resolvedBase);
        if (!npcBase)
        {
            LOG_ERROR(
                "[SpawnTask] BaseForm 0x%08X is not TESNPC (entity=%u type=%u)",
                resolvedBase,
                m_entityID,
                static_cast<uint32_t>(m_createType)
            );
            return;
        }
        LOG_INFO("[SpawnTask] TESNPC OK: 0x%p", npcBase);


        LOG_INFO("[SpawnTask] Calling SpawnInWorld...");
        if (!pl.SpawnInWorld(anchor, npcBase))
        {
            LOG_ERROR("[SpawnTask] SpawnInWorld FAILED entity=%u", m_entityID);
            return;
        }
        LOG_INFO("[SpawnTask] SpawnInWorld SUCCESS entity=%u actorRef=%p refID=0x%08X baseForm=0x%08X",
            m_entityID, (void*)pl.actorRef,
            pl.actorRef ? pl.actorRef->formID : 0,
            (pl.actorRef && pl.actorRef->baseForm) ? pl.actorRef->baseForm->formID : 0);

        // Fix #2 (spawn/despawn race, proven 2026-08-02 client log): a host
        // DESTROY can arrive while this async spawn is still in flight — on an
        // ext->int cell change the host sends CREATE then DESTROY within ~100ms
        // (DESTROY logged 17:00:14.857, this task completed 17:00:14.972).
        // ClientUnbindWorldActor ran first but the clone didn't exist yet, so it
        // couldn't despawn it and just cleared the pending flag. Registering now
        // would orphan the fresh clone until the 15s PlayerMgr timeout (the
        // "enemies frozen outside the building" symptom). Re-validate the entity
        // is still wanted; if not, despawn the just-created clone and drop the
        // re-created manager entry instead of leaving an orphan. World-actor
        // proxies only — player proxies are never destroyed this way. NOTE: do
        // not touch `pl` after UnregisterEntityIDOnly (it erases the entry `pl`
        // references), hence despawn first, unregister last, return immediately.
        if (m_createType == CoSyncEntityType::NPC &&
            CoSyncNpcWorldSync::IsWorldActorEntityID(m_entityID) &&
            !CoSyncNpcWorldSync::IsWorldActorWanted(m_entityID))
        {
            LOG_UPDATED("[SpawnTask] entity=%u DESTROY raced ahead of spawn — despawning orphaned clone actor=%p",
                m_entityID, (void*)pl.actorRef);
            if (pl.actorRef)
                CoSyncGameAPI::DespawnProxyActor(pl.actorRef);
            g_CoSyncPlayerManager.UnregisterEntityIDOnly(m_entityID);
            return;
        }

        if (m_createType == CoSyncEntityType::NPC && pl.actorRef && pl.actorRef->baseForm)
        {
            const uint32_t liveBaseFormID = pl.actorRef->baseForm->formID;
            const bool isPrivateCopy = (liveBaseFormID != m_baseFormID);
            LOG_INFO("[AppearanceDiag] entity=%u packetBase=0x%08X liveBase=0x%08X privateCopy=%s",
                m_entityID, m_baseFormID, liveBaseFormID, isPrivateCopy ? "YES" : "NO");
        }


        if (m_createType == CoSyncEntityType::NPC)
        {
            // World NPC proxy: move to actual world position from EntityCreatePacket.
            // Skip if spawnPos is zero (debug NPC or uninitialized packet position)
            // to avoid teleporting the actor to the world origin.
            const bool hasPos = (m_spawnPos.x != 0.0f || m_spawnPos.y != 0.0f || m_spawnPos.z != 0.0f);
            LOG_INFO("[SpawnTask] NPC proxy entity=%u hasPos=%s spawnPos=(%.1f,%.1f,%.1f)",
                m_entityID, hasPos ? "YES" : "NO",
                m_spawnPos.x, m_spawnPos.y, m_spawnPos.z);
            if (hasPos)
            {
                CoSyncGameAPI::PositionRemoteActor(pl.actorRef, m_spawnPos, m_spawnRot);
                LOG_INFO("[SpawnTask] NPC proxy entity=%u PositionRemoteActor called -> (%.1f,%.1f,%.1f)",
                    m_entityID, m_spawnPos.x, m_spawnPos.y, m_spawnPos.z);
            }
            else
            {
                LOG_INFO("[SpawnTask] NPC proxy entity=%u spawnPos=zero — keeping spawn position", m_entityID);
            }

            // Diagnostic: read back actual world position after positioning
            {
                NiPoint3 dbgPos{}, dbgRot{};
                if (CoSyncGameAPI::GetActorWorldTransform(pl.actorRef, dbgPos, dbgRot))
                    LOG_INFO("[SpawnDiag] NPC proxy entity=%u actual world pos after placement: (%.1f,%.1f,%.1f)",
                        m_entityID, dbgPos.x, dbgPos.y, dbgPos.z);
                else
                    LOG_WARN("[SpawnDiag] NPC proxy entity=%u GetActorWorldTransform returned false", m_entityID);
            }
            // AI disabled for NPC proxies. The round-130 experiment (AI left
            // enabled as a hypothesis for why proxies never animated) is
            // concluded: the real cause was IdleForceDefaultState firing at
            // ~0.9s before the behavior graph was ready (~3.5s). That is now
            // fixed (first IsGraphReady pass in TickAnimVars fires the event
            // natively). With AI enabled the clone walks around independently
            // on its own packages, fighting PositionRemoteActor entirely.
            // Puppet (the only NPC path now — legacy AI-disable removed): keep the
            // engine AI + behavior graph ALIVE so the enemy animates natively
            // (walk/aim/idle/combat); only disable avoidance so it doesn't
            // side-step the network-steered position. No AI-disable, no 299-var
            // hand-driving — that pipeline is skipped for NPC puppets (see
            // CoSyncPlayer TickSmoothing / TickAnimVars gating). Position is pinned
            // every tick via the native SetPosition steer in TickSmoothing.
            CoSyncGameAPI::SetActorAvoidanceDisabled(pl.actorRef, true);
            // 2026-08-12: avoidance-disable above is a verified engine no-op
            // (see CoSyncGameAPI.cpp) -- StopActorCombat is the call that
            // actually stops this puppet's local AI from fighting the
            // network-steered position once it decides it's in combat.
            CoSyncGameAPI::StopActorCombat(pl.actorRef);
            LOG_INFO("[SpawnTask] NPC proxy entity=%u PUPPET: AI kept ALIVE, avoidance disabled (network-steered), combat neutralized", m_entityID);
        }
        else
        {
            LOG_INFO("[SpawnTask] round-130: AI left ENABLED for player proxy entity=%u (anim experiment)", m_entityID);
            pl.m_pendingFactionDelay = 120;
            LOG_INFO("[SpawnTask] player proxy entity=%u: SetFactionRank deferred 120 ticks", m_entityID);
        }

        // Diagnostic: final position readback for all entity types
        {
            NiPoint3 dbgPos{}, dbgRot{};
            if (CoSyncGameAPI::GetActorWorldTransform(pl.actorRef, dbgPos, dbgRot))
                LOG_INFO("[SpawnDiag] entity=%u final world pos after full spawn: (%.1f,%.1f,%.1f)",
                    m_entityID, dbgPos.x, dbgPos.y, dbgPos.z);
            else
                LOG_WARN("[SpawnDiag] entity=%u GetActorWorldTransform returned false at final readback", m_entityID);
        }

        InstallRemoteActorHook(pl.actorRef);
        RegisterRemoteActor(pl.actorRef, &pl);
        LOG_INFO("[SpawnTask] Registered remote actor %p with TickHook entity=%u",
            (void*)pl.actorRef, m_entityID);

        // Round 123: freshly created RMPlayer-based player proxies come up as
        // invisible skeletons — the direct engine create skips the appearance
        // pipeline, so a Disable+Enable cycle forces a full 3D rebuild.
        // NPC world-sync proxies use REAL NPC base forms that the engine renders
        // correctly without this cycle. MORE CRITICALLY: calling RefreshActor3D
        // on NPC proxies that have kFlag_InitiallyDisabled set (common for
        // world-placed NPCs) causes a Papyrus EXEC AV inside CallFunctionNoWait
        // ("Disable" on an actor whose Papyrus script object is uninitialized).
        // The VEH catches that AV and simulates a return to f4se's task-dispatch
        // return address — which the dispatcher mistakes for the NEXT task's
        // Run() completing, silently dropping it. Skipping RefreshActor3D for
        // NPC proxies eliminates the AV chain that was causing every other task
        // in the queue to be dropped (including the host-player proxy spawn).
        if (m_createType != CoSyncEntityType::NPC)
        {
            CoSyncGameAPI::RefreshActor3D(pl.actorRef);
            // 2026-07-27 diagnostic: investigating a reported "other player's
            // proxy invisible after both sides died/reloaded near-simultaneously"
            // report. RefreshActor3D's Disable+Enable cycle is async
            // (CallFunctionNoWait) -- if Enable silently fails to complete
            // (e.g. queued during the OTHER side's own chaotic mid-reload
            // traffic), kFlag_IsDisabled (0x800) would still read set here.
            // Log raw flags every respawn so the next occurrence is provable
            // instead of guessed at.
            LOG_UPDATED("[SpawnDiag] entity=%u post-RefreshActor3D flags=0x%08X (kFlag_IsDisabled=%d kFlag_InitiallyDisabled=%d)",
                m_entityID, pl.actorRef->flags,
                (pl.actorRef->flags & 0x800) ? 1 : 0,
                (pl.actorRef->flags & 0x400) ? 1 : 0);
        }

        // NPC proxies: clear essential/protected flags and kick out of A-pose.
        if (m_createType == CoSyncEntityType::NPC)
        {
            // kFlag_InitiallyDisabled (bit 10 = 0x400) is inherited from the
            // real NPC's base form flags by CreateReferenceAtLocation. With it
            // set the Papyrus VM's script object for the actor is never
            // initialized; CallFunctionNoWait (SetEssential/SetProtected/
            // SendAnimationEvent) then EXEC AVs on the null vtable dispatch.
            // The VEH simulates a return to f4se's task-dispatch site, which
            // the dispatcher mistakes for the NEXT task completing and drops
            // it. This is the same queue-corruption chain as the RefreshActor3D
            // EXEC AV above — confirmed in logs as the terminal client crash
            // (01:19:52 EXEC AVs from entities 65539-65542 → READ AV at 0x14).
            // A freshly-spawned dynamic proxy has no semantic need for this
            // flag (it is not a CK-placed initially-disabled ref), so clearing
            // it here is correct. TESForm::flags is at offset 0x10.
            {
                auto* pFlags = reinterpret_cast<uint32_t*>(
                    reinterpret_cast<uint8_t*>(pl.actorRef) + 0x10);
                if (*pFlags & 0x400u)
                {
                    *pFlags &= ~0x400u;
                    LOG_INFO("[SpawnTask] NPC proxy entity=%u: cleared kFlag_InitiallyDisabled from flags (was 0x%08X)",
                        m_entityID, *pFlags | 0x400u);
                }
            }
            // DEFERRED: calling InitNpcProxy (Papyrus SetEssential/SetProtected/
            // IdleForceDefaultState) here caused an EXEC AV — the actor's Papyrus
            // VM script object is not ready immediately after CreateReferenceAtLocation.
            // The VEH "fixed" that AV by simulating a return to the F4SE task-dispatch
            // site, silently dropping the NEXT task in the queue (host player proxy
            // spawn, other NPC spawns). CoSyncPlayerManager ticks down this delay and
            // calls InitNpcProxy at 0, by which time the VM object is initialized.
            pl.m_pendingNpcInitDelay = 120;
            LOG_INFO("[SpawnTask] NPC proxy entity=%u: InitNpcProxy deferred 120 ticks", m_entityID);
        }

        LOG_INFO("[SpawnTask] Run COMPLETE entity=%u", m_entityID);
    }

private:
    uint32_t m_entityID;
    uint32_t m_baseFormID;
    CoSyncEntityType m_createType;
    uint32_t m_spawnFlags;
    NiPoint3 m_spawnPos;
    NiPoint3 m_spawnRot;
};

// Generic main-thread task (see CoSyncSpawnTasks::EnqueueMainThread).
class MainThreadLambdaTask final : public ITaskDelegate
{
public:
    explicit MainThreadLambdaTask(std::function<void()> fn) : m_fn(std::move(fn)) {}
    void Run() override
    {
        LOG_REQUIREMENT("MainThreadLambdaTask::Run", "Runs on the main thread from F4SE's task queue.");
        if (m_fn)
            m_fn();
    }

private:
    std::function<void()> m_fn;
};

bool CoSyncSpawnTasks::EnqueueMainThread(std::function<void()> fn)
{
    LOG_REQUIREMENT("CoSyncSpawnTasks::EnqueueMainThread", "Needs Init() to have run. Safe from any thread.");
    if (!s_taskIFace)
    {
        LOG_ERROR("[CoSyncSpawnTasks] EnqueueMainThread: task interface not ready");
        return false;
    }
    s_taskIFace->AddTask(new MainThreadLambdaTask(std::move(fn)));
    return true;
}

void CoSyncSpawnTasks::Init(const F4SETaskInterface* taskIFace)
{
    LOG_REQUIREMENT("CoSyncSpawnTasks::Init", "Needs a valid task interface. Nothing else in this module works until this succeeds -- EnqueueSpawn just does nothing until then.");
    if (!taskIFace)
    {
        LOG_ERROR("[CoSyncSpawnTasks] Init called with NULL taskIFace!");
        return;
    }

    s_taskIFace = taskIFace;
    LOG_INFO("[CoSyncSpawnTasks] Init taskIFace=%p", taskIFace);
}

void CoSyncSpawnTasks::EnqueueSpawn(
    uint32_t entityID,
    uint32_t baseFormID,
    CoSyncEntityType createType,
    uint32_t spawnFlags,
    const NiPoint3& spawnPos,
    const NiPoint3& spawnRot)
{
    LOG_REQUIREMENT("CoSyncSpawnTasks::EnqueueSpawn", "Needs Init() to have already run successfully so the task system is set up.");
    LOG_INFO("[CoSyncSpawnTasks::EnqueueSpawn] START entity=%u base=0x%08X",
        entityID, baseFormID);

    if (!s_taskIFace)
    {
        LOG_ERROR("[CoSyncSpawnTasks] EnqueueSpawn: taskIFace is NULL! Call Init first!");
        return;
    }

    LOG_INFO("[CoSyncSpawnTasks::EnqueueSpawn] Creating SpawnRemotePlayerTask...");

    auto* task = new SpawnRemotePlayerTask(
        entityID,
        baseFormID,
        createType,
        spawnFlags,
        spawnPos,
        spawnRot
    );

    LOG_INFO("[CoSyncSpawnTasks::EnqueueSpawn] Adding task to queue...");
    s_taskIFace->AddTask(task);

    LOG_INFO("[CoSyncSpawnTasks::EnqueueSpawn] SUCCESS entity=%u", entityID);
}
