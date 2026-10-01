#include "CoSyncFormIdMap.h"

#include "CoSyncModSync.h"
#include "ConsoleLogger.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint16_t kMissing = 0xFFFF;

    struct Table
    {
        uint16_t full[256];
        uint16_t light[4096];
        void Clear()
        {
            for (auto& v : full)  v = kMissing;
            for (auto& v : light) v = kMissing;
        }
    };

    std::mutex        s_mx;
    Table             s_toHost;
    Table             s_toLocal;
    std::atomic<bool> s_active{ false };

    void Split(const std::string& lo, std::vector<std::string>& full, std::vector<std::string>& light)
    {
        full.clear();
        light.clear();
        const size_t semi = lo.find(';');
        auto take = [](const std::string& part, char kind, std::vector<std::string>& out)
        {
            std::stringstream ss(part);
            std::string tok;
            while (std::getline(ss, tok, ','))
                if (tok.size() > 2 && tok[0] == kind && tok[1] == ':')
                    out.push_back(tok.substr(2));
        };
        take(lo.substr(0, semi), 'F', full);
        if (semi != std::string::npos)
            take(lo.substr(semi + 1), 'L', light);
    }

    uint32_t MapWith(const Table& t, uint32_t id)
    {
        if (id == 0)
            return 0;
        const uint32_t hi = id >> 24;
        if (hi == 0xFF)
            return id;                                  // dynamic: machine-local by design
        if (hi == 0xFE)
        {
            const uint32_t li = (id >> 12) & 0xFFF;
            const uint16_t m = t.light[li];
            if (m == kMissing) return 0;
            return 0xFE000000u | (static_cast<uint32_t>(m) << 12) | (id & 0xFFFu);
        }
        const uint16_t m = t.full[hi];
        if (m == kMissing) return 0;
        return (static_cast<uint32_t>(m) << 24) | (id & 0x00FFFFFFu);
    }

    using MapFn = uint32_t(*)(uint32_t);

    // ── field helpers ─────────────────────────────────────────────────────
    std::vector<std::string> Fields(const std::string& msg)
    {
        std::vector<std::string> f;
        size_t start = 0;
        for (;;)
        {
            const size_t bar = msg.find('|', start);
            if (bar == std::string::npos) { f.push_back(msg.substr(start)); break; }
            f.push_back(msg.substr(start, bar - start));
            start = bar + 1;
        }
        return f;
    }

    std::string Join(const std::vector<std::string>& f)
    {
        std::string out;
        for (size_t i = 0; i < f.size(); ++i)
        {
            if (i) out += '|';
            out += f[i];
        }
        return out;
    }

    bool ParseU32(const std::string& s, int base, uint32_t& v)
    {
        if (s.empty()) return false;
        char* e = nullptr;
        const unsigned long long x = std::strtoull(s.c_str(), &e, base);
        if (!e || *e != '\0' || x > 0xFFFFFFFFull) return false;
        v = static_cast<uint32_t>(x);
        return true;
    }

    std::string Dec(uint32_t v) { return std::to_string(v); }
    std::string Hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof(b), "%08X", v); return b; }

    // One decimal FormID field; negative index counts from the end.
    void D(std::vector<std::string>& f, int i, MapFn map)
    {
        if (i < 0) i += static_cast<int>(f.size());
        if (i < 0 || i >= static_cast<int>(f.size())) return;
        uint32_t v = 0;
        if (ParseU32(f[i], 10, v))
            f[i] = Dec(map(v));
    }

    // "a:b:c" (or "a") -> map the first component, decimal or hex.
    std::string MapHead(const std::string& tok, int base, MapFn map)
    {
        const size_t colon = tok.find(':');
        const std::string head = tok.substr(0, colon);
        uint32_t v = 0;
        if (!ParseU32(head, base, v)) return tok;
        return (base == 16 ? Hex(map(v)) : Dec(map(v))) + (colon == std::string::npos ? "" : tok.substr(colon));
    }

    // Comma list whose items are "fid[:...]".
    std::string MapHeadList(const std::string& s, int base, MapFn map)
    {
        std::string out, tok;
        std::stringstream ss(s);
        bool first = true;
        while (std::getline(ss, tok, ','))
        {
            if (!first) out += ',';
            first = false;
            out += MapHead(tok, base, map);
        }
        return out;
    }

    // Item mods: "ITEM:MOD/MOD,ITEM:MOD" -- every number is a hex FormID.
    std::string MapModList(const std::string& s, MapFn map)
    {
        std::string out, cur;
        auto flush = [&]()
        {
            uint32_t v = 0;
            out += ParseU32(cur, 16, v) ? Hex(map(v)) : cur;
            cur.clear();
        };
        for (char c : s)
        {
            if (c == ',' || c == ':' || c == '/') { flush(); out += c; }
            else cur += c;
        }
        flush();
        return out;
    }

    // Profile / looks text ("key=value" lines, hex FormIDs).
    std::string MapProfileText(const std::string& blob, MapFn map)
    {
        std::string out;
        std::stringstream ss(blob);
        std::string line;
        bool firstLine = true;
        while (std::getline(ss, line, '\n'))
        {
            if (!firstLine) out += '\n';
            firstLine = false;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) { out += line; continue; }
            const std::string key = line.substr(0, eq);
            const std::string val = line.substr(eq + 1);
            std::string nv = val;
            if (key == "perks" || key == "equip" || key == "comps" || key == "heads" || key == "items")
                nv = MapHeadList(val, 16, map);
            else if (key == "race" || key == "skin" || key == "hair" || key == "facetex")
            {
                uint32_t v = 0;
                if (ParseU32(val, 16, v)) nv = Hex(map(v));
            }
            else if (key == "mods")
            {
                // BASE:equipped:MOD/MOD,... -> the middle field is a flag, not an ID
                std::string o, tok;
                std::stringstream ts(val);
                bool first = true;
                while (std::getline(ts, tok, ','))
                {
                    if (!first) o += ',';
                    first = false;
                    const size_t c1 = tok.find(':');
                    const size_t c2 = (c1 == std::string::npos) ? std::string::npos : tok.find(':', c1 + 1);
                    if (c2 == std::string::npos) { o += tok; continue; }
                    o += MapModList(tok.substr(0, c1), map) + tok.substr(c1, c2 - c1 + 1) + MapModList(tok.substr(c2 + 1), map);
                }
                nv = o;
            }
            else if (key == "pos")
            {
                // I|W:SPACE:x,y,z,rot
                const size_t c1 = val.find(':');
                const size_t c2 = (c1 == std::string::npos) ? std::string::npos : val.find(':', c1 + 1);
                uint32_t v = 0;
                if (c2 != std::string::npos && ParseU32(val.substr(c1 + 1, c2 - c1 - 1), 16, v))
                    nv = val.substr(0, c1 + 1) + Hex(map(v)) + val.substr(c2);
            }
            out += key + "=" + nv;
        }
        if (!blob.empty() && blob.back() == '\n') out += '\n';
        return out;
    }

    // ── the schema ────────────────────────────────────────────────────────
    // Field 0 is the message tag. Every rule below was read off the message's
    // serializer (file noted); anything not listed carries no FormIDs.
    using Rule = void(*)(std::vector<std::string>&, MapFn);

    const std::unordered_map<std::string, Rule>& Schema()
    {
        static const std::unordered_map<std::string, Rule> s = {
            // Networking/EntitySerialization.h
            { "EC",  [](std::vector<std::string>& f, MapFn m) { D(f, 3, m); D(f, 8, m); D(f, 9, m); D(f, 10, m); } },
            { "EU",  [](std::vector<std::string>& f, MapFn m) { if (f.size() >= 16) { D(f, -8, m); D(f, -4, m); } } },
            // World/CoSyncWorldSync.cpp
            { "WS",  [](std::vector<std::string>& f, MapFn m) { D(f, 6, m); D(f, 7, m); } },
            { "WBF", [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); D(f, 9, m); D(f, 10, m); D(f, 11, m); D(f, 12, m); } },
            { "WDS", [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); D(f, 3, m); } },
            { "FU",  [](std::vector<std::string>& f, MapFn m) { D(f, 1, m); } },
            { "OBJ", [](std::vector<std::string>& f, MapFn m) { D(f, 1, m); } },
            { "PU",  [](std::vector<std::string>& f, MapFn m) { D(f, 1, m); } },
            // World/*
            { "DOOR", [](std::vector<std::string>& f, MapFn m) { D(f, 1, m); } },
            { "LOCK", [](std::vector<std::string>& f, MapFn m) { D(f, 1, m); } },
            { "MM",   [](std::vector<std::string>& f, MapFn m) { D(f, 1, m); } },
            { "WX",   [](std::vector<std::string>& f, MapFn m) { D(f, 1, m); } },
            { "NPCSEEN", [](std::vector<std::string>& f, MapFn m) { D(f, 1, m); D(f, 2, m); D(f, 5, m); D(f, 6, m); } },
            { "NPCDEAD", [](std::vector<std::string>& f, MapFn m) { D(f, 1, m); } },
            { "QS",          [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "QS_ST",       [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "QSREQ",       [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "QS_DONE",     [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "QS_OBJ_ALL",  [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "QS_OBJ_SET",  [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "QS_OBJ_DISP", [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "QS_TRK",      [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            // Items/*
            { "WM",  [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "WD",  [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); D(f, 3, m); } },
            { "CTR", [](std::vector<std::string>& f, MapFn m) { D(f, 1, m); D(f, 2, m); } },
            { "CTRD",[](std::vector<std::string>& f, MapFn m) { D(f, 1, m); D(f, 2, m); } },
            { "CONS",[](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "SPELL",[](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "DROPCREATE", [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "DROPSEEN",   [](std::vector<std::string>& f, MapFn m) { D(f, 1, m); D(f, 2, m); } },
            { "INV", [](std::vector<std::string>& f, MapFn m) { for (size_t i = 2; i < f.size(); ++i) f[i] = MapHead(f[i], 10, m); } },
            // Clothing / Character / Dialog / Papyrus
            { "EQUIP",  [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "OUTFIT", [](std::vector<std::string>& f, MapFn m) { for (int i = 2; i < static_cast<int>(f.size()); ++i) D(f, i, m); } },
            { "SPEC",   [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "ACT",    [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "DLG",    [](std::vector<std::string>& f, MapFn m) { if (f.size() > 1 && f[1] == "L") D(f, 3, m); } },
            { "COMP_REQ", [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "COMPOWN",  [](std::vector<std::string>& f, MapFn m) { D(f, 1, m); D(f, 2, m); } },
            { "IMODS",  [](std::vector<std::string>& f, MapFn m) { if (f.size() > 2) f[2] = MapModList(f[2], m); } },
            { "LOOKS",  [](std::vector<std::string>& f, MapFn m)
                        {
                            // the blob may itself contain '|' (names are sanitised, but be safe)
                            if (f.size() < 3) return;
                            std::string blob = f[2];
                            for (size_t i = 3; i < f.size(); ++i) blob += "|" + f[i];
                            f.resize(3);
                            f[2] = MapProfileText(blob, m);
                        } },
            // Weapons/*
            { "EXP",  [](std::vector<std::string>& f, MapFn m) { D(f, 1, m); } },
            { "SHOT", [](std::vector<std::string>& f, MapFn m) { D(f, 3, m); } },
            // World/CoSyncEnemySync.cpp: ENS|eid|ver|combat|target|class|life|spell,spell
            { "ENS",  [](std::vector<std::string>& f, MapFn m) { if (f.size() > 7) f[7] = MapHeadList(f[7], 10, m); } },
            { "WSD",  [](std::vector<std::string>& f, MapFn m) { D(f, 1, m); D(f, 2, m); } },
            { "WSO",  [](std::vector<std::string>& f, MapFn m) { D(f, 1, m); D(f, 2, m); } },
            { "WSE",  [](std::vector<std::string>& f, MapFn m) { D(f, 1, m); } },
            { "THRP", [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); D(f, 3, m); D(f, 10, m); } },
            { "THRM", [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "THRX", [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            // Armor/CoSyncPowerArmorSync.cpp
            { "PA",   [](std::vector<std::string>& f, MapFn m)
                      {
                          D(f, 2, m); D(f, 3, m);
                          if (f.size() > 7 && f[7] == "1")
                              for (int i = 8; i <= 13; ++i) D(f, i, m);
                      } },
            { "PAI",  [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); D(f, 3, m); if (f.size() > 7) f[7] = MapHeadList(f[7], 10, m); } },
            { "PAMODS", [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); D(f, 3, m); if (f.size() > 7) f[7] = MapModList(f[7], m); } },
            // World/CoSyncWorkshopAuthority.cpp
            { "WKA", [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); D(f, 3, m); D(f, 10, m); D(f, 11, m); D(f, 12, m); D(f, 14, m); } },
            { "WKV", [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); D(f, 3, m); D(f, 10, m); } },
            { "WKX", [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); D(f, 3, m); } },
            { "WKR", [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "WKN", [](std::vector<std::string>& f, MapFn m) { D(f, 2, m); } },
            { "WKQ", [](std::vector<std::string>& f, MapFn m)
                     {
                         if (f.size() < 2) return;
                         if (f[1] == "P") { D(f, 2, m); D(f, 3, m); D(f, 10, m); D(f, 11, m); D(f, 12, m); return; }
                         const bool byRef = f.size() > 2 && f[2] == "R";   // "N" = a network id, not a FormID
                         if (f[1] == "M") { if (byRef) D(f, 3, m); D(f, 4, m); D(f, 11, m); D(f, 12, m); return; }
                         if (byRef) D(f, 3, m);
                         D(f, 4, m); D(f, 8, m); D(f, 9, m);
                     } },
            { "WKC", [](std::vector<std::string>& f, MapFn m)
                     {
                         if (f.size() < 2) return;
                         std::string out, tok;
                         std::stringstream ss(f[1]);
                         bool first = true;
                         while (std::getline(ss, tok, ';'))
                         {
                             if (!first) out += ';';
                             first = false;
                             const size_t eq = tok.find('=');
                             uint32_t v = 0;
                             if (eq != std::string::npos && ParseU32(tok.substr(0, eq), 10, v))
                                 out += Dec(m(v)) + tok.substr(eq);
                             else
                                 out += tok;
                         }
                         f[1] = out;
                     } },
            // World/CoSyncFactions.cpp
            { "FQ", [](std::vector<std::string>& f, MapFn m)
                    {
                        if (f.size() > 2 && (f[1] == "claim" || f[1] == "release" || f[1] == "rename_terr"))
                            D(f, 2, m);
                    } },
        };
        return s;
    }

    // FS is a multi-line body whose "T|faction|workshop|name" lines carry a
    // FormID; handled on the whole text because its lines contain '|'.
    std::string MapFactionState(const std::string& msg, MapFn m)
    {
        std::string out;
        std::stringstream ss(msg);
        std::string line;
        bool firstLine = true;
        while (std::getline(ss, line, '\n'))
        {
            if (!firstLine) out += '\n';
            firstLine = false;
            // the first line starts with "FS|" before the body's first record
            std::string prefix;
            std::string body = line;
            if (body.rfind("FS|", 0) == 0) { prefix = "FS|"; body = body.substr(3); }
            if (body.rfind("T|", 0) == 0)
            {
                std::vector<std::string> f = Fields(body);
                D(f, 2, m);
                body = Join(f);
            }
            out += prefix + body;
        }
        if (!msg.empty() && msg.back() == '\n') out += '\n';
        return out;
    }

    std::string Translate(const std::string& msg, MapFn map)
    {
        const size_t bar = msg.find('|');
        if (bar == std::string::npos)
            return msg;
        const std::string tag = msg.substr(0, bar);
        if (tag == "FS")
            return MapFactionState(msg, map);
        const auto& schema = Schema();
        auto it = schema.find(tag);
        if (it == schema.end())
            return msg;
        std::vector<std::string> f = Fields(msg);
        it->second(f, map);
        return Join(f);
    }

    uint32_t MapToHost(uint32_t id)
    {
        std::lock_guard<std::mutex> lk(s_mx);
        return MapWith(s_toHost, id);
    }
    uint32_t MapToLocal(uint32_t id)
    {
        std::lock_guard<std::mutex> lk(s_mx);
        return MapWith(s_toLocal, id);
    }
}

namespace CoSyncFormIdMap
{
    void SetHostLoadOrder(const std::string& hostLo)
    {
        const std::string localLo = CoSyncModSync::BuildLoadOrder();
        std::vector<std::string> hf, hl, lf, ll;
        Split(hostLo, hf, hl);
        Split(localLo, lf, ll);
        if (hf.empty() || lf.empty())
        {
            LOG_WARN("[FormIdMap] Load order unavailable (host %zu, local %zu plugins) -- FormIDs sent as they are",
                hf.size(), lf.size());
            s_active.store(false);
            return;
        }

        std::lock_guard<std::mutex> lk(s_mx);
        s_toHost.Clear();
        s_toLocal.Clear();
        bool identity = (hf.size() == lf.size()) && (hl.size() == ll.size());
        int missingHere = 0, missingThere = 0;
        for (size_t i = 0; i < lf.size() && i < 0xFE; ++i)
            for (size_t j = 0; j < hf.size() && j < 0xFE; ++j)
                if (lf[i] == hf[j])
                {
                    s_toHost.full[i]  = static_cast<uint16_t>(j);
                    s_toLocal.full[j] = static_cast<uint16_t>(i);
                    if (i != j) identity = false;
                    break;
                }
        for (size_t i = 0; i < ll.size() && i < 4096; ++i)
            for (size_t j = 0; j < hl.size() && j < 4096; ++j)
                if (ll[i] == hl[j])
                {
                    s_toHost.light[i]  = static_cast<uint16_t>(j);
                    s_toLocal.light[j] = static_cast<uint16_t>(i);
                    if (i != j) identity = false;
                    break;
                }
        for (size_t i = 0; i < lf.size() && i < 0xFE; ++i) if (s_toHost.full[i] == kMissing) { ++missingThere; identity = false; }
        for (size_t j = 0; j < hf.size() && j < 0xFE; ++j) if (s_toLocal.full[j] == kMissing) { ++missingHere; identity = false; }
        for (size_t i = 0; i < ll.size() && i < 4096; ++i) if (s_toHost.light[i] == kMissing) { ++missingThere; identity = false; }
        for (size_t j = 0; j < hl.size() && j < 4096; ++j) if (s_toLocal.light[j] == kMissing) { ++missingHere; identity = false; }

        s_active.store(!identity);
        if (identity)
            LOG_INFO("[FormIdMap] Load order identical to the host's -- no FormID translation needed");
        else
            LOG_INFO("[FormIdMap] Translating FormIDs to/from the host's load order "
                     "(host %zu+%zu plugins, here %zu+%zu; %d host plugin(s) missing here, %d of ours missing there)",
                hf.size(), hl.size(), lf.size(), ll.size(), missingHere, missingThere);
    }

    void Reset()
    {
        s_active.store(false);
        std::lock_guard<std::mutex> lk(s_mx);
        s_toHost.Clear();
        s_toLocal.Clear();
    }

    bool IsActive() { return s_active.load(); }

    uint32_t ToHost(uint32_t id)  { return IsActive() ? MapToHost(id) : id; }
    uint32_t ToLocal(uint32_t id) { return IsActive() ? MapToLocal(id) : id; }

    std::string Outbound(const std::string& msg)
    {
        return IsActive() ? Translate(msg, &MapToHost) : msg;
    }

    std::string Inbound(const std::string& msg)
    {
        return IsActive() ? Translate(msg, &MapToLocal) : msg;
    }
}
