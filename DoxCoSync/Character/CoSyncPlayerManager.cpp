#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncPlayerManager.h"

#include "CoSyncRequirementsLog.h"
#include "CoSyncTransport.h"
#include "CoSyncPlayer.h"
#include "CoSyncProxyLooks.h"
#include "ConsoleLogger.h"
#include "CoSyncWorld.h"
#include "CoSyncSpawnTasks.h"
#include "EntitySerialization.h"
#include "CoSyncEntityRegistry.h"
#include "CoSyncEntityTypes.h"
#include "CoSyncNet.h"
#include "CoSyncGameAPI.h"
#include "CoSyncAnimationUtils.h"
#include "CoSyncHealthSync.h"
#include "CoSyncActorValueSafety.h"
#include "CoSyncInventorySync.h"
#include "CoSyncLootSync.h"
#include "CoSyncMapSync.h"
#include "CoSyncPlayerMarker.h"
#include "CoSyncTeleport.h"
#include "CoSyncConsumableSync.h"
#include "CoSyncOutfitSync.h"
#include "TickHook.h"
#include "CoSyncPlayerAnimation.h"
#include "CoSyncBehaviorGraph.h"
#include "CoSyncAnimationUtils.h"
#include "CoSyncPapyrusHelper.h"
#include "CoSyncCreatureAnimSync.h"
#include "CoSyncNpcWorldSync.h"
#include "CoSyncContainerSync.h"
#include "CoSyncOverlay.h"
#include "CoSyncEquipHook.h"
#include "CoSyncThrowableSync.h"
#include "CoSyncVMFormArg.h"
#include "GameReferences.h"
#include "GameObjects.h"
#include "GameForms.h"
#include "GameRTTI.h"
#include "GameFormComponents.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <unordered_set>
#include <vector>

// Was extern RelocPtr<PlayerCharacter*> g_player -- switched to CoSyncGetPlayer()
// (see GameGlobals.cpp), the established, confirmed-working replacement for
// raw g_player dereferences used throughout the rest of the codebase.
extern PlayerCharacter* CoSyncGetPlayer();

// Round 117: g_lastTickStep (CoSyncNet.cpp) only marks this whole function as
// one step ("g_CoSyncPlayerManager.Tick") -- if anything inside it faults,
// the log can't say which phase or which entity. Breaking it down further,
// same pattern as CoSyncNet::Tick's own internal breadcrumbs.
extern volatile const char* g_lastTickStep;

CoSyncPlayerManager g_CoSyncPlayerManager;


// Round 134: was constexpr vanilla 0x00020749 — a confirmed A-pose dud (frozen
// even from console with no plugin). Now resolved at use time from CoSync.esp.
#define kLocalRemotePlayerBaseFormID (CoSyncPapyrusHelper::GetRemotePlayerBaseFormID())

static constexpr uint32_t kDebugNpcEntityID = 36865;

static double NowSeconds()
{
    LOG_REQUIREMENT("CoSyncPlayerManager::NowSeconds", "Just reads the system clock. Needs Windows to report a working timer, which it always does.");
    static double s_freq = [] {
        LOG_REQUIREMENT("CoSyncPlayerManager::NowSeconds::lambda", "Just reads the system clock. Needs Windows to report a working timer, which it always does.");
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
        }();

    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / s_freq;
}

// Round 142: weapon-equip ground-truth verification queue. Deliberately
// file-scope (not nested in the per-entity equip branch, see round-141/142
// history in [[project_cosync_crash_fixes]]) so ProcessWeaponVerifyQueue can
// be called unconditionally once per Tick() regardless of which entity (if
// any) equipped a weapon that frame.
namespace
{
    struct WeaponVerify { uint32_t entityID; uint32_t formID; double at; };
    std::vector<WeaponVerify>           s_weaponVerifyQueue;
}

static void QueueWeaponVerify(uint32_t entityID, uint32_t formID)
{
    LOG_REQUIREMENT("(anonymous)::QueueWeaponVerify", "Just adds an entry to a list, nothing else needed.");
    s_weaponVerifyQueue.push_back({ entityID, formID, NowSeconds() + 2.0 });
}

static void ProcessWeaponVerifyQueue()
{
    LOG_REQUIREMENT("(anonymous)::ProcessWeaponVerifyQueue", "Needs the player manager to be set up. Checks each entry's actor is real before touching it.");
    const double now = NowSeconds();
    for (size_t vi = 0; vi < s_weaponVerifyQueue.size(); )
    {
        WeaponVerify& pending = s_weaponVerifyQueue[vi];
        if (now < pending.at) { ++vi; continue; }

        CoSyncPlayer* vProxy = g_CoSyncPlayerManager.GetPlayerByEntityID(pending.entityID);
        if (vProxy && vProxy->actorRef)
        {
            bool present = false, equipped = false;
            BGSInventoryList* invList = vProxy->actorRef->inventoryList;
            if (invList)
            {
                for (UInt32 k = 0; k < invList->items.count; ++k)
                {
                    const BGSInventoryItem& item = invList->items[k];
                    if (!item.form || item.form->formID != pending.formID)
                        continue;
                    present = true;
                    if (item.stack)
                    {
                        item.stack->Visit([&equipped](BGSInventoryItem::Stack* s) {
                            if (s->flags & BGSInventoryItem::Stack::kFlagEquipped)
                            {
                                equipped = true;
                                return false;
                            }
                            return true;
                        });
                    }
                    break;
                }
            }
            LOG_UPDATED("[PlayerMgr] WEAPON VERIFY entity=%u weapon=0x%08X present=%d equipped=%d",
                pending.entityID, pending.formID, present ? 1 : 0, equipped ? 1 : 0);

            // 2026-07-27 diagnostic: this fires ~2s after every spawn/respawn
            // regardless of weapon state, so it doubles as a settled-state
            // check for the "other player's proxy invisible after a
            // simultaneous double-death" report -- if kFlag_IsDisabled (0x800)
            // still reads set here, RefreshActor3D's async Enable() silently
            // never completed for this respawn.
            LOG_UPDATED("[PlayerMgr] entity=%u settled flags=0x%08X (kFlag_IsDisabled=%d)",
                pending.entityID, vProxy->actorRef->flags,
                (vProxy->actorRef->flags & 0x800) ? 1 : 0);

            // Round 144: a persistent, repeatable (not one-off) failure for
            // specific formIDs -- live 2026-07-06 log: the SAME weapon
            // failed present=0 on every retry across a whole session, one
            // SPECIFIC armor piece failed every retry while another
            // succeeded every time. That pattern rules out the round-143
            // spawn-timing race (which would be intermittent) and points at
            // either (a) a leveled-list form -- AddItem(LVLI) adds a
            // RANDOMLY ROLLED concrete item, so a formID-exact check never
            // matches even though something real landed, or (b) the client
            // resolving pending.formID to a wrong/non-WEAP record. Dump
            // ground truth on failure so the next log settles which.
            if (!present)
            {
                TESForm* resolvedForm = LookupFormByID(pending.formID);
                LOG_UPDATED_WARN("[PlayerMgr] WEAPON VERIFY MISS entity=%u formID=0x%08X resolvedFormType=%d invCount=%u",
                    pending.entityID, pending.formID,
                    resolvedForm ? (int)resolvedForm->formType : -1,
                    invList ? (unsigned)invList->items.count : 0u);
                if (invList)
                {
                    UInt32 dumpCount = invList->items.count < 15 ? invList->items.count : 15;
                    for (UInt32 k = 0; k < dumpCount; ++k)
                    {
                        const BGSInventoryItem& item = invList->items[k];
                        LOG_UPDATED_WARN("[PlayerMgr]   inv[%u] formID=0x%08X formType=%d",
                            k, item.form ? item.form->formID : 0u, item.form ? (int)item.form->formType : -1);
                    }
                }
            }

            // Round 143: this is what CoSyncPlayer::ApplyUpdate's periodic
            // retry checks — only a CONFIRMED equip (present+equipped read
            // back from the actual inventory) suppresses further retries.
            // Leaving it false on failure lets the 10s periodic re-trigger
            // in ApplyUpdate fire AddItem/EquipItem again automatically.
            vProxy->m_weaponConfirmedEquipped = equipped;
            if (!equipped)
                LOG_UPDATED_WARN("[PlayerMgr] WEAPON VERIFY entity=%u weapon=0x%08X FAILED verification -- will retry",
                    pending.entityID, pending.formID);

            // 2026-08-26 REMOVED: this used to unconditionally fire
            // RefreshActor3D (Disable+Enable) on every SUCCESSFUL weapon
            // verify -- but a live test with [0.0.82]'s shared throttle
            // (which fixed the RACING between this and CoSyncOutfitSync's own
            // independent refresh) showed the underlying symptom persisted:
            // "weapon appears for a second after drawing, then vanishes."
            // Root cause was this call itself, not a race -- Enable() drops
            // any individually-equipped weapon (already established
            // elsewhere in this codebase), so firing it right after
            // confirming the weapon landed just stripped it again, forcing
            // the ~1s m_postRefreshWeaponDelay re-equip cycle that produces
            // exactly that flicker, EVERY single time a weapon is verified --
            // not a rare race, a guaranteed self-inflicted cycle. Unlike
            // CoSyncOutfitSync's own RefreshActor3D (which legitimately needs
            // the biped-slot rebuild for ARMOR visuals), a weapon's mesh
            // attaches through DrawWeaponMagicHands/EquipObject's own
            // weapon-specific chain (IDA-decompiled this session, no biped-
            // slot dependency found) -- this rebuild was never fixing
            // anything for weapons specifically. Verification FAILURE still
            // has its own recovery path (ApplyUpdate's periodic 10s retry,
            // per the comment above), so nothing is lost by not rebuilding
            // here on success.
        }

        s_weaponVerifyQueue[vi] = s_weaponVerifyQueue.back();
        s_weaponVerifyQueue.pop_back();
    }
}

static EntityCreatePacket MakeHostDebugNpcCreate(uint32_t localEntityID)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::MakeHostDebugNpcCreate", "Just builds a packet from the given entity ID, no game state touched.");
    EntityCreatePacket p{};

    p.entityID = kDebugNpcEntityID;
    p.ownerEntityID = localEntityID;

    p.type = CoSyncEntityType::NPC;

    // round 34: this packet never set baseFormID, so it stayed at the
    // struct's zero-init default. CoSyncSpawnTasks::ResolveBaseForSpawn only
    // has a zero-baseFormID fallback for CoSyncEntityType::Player (resolves
    // via CoSyncPapyrusHelper::GetRemotePlayerBase()) -- NPC-type spawns have
    // no equivalent fallback, so this debug NPC's spawn task always failed
    // immediately with "Resolved baseFormID is 0 ... cannot spawn", before
    // PlaceAtMe_Native was ever called. Use the same base form already
    // confirmed working for real player proxies (kLocalRemotePlayerBaseFormID)
    // so this spawns as a normal, fully-functional human actor body.
    p.baseFormID = kLocalRemotePlayerBaseFormID;

    p.spawnFlags = EntityCreatePacket::RemoteControlled;


    p.spawnPos = NiPoint3(0.f, 0.f, 0.f);
    p.spawnRot = NiPoint3(0.f, 0.f, 0.f);

    return p;
}


static CoSyncPlayer* FindAnyRemoteClientPlayerProxy(
    std::unordered_map<uint32_t, std::unique_ptr<CoSyncPlayer>>& playersByEntityID,
    std::unordered_map<uint32_t, CoSyncEntityState>& statesByEntityID,
    uint32_t localEntityID)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::FindAnyRemoteClientPlayerProxy", "Needs the player and state lists to exist. Checks each candidate has actually spawned before using it.");
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

void CoSyncPlayerManager::EnqueueEntityCreate(const EntityCreatePacket& p)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::EnqueueEntityCreate", "Just queues the packet for later. Skips it if it's our own entity or a host-only debug NPC.");

    if (p.entityID != 0 && p.entityID == m_localEntityID)
    {
        LOG_INFO("[PlayerMgr] Ignoring CREATE for local entity=%u", p.entityID);
        return;
    }


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
    LOG_REQUIREMENT("CoSyncPlayerManager::EnqueueEntityUpdate", "Just queues the packet for later. Skips the host-only debug NPC packet.");

    if (p.entityID == kDebugNpcEntityID && !CoSyncNet::IsHost())
        return;

    static int s_euDiag = 0;
    if (++s_euDiag % 60 == 1)
        LOG_INFO("[PlayerMgr] EnqueueEntityUpdate entity=%u [inbox#%d]", p.entityID, s_euDiag);

    std::lock_guard<std::mutex> lk(m_inboxMutex);

    InboxItem item{};
    item.type = InboxItem::Type::Update;
    item.update = p;

    m_inbox.push_back(item);
}

void CoSyncPlayerManager::EnqueueEntityDestroy(const EntityDestroyPacket& p)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::EnqueueEntityDestroy", "Just queues the packet for later. Skips it if it's blank, our own entity, or a host-only debug NPC.");
    if (p.entityID == 0 || p.entityID == m_localEntityID)
        return;


    if (p.entityID == kDebugNpcEntityID && !CoSyncNet::IsHost())
        return;

    std::lock_guard<std::mutex> lk(m_inboxMutex);

    InboxItem item{};
    item.type = InboxItem::Type::Destroy;
    item.destroy = p;

    m_inbox.push_back(item);
    LOG_INFO("[PlayerMgr] Enqueued DESTROY entity=%u", p.entityID);
}

void CoSyncPlayerManager::ProcessInbox()
{
    LOG_REQUIREMENT("CoSyncPlayerManager::ProcessInbox", "Grabs the queued packets and hands each one to the right handler.");
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

CoSyncEntityState& CoSyncPlayerManager::GetOrCreateState(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::GetOrCreateState", "Just looks up (or creates) an entry by entity ID.");
    auto it = m_statesByEntityID.find(entityID);
    if (it != m_statesByEntityID.end())
        return it->second;

    CoSyncEntityState st{};
    st.entityID = entityID;


    auto result = m_statesByEntityID.emplace(entityID, std::move(st));
    return result.first->second;
}

CoSyncPlayer& CoSyncPlayerManager::GetOrCreateByEntityID(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::GetOrCreateByEntityID", "Just looks up (or creates) a player entry by entity ID. It has no actor yet until spawning happens separately.");
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


CoSyncPlayer* CoSyncPlayerManager::GetPlayerByEntityID(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::GetPlayerByEntityID", "Just a lookup. Returns null safely if the entity ID isn't known -- callers still need to check for that.");
    auto it = m_playersByEntityID.find(entityID);
    if (it != m_playersByEntityID.end())
        return it->second.get();
    return nullptr;
}

CoSyncPlayer* CoSyncPlayerManager::GetPlayerByActorFormID(uint32_t actorFormID)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::GetPlayerByActorFormID", "Scans the player list for a match. Returns null if the form ID is blank or not found.");
    if (actorFormID == 0)
        return nullptr;

    for (auto& kv : m_playersByEntityID)
    {
        CoSyncPlayer* p = kv.second.get();
        if (p && p->actorRef && p->actorRef->formID == actorFormID)
            return p;
    }
    return nullptr;
}

int CoSyncPlayerManager::GetConnectedPlayerCount() const
{
    LOG_REQUIREMENT("CoSyncPlayerManager::GetConnectedPlayerCount", "Just counts entries in the player list, nothing else needed.");
    int n = 0;
    for (const auto& kv : m_playersByEntityID)
        if (kv.second && kv.second->createType == CoSyncEntityType::Player)
            ++n;
    return n;
}

void CoSyncPlayerManager::ProcessEntityCreate(const EntityCreatePacket& p)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::ProcessEntityCreate", "Needs a valid entity ID that isn't our own player. Doesn't spawn anything directly -- just queues the spawn for later once the world is ready.");
    if (p.entityID == 0 || p.entityID == m_localEntityID)
        return;


    if (p.entityID == kDebugNpcEntityID && !CoSyncNet::IsHost())
        return;

    CoSyncEntityState& st = GetOrCreateState(p.entityID);


    st.isLocallyOwned = (p.ownerEntityID != 0 && p.ownerEntityID == m_localEntityID);


    st.lastCreate = p;
    st.hasCreate = true;
    st.lastUpdateLocalTime = NowSeconds();


    st.isNPC = (p.type == CoSyncEntityType::NPC || p.type == CoSyncEntityType::Companion);


    st.hostAuthoritative = st.isNPC && CoSyncNet::IsHost();


    CoSyncPlayer& player = GetOrCreateByEntityID(p.entityID);
    player.ownerEntityID = p.ownerEntityID;
    player.createType = p.type;


    if (!p.username.empty())
    {
        const bool nameChanged = (player.username != p.username);
        player.username = p.username;
        if (nameChanged)
            LOG_UPDATED("[NameSync APPLY] entity=%u username set to '%s' (type=%u)",
                p.entityID, p.username.c_str(), (unsigned)p.type);
    }
    else
    {
        LOG_UPDATED_WARN("[NameSync] entity=%u CREATE has no username — proxy will show as '%s'",
            p.entityID, player.username.c_str());
    }
    player.isRemoteControlled =
        EntityCreatePacket::HasFlag(p.spawnFlags, EntityCreatePacket::RemoteControlled);

    player.authoritativePos = p.spawnPos;
    player.authoritativeRot = p.spawnRot;

    // See CoSyncPlayer.h authoritativeWorldspaceID comment: captures which
    // worldspace context this position is actually meaningful in, so
    // SpawnWatchdog can refuse to respawn across an ext/int transition
    // instead of trusting a raw-distance coincidence.
    {
        PlayerCharacter* lp = CoSyncGetPlayer();
        if (lp)
        {
            TESWorldSpace* lpWS = CALL_MEMBER_FN(reinterpret_cast<TESObjectREFR*>(lp), GetWorldspace)();
            player.authoritativeWorldspaceID = lpWS ? reinterpret_cast<TESForm*>(lpWS)->formID : 0u;
        }
    }

    if (!player.hasSpawned)
    {
        player.pendingPos = p.spawnPos;
        player.pendingRot = p.spawnRot;
        player.hasPendingTransform = true;
    }


    if (!st.spawnQueued)
    {
        st.spawnQueued = true;
        std::lock_guard<std::mutex> lk(m_spawnMutex);
        m_pendingCreates.push_back(p);

        LOG_INFO("[PlayerMgr] Queued CREATE for spawn entity=%u base=0x%08X",
            p.entityID, p.baseFormID);
    }
}

void CoSyncPlayerManager::ProcessEntityUpdate(const EntityUpdatePacket& u)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::ProcessEntityUpdate", "Needs a valid entity ID that isn't our own player, and that entity must already have a recorded spawn before this applies the update.");
    if (u.entityID == 0 || u.entityID == m_localEntityID)
    {
        static int s_selfDropDiag = 0;
        if (++s_selfDropDiag <= 5 || s_selfDropDiag % 300 == 1)
            LOG_WARN("[PlayerMgr] ProcessEntityUpdate DROP(self) entity=%u localEID=%u [drop#%d]",
                u.entityID, m_localEntityID, s_selfDropDiag);
        return;
    }


    if (u.entityID == kDebugNpcEntityID && !CoSyncNet::IsHost())
        return;

    CoSyncEntityState& st = GetOrCreateState(u.entityID);

    // Never apply a relayed update back onto an entity this machine itself
    // drives (the actor is real and natively simulated here).
    if (st.isLocallyOwned && CoSyncNpcWorldSync::IsWorldActorEntityID(u.entityID))
        return;

    st.lastUpdateLocalTime = NowSeconds();
    st.lastUpdate = u;

    if (!st.hasCreate)
    {
        // Self-heal: we are receiving live updates for an entity whose create
        // we don't have, which is otherwise a permanent deadlock (the sender
        // has no idea we forgot). Ask whoever still holds the create to
        // re-broadcast it, throttled per entity so a stream of updates can't
        // turn into a request flood.
        {
            static std::unordered_map<uint32_t, double> s_lastReqTime;
            const double nowReq = NowSeconds();
            double& last = s_lastReqTime[u.entityID];
            if (nowReq - last > 2.0)
            {
                last = nowReq;
                char reqBuf[48];
                std::snprintf(reqBuf, sizeof(reqBuf), "REQENT|%u", u.entityID);
                CoSyncTransport::Send(std::string(reqBuf));
                LOG_UPDATED("[PlayerMgr] no create for entity=%u — requested re-broadcast", u.entityID);
            }
        }

        static int s_noCreateDropDiag = 0;
        if (++s_noCreateDropDiag <= 5 || s_noCreateDropDiag % 300 == 1)
            LOG_WARN("[PlayerMgr] ProcessEntityUpdate DROP(no-create) entity=%u [drop#%d]",
                u.entityID, s_noCreateDropDiag);
        return;
    }


    if (st.isNPC && CoSyncNet::IsHost()
        && !(CoSyncNpcWorldSync::IsWorldActorEntityID(u.entityID) && st.lastCreate.ownerEntityID != 0))
        return;

    CoSyncPlayer& player = GetOrCreateByEntityID(u.entityID);
    player.ApplyUpdate(u);
}

void CoSyncPlayerManager::ProcessEntityDestroy(const EntityDestroyPacket& d)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::ProcessEntityDestroy", "Needs a valid entity ID that isn't our own player or a host-only debug NPC before it hands off to the despawn logic.");
    if (d.entityID == 0 || d.entityID == m_localEntityID)
        return;


    if (d.entityID == kDebugNpcEntityID && !CoSyncNet::IsHost())
        return;

    // World NPCs are owned by NpcWorldSync (binding, clone, hidden original):
    // unbind them there, as CoSyncNet's ED receiver and the timeout path do.
    // DespawnEntity alone left NpcWorldSync's binding behind (bug N1 class).
    if (CoSyncNpcWorldSync::IsWorldActorEntityID(d.entityID))
    {
        CoSyncNpcWorldSync::ClientUnbindWorldActor(d.entityID, d.reasonFlags);
        return;
    }

    DespawnEntity(d.entityID, "network destroy");
}

void CoSyncPlayerManager::DespawnAllRemote(const char* reason)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::DespawnAllRemote", "Game thread, session end. Collects every remote entity ID first, then tears each one down.");
    std::vector<uint32_t> ids;
    ids.reserve(m_playersByEntityID.size() + m_statesByEntityID.size());
    for (const auto& kv : m_playersByEntityID)
        if (kv.first != 0 && kv.first != m_localEntityID)
            ids.push_back(kv.first);
    for (const auto& kv : m_statesByEntityID)
        if (kv.first != 0 && kv.first != m_localEntityID && !m_playersByEntityID.count(kv.first))
            ids.push_back(kv.first);

    for (uint32_t id : ids)
    {
        if (CoSyncNpcWorldSync::IsWorldActorEntityID(id))
            CoSyncNpcWorldSync::ClientUnbindWorldActor(id);
        else
            DespawnEntity(id, reason);
    }

    {
        std::lock_guard<std::mutex> lk(m_inboxMutex);
        m_inbox.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_spawnMutex);
        m_pendingCreates.clear();
    }
    LOG_UPDATED("[PlayerMgr] Session end: removed %zu remote entities (%s)", ids.size(), reason);
}

void CoSyncPlayerManager::DespawnEntity(uint32_t entityID, const char* reason)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::DespawnEntity", "Needs a valid entity ID that isn't our own player. Only touches the actor once it confirms the proxy and its actor both exist.");
    if (entityID == 0 || entityID == m_localEntityID)
        return;

    // Drop the player's card from the lobby bar (no-op for world NPCs,
    // which never get one).
    CoSyncOverlay_RemoveLobbyEntry(entityID);

    // Its proxy base slot (own face/body) can be reused.
    CoSyncProxyLooks::OnProxyDespawned(entityID);

    {
        std::lock_guard<std::mutex> lk(m_spawnMutex);
        m_pendingCreates.erase(
            std::remove_if(
                m_pendingCreates.begin(),
                m_pendingCreates.end(),
                [entityID](const EntityCreatePacket& p)
                {
                    LOG_REQUIREMENT("CoSyncPlayerManager::DespawnEntity::lambda", "Just compares two entity IDs, nothing else needed.");
                    return p.entityID == entityID;
                }),
            m_pendingCreates.end());
    }

    g_CoSyncEntities.Remove(entityID);

    auto itPlayer = m_playersByEntityID.find(entityID);
    if (itPlayer != m_playersByEntityID.end())
    {
        CoSyncPlayer* proxy = itPlayer->second.get();
        LOG_UPDATED("[PlayerMgr] Despawn entity=%u actor=%p (%s)",
            entityID, static_cast<void*>(proxy ? proxy->actorRef : nullptr), reason);

        if (proxy && proxy->actorRef &&
            proxy->createType == CoSyncEntityType::Companion &&
            !CoSyncNet::IsHost())
        {
            CoSyncGameAPI::EnableActorAI(proxy->actorRef);
            LOG_UPDATED("[CompanionSync] EnableActorAI on dismissed companion entity=%u", entityID);
        }
        else if (proxy && proxy->actorRef &&
            (proxy->createType == CoSyncEntityType::Player ||
             proxy->createType == CoSyncEntityType::NPC) &&
            (proxy->actorRef->formID & 0xFF000000u) == 0xFF000000u)
        {
            // Player entities: always an FF-ref clone we spawned — must be torn down.
            // NPC entities (world NPC proxies): also FF-ref clones since SpawnRemotePlayerTask
            // creates them via SpawnInWorld. Guard on the 0xFF formID prefix to be
            // safe — if somehow a real world actor slipped in, this check prevents
            // deleting a save-persistent reference.
            // Companion entities are excluded (real world actors, restoring AI is
            // handled above; deleting them would corrupt the save).
            CoSyncGameAPI::DespawnProxyActor(proxy->actorRef);
            if (proxy->createType == CoSyncEntityType::Player)
                CoSyncPlayerMarker::HideMarker();
        }

        // 2026-08-20: release this proxy's write-last hook registration and
        // pinned bone cache (see TickHook.cpp RemoteActorUpdate_Hook /
        // CoSyncPlayerAnimation's BoneCacheEntry). Was previously a no-op gap --
        // s_remoteActors/g_boneCaches were never cleaned up on despawn even
        // when the hook was active, since this call was simply missing.
        // Idempotent either way (both are safe on an actor with no entry).
        if (proxy && proxy->actorRef)
        {
            UnregisterRemoteActor(proxy->actorRef);
            CoSyncPlayerAnimation::ReleaseBoneCache(proxy->actorRef);
        }

        // 2026-08-21: ReleaseBoneCache drops the proxy's pinned NODE pointers,
        // but the received POSE for this entity lived in a separate map that
        // nothing ever cleared. If the same entity respawned, the very first
        // apply drove it with a pose captured before it left.
        CoSyncPlayerAnimation::ForgetReceivedSkeleton(entityID);

        m_playersByEntityID.erase(itPlayer);
    }
    else
    {
        LOG_UPDATED("[PlayerMgr] Despawn entity=%u (no proxy) (%s)", entityID, reason);
    }

    auto itState = m_statesByEntityID.find(entityID);
    if (itState != m_statesByEntityID.end())
        m_statesByEntityID.erase(itState);
}

void CoSyncPlayerManager::UnregisterEntityIDOnly(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::UnregisterEntityIDOnly", "Needs a valid entity ID that isn't our own player. Only removes list entries, never touches an actor.");
    if (entityID == 0 || entityID == m_localEntityID)
        return;

    auto itPlayer = m_playersByEntityID.find(entityID);
    if (itPlayer != m_playersByEntityID.end())
        m_playersByEntityID.erase(itPlayer);

    auto itState = m_statesByEntityID.find(entityID);
    if (itState != m_statesByEntityID.end())
        m_statesByEntityID.erase(itState);

    LOG_UPDATED("[PlayerMgr] UnregisterEntityIDOnly entity=%u", entityID);
}

// Round 106 audit: p.reserved0 (Companion refFormID) is peer-controlled --
// it comes straight off a network EntityCreatePacket, same as every other
// formID this codebase already SEH-guards (CoSyncMapSync, CoSyncPowerArmorSync,
// CoSyncCompanionSync's HostScanCompanions, CoSyncDoorHook/LockHook all wrap
// their LookupFormByID+DYNAMIC_CAST call sites). This one didn't -- a garbage
// or stale refFormID reaching LookupFormByID/DYNAMIC_CAST here had no
// backstop at all, unlike every analogous site. Split into _Impl (holds the
// real logic) + thin Safe wrapper (just the __try/__except), the established
// pattern in this codebase for avoiding C2712 (EntityCreatePacket has a
// std::string member, so it can't share a function scope with __try).
__declspec(noinline) static void PumpDeferredSpawns_Impl(
    CoSyncPlayerManager* self, const EntityCreatePacket& p)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::PumpDeferredSpawns_Impl", "For companion packets, needs the linked reference ID (sent by the other player) to resolve to a real actor. The caller wraps this in a safety net since that lookup isn't checked here.");
    if (p.type == CoSyncEntityType::Companion)
    {
        const uint32_t refFormID = p.reserved0;
        if (refFormID == 0)
        {
            LOG_WARN("[PlayerMgr] Companion CREATE entity=%u has no refFormID", p.entityID);
            return;
        }

        TESForm* form = LookupFormByID(refFormID);
        // DYNAMIC_CAST (__RTDynamicCast) is confirmed broken in this codebase
        // (same as HostScanCompanions below) -- and this exact call site is the
        // 2026-07-04 04:00 minidump crash: RTTI walked into Fallout4.exe's
        // __std_type_info_name thunk and ucrtbase strlen'd the formID value
        // (AV READ at 0xFF001CC0). formType == kFormType_ACHR guarantees Actor
        // by engine contract (engine itself checks the +0x1A type byte == 65),
        // so static_cast avoids the broken RTTI path entirely.
        Actor* actor = (form && form->formType == kFormType_ACHR)
            ? static_cast<Actor*>(form) : nullptr;
        if (!actor)
        {
            LOG_WARN("[PlayerMgr] Companion entity=%u refFormID=0x%08X not found as Actor (form=%p type=%u)",
                p.entityID, refFormID, static_cast<void*>(form),
                form ? (unsigned)form->formType : 0u);
            return;
        }

        CoSyncPlayer& companion = self->GetOrCreateByEntityID(p.entityID);
        companion.ownerEntityID  = p.ownerEntityID;
        companion.createType     = p.type;
        companion.isRemoteControlled = true;
        companion.actorRef       = actor;
        companion.hasSpawned     = true;

        InstallRemoteActorHook(actor);
        RegisterRemoteActor(actor, &companion);

        if (!CoSyncNet::IsHost())
        {
            CoSyncGameAPI::DisableActorAI(actor);
            LOG_UPDATED("[CompanionSync] DisableActorAI on client companion entity=%u (host drives position)",
                p.entityID);
        }

        // Round 123: save-persisted proxies re-linked here have the same
        // invisible-skeleton problem as fresh spawns — rebuild their 3D too.
        CoSyncGameAPI::RefreshActor3D(actor);

        LOG_UPDATED("[CompanionSync] Linked companion entity=%u refFormID=0x%08X actor=%p name='%s'",
            p.entityID, refFormID, static_cast<void*>(actor),
            companion.username.empty() ? "?" : companion.username.c_str());
        return;
    }

    LOG_INFO("[PlayerMgr] Spawning entity=%u base=0x%08X",
        p.entityID, p.baseFormID);

    // BISECTION (2026-06-24): re-enabled. The 2026-06-24 19:54 test with
    // this disabled crashed at the exact same point with the exact same
    // signature (same cell, same WBF formID/positions, same silent
    // death) -- proves remote-player spawning is NOT the cause. Next
    // suspect: World/CoSyncWorldSync.cpp's HostTickObjects_Impl (see its
    // round-16 comment, an already-documented match for this exact
    // symptom).
    CoSyncSpawnTasks::EnqueueSpawn(
        p.entityID,
        p.baseFormID,
        p.type,
        p.spawnFlags,
        p.spawnPos,
        p.spawnRot
    );


    CoSyncPlayer& player = self->GetOrCreateByEntityID(p.entityID);
    player.ownerEntityID = p.ownerEntityID;
}

__declspec(noinline) static void PumpDeferredSpawnsSafe(
    CoSyncPlayerManager* self, const EntityCreatePacket& p)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::PumpDeferredSpawnsSafe", "Needs a valid manager instance. Wraps the spawn logic in a safety net in case the peer sent a bad reference ID.");
    __try
    {
        PumpDeferredSpawns_Impl(self, p);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[PlayerMgr] EXCEPTION in PumpDeferredSpawnsSafe entity=%u type=%d reserved0=0x%08X",
            p.entityID, (int)p.type, p.reserved0);
    }
}

void CoSyncPlayerManager::PumpDeferredSpawns()
{
    LOG_REQUIREMENT("CoSyncPlayerManager::PumpDeferredSpawns", "Waits until the world is fully loaded before processing a queued spawn. Spawns are held, not lost, while it isn't ready yet.");
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

    // 2026-07-21: this already only pops+processes ONE pending spawn per
    // tick (not the whole queue at once) -- but at ~60 ticks/sec that's
    // still 30-40 new CreateReferenceAtLocation calls landing within under
    // a second in a dense area, which can outpace the engine's own 3D-
    // streaming/attach queue even though our own enqueue rate is spaced out.
    // Matches the user-reported "too many spawning at once, 3D never
    // finishes loading" symptom this round's IsGraphReady retry fix
    // (CoSyncPlayer.cpp TickAnimVars) was built to recover FROM -- this
    // gives the engine more room between each new actor so fewer ever get
    // stuck in the first place.
    static constexpr double kMinSpawnInterval = 0.15; // ~6-7/sec instead of ~60/sec
    static double s_lastSpawnTime = 0.0;
    const double spawnNow = NowSeconds();
    if (spawnNow - s_lastSpawnTime < kMinSpawnInterval)
        return;

    EntityCreatePacket p{};
    {
        std::lock_guard<std::mutex> lk(m_spawnMutex);
        if (m_pendingCreates.empty())
            return;

        p = m_pendingCreates.front();

        // Round 135: never spawn a remote PLAYER's proxy until that player has
        // sent at least one EntityUpdate — peers only tick EU once fully
        // loaded in-world, so this holds the spawn while they sit on a
        // loading screen (user-reported: proxies appearing before p2 loaded).
        if (p.type == CoSyncEntityType::Player)
        {
            auto its = m_statesByEntityID.find(p.entityID);
            const bool peerInWorld =
                (its != m_statesByEntityID.end()) && its->second.lastUpdateLocalTime > 0.0;
            if (!peerInWorld)
            {
                static uint32_t s_waitLog = 0;
                if ((s_waitLog++ % 600) == 0)
                    LOG_INFO("[PlayerMgr] Holding spawn entity=%u until peer sends first EU (still loading?) #%u",
                        p.entityID, s_waitLog);
                return;   // leave queued; retry next tick
            }
        }

        m_pendingCreates.pop_front();
    }

    s_lastSpawnTime = spawnNow;

    {
        auto it = m_statesByEntityID.find(p.entityID);
        if (it != m_statesByEntityID.end())
            it->second.lastSpawnAttemptTime = NowSeconds();
    }

    PumpDeferredSpawnsSafe(this, p);
}

static uint32_t GetEquippedWeaponFormID(Actor* actor);   // defined below

uint32_t CoSyncPlayerManager::EquippedWeaponFormID(Actor* actor)
{
    return GetEquippedWeaponFormID(actor);
}

bool CoSyncPlayerManager::IsLocalLocation(uint32_t ws, uint32_t cell)
{
    if (ws == 0xFFFFFFFFu)
        return true;
    PlayerCharacter* lp = CoSyncGetPlayer();
    if (!lp)
        return true;
    TESObjectREFR* r = reinterpret_cast<TESObjectREFR*>(lp);
    TESWorldSpace* lws = CALL_MEMBER_FN(r, GetWorldspace)();
    const uint32_t lwsID = lws ? reinterpret_cast<TESForm*>(lws)->formID : 0u;
    if (ws != 0)
        return ws == lwsID;
    return lwsID == 0 && r->parentCell && r->parentCell->formID == cell;
}

bool CoSyncPlayerManager::GetEntityLocation(uint32_t entityID, uint32_t& ws, uint32_t& cell)
{
    ws = 0xFFFFFFFFu;
    cell = 0;
    if (entityID == 0)
        return false;
    if (entityID == m_localEntityID)
    {
        PlayerCharacter* lp = CoSyncGetPlayer();
        if (!lp) return false;
        TESObjectREFR* r = reinterpret_cast<TESObjectREFR*>(lp);
        TESWorldSpace* lws = CALL_MEMBER_FN(r, GetWorldspace)();
        ws = lws ? reinterpret_cast<TESForm*>(lws)->formID : 0u;
        cell = r->parentCell ? r->parentCell->formID : 0u;
        return true;
    }
    CoSyncPlayer* p = GetPlayerByEntityID(entityID);
    if (p && p->createType == CoSyncEntityType::Player)
    {
        if (p->lastRemoteWorldspaceID == 0xFFFFFFFFu)
            return false;
        ws = p->lastRemoteWorldspaceID;
        cell = p->lastRemoteCellFormID;
        return true;
    }
    auto it = m_statesByEntityID.find(entityID);
    if (it == m_statesByEntityID.end() || !it->second.hasCreate)
        return false;
    const EntityCreatePacket& c = it->second.lastCreate;
    if (c.locWS != 0xFFFFFFFFu)
    {
        ws = c.locWS;
        cell = c.locCell;
        return true;
    }
    // No location on the CREATE: an NPC is where its simulating owner is.
    if (c.ownerEntityID != 0 && c.ownerEntityID != entityID)
    {
        uint32_t w2, c2;
        CoSyncPlayer* owner = GetPlayerByEntityID(c.ownerEntityID);
        if ((owner && owner->createType == CoSyncEntityType::Player) || c.ownerEntityID == m_localEntityID)
            if (GetEntityLocation(c.ownerEntityID, w2, c2)) { ws = w2; cell = c2; return true; }
    }
    return false;
}

void CoSyncPlayerManager::SpawnWatchdog(double now)
{
    for (auto& kv : m_statesByEntityID)
    {
        CoSyncEntityState& st = kv.second;
        if (!st.hasCreate || !st.spawnQueued) continue;
        if (st.lastSpawnAttemptTime <= 0.0) continue;
        if ((now - st.lastSpawnAttemptTime) < 3.0) continue;

        CoSyncPlayer* pl = GetPlayerByEntityID(kv.first);
        if (pl && pl->hasSpawned) continue;

        // Don't respawn a proxy whose authoritative position is beyond this
        // machine's loaded grid — the engine would just free the fresh actor
        // again (spawn/free churn every pass while players are split up).
        // Postpone by bumping the attempt time; spawn resumes automatically
        // once the remote side is back within range.
        if (pl)
        {
            PlayerCharacter* lp = CoSyncGetPlayer();

            // See CoSyncPlayer.h authoritativeWorldspaceID comment: refuse to
            // trust the distance check at all across a worldspace mismatch --
            // exterior/interior coordinates are numerically incomparable, and
            // a stale authoritativePos (set once at CREATE, never refreshed)
            // makes a false "in range" coincidence even more likely across a
            // real ext/int transition. This is what was letting a freed
            // exterior clone respawn at the player's own current position
            // (the door) instead of being correctly postponed.
            // 0.3.1: the entity's REAL location (players report theirs with
            // WS|). A proxy for someone in another interior/worldspace is not
            // spawned here at all -- it would land at meaningless coordinates
            // and the engine would free it again (the 0.3.0 "snap back").
            uint32_t locWS = 0xFFFFFFFFu, locCell = 0;
            if (GetEntityLocation(kv.first, locWS, locCell) && !IsLocalLocation(locWS, locCell))
            {
                st.lastSpawnAttemptTime = now;
                continue;
            }
            if (lp && locWS == 0xFFFFFFFFu && pl->authoritativeWorldspaceID != 0xFFFFFFFFu)
            {
                TESWorldSpace* lpWS = CALL_MEMBER_FN(reinterpret_cast<TESObjectREFR*>(lp), GetWorldspace)();
                const uint32_t lpWSID = lpWS ? reinterpret_cast<TESForm*>(lpWS)->formID : 0u;
                if (lpWSID != pl->authoritativeWorldspaceID)
                {
                    st.lastSpawnAttemptTime = now;
                    continue;
                }
            }

            const NiPoint3& ap = pl->authoritativePos;
            if (lp && (ap.x != 0.f || ap.y != 0.f || ap.z != 0.f))
            {
                static constexpr float kSpawnRangeGate   = 12288.0f;
                static constexpr float kSpawnRangeGateSq = kSpawnRangeGate * kSpawnRangeGate;
                const float dx = ap.x - lp->pos.x;
                const float dy = ap.y - lp->pos.y;
                const float dz = ap.z - lp->pos.z;
                if (dx * dx + dy * dy + dz * dz > kSpawnRangeGateSq)
                {
                    st.lastSpawnAttemptTime = now;
                    continue;
                }
            }
        }

        LOG_WARN("[PlayerMgr] SpawnWatchdog: entity=%u not spawned %.1fs after task enqueue — re-enqueuing spawn",
            kv.first, now - st.lastSpawnAttemptTime);

        // Respawn where the entity is NOW, not where it was when it first
        // appeared (the HELLO/CREATE position: 0.3.0 host log 16:50:18).
        if (pl && (pl->authoritativePos.x != 0.f || pl->authoritativePos.y != 0.f || pl->authoritativePos.z != 0.f))
            st.lastCreate.spawnPos = pl->authoritativePos;

        st.lastSpawnAttemptTime = now;
        CoSyncSpawnTasks::EnqueueSpawn(
            st.lastCreate.entityID,
            st.lastCreate.baseFormID,
            st.lastCreate.type,
            st.lastCreate.spawnFlags,
            st.lastCreate.spawnPos,
            st.lastCreate.spawnRot
        );
    }
}

static constexpr uint32_t kCompanionEntityBase = 0x00020000u;
static constexpr uint32_t kCompanionEntityMax  = 0x000200FFu;
static uint32_t s_nextCompanionEntityID = kCompanionEntityBase;


static std::unordered_map<uint32_t, uint32_t> s_companionRefToEntityID;

// UPDATE 2026-06-18 (round 23 audit): split into a noinline _Impl plus a thin
// SEH wrapper -- same unguarded cell->objectList raw-loop pattern (dereferencing
// ->formType/->flags/->formID/->baseForm) as the confirmed HostTickObjects bug
// (CoSyncWorldSync.cpp, round 16), called every 2s while host+connected with
// zero protection beforehand.
void CoSyncPlayerManager::HostScanCompanions_Impl(double now)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::HostScanCompanions_Impl", "Currently does nothing on purpose -- safe to call any time, touches no game state.");
    // Companion ownership fix: this used to blindly auto-claim ANY
    // IsPlayerTeammate() actor found in the host's own local cell as
    // host-owned (p.ownerEntityID = m_localEntityID, unconditional),
    // running uncoordinated alongside the correct event-driven system in
    // CoSyncCompanionSync.cpp/Papyrus_CoSync.cpp's NotifyCompanionRecruited,
    // which properly attributes ownership to whoever actually recruited the
    // companion (host or client) via COMP_REQ. Two systems tracking the same
    // actors in separate maps (s_companionRefToEntityID here vs.
    // s_companionOwners in CoSyncCompanionSync.cpp) with zero coordination
    // meant a client-recruited companion could get silently re-claimed by
    // the host here -- confirmed root cause of "whoever gets to the
    // companion first gets to own it, but commands are shared between
    // players". Disabled entirely: the event-driven system is the sole
    // correct path and already handles both spawn (COMP_REQ) and dismiss
    // (COMP_DISMISS via NotifyCompanionDismissed) lifecycle on its own.
    // Left as a no-op stub (not removed) so the call site in Tick(), the SEH
    // wrapper below, and every comment elsewhere in this codebase that
    // references HostScanCompanions by name as a pattern example don't need
    // touching -- trivially revertible if ever needed again.
    (void)now;
}

void CoSyncPlayerManager::HostScanCompanions(double now)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::HostScanCompanions", "Just forwards to the real scan function, wrapped in a safety net since that one isn't guarded itself.");
    __try
    {
        HostScanCompanions_Impl(now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[CompanionSync] HostScanCompanions faulted -- caught, skipping this scan");
    }
}

// UPDATE 2026-06-18 (round 23 audit, exhaustive pass): noinline + SEH guard --
// raw actor->middleProcess->unk08 struct-chain dereference and an unguarded
// loop over data->equipData, same unguarded-engine-access pattern already
// fixed several other times this session. Called from HostSendNpcUpdates for
// every remote NPC, every 50ms while host+connected.
__declspec(noinline) static uint32_t GetEquippedWeaponFormID(Actor* actor)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::GetEquippedWeaponFormID", "Checks the actor and its internal data are valid before reading its equipped-items list. Wrapped in a safety net since that's raw engine memory.");
    __try
    {
        if (!actor || !actor->middleProcess || !actor->middleProcess->unk08)
            return 0;

        Actor::AIProcess::Data08* data = actor->middleProcess->unk08;
        for (UInt32 i = 0; i < data->equipData.count; ++i)
        {
            const Actor::AIProcess::Data08::EquipData& ed = data->equipData[i];
            if (ed.item && ed.item->formType == kFormType_WEAP)
                return ed.item->formID;
        }
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}


struct NpcHealthTrack
{
    float  lastSentHealth = -1.f;
    bool   lastSentDead   = false;
    double lastSentTime   = 0.0;   // drives the periodic unchanged-HP baseline
};
static std::unordered_map<uint32_t, NpcHealthTrack> s_npcHealthStates;
static double s_lastHealthSend = 0.0;

static constexpr uint32_t kHealthAVI_FormID = 0x0002D4;
static ActorValueInfo* GetHealthAVICached()
{
    LOG_REQUIREMENT("CoSyncPlayerManager::GetHealthAVICached", "Needs the game's data to be loaded so it can look up the Health stat definition.");
    static ActorValueInfo* s_avi = nullptr;
    if (!s_avi)
    {
        TESForm* form = LookupFormByID(kHealthAVI_FormID);
        if (form)
            s_avi = static_cast<ActorValueInfo*>(form);
    }
    return s_avi;
}

// 2026-07-21: user reported most NPCs A-posing in a densely populated city
// (Diamond City). Log cross-check found the real gap: of 44 tracked world-
// actor entities, only 9 ever received an AnimVarSystem apply -- the other 35
// completed spawn/RefreshActor3D fine and then simply never got animation
// data again. HostSendNpcUpdates_Impl (below) only sends for an entity when
// `drivesEntity` is true (isLocallyOwned for world-actor IDs) -- if NEITHER
// peer ends up marking an entity locally-owned, nobody ever sends its anim
// data and it sits frozen forever, exactly matching the symptom. This
// diagnostic logs each tracked world-actor entity's ownership state
// (isLocallyOwned/hostAuthoritative/how it was bound) every 5s on whichever
// role calls it, so an actual test can show which specific path leaves
// entities unowned before the ownership logic itself gets touched -- this
// subsystem has been redesigned and reverted three times already (see
// CHANGELOG [0.0.16]/[0.0.17]/[0.0.19] history) without enough diagnostic
// evidence to know why each attempt made things worse.
static void LogEntityOwnershipDiag(double now)
{
    static double s_lastOwnershipDiag = 0.0;
    if (now - s_lastOwnershipDiag < 5.0)
        return;
    s_lastOwnershipDiag = now;

    const auto& states = g_CoSyncPlayerManager.GetStates();
    const auto& players = g_CoSyncPlayerManager.GetAllPlayers();

    int worldActorCount = 0, ownedCount = 0, unownedCount = 0;
    for (const auto& kv : states)
    {
        const uint32_t entityID = kv.first;
        const CoSyncEntityState& st = kv.second;
        if (!CoSyncNpcWorldSync::IsWorldActorEntityID(entityID))
            continue;

        ++worldActorCount;
        const bool owned = st.isLocallyOwned;
        owned ? ++ownedCount : ++unownedCount;

        auto pit = players.find(entityID);
        const bool spawned = (pit != players.end()) && pit->second && pit->second->hasSpawned;

        LOG_UPDATED("[OwnershipDiag] entity=%u isLocallyOwned=%d hostAuthoritative=%d "
            "boundVia=%s reserved0=0x%08X ownerEntityID=%u spawned=%d isHostRole=%d",
            entityID, owned ? 1 : 0, st.hostAuthoritative ? 1 : 0,
            (st.lastCreate.reserved0 != 0 && (st.lastCreate.reserved0 & 0xFF000000u) != 0xFF000000u) ? "direct-bind" : "clone-fallback",
            st.lastCreate.reserved0, st.lastCreate.ownerEntityID, spawned ? 1 : 0,
            CoSyncNet::IsHost() ? 1 : 0);
    }

    LOG_UPDATED("[OwnershipDiag] summary: worldActors=%d owned=%d UNOWNED=%d isHostRole=%d",
        worldActorCount, ownedCount, unownedCount, CoSyncNet::IsHost() ? 1 : 0);
}

// 2026-07-21: every round of the NPC-sync/A-pose investigation so far had to
// manually cross-reference RefreshActor3D log lines against AnimVarSystem
// apply lines by hand (grep + comm) to figure out which specific proxies
// were stuck and where. This puts that same cross-reference directly into
// one periodic log line per tracked world-actor entity, covering the full
// spawn->graph-ready->actively-animating pipeline end to end: whether it's
// spawned, whether IsGraphReady is true RIGHT NOW, how long since its last
// successful animation apply (CoSyncPlayer::m_lastAnimApplyTime, set in
// TickAnimVars only once IsGraphReady passes), and a computed verdict so a
// stuck entity is visible directly in the log instead of needing another
// round of manual log archaeology. Also reports host-side world-actor counts
// specifically so [0.0.58]'s host-only-authority revert can be confirmed
// directly: host should show 0 tracked world actors needing this pipeline
// at all going forward (it only has real, natively-driven actors now).
// [Animations] bDeltaPositionLocomotion in CoSync.ini, default 0 (off --
// preserves the existing captured/relayed graph-Speed behavior exactly).
//
// FO4_Wrld (github.com desktop copy, fw_native/src/hooks/npc_ai_suppress.cpp,
// "Build 68.7 — LOCOMOTION") independently hit and documented the same
// symptom this file's own 2026-08-02 comment above already recorded: a world
// NPC's own graph "Speed" variable reads 0.0 while the actor visibly walks/
// runs across the map -- "the raider slides like a wooden log". Their fix
// (and this one) doesn't try to read a more correct Speed from the graph at
// all; it derives WALKING/RUNNING/IDLE directly from how far the actor's own
// world position actually moved, which is authoritative by construction and
// immune to whatever the graph's var-index table does or doesn't populate
// for combat-AI movement.
//
// Samples over a >=100ms window (60Hz tick jitter can't masquerade as a real
// speed sample) and drops any gap >1s (a teleport/cell-load, not real
// movement) rather than letting it read as a burst of speed. Thresholds are
// vanilla FO4 units/sec (walk ~130, run ~400), matched to FO4_Wrld's own
// calibration since both are measuring the same game's movement speeds.
static bool ReadDeltaPositionLocomotionSetting()
{
    LOG_REQUIREMENT("(global)::ReadDeltaPositionLocomotionSetting", "Reads an optional settings file. If it's missing, it just falls back to off, which keeps the old behavior.");
    static int s_cached = -1;
    if (s_cached >= 0)
        return s_cached != 0;

    char exePath[MAX_PATH] = {};
    if (::GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0)
    {
        s_cached = 0;
        return false;
    }

    std::string ini(exePath);
    const size_t slash = ini.find_last_of("\\/");
    ini = (slash == std::string::npos) ? std::string() : ini.substr(0, slash + 1);
    ini += "Data\\F4SE\\Plugins\\CoSync.ini";

    s_cached = ::GetPrivateProfileIntA("Animations", "bDeltaPositionLocomotion", 0, ini.c_str());
    LOG_INFO("[PlayerMgr] bDeltaPositionLocomotion=%d (from '%s')", s_cached, ini.c_str());
    return s_cached != 0;
}

// Overwrites snap's Speed/SpeedSmoothed/Direction/iState with a locomotion
// state derived from entityID's own position delta since the last call,
// FO4_Wrld-style. No-ops (leaves snap untouched) until a second sample
// arrives at least 100ms apart, so the first call after a spawn or a >1s gap
// only seeds the tracker rather than reporting a bogus instantaneous speed.
static void DeriveNpcLocomotionFromPositionDelta(Actor* actor, uint32_t entityID, const NiPoint3& pos, AnimVarSnapshot& snap)
{
    struct LocoSample
    {
        float x, y, z; ULONGLONG t;
        float dirSmoothed; bool dirValid;
        // Last computed output, reapplied on every call between sample
        // windows -- SaveAnimationVariables/the by-name fallback run every
        // tick (far more often than our >=100ms sample interval), so if we
        // only wrote snap on the tick a fresh sample completes, every OTHER
        // tick would silently fall back through to the broken captured
        // value this function exists to replace.
        float lastSpeed = 0.0f;
        uint32_t lastIState = 0;
    };
    static std::unordered_map<uint32_t, LocoSample> s_loco;

    const ULONGLONG nowTicks = ::GetTickCount64();
    auto it = s_loco.find(entityID);
    if (it == s_loco.end())
    {
        s_loco.emplace(entityID, LocoSample{ pos.x, pos.y, pos.z, nowTicks, 0.0f, false, 0.0f, 0u });
        // No prior sample to diff against yet -- report stationary rather
        // than leaving snap on whatever the (broken) captured path set.
        snap.floats[4] = snap.floats[11] = 0.0f;
        snap.integers[0] = 0;
        snap.isValid = true;
        return;
    }

    const ULONGLONG dt = nowTicks - it->second.t;
    if (dt > 1000)
    {
        // Gap too large to be a real movement sample (teleport/cell load) --
        // reseed rather than reporting whatever huge delta this would compute.
        // Direction smoothing state deliberately survives the reseed: it's a
        // facing-relative angle, not tied to the stale position.
        it->second.x = pos.x; it->second.y = pos.y; it->second.z = pos.z; it->second.t = nowTicks;
        snap.floats[4] = snap.floats[11] = it->second.lastSpeed;
        snap.floats[0] = it->second.dirValid ? it->second.dirSmoothed : 0.0f;
        snap.integers[0] = it->second.lastIState;
        snap.isValid = true;
        return;
    }
    if (dt < 100)
    {
        // Too soon to take a fresh sample without 60Hz jitter reading as
        // speed -- reapply the last derived values rather than leaving snap
        // untouched.
        snap.floats[4] = snap.floats[11] = it->second.lastSpeed;
        snap.floats[0] = it->second.dirValid ? it->second.dirSmoothed : 0.0f;
        snap.integers[0] = it->second.lastIState;
        snap.isValid = true;
        return;
    }

    const float dtSec = static_cast<float>(dt) / 1000.0f;
    const float vx = (pos.x - it->second.x) / dtSec;
    const float vy = (pos.y - it->second.y) / dtSec;
    const float vz = (pos.z - it->second.z) / dtSec;
    it->second.x = pos.x; it->second.y = pos.y; it->second.z = pos.z; it->second.t = nowTicks;

    const float horzSpeed = std::sqrt(vx * vx + vy * vy);

    // Continuous velocity-derived Speed/Direction, same technique and same
    // atan2/facing convention as CoSyncPlayer::TickAnimVars's player-proxy
    // "Movement Direction, recomputed on the proxy" block (this file's own
    // proven fix for the identical "always plays forward regardless of real
    // heading" symptom) -- reused here rather than reinvented so both paths
    // share one verified convention instead of two independently-guessed
    // ones. NOTE (inherited, still open): if a live test on an NPC puppet
    // shows strafe-left/right swapped, negate `rel`; if forward/back are
    // swapped, add pi.
    constexpr float kPi = 3.14159265358979f;
    if (horzSpeed > 8.0f)
    {
        const float velHeading = std::atan2(vx, vy);
        const float facing     = actor ? actor->rot.z : 0.0f;
        float rel = velHeading - facing;
        while (rel >  kPi) rel -= 2.0f * kPi;
        while (rel < -kPi) rel += 2.0f * kPi;

        if (!it->second.dirValid)
        {
            it->second.dirSmoothed = rel;
            it->second.dirValid = true;
        }
        else
        {
            float d = rel - it->second.dirSmoothed;
            while (d >  kPi) d -= 2.0f * kPi;
            while (d < -kPi) d += 2.0f * kPi;
            it->second.dirSmoothed += d * 0.35f;
            while (it->second.dirSmoothed >  kPi) it->second.dirSmoothed -= 2.0f * kPi;
            while (it->second.dirSmoothed < -kPi) it->second.dirSmoothed += 2.0f * kPi;
        }
    }
    // else: below the noise floor -- leave dirSmoothed at its last real
    // heading rather than snapping toward 0, so a brief pause mid-stride
    // doesn't visibly re-aim the character.

    const float clampedSpeed = (horzSpeed > 600.0f) ? 600.0f : horzSpeed;
    const uint32_t iState = (clampedSpeed > 30.0f) ? 190u : 0u; // gates the
        // NPC blend tree branch the same way the already-active raw path's
        // own npcKiState does.

    it->second.lastSpeed  = clampedSpeed;
    it->second.lastIState = iState;

    snap.floats[4]   = clampedSpeed;              // Speed
    snap.floats[11]  = clampedSpeed;               // SpeedSmoothed
    snap.floats[0]   = it->second.dirValid ? it->second.dirSmoothed : 0.0f; // Direction
    snap.integers[0] = iState;
    snap.isValid     = true;
}

static void LogNpcSyncHealthDiag(double now)
{
    static double s_lastNpcHealthDiag = 0.0;
    if (now - s_lastNpcHealthDiag < 5.0)
        return;
    s_lastNpcHealthDiag = now;

    const auto& players = g_CoSyncPlayerManager.GetAllPlayers();
    const auto& states  = g_CoSyncPlayerManager.GetStates();

    int total = 0, notSpawned = 0, initializing = 0, graphReady = 0, stuck = 0;
    for (const auto& kv : players)
    {
        const uint32_t entityID = kv.first;
        if (!CoSyncNpcWorldSync::IsWorldActorEntityID(entityID))
            continue;
        const CoSyncPlayer* p = kv.second.get();
        if (!p) continue;

        // 2026-07-21: entities THIS machine locally owns/drives (real,
        // natively-AI-driven actors -- host's own discovered NPCs under the
        // [0.0.58] host-only-authority model) deliberately skip TickAnimVars
        // entirely per [0.0.21]'s established design (they don't need it --
        // native AI drives them). m_spawnTime/m_lastAnimApplyTime are
        // meaningless for them (never set, since they were bound not
        // spawned) -- confirmed live: host showed these as permanently
        // "STUCK" at spawnAge=17556s, a diagnostic false positive, not a
        // real bug. Only clones (not locally owned) actually depend on this
        // pipeline, so only they're meaningful to report here.
        auto sit = states.find(entityID);
        if (sit != states.end() && sit->second.isLocallyOwned)
            continue;

        ++total;

        if (!p->hasSpawned || !p->actorRef)
        {
            ++notSpawned;
            LOG_UPDATED("[NpcSyncDiag] entity=%u NOT_SPAWNED isHostRole=%d",
                entityID, CoSyncNet::IsHost() ? 1 : 0);
            continue;
        }

        const double spawnAge = now - p->m_spawnTime;
        const bool ready = CoSyncAnimUtils::IsGraphReady(p->actorRef);
        const double sinceApply = (p->m_lastAnimApplyTime > 0.0) ? (now - p->m_lastAnimApplyTime) : -1.0;

        const char* verdict;
        if (spawnAge < 2.0)                              { verdict = "INITIALIZING"; ++initializing; }
        else if (p->m_lastAnimApplyTime > 0.0 && sinceApply < 5.0) { verdict = "OK";           ++graphReady;  }
        else                                              { verdict = "STUCK";        ++stuck;       }

        LOG_UPDATED("[NpcSyncDiag] entity=%u %s spawnAge=%.1fs graphReadyNow=%d neverApplied=%d sinceLastApply=%.1fs retried3D=%d isHostRole=%d",
            entityID, verdict, spawnAge, ready ? 1 : 0,
            (p->m_lastAnimApplyTime == 0.0) ? 1 : 0, sinceApply,
            p->m_3dLoadRetried ? 1 : 0, CoSyncNet::IsHost() ? 1 : 0);
    }

    LOG_UPDATED("[NpcSyncDiag] summary: total=%d notSpawned=%d initializing=%d ok=%d STUCK=%d isHostRole=%d",
        total, notSpawned, initializing, graphReady, stuck, CoSyncNet::IsHost() ? 1 : 0);
}

// UPDATE 2026-06-18 (round 23 audit, exhaustive pass): split into a noinline
// _Impl plus a thin SEH wrapper -- this loop touches actor->actorValueOwner,
// GetEquippedWeaponFormID (now individually guarded above), and
// CoSyncInventorySync::SnapshotInventory (separately guarded in its own
// file) for every remote NPC/companion, every 50ms while host+connected,
// with no outer backstop before this fix.
void CoSyncPlayerManager::HostSendNpcUpdates_Impl(double now)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::HostSendNpcUpdates_Impl", "Needs an active connection. Only touches an actor once it's spawned and this machine is the one driving it. Wrapped in a safety net by the caller.");
    if (!CoSyncNet::IsConnected())
        return;

    static double s_lastSend = 0.0;
    if ((now - s_lastSend) < 0.05)
        return;
    s_lastSend = now;

    // 2026-07-21: user confirmed (via [OwnershipDiag]) the host correctly
    // owns all 45 world-actor entities -- isLocallyOwned isn't the blocker
    // for this send loop. Counting every OTHER reason an owned entity can
    // still get skipped here, to find which one is actually eating the
    // ~35 of 45 that never get an update sent (matching the client-side
    // finding that only 9/44 ever receive an AnimVarSystem apply).
    static double s_lastSendDiag = 0.0;
    static int s_dNotSpawned = 0, s_dNoState = 0, s_dNotWorldType = 0,
               s_dNotDriving = 0, s_dNotRemoteControlled = 0, s_dTransformFail = 0, s_dSent = 0,
               s_dOutOfRange = 0, s_dRotationSkipped = 0;

    // 2026-08-16 (FO4_Wrld-informed): every world-actor entity used to get a
    // full EntityUpdatePacket (position + weapon + throwable + combat target +
    // a complete anim-var snapshot) sent unconditionally on EVERY call to this
    // function (20Hz) -- bandwidth scaled linearly with tracked-entity count
    // with no ceiling. Live-measured: 140 tracked world actors produced
    // 1000+ updates/sec combined (~148MB/4min, GNS lock held up to 68.6ms),
    // which is what forced kMaxConcurrentWorldActors down to a blunt 15-actor
    // hard cap (silently dropping enemies once a fight got big). A separate
    // FO4 co-op project (FO4_Wrld) hit the identical shape of bug from the
    // opposite direction -- a fixed per-tick batch size too small for its
    // population, silently starving 12 of 29 NPCs of any position ever -- and
    // fixed it by bounding per-tick send volume and rotating through the full
    // tracked set, rather than either sending everyone unconditionally (this)
    // or hard-capping how many can be tracked at all. Round-robin a bounded
    // number of world-actor sends per call here: every entity still gets
    // updated, just at a lower effective rate as the population grows (24/tick
    // @ 20Hz call rate = 480 sends/sec worst case, well under the measured
    // overload threshold; worst-case per-actor update interval is bounded by
    // trackedCount/kMaxWorldActorSendsPerTick calls). Players and companions
    // are exempt -- few in number, need full responsiveness.
    static constexpr size_t kMaxWorldActorSendsPerTick = 24;
    static uint32_t s_worldActorRotationCursor = 0;
    std::unordered_set<uint32_t> allowedWorldActorsThisTick;
    {
        std::vector<uint32_t> worldActorIDs;
        worldActorIDs.reserve(m_playersByEntityID.size());
        for (auto& kv : m_playersByEntityID)
        {
            if (CoSyncNpcWorldSync::IsWorldActorEntityID(kv.first))
                worldActorIDs.push_back(kv.first);
        }
        if (worldActorIDs.size() <= kMaxWorldActorSendsPerTick)
        {
            allowedWorldActorsThisTick.insert(worldActorIDs.begin(), worldActorIDs.end());
        }
        else
        {
            std::sort(worldActorIDs.begin(), worldActorIDs.end());
            const size_t n = worldActorIDs.size();
            for (size_t i = 0; i < kMaxWorldActorSendsPerTick; ++i)
                allowedWorldActorsThisTick.insert(worldActorIDs[(s_worldActorRotationCursor + i) % n]);
            s_worldActorRotationCursor = static_cast<uint32_t>((s_worldActorRotationCursor + kMaxWorldActorSendsPerTick) % n);
        }
    }

    for (auto& kv : m_playersByEntityID)
    {
        CoSyncPlayer& npc = *kv.second;

        if (npc.entityID == kDebugNpcEntityID)
            continue;

        if (!npc.hasSpawned || !npc.actorRef)
        {
            ++s_dNotSpawned;
            continue;
        }

        auto it = m_statesByEntityID.find(npc.entityID);
        if (it == m_statesByEntityID.end() || !it->second.hasCreate)
        {
            ++s_dNoState;
            continue;
        }

        const CoSyncEntityState& st = it->second;

        const bool isWorldActor = (st.lastCreate.type == CoSyncEntityType::NPC ||
                                    st.lastCreate.type == CoSyncEntityType::Companion);
        if (!isWorldActor)
        {
            ++s_dNotWorldType;
            continue;
        }

        const bool drivesEntity = CoSyncNpcWorldSync::IsWorldActorEntityID(npc.entityID)
            ? st.isLocallyOwned
            : (CoSyncNet::IsHost() && st.hostAuthoritative);
        if (!drivesEntity)
        {
            ++s_dNotDriving;
            continue;
        }


        if (st.lastCreate.type == CoSyncEntityType::NPC &&
            !EntityCreatePacket::HasFlag(st.lastCreate.spawnFlags, EntityCreatePacket::RemoteControlled))
        {
            ++s_dNotRemoteControlled;
            continue;
        }

        // Corpses are driven by ragdoll on every machine; streaming transforms
        // and anim vars for them only drags the remote corpse around.
        if (CoSyncNpcWorldSync::IsWorldActorEntityID(npc.entityID) &&
            CoSyncNpcWorldSync::IsEntityDead(npc.entityID))
        {
            // ...but the settled corpse position is reported once (K4).
            CoSyncHealthSync::TickOwnedCorpse(npc.entityID, npc.actorRef, now);
            continue;
        }

        NiPoint3 pos(0.f, 0.f, 0.f);
        NiPoint3 rot(0.f, 0.f, 0.f);

        if (!CoSyncGameAPI::GetActorWorldTransform(npc.actorRef, pos, rot))
        {
            ++s_dTransformFail;
            continue;
        }

        // Range cap: only stream a world NPC that is near SOME player. Bounds
        // bandwidth so the NPCs that matter update faster/smoother, and stops
        // syncing stale/distant registrations (a build-226 test streamed NPCs
        // ~17,000u away while the players fought elsewhere). ~8192u ≈ 2 exterior
        // cells; FalloutTogether caps at 8000. Companions are exempt (always
        // near their owner and few in number).
        if (CoSyncNpcWorldSync::IsWorldActorEntityID(npc.entityID))
        {
            if (!allowedWorldActorsThisTick.count(npc.entityID))
            {
                ++s_dRotationSkipped;
                continue;
            }

            // 2026-08-08: this cap was 8192, but NpcWorldSync DISCOVERS creatures
            // out to 16384 and KEEPS them out to 20480 -- so every creature in the
            // 8192..20480 band was registered + broadcast (a puppet spawned on the
            // client) yet NEVER streamed a position, leaving the puppet frozen at
            // its spawn point while the host's real AI-alive creature walked away.
            // Live proof: host at (-18586,-21909), creatures at (-14869,-29788)
            // etc. = ~8700-18000u away, SendDiag showed sent=0 outOfRange=450.
            // The cap must not be TIGHTER than the keep radius or discovered
            // creatures silently never sync. Matched to the 20480 keep radius.
            static constexpr float kMaxNpcSyncDistSq = 20480.0f * 20480.0f;
            float bestSq = 3.4e38f;
            if (PlayerCharacter* lp = CoSyncGetPlayer())
            {
                const float dx = lp->pos.x - pos.x, dy = lp->pos.y - pos.y, dz = lp->pos.z - pos.z;
                bestSq = dx * dx + dy * dy + dz * dz;
            }
            for (auto& kvP : m_playersByEntityID)
            {
                CoSyncPlayer& other = *kvP.second;
                if (other.createType != CoSyncEntityType::Player || !other.hasSpawned || !other.actorRef)
                    continue;
                const float dx = other.actorRef->pos.x - pos.x, dy = other.actorRef->pos.y - pos.y, dz = other.actorRef->pos.z - pos.z;
                const float d = dx * dx + dy * dy + dz * dz;
                if (d < bestSq) bestSq = d;
            }
            if (bestSq > kMaxNpcSyncDistSq)
            {
                ++s_dOutOfRange;
                continue;
            }
        }

        ++s_dSent;


        uint32_t weapFormID = GetEquippedWeaponFormID(npc.actorRef);

        EntityUpdatePacket u{};
        u.entityID              = npc.entityID;
        u.pos                   = pos;
        u.rot                   = rot;
        u.vel                   = NiPoint3(0.f, 0.f, 0.f);
        u.timestamp             = now;
        u.equippedWeaponFormID  = weapFormID;
        // Raiders and gunners throw Molotovs and frag grenades constantly, so
        // host-owned NPCs need the same throwable field the local player gets
        // in CoSyncNet::SendMyEntityUpdate -- otherwise their throw animation
        // on peers would fall back to the grenade clip for every throwable.
        // Note this cannot reuse weapFormID: GetEquippedWeaponFormID stops at
        // the first WEAP (the gun), and a throw equips the grenade in a second
        // slot alongside it.
        u.equippedThrowableFormID =
            CoSyncThrowableSync::GetEquippedThrowableFormID(npc.actorRef);
        // By-name, for the same reason as the local player in
        // CoSyncNet::SendMyEntityUpdate -- animVars bit 21's hardcoded graph
        // index never went high in the 2026-08-04 test.
        {
            // Discrete anim events (fire/reload/throw) for host-owned NPCs go
            // out as AEV| one-shots, same as the local player. This runs at
            // the NPC update rate rather than frame rate, so a very short
            // isFiring window can still be missed for an NPC -- acceptable for
            // now, and strictly better than the level-sampling it replaces.
            static std::unordered_map<uint32_t, CoSyncNet::AnimEdgeState> s_npcAnimEdges;
            CoSyncNet::SendAnimEdges(npc.actorRef, npc.entityID, s_npcAnimEdges[npc.entityID]);
        }
        u.isCombatActive        = npc.actorRef->IsInCombat();

        if (u.isCombatActive)
        {
            // ── Real combat target first ─────────────────────────────────────
            // Ask the engine who this NPC is ACTUALLY fighting (Actor+0x380
            // handle, IDA-verified) and map that actor to a known entityID: the
            // local player, a player proxy, or another synced world NPC. This
            // makes every peer's puppet attack the same actor the host's NPC is
            // attacking -- including NPC-vs-NPC fights (a Deathclaw mauling a
            // Super Mutant) -- instead of just "nearest player".
            u.combatTargetEntityID = 0;
            Actor* realTarget = CoSyncGameAPI::GetCombatTargetActor(npc.actorRef);
            if (realTarget)
            {
                PlayerCharacter* lpForCmp = CoSyncGetPlayer();
                if (lpForCmp && realTarget == reinterpret_cast<Actor*>(lpForCmp))
                    u.combatTargetEntityID = CoSyncNet::GetMyEntityID();
                else
                    u.combatTargetEntityID = g_CoSyncEntities.GetEntityID(realTarget);
            }

            // ── Nearest-player fallback ──────────────────────────────────────
            // Used only when the real target doesn't resolve to a synced entity
            // (e.g. the NPC is fighting an unsynced local critter). The puppet
            // still needs SOMEONE to fight so it doesn't stand idle while
            // isCombatActive is true, so aim it at the closest player.
            if (u.combatTargetEntityID == 0)
            {
                float bestDistSq = -1.f;

                PlayerCharacter* localPlayer = CoSyncGetPlayer();
                NiPoint3 lp, lr;
                if (localPlayer && CoSyncGameAPI::GetActorWorldTransform(reinterpret_cast<Actor*>(localPlayer), lp, lr))
                {
                    const float dx = lp.x - pos.x, dy = lp.y - pos.y, dz = lp.z - pos.z;
                    bestDistSq = dx * dx + dy * dy + dz * dz;
                    u.combatTargetEntityID = CoSyncNet::GetMyEntityID();
                }

                for (auto& kvPlayer : m_playersByEntityID)
                {
                    CoSyncPlayer& pl = *kvPlayer.second;
                    if (pl.createType != CoSyncEntityType::Player || !pl.hasSpawned || !pl.actorRef)
                        continue;

                    NiPoint3 pp, pr;
                    if (!CoSyncGameAPI::GetActorWorldTransform(pl.actorRef, pp, pr))
                        continue;

                    const float dx = pp.x - pos.x, dy = pp.y - pos.y, dz = pp.z - pos.z;
                    const float distSq = dx * dx + dy * dy + dz * dz;
                    if (bestDistSq < 0.f || distSq < bestDistSq)
                    {
                        bestDistSq = distSq;
                        u.combatTargetEntityID = pl.entityID;
                    }
                }
            }

            // 2026-08-16 pt.8 diag: "enemies only aggro the host" report --
            // this proves whether the OWNER's own native AI ever actually
            // picks a non-owner peer as a target at all, vs. the puppet-side
            // mirroring being at fault. Throttled per-entity so a long fight
            // against the same target doesn't spam.
            if (CoSyncNpcWorldSync::IsWorldActorEntityID(npc.entityID))
            {
                static std::unordered_map<uint32_t, uint32_t> s_lastLoggedTarget;
                uint32_t& lastLogged = s_lastLoggedTarget[npc.entityID];
                if (lastLogged != u.combatTargetEntityID)
                {
                    lastLogged = u.combatTargetEntityID;
                    LOG_UPDATED("[AggroDiag] entity=%u realTarget=%p resolvedTargetEntity=%u (myEntityID=%u)",
                        npc.entityID, static_cast<void*>(realTarget), u.combatTargetEntityID,
                        CoSyncNet::GetMyEntityID());
                }
            }
        }

        {
            AnimVarSnapshot snap{};
            if (CoSyncAnimVarSystem::SaveAnimationVariables(npc.actorRef, snap))
            {
                u.hasAnimVars = true;
                u.animVars    = snap;
            }

            // 2026-08-02: every NPC snapshot leaving this machine reads
            // speed=0.0 while the host's own raiders visibly A-pose. Log the
            // by-name-resolved index/value against the hardcoded one for a
            // couple of world NPCs so the next session says outright whether
            // the index table is wrong for these graphs or the actors are
            // genuinely frozen. Throttled hard -- diagnostic only.
            static double s_lastCaptureDiag = 0.0;
            static int    s_captureDiagCount = 0;
            if (CoSyncNpcWorldSync::IsWorldActorEntityID(npc.entityID) &&
                (now - s_lastCaptureDiag) > 3.0 && s_captureDiagCount < 40)
            {
                s_lastCaptureDiag = now;
                ++s_captureDiagCount;
                CoSyncAnimVarSystem::DiagnoseCaptureIndices(npc.actorRef, npc.entityID);
            }
            else
            {


                float fVal = 0.f;
                int   iVal = 0;
                bool  bVal = false;
                if (CoSyncAnimUtils::GetFloat(npc.actorRef, CoSyncAnimUtils::Vars::Speed,        fVal)) snap.floats[4]  = fVal;
                if (CoSyncAnimUtils::GetFloat(npc.actorRef, CoSyncAnimUtils::Vars::SpeedSmoothed, fVal)) snap.floats[11] = fVal;
                if (CoSyncAnimUtils::GetFloat(npc.actorRef, CoSyncAnimUtils::Vars::Direction,    fVal)) snap.floats[0]  = fVal;
                if (CoSyncAnimUtils::GetInt  (npc.actorRef, CoSyncAnimUtils::Vars::iState,       iVal)) snap.integers[0]= iVal;
                if (CoSyncAnimUtils::GetBool (npc.actorRef, CoSyncAnimUtils::Vars::bIsSneaking,  bVal) && bVal)
                    snap.booleans |= (1ULL << 11);
                snap.isValid  = true;
                u.hasAnimVars = true;
                u.animVars    = snap;
            }

            // Opt-in override -- see ReadDeltaPositionLocomotionSetting's doc
            // comment. Runs after both capture paths above so it always wins
            // when enabled, regardless of which one the graph-index diagnostic
            // branch happened to take this tick.
            if (CoSyncNpcWorldSync::IsWorldActorEntityID(npc.entityID) &&
                ReadDeltaPositionLocomotionSetting())
            {
                DeriveNpcLocomotionFromPositionDelta(npc.actorRef, npc.entityID, pos, u.animVars);
                u.hasAnimVars = true;
            }
        }

        CoSyncTransport::Send(SerializeEntityUpdate(u));
    }

    if (now - s_lastSendDiag >= 5.0)
    {
        LOG_UPDATED("[SendDiag] last 5s: sent=%d notSpawned=%d noState=%d notWorldType=%d "
            "notDriving=%d notRemoteControlled=%d transformFail=%d outOfRange=%d rotationSkipped=%d",
            s_dSent, s_dNotSpawned, s_dNoState, s_dNotWorldType,
            s_dNotDriving, s_dNotRemoteControlled, s_dTransformFail, s_dOutOfRange, s_dRotationSkipped);
        s_lastSendDiag = now;
        s_dNotSpawned = s_dNoState = s_dNotWorldType = s_dNotDriving =
            s_dNotRemoteControlled = s_dTransformFail = s_dSent = s_dOutOfRange = s_dRotationSkipped = 0;
    }

    if ((now - s_lastHealthSend) < 0.5)
        return;
    s_lastHealthSend = now;

    ActorValueInfo* healthAVI = GetHealthAVICached();
    if (!healthAVI)
    {
        // 2026-08-02: this used to return silently. A 04:56 session logged 7
        // registered NPCs, 480 position updates/5s and notDriving=0, yet ZERO
        // HP| packets for any world NPC -- the client therefore had no enemy
        // health data at all ("enemy health not in sync"). A silent return here
        // is one of only two ways that can happen, and it left no trace.
        static double s_lastAviWarn = 0.0;
        if (now - s_lastAviWarn > 10.0)
        {
            s_lastAviWarn = now;
            LOG_UPDATED_WARN("[HealthSync] Skipping NPC health updates — couldn't find the Health stat (0x%08X)",
                kHealthAVI_FormID);
        }
        return;
    }

    // Counts why NPC health broadcasts do/don't leave this machine. Mirrors
    // SendDiag, which is what made the position path debuggable.
    static double s_lastHpDiag = 0.0;
    static int s_hpSent = 0, s_hpNoActor = 0, s_hpNoState = 0,
               s_hpNotType = 0, s_hpNotDriving = 0, s_hpReadFail = 0, s_hpUnchanged = 0;

    for (auto& kv : m_playersByEntityID)
    {
        CoSyncPlayer& npc = *kv.second;

        if (!npc.hasSpawned || !npc.actorRef)
        {
            ++s_hpNoActor;
            continue;
        }

        if (npc.entityID == kDebugNpcEntityID)
            continue;

        auto it = m_statesByEntityID.find(npc.entityID);
        if (it == m_statesByEntityID.end() || !it->second.hasCreate)
        {
            ++s_hpNoState;
            continue;
        }

        const auto& createType = it->second.lastCreate.type;
        if (createType != CoSyncEntityType::NPC && createType != CoSyncEntityType::Companion)
        {
            ++s_hpNotType;
            continue;
        }

        const bool drivesHealth = CoSyncNpcWorldSync::IsWorldActorEntityID(npc.entityID)
            ? it->second.isLocallyOwned
            : (CoSyncNet::IsHost() && it->second.hostAuthoritative);
        if (!drivesHealth)
        {
            ++s_hpNotDriving;
            continue;
        }

        Actor* actor = npc.actorRef;
        // round 30: direct actorValueOwner touches on a CoSync proxy can
        // crash at Fallout4.exe+0xCC934D (AI permanently disabled on these
        // actors -- see CoSyncActorValueSafety.h). Route through the SEH-safe
        // wrappers instead of calling GetValue/GetMaximum directly.
        // round 126: track actors whose actorValueOwner is permanently broken
        // (crashes every poll) and skip them for the rest of the session.
        static std::unordered_set<Actor*> s_badHealthActors;
        if (s_badHealthActors.count(actor))
            continue;
        float current = 0.f, maxHP = 0.f;
        if (!CoSyncAVSafety::SafeGetValue(actor, healthAVI, current))
        {
            s_badHealthActors.insert(actor);
            ++s_hpReadFail;
            continue;
        }
        if (!CoSyncAVSafety::SafeGetMaximum(actor, healthAVI, maxHP))
            s_badHealthActors.insert(actor);

        NpcHealthTrack& track = s_npcHealthStates[npc.entityID];

        bool isDead = (current <= 0.f);


        if (isDead && !track.lastSentDead)
        {
            track.lastSentDead = true;
            CoSyncTransport::Send(CoSyncHealthSync::SerializeKill(npc.entityID));
            LOG_UPDATED("[HealthSync] TX KILL entity=%u", npc.entityID);


            auto items = CoSyncInventorySync::SnapshotInventory(actor);
            if (!items.empty())
            {

                CoSyncLootSync::HostBroadcastPersonalLoot(npc.entityID, items);
                LOG_UPDATED("[LootSync] TX personal loot for entity=%u base=%u items",
                    npc.entityID, (unsigned)items.size());
            }
            continue;
        }

        if (isDead)
            continue;

        track.lastSentDead = false;


        // 2026-08-02: HP used to go out ONLY on a >0.5 delta, so a peer that
        // bound late, rebound after a cell transition, or simply dropped the
        // packet had no health value for an enemy until something damaged it.
        // A 05:16 host log showed unchanged=50 with sent=0 for exactly this
        // reason. Re-send an unchanged value every few seconds as a baseline so
        // late/recovering peers converge without needing combat to happen.
        static constexpr double kHpBaselineInterval = 4.0;
        const bool hpUnchanged =
            (std::abs(current - track.lastSentHealth) < 0.5f && track.lastSentHealth >= 0.f);
        if (hpUnchanged && (now - track.lastSentTime) < kHpBaselineInterval)
        {
            ++s_hpUnchanged;
            continue;
        }

        track.lastSentTime   = now;
        track.lastSentHealth = current;
        ++s_hpSent;
        CoSyncTransport::Send(CoSyncHealthSync::SerializeHP(npc.entityID, current, maxHP));
    }

    if (now - s_lastHpDiag >= 5.0)
    {
        LOG_UPDATED("[HpDiag] last 5s: sent=%d noActor=%d noState=%d notType=%d "
            "notDriving=%d readFail=%d unchanged=%d",
            s_hpSent, s_hpNoActor, s_hpNoState, s_hpNotType,
            s_hpNotDriving, s_hpReadFail, s_hpUnchanged);
        s_lastHpDiag = now;
        s_hpSent = s_hpNoActor = s_hpNoState = s_hpNotType =
            s_hpNotDriving = s_hpReadFail = s_hpUnchanged = 0;
    }
}

void CoSyncPlayerManager::HostSendNpcUpdates(double now)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::HostSendNpcUpdates", "Just forwards to the real update function, wrapped in a safety net since it reads raw engine data for every tracked NPC.");
    __try
    {
        HostSendNpcUpdates_Impl(now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[PlayerManager] HostSendNpcUpdates faulted -- caught, skipping this update");
    }
}

static void HostDebugNpcHardSnap(
    std::unordered_map<uint32_t, std::unique_ptr<CoSyncPlayer>>& playersByEntityID,
    std::unordered_map<uint32_t, CoSyncEntityState>& statesByEntityID,
    uint32_t localEntityID)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::HostDebugNpcHardSnap", "Needs a spawned remote player proxy and a spawned debug NPC before it can move the NPC to match.");

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

// Combat-toggle (StartCombat/StopCombat) faults the Papyrus VM on proxies that
// have no VM script object: AI-disabled world-NPC clones created via
// CreateReferenceAtLocation never get a script instance, so the VM's handle
// lookup returns null and ExecuteFunction does `call [rax+0x190]` with rax=0
// (code=0xC0000005, confirmed 2026-08-02 client crash on the first enemy
// combat-entry — lastTickStep=PlayerMgr:Tick:entity-loop:CombatToggle). The
// entity-loop's IsFormStillLive guard passes here because the form IS live —
// it just lacks the VM binding — so only SEH can catch this. Split into a
// noinline _Impl (holds VMArray/VMVariable, which have destructors and can't
// share a __try scope — the established _Impl/Safe pattern in this file) plus a
// thin SEH wrapper. target==nullptr issues StopCombat (no args); a non-null
// target issues StartCombat(target).
__declspec(noinline) static void DoCombatToggle_Impl(Actor* actor, const BSFixedString& fn, Actor* target)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::DoCombatToggle_Impl", "Needs a real actor and a valid script function name to start or stop combat. Some proxies lack a script object and can crash the scripting engine, so the caller wraps this in a safety net.");
    VMArray<VMVariable> args;
    if (target)
    {
        VMVariable vTarget;
        vTarget.Set(&target);
        args.Push(&vTarget);
    }
    CallFunctionNoWait(actor, fn, args);
}

static bool SafeCombatToggle(Actor* actor, const BSFixedString& fn, Actor* target)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::SafeCombatToggle", "Wraps the combat toggle in a safety net so a script-less proxy fails gracefully instead of crashing the game.");
    __try { DoCombatToggle_Impl(actor, fn, target); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Clears a proxy whose actor the engine has freed. Returns true if it did.
// Guard against stale proxy actors of ANY entity type. When the local
// engine unloads a cell (transition, or the remote player moving
// beyond this machine's loaded grid), it frees the actors in it.
// TickAnimVars/TickSmoothing/ApplyPendingTransformIfAny dereference
// actorRef directly; on a freed actor the READ AV lands at
// 0xFFFF...FFFF. IsFormStillLive is SEH-wrapped so it's safe even on
// freed pointers. World-actor entities additionally drop spawnQueued
// so the watchdog doesn't respawn a clone the engine just unloaded
// (they re-bind via the owner's CREATE re-broadcast instead); player
// proxies keep spawnQueued so the watchdog can respawn them once the
// remote player is back within loaded range.
bool CoSyncPlayerManager::ReleaseIfFreed(CoSyncPlayer& ent)
{
    if (!ent.hasSpawned || !ent.actorRef || ent.isLocalPlayer)
        return false;
    if (!CoSyncGameAPI::IsFormStillLive(static_cast<TESForm*>(ent.actorRef)))
    {
        LOG_WARN("[PlayerMgr] entity=%u actor=%p freed (unloaded) — clearing",
            ent.entityID, static_cast<void*>(ent.actorRef));
        Actor* const freedActor = ent.actorRef;
        ent.actorRef   = nullptr;
        ent.hasSpawned = false;
        // L2 (2026-09-26): release every other index still keyed by the
        // dead actor, exactly as DespawnEntity does. Left behind, the
        // registry refused the respawned actor ("already registered",
        // runtime log 16708 12:57:45) and the TickHook / bone-cache
        // entries pointed at freed memory. All three only use the
        // pointer as a lookup key (the bone cache's nodes are pinned by
        // our own reference, so releasing them is correct).
        g_CoSyncEntities.Remove(ent.entityID);
        UnregisterRemoteActor(freedActor);
        CoSyncPlayerAnimation::ReleaseBoneCache(freedActor);
        // The rebuilt replacement actor gets a fresh body -- any
        // outfit/weapon marked "applied" against the OLD actor is
        // stale. Outfit's cache is keyed by entityID only (no actor
        // identity), so without this it silently never re-equips
        // (confirmed: FLUSH kept logging "confirmed" with a visibly
        // bare proxy after a reload rebuilt it).
        CoSyncOutfitSync::ClearAppliedOutfits(ent.entityID);
        if (CoSyncNpcWorldSync::IsWorldActorEntityID(ent.entityID))
        {
            auto stIt = m_statesByEntityID.find(ent.entityID);
            if (stIt != m_statesByEntityID.end())
                stIt->second.spawnQueued = false;
            // N1: "re-bind via the owner's CREATE re-broadcast" only
            // works if NpcWorldSync forgets the dead binding too --
            // its "already bound" guard otherwise rejects every
            // re-bind for the rest of the session.
            CoSyncNpcWorldSync::ForgetBinding(ent.entityID);
        }
        return true;
    }
    return false;
}

void CoSyncPlayerManager::Tick()
{
    LOG_REQUIREMENT("CoSyncPlayerManager::Tick", "Runs each per-entity step in turn (position, animation, weapons, combat, dialog, pipboy); each step checks its own actor is valid. A breadcrumb marks which step is running so a crash shows exactly where it happened.");
    static const BSFixedString kUnequipItem("UnequipItem");
    static const BSFixedString kEquipItem("EquipItem");
    static const BSFixedString kStartCombat("StartCombat");
    static const BSFixedString kStopCombat("StopCombat");
    // Drop engine-freed actors before ANY step below touches actorRef.
    // This sweep used to run only in the entity loop at the END of Tick, so
    // an actor unloaded since the previous frame was still dereferenced by
    // the steps before it -- HostSendNpcUpdates -> SendAnimEdges ->
    // CoSyncAnimUtils::Is3DLoaded made a vcall on a freed actor (client
    // READ AV at 0xFFFF...FFFF, 20:47:57 2026-09-26, lastTickStep
    // AnimVarSystem:SaveAnimationVariables:exit).
    g_lastTickStep = "PlayerMgr:Tick:ReleaseFreedActors";
    for (auto& kv : m_playersByEntityID)
        ReleaseIfFreed(*kv.second);

    g_lastTickStep = "PlayerMgr:Tick:ProcessInbox";
    ProcessInbox();


    // Round 135: DEBUG NPC RETIRED. This spawned a test human (entity 36865)
    // on every host connect — it's the "second remote NPC that spawns but
    // nobody controls" the user reported now that proxies are finally
    // visible and animated. It served its purpose as spawn-pipeline test
    // scaffolding; the real player proxy exercises the same path now.
    // (MakeHostDebugNpcCreate kept for manual debugging.)
    (void)&MakeHostDebugNpcCreate;

    g_lastTickStep = "PlayerMgr:Tick:PumpDeferredSpawns";
    PumpDeferredSpawns();


    g_lastTickStep = "PlayerMgr:Tick:FlushHealthOps";
    CoSyncHealthSync::FlushPendingHealthOps();
    g_lastTickStep = "PlayerMgr:Tick:FlushInventoryOps";
    CoSyncInventorySync::FlushPendingInventoryOps();
    g_lastTickStep = "PlayerMgr:Tick:FlushMapOps";
    CoSyncMapSync::FlushPendingMapOps();
    g_lastTickStep = "PlayerMgr:Tick:FlushConsumableOps";
    CoSyncConsumableSync::FlushPendingConsumableOps();
    g_lastTickStep = "PlayerMgr:Tick:FlushLegendaryOps";
    CoSyncNpcWorldSync::FlushPendingLegendaryOps();
    g_lastTickStep = "PlayerMgr:Tick:FlushContainerOps";
    CoSyncContainerSync::FlushPendingContainerOps();

    const double now = NowSeconds();
    g_lastTickStep = "PlayerMgr:Tick:PlayerMarker";
    CoSyncPlayerMarker::Tick(now);
    g_lastTickStep = "PlayerMgr:Tick:FlushTeleport";
    CoSyncTeleport::FlushPendingTeleport();
    g_lastTickStep = "PlayerMgr:Tick:HandleTimeouts";
    HandleTimeouts(now);

    g_lastTickStep = "PlayerMgr:Tick:SpawnWatchdog";
    SpawnWatchdog(now);

    g_lastTickStep = "PlayerMgr:Tick:HostScanCompanions";
    HostScanCompanions(now);


    g_lastTickStep = "PlayerMgr:Tick:HostSendNpcUpdates";
    HostSendNpcUpdates(now);

    g_lastTickStep = "PlayerMgr:Tick:OwnershipDiag";
    LogEntityOwnershipDiag(now);

    g_lastTickStep = "PlayerMgr:Tick:NpcSyncHealthDiag";
    LogNpcSyncHealthDiag(now);


    if (CoSyncNet::IsHost() && CoSyncNet::IsConnected())
    {

    }

    g_lastTickStep = "PlayerMgr:Tick:entity-loop";
    for (auto& kv : m_playersByEntityID)
    {
        CoSyncPlayer& ent = *kv.second;

        if (!ent.hasSpawned)
            continue;

        if (ReleaseIfFreed(ent))
            continue;

        bool isMyOwnRealActor = false;
        if (CoSyncNpcWorldSync::IsWorldActorEntityID(ent.entityID))
        {
            auto stIt = m_statesByEntityID.find(ent.entityID);
            if (stIt != m_statesByEntityID.end() && stIt->second.isLocallyOwned)
                isMyOwnRealActor = true;
        }

        if (!isMyOwnRealActor)
        {
            g_lastTickStep = "PlayerMgr:Tick:entity-loop:ApplyPendingTransform";
            ent.ApplyPendingTransformIfAny();


            // AI-alive NPC puppets animate natively — hand-driving their 299-var
            // graph here would fight the engine's own animation. Skip it for them;
            // players and legacy-mode NPC proxies still drive it.
            if (!(CoSyncGameAPI::PuppetModeEnabled() && ent.createType == CoSyncEntityType::NPC))
            {
                g_lastTickStep = "PlayerMgr:Tick:entity-loop:TickAnimVars";
                ent.TickAnimVars();
            }
            else
            {
                // Puppets still play the owner's shots, reloads and throws
                // (AEV|); only the continuous graph-variable drive is skipped.
                g_lastTickStep = "PlayerMgr:Tick:entity-loop:ReplayDiscreteEvents";
                ent.ReplayDiscreteEvents();
            }
        }

        g_lastTickStep = "PlayerMgr:Tick:entity-loop:WeaponChange";
        // 2026-09-05: this used to discard weapon changes for EVERY world-actor
        // entity, so the equippedWeaponFormID the host already streams for
        // enemies was never applied -- NPC proxies animated firing with empty
        // hands. Only real actors this machine drives are excluded now.
        if (isMyOwnRealActor)
        {
            ent.m_pendingWeaponChange = false;
        }
        else if (ent.m_pendingWeaponChange && ent.actorRef)
        {
            // Round 143: same spawn-settle race as OutfitSync (see that
            // file's round-143 comment) — CoSyncSpawnTasks queues a
            // Disable()+Enable() body refresh right after spawn, and an
            // AddItem queued on the same actor's VM queue within that
            // window can silently fail to land. Only gates the FIRST-ever
            // weapon equip (m_lastEquippedWeaponFormID == 0) so a mid-
            // session weapon SWITCH is never delayed. The pending flag is
            // deliberately left set (not consumed) so this retries next tick.
            static constexpr double kWeaponSpawnSettleDelay = 2.0;
            if (ent.m_lastEquippedWeaponFormID == 0 &&
                (NowSeconds() - ent.m_spawnTime) < kWeaponSpawnSettleDelay)
            {
                // leave m_pendingWeaponChange set; retry next tick
            }
            else
            {
            ent.m_pendingWeaponChange = false;

            if (ent.m_pendingWeaponFormID != 0)
            {

                TESForm* weapForm = LookupFormByID(ent.m_pendingWeaponFormID);
                if (!weapForm)
                {
                    LOG_UPDATED_WARN("[PlayerMgr] LookupFormByID FAILED for weapon 0x%08X entity=%u"
                                     " — proxy will be unarmed (form not loaded on this client?)",
                                     ent.m_pendingWeaponFormID, ent.entityID);
                }
                else
                {
                    // 2026-08-26: IDA-confirmed (1.11.240.0, Actor::DrawWeaponMagicHands
                    // decompiled) that the draw call below is a weapon-STATE-MACHINE
                    // action, not a mesh-reparent primitive -- it reads the actor's
                    // current 4-bit weapon-state field and short-circuits to a no-op
                    // whenever the requested state already matches, i.e. it only
                    // actually re-attaches anything on a real drawn<->holstered EDGE.
                    // A same-state weapon SWAP (proxy already drawn, host switches to
                    // a different weapon without holstering) never crosses that edge,
                    // so the m_weaponDrawDelay=8 redraw a few lines below this block
                    // was silently a no-op the whole time the proxy was already armed
                    // -- old mesh stays attached until an unrelated real holster/draw
                    // happens to reset the state bits. Force a real edge here: holster
                    // NOW (synchronous, harmless if already holstered) so the redraw
                    // after equip completes is a genuine 0->1 transition instead of a
                    // 1->1 no-op.
                    if (ent.m_lastEquippedWeaponFormID != 0 &&
                        ent.m_lastEquippedWeaponFormID != ent.m_pendingWeaponFormID &&
                        ent.m_prevWeaponState != 0)
                    {
                        CoSyncEquipHook::DrawWeaponEngine(ent.actorRef, false);
                        LOG_UPDATED("[PlayerMgr] Weapon swap while drawn entity=%u old=0x%08X new=0x%08X -- forced holster edge",
                            ent.entityID, ent.m_lastEquippedWeaponFormID, ent.m_pendingWeaponFormID);
                    }

                    // Round 145: Papyrus AddItem (CallFunctionNoWait) silently
                    // fails on the RMPlayer proxy for ALL form-arg calls — the
                    // weapon never lands in BGSInventoryList regardless of type
                    // or delay. Bypass the Papyrus VM entirely: directly inject
                    // the Stack into BGSInventoryList from C++ (synchronous).
                    // EquipItemDirect then fires in the same tick via a 2-frame
                    // countdown (single engine pass for inventory propagation).
                    const bool injected = CoSyncGameAPI::DirectInjectInventoryItem(
                        ent.actorRef, weapForm);
                    if (injected)
                    {
                        ent.m_weaponEquipFormID  = ent.m_pendingWeaponFormID;
                        ent.m_weaponEquipDelay   = 2;
                        ent.m_weaponEquipRetried = false;
                        LOG_UPDATED("[PlayerMgr] DirectInject weapon entity=%u formID=0x%08X (EquipItem in ~2 frames)",
                            ent.entityID, ent.m_pendingWeaponFormID);
                    }
                    else
                    {
                        LOG_UPDATED_WARN("[PlayerMgr] DirectInject FAILED entity=%u formID=0x%08X (no vtable source?)",
                            ent.entityID, ent.m_pendingWeaponFormID);
                    }
                }
            }
            else if (ent.m_lastEquippedWeaponFormID != 0)
            {

                TESForm* lastWeap = LookupFormByID(ent.m_lastEquippedWeaponFormID);
                if (lastWeap)
                {
                    VMArray<VMVariable> unArgs;
                    VMVariable vF; CoSyncVM::SetFormArg(vF, lastWeap); unArgs.Push(&vF);
                    bool prev = false;
                    VMVariable vP; vP.Set(&prev);     unArgs.Push(&vP);  // abPreventEquip=false
                    bool sil = true;
                    VMVariable vS; vS.Set(&sil);      unArgs.Push(&vS);  // abSilent=true
                    CallFunctionNoWait(ent.actorRef, kUnequipItem, unArgs);
                    LOG_UPDATED("[PlayerMgr] UnequipItem entity=%u weapon=0x%08X (holstered)",
                        ent.entityID, ent.m_lastEquippedWeaponFormID);

                    // Engine sheathe (DrawWeaponMagicHands(false)) runs the real
                    // BGSAction sheathe and detaches the weapon 3D; the raw graph
                    // event only animates the arms.
                    if (!CoSyncEquipHook::DrawWeaponEngine(ent.actorRef, false))
                        CoSyncAnimUtils::SendEvent(ent.actorRef, CoSyncAnimUtils::Events::WeaponSheathe);
                }
            }

            ent.m_lastEquippedWeaponFormID = ent.m_pendingWeaponFormID;
            ent.m_pendingWeaponFormID = 0;
            }
        }

        // Phase-2 weapon equip: countdown started by the AddItem above.
        // Inventory pre-check before equipping: EquipItemDirect (and Papyrus
        // EquipItem) silently no-op when the item isn't in the proxy's
        // container yet. On first miss, re-issue AddItem and reset the
        // countdown for one more attempt (~45 frames). On the second pass
        // equip regardless so the verify can measure the final result.
        if (ent.m_weaponEquipDelay > 0 && ent.actorRef)
        {
            if (--ent.m_weaponEquipDelay == 0 && ent.m_weaponEquipFormID != 0)
            {
                TESForm* weapForm = LookupFormByID(ent.m_weaponEquipFormID);
                if (weapForm)
                {
                    // Check inventory before equipping.
                    bool itemInInventory = false;
                    BGSInventoryList* inv = ent.actorRef->inventoryList;
                    if (inv)
                    {
                        for (UInt32 k = 0; k < inv->items.count; ++k)
                        {
                            const BGSInventoryItem& itm = inv->items[k];
                            if (itm.form && itm.form->formID == ent.m_weaponEquipFormID)
                            { itemInInventory = true; break; }
                        }
                    }

                    if (!itemInInventory)
                    {
                        // Phase-1 DirectInject should have landed the item; if
                        // somehow it didn't, re-inject now (idempotent) so the
                        // equip below always has something to work with.
                        CoSyncGameAPI::DirectInjectInventoryItem(ent.actorRef, weapForm);
                        LOG_UPDATED_WARN("[PlayerMgr] Weapon 0x%08X not in inventory at phase-2 entity=%u -- re-injecting",
                            ent.m_weaponEquipFormID, ent.entityID);
                    }

                    {
                        // Equip — item is now guaranteed in inventory (injected above if needed).
                        const bool directOk = CoSyncEquipHook::EquipItemDirect(
                            ent.actorRef, weapForm);

                        if (!directOk)
                        {
                            VMArray<VMVariable> eqArgs;
                            VMVariable vF; CoSyncVM::SetFormArg(vF, weapForm); eqArgs.Push(&vF);
                            bool prev = true;
                            VMVariable vP; vP.Set(&prev); eqArgs.Push(&vP);
                            bool sil = true;
                            VMVariable vS; vS.Set(&sil); eqArgs.Push(&vS);
                            CallFunctionNoWait(ent.actorRef, kEquipItem, eqArgs);
                        }

                        ent.m_weaponDrawDelay    = 8;
                        ent.m_weaponEquipRetried = false;
                        LOG_UPDATED("[PlayerMgr] EquipItem %s entity=%u weapon=0x%08X (inInventory=%d weaponDraw in ~8 frames)",
                            directOk ? "DIRECT" : "PAPYRUS",
                            ent.entityID, ent.m_weaponEquipFormID,
                            itemInInventory ? 1 : 0);

                        QueueWeaponVerify(ent.entityID, ent.m_weaponEquipFormID);
                        ent.m_weaponEquipFormID = 0;
                    }
                }
                else
                {
                    ent.m_weaponEquipFormID  = 0;
                    ent.m_weaponEquipRetried = false;
                }
            }
        }


        // Post-RefreshActor3D weapon re-equip: Enable() does not re-attach
        // individually-equipped weapons, so we re-call EquipItemDirect after
        // the async Enable() has had time to complete (~60 frames).
        if (ent.m_postRefreshWeaponDelay > 0 && ent.actorRef)
        {
            if (--ent.m_postRefreshWeaponDelay == 0 && ent.m_postRefreshWeaponFormID != 0)
            {
                TESForm* wf = LookupFormByID(ent.m_postRefreshWeaponFormID);
                if (wf)
                {
                    CoSyncEquipHook::EquipItemDirect(ent.actorRef, wf);
                    ent.m_weaponDrawDelay = 8;
                    LOG_UPDATED("[PlayerMgr] Post-refresh weapon re-equip entity=%u form=0x%08X",
                        ent.entityID, ent.m_postRefreshWeaponFormID);
                }
                ent.m_postRefreshWeaponFormID = 0;
            }
        }

        // Deferred InitNpcProxy: fires once the Papyrus VM script object is
        // ready (120 ticks after spawn). Immediate dispatch during SpawnTask
        // caused an EXEC AV + VEH silent task-drop that swallowed the host
        // player proxy and other NPC spawns queued after the bad actor.
        if (ent.m_pendingNpcInitDelay > 0 && ent.actorRef)
        {
            if (--ent.m_pendingNpcInitDelay == 0)
            {
                CoSyncGameAPI::InitNpcProxy(ent.actorRef);
                LOG_UPDATED("[PlayerMgr] InitNpcProxy deferred fire entity=%u", ent.entityID);

                CoSyncGameAPI::RefreshActor3D(ent.actorRef);
                LOG_UPDATED("[PlayerMgr] RefreshActor3D deferred fire entity=%u", ent.entityID);
            }
        }

        if (ent.m_pendingFactionDelay > 0 && ent.actorRef)
        {
            if (--ent.m_pendingFactionDelay == 0)
            {
                CoSyncGameAPI::QueueProxyFactionRank(ent.actorRef);
                LOG_UPDATED("[PlayerMgr] SetFactionRank deferred fire entity=%u", ent.entityID);
            }
        }

        g_lastTickStep = "PlayerMgr:Tick:entity-loop:CombatToggle";
        // Per-actor blacklist: a proxy whose first combat-toggle faulted the
        // Papyrus VM (no script object) is never retried, so a single caught AV
        // disables it instead of crash-storming every tick. Mirrors the
        // s_badHealthActors pattern above. Pointer-keyed; the entity-loop's
        // IsFormStillLive guard clears freed actors before we get here.
        static std::unordered_set<Actor*> s_badCombatActors;
        // Re-issue StartCombat both on the combat on/off edge AND when the host
        // NPC switches targets while staying in combat -- otherwise the puppet
        // keeps attacking whoever it first locked onto even after the real NPC
        // has turned to a new target ("attacking the same target" requirement).
        const bool combatStateEdge = (ent.m_pendingCombatActive != ent.m_lastCombatActive);
        const bool targetSwitched  = (ent.m_pendingCombatActive
                                      && ent.m_pendingCombatTarget != ent.m_lastCombatTarget);

        // K3 (0.3.1): an NPC puppet never runs its own combat AI. Its owner's
        // real NPC is the only one that fights (its hits on player proxies are
        // relayed as DMG), so a puppet in local combat would double every hit
        // -- and CoSyncPlayer::ApplyProxyTransform stops puppet combat on every
        // update anyway, so the StartCombat below only made the two fight.
        // The puppet shows the fight instead: weapon drawn while its owner's
        // NPC is in combat, facing from the owner's rotation, and the owner's
        // attack/reload/hit/stagger events replayed (AEV| and AE|).
        if (CoSyncGameAPI::PuppetModeEnabled() && ent.createType == CoSyncEntityType::NPC)
        {
            if (combatStateEdge && ent.actorRef)
            {
                CoSyncEquipHook::DrawWeaponEngine(ent.actorRef, ent.m_pendingCombatActive);
                LOG_UPDATED("[PlayerMgr] puppet entity=%u %s weapon (owner combat %s, target=%u)",
                    ent.entityID, ent.m_pendingCombatActive ? "draws" : "holsters",
                    ent.m_pendingCombatActive ? "start" : "end", ent.m_pendingCombatTarget);
            }
            ent.m_lastCombatActive = ent.m_pendingCombatActive;
            ent.m_lastCombatTarget = ent.m_pendingCombatTarget;
        }
        else if ((combatStateEdge || targetSwitched) && ent.actorRef
            && !s_badCombatActors.count(ent.actorRef))
        {
            ent.m_lastCombatActive = ent.m_pendingCombatActive;
            ent.m_lastCombatTarget = ent.m_pendingCombatTarget;

            Actor* targetActor = nullptr;
            if (ent.m_pendingCombatActive)
            {
                // Round <combat-target fix>: m_pendingCombatTarget carries a real,
                // network-computed nearest-player entityID (HostSendNpcUpdates_Impl's
                // distance heuristic) but was previously discarded here -- StartCombat
                // always targeted the local player regardless of who the NPC was
                // actually closest to. Resolve the real target: self if it's us,
                // otherwise look up the target's proxy via the entity registry,
                // falling back to the local player if that proxy isn't spawned yet
                // on this side (spawn-ordering race, not an error).
                if (ent.m_pendingCombatTarget == CoSyncNet::GetMyEntityID() ||
                    ent.m_pendingCombatTarget == 0)
                {
                    PlayerCharacter* localPlayer = CoSyncGetPlayer();
                    targetActor = localPlayer ? reinterpret_cast<Actor*>(localPlayer) : nullptr;
                }
                else
                {
                    targetActor = g_CoSyncEntities.GetActor(ent.m_pendingCombatTarget);
                    if (!targetActor)
                    {
                        PlayerCharacter* localPlayer = CoSyncGetPlayer();
                        targetActor = localPlayer ? reinterpret_cast<Actor*>(localPlayer) : nullptr;
                        LOG_UPDATED_WARN("[PlayerMgr] CombatTarget entity=%u target=%u unresolved -- falling back to local player",
                            ent.entityID, ent.m_pendingCombatTarget);
                    }
                }
            }

            // StartCombat needs a resolved target; StopCombat never does. Route
            // both through the SEH-guarded helper (target==nullptr => StopCombat).
            if (!ent.m_pendingCombatActive || targetActor)
            {
                const BSFixedString& fn = ent.m_pendingCombatActive ? kStartCombat : kStopCombat;
                Actor* combatTarget     = ent.m_pendingCombatActive ? targetActor : nullptr;
                if (SafeCombatToggle(ent.actorRef, fn, combatTarget))
                {
                    if (ent.m_pendingCombatActive)
                        LOG_INFO("[PlayerMgr] StartCombat entity=%u target=%u", ent.entityID, ent.m_pendingCombatTarget);
                    else
                        LOG_INFO("[PlayerMgr] StopCombat entity=%u", ent.entityID);
                }
                else
                {
                    s_badCombatActors.insert(ent.actorRef);
                    LOG_UPDATED_WARN("[PlayerMgr] entity=%u actor=%p combat-toggle faulted the Papyrus VM -- blacklisted (AI-disabled proxy has no VM script object)",
                        ent.entityID, static_cast<void*>(ent.actorRef));
                }
            }
        }

        // Target change while NOT in combat (or on an actor whose combat toggle
        // is blacklisted): keep the tracking field in sync and just log it. The
        // in-combat retarget is handled by the targetSwitched path above.
        if (ent.m_pendingCombatTarget != ent.m_lastCombatTarget)
        {
            ent.m_lastCombatTarget = ent.m_pendingCombatTarget;
            LOG_INFO("[PlayerMgr] CombatTarget entity=%u -> target=%u",
                ent.entityID, ent.m_pendingCombatTarget);
        }


        g_lastTickStep = "PlayerMgr:Tick:entity-loop:DialogToggle";
        if (ent.m_pendingInDialog != ent.m_lastInDialog && ent.actorRef)
        {
            ent.m_lastInDialog = ent.m_pendingInDialog;

            // UPDATE 2026-06-21 (round 87): this was calling a Papyrus
            // function named "DisableAI", which doesn't exist on Actor (the
            // real native is EnableAI(bool,bool); disabling AI means calling
            // EnableAI with abEnable=false) -- and calling "EnableAI" with
            // zero arguments instead of the two required bools, the exact
            // argument-count bug an earlier round (2026-06-17) already found
            // and fixed in CoSyncGameAPI::EnableActorAI/DisableActorAI, just
            // not here. Route through those already-correct helpers instead
            // of duplicating the broken inline calls.
            if (ent.m_pendingInDialog)
                CoSyncGameAPI::DisableActorAI(ent.actorRef);
            else
                CoSyncGameAPI::EnableActorAI(ent.actorRef);
        }

        {
            g_lastTickStep = "PlayerMgr:Tick:entity-loop:Pipboy";
            const bool pendingOpen = ent.m_pendingPipboyOpen.load();
            if (pendingOpen != ent.m_lastPipboyOpen)
            {
                // Round 139: "PipBoyMenuOpen/Close" events do not exist in
                // the third-person human behavior graph (HKX string-table
                // check) — the sends were silent no-ops. The visible pipboy
                // pose comes from the head-pitch override keyed off
                // m_lastPipboyOpen in TickAnimVars, which this flag flip
                // still drives.
                LOG_UPDATED("[PlayerMgr] Pipboy entity=%u open=%d (pitch override only; no graph event exists)",
                    ent.entityID, pendingOpen ? 1 : 0);
                ent.m_lastPipboyOpen = pendingOpen;
            }
        }


        // NPC proxies now interpolate through the same buffered path as
        // players (CoSyncPlayer::ApplyUpdate no longer hard-snaps them per
        // packet), so they MUST reach TickSmoothing or they would never be
        // positioned at all. On the host, its own authoritative world NPCs
        // never receive EU packets, so their buffer stays empty and
        // TickSmoothing is a no-op for them. Companions stay excluded: they
        // are real AI-driven followers on both machines and are steered by
        // TickCompanionFollowNudge, not by transform writes.
        auto it = m_statesByEntityID.find(ent.entityID);
        if (it != m_statesByEntityID.end() && it->second.hasCreate &&
            it->second.lastCreate.type == CoSyncEntityType::Companion)
            continue;

        g_lastTickStep = "PlayerMgr:Tick:entity-loop:TickSmoothing";
        ent.TickSmoothing(now);
    }

    g_lastTickStep = "PlayerMgr:Tick:ProcessWeaponVerifyQueue";
    ProcessWeaponVerifyQueue();

    g_lastTickStep = "PlayerMgr:Tick:exit";
}

void CoSyncPlayerManager::HandleTimeouts(double now)
{
    LOG_REQUIREMENT("CoSyncPlayerManager::HandleTimeouts", "Goes through the tracked entities and hands any that timed out to the despawn function, which handles its own safety checks.");
    constexpr double kEntityTimeoutSec = 15.0;

    // 2026-08-02: a cell transition (exterior->interior) stops packet
    // processing for the whole loading screen while wall-clock time keeps
    // running, so every remote entity looked 15s+ stale the instant the game
    // came back and got despawned -- taking its state with it. Every later
    // update then hit the !hasCreate path and was dropped FOREVER
    // ("DROP(no-create)" x600+ in the 04:58 logs), which is exactly the
    // reported "after going interior the proxy and I can't see each other".
    // Forgive any gap large enough to be a load/hitch rather than a dropout:
    // push every entity's last-update stamp forward by the stalled duration so
    // nothing expires purely because the game thread was busy.
    {
        static double s_lastTimeoutTick = 0.0;
        if (s_lastTimeoutTick > 0.0)
        {
            const double gap = now - s_lastTimeoutTick;
            if (gap > 3.0)
            {
                for (auto& kv : m_statesByEntityID)
                    if (kv.second.lastUpdateLocalTime > 0.0)
                        kv.second.lastUpdateLocalTime += gap;
                LOG_UPDATED("[PlayerMgr] tick stalled %.1fs (loading screen?) — forgiving entity timeouts", gap);
            }
        }
        s_lastTimeoutTick = now;
    }

    std::vector<uint32_t> expired;
    expired.reserve(m_statesByEntityID.size());

    for (const auto& kv : m_statesByEntityID)
    {
        const CoSyncEntityState& st = kv.second;

        if (st.entityID == 0 || st.entityID == m_localEntityID)
            continue;

        if (st.entityID == kDebugNpcEntityID)
            continue;

        // Player entities are torn down by an explicit EntityDestroy on
        // disconnect, so this update-gap timeout is a redundant safety net for
        // them -- and a harmful one, since expiring a peer destroys the create
        // state that peer's updates need and there is no re-create path.
        if (st.hasCreate && st.lastCreate.type == CoSyncEntityType::Player)
            continue;

        if (st.isNPC && CoSyncNet::IsHost())
            continue;

        if (st.isLocallyOwned && CoSyncNpcWorldSync::IsWorldActorEntityID(st.entityID))
            continue;

        if (st.lastUpdateLocalTime <= 0.0)
            continue;

        if ((now - st.lastUpdateLocalTime) > kEntityTimeoutSec)
            expired.push_back(st.entityID);
    }

    for (uint32_t entityID : expired)
    {
        LOG_UPDATED("[PlayerMgr] TIMEOUT entity=%u — no update for %.0fs, removing",
            entityID, kEntityTimeoutSec);
        // N1 (2026-09-26, proven by the Dogmeat/65536 log): a world NPC must be
        // unbound through NpcWorldSync, which owns its binding, clone and
        // hidden original. DespawnEntity erased only this manager's state, so
        // NpcWorldSync still believed it bound and ignored every re-sent
        // CREATE: the NPC stayed gone while REQENT fired every 2 s. The full
        // unbind also re-shows the client's own original, and the owner's
        // next CREATE (5 s re-broadcast or a REQENT answer) binds it again.
        if (CoSyncNpcWorldSync::IsWorldActorEntityID(entityID))
            CoSyncNpcWorldSync::ClientUnbindWorldActor(entityID);
        else
            DespawnEntity(entityID, "timeout");
    }
}
