#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncTeams.h"

#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncProfiles.h"
#include "CoSyncGameAPI.h"
#include "CoSyncWorkshopEngine.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "f4se/PluginAPI.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern PlayerCharacter* CoSyncGetPlayer();

// Wire format (text, one message each):
//   TQ|<op>|<arg>|...       client -> host   request (fields percent-encoded)
//   TR|<ok 0/1>|<text>      host -> sender   result of that request
//   TN|<text>               host -> player   notice (invited, kicked, ...)
//   TS|<body>               host -> all      the whole team state
// TS body: '\n'-separated lines, fields '|'-separated and percent-encoded:
//   V|<version>
//   R|<limitMode>|<customLimit>|<freeBuild>
//   T|<id>|<name>
//   M|<teamId>|<key>|<role 0/1/2>|<name>     (members in join order)
//   P|<ws>|<mode 1/2>|<teamId>|<ownerKey>|<name>
//   I|<teamId>|<key>|<inviterName>
//   O|<key>|<entityID>|<name>                (online players; not saved)
namespace
{
    using namespace CoSyncTeams;

    constexpr uint32_t kRecordType    = 'TEAM';
    constexpr uint32_t kRecordVersion = 1;

    constexpr size_t kMaxTeams       = 32;
    constexpr size_t kMaxMembers     = 32;
    constexpr size_t kMaxSettlements = 128;
    constexpr double kInviteSeconds  = 120.0;
    constexpr size_t kMaxBodyBytes   = 256 * 1024;

    struct Member
    {
        std::string key;
        std::string name;
        Role        role = Role::Member;
    };

    struct Team
    {
        uint32_t            id = 0;
        std::string         name;
        std::vector<Member> members;   // join order; exactly one Leader
    };

    struct Settlement
    {
        uint32_t       ws = 0;
        SettlementMode mode = SettlementMode::None;
        uint32_t       teamId = 0;     // Team
        std::string    owner;          // Personal: player key
        std::string    name;
    };

    struct Invite
    {
        uint32_t    teamId = 0;
        std::string key;
        std::string inviter;
        double      expires = 0.0;     // authority clock; not replicated
    };

    struct Online
    {
        std::string key;
        uint32_t    entityID = 0;
        std::string name;
        uint32_t    conn = 0;          // host only; not replicated
    };

    struct State
    {
        uint32_t                version = 0;
        Rules                   rules;
        std::vector<Team>       teams;
        std::vector<Settlement> settlements;
        std::vector<Invite>     invites;
        std::vector<Online>     online;
    };

    std::recursive_mutex s_mx;
    State                s_state;
    uint32_t             s_nextTeamId = 1;
    bool                 s_dirty = false;
    bool                 s_inSession = false;
    std::string          s_lastResult;
    uint32_t             s_nearestWorkshop = 0;
    std::string          s_nearestName;
    double               s_nextNearest = 0.0;

    struct UiCmd { std::string cmd, arg; };
    std::deque<UiCmd> s_uiQueue;

    struct Bucket { double tokens = 20.0; double last = 0.0; };
    std::unordered_map<uint32_t, Bucket> s_buckets;

    double Now() { return static_cast<double>(GetTickCount64()) / 1000.0; }

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

    uint32_t ToU32(const std::string& s) { return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10)); }

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

    bool IEquals(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    // ── Lookups (caller holds s_mx) ─────────────────────────────────────────
    Team* FindTeamIn(State& st, uint32_t id)
    {
        for (auto& t : st.teams) if (t.id == id) return &t;
        return nullptr;
    }

    Team* FindTeam(uint32_t id) { return FindTeamIn(s_state, id); }

    Team* TeamOfKey(const std::string& key, Member** outMember = nullptr)
    {
        if (key.empty()) return nullptr;
        for (auto& t : s_state.teams)
            for (auto& m : t.members)
                if (m.key == key)
                {
                    if (outMember) *outMember = &m;
                    return &t;
                }
        return nullptr;
    }

    Member* LeaderOf(Team& t)
    {
        for (auto& m : t.members) if (m.role == Role::Leader) return &m;
        return nullptr;
    }

    Settlement* FindSettlement(uint32_t ws)
    {
        for (auto& s : s_state.settlements) if (s.ws == ws) return &s;
        return nullptr;
    }

    Settlement* TeamSettlement(uint32_t teamId)
    {
        for (auto& s : s_state.settlements)
            if (s.mode == SettlementMode::Team && s.teamId == teamId) return &s;
        return nullptr;
    }

    Settlement* PersonalSettlement(const std::string& key)
    {
        for (auto& s : s_state.settlements)
            if (s.mode == SettlementMode::Personal && s.owner == key) return &s;
        return nullptr;
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

    std::string NameOfKey(const std::string& key)
    {
        if (const Online* o = OnlineByKey(key)) if (!o->name.empty()) return o->name;
        for (const auto& t : s_state.teams)
            for (const auto& m : t.members)
                if (m.key == key && !m.name.empty()) return m.name;
        return "a player";
    }

    std::string MyKey() { return CoSyncProfiles::GetMyProfileKey(); }

    bool AuthorityNow() { return !CoSyncTransport::IsInitialized() || CoSyncTransport::IsHost(); }

    const char* RoleText(Role r)
    {
        switch (r)
        {
        case Role::Leader:  return "Leader";
        case Role::Officer: return "Officer";
        default:            return "Member";
        }
    }

    // ── Serialization ───────────────────────────────────────────────────────
    std::string Serialize(bool includeOnline)
    {
        std::string o;
        char b[128];
        std::snprintf(b, sizeof(b), "V|%u\nR|%u|%u|%u\n", s_state.version,
            static_cast<unsigned>(s_state.rules.limitMode), s_state.rules.customLimit, s_state.rules.freeBuild ? 1u : 0u);
        o += b;
        for (const auto& t : s_state.teams)
        {
            o += "T|" + std::to_string(t.id) + "|" + Pct(t.name) + "\n";
            for (const auto& m : t.members)
                o += "M|" + std::to_string(t.id) + "|" + Pct(m.key) + "|" + std::to_string(static_cast<unsigned>(m.role)) +
                     "|" + Pct(m.name) + "\n";
        }
        for (const auto& s : s_state.settlements)
            o += "P|" + std::to_string(s.ws) + "|" + std::to_string(static_cast<unsigned>(s.mode)) + "|" +
                 std::to_string(s.teamId) + "|" + Pct(s.owner) + "|" + Pct(s.name) + "\n";
        for (const auto& i : s_state.invites)
            o += "I|" + std::to_string(i.teamId) + "|" + Pct(i.key) + "|" + Pct(i.inviter) + "\n";
        if (includeOnline)
            for (const auto& x : s_state.online)
                o += "O|" + Pct(x.key) + "|" + std::to_string(x.entityID) + "|" + Pct(x.name) + "\n";
        return o;
    }

    bool Parse(const std::string& body, State& out, uint32_t& maxTeamId)
    {
        out = State{};
        maxTeamId = 0;
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
            case 'R':
                if (v.size() >= 3)
                {
                    const uint32_t mode = ToU32(v[0]);
                    out.rules.limitMode   = mode <= 2 ? static_cast<BuildLimitMode>(mode) : BuildLimitMode::Default;
                    out.rules.customLimit = (std::min)(ToU32(v[1]), 100000u);
                    out.rules.freeBuild   = v[2] == "1";
                }
                break;
            case 'T':
                if (v.size() >= 2 && out.teams.size() < kMaxTeams)
                {
                    Team t;
                    t.id = ToU32(v[0]);
                    t.name = v[1];
                    if (t.id == 0) return false;
                    maxTeamId = (std::max)(maxTeamId, t.id);
                    out.teams.push_back(std::move(t));
                }
                break;
            case 'M':
                if (v.size() >= 4)
                    if (Team* t = FindTeamIn(out, ToU32(v[0])))
                        if (t->members.size() < kMaxMembers)
                        {
                            const uint32_t r = ToU32(v[2]);
                            t->members.push_back({ v[1], v[3], r <= 2 ? static_cast<Role>(r) : Role::Member });
                        }
                break;
            case 'P':
                if (v.size() >= 5 && out.settlements.size() < kMaxSettlements)
                {
                    Settlement s;
                    s.ws = ToU32(v[0]);
                    const uint32_t mode = ToU32(v[1]);
                    s.mode = (mode == 1 || mode == 2) ? static_cast<SettlementMode>(mode) : SettlementMode::None;
                    s.teamId = ToU32(v[2]);
                    s.owner = v[3];
                    s.name = v[4];
                    if (s.ws != 0 && s.mode != SettlementMode::None)
                        out.settlements.push_back(std::move(s));
                }
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
        // Integrity: exactly one Leader per team (a hand-edited or damaged
        // record must not leave a team nobody can manage).
        for (auto& t : out.teams)
        {
            bool haveLeader = false;
            for (auto& m : t.members)
            {
                if (m.role == Role::Leader)
                {
                    if (haveLeader) m.role = Role::Officer;
                    haveLeader = true;
                }
            }
            if (!haveLeader && !t.members.empty())
                t.members.front().role = Role::Leader;
        }
        return true;
    }
}


namespace
{
    // ── Authority: publishing ───────────────────────────────────────────────
    void MarkChanged()
    {
        ++s_state.version;
        s_dirty = true;
    }

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
        for (auto& t : s_state.teams)
            for (auto& m : t.members)
                if (const Online* o = OnlineByKey(m.key))
                    if (!o->name.empty())
                        m.name = o->name;
    }

    void Publish()
    {
        RebuildOnline();
        if (CoSyncTransport::IsInitialized() && CoSyncTransport::IsHost() && CoSyncNet::IsConnected())
        {
            const std::string msg = "TS|" + Serialize(true);
            CoSyncTransport::Send(msg);
            LOG_INFO("[Teams] TX state v%u (%zu bytes, %zu team(s), %zu settlement(s))", s_state.version, msg.size(),
                s_state.teams.size(), s_state.settlements.size());
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
                CoSyncTransport::SendTo(o->conn, "TN|" + Pct(text));
    }

    void NotifyTeam(const Team& t, const std::string& text, const std::string& exceptKey)
    {
        for (const auto& m : t.members)
            if (m.key != exceptKey)
                NotifyKey(m.key, text);
    }

    void RemoveInvitesFor(const std::string& key)
    {
        s_state.invites.erase(std::remove_if(s_state.invites.begin(), s_state.invites.end(),
            [&](const Invite& i) { return i.key == key; }), s_state.invites.end());
    }

    void RemoveMember(Team& t, const std::string& key)
    {
        t.members.erase(std::remove_if(t.members.begin(), t.members.end(),
            [&](const Member& m) { return m.key == key; }), t.members.end());
    }

    void DisbandTeam(uint32_t id)
    {
        s_state.invites.erase(std::remove_if(s_state.invites.begin(), s_state.invites.end(),
            [&](const Invite& i) { return i.teamId == id; }), s_state.invites.end());
        // The team settlement goes back to unassigned; the objects in it stay
        // (they belong to the world, not to the team).
        s_state.settlements.erase(std::remove_if(s_state.settlements.begin(), s_state.settlements.end(),
            [&](const Settlement& s) { return s.mode == SettlementMode::Team && s.teamId == id; }), s_state.settlements.end());
        s_state.teams.erase(std::remove_if(s_state.teams.begin(), s_state.teams.end(),
            [&](const Team& t) { return t.id == id; }), s_state.teams.end());
    }

    // Leadership goes to the first Officer (join order), else the first
    // Member. `preferOnline`: only an online member (disconnect handoff).
    Member* PickSuccessor(Team& t, bool preferOnline)
    {
        for (int pass = 0; pass < 2; ++pass)
        {
            const Role want = pass == 0 ? Role::Officer : Role::Member;
            for (auto& m : t.members)
                if (m.role == want && (!preferOnline || OnlineByKey(m.key)))
                    return &m;
        }
        return nullptr;
    }

    // ── Engine reads used by the authority (tick thread) ────────────────────
    TESObjectREFR* WorkshopRefFor(uint32_t formID)
    {
        if (formID == 0) return nullptr;
        TESForm* form = LookupFormByID(formID);
        if (!form || form->formType < kFormType_REFR || form->formType > kFormType_PHZD)
            return nullptr;
        TESObjectREFR* ref = static_cast<TESObjectREFR*>(form);
        return CoSyncWorkshopEngine::IsWorkshopRef(ref) ? ref : nullptr;
    }

    __declspec(noinline) TESObjectREFR* SafeWorkshopRef(uint32_t formID)
    {
        __try { return WorkshopRefFor(formID); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    bool ValidWorkshop(uint32_t formID) { return SafeWorkshopRef(formID) != nullptr; }

    // TESObjectREFR vfunc +0x2E0 = GetCurrentLocation (the Papyrus native
    // sub_141181460 is exactly this call, 1.11.240). BGSLocation's FULL name
    // is the settlement name the game shows ("Sanctuary Hills").
    __declspec(noinline) bool LocationNameImpl(TESObjectREFR* ref, char* out, size_t cap)
    {
        __try
        {
            using GetLoc_t = BGSLocation* (__fastcall*)(TESObjectREFR*);
            auto fn = (*reinterpret_cast<GetLoc_t**>(ref))[0x2E0 / 8];
            BGSLocation* loc = fn(ref);
            const char* name = loc ? loc->fullName.name.c_str() : nullptr;
            if (!name || !*name) return false;
            strncpy_s(out, cap, name, _TRUNCATE);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    std::string SettlementNameOf(uint32_t ws)
    {
        char buf[96] = {};
        TESObjectREFR* ref = SafeWorkshopRef(ws);
        if (ref && LocationNameImpl(ref, buf, sizeof(buf)))
        {
            std::string n = buf, clean;
            for (unsigned char c : n) if (c >= 0x20 && c <= 0x7E && c != '|' && c != '%') clean += static_cast<char>(c);
            if (!clean.empty()) return clean.substr(0, 48);
        }
        char b[40];
        std::snprintf(b, sizeof(b), "Settlement %08X", ws);
        return b;
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

    // "When a player joins a team they are associated with the settlement the
    // host is currently using": a team with no settlement yet takes the
    // workshop the host stands in, when it is free.
    void AdoptHostSettlement(Team& t)
    {
        if (TeamSettlement(t.id))
            return;
        const uint32_t ws = s_nearestWorkshop;   // the authority's own nearest workbench
        if (ws == 0 || FindSettlement(ws) || !ValidWorkshop(ws))
            return;
        Settlement s;
        s.ws = ws;
        s.mode = SettlementMode::Team;
        s.teamId = t.id;
        s.name = SettlementNameOf(ws);
        s_state.settlements.push_back(s);
        LOG_INFO("[Teams] team '%s' adopts the host's settlement 0x%08X ('%s')", t.name.c_str(), ws, s.name.c_str());
        NotifyTeam(t, s.name + " is now the team settlement of " + t.name + ".", "");
    }

    // Host: online changes -- stale invitations and leaderless teams.
    void HostReconcileOnline()
    {
        const size_t before = s_state.invites.size();
        s_state.invites.erase(std::remove_if(s_state.invites.begin(), s_state.invites.end(),
            [&](const Invite& i) { return !OnlineByKey(i.key) || !FindTeam(i.teamId); }), s_state.invites.end());
        bool changed = s_state.invites.size() != before;

        for (auto& t : s_state.teams)
        {
            Member* leader = LeaderOf(t);
            if (!leader || OnlineByKey(leader->key))
                continue;
            Member* next = PickSuccessor(t, true);
            if (!next)
                continue;   // nobody online to lead: unchanged until someone is
            const std::string oldName = leader->name;
            leader->role = Role::Officer;
            next->role = Role::Leader;
            NotifyTeam(t, oldName + " disconnected. " + next->name + " now leads " + t.name + ".", "");
            LOG_INFO("[Teams] leader of '%s' left the session -- '%s' leads now", t.name.c_str(), next->name.c_str());
            changed = true;
        }
        if (changed)
            MarkChanged();
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
        const bool isHost = actorKey == MyKey();

        Member* me = nullptr;
        Team* mine = TeamOfKey(actorKey, &me);

        // ── No team needed ─────────────────────────────────────────────────
        if (op == "create")
        {
            if (mine) return Fail("Leave your current team first.");
            std::string name;
            if (!CleanName(arg(1), 3, 24, name)) return Fail("Team names are 3-24 letters, numbers or symbols.");
            for (const auto& t : s_state.teams)
                if (IEquals(t.name, name)) return Fail("That team name is taken.");
            if (s_state.teams.size() >= kMaxTeams) return Fail("This game has too many teams.");
            Team t;
            t.id = s_nextTeamId++;
            t.name = name;
            t.members.push_back({ actorKey, actorName, Role::Leader });
            RemoveInvitesFor(actorKey);
            s_state.teams.push_back(std::move(t));
            AdoptHostSettlement(s_state.teams.back());
            LOG_INFO("[Teams] '%s' created team '%s'", actorName.c_str(), name.c_str());
            return { true, "Team " + name + " created. You are its leader." };
        }

        if (op == "accept" || op == "decline")
        {
            const uint32_t tid = ToU32(arg(1));
            auto it = std::find_if(s_state.invites.begin(), s_state.invites.end(),
                [&](const Invite& i) { return i.teamId == tid && i.key == actorKey; });
            if (it == s_state.invites.end()) return Fail("That invitation is no longer valid.");
            const std::string inviter = it->inviter;
            s_state.invites.erase(it);
            Team* t = FindTeam(tid);
            if (!t) return Fail("That team no longer exists.");
            if (op == "decline")
            {
                if (Member* l = LeaderOf(*t)) NotifyKey(l->key, actorName + " declined the invitation to " + t->name + ".");
                return { true, "Invitation declined." };
            }
            if (mine) return Fail("Leave your current team first.");
            if (t->members.size() >= kMaxMembers) return Fail("That team is full.");
            t->members.push_back({ actorKey, actorName, Role::Member });
            RemoveInvitesFor(actorKey);
            NotifyTeam(*t, actorName + " joined " + t->name + ".", actorKey);
            AdoptHostSettlement(*t);
            return { true, "You joined " + t->name + "." };
        }

        if (op == "personal")   // claim the settlement I stand in as my own
        {
            const uint32_t ws = ToU32(arg(1));
            if (!ValidWorkshop(ws)) return Fail("Stand in a settlement (near its workbench) first.");
            if (Settlement* s = FindSettlement(ws))
            {
                if (s->mode == SettlementMode::Personal && s->owner == actorKey) return Fail("This is already your settlement.");
                return Fail(s->mode == SettlementMode::Team ? "This is a team settlement." : "This settlement belongs to another player.");
            }
            if (PersonalSettlement(actorKey)) return Fail("You already have a personal settlement. Release it first.");
            if (s_state.settlements.size() >= kMaxSettlements) return Fail("Too many assigned settlements.");
            Settlement s;
            s.ws = ws;
            s.mode = SettlementMode::Personal;
            s.owner = actorKey;
            s.name = SettlementNameOf(ws);
            s_state.settlements.push_back(s);
            LOG_INFO("[Teams] '%s' made 0x%08X ('%s') a personal settlement", actorName.c_str(), ws, s.name.c_str());
            return { true, s.name + " is now your personal settlement. Only you can build there." };
        }

        if (op == "personal_release")
        {
            Settlement* s = PersonalSettlement(actorKey);
            if (!s) return Fail("You do not have a personal settlement.");
            const std::string n = s->name;
            const uint32_t ws = s->ws;
            s_state.settlements.erase(std::remove_if(s_state.settlements.begin(), s_state.settlements.end(),
                [&](const Settlement& x) { return x.ws == ws; }), s_state.settlements.end());
            return { true, n + " is no longer your personal settlement." };
        }

        if (op == "admin_release")   // host: return any settlement to unassigned
        {
            if (!isHost) return Fail("Only the host can do that.");
            const uint32_t ws = ToU32(arg(1));
            Settlement* s = FindSettlement(ws);
            if (!s) return Fail("That settlement is not assigned.");
            const std::string n = s->name;
            s_state.settlements.erase(std::remove_if(s_state.settlements.begin(), s_state.settlements.end(),
                [&](const Settlement& x) { return x.ws == ws; }), s_state.settlements.end());
            return { true, n + " is unassigned again." };
        }

        if (op == "rules")
        {
            if (!isHost) return Fail("Only the host can change workshop rules.");
            const uint32_t mode = ToU32(arg(1));
            if (mode > 2) return Fail("Unknown build limit mode.");
            const uint32_t limit = ToU32(arg(2));
            if (mode == 1 && (limit < 1 || limit > 100000)) return Fail("Custom limit must be 1-100000.");
            s_state.rules.limitMode = static_cast<BuildLimitMode>(mode);
            if (limit >= 1 && limit <= 100000) s_state.rules.customLimit = limit;
            s_state.rules.freeBuild = arg(3) == "1";
            return { true, "Workshop rules saved." };
        }

        // ── Everything below needs a team ──────────────────────────────────
        if (!mine || !me) return Fail("You are not in a team.");
        const bool leader = me->role == Role::Leader;
        const bool officer = me->role == Role::Officer;

        auto target = [&](const std::string& key) -> Member* {
            for (auto& m : mine->members) if (m.key == key) return &m;
            return nullptr;
        };

        if (op == "leave")
        {
            const std::string tname = mine->name;
            const uint32_t tid = mine->id;
            RemoveMember(*mine, actorKey);
            if (mine->members.empty())
            {
                DisbandTeam(tid);
                LOG_INFO("[Teams] '%s' disbanded (last member left)", tname.c_str());
                return { true, "You left " + tname + ". It had no members left and was disbanded." };
            }
            if (leader)
            {
                Member* next = PickSuccessor(*mine, false);
                if (next) next->role = Role::Leader;
                NotifyTeam(*mine, actorName + " left. " + tname + " is now led by " + (next ? next->name : std::string("nobody")) + ".", "");
            }
            else
                NotifyTeam(*mine, actorName + " left " + tname + ".", "");
            return { true, "You left " + tname + "." };
        }

        if (op == "disband")
        {
            if (!leader) return Fail("Only the leader can disband the team.");
            const std::string tname = mine->name;
            NotifyTeam(*mine, tname + " was disbanded by its leader.", actorKey);
            DisbandTeam(mine->id);
            return { true, tname + " was disbanded." };
        }

        if (op == "rename")
        {
            if (!leader) return Fail("Only the leader can rename the team.");
            std::string name;
            if (!CleanName(arg(1), 3, 24, name)) return Fail("Team names are 3-24 letters, numbers or symbols.");
            for (const auto& t : s_state.teams)
                if (t.id != mine->id && IEquals(t.name, name)) return Fail("That team name is taken.");
            mine->name = name;
            return { true, "Team renamed to " + name + "." };
        }

        if (op == "invite")
        {
            if (!leader && !officer) return Fail("Only the leader and officers can invite players.");
            const std::string& tk = arg(1);
            const Online* t = OnlineByKey(tk);
            if (!t) return Fail("That player is not online.");
            if (tk == actorKey) return Fail("You are already in the team.");
            if (TeamOfKey(tk)) return Fail(t->name + " is already in a team.");
            if (mine->members.size() >= kMaxMembers) return Fail("Your team is full.");
            for (const auto& i : s_state.invites)
                if (i.teamId == mine->id && i.key == tk) return Fail(t->name + " already has an invitation from your team.");
            s_state.invites.push_back({ mine->id, tk, actorName, Now() + kInviteSeconds });
            NotifyKey(tk, actorName + " invited you to join " + mine->name + ". Open Multiplayer (F10) > Team to answer.");
            return { true, "Invitation sent to " + t->name + "." };
        }

        if (op == "kick")
        {
            Member* t = target(arg(1));
            if (!t) return Fail("That player is not in your team.");
            if (t->key == actorKey) return Fail("Use Leave Team instead.");
            if (!leader && !(officer && t->role == Role::Member))
                return Fail(officer ? "Officers can only remove members." : "Only the leader and officers can remove players.");
            const std::string tname = t->name, tkey = t->key;
            RemoveMember(*mine, tkey);
            NotifyKey(tkey, "You were removed from " + mine->name + ".");
            NotifyTeam(*mine, tname + " was removed from the team.", actorKey);
            return { true, tname + " was removed from the team." };
        }

        if (op == "promote" || op == "demote")
        {
            if (!leader) return Fail("Only the leader can promote or demote.");
            Member* t = target(arg(1));
            if (!t || t->key == actorKey) return Fail("Choose another member of your team.");
            if (op == "promote")
            {
                if (t->role != Role::Member) return Fail(t->name + " is already an officer.");
                t->role = Role::Officer;
                NotifyKey(t->key, "You were promoted to Officer of " + mine->name + ".");
                return { true, t->name + " is now an Officer." };
            }
            if (t->role != Role::Officer) return Fail(t->name + " is not an officer.");
            t->role = Role::Member;
            NotifyKey(t->key, "You were demoted to Member of " + mine->name + ".");
            return { true, t->name + " is now a Member." };
        }

        if (op == "transfer")
        {
            if (!leader) return Fail("Only the leader can hand over leadership.");
            Member* t = target(arg(1));
            if (!t || t->key == actorKey) return Fail("Choose another member of your team.");
            me->role = Role::Officer;
            t->role = Role::Leader;
            NotifyTeam(*mine, t->name + " now leads " + mine->name + ".", "");
            return { true, "Leadership handed to " + t->name + "." };
        }

        if (op == "settle")   // make the settlement I stand in the team settlement
        {
            if (!leader && !officer) return Fail("Only the leader and officers can choose the team settlement.");
            const uint32_t ws = ToU32(arg(1));
            if (!ValidWorkshop(ws)) return Fail("Stand in a settlement (near its workbench) first.");
            if (Settlement* s = FindSettlement(ws))
            {
                if (s->mode == SettlementMode::Team && s->teamId == mine->id) return Fail("This already is your team settlement.");
                return Fail(s->mode == SettlementMode::Team ? "This is another team's settlement." :
                            "This is a player's personal settlement.");
            }
            // One team settlement: moving it releases the old one.
            s_state.settlements.erase(std::remove_if(s_state.settlements.begin(), s_state.settlements.end(),
                [&](const Settlement& x) { return x.mode == SettlementMode::Team && x.teamId == mine->id; }), s_state.settlements.end());
            if (s_state.settlements.size() >= kMaxSettlements) return Fail("Too many assigned settlements.");
            Settlement s;
            s.ws = ws;
            s.mode = SettlementMode::Team;
            s.teamId = mine->id;
            s.name = SettlementNameOf(ws);
            s_state.settlements.push_back(s);
            NotifyTeam(*mine, s.name + " is now the team settlement of " + mine->name + ".", actorKey);
            return { true, s.name + " is now your team settlement." };
        }

        if (op == "settle_clear")
        {
            if (!leader && !officer) return Fail("Only the leader and officers can do that.");
            Settlement* s = TeamSettlement(mine->id);
            if (!s) return Fail("Your team has no settlement.");
            const std::string n = s->name;
            s_state.settlements.erase(std::remove_if(s_state.settlements.begin(), s_state.settlements.end(),
                [&](const Settlement& x) { return x.mode == SettlementMode::Team && x.teamId == mine->id; }), s_state.settlements.end());
            NotifyTeam(*mine, n + " is no longer the team settlement.", actorKey);
            return { true, n + " is no longer your team settlement." };
        }

        return Fail("Unknown team request.");
    }

    void RunAuthority(const std::string& actorKey, const std::string& actorName,
                      const std::vector<std::string>& args, uint32_t replyConn)
    {
        const Result r = Handle(actorKey, actorName, args);
        LOG_INFO("[Teams] request '%s' from '%s' -> %s (%s)", args.empty() ? "" : args[0].c_str(),
            actorName.c_str(), r.ok ? "OK" : "DENIED", r.text.c_str());
        if (r.ok)
            MarkChanged();
        if (replyConn == 0)
        {
            s_lastResult = std::string(r.ok ? "1|" : "0|") + r.text;
            CoSyncGameAPI::QueueNotification(r.text.c_str());
        }
        else
            CoSyncTransport::SendTo(replyConn, std::string("TR|") + (r.ok ? "1|" : "0|") + Pct(r.text));
    }

    // Menu command -> request arguments. Unknown commands are dropped.
    bool UiToArgs(const std::string& cmd, const std::string& arg, std::vector<std::string>& out)
    {
        static const char* const kOps[] = {
            "create", "rename", "disband", "invite", "accept", "decline", "leave",
            "kick", "promote", "demote", "transfer", "settle", "settle_clear",
            "personal", "personal_release", "admin_release", "rules",
        };
        if (cmd.rfind("team_", 0) != 0)
            return false;
        const std::string op = cmd.substr(5);
        bool known = false;
        for (const char* k : kOps) if (op == k) known = true;
        if (!known)
            return false;
        out.clear();
        out.push_back(op);
        if (!arg.empty())
            for (const std::string& s : Split(arg, '|'))
                out.push_back(s);
        // "here" operations: the workbench this machine's player stands at.
        if (op == "settle" || op == "personal")
        {
            out.resize(1);
            out.push_back(std::to_string(s_nearestWorkshop));
        }
        return true;
    }
}


namespace CoSyncTeams
{
    void OnSessionStart(bool isHost)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        s_inSession = true;
        s_buckets.clear();
        s_lastResult.clear();
        if (!isHost)
            s_state = State{};   // a client's view is whatever the host sends
        s_dirty = true;
        LOG_INFO("[Teams] session start (%s)", isHost ? "host" : "client");
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

    bool IsAuthority() { return AuthorityNow(); }

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
            if (n != s_nearestWorkshop)
                s_nearestName = n ? SettlementNameOf(n) : std::string();
            s_nearestWorkshop = n;
        }

        std::lock_guard<std::recursive_mutex> lk(s_mx);
        for (const UiCmd& c : cmds)
        {
            std::vector<std::string> args;
            if (!UiToArgs(c.cmd, c.arg, args))
                continue;
            if (AuthorityNow())
            {
                RebuildOnline();
                RunAuthority(MyKey(), CoSyncNet::GetMyName(), args, 0);
            }
            else if (CoSyncNet::IsConnected())
            {
                std::string msg = "TQ";
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
            RebuildOnline();
            HostReconcileOnline();
            s_dirty = false;
            Publish();
        }
    }

    bool IsTeamMessage(const std::string& msg)
    {
        return msg.size() >= 3 && msg[0] == 'T' && msg[2] == '|' &&
               (msg[1] == 'Q' || msg[1] == 'R' || msg[1] == 'S' || msg[1] == 'N');
    }

    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole)
    {
        LOG_REQUIREMENT("CoSyncTeams::OnReceive", "CoSync tick. Untrusted input: the host acts only on the sender's connection identity.");
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
                LOG_WARN("[Teams] RX malformed state -- ignored");
                return;
            }
            s_state = std::move(st);
            LOG_INFO("[Teams] RX state v%u (%zu team(s), %zu settlement(s), %zu online)", s_state.version,
                s_state.teams.size(), s_state.settlements.size(), s_state.online.size());
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

    void HostOnPeerJoined(uint32_t)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        s_dirty = true;   // everyone's online list changed; the newcomer gets the state too
    }

    void HostMarkDirty()
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        s_dirty = true;
    }

    bool CheckWorkshop(const std::string& key, uint32_t ws, WorkshopOp op, std::string* whyNot)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        (void)op;
        const Settlement* s = FindSettlement(ws);
        if (!s)
            return true;   // unassigned: anyone
        auto no = [&](const std::string& w) { if (whyNot) *whyNot = w; return false; };
        if (s->mode == SettlementMode::Personal)
        {
            if (s->owner == key) return true;
            return no(s->name + " is " + NameOfKey(s->owner) + "'s personal settlement. Only they can change it.");
        }
        Team* mine = TeamOfKey(key);
        if (mine && mine->id == s->teamId) return true;
        const Team* owner = FindTeam(s->teamId);
        return no(s->name + " is the team settlement of " + (owner ? owner->name : std::string("another team")) +
                  ". Only its members can change it.");
    }

    bool CheckLocalWorkshop(uint32_t ws, WorkshopOp op, std::string* whyNot)
    {
        return CheckWorkshop(MyKey(), ws, op, whyNot);
    }

    std::vector<uint32_t> GetAssignedSettlements()
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        std::vector<uint32_t> out;
        for (const auto& s : s_state.settlements) out.push_back(s.ws);
        return out;
    }

    SettlementMode GetSettlementMode(uint32_t ws)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        const Settlement* s = FindSettlement(ws);
        return s ? s->mode : SettlementMode::None;
    }

    bool GetSettlementAudience(uint32_t ws, std::vector<uint32_t>& conns)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        conns.clear();
        const Settlement* s = FindSettlement(ws);
        if (!s) return false;
        const std::string me = MyKey();
        auto add = [&](const std::string& key) {
            if (key == me) { conns.push_back(0); return; }
            if (const Online* o = OnlineByKey(key))
                if (o->conn != 0) conns.push_back(o->conn);
        };
        if (s->mode == SettlementMode::Personal)
            add(s->owner);
        else if (const Team* t = FindTeam(s->teamId))
            for (const auto& m : t->members) add(m.key);
        return true;
    }

    bool HostIsParticipant(uint32_t conn, uint32_t ws)
    {
        const std::string key = conn == 0 ? MyKey() : CoSyncProfiles::HostGetConnKey(conn);
        if (key.empty()) return false;
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        const Settlement* s = FindSettlement(ws);
        if (!s) return false;
        if (s->mode == SettlementMode::Personal) return s->owner == key;
        Team* t = TeamOfKey(key);
        return t && t->id == s->teamId;
    }

    bool IsLocalParticipant(uint32_t ws)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        const Settlement* s = FindSettlement(ws);
        if (!s) return false;
        const std::string me = MyKey();
        if (s->mode == SettlementMode::Personal) return s->owner == me;
        Team* t = TeamOfKey(me);
        return t && t->id == s->teamId;
    }

    Rules GetRules()
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        return s_state.rules;
    }

    bool GetTeamAudience(const std::string& playerKey, std::vector<uint32_t>& conns, std::string& teamName)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        conns.clear();
        if (AuthorityNow() && CoSyncTransport::IsInitialized())
            RebuildOnline();
        const Team* t = TeamOfKey(playerKey);
        if (!t)
            return false;
        teamName = t->name;
        const std::string me = MyKey();
        for (const auto& m : t->members)
        {
            if (m.key == me) { conns.push_back(0); continue; }
            if (const Online* o = OnlineByKey(m.key))
                if (o->conn != 0)
                    conns.push_back(o->conn);
        }
        return true;
    }

    std::string TeamNameOfKey(const std::string& playerKey)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        const Team* t = TeamOfKey(playerKey);
        return t ? t->name : std::string();
    }

    std::string KeyForEntity(uint32_t entityID)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        if (AuthorityNow() && CoSyncTransport::IsInitialized())
            RebuildOnline();
        const Online* o = OnlineByEid(entityID);
        return o ? o->key : std::string();
    }

    bool AreTeammates(uint32_t entityA, uint32_t entityB)
    {
        if (!entityA || !entityB || entityA == entityB)
            return false;
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        if (AuthorityNow() && CoSyncTransport::IsInitialized())
            RebuildOnline();
        const Online* a = OnlineByEid(entityA);
        const Online* b = OnlineByEid(entityB);
        if (!a || !b || a->key.empty() || b->key.empty())
            return false;
        const Team* ta = TeamOfKey(a->key);
        return ta && ta == TeamOfKey(b->key);
    }

    bool IsLocalTeammate(uint32_t entityID)
    {
        return AreTeammates(CoSyncNet::GetMyEntityID(), entityID);
    }

    bool GetBadge(uint32_t entityID, Badge& out)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        const Online* o = OnlineByEid(entityID);
        if (!o) return false;
        Member* m = nullptr;
        const Team* t = TeamOfKey(o->key, &m);
        if (!t || !m) return false;
        out.team = t->name;
        out.role = m->role;
        return true;
    }

    const char* RoleName(Role r) { return RoleText(r); }

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

    std::string BuildUiJson()
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        const std::string me = MyKey();
        Member* myMember = nullptr;
        const Team* mine = TeamOfKey(me, &myMember);
        const bool authority = AuthorityNow();

        std::string j = "{";
        j += "\"authority\":"; j += authority ? "true" : "false";
        j += ",\"session\":"; j += CoSyncTransport::IsInitialized() ? "true" : "false";
        j += ",\"me\":{\"key\":\"" + JsonEscape(me) + "\",\"team\":" + std::to_string(mine ? mine->id : 0u);
        j += ",\"role\":" + std::to_string(myMember ? static_cast<unsigned>(myMember->role) : 0u) + "}";

        const Rules& r = s_state.rules;
        j += ",\"rules\":{\"limitMode\":" + std::to_string(static_cast<unsigned>(r.limitMode));
        j += ",\"customLimit\":" + std::to_string(r.customLimit);
        j += ",\"freeBuild\":"; j += r.freeBuild ? "true" : "false";
        j += "}";

        // The settlement the player stands in, who it belongs to, and whether
        // this player may build there.
        {
            const uint32_t ws = s_nearestWorkshop;
            const Settlement* s = ws ? FindSettlement(ws) : nullptr;
            std::string why;
            j += ",\"nearest\":{\"id\":" + std::to_string(ws);
            j += ",\"name\":\"" + JsonEscape(s ? s->name : s_nearestName) + "\"";
            j += ",\"mode\":" + std::to_string(s ? static_cast<unsigned>(s->mode) : 0u);
            j += ",\"teamId\":" + std::to_string(s ? s->teamId : 0u);
            const Team* st = (s && s->mode == SettlementMode::Team) ? FindTeam(s->teamId) : nullptr;
            j += ",\"teamName\":\"" + JsonEscape(st ? st->name : std::string()) + "\"";
            j += ",\"ownerName\":\"" + JsonEscape(s && s->mode == SettlementMode::Personal ? NameOfKey(s->owner) : std::string()) + "\"";
            j += ",\"mine\":"; j += (s && s->mode == SettlementMode::Personal && s->owner == me) ? "true" : "false";
            if (ws)
            {
                const bool b = CheckWorkshop(me, ws, WorkshopOp::Build, &why);
                j += ",\"build\":"; j += b ? "true" : "false";
                j += ",\"why\":\"" + JsonEscape(b ? std::string() : why) + "\"";
            }
            j += "}";
        }

        j += ",\"teams\":[";
        for (size_t ti = 0; ti < s_state.teams.size(); ++ti)
        {
            const Team& t = s_state.teams[ti];
            if (ti) j += ",";
            std::string leaderName;
            for (const auto& m : t.members) if (m.role == Role::Leader) leaderName = m.name;
            const Settlement* ts = nullptr;
            for (const auto& s : s_state.settlements) if (s.mode == SettlementMode::Team && s.teamId == t.id) ts = &s;
            j += "{\"id\":" + std::to_string(t.id) + ",\"name\":\"" + JsonEscape(t.name) + "\"";
            j += ",\"leaderName\":\"" + JsonEscape(leaderName) + "\"";
            j += ",\"settlement\":" + std::to_string(ts ? ts->ws : 0u);
            j += ",\"settlementName\":\"" + JsonEscape(ts ? ts->name : std::string()) + "\"";
            j += ",\"members\":[";
            for (size_t i = 0; i < t.members.size(); ++i)
            {
                const Member& m = t.members[i];
                if (i) j += ",";
                j += "{\"key\":\"" + JsonEscape(m.key) + "\",\"name\":\"" + JsonEscape(m.name) + "\",\"role\":" +
                     std::to_string(static_cast<unsigned>(m.role)) + ",\"online\":";
                j += OnlineByKey(m.key) ? "true" : "false";
                j += "}";
            }
            j += "]}";
        }
        j += "],\"settlements\":[";
        for (size_t i = 0; i < s_state.settlements.size(); ++i)
        {
            const Settlement& s = s_state.settlements[i];
            if (i) j += ",";
            const Team* t = s.mode == SettlementMode::Team ? FindTeam(s.teamId) : nullptr;
            j += "{\"id\":" + std::to_string(s.ws) + ",\"mode\":" + std::to_string(static_cast<unsigned>(s.mode));
            j += ",\"name\":\"" + JsonEscape(s.name) + "\"";
            j += ",\"teamId\":" + std::to_string(s.teamId);
            j += ",\"teamName\":\"" + JsonEscape(t ? t->name : std::string()) + "\"";
            j += ",\"ownerName\":\"" + JsonEscape(s.mode == SettlementMode::Personal ? NameOfKey(s.owner) : std::string()) + "\"";
            j += ",\"mine\":"; j += (s.mode == SettlementMode::Personal && s.owner == me) ? "true" : "false";
            j += "}";
        }
        j += "],\"invites\":[";
        bool first = true;
        for (const auto& i : s_state.invites)
        {
            if (i.key != me) continue;
            const Team* t = FindTeam(i.teamId);
            if (!t) continue;
            if (!first) j += ",";
            first = false;
            j += "{\"team\":" + std::to_string(t->id) + ",\"name\":\"" + JsonEscape(t->name) + "\",\"by\":\"" +
                 JsonEscape(i.inviter) + "\",\"members\":" + std::to_string(t->members.size()) + "}";
        }
        j += "],\"pending\":[";   // invitations my team has sent and not been answered
        first = true;
        for (const auto& i : s_state.invites)
        {
            if (!mine || i.teamId != mine->id) continue;
            if (!first) j += ",";
            first = false;
            j += "\"" + JsonEscape(i.key) + "\"";
        }
        j += "],\"online\":[";
        for (size_t i = 0; i < s_state.online.size(); ++i)
        {
            const Online& o = s_state.online[i];
            if (i) j += ",";
            const Team* ot = TeamOfKey(o.key);
            j += "{\"key\":\"" + JsonEscape(o.key) + "\",\"name\":\"" + JsonEscape(o.name) + "\",\"team\":" +
                 std::to_string(ot ? ot->id : 0u) + ",\"you\":";
            j += o.key == me ? "true" : "false";
            j += "}";
        }
        j += "],\"lastResult\":\"" + JsonEscape(s_lastResult) + "\"}";
        return j;
    }

    void SaveRecord(const F4SESerializationInterface* ser)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        // Only the world's owner saves teams; a client's copy is the host's.
        if (!AuthorityNow())
            return;
        const std::string body = Serialize(false);
        const uint32_t len = static_cast<uint32_t>(body.size());
        ser->OpenRecord(kRecordType, kRecordVersion);
        ser->WriteRecordData(&s_nextTeamId, 4);
        ser->WriteRecordData(&len, 4);
        if (len)
            ser->WriteRecordData(body.data(), len);
        LOG_INFO("[Teams] Co-save: wrote %zu team(s), %zu settlement(s), %u bytes",
            s_state.teams.size(), s_state.settlements.size(), len);
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
        // A client loading the host's world gets the live state from the host.
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
            s_nextTeamId = (std::max)(nextId, maxId + 1);
            s_dirty = true;
            LOG_INFO("[Teams] Co-save: loaded %zu team(s), %zu settlement(s)", s_state.teams.size(), s_state.settlements.size());
        }
        return true;
    }

    void Revert()
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        if (!AuthorityNow())
            return;
        s_state = State{};
        s_nextTeamId = 1;
        s_dirty = true;
    }
}
