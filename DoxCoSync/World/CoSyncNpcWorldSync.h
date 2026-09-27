#pragma once

#include <cstdint>
#include <string>
#include "Packets_EntityCreate.h"

class Actor;

namespace CoSyncNpcWorldSync
{

    static constexpr uint32_t kWorldActorBase = 0x00010000u;
    static constexpr uint32_t kWorldActorMax  = 0x0001FFFFu;

    // EntityDestroyPacket::reasonFlags bit: the actor died before it was
    // destroyed/unloaded -- receivers keep the corpse and never re-enable the
    // original ref (otherwise the enemy pops back to life on the client).
    static constexpr uint32_t kDestroyReason_Dead = 1u << 0;

    void TickScanWorldActors(double now);

    void TickSuppressLocalDynamicSpawns(double now);

    // Call at session start (CoSyncNet::Init) to clear stale actor pointers
    // from the previous session. Without this, heap address reuse causes
    // re-spawned actors to match old map entries and be silently skipped.
    void Reset();

    uint32_t GetEntityIDForActor(Actor* actor);

    inline bool IsWorldActorEntityID(uint32_t id)
    {
        return id >= kWorldActorBase && id <= kWorldActorMax;
    }

    Actor* FindClosestRegisteredActor(Actor* fromActor, float maxDistSq);

    void ClientBindWorldActor(const EntityCreatePacket& p);

    // Keeps the client's own (disabled) original NPC co-located with the proxy
    // that replaced it. Quest aliases and quest TARGET MARKERS still resolve to
    // the original ref, so leaving it frozen where it was disabled makes the
    // objective marker sit still while the visible NPC walks away. Throttled
    // to 1Hz internally; no-op for entities that never replaced a local ref.
    void SyncDisabledOriginalToProxy(uint32_t entityID, const NiPoint3& pos);

    void ClientUnbindWorldActor(uint32_t entityID, uint32_t reasonFlags = 0);

    // Drops only this entity's actor binding (s_eidToActor / s_actorToEID /
    // pending-spawn flag) after the engine freed the bound actor. Claims and
    // hidden originals are kept -- the entity still exists and its owner's
    // next CREATE re-binds it. Without this, ClientBindWorldActor's
    // "already bound" guard rejected every re-bind forever (bug N1).
    // Game thread; never touches the player manager, so it is safe to call
    // from inside CoSyncPlayerManager::Tick's entity loop.
    void ForgetBinding(uint32_t entityID);

    // Fix #2 (spawn/despawn race, proven 2026-08-02): true while a world-actor
    // entity is still pending a proxy spawn (s_spawnFallback) or already bound
    // (s_eidToActor). The async clone-spawn task calls this on completion — a
    // host DESTROY can race ahead of an in-flight spawn (ext->int cell change
    // sends CREATE then DESTROY within ~100ms), and ClientUnbindWorldActor runs
    // before the clone exists, so it can't despawn it and just clears the
    // pending flag. Without this re-check the fresh clone registers as an orphan
    // that lingers until the 15s PlayerMgr timeout. Returns false => the task
    // self-despawns the clone instead of orphaning it.
    bool IsWorldActorWanted(uint32_t entityID);

    // Despawns stale runtime-created proxy bodies (old bases 0x7 / 0x20749 /
    // 0x3599 / current RMPlayer) that persisted in a save from previous
    // sessions and aren't registered to any live entity. Runs on both roles.
    void CleanupStaleProxyBodies(double now);

    // Legendary rank sync — host broadcasts LGD|entityID|rank when an NPC's
    // LegendaryRank AV changes; client queues it and flushes on main thread.
    bool IsLegendary(const std::string& msg);
    void OnReceiveLegendary(const std::string& msg);
    void FlushPendingLegendaryOps();

    bool IsNpcSeen(const std::string& msg);
    void OnReceiveNpcSeen(const std::string& msg);

    // Host: registers an actor a client owns outright (its companion copy),
    // without NPCSEEN's de-duplication. Returns the new entity ID (0 = failed).
    uint32_t HostRegisterClientOwnedActor(uint32_t baseFormID, uint32_t refFormID, const NiPoint3& pos, uint32_t ownerEid);

    // "NPCDEAD|refFormID" -- host tells clients that a placed actor they
    // reported alive (NPCSEEN) is dead in the authoritative world, so the
    // client kills its own copy instead of the host allocating a live
    // client-owned entity for a corpse.
    bool IsNpcDead(const std::string& msg);
    void OnReceiveNpcDead(const std::string& msg);
    void FlushPendingNpcDeadOps();

    // Client: a KILL was applied to this world-actor proxy (HealthSync).
    void MarkEntityKilled(uint32_t entityID);

    // True if this world actor is known dead (host: KILL sent; client: KILL applied).
    bool IsEntityDead(uint32_t entityID);

    // Save-crash protection for BGSSaveLoadGame::SaveGame() null-parentCell
    // crash (Fallout4.exe+0xBF712D, IDA-confirmed 2026-07-21). Logs all
    // tracked proxy actors, and for any whose parentCell is null, temporarily
    // patches it to the player's current cell before the save walk so the
    // engine can build a valid location descriptor. SEH-wrapped.
    // Call from kMessage_PreSaveGame.
    void DiagnoseDisabledOriginalsForSave();

    // Counterpart to DiagnoseDisabledOriginalsForSave: restores parentCell=nullptr
    // for every actor that was patched. SEH-wrapped.
    // Call from kMessage_PostSaveGame.
    void RestoreActorsAfterSave();

}
