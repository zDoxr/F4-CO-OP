#pragma once

#include <cstdint>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <memory>

#include "Packets_EntityDestroy.h"
#include "Packets_EntityCreate.h"
#include "Packets_EntityUpdate.h"
#include "CoSyncEntityState.h"

struct InboxItem
{
    enum class Type : uint8_t
    {
        Create,
        Update,
        Destroy
    };

    Type type = Type::Update;

    EntityCreatePacket  create{};
    EntityUpdatePacket  update{};
    EntityDestroyPacket destroy{};
};

class CoSyncPlayer;

class CoSyncPlayerManager
{
public:

    void EnqueueEntityCreate(const EntityCreatePacket& p);
    void EnqueueEntityUpdate(const EntityUpdatePacket& p);
    void EnqueueEntityDestroy(const EntityDestroyPacket& p);


    void HostSendNpcUpdates(double now);


    void HostScanCompanions(double now);

    void ProcessInbox();
    void Tick();

    // 0.4.3: the owner sent something about this entity (see
    // CoSyncEntityState::lastContactLocalTime). Game thread. Never creates a state.
    void NoteAuthoritativeContact(uint32_t entityID);

    void SetLocalEntityID(uint32_t id) { m_localEntityID = id; }
    uint32_t GetLocalEntityID() const { return m_localEntityID; }

    CoSyncPlayer& GetOrCreateByEntityID(uint32_t entityID);


    CoSyncPlayer* GetPlayerByEntityID(uint32_t entityID);

    // Where an entity is (worldspace / cell, as in WS| and CREATE): a player
    // from its own location reports, an NPC from its CREATE or, failing that,
    // its owner's location. false = unknown. GAME THREAD.
    bool GetEntityLocation(uint32_t entityID, uint32_t& ws, uint32_t& cell);

    // Where an entity is in its location: the local player's own position,
    // anyone else's last received position update. false = none yet.
    // GAME THREAD.
    bool GetEntityPosition(uint32_t entityID, float& x, float& y, float& z);

    // True when (ws, cell) is where the local player is -- same exterior
    // worldspace, or the same interior cell. Unknown (ws == 0xFFFFFFFF) = true
    // (callers keep their old behaviour until a location is known).
    static bool IsLocalLocation(uint32_t ws, uint32_t cell);

    // Form ID of the weapon an actor has equipped (0 = none). SEH-guarded.
    static uint32_t EquippedWeaponFormID(Actor* actor);

    // Reverse lookup for the revive-activate hook (CoSyncDoorHook), which only
    // has the activated ref's formID off the engine's Activate worker. Linear
    // scan -- no reverse actor->entityID index exists, but session player
    // counts are small (same tradeoff CoSyncHealthSync::TickDamageDetection
    // already makes iterating GetAllPlayers() every tick).
    CoSyncPlayer* GetPlayerByActorFormID(uint32_t actorFormID);

    CoSyncEntityState& GetOrCreateState(uint32_t entityID);

    void UnregisterEntityIDOnly(uint32_t entityID);

    // Session end (L3): despawn every remote player proxy and unbind every
    // world NPC through NpcWorldSync (re-showing hidden originals), right now
    // rather than via the inbox -- the tick that drains it stops at shutdown.
    // Game thread.
    void DespawnAllRemote(const char* reason);


    const std::unordered_map<uint32_t, CoSyncEntityState>&
        GetStates() const { return m_statesByEntityID; }


    const std::unordered_map<uint32_t, std::unique_ptr<CoSyncPlayer>>&
        GetAllPlayers() const { return m_playersByEntityID; }

    // Live count of actual players (self + remote player proxies), excluding
    // world NPCs/companions. Both host and client maintain this map identically,
    // so it is the authoritative player count on either role (bug #7: the HUD
    // count used to only update on the host). Defined in the .cpp where
    // CoSyncPlayer is a complete type.
    int GetConnectedPlayerCount() const;

private:

    void ProcessEntityCreate(const EntityCreatePacket& p);
    void ProcessEntityUpdate(const EntityUpdatePacket& u);
    void ProcessEntityDestroy(const EntityDestroyPacket& d);
public:
    void NoteEntityDestroyed(uint32_t entityID);
    void ClearEntityDestroyed(uint32_t entityID) { m_destroyedAt.erase(entityID); }
private:

    void DespawnEntity(uint32_t entityID, const char* reason);
    void HandleTimeouts(double now);

    __declspec(noinline) void HostScanCompanions_Impl(double now);
    __declspec(noinline) void HostSendNpcUpdates_Impl(double now);
    bool ReleaseIfFreed(CoSyncPlayer& ent);


    void PumpDeferredSpawns();
    void SpawnWatchdog(double now);

private:

    std::mutex m_inboxMutex;
    std::deque<InboxItem> m_inbox;

    std::mutex m_spawnMutex;
    std::deque<EntityCreatePacket> m_pendingCreates;

    std::unordered_map<uint32_t, std::unique_ptr<CoSyncPlayer>> m_playersByEntityID;

    std::unordered_map<uint32_t, CoSyncEntityState> m_statesByEntityID;

    // 0.3.3 (Stage 2): entities a DESTROY removed, with the time. Updates for
    // them are dropped until a new CREATE arrives (EU is unreliable and can
    // land after the reliable DESTROY). Game thread only.
    std::unordered_map<uint32_t, double> m_destroyedAt;

    uint32_t m_localEntityID = 0;
};


extern CoSyncPlayerManager g_CoSyncPlayerManager;
