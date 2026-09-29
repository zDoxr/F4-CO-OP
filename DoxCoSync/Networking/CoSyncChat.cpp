#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncChat.h"

#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncProfiles.h"
#include "CoSyncFactions.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    struct Line
    {
        uint64_t    seq = 0;
        char        scope = 'G';
        uint32_t    senderEid = 0;
        std::string name;
        std::string faction;
        std::string text;
        uint64_t    timeMs = 0;
    };

    std::mutex             s_mx;
    std::deque<Line>       s_history;          // newest at the back
    uint64_t               s_seq = 0;
    constexpr size_t       kHistory = 100;

    struct Pending { char scope; std::string text; };
    std::deque<Pending>    s_outbox;

    // Host: per-connection budget (5 messages per 10 s, at most 2 per second).
    struct Budget { double windowStart = 0.0; int inWindow = 0; double last = 0.0; };
    std::unordered_map<uint32_t, Budget> s_budget;

    double Now() { return static_cast<double>(GetTickCount64()) / 1000.0; }

    std::string Pct(const std::string& s)
    {
        static const char* hex = "0123456789ABCDEF";
        std::string o;
        for (unsigned char c : s)
        {
            if (c == '%' || c == '|' || c == '\n' || c == '\r' || c < 0x20)
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

    std::vector<std::string> Split(const std::string& s, char sep, size_t maxParts)
    {
        std::vector<std::string> out;
        size_t a = 0;
        while (out.size() + 1 < maxParts)
        {
            const size_t b = s.find(sep, a);
            if (b == std::string::npos) break;
            out.push_back(s.substr(a, b - a));
            a = b + 1;
        }
        out.push_back(s.substr(a));
        return out;
    }

    // Well-formed UTF-8, no control characters, no overlong/surrogate forms,
    // trimmed, 1..kMaxBytes bytes.
    bool CleanText(const std::string& in, std::string& out)
    {
        size_t a = 0, b = in.size();
        while (a < b && (in[a] == ' ' || in[a] == '\t')) ++a;
        while (b > a && (in[b - 1] == ' ' || in[b - 1] == '\t')) --b;
        out = in.substr(a, b - a);
        if (out.empty() || out.size() > CoSyncChat::kMaxBytes)
            return false;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(out.data());
        const size_t n = out.size();
        for (size_t i = 0; i < n; )
        {
            const unsigned char c = p[i];
            if (c < 0x20 || c == 0x7F)
                return false;
            if (c < 0x80) { ++i; continue; }
            int len = 0;
            uint32_t cp = 0;
            if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
            else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
            else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
            else return false;
            if (i + len > n) return false;
            for (int k = 1; k < len; ++k)
            {
                if ((p[i + k] & 0xC0) != 0x80) return false;
                cp = (cp << 6) | (p[i + k] & 0x3F);
            }
            if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
                cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || (cp >= 0x80 && cp < 0xA0))
                return false;
            i += len;
        }
        return true;
    }

    std::string JsonEscape(const std::string& s)
    {
        std::string o;
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
            }
        }
        return o;
    }

    void Remember(char scope, uint32_t eid, const std::string& name, const std::string& faction, const std::string& text)
    {
        Line l;
        l.seq = ++s_seq;
        l.scope = scope;
        l.senderEid = eid;
        l.name = name;
        l.faction = faction;
        l.text = text;
        l.timeMs = GetTickCount64();
        s_history.push_back(std::move(l));
        while (s_history.size() > kHistory)
            s_history.pop_front();
    }

    std::string NameForConn(uint32_t conn, uint32_t& eidOut)
    {
        for (const auto& p : CoSyncNet::HostGetPeers())
            if (p.conn == conn) { eidOut = p.entityID; return p.name; }
        eidOut = 0;
        return std::string();
    }

    // Host: deliver one message. conn 0 = the host itself is the sender.
    void HostDeliver(char scope, const std::string& senderKey, uint32_t senderEid,
                     const std::string& senderName, const std::string& text)
    {
        std::string faction;
        std::vector<uint32_t> conns;   // 0 = the host
        if (scope == 'F')
        {
            if (!CoSyncFactions::GetFactionAudience(senderKey, conns, faction))
                return;   // not in a faction: nobody to send to
        }
        else
        {
            faction = CoSyncFactions::FactionNameOfKey(senderKey);
            conns.push_back(0);
            for (const auto& p : CoSyncNet::HostGetPeers())
                conns.push_back(p.conn);
        }

        const std::string wire = std::string("CHATM|") + scope + "|" + std::to_string(senderEid) + "|" +
                                 Pct(senderName) + "|" + Pct(faction) + "|" + Pct(text);
        for (uint32_t c : conns)
        {
            if (c == 0)
                Remember(scope, senderEid, senderName, faction, text);
            else
                CoSyncTransport::SendTo(c, wire);
        }
        LOG_INFO("[Chat] %s message from '%s' (%zu bytes) to %zu recipient(s)",
            scope == 'F' ? "faction" : "global", senderName.c_str(), text.size(), conns.size());
    }
}

namespace CoSyncChat
{
    void OnSessionStart()
    {
        std::lock_guard<std::mutex> lk(s_mx);
        s_outbox.clear();
        s_budget.clear();
    }

    void OnSessionEnd()
    {
        std::lock_guard<std::mutex> lk(s_mx);
        s_outbox.clear();
        s_budget.clear();
    }

    bool IsChatMessage(const std::string& msg)
    {
        return msg.rfind("CHAT|", 0) == 0 || msg.rfind("CHATM|", 0) == 0;
    }

    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole)
    {
        LOG_REQUIREMENT("CoSyncChat::OnReceive", "CoSync tick. Untrusted text: the host validates and picks recipients by the sender's connection identity.");
        if (msg.size() > 2048)
            return;
        std::lock_guard<std::mutex> lk(s_mx);

        if (msg.rfind("CHAT|", 0) == 0)
        {
            if (!isHostRole || conn == 0)
                return;
            const std::vector<std::string> v = Split(msg.substr(5), '|', 2);
            if (v.size() != 2 || (v[0] != "G" && v[0] != "F"))
                return;
            std::string text;
            if (!CleanText(Unpct(v[1]), text))
                return;

            Budget& b = s_budget[conn];
            const double t = Now();
            if (t - b.windowStart > 10.0) { b.windowStart = t; b.inWindow = 0; }
            if (b.inWindow >= 5 || t - b.last < 0.5)
            {
                CoSyncTransport::SendTo(conn, "CHATM|S|0|||" + Pct("You are sending messages too fast."));
                return;
            }
            ++b.inWindow;
            b.last = t;

            uint32_t eid = 0;
            const std::string name = NameForConn(conn, eid);
            const std::string key = CoSyncProfiles::HostGetConnKey(conn);
            if (eid == 0 || key.empty())
                return;   // not introduced yet
            HostDeliver(v[0][0], key, eid, name, text);
            return;
        }

        // CHATM: only from the host.
        if (isHostRole)
            return;
        const std::vector<std::string> v = Split(msg.substr(6), '|', 5);
        if (v.size() != 5 || v[0].size() != 1)
            return;
        const char scope = v[0][0];
        if (scope != 'G' && scope != 'F' && scope != 'S')
            return;
        std::string text = Unpct(v[4]);
        if (text.size() > kMaxBytes + 64)
            return;
        Remember(scope, static_cast<uint32_t>(std::strtoul(v[1].c_str(), nullptr, 10)),
                 Unpct(v[2]), Unpct(v[3]), text);
    }

    void Tick(double now)
    {
        (void)now;
        std::deque<Pending> out;
        {
            std::lock_guard<std::mutex> lk(s_mx);
            out.swap(s_outbox);
        }
        for (const Pending& p : out)
        {
            std::string text;
            if (!CleanText(p.text, text))
                continue;
            if (!CoSyncTransport::IsInitialized() || !CoSyncNet::IsConnected())
            {
                std::lock_guard<std::mutex> lk(s_mx);
                Remember('S', 0, "", "", "Not connected to a multiplayer session.");
                continue;
            }
            if (CoSyncTransport::IsHost())
            {
                std::lock_guard<std::mutex> lk(s_mx);
                HostDeliver(p.scope, CoSyncProfiles::GetMyProfileKey(), CoSyncNet::GetMyEntityID(),
                            CoSyncNet::GetMyName(), text);
            }
            else
                CoSyncTransport::Send(std::string("CHAT|") + p.scope + "|" + Pct(text));
        }
    }

    void QueueSend(char scope, const std::string& text)
    {
        if (scope != 'G' && scope != 'F')
            return;
        std::lock_guard<std::mutex> lk(s_mx);
        if (s_outbox.size() < 16 && text.size() <= kMaxBytes * 2)
            s_outbox.push_back({ scope, text });
    }

    uint64_t LatestSeq()
    {
        std::lock_guard<std::mutex> lk(s_mx);
        return s_seq;
    }

    std::string BuildJson(uint64_t afterSeq)
    {
        std::lock_guard<std::mutex> lk(s_mx);
        const uint32_t me = CoSyncNet::GetMyEntityID();
        std::string j = "{\"seq\":" + std::to_string(s_seq) + ",\"msgs\":[";
        bool first = true;
        for (const Line& l : s_history)
        {
            if (l.seq <= afterSeq) continue;
            if (!first) j += ",";
            first = false;
            j += "{\"seq\":" + std::to_string(l.seq);
            j += ",\"scope\":\"" + std::string(1, l.scope) + "\"";
            j += ",\"name\":\"" + JsonEscape(l.name) + "\"";
            j += ",\"faction\":\"" + JsonEscape(l.faction) + "\"";
            j += ",\"text\":\"" + JsonEscape(l.text) + "\"";
            j += ",\"me\":"; j += (l.senderEid != 0 && l.senderEid == me) ? "true" : "false";
            j += "}";
        }
        j += "]}";
        return j;
    }
}
