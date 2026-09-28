#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncFactions.h"

#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncProfiles.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncGameAPI.h"
#include "CoSyncWorkshopEngine.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "f4se/PluginAPI.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern PlayerCharacter* CoSyncGetPlayer();

// Wire format (text, one message each):
//   FQ|<op>|<arg>|...       client -> host   request (fields percent-encoded)
//   FR|<ok 0/1>|<text>      host -> sender   result of that request
//   FN|<text>               host -> player   notice (invited, kicked, ...)
//   FS|<body>               host -> all      the whole faction state
// FS body: '\n'-separated lines, fields '|'-separated and percent-encoded:
//   V|<version>
//   S|<limitMode>|<customLimit>|<freeBuild>|<unclaimed>
//   F|<id>|<name>|<leaderKey>|<banner>
//   R|<factionId>|<rankId>|<priority>|<perms>|<name>
//   M|<factionId>|<key>|<rankId>|<name>
//   B|<factionId>|<key>|<name>
//   T|<factionId>|<workshopFormID>|<name>
//   I|<factionId>|<key>|<inviterName>
//   O|<key>|<entityID>|<name>                (online players; not saved)
namespace
{
    using namespace CoSyncFactions;

    constexpr uint32_t kRecordType    = 'FACT';
    constexpr uint32_t kRecordVersion = 1;

    constexpr uint32_t kLeaderRankId   = 1;
    constexpr int      kLeaderPriority = 1000;
    constexpr size_t   kMaxFactions    = 32;
    constexpr size_t   kMaxRanks       = 10;
    constexpr size_t   kMaxMembers     = 64;
    constexpr size_t   kMaxTerritories = 64;
    constexpr size_t   kMaxBans        = 64;
    constexpr double   kInviteSeconds  = 600.0;
    constexpr float    kClaimRange     = 10000.f;   // game units from the workbench
    constexpr size_t   kMaxBodyBytes   = 256 * 1024;

    struct Rank
    {
        uint32_t    id = 0;
        std::string name;
        int         priority = 0;
        uint32_t    perms = 0;
    };

    struct Member
    {
        std::string key;
        std::string name;
        uint32_t    rankId = 0;
    };

    struct Ban
    {
        std::string key;
        std::string name;
    };

    struct Territory
    {
        uint32_t    workshop = 0;
        std::string name;
    };

    struct Faction
    {
        uint32_t               id = 0;
        std::string            name;
        std::string            leaderKey;
        std::string            banner;
        std::vector<Rank>      ranks;
        std::vector<Member>    members;
        std::vector<Ban>       bans;
        std::vector<Territory> territories;
    };

    struct Invite
    {
        uint32_t    factionId = 0;
        std::string key;
        std::string inviter;
        double      expires = 0.0;   // authority clock; not replicated
    };

    struct Online
    {
        std::string key;
        uint32_t    entityID = 0;
        std::string name;
        uint32_t    conn = 0;        // host only; not replicated
    };

    struct State
    {
        uint32_t              version = 0;
        Settings              settings;
        std::vector<Faction>  factions;
        std::vector<Invite>   invites;
        std::vector<Online>   online;
    };

    Faction* FindFactionIn(State& st, uint32_t id)
    {
        for (auto& f : st.factions) if (f.id == id) return &f;
        return nullptr;
    }

    std::recursive_mutex s_mx;
    State                s_state;
    uint32_t             s_nextFactionId = 1;
    bool                 s_dirty = false;       // authority: publish FS on next tick
    bool                 s_inSession = false;
    std::string          s_lastResult;
    uint32_t             s_nearestWorkshop = 0;
    double               s_nextNearest = 0.0;

    struct UiCmd { std::string cmd, arg; };
    std::deque<UiCmd>    s_uiQueue;

    // Host: per-connection request budget (token bucket, 10/s, burst 20).
    struct Bucket { double tokens = 20.0; double last = 0.0; };
    std::unordered_map<uint32_t, Bucket> s_buckets;

    double Now()
    {
        return static_cast<double>(GetTickCount64()) / 1000.0;
    }

    // ── Encoding ────────────────────────────────────────────────────────────
    std::string Pct(const std::string& s)
    {
        static const char* hex = "0123456789ABCDEF";
        std::string o;
        o.reserve(s.size());
        for (unsigned char c : s)
        {
            if (c == '%' || c == '|' || c == '\n' || c == '\r' || c < 0x20 || c == 0x7F)
            {
                o += '%'; o += hex[c >> 4]; o += hex[c & 15];
            }
            else
                o += static_cast<char>(c);
        }
        return o;
    }

    std::string Unpct(const std::string& s)
    {
        auto hv = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        std::string o;
        o.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '%' && i + 2 < s.size() && hv(s[i + 1]) >= 0 && hv(s[i + 2]) >= 0)
            {
                o += static_cast<char>(hv(s[i + 1]) * 16 + hv(s[i + 2]));
                i += 2;
            }
            else
                o += s[i];
        }
        return o;
    }

    std::vector<std::string> Split(const std::string& s, char sep)
    {
        std::vector<std::string> out;
        size_t a = 0;
        while (true)
        {
            const size_t b = s.find(sep, a);
            out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        return out;
    }

    uint32_t ToU32(const std::string& s)
    {
        return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
    }

    int ToInt(const std::string& s)
    {
        return static_cast<int>(std::strtol(s.c_str(), nullptr, 10));
    }

    std::string JsonEscape(const std::string& s)
    {
        std::string o;
        o.reserve(s.size() + 8);
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20)
                {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                }
                else
                    o += static_cast<char>(c);
            }
        }
        return o;
    }

    // ── Validation ──────────────────────────────────────────────────────────
    // Display text typed by players: printable ASCII, trimmed.
    bool CleanName(const std::string& in, size_t minLen, size_t maxLen, std::string& out)
    {
        size_t a = 0, b = in.size();
        while (a < b && in[a] == ' ') ++a;
        while (b > a && in[b - 1] == ' ') --b;
        out = in.substr(a, b - a);
        if (out.size() < minLen || out.size() > maxLen)
            return false;
        for (unsigned char c : out)
            if (c < 0x20 || c > 0x7E || c == '|' || c == '%')
                return false;
        return true;
    }

    // Banner ids: "emblem:<name>:<#rrggbb>" or "custom:<file>|emblem:..."
    bool CleanBanner(const std::string& in)
    {
        if (in.empty() || in.size() > 128)
            return false;
        for (unsigned char c : in)
        {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '_' || c == '-' || c == '.' || c == ':' || c == '#' || c == ';';
            if (!ok)
                return false;
        }
        return true;
    }

    bool IEquals(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    // ── Lookups (caller holds s_mx) ─────────────────────────────────────────
    Faction* FindFaction(uint32_t id)
    {
        for (auto& f : s_state.factions) if (f.id == id) return &f;
        return nullptr;
    }

    Faction* FactionOfKey(const std::string& key, Member** outMember = nullptr)
    {
        if (key.empty()) return nullptr;
        for (auto& f : s_state.factions)
            for (auto& m : f.members)
                if (m.key == key)
                {
                    if (outMember) *outMember = &m;
                    return &f;
                }
        return nullptr;
    }

    const Rank* FindRank(const Faction& f, uint32_t rankId)
    {
        for (const auto& r : f.ranks) if (r.id == rankId) return &r;
        return nullptr;
    }

    const Rank* LowestRank(const Faction& f)
    {
        const Rank* best = nullptr;
        for (const auto& r : f.ranks)
            if (r.id != kLeaderRankId && (!best || r.priority < best->priority))
                best = &r;
        return best;
    }

    int PriorityOf(const Faction& f, const Member& m)
    {
        if (m.key == f.leaderKey) return kLeaderPriority;
        const Rank* r = FindRank(f, m.rankId);
        return r ? r->priority : 0;
    }

    uint32_t PermsOf(const Faction& f, const Member& m)
    {
        if (m.key == f.leaderKey) return kPermAll;
        const Rank* r = FindRank(f, m.rankId);
        return r ? r->perms : 0;
    }

    bool IsBanned(const Faction& f, const std::string& key)
    {
        for (const auto& b : f.bans) if (b.key == key) return true;
        return false;
    }

    const Online* OnlineByKey(const std::string& key)
    {
        for (const auto& o : s_state.online) if (o.key == key) return &o;
        return nullptr;
    }

    const Online* OnlineByEid(uint32_t eid)
    {
        for (const auto& o : s_state.online) if (o.entityID == eid) return &o;
        return nullptr;
    }

    Faction* TerritoryOwner(uint32_t workshop, Territory** outT = nullptr)
    {
        for (auto& f : s_state.factions)
            for (auto& t : f.territories)
                if (t.workshop == workshop)
                {
                    if (outT) *outT = &t;
                    return &f;
                }
        return nullptr;
    }

    std::string MyKey()
    {
        return CoSyncProfiles::GetMyProfileKey();
    }

    bool AuthorityNow()
    {
        return !CoSyncTransport::IsInitialized() || CoSyncTransport::IsHost();
    }

    // ── Serialization ───────────────────────────────────────────────────────
    std::string Serialize(bool includeOnline)
    {
        std::string o;
        char b[160];
        std::snprintf(b, sizeof(b), "V|%u\nS|%u|%u|%u|%u\n", s_state.version,
            static_cast<unsigned>(s_state.settings.limitMode), s_state.settings.customLimit,
            s_state.settings.freeBuild ? 1u : 0u, static_cast<unsigned>(s_state.settings.unclaimed));
        o += b;
        for (const auto& f : s_state.factions)
        {
            o += "F|" + std::to_string(f.id) + "|" + Pct(f.name) + "|" + Pct(f.leaderKey) + "|" + Pct(f.banner) + "\n";
            for (const auto& r : f.ranks)
                o += "R|" + std::to_string(f.id) + "|" + std::to_string(r.id) + "|" + std::to_string(r.priority) + "|" +
                     std::to_string(r.perms) + "|" + Pct(r.name) + "\n";
            for (const auto& m : f.members)
                o += "M|" + std::to_string(f.id) + "|" + Pct(m.key) + "|" + std::to_string(m.rankId) + "|" + Pct(m.name) + "\n";
            for (const auto& x : f.bans)
                o += "B|" + std::to_string(f.id) + "|" + Pct(x.key) + "|" + Pct(x.name) + "\n";
            for (const auto& t : f.territories)
                o += "T|" + std::to_string(f.id) + "|" + std::to_string(t.workshop) + "|" + Pct(t.name) + "\n";
        }
        for (const auto& i : s_state.invites)
            o += "I|" + std::to_string(i.factionId) + "|" + Pct(i.key) + "|" + Pct(i.inviter) + "\n";
        if (includeOnline)
            for (const auto& x : s_state.online)
                o += "O|" + Pct(x.key) + "|" + std::to_string(x.entityID) + "|" + Pct(x.name) + "\n";
        return o;
    }

    bool Parse(const std::string& body, State& out, uint32_t& maxFactionId)
    {
        out = State{};
        maxFactionId = 0;
        for (const std::string& line : Split(body, '\n'))
        {
            if (line.size() < 2 || line[1] != '|')
                continue;
            std::vector<std::string> v = Split(line.substr(2), '|');
            for (auto& s : v) s = Unpct(s);
            switch (line[0])
            {
            case 'V':
                if (v.size() >= 1) out.version = ToU32(v[0]);
                break;
            case 'S':
                if (v.size() >= 4)
                {
                    const uint32_t mode = ToU32(v[0]);
                    out.settings.limitMode   = mode <= 2 ? static_cast<BuildLimitMode>(mode) : BuildLimitMode::Default;
                    out.settings.customLimit = (std::min)(ToU32(v[1]), 100000u);
                    out.settings.freeBuild   = v[2] == "1";
                    out.settings.unclaimed   = v[3] == "1" ? UnclaimedPolicy::ClaimRequired : UnclaimedPolicy::Anyone;
                }
                break;
            case 'F':
                if (v.size() >= 4 && out.factions.size() < kMaxFactions)
                {
                    Faction f;
                    f.id = ToU32(v[0]); f.name = v[1]; f.leaderKey = v[2]; f.banner = v[3];
                    if (f.id == 0) return false;
                    maxFactionId = (std::max)(maxFactionId, f.id);
                    out.factions.push_back(std::move(f));
                }
                break;
            case 'R':
                if (v.size() >= 5)
                    if (Faction* f = FindFactionIn(out, ToU32(v[0])))
                        if (f->ranks.size() < kMaxRanks)
                            f->ranks.push_back({ ToU32(v[1]), v[4], ToInt(v[2]), ToU32(v[3]) });
                break;
            case 'M':
                if (v.size() >= 4)
                    if (Faction* f = FindFactionIn(out, ToU32(v[0])))
                        if (f->members.size() < kMaxMembers)
                            f->members.push_back({ v[1], v[3], ToU32(v[2]) });
                break;
            case 'B':
                if (v.size() >= 3)
                    if (Faction* f = FindFactionIn(out, ToU32(v[0])))
                        if (f->bans.size() < kMaxBans)
                            f->bans.push_back({ v[1], v[2] });
                break;
            case 'T':
                if (v.size() >= 3)
                    if (Faction* f = FindFactionIn(out, ToU32(v[0])))
                        if (f->territories.size() < kMaxTerritories)
                            f->territories.push_back({ ToU32(v[1]), v[2] });
                break;
            case 'I':
                if (v.size() >= 3)
                    out.invites.push_back({ ToU32(v[0]), v[1], v[2], 0.0 });
                break;
            case 'O':
                if (v.size() >= 3)
                    out.online.push_back({ v[0], ToU32(v[1]), v[2], 0 });
                break;
            }
        }
        return true;
    }
}


namespace
{
    // ── Authority: publishing ───────────────────────────────────────────────
    void RebuildOnline()
    {
        s_state.online.clear();
        s_state.online.push_back({ MyKey(), CoSyncNet::GetMyEntityID(), CoSyncNet::GetMyName(), 0 });
        if (CoSyncTransport::IsInitialized() && CoSyncTransport::IsHost())
        {
            for (const auto& p : CoSyncNet::HostGetPeers())
            {
                const std::string key = CoSyncProfiles::HostGetConnKey(p.conn);
                if (!key.empty())
                    s_state.online.push_back({ key, p.entityID, p.name, p.conn });
            }
        }
        // Keep member display names current for players who are online.
        for (auto& f : s_state.factions)
            for (auto& m : f.members)
                if (const Online* o = OnlineByKey(m.key))
                    if (!o->name.empty())
                        m.name = o->name;
    }

    void MarkChanged()
    {
        ++s_state.version;
        s_dirty = true;
    }

    void Publish()
    {
        RebuildOnline();
        if (CoSyncTransport::IsInitialized() && CoSyncTransport::IsHost() && CoSyncNet::IsConnected())
        {
            const std::string msg = "FS|" + Serialize(true);
            CoSyncTransport::Send(msg);
            LOG_INFO("[Factions] TX state v%u (%zu bytes, %zu factions)", s_state.version, msg.size(), s_state.factions.size());
        }
    }

    void NotifyKey(const std::string& key, const std::string& text)
    {
        if (key == MyKey())
        {
            CoSyncGameAPI::QueueNotification(text.c_str());
            return;
        }
        if (const Online* o = OnlineByKey(key))
            if (o->conn != 0)
                CoSyncTransport::SendTo(o->conn, "FN|" + Pct(text));
    }

    void NotifyFaction(const Faction& f, const std::string& text, const std::string& exceptKey)
    {
        for (const auto& m : f.members)
            if (m.key != exceptKey)
                NotifyKey(m.key, text);
    }

    // Default Fallout-style hierarchy for a new faction. Data, not code:
    // every rank can be renamed, re-prioritized, re-permissioned or deleted.
    void AddDefaultRanks(Faction& f)
    {
        f.ranks.push_back({ kLeaderRankId, "General", kLeaderPriority, kPermAll });
        f.ranks.push_back({ 2, "Commander", 80, kPermAllWorkshop | kPermInvite | kPermKick | kPermBan | kPermPromote | kPermManageTerritory });
        f.ranks.push_back({ 3, "Officer",   60, kPermAllWorkshop | kPermInvite | kPermKick });
        f.ranks.push_back({ 4, "Soldier",   40, kPermBuild | kPermMove | kPermRepair });
        f.ranks.push_back({ 5, "Recruit",   10, 0 });
    }

    uint32_t NextRankId(const Faction& f)
    {
        uint32_t id = kLeaderRankId;
        for (const auto& r : f.ranks) id = (std::max)(id, r.id);
        return id + 1;
    }

    void RemoveInvitesFor(const std::string& key)
    {
        s_state.invites.erase(std::remove_if(s_state.invites.begin(), s_state.invites.end(),
            [&](const Invite& i) { return i.key == key; }), s_state.invites.end());
    }

    void RemoveMember(Faction& f, const std::string& key)
    {
        f.members.erase(std::remove_if(f.members.begin(), f.members.end(),
            [&](const Member& m) { return m.key == key; }), f.members.end());
    }

    void DisbandFaction(uint32_t id)
    {
        s_state.invites.erase(std::remove_if(s_state.invites.begin(), s_state.invites.end(),
            [&](const Invite& i) { return i.factionId == id; }), s_state.invites.end());
        s_state.factions.erase(std::remove_if(s_state.factions.begin(), s_state.factions.end(),
            [&](const Faction& f) { return f.id == id; }), s_state.factions.end());
    }

    // After a leader leaves: the highest-ranked member leads, or the faction ends.
    void ReassignLeader(Faction& f)
    {
        Member* best = nullptr;
        int bestP = -1;
        for (auto& m : f.members)
        {
            const Rank* r = FindRank(f, m.rankId);
            const int p = r ? r->priority : 0;
            if (p > bestP) { bestP = p; best = &m; }
        }
        if (!best)
        {
            f.leaderKey.clear();
            return;
        }
        f.leaderKey = best->key;
        best->rankId = kLeaderRankId;
    }

    // ── Engine checks used by claims (tick thread) ──────────────────────────
    TESObjectREFR* WorkshopRefFor(uint32_t formID)
    {
        if (formID == 0) return nullptr;
        TESForm* form = LookupFormByID(formID);
        if (!form || form->formType < kFormType_REFR || form->formType > kFormType_PHZD)
            return nullptr;
        TESObjectREFR* ref = static_cast<TESObjectREFR*>(form);
        return CoSyncWorkshopEngine::IsWorkshopRef(ref) ? ref : nullptr;
    }

    __declspec(noinline) bool IsNearWorkshopImpl(const std::string& key, uint32_t workshopFormID)
    {
        TESObjectREFR* ws = WorkshopRefFor(workshopFormID);
        if (!ws) return false;
        TESWorldSpace* wsSpace = CALL_MEMBER_FN(ws, GetWorldspace)();
        const uint32_t wsSpaceId = wsSpace ? reinterpret_cast<TESForm*>(wsSpace)->formID : 0u;
        const uint32_t wsCellId = ws->parentCell ? ws->parentCell->formID : 0u;

        NiPoint3 pos;
        uint32_t spaceId = 0, cellId = 0;
        if (key == MyKey())
        {
            PlayerCharacter* pc = CoSyncGetPlayer();
            if (!pc) return false;
            TESObjectREFR* r = reinterpret_cast<TESObjectREFR*>(pc);
            pos = r->pos;
            TESWorldSpace* s = CALL_MEMBER_FN(r, GetWorldspace)();
            spaceId = s ? reinterpret_cast<TESForm*>(s)->formID : 0u;
            cellId = r->parentCell ? r->parentCell->formID : 0u;
        }
        else
        {
            const Online* o = OnlineByKey(key);
            if (!o) return false;
            CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(o->entityID);
            if (!p || p->lastRemoteWorldspaceID == 0xFFFFFFFFu) return false;
            pos = p->authoritativePos;
            spaceId = p->lastRemoteWorldspaceID;
            cellId = p->lastRemoteCellFormID;
        }

        if (wsSpaceId == 0)   // interior workshop: same cell
            return spaceId == 0 && cellId == wsCellId;
        if (spaceId != wsSpaceId)
            return false;
        const float dx = pos.x - ws->pos.x, dy = pos.y - ws->pos.y;
        return dx * dx + dy * dy <= kClaimRange * kClaimRange;
    }

    bool IsNearWorkshop(const std::string& key, uint32_t workshopFormID)
    {
        __try { return IsNearWorkshopImpl(key, workshopFormID); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool ValidWorkshop(uint32_t formID)
    {
        __try { return WorkshopRefFor(formID) != nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // ── Authority: one request ──────────────────────────────────────────────
    struct Result
    {
        bool ok = false;
        std::string text;
    };

    Result Fail(const std::string& t) { return { false, t }; }

    Result Handle(const std::string& actorKey, const std::string& actorName, const std::vector<std::string>& a)
    {
        if (actorKey.empty())
            return Fail("Your identity is not known to the host yet.");
        if (a.empty())
            return Fail("Empty request.");
        const std::string& op = a[0];
        auto arg = [&](size_t i) -> const std::string& {
            static const std::string empty;
            return i < a.size() ? a[i] : empty;
        };

        Member* me = nullptr;
        Faction* mine = FactionOfKey(actorKey, &me);
        const uint32_t myPerms = (mine && me) ? PermsOf(*mine, *me) : 0;
        const int myPrio = (mine && me) ? PriorityOf(*mine, *me) : 0;

        // Target member in MY faction, and whether I outrank them.
        auto target = [&](const std::string& key, Member** out) -> bool {
            *out = nullptr;
            if (!mine) return false;
            for (auto& m : mine->members) if (m.key == key) { *out = &m; return true; }
            return false;
        };

        if (op == "create")
        {
            if (mine) return Fail("Leave your current faction first.");
            std::string name;
            if (!CleanName(arg(1), 3, 24, name)) return Fail("Faction names are 3-24 letters, numbers or symbols.");
            for (const auto& f : s_state.factions)
                if (IEquals(f.name, name)) return Fail("That faction name is taken.");
            if (s_state.factions.size() >= kMaxFactions) return Fail("The server has too many factions.");
            Faction f;
            f.id = s_nextFactionId++;
            f.name = name;
            f.leaderKey = actorKey;
            f.banner = CleanBanner(arg(2)) ? arg(2) : "emblem:star:#c8a24a";
            AddDefaultRanks(f);
            f.members.push_back({ actorKey, actorName, kLeaderRankId });
            RemoveInvitesFor(actorKey);
            s_state.factions.push_back(std::move(f));
            LOG_INFO("[Factions] '%s' created faction '%s'", actorName.c_str(), name.c_str());
            return { true, "Faction " + name + " founded." };
        }

        if (op == "accept" || op == "decline")
        {
            const uint32_t fid = ToU32(arg(1));
            auto it = std::find_if(s_state.invites.begin(), s_state.invites.end(),
                [&](const Invite& i) { return i.factionId == fid && i.key == actorKey; });
            if (it == s_state.invites.end()) return Fail("That invitation is no longer valid.");
            s_state.invites.erase(it);
            Faction* f = FindFaction(fid);
            if (!f) return Fail("That faction no longer exists.");
            if (op == "decline")
            {
                NotifyKey(f->leaderKey, actorName + " declined the invitation to " + f->name + ".");
                return { true, "Invitation declined." };
            }
            if (mine) return Fail("Leave your current faction first.");
            if (IsBanned(*f, actorKey)) return Fail("You are banned from that faction.");
            if (f->members.size() >= kMaxMembers) return Fail("That faction is full.");
            const Rank* low = LowestRank(*f);
            if (!low) return Fail("That faction has no rank for new members.");
            f->members.push_back({ actorKey, actorName, low->id });
            RemoveInvitesFor(actorKey);
            NotifyFaction(*f, actorName + " joined " + f->name + ".", actorKey);
            return { true, "You joined " + f->name + " as " + low->name + "." };
        }

        if (op == "settings")
        {
            // Session rules belong to whoever owns the world.
            if (actorKey != MyKey()) return Fail("Only the host can change workshop settings.");
            const uint32_t mode = ToU32(arg(1));
            if (mode > 2) return Fail("Unknown build limit mode.");
            const uint32_t limit = ToU32(arg(2));
            if (mode == 1 && (limit < 1 || limit > 100000)) return Fail("Custom limit must be 1-100000.");
            s_state.settings.limitMode   = static_cast<BuildLimitMode>(mode);
            if (limit >= 1 && limit <= 100000) s_state.settings.customLimit = limit;
            s_state.settings.freeBuild   = arg(3) == "1";
            s_state.settings.unclaimed   = arg(4) == "1" ? UnclaimedPolicy::ClaimRequired : UnclaimedPolicy::Anyone;
            return { true, "Workshop settings saved." };
        }

        // Everything below needs a faction.
        if (!mine || !me) return Fail("You are not in a faction.");

        if (op == "leave")
        {
            const std::string fname = mine->name;
            const bool wasLeader = mine->leaderKey == actorKey;
            RemoveMember(*mine, actorKey);
            if (wasLeader)
            {
                ReassignLeader(*mine);
                if (mine->leaderKey.empty())
                {
                    LOG_INFO("[Factions] '%s' disbanded (last member left)", fname.c_str());
                    DisbandFaction(mine->id);
                    return { true, "You left " + fname + ". It had no members left and was disbanded." };
                }
                std::string newLeader = "a new leader";
                for (const auto& m : mine->members) if (m.key == mine->leaderKey) newLeader = m.name;
                NotifyFaction(*mine, actorName + " left. " + fname + " is now led by " + newLeader + ".", "");
            }
            else
                NotifyFaction(*mine, actorName + " left " + fname + ".", "");
            return { true, "You left " + fname + "." };
        }

        if (op == "disband")
        {
            if (mine->leaderKey != actorKey) return Fail("Only the leader can disband the faction.");
            const std::string fname = mine->name;
            NotifyFaction(*mine, fname + " was disbanded by its leader.", actorKey);
            DisbandFaction(mine->id);
            return { true, fname + " was disbanded." };
        }

        if (op == "rename")
        {
            if (!(myPerms & kPermEditFaction)) return Fail("Your rank cannot rename the faction.");
            std::string name;
            if (!CleanName(arg(1), 3, 24, name)) return Fail("Faction names are 3-24 letters, numbers or symbols.");
            for (const auto& f : s_state.factions)
                if (f.id != mine->id && IEquals(f.name, name)) return Fail("That faction name is taken.");
            mine->name = name;
            return { true, "Faction renamed to " + name + "." };
        }

        if (op == "banner")
        {
            if (!(myPerms & kPermEditFaction)) return Fail("Your rank cannot change the banner.");
            if (!CleanBanner(arg(1))) return Fail("Invalid banner.");
            mine->banner = arg(1);
            return { true, "Banner changed." };
        }

        if (op == "invite")
        {
            if (!(myPerms & kPermInvite)) return Fail("Your rank cannot invite players.");
            const std::string& tk = arg(1);
            const Online* t = OnlineByKey(tk);
            if (!t) return Fail("That player is not online.");
            if (tk == actorKey) return Fail("You are already a member.");
            if (FactionOfKey(tk)) return Fail(t->name + " is already in a faction.");
            if (IsBanned(*mine, tk)) return Fail("That player is banned from your faction. Unban them first.");
            for (const auto& i : s_state.invites)
                if (i.factionId == mine->id && i.key == tk) return Fail("That player already has an invitation.");
            s_state.invites.push_back({ mine->id, tk, actorName, Now() + kInviteSeconds });
            NotifyKey(tk, actorName + " invited you to join " + mine->name + ". Open Multiplayer > Factions to answer.");
            return { true, "Invitation sent to " + t->name + "." };
        }

        if (op == "kick" || op == "ban")
        {
            const bool ban = op == "ban";
            if (!(myPerms & (ban ? kPermBan : kPermKick))) return Fail(ban ? "Your rank cannot ban players." : "Your rank cannot kick players.");
            const std::string& tk = arg(1);
            if (tk == actorKey) return Fail("Use Leave Faction instead.");
            Member* t = nullptr;
            const bool isMember = target(tk, &t);
            if (isMember && PriorityOf(*mine, *t) >= myPrio) return Fail("You can only act on members ranked below you.");
            if (!isMember && !ban) return Fail("That player is not in your faction.");
            std::string tname = isMember ? t->name : std::string();
            if (tname.empty()) if (const Online* o = OnlineByKey(tk)) tname = o->name;
            if (tname.empty()) tname = "Player";
            if (isMember) RemoveMember(*mine, tk);
            if (ban)
            {
                if (IsBanned(*mine, tk)) return Fail("That player is already banned.");
                if (mine->bans.size() >= kMaxBans) return Fail("The ban list is full.");
                mine->bans.push_back({ tk, tname });
                s_state.invites.erase(std::remove_if(s_state.invites.begin(), s_state.invites.end(),
                    [&](const Invite& i) { return i.factionId == mine->id && i.key == tk; }), s_state.invites.end());
            }
            NotifyKey(tk, ban ? "You were banned from " + mine->name + "." : "You were removed from " + mine->name + ".");
            return { true, tname + (ban ? " was banned." : " was removed from the faction.") };
        }

        if (op == "unban")
        {
            if (!(myPerms & kPermBan)) return Fail("Your rank cannot unban players.");
            const std::string& tk = arg(1);
            auto it = std::find_if(mine->bans.begin(), mine->bans.end(), [&](const Ban& b) { return b.key == tk; });
            if (it == mine->bans.end()) return Fail("That player is not banned.");
            const std::string tname = it->name;
            mine->bans.erase(it);
            return { true, tname + " was unbanned." };
        }

        if (op == "setrank")
        {
            if (!(myPerms & kPermPromote)) return Fail("Your rank cannot promote or demote.");
            Member* t = nullptr;
            if (!target(arg(1), &t)) return Fail("That player is not in your faction.");
            if (t->key == actorKey) return Fail("You cannot change your own rank.");
            const Rank* nr = FindRank(*mine, ToU32(arg(2)));
            if (!nr) return Fail("Unknown rank.");
            if (nr->id == kLeaderRankId) return Fail("Use Transfer Leadership to hand over command.");
            if (PriorityOf(*mine, *t) >= myPrio) return Fail("You can only change members ranked below you.");
            if (nr->priority >= myPrio) return Fail("You can only assign ranks below your own.");
            const Rank* old = FindRank(*mine, t->rankId);
            const bool up = !old || nr->priority > old->priority;
            t->rankId = nr->id;
            NotifyKey(t->key, std::string(up ? "You were promoted to " : "You were demoted to ") + nr->name + ".");
            return { true, t->name + " is now " + nr->name + "." };
        }

        if (op == "transfer")
        {
            if (mine->leaderKey != actorKey) return Fail("Only the leader can transfer leadership.");
            Member* t = nullptr;
            if (!target(arg(1), &t) || t->key == actorKey) return Fail("Choose another member of your faction.");
            const Rank* second = nullptr;
            for (const auto& r : mine->ranks)
                if (r.id != kLeaderRankId && (!second || r.priority > second->priority)) second = &r;
            if (!second) return Fail("Create another rank first.");
            me->rankId = second->id;
            t->rankId = kLeaderRankId;
            mine->leaderKey = t->key;
            NotifyFaction(*mine, t->name + " now leads " + mine->name + ".", "");
            return { true, "Leadership transferred." };
        }

        if (op == "rank_add" || op == "rank_edit")
        {
            if (!(myPerms & kPermManageRanks)) return Fail("Your rank cannot manage ranks.");
            std::string name;
            if (!CleanName(arg(op == "rank_add" ? 1 : 2), 2, 20, name)) return Fail("Rank names are 2-20 characters.");
            const int prio = ToInt(arg(op == "rank_add" ? 2 : 3));
            uint32_t perms = ToU32(arg(op == "rank_add" ? 3 : 4)) & kPermAll;
            if (op == "rank_edit")
            {
                const uint32_t rid = ToU32(arg(1));
                Rank* r = nullptr;
                for (auto& x : mine->ranks) if (x.id == rid) r = &x;
                if (!r) return Fail("Unknown rank.");
                if (r->id == kLeaderRankId)
                {
                    if (mine->leaderKey != actorKey) return Fail("Only the leader can rename the leader rank.");
                    r->name = name;   // leader rank keeps its priority and full permissions
                    return { true, "Rank renamed to " + name + "." };
                }
                if (r->priority >= myPrio) return Fail("You can only edit ranks below your own.");
                if (prio < 1 || prio >= myPrio) return Fail("Priority must be between 1 and your own rank.");
                if ((perms & ~myPerms) != 0) return Fail("You cannot grant permissions you do not have.");
                r->name = name; r->priority = prio; r->perms = perms;
                return { true, "Rank " + name + " updated." };
            }
            if (mine->ranks.size() >= kMaxRanks) return Fail("A faction can have at most 10 ranks.");
            if (prio < 1 || prio >= myPrio) return Fail("Priority must be between 1 and your own rank.");
            if ((perms & ~myPerms) != 0) return Fail("You cannot grant permissions you do not have.");
            mine->ranks.push_back({ NextRankId(*mine), name, prio, perms });
            return { true, "Rank " + name + " created." };
        }

        if (op == "rank_del")
        {
            if (!(myPerms & kPermManageRanks)) return Fail("Your rank cannot manage ranks.");
            const uint32_t rid = ToU32(arg(1));
            auto it = std::find_if(mine->ranks.begin(), mine->ranks.end(), [&](const Rank& r) { return r.id == rid; });
            if (it == mine->ranks.end()) return Fail("Unknown rank.");
            if (rid == kLeaderRankId) return Fail("The leader rank cannot be deleted.");
            if (it->priority >= myPrio) return Fail("You can only delete ranks below your own.");
            const std::string rname = it->name;
            mine->ranks.erase(it);
            const Rank* low = LowestRank(*mine);
            if (!low)
            {
                mine->ranks.push_back({ NextRankId(*mine), "Recruit", 10, 0 });
                low = &mine->ranks.back();
            }
            for (auto& m : mine->members)
                if (m.rankId == rid) m.rankId = low->id;
            return { true, "Rank " + rname + " deleted; its members are now " + low->name + "." };
        }

        if (op == "claim")
        {
            if (!(myPerms & kPermManageTerritory)) return Fail("Your rank cannot claim territory.");
            const uint32_t ws = ToU32(arg(1));
            if (!ValidWorkshop(ws)) return Fail("That is not a settlement workbench.");
            if (Faction* owner = TerritoryOwner(ws))
                return Fail(owner->id == mine->id ? "Your faction already holds this settlement." :
                            "This settlement belongs to another faction. They must release it first.");
            if (mine->territories.size() >= kMaxTerritories) return Fail("Your faction holds too many settlements.");
            if (!IsNearWorkshop(actorKey, ws)) return Fail("You must be at the settlement to claim it.");
            std::string name;
            if (!CleanName(arg(2), 1, 32, name))
            {
                char b[40];
                std::snprintf(b, sizeof(b), "Settlement %08X", ws);
                name = b;
            }
            mine->territories.push_back({ ws, name });
            NotifyFaction(*mine, mine->name + " claimed " + name + ".", actorKey);
            LOG_INFO("[Factions] '%s' claimed workshop 0x%08X ('%s') for '%s'", actorName.c_str(), ws, name.c_str(), mine->name.c_str());
            return { true, name + " is now " + mine->name + " territory." };
        }

        if (op == "release" || op == "rename_terr")
        {
            if (!(myPerms & kPermManageTerritory)) return Fail("Your rank cannot manage territory.");
            const uint32_t ws = ToU32(arg(1));
            Territory* t = nullptr;
            Faction* owner = TerritoryOwner(ws, &t);
            if (!owner || owner->id != mine->id) return Fail("Your faction does not hold that settlement.");
            if (op == "rename_terr")
            {
                std::string name;
                if (!CleanName(arg(2), 1, 32, name)) return Fail("Settlement names are 1-32 characters.");
                t->name = name;
                return { true, "Settlement renamed to " + name + "." };
            }
            const std::string tname = t->name;
            mine->territories.erase(std::remove_if(mine->territories.begin(), mine->territories.end(),
                [&](const Territory& x) { return x.workshop == ws; }), mine->territories.end());
            NotifyFaction(*mine, mine->name + " released " + tname + ".", actorKey);
            return { true, tname + " was released." };
        }

        return Fail("Unknown faction request.");
    }

    // Authority entry point: apply, answer, publish.
    void RunAuthority(const std::string& actorKey, const std::string& actorName,
                      const std::vector<std::string>& args, uint32_t replyConn)
    {
        const Result r = Handle(actorKey, actorName, args);
        LOG_INFO("[Factions] request '%s' from '%s' -> %s (%s)", args.empty() ? "" : args[0].c_str(),
            actorName.c_str(), r.ok ? "OK" : "DENIED", r.text.c_str());
        if (r.ok)
            MarkChanged();
        if (replyConn == 0)
        {
            s_lastResult = std::string(r.ok ? "1|" : "0|") + r.text;
            CoSyncGameAPI::QueueNotification(r.text.c_str());
        }
        else
        {
            CoSyncTransport::SendTo(replyConn, std::string("FR|") + (r.ok ? "1|" : "0|") + Pct(r.text));
        }
    }

    // Menu command -> request arguments. Unknown commands are dropped.
    bool UiToArgs(const std::string& cmd, const std::string& arg, std::vector<std::string>& out)
    {
        static const char* const kOps[] = {
            "create", "rename", "banner", "disband", "invite", "accept", "decline", "leave",
            "kick", "ban", "unban", "setrank", "transfer", "rank_add", "rank_edit", "rank_del",
            "claim", "release", "rename_terr", "settings",
        };
        if (cmd.rfind("fac_", 0) != 0)
            return false;
        const std::string op = cmd.substr(4);
        bool known = false;
        for (const char* k : kOps) if (op == k) known = true;
        if (!known)
            return false;
        out.clear();
        out.push_back(op);
        if (!arg.empty())
            for (const std::string& s : Split(arg, '|'))
                out.push_back(s);
        // "claim" without an id = the settlement the player is standing in.
        if (op == "claim")
        {
            const std::string name = out.size() >= 2 ? out[1] : std::string();
            out.resize(1);
            out.push_back(std::to_string(s_nearestWorkshop));
            out.push_back(name);
        }
        return true;
    }

    __declspec(noinline) uint32_t ResolveNearestWorkshop()
    {
        __try
        {
            PlayerCharacter* pc = CoSyncGetPlayer();
            if (!pc) return 0;
            TESObjectREFR* w = CoSyncWorkshopEngine::GetCurrentWorkshop();
            if (!w) w = CoSyncWorkshopEngine::FindNearestValidWorkshop(reinterpret_cast<TESObjectREFR*>(pc));
            return w ? w->formID : 0u;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }
}


namespace CoSyncFactions
{
    void OnSessionStart(bool isHost)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        s_inSession = true;
        s_buckets.clear();
        s_lastResult.clear();
        if (!isHost)
        {
            // A client's view is whatever the host sends.
            s_state = State{};
        }
        s_dirty = true;
        LOG_INFO("[Factions] session start (%s)", isHost ? "host" : "client");
    }

    void OnSessionEnd()
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        const bool wasClient = s_inSession && !AuthorityNow();
        s_inSession = false;
        s_uiQueue.clear();
        s_buckets.clear();
        if (wasClient || (CoSyncTransport::IsInitialized() && !CoSyncTransport::IsHost()))
            s_state = State{};
        s_state.online.clear();
        s_state.invites.clear();
    }

    bool IsAuthority()
    {
        return AuthorityNow();
    }

    void Tick(double now)
    {
        (void)now;
        std::vector<UiCmd> cmds;
        {
            std::lock_guard<std::recursive_mutex> lk(s_mx);
            cmds.assign(s_uiQueue.begin(), s_uiQueue.end());
            s_uiQueue.clear();
        }

        const double t = Now();
        if (t >= s_nextNearest)
        {
            s_nextNearest = t + 1.0;
            const uint32_t n = ResolveNearestWorkshop();
            std::lock_guard<std::recursive_mutex> lk(s_mx);
            s_nearestWorkshop = n;
        }

        std::lock_guard<std::recursive_mutex> lk(s_mx);
        for (const UiCmd& c : cmds)
        {
            std::vector<std::string> args;
            if (!UiToArgs(c.cmd, c.arg, args))
                continue;
            if (AuthorityNow())
                RunAuthority(MyKey(), CoSyncNet::GetMyName(), args, 0);
            else if (CoSyncNet::IsConnected())
            {
                std::string msg = "FQ";
                for (const auto& s : args) msg += "|" + Pct(s);
                CoSyncTransport::Send(msg);
            }
            else
                s_lastResult = "0|Not connected to the host.";
        }

        if (!AuthorityNow())
            return;

        // Expire invitations.
        const size_t before = s_state.invites.size();
        s_state.invites.erase(std::remove_if(s_state.invites.begin(), s_state.invites.end(),
            [&](const Invite& i) { return i.expires > 0.0 && t > i.expires; }), s_state.invites.end());
        if (s_state.invites.size() != before)
            MarkChanged();

        if (s_dirty)
        {
            s_dirty = false;
            Publish();
        }
    }

    bool IsFactionMessage(const std::string& msg)
    {
        return msg.size() >= 3 && msg[0] == 'F' && msg[2] == '|' &&
               (msg[1] == 'Q' || msg[1] == 'R' || msg[1] == 'S' || msg[1] == 'N');
    }

    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole)
    {
        LOG_REQUIREMENT("CoSyncFactions::OnReceive", "CoSync tick. Untrusted input: the host acts only on the sender's connection identity.");
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        const char kind = msg[1];

        if (kind == 'Q')
        {
            if (!isHostRole || conn == 0 || msg.size() > 2048)
                return;
            Bucket& b = s_buckets[conn];
            const double t = Now();
            b.tokens = (std::min)(20.0, b.tokens + (t - b.last) * 10.0);
            b.last = t;
            if (b.tokens < 1.0)
                return;
            b.tokens -= 1.0;

            std::vector<std::string> args = Split(msg.substr(3), '|');
            for (auto& s : args) s = Unpct(s);
            RebuildOnline();
            const std::string key = CoSyncProfiles::HostGetConnKey(conn);
            std::string name = "Player";
            if (const Online* o = OnlineByKey(key)) name = o->name;
            RunAuthority(key, name, args, conn);
            return;
        }

        // Everything else only comes from the host.
        if (isHostRole)
            return;

        if (kind == 'S')
        {
            if (msg.size() > kMaxBodyBytes)
                return;
            State st;
            uint32_t maxId = 0;
            if (!Parse(msg.substr(3), st, maxId))
            {
                LOG_WARN("[Factions] RX malformed state -- ignored");
                return;
            }
            s_state = std::move(st);
            LOG_INFO("[Factions] RX state v%u (%zu factions, %zu online)", s_state.version,
                s_state.factions.size(), s_state.online.size());
        }
        else if (kind == 'R')
        {
            const std::vector<std::string> v = Split(msg.substr(3), '|');
            if (v.size() < 2) return;
            const std::string text = Unpct(v[1]);
            s_lastResult = (v[0] == "1" ? "1|" : "0|") + text;
            CoSyncGameAPI::QueueNotification(text.c_str());
        }
        else if (kind == 'N')
        {
            const std::string text = Unpct(msg.substr(3));
            if (!text.empty() && text.size() < 300)
                CoSyncGameAPI::QueueNotification(text.c_str());
        }
    }

    void HostOnPeerJoined(uint32_t conn)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        RebuildOnline();
        s_dirty = true;   // everyone's online list changed; the newcomer gets it too
        (void)conn;
    }

    void HostMarkDirty()
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        s_dirty = true;
    }

    bool CheckWorkshop(const std::string& key, uint32_t workshopFormID, WorkshopOp op, std::string* whyNot)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        auto no = [&](const std::string& w) { if (whyNot) *whyNot = w; return false; };

        Faction* owner = TerritoryOwner(workshopFormID);
        if (!owner)
        {
            if (s_state.settings.unclaimed == UnclaimedPolicy::ClaimRequired)
                return no("This settlement is unclaimed. A faction must claim it before anyone can build here.");
            return true;
        }

        Member* m = nullptr;
        Faction* mine = FactionOfKey(key, &m);
        if (!mine || mine->id != owner->id || !m)
            return no(owner->name + " territory. Only its members can do that here.");
        if (IsBanned(*owner, key))
            return no("You are banned from " + owner->name + ".");

        uint32_t need = 0;
        const char* verb = "";
        switch (op)
        {
        case WorkshopOp::Build:  need = kPermBuild;  verb = "build";  break;
        case WorkshopOp::Move:   need = kPermMove;   verb = "move objects"; break;
        case WorkshopOp::Scrap:  need = kPermScrap;  verb = "scrap or store objects"; break;
        case WorkshopOp::Repair: need = kPermRepair; verb = "repair objects"; break;
        }
        if ((PermsOf(*owner, *m) & need) == 0)
        {
            const Rank* r = FindRank(*owner, m->rankId);
            return no(std::string("Your rank (") + (r ? r->name : std::string("none")) + ") cannot " + verb + " here.");
        }
        return true;
    }

    bool CheckLocalWorkshop(uint32_t workshopFormID, WorkshopOp op, std::string* whyNot)
    {
        return CheckWorkshop(MyKey(), workshopFormID, op, whyNot);
    }

    uint32_t GetTerritoryOwner(uint32_t workshopFormID)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        const Faction* f = TerritoryOwner(workshopFormID);
        return f ? f->id : 0u;
    }

    std::string GetFactionName(uint32_t factionId)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        const Faction* f = FindFaction(factionId);
        return f ? f->name : std::string();
    }

    Settings GetSettings()
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        return s_state.settings;
    }

    std::string KeyForEntity(uint32_t entityID)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        if (AuthorityNow() && CoSyncTransport::IsInitialized())
            RebuildOnline();
        const Online* o = OnlineByEid(entityID);
        return o ? o->key : std::string();
    }

    bool GetBadge(uint32_t entityID, Badge& out)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        const Online* o = OnlineByEid(entityID);
        if (!o) return false;
        Member* m = nullptr;
        const Faction* f = FactionOfKey(o->key, &m);
        if (!f || !m) return false;
        out.faction = f->name;
        const Rank* r = FindRank(*f, m->rankId);
        out.rank = r ? r->name : std::string();
        out.banner = f->banner;
        return true;
    }

    uint32_t GetLocalNearestWorkshop()
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        return s_nearestWorkshop;
    }

    void QueueUiCommand(const std::string& cmd, const std::string& arg)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        if (s_uiQueue.size() < 32 && arg.size() < 512)
            s_uiQueue.push_back({ cmd, arg });
    }

    std::string ConsumeLastResult()
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        std::string r;
        r.swap(s_lastResult);
        return r;
    }

    std::string BuildUiJson()
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        const std::string me = MyKey();
        Member* myMember = nullptr;
        const Faction* mine = FactionOfKey(me, &myMember);
        const bool authority = AuthorityNow();

        std::string j = "{";
        j += "\"authority\":"; j += authority ? "true" : "false";
        j += ",\"isHost\":"; j += (authority && CoSyncTransport::IsInitialized()) || !CoSyncTransport::IsInitialized() ? "true" : "false";
        j += ",\"session\":"; j += CoSyncTransport::IsInitialized() ? "true" : "false";
        j += ",\"me\":{\"key\":\"" + JsonEscape(me) + "\",\"faction\":" + std::to_string(mine ? mine->id : 0u);
        j += ",\"rank\":" + std::to_string(myMember ? myMember->rankId : 0u);
        j += ",\"perms\":" + std::to_string((mine && myMember) ? PermsOf(*mine, *myMember) : 0u);
        j += ",\"priority\":" + std::to_string((mine && myMember) ? PriorityOf(*mine, *myMember) : 0);
        j += ",\"leader\":"; j += (mine && mine->leaderKey == me) ? "true" : "false";
        j += "}";

        const Settings& s = s_state.settings;
        j += ",\"settings\":{\"limitMode\":" + std::to_string(static_cast<unsigned>(s.limitMode));
        j += ",\"customLimit\":" + std::to_string(s.customLimit);
        j += ",\"freeBuild\":"; j += s.freeBuild ? "true" : "false";
        j += ",\"unclaimed\":" + std::to_string(static_cast<unsigned>(s.unclaimed)) + "}";

        // The settlement the player stands in and what they may do there.
        {
            const uint32_t ws = s_nearestWorkshop;
            Territory* t = nullptr;
            const Faction* owner = ws ? TerritoryOwner(ws, &t) : nullptr;
            std::string why;
            j += ",\"nearest\":{\"id\":" + std::to_string(ws);
            j += ",\"owner\":" + std::to_string(owner ? owner->id : 0u);
            j += ",\"ownerName\":\"" + JsonEscape(owner ? owner->name : std::string()) + "\"";
            j += ",\"name\":\"" + JsonEscape(t ? t->name : std::string()) + "\"";
            if (ws)
            {
                const bool b = CheckWorkshop(me, ws, WorkshopOp::Build, &why);
                j += ",\"why\":\"" + JsonEscape(b ? std::string() : why) + "\"";
                j += ",\"build\":"; j += b ? "true" : "false";
                j += ",\"move\":"; j += CheckWorkshop(me, ws, WorkshopOp::Move, nullptr) ? "true" : "false";
                j += ",\"scrap\":"; j += CheckWorkshop(me, ws, WorkshopOp::Scrap, nullptr) ? "true" : "false";
                j += ",\"repair\":"; j += CheckWorkshop(me, ws, WorkshopOp::Repair, nullptr) ? "true" : "false";
            }
            j += "}";
        }

        j += ",\"factions\":[";
        for (size_t fi = 0; fi < s_state.factions.size(); ++fi)
        {
            const Faction& f = s_state.factions[fi];
            if (fi) j += ",";
            std::string leaderName;
            for (const auto& m : f.members) if (m.key == f.leaderKey) leaderName = m.name;
            j += "{\"id\":" + std::to_string(f.id) + ",\"name\":\"" + JsonEscape(f.name) + "\"";
            j += ",\"banner\":\"" + JsonEscape(f.banner) + "\",\"leader\":\"" + JsonEscape(f.leaderKey) + "\"";
            j += ",\"leaderName\":\"" + JsonEscape(leaderName) + "\"";
            j += ",\"ranks\":[";
            for (size_t i = 0; i < f.ranks.size(); ++i)
            {
                const Rank& r = f.ranks[i];
                if (i) j += ",";
                j += "{\"id\":" + std::to_string(r.id) + ",\"name\":\"" + JsonEscape(r.name) + "\",\"priority\":" +
                     std::to_string(r.priority) + ",\"perms\":" + std::to_string(r.perms) + "}";
            }
            j += "],\"members\":[";
            for (size_t i = 0; i < f.members.size(); ++i)
            {
                const Member& m = f.members[i];
                if (i) j += ",";
                j += "{\"key\":\"" + JsonEscape(m.key) + "\",\"name\":\"" + JsonEscape(m.name) + "\",\"rank\":" +
                     std::to_string(m.rankId) + ",\"online\":";
                j += OnlineByKey(m.key) ? "true" : "false";
                j += "}";
            }
            j += "],\"bans\":[";
            for (size_t i = 0; i < f.bans.size(); ++i)
            {
                if (i) j += ",";
                j += "{\"key\":\"" + JsonEscape(f.bans[i].key) + "\",\"name\":\"" + JsonEscape(f.bans[i].name) + "\"}";
            }
            j += "],\"territories\":[";
            for (size_t i = 0; i < f.territories.size(); ++i)
            {
                if (i) j += ",";
                j += "{\"id\":" + std::to_string(f.territories[i].workshop) + ",\"name\":\"" + JsonEscape(f.territories[i].name) + "\"}";
            }
            j += "]}";
        }
        j += "],\"invites\":[";
        bool first = true;
        for (const auto& i : s_state.invites)
        {
            if (i.key != me) continue;
            const Faction* f = FindFaction(i.factionId);
            if (!f) continue;
            if (!first) j += ",";
            first = false;
            j += "{\"faction\":" + std::to_string(f->id) + ",\"name\":\"" + JsonEscape(f->name) + "\",\"by\":\"" + JsonEscape(i.inviter) + "\"}";
        }
        j += "],\"online\":[";
        for (size_t i = 0; i < s_state.online.size(); ++i)
        {
            const Online& o = s_state.online[i];
            if (i) j += ",";
            const Faction* of = FactionOfKey(o.key);
            j += "{\"key\":\"" + JsonEscape(o.key) + "\",\"name\":\"" + JsonEscape(o.name) + "\",\"faction\":" +
                 std::to_string(of ? of->id : 0u) + ",\"you\":";
            j += o.key == me ? "true" : "false";
            j += "}";
        }
        j += "],\"lastResult\":\"" + JsonEscape(s_lastResult) + "\"}";
        return j;
    }

    void SaveRecord(const F4SESerializationInterface* ser)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        // Only the world's owner saves factions; a client's copy is the host's.
        if (!AuthorityNow())
            return;
        const std::string body = Serialize(false);
        const uint32_t len = static_cast<uint32_t>(body.size());
        ser->OpenRecord(kRecordType, kRecordVersion);
        ser->WriteRecordData(&s_nextFactionId, 4);
        ser->WriteRecordData(&len, 4);
        if (len)
            ser->WriteRecordData(body.data(), len);
        LOG_INFO("[Factions] Co-save: wrote %zu faction(s), %u bytes", s_state.factions.size(), len);
    }

    bool LoadRecord(const F4SESerializationInterface* ser, uint32_t type, uint32_t version, uint32_t length)
    {
        if (type != kRecordType)
            return false;
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        if (version != kRecordVersion || length < 8)
            return true;
        std::string buf(length, '\0');
        if (ser->ReadRecordData(&buf[0], length) != length)
            return true;
        // A client loading the host's world gets the live state from the
        // host instead (FS|), which is newer than the save.
        if (!AuthorityNow())
            return true;
        uint32_t nextId = 1, len = 0;
        std::memcpy(&nextId, buf.data(), 4);
        std::memcpy(&len, buf.data() + 4, 4);
        if (len > length - 8)
            return true;
        State st;
        uint32_t maxId = 0;
        if (Parse(buf.substr(8, len), st, maxId))
        {
            st.online.clear();
            st.invites.clear();
            s_state = std::move(st);
            s_nextFactionId = (std::max)(nextId, maxId + 1);
            s_dirty = true;
            LOG_INFO("[Factions] Co-save: loaded %zu faction(s)", s_state.factions.size());
        }
        return true;
    }

    void Revert()
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        if (!AuthorityNow())
            return;
        s_state = State{};
        s_nextFactionId = 1;
        s_dirty = true;
    }
}
