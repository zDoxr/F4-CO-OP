#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct F4SESerializationInterface;

// Player factions, ranks and settlement territory.
//
// AUTHORITY: the host (or a solo player, who is their own authority) owns the
// only real copy. Clients send requests (FQ|), the host validates every one
// against the sender's CONNECTION identity (never an ID written in the
// message), applies it, and broadcasts the whole state (FS|) to everyone.
// Clients never change faction state themselves.
//
// IDENTITY: players are keyed by their CoSync profile key (SteamID64, plus
// "-P<n>" for a second instance of one account) -- the same stable key the
// shared host save uses for characters. Names are display only.
//
// PERSISTENCE: the host's copy is written into its save's F4SE co-save
// (record 'FACT'), next to the player profiles, so factions and territory
// roll back and forward with the world.
//
// THREADING: every entry point is thread-safe (one internal mutex). Network
// and engine work happens in Tick() on CoSync's tick; UI commands are queued.
namespace CoSyncFactions
{
    // Rank permissions. Workshop permissions are enforced by
    // CoSyncWorkshopAuthority at the real engine operation and again by the
    // host; management permissions are enforced by the host here.
    enum Perm : uint32_t
    {
        kPermBuild          = 1u << 0,   // place new workshop objects (and wires)
        kPermMove           = 1u << 1,   // move existing workshop objects
        kPermScrap          = 1u << 2,   // scrap or store workshop objects
        kPermRepair         = 1u << 3,   // repair damaged workshop objects

        kPermInvite         = 1u << 8,
        kPermKick           = 1u << 9,
        kPermBan            = 1u << 10,
        kPermPromote        = 1u << 11,  // change the rank of lower-ranked members
        kPermManageRanks    = 1u << 12,  // add / edit / delete lower ranks
        kPermManageTerritory= 1u << 13,  // claim / release settlements
        kPermEditFaction    = 1u << 14,  // name and banner

        kPermAllWorkshop    = kPermBuild | kPermMove | kPermScrap | kPermRepair,
        kPermAll            = 0x7F0Fu,
    };

    enum class WorkshopOp { Build, Move, Scrap, Repair };

    // Host session rules for building (owned here so one authoritative
    // snapshot carries everything the permission check needs).
    enum class BuildLimitMode : uint8_t { Default = 0, Custom = 1, Unlimited = 2 };
    enum class UnclaimedPolicy : uint8_t { Anyone = 0, ClaimRequired = 1 };

    struct Settings
    {
        BuildLimitMode  limitMode   = BuildLimitMode::Default;
        uint32_t        customLimit = 1000;
        bool            freeBuild   = false;   // remove world build restrictions
        UnclaimedPolicy unclaimed   = UnclaimedPolicy::Anyone;
    };

    struct Badge
    {
        std::string faction;
        std::string rank;
        std::string banner;
    };

    // ── Lifecycle ───────────────────────────────────────────────────────────
    // Session start / end (CoSyncNet::Init / Shutdown). A client drops its
    // copy (it was the host's); the host keeps its data (it belongs to the
    // world and is saved with it).
    void OnSessionStart(bool isHost);
    void OnSessionEnd();

    // CoSync tick: runs queued UI commands, sends requests / snapshots.
    void Tick(double now);

    // ── Network ─────────────────────────────────────────────────────────────
    bool IsFactionMessage(const std::string& msg);
    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole);

    // Host: a client finished joining (HELLO) -- send it the full state.
    void HostOnPeerJoined(uint32_t conn);
    // Host: someone joined or left; the online list changed.
    void HostMarkDirty();

    // ── Queries (any thread) ────────────────────────────────────────────────
    // True when this machine decides faction questions (host or solo).
    bool IsAuthority();

    // The permission rule shared by the local gate and the host's validation.
    // workshopFormID = the workbench the operation is attributed to.
    bool CheckWorkshop(const std::string& playerKey, uint32_t workshopFormID,
                       WorkshopOp op, std::string* whyNot);

    // Same, for the local player.
    bool CheckLocalWorkshop(uint32_t workshopFormID, WorkshopOp op, std::string* whyNot);

    // 0 = unclaimed.
    uint32_t GetTerritoryOwner(uint32_t workshopFormID);

    // Every claimed settlement's workbench (any faction).
    std::vector<uint32_t> GetClaimedWorkshops();

    // Host: who a faction chat message from `playerKey` reaches -- the
    // connections of that faction's ONLINE members (0 = the host itself).
    // false if the player is in no faction.
    bool GetFactionAudience(const std::string& playerKey, std::vector<uint32_t>& conns, std::string& factionName);
    // "" if the player is in no faction.
    std::string FactionNameOfKey(const std::string& playerKey);
    std::string GetFactionName(uint32_t factionId);

    Settings GetSettings();

    // Player key of an online player entity (host view / replicated), "" if unknown.
    std::string KeyForEntity(uint32_t entityID);

    // Faction name, rank and banner of an online player entity (HUD).
    bool GetBadge(uint32_t entityID, Badge& out);

    // ── UI ──────────────────────────────────────────────────────────────────
    // JSON object for the Multiplayer menu (window thread).
    std::string BuildUiJson();
    // A Factions/Workshop menu command ("fac_*"). Queued; runs on Tick.
    void QueueUiCommand(const std::string& cmd, const std::string& arg);

    // Last result of this player's own request, for the menu ("" = none).
    std::string ConsumeLastResult();

    // Current settlement the local player stands in (resolved on Tick);
    // 0 = none. Used by the UI and by claim requests.
    uint32_t GetLocalNearestWorkshop();

    // ── Persistence (host save) ─────────────────────────────────────────────
    void SaveRecord(const F4SESerializationInterface* ser);
    // Called for each co-save record; returns true if it was ours.
    bool LoadRecord(const F4SESerializationInterface* ser, uint32_t type, uint32_t version, uint32_t length);
    void Revert();
}
