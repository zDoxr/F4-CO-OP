#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct F4SESerializationInterface;

// Player teams and settlement assignment (0.5.0; replaces the 0.3.0 factions).
//
// TEAMS: a small cooperative group. One Leader, any number of Officers and
// Members. A player is in at most one team. The Leader can rename/disband,
// promote/demote, kick anyone and hand over leadership; Officers can invite
// and kick Members; everyone can leave. Invitations expire after 2 minutes.
//
// SETTLEMENTS: a workbench (workshop ref) can be
//   - unassigned: anyone may build (the pre-0.3.0 behaviour), no state stream;
//   - a TEAM settlement: one per team, built and used by every member; the
//     host streams its settlement state (ratings, workbench) to every online
//     member (CoSyncSettlementSync);
//   - a PERSONAL settlement: one per player, built and used only by its owner;
//     the state stream goes to that owner only.
// The host's world is the only persistent one, so the host stays authoritative
// for every settlement; assignment only decides who may change it and who
// receives its state.
//
// AUTHORITY: the host (or a solo player) owns the only real copy. Clients send
// requests (TQ|), the host validates each one against the sender's CONNECTION
// identity (CoSyncProfiles::HostGetConnKey -- never an ID written in the
// message), applies it and broadcasts the whole state (TS|). Clients never
// change team state themselves.
//
// PERSISTENCE: host co-save record 'TEAM' next to the player profiles.
//
// THREADING: every entry point is thread-safe (one internal mutex). Network
// and engine work runs in Tick() on CoSync's tick; UI commands are queued.
namespace CoSyncTeams
{
    enum class Role : uint8_t { Member = 0, Officer = 1, Leader = 2 };
    enum class SettlementMode : uint8_t { None = 0, Team = 1, Personal = 2 };
    enum class WorkshopOp { Build, Move, Scrap, Repair };

    // Host build rules (kept in the same authoritative snapshot so every
    // machine's workshop gate sees the same values).
    enum class BuildLimitMode : uint8_t { Default = 0, Custom = 1, Unlimited = 2 };
    struct Rules
    {
        BuildLimitMode limitMode   = BuildLimitMode::Default;
        uint32_t       customLimit = 1000;
        bool           freeBuild   = false;   // remove world build restrictions
    };

    struct Badge
    {
        std::string team;
        Role        role = Role::Member;
    };

    // ── Lifecycle ───────────────────────────────────────────────────────────
    void OnSessionStart(bool isHost);
    void OnSessionEnd();
    void Tick(double now);

    // ── Network ─────────────────────────────────────────────────────────────
    bool IsTeamMessage(const std::string& msg);
    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole);

    // Host: a client finished joining (HELLO) / someone left.
    void HostOnPeerJoined(uint32_t conn);
    void HostMarkDirty();

    // ── Queries (any thread) ────────────────────────────────────────────────
    bool IsAuthority();

    // The permission rule shared by the local gate and the host's validation.
    bool CheckWorkshop(const std::string& playerKey, uint32_t workshopFormID, WorkshopOp op, std::string* whyNot);
    bool CheckLocalWorkshop(uint32_t workshopFormID, WorkshopOp op, std::string* whyNot);

    // Every assigned settlement (team and personal).
    std::vector<uint32_t> GetAssignedSettlements();
    SettlementMode GetSettlementMode(uint32_t workshopFormID);

    // Host: the connections that receive a settlement's state (0 = the host
    // itself). False when the settlement is unassigned.
    bool GetSettlementAudience(uint32_t workshopFormID, std::vector<uint32_t>& conns);
    // Host: is the player behind `conn` a participant of the settlement?
    bool HostIsParticipant(uint32_t conn, uint32_t workshopFormID);
    // This machine's player takes part in the settlement (team member / owner).
    bool IsLocalParticipant(uint32_t workshopFormID);

    Rules GetRules();

    // Host: who a team chat / voice message from `playerKey` reaches -- the
    // connections of that team's ONLINE members (0 = the host itself).
    bool GetTeamAudience(const std::string& playerKey, std::vector<uint32_t>& conns, std::string& teamName);
    std::string TeamNameOfKey(const std::string& playerKey);

    // Player key of an online player entity, "" if unknown.
    std::string KeyForEntity(uint32_t entityID);
    // 0.5.1 (Bug #6): both online players are members of the same team.
    // Health, AP, radiation and the name of another player are shown -- and
    // their vitals sent -- only when this is true.
    bool AreTeammates(uint32_t entityA, uint32_t entityB);
    bool IsLocalTeammate(uint32_t entityID);
    // Team name and role of an online player entity (HUD / nameplates).
    bool GetBadge(uint32_t entityID, Badge& out);
    const char* RoleName(Role r);

    // ── UI ──────────────────────────────────────────────────────────────────
    std::string BuildUiJson();
    // A Team / Settlement menu command ("team_*", "settle_*", "ws_rules").
    // Queued; runs on Tick.
    void QueueUiCommand(const std::string& cmd, const std::string& arg);
    uint32_t GetLocalNearestWorkshop();

    // ── Persistence (host save) ─────────────────────────────────────────────
    void SaveRecord(const F4SESerializationInterface* ser);
    bool LoadRecord(const F4SESerializationInterface* ser, uint32_t type, uint32_t version, uint32_t length);
    void Revert();
}
