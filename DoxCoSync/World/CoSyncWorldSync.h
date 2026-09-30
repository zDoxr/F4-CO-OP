#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

class TESObjectREFR;

namespace CoSyncWorldSync
{

    struct WorldSyncPacket
    {
        uint32_t entityID        = 0;
        float    posX            = 0.f;
        float    posY            = 0.f;
        float    posZ            = 0.f;
        float    rotZ            = 0.f;
        uint32_t worldspaceFormID = 0;
        uint32_t cellFormID      = 0;
    };

    inline bool IsWorldSync(const std::string& msg)
    {
        return msg.size() > 3 && msg[0]=='W' && msg[1]=='S' && msg[2]=='|';
    }

    std::string SerializeWorldSync   (const WorldSyncPacket& p);
    bool        DeserializeWorldSync (const std::string& msg, WorldSyncPacket& out);

    struct WorkshopBuildFullPacket
    {
        uint32_t entityID   = 0;
        uint32_t baseFormID = 0;
        float    posX       = 0.f;
        float    posY       = 0.f;
        float    posZ       = 0.f;
        float    rotX       = 0.f;
        float    rotY       = 0.f;
        float    rotZ       = 0.f;

        // 2026-09-05: where the object lives on the sender. Exterior ->
        // worldspaceFormID set (cellFormID is the exterior cell); interior ->
        // worldspaceFormID == 0 and cellFormID is the interior. Both 0 =
        // legacy packet (applied in the receiver's current cell as before).
        uint32_t worldspaceFormID = 0;
        uint32_t cellFormID       = 0;

        // Sender-side ref formID. 0xFF... ids are per-save, so receivers map
        // (entityID, refFormID) -> their own placed ref; a later packet for a
        // known ref is a MOVE, not a new object. 0 = unknown.
        uint32_t refFormID        = 0;

        // Reserved for the owning workshop ref; 0 when unknown.
        uint32_t workshopFormID   = 0;
    };

    inline bool IsWorkshopBuildFull(const std::string& msg)
    {
        return msg.rfind("WBF|", 0) == 0;
    }

    std::string SerializeWorkshopBuildFull   (const WorkshopBuildFullPacket& p);
    bool        DeserializeWorkshopBuildFull (const std::string& msg, WorkshopBuildFullPacket& out);

    struct WorkshopScrapFullPacket
    {
        uint32_t entityID   = 0;
        uint32_t refFormID  = 0;
        uint32_t baseFormID = 0;
        float    posX       = 0.f;
        float    posY       = 0.f;
        float    posZ       = 0.f;
    };

    inline bool IsWorkshopScrapFull(const std::string& msg)
    {
        return msg.rfind("WDS|", 0) == 0;
    }

    std::string SerializeWorkshopScrapFull   (const WorkshopScrapFullPacket& p);
    bool        DeserializeWorkshopScrapFull (const std::string& msg, WorkshopScrapFullPacket& out);
    void        OnReceiveWorkshopScrapFull   (const WorkshopScrapFullPacket& p);

    void Init();
    void Shutdown();

    // The cell-scan workshop mirroring (WBF/WDS: detect new 0xFF refs after
    // the fact, peer-to-peer, no authority). Switched OFF once
    // CoSyncWorkshopAuthority's verified engine hooks are live -- that
    // system gates and replicates the real workshop operations instead.
    // Debris (OBJ), pickups (PU) and furniture (FU) are not affected.
    void SetLegacyWorkshopSync(bool enabled);
    bool IsLegacyWorkshopSync();


    void Tick(double now);

    void OnReceiveFurniture         (const std::string&            msg);
    void OnReceiveWorldSync         (const WorldSyncPacket&         p);
    void OnReceivePickup            (const std::string&            msg);
    void OnReceiveWorkshopBuildFull (const WorkshopBuildFullPacket& p);

    // Fired directly from the Papyrus WorkshopObjectBuilt event the instant a
    // build completes, instead of waiting for HostTickObjects_Impl's cell-scan
    // + settle-delay to notice the same object. Sends the same "WBF|" wire
    // packet the poll path uses (so the receiving side's existing apply/dedupe
    // logic handles it identically) and pre-seeds s_knownDynamicBuildings with
    // placedRefFormID so the sender's own later poll pass doesn't re-detect and
    // re-broadcast the object it just sent.
    void NotifyWorkshopBuildImmediate(const WorkshopBuildFullPacket& p, uint32_t placedRefFormID);

    // Local ref was scrapped/removed by this machine (scrap hook, WDS apply)
    // -- stop tracking it so the building scan does not also report it as
    // gone, and drop any remote->local mapping that pointed at it.
    void NotifyLocalBuildingRemoved(uint32_t refFormID);

    // Fills worldspace/cell for a placed ref.
    void FillPlacementContext(TESObjectREFR* ref, WorkshopBuildFullPacket& p);

    inline bool IsObjectMove(const std::string& msg)
    {
        return msg.rfind("OBJ|", 0) == 0;
    }

    void OnReceiveObjectMove(const std::string& msg);

    inline bool IsStateRequest(const std::string& msg)
    {
        return msg == "REQSTATE";
    }

    void SendStateRequest();
    void OnReceiveStateRequest();

    void NotifyFurnitureUse(uint32_t refFormID, uint32_t actorEntityID, bool isSitting);
    // 0.3.6: an NPC's TESFurnitureEvent (any thread); handled on the tick.
    void QueueNpcFurnitureEvent(uint32_t actorFormID, uint32_t furnFormID, bool sitting);

    // Shared with any other subsystem that independently calls
    // PlaceAtMe_Native on a peer-supplied base form (e.g. CoSyncPowerArmorSync
    // spawning a PA chassis) -- exposes the same crash-blocklist the workshop
    // build scan already maintains (kWorkshopInternalHelperForms) so a form
    // found dangerous in one context is never silently missed in another.
    bool IsKnownUnsafePlaceAtMeForm(uint32_t baseFormID);
}
