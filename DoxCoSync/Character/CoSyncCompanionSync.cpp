#define WIN32_LEAN_AND_MEAN
#include "CoSyncProxyLooks.h"
#include <Windows.h>

#include "CoSyncCompanionSync.h"

#include "CoSyncRequirementsLog.h"
#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncEntityRegistry.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncEntityTypes.h"
#include "CoSyncGameAPI.h"
#include "CoSyncSpawnTasks.h"
#include "CoSyncPapyrusHelper.h"

#include "EntitySerialization.h"
#include "Packets_EntityCreate.h"
#include "Packets_EntityDestroy.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "GameRTTI.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"

#include <mutex>
#include <vector>
#include <unordered_map>
#include <cstdlib>
#include <cstdio>
#include <cstring>

// Was extern RelocPtr<PlayerCharacter*> g_player -- switched to CoSyncGetPlayer()
// (see GameGlobals.cpp), the established, confirmed-working replacement for
// raw g_player dereferences used throughout the rest of the codebase.
extern PlayerCharacter* CoSyncGetPlayer();
extern RelocAddr<_LookupFormByID> LookupFormByID;
extern CoSyncEntityRegistry       g_CoSyncEntities;
extern CoSyncPlayerManager        g_CoSyncPlayerManager;

namespace
{

    enum class CompanionOpType : uint8_t { Spawn, Dismiss };

    struct CompanionOp
    {
        CompanionOpType type           = CompanionOpType::Spawn;
        uint32_t        ownerEntityID  = 0;
        uint32_t        baseFormID     = 0;
        uint32_t        entityID       = 0;
    };

    std::mutex              s_opsMutex;
    std::vector<CompanionOp> s_pending;

    std::unordered_map<uint32_t, uint32_t> s_companionOwners;

    // Reverse index: (ownerEntityID, baseFormID) -> the companion entityID we
    // already spawned for that owner+base. Prevents the DogMeat bug where the
    // Papyrus follower poll (GetCurrentFollowers diff) re-fires
    // NotifyCompanionRecruited for a companion that briefly dropped out of the
    // follower list -- e.g. DogMeat during a cell transition or scripted scene.
    // Without this, every re-recruit allocated a fresh EID and spawned ANOTHER
    // proxy dog, so companions "kept randomly spawning" and stray duplicates
    // piled up. Keyed by (owner<<32)|base; erased on dismiss.
    inline uint64_t OwnerBaseKey(uint32_t ownerEID, uint32_t baseFormID)
    {
        return (static_cast<uint64_t>(ownerEID) << 32) | baseFormID;
    }
    std::unordered_map<uint64_t, uint32_t> s_ownerBaseToComp;


    double s_lastNudgeTime = 0.0;
    static constexpr double kNudgeInterval  = 3.0;
    static constexpr float  kFollowDistSq   = 1500.f * 1500.f;

}

bool CoSyncCompanionSync::IsCompanionRequest(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncCompanionSync::IsCompanionRequest", "Just checks the text of a message. No game state needed.");
    return msg.rfind("COMP_REQ|", 0) == 0;
}

bool CoSyncCompanionSync::IsCompanionDismiss(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncCompanionSync::IsCompanionDismiss", "Just checks the text of a message. No game state needed.");
    return msg.rfind("COMP_DISMISS|", 0) == 0;
}

std::string CoSyncCompanionSync::SerializeRequest(uint32_t ownerEntityID, uint32_t baseFormID)
{
    LOG_REQUIREMENT("CoSyncCompanionSync::SerializeRequest", "Just turns a companion request into text. Doesn't touch the game at all.");
    char buf[80];
    std::snprintf(buf, sizeof(buf), "COMP_REQ|%u|%u", ownerEntityID, baseFormID);
    return buf;
}

std::string CoSyncCompanionSync::SerializeDismiss(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncCompanionSync::SerializeDismiss", "Just turns a player ID into text. Doesn't touch the game at all.");
    char buf[64];
    std::snprintf(buf, sizeof(buf), "COMP_DISMISS|%u", entityID);
    return buf;
}

void CoSyncCompanionSync::RequestCompanion(uint32_t ownerEntityID, uint32_t baseFormID)
{
    LOG_REQUIREMENT("CoSyncCompanionSync::RequestCompanion", "Needs a real companion to request and the network to be set up first (both checked).");
    if (baseFormID == 0) return;

    if (CoSyncNet::IsHost())
    {

        CompanionOp op{};
        op.type          = CompanionOpType::Spawn;
        op.ownerEntityID = ownerEntityID;
        op.baseFormID    = baseFormID;

        std::lock_guard<std::mutex> lk(s_opsMutex);
        s_pending.push_back(op);
        LOG_UPDATED("[CompanionSync] Host queued own companion spawn base=0x%08X", baseFormID);
    }
    else
    {

        CoSyncTransport::Send(SerializeRequest(ownerEntityID, baseFormID));
        LOG_UPDATED("[CompanionSync] Client TX COMP_REQ owner=%u base=0x%08X",
            ownerEntityID, baseFormID);
    }
}

void CoSyncCompanionSync::DismissCompanion(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncCompanionSync::DismissCompanion", "Needs a real player ID and the network to be set up first (both checked).");
    if (entityID == 0) return;

    if (CoSyncNet::IsHost())
    {

        CompanionOp op{};
        op.type     = CompanionOpType::Dismiss;
        op.entityID = entityID;

        std::lock_guard<std::mutex> lk(s_opsMutex);
        s_pending.push_back(op);
    }
    else
    {
        CoSyncTransport::Send(SerializeDismiss(entityID));
        LOG_UPDATED("[CompanionSync] Client TX COMP_DISMISS entity=%u", entityID);
    }
}

void CoSyncCompanionSync::OnReceiveRequest(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncCompanionSync::OnReceiveRequest", "Reads a companion request out of text. A badly formed message is safely rejected, not crashed on.");

    const char* p = msg.c_str() + 9;
    char* e = nullptr;
    uint32_t ownerEID  = static_cast<uint32_t>(std::strtoul(p, &e, 10));
    if (!e || *e != '|') { LOG_UPDATED_WARN("[CompanionSync] Malformed COMP_REQ: '%s'", msg.c_str()); return; }
    uint32_t baseFormID = static_cast<uint32_t>(std::strtoul(e + 1, nullptr, 10));
    if (baseFormID == 0) return;

    // Round 141: defense in depth — reject CoSync proxy/ghost bases here too,
    // in case a stale/older client still sends one (the primary filter is at
    // the source, Papyrus_CoSync.cpp's NotifyCompanionRecruited).
    if (baseFormID == 0x00000007u ||
        baseFormID == 0x00020749u ||
        (baseFormID & 0x00FFFFFFu) == 0x00003599u ||
        CoSyncProxyLooks::IsProxyBaseFormID(baseFormID))
    {
        LOG_UPDATED_WARN("[CompanionSync] RX COMP_REQ owner=%u base=0x%08X is a CoSync proxy/ghost base -- REJECTED",
            ownerEID, baseFormID);
        return;
    }

    CompanionOp op{};
    op.type          = CompanionOpType::Spawn;
    op.ownerEntityID = ownerEID;
    op.baseFormID    = baseFormID;

    std::lock_guard<std::mutex> lk(s_opsMutex);
    s_pending.push_back(op);
    LOG_UPDATED("[CompanionSync] RX COMP_REQ owner=%u base=0x%08X — queued", ownerEID, baseFormID);
}

void CoSyncCompanionSync::OnReceiveDismiss(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncCompanionSync::OnReceiveDismiss", "Reads a companion-dismiss notice out of text. Needs the message to be long enough to actually contain one.");

    const char* p = msg.c_str() + 13;
    uint32_t entityID = static_cast<uint32_t>(std::strtoul(p, nullptr, 10));
    if (entityID == 0) return;

    CompanionOp op{};
    op.type     = CompanionOpType::Dismiss;
    op.entityID = entityID;

    std::lock_guard<std::mutex> lk(s_opsMutex);
    s_pending.push_back(op);
    LOG_UPDATED("[CompanionSync] RX COMP_DISMISS entity=%u — queued", entityID);
}

static uint32_t AllocCompanionEID()
{
    LOG_REQUIREMENT("CoSyncCompanionSync::AllocCompanionEID", "Just hands out the next ID from an internal counter. A 0 result means it's run out of IDs to give.");

    static uint32_t s_next = 0x00020100u;
    static constexpr uint32_t kMax = 0x000201FFu;

    if (s_next > kMax)
    {
        LOG_UPDATED_WARN("[CompanionSync] COMP_REQ entity ID pool exhausted!");
        return 0;
    }
    return s_next++;
}

// UPDATE 2026-06-20 (round 41 audit): same unguarded peer-controlled
// op.baseFormID -> LookupFormByID(+field deref +DYNAMIC_CAST) crash pattern
// fixed elsewhere this session. SEH-wrapped, noinline (load-bearing under
// /GL+/LTCG). The formType field access is included inside the guard since
// it's the first dereference of a peer-controlled, potentially-dangling
// TESForm*.
__declspec(noinline) static TESForm* SafeLookupAndCheckNPCForm(uint32_t formID)
{
    LOG_REQUIREMENT("CoSyncCompanionSync::SafeLookupAndCheckNPCForm", "The ID comes from the other player and isn't trusted. Looked up safely so a bad ID can't crash the game.");
    __try
    {
        TESForm* form = LookupFormByID(formID);
        return (form && form->formType == kFormType_NPC_) ? form : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

__declspec(noinline) static TESNPC* SafeCastNPC(TESForm* form)
{
    LOG_REQUIREMENT("CoSyncCompanionSync::SafeCastNPC", "Handles a missing or non-NPC form gracefully instead of crashing.");
    __try
    {
        // 2026-07-04: DYNAMIC_CAST removed — minidump-confirmed broken RTTI
        // (see CoSyncPlayerManager.cpp PumpDeferredSpawns_Impl); an engine-side
        // SEH frame terminates the process before our __except runs. formType
        // check by engine contract instead (kFormType_NPC_ == TESNPC).
        if (form && form->formType == kFormType_NPC_)
            return static_cast<TESNPC*>(form);
        return nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

void CoSyncCompanionSync::FlushPendingCompanionOps()
{
    LOG_REQUIREMENT("CoSyncCompanionSync::FlushPendingCompanionOps", "Only the host processes queued spawn/dismiss requests (checked first); looks up any companion forms sent by other players safely.");
    if (!CoSyncNet::IsHost())
        return;

    static const BSFixedString kSetPlayerTeammate("SetPlayerTeammate");

    std::vector<CompanionOp> ops;
    {
        std::lock_guard<std::mutex> lk(s_opsMutex);
        if (s_pending.empty()) return;
        ops.swap(s_pending);
    }

    for (const CompanionOp& op : ops)
    {

        if (op.type == CompanionOpType::Spawn)
        {
            // ── Duplicate guard (DogMeat fix) ────────────────────────────────
            // If this owner already has a live companion proxy of this base,
            // do NOT spawn a second one. The follower poll re-fires recruit
            // whenever a companion flickers out of GetCurrentFollowers (very
            // common for DogMeat), and each re-fire used to stack another dog.
            const uint64_t obKey = OwnerBaseKey(op.ownerEntityID, op.baseFormID);
            auto dupIt = s_ownerBaseToComp.find(obKey);
            if (dupIt != s_ownerBaseToComp.end()
                && s_companionOwners.count(dupIt->second))
            {
                LOG_UPDATED("[CompanionSync] Duplicate recruit ignored: owner=%u base=0x%08X "
                            "already tracked as entity=%u",
                    op.ownerEntityID, op.baseFormID, dupIt->second);
                continue;
            }


            NiPoint3 spawnPos{0.f, 0.f, 0.f};
            NiPoint3 spawnRot{0.f, 0.f, 0.f};

            const uint32_t myEID = CoSyncNet::GetMyEntityID();
            if (op.ownerEntityID == myEID || op.ownerEntityID == 0)
            {

                PlayerCharacter* player = CoSyncGetPlayer();
                if (player)
                    CoSyncGameAPI::GetActorWorldTransform(
                        reinterpret_cast<Actor*>(player), spawnPos, spawnRot);
            }
            else
            {

                Actor* ownerProxy = g_CoSyncEntities.GetActor(op.ownerEntityID);
                if (ownerProxy)
                    CoSyncGameAPI::GetActorWorldTransform(ownerProxy, spawnPos, spawnRot);
                else
                {
                    LOG_UPDATED_WARN("[CompanionSync] Owner proxy for entity=%u not found — "
                        "spawning at host position", op.ownerEntityID);
                    PlayerCharacter* player = CoSyncGetPlayer();
                    if (player)
                        CoSyncGameAPI::GetActorWorldTransform(
                            reinterpret_cast<Actor*>(player), spawnPos, spawnRot);
                }
            }


            TESForm* baseForm = SafeLookupAndCheckNPCForm(op.baseFormID);
            if (!baseForm)
            {
                LOG_UPDATED_WARN("[CompanionSync] COMP_REQ: base=0x%08X is not a valid NPC — skipping",
                    op.baseFormID);
                continue;
            }


            const uint32_t eid = AllocCompanionEID();
            if (eid == 0)
                continue;


            EntityCreatePacket cp{};
            cp.entityID      = eid;
            cp.type          = CoSyncEntityType::NPC;
            cp.baseFormID    = op.baseFormID;
            cp.ownerEntityID = op.ownerEntityID;
            cp.spawnFlags    = EntityCreatePacket::RemoteControlled;
            cp.spawnPos      = spawnPos;
            cp.spawnRot      = spawnRot;
            cp.reserved0     = op.ownerEntityID;
            cp.username      = "Companion";


            TESNPC* npc = SafeCastNPC(baseForm);
            if (npc && npc->fullName.name.c_str() && npc->fullName.name.c_str()[0] != '\0')
                cp.username = npc->fullName.name.c_str();


            CoSyncSpawnTasks::EnqueueSpawn(
                eid,
                op.baseFormID,
                CoSyncEntityType::NPC,
                cp.spawnFlags,
                spawnPos,
                spawnRot
            );


            CoSyncEntityState& st = g_CoSyncPlayerManager.GetOrCreateState(eid);
            st.entityID           = eid;
            st.hasCreate          = true;
            st.isNPC              = true;
            st.hostAuthoritative  = true;
            st.lastCreate         = cp;

            CoSyncPlayer& proxy = g_CoSyncPlayerManager.GetOrCreateByEntityID(eid);
            proxy.entityID      = eid;
            proxy.createType    = CoSyncEntityType::NPC;
            proxy.ownerEntityID = op.ownerEntityID;
            proxy.username      = cp.username;
            proxy.pendingPos    = spawnPos;
            proxy.pendingRot    = spawnRot;
            proxy.hasPendingTransform = true;


            s_companionOwners[eid] = op.ownerEntityID;
            s_ownerBaseToComp[obKey] = eid;


            CoSyncTransport::Send(SerializeEntityCreate(cp));

            if (op.ownerEntityID == myEID || op.ownerEntityID == 0)
            {

                LOG_UPDATED("[CompanionSync] Host companion queued spawn entity=%u base=0x%08X name='%s'",
                    eid, op.baseFormID, cp.username.c_str());
            }
            else
            {
                LOG_UPDATED("[CompanionSync] Client companion queued spawn entity=%u owner=%u base=0x%08X",
                    eid, op.ownerEntityID, op.baseFormID);
            }
        }

        else if (op.type == CompanionOpType::Dismiss)
        {
            Actor* actor = g_CoSyncEntities.GetActor(op.entityID);
            if (actor)
            {


                VMArray<VMVariable> args;
                VMVariable vFalse; bool bFalse = false; vFalse.Set(&bFalse); args.Push(&vFalse);
                VMVariable vFalse2; vFalse2.Set(&bFalse); args.Push(&vFalse2);
                CallFunctionNoWait(actor, kSetPlayerTeammate, args);

                LOG_UPDATED("[CompanionSync] Dismissed companion entity=%u actor=%p",
                    op.entityID, static_cast<void*>(actor));
            }


            s_companionOwners.erase(op.entityID);
            // Drop the (owner,base) reverse entry so a later legitimate
            // re-recruit of this companion can spawn a fresh proxy.
            for (auto it = s_ownerBaseToComp.begin(); it != s_ownerBaseToComp.end(); )
            {
                if (it->second == op.entityID) it = s_ownerBaseToComp.erase(it);
                else ++it;
            }


            EntityDestroyPacket dp{};
            dp.entityID = op.entityID;
            CoSyncTransport::Send(SerializeEntityDestroy(dp));
            g_CoSyncPlayerManager.EnqueueEntityDestroy(dp);
            g_CoSyncEntities.Remove(op.entityID);
        }
    }
}

void CoSyncCompanionSync::TickCompanionFollowNudge(double now)
{
    LOG_REQUIREMENT("CoSyncCompanionSync::TickCompanionFollowNudge", "Needs to be the host and connected (both checked). Fetches each companion and owner safely, checking they actually exist before using them.");
    if (!CoSyncNet::IsHost() || !CoSyncNet::IsConnected())
        return;

    if ((now - s_lastNudgeTime) < kNudgeInterval)
        return;
    s_lastNudgeTime = now;

    const uint32_t myEID = CoSyncNet::GetMyEntityID();

    for (const auto& kv : s_companionOwners)
    {
        const uint32_t compEID  = kv.first;
        const uint32_t ownerEID = kv.second;


        Actor* compActor = g_CoSyncEntities.GetActor(compEID);
        if (!compActor)
            continue;


        NiPoint3 ownerPos{0.f, 0.f, 0.f};
        NiPoint3 ownerRot{0.f, 0.f, 0.f};

        if (ownerEID == myEID || ownerEID == 0)
        {
            PlayerCharacter* player = CoSyncGetPlayer();
            if (!player) continue;
            CoSyncGameAPI::GetActorWorldTransform(
                reinterpret_cast<Actor*>(player), ownerPos, ownerRot);
        }
        else
        {
            Actor* ownerProxy = g_CoSyncEntities.GetActor(ownerEID);
            if (!ownerProxy) continue;
            CoSyncGameAPI::GetActorWorldTransform(ownerProxy, ownerPos, ownerRot);
        }


        NiPoint3 compPos{0.f, 0.f, 0.f}, compRot{0.f, 0.f, 0.f};
        CoSyncGameAPI::GetActorWorldTransform(compActor, compPos, compRot);

        const float dx = compPos.x - ownerPos.x;
        const float dy = compPos.y - ownerPos.y;
        const float dz = compPos.z - ownerPos.z;
        const float distSq = dx * dx + dy * dy + dz * dz;

        if (distSq > kFollowDistSq)
        {

            NiPoint3 nudgePos = ownerPos;
            nudgePos.x -= 300.f * cosf(ownerRot.z);
            nudgePos.y -= 300.f * sinf(ownerRot.z);

            CoSyncGameAPI::PositionRemoteActor(compActor, nudgePos, ownerRot);

            LOG_UPDATED("[CompanionSync] Nudged companion entity=%u back to owner=%u (dist=%.0f)",
                compEID, ownerEID, sqrtf(distSq));
        }
    }
}
