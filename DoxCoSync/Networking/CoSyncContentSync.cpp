#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <Windows.h>
#include <bcrypt.h>
#include <ShlObj.h>

#include "CoSyncContentSync.h"
#include "CoSyncContentManifest.h"

#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncWorldTransfer.h"
#include "CoSyncProfiles.h"
#include "CoSyncGameAPI.h"
#include "CoSyncSessionConfig.h"
#include "CoSyncPrismaUI.h"
#include "CoSyncWorld.h"
#include "GNS_Session.h"
#include "ConsoleLogger.h"
#include "GameData.h"

extern RelocPtr<DataHandler*> g_dataHandler;

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using namespace CoSyncContent;

namespace
{
    // ── Tunables ────────────────────────────────────────────────────────────
    constexpr size_t   kBigPartBytes      = 256 * 1024;   // one CSBIG part
    constexpr size_t   kBigMaxParts       = 64;           // 16 MB text at most
    constexpr int      kWindow            = 12;           // chunks in flight per download (3 MB)
    constexpr double   kChunkTimeoutMs    = 30000.0;      // re-request an unanswered chunk
    constexpr int      kMaxChunkTries     = 5;
    constexpr double   kStallMs           = 90000.0;      // nothing at all arrived
    constexpr double   kAnswerTimeoutMs   = 60000.0;      // host silent after CSREQ / CSWAIT
    constexpr double   kVerdictTimeoutMs  = 180000.0;     // host proving the report
    constexpr int      kHostMaxQueued     = 32;           // chunk requests queued per client
    constexpr size_t   kHostSendBudget    = 16u << 20;    // bytes sent per frame, all clients
    constexpr double   kBlockedKickMs     = 30000.0;      // a refused client that stays connected
    constexpr int      kMaxRequests       = 3;            // CSREQ per connection
    constexpr uint64_t kDiskMargin        = 512ull << 20; // free space left after a download
    constexpr uint64_t kMetaEveryBytes    = 16ull << 20;  // resume point saved this often
    constexpr double   kRejoinMaxAgeSec   = 30.0 * 60.0;
    constexpr double   kRejoinDelayMs     = 5000.0;

    double NowMs() { return static_cast<double>(GetTickCount64()); }

    // Debug.QuitGame, the vanilla Papyrus native (registered at 0x14111E1EE,
    // NativeFunction0<StaticFunctionTag,void>): sets Main's quit flag
    // (Main+0x24), after which the engine exits its main loop normally.
    // IDA 1.11.240, 2026-09-30.
    constexpr uintptr_t kRVA_QuitGame = 0x0111C8C0;
    constexpr uint8_t   kQuitGamePrologue[12] = { 0x48, 0x8B, 0x05, 0x69, 0x08, 0x1C, 0x02, 0xC6, 0x40, 0x24, 0x01, 0xC3 };

    // ── Paths ───────────────────────────────────────────────────────────────
    struct Paths
    {
        std::wstring root, data, state, staging, backup, journal, pending, rejoin, cache, include;
        std::wstring pluginsTxt, ccc, catalog;
        std::string  dataUtf8;
    };

    Paths BuildPaths(const std::wstring& root, const std::wstring& userDir)
    {
        Paths x;
        x.root = root;
        x.data = root + L"\\Data";
        x.dataUtf8 = Narrow(x.data);
        // Our own state lives inside Data so staged files are on the same
        // volume as their destination (the install is a rename).
        x.state = x.data + L"\\CoSyncContent";
        x.staging = x.state + L"\\staging";
        x.backup = x.state + L"\\backup";
        x.journal = x.state + L"\\install.journal";
        x.pending = x.state + L"\\pending.txt";
        x.rejoin = x.state + L"\\rejoin.txt";
        x.cache = x.state + L"\\hashcache.txt";
        x.include = x.state + L"\\include.txt";
        x.ccc = root + L"\\Fallout4.ccc";
        if (!userDir.empty())
        {
            x.pluginsTxt = userDir + L"\\Plugins.txt";
            x.catalog = userDir + L"\\ContentCatalog.txt";
        }
        return x;
    }

#ifndef COSYNC_CONTENT_OFFLINE_TEST
    const Paths& P()
    {
        static const Paths p = []
        {
            wchar_t exe[MAX_PATH] = {};
            HMODULE mod = GetModuleHandleW(L"Fallout4.exe");
            GetModuleFileNameW(mod ? mod : GetModuleHandleW(nullptr), exe, MAX_PATH);
            std::wstring root = exe;
            const size_t slash = root.find_last_of(L"\\/");
            root = slash == std::wstring::npos ? L"." : root.substr(0, slash);
            // The engine reads "%sPlugins.txt" / "%sContentCatalog.txt" from its
            // user folder (sub_14024D760 / sub_1411B7330), which is
            // %LOCALAPPDATA%\Fallout4\ on this runtime.
            std::wstring userDir;
            PWSTR la = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la)) && la)
            {
                userDir = std::wstring(la) + L"\\Fallout4";
                CoTaskMemFree(la);
            }
            return BuildPaths(root, userDir);
        }();
        return p;
    }
    // The host's files (same install as P() in the game).
    const Paths& PH() { return P(); }
#else
}
// Offline test build (Tools/ContentSyncTests): host and client run in one
// process against two separate game folders.
std::mutex   g_contentTestMx;
std::wstring g_contentTestHostRoot, g_contentTestClientRoot;
uint64_t     g_contentTestFreeBytes = 0;   // 0 = ask Windows
namespace
{
    Paths P()
    {
        std::lock_guard<std::mutex> lk(g_contentTestMx);
        return BuildPaths(g_contentTestClientRoot, g_contentTestClientRoot + L"\\UserDir");
    }
    Paths PH()
    {
        std::lock_guard<std::mutex> lk(g_contentTestMx);
        return BuildPaths(g_contentTestHostRoot, g_contentTestHostRoot + L"\\UserDir");
    }
#endif

    std::string JsonEscape(const std::string& s)
    {
        std::string out;
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
                else out += static_cast<char>(c);
            }
        }
        return out;
    }

    std::vector<std::string> Split(const std::string& s, char sep)
    {
        std::vector<std::string> out;
        size_t pos = 0;
        for (;;)
        {
            const size_t bar = s.find(sep, pos);
            if (bar == std::string::npos) { out.push_back(s.substr(pos)); break; }
            out.push_back(s.substr(pos, bar - pos));
            pos = bar + 1;
        }
        return out;
    }

    bool ParseU64(const std::string& s, uint64_t& v)
    {
        if (s.empty() || s.size() > 20) return false;
        v = 0;
        for (char c : s)
        {
            if (c < '0' || c > '9') return false;
            const uint64_t d = static_cast<uint64_t>(c - '0');
            if (v > (UINT64_MAX - d) / 10) return false;
            v = v * 10 + d;
        }
        return true;
    }

    std::string RandomHex(size_t bytes)
    {
        std::vector<uint8_t> b(bytes);
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(b.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        {
            LARGE_INTEGER q; QueryPerformanceCounter(&q);
            for (size_t i = 0; i < bytes; ++i) b[i] = static_cast<uint8_t>((q.QuadPart >> ((i % 8) * 8)) ^ (i * 131));
        }
        static const char kHex[] = "0123456789abcdef";
        std::string out;
        for (uint8_t x : b) { out += kHex[x >> 4]; out += kHex[x & 15]; }
        return out;
    }

    // ── One worker thread for all disk work ─────────────────────────────────
    class Worker
    {
    public:
        void Post(std::function<void()> fn)
        {
            std::lock_guard<std::mutex> lk(m_mx);
            if (!m_thread)
                m_thread = new std::thread([this] { Loop(); });   // never joined: lives with the process
            m_q.push_back(std::move(fn));
            m_cv.notify_one();
        }
    private:
        void Loop()
        {
            for (;;)
            {
                std::function<void()> fn;
                {
                    std::unique_lock<std::mutex> lk(m_mx);
                    m_cv.wait(lk, [this] { return !m_q.empty(); });
                    fn = std::move(m_q.front());
                    m_q.pop_front();
                }
                try { fn(); }
                catch (...) { LOG_ERROR("[ContentSync] worker job threw -- ignored"); }
            }
        }
        std::mutex m_mx;
        std::condition_variable m_cv;
        std::deque<std::function<void()>> m_q;
        std::thread* m_thread = nullptr;
    };

    Worker& Disk()
    {
        static Worker* w = new Worker();
        return *w;
    }

    // Results from the worker, run by Tick on the main thread.
    std::mutex s_mainMx;
    std::deque<std::function<void()>> s_mainQ;

    void PostMain(std::function<void()> fn)
    {
        std::lock_guard<std::mutex> lk(s_mainMx);
        s_mainQ.push_back(std::move(fn));
    }

    // ── Sending ─────────────────────────────────────────────────────────────
    // Straight to GNS: content messages must never go through the FormID
    // translation CoSyncTransport applies to a client's session traffic.
    void ClientSend(const std::string& m)
    {
        if (CoSyncTransport::IsInitialized())
            GNS_Session::Get().SendText(m);
    }

    void HostSendTo(uint32_t conn, const std::string& m)
    {
        if (CoSyncTransport::IsInitialized())
            GNS_Session::Get().SendTextTo(static_cast<HSteamNetConnection>(conn), m);
    }

    void SendBig(bool fromHost, uint32_t conn, const char* type, const std::string& text)
    {
        size_t count = (text.size() + kBigPartBytes - 1) / kBigPartBytes;
        if (count == 0) count = 1;
        for (size_t i = 0; i < count; ++i)
        {
            std::string part = std::string("CSBIG|") + type + "|" + std::to_string(i) + "|" + std::to_string(count) + "|";
            if (i * kBigPartBytes < text.size())
                part.append(text, i * kBigPartBytes, kBigPartBytes);
            if (fromHost) HostSendTo(conn, part);
            else ClientSend(part);
        }
    }

    // Reassembly of CSBIG parts. Returns 1 = complete (type/out set),
    // 0 = more to come, -1 = malformed (state reset).
    struct BigRx
    {
        std::string type;
        size_t next = 0, count = 0;
        std::string buf;
    };

    int BigFeed(BigRx& rx, const std::string& msg, std::string& type, std::string& out)
    {
        // CSBIG|<type>|<seq>|<count>|<data>
        size_t pos = 6;
        std::string f[3];
        for (int i = 0; i < 3; ++i)
        {
            const size_t bar = msg.find('|', pos);
            if (bar == std::string::npos) { rx = BigRx(); return -1; }
            f[i] = msg.substr(pos, bar - pos);
            pos = bar + 1;
        }
        uint64_t seq = 0, count = 0;
        if ((f[0] != "MAN" && f[0] != "REP" && f[0] != "VER") || !ParseU64(f[1], seq) || !ParseU64(f[2], count) ||
            count == 0 || count > kBigMaxParts || seq >= count || msg.size() - pos > kBigPartBytes)
        { rx = BigRx(); return -1; }
        if (seq == 0)
        {
            rx = BigRx();
            rx.type = f[0];
            rx.count = static_cast<size_t>(count);
        }
        else if (rx.type != f[0] || rx.count != count || rx.next != seq)
        { rx = BigRx(); return -1; }
        rx.buf.append(msg, pos, std::string::npos);
        rx.next = static_cast<size_t>(seq) + 1;
        if (rx.next < rx.count)
            return 0;
        type = rx.type;
        out.swap(rx.buf);
        rx = BigRx();
        return 1;
    }

    // ── Game-thread snapshot of what the engine loaded ─────────────────────
    struct LoadedPlugin { std::string name; bool light; };

#ifdef COSYNC_CONTENT_OFFLINE_TEST
}
std::vector<std::pair<std::string, bool>> g_contentTestHostLoaded, g_contentTestClientLoaded;
namespace
{
    std::vector<LoadedPlugin> SnapshotLoadedPlugins(bool forHost)
    {
        std::vector<LoadedPlugin> out;
        for (const auto& p : forHost ? g_contentTestHostLoaded : g_contentTestClientLoaded)
            out.push_back({ p.first, p.second });
        return out;
    }
#else
    std::vector<LoadedPlugin> SnapshotLoadedPlugins(bool /*forHost*/)
    {
        std::vector<LoadedPlugin> out;
        DataHandler* dh = *g_dataHandler;
        if (!dh) return out;
        for (UInt32 i = 0; i < dh->modList.loadedMods.count; ++i)
        {
            ModInfo* m = nullptr;
            if (dh->modList.loadedMods.GetNthItem(i, m) && m)
                out.push_back({ m->name, false });
        }
        for (UInt32 i = 0; i < dh->modList.lightMods.count; ++i)
        {
            ModInfo* m = nullptr;
            if (dh->modList.lightMods.GetNthItem(i, m) && m)
                out.push_back({ m->name, true });
        }
        return out;
    }
#endif

    std::string LoadOrderText(const std::vector<LoadedPlugin>& list)
    {
        std::string out;
        for (const LoadedPlugin& p : list)
        {
            if (!out.empty()) out += "*";   // '*' cannot appear in a file name
            out += (p.light ? "L:" : "F:") + Lower(p.name);
        }
        return out;
    }

    struct OfficialLists
    {
        std::vector<std::string> ccc;
        std::string catalog;
    };

    OfficialLists LoadOfficialLists(const Paths& p)   // worker
    {
        OfficialLists o;
        std::string text;
        if (ReadWholeFile(p.ccc, text, 1u << 20))
            o.ccc = ParseNameList(text);
        if (!p.catalog.empty())
            ReadWholeFile(p.catalog, o.catalog, 8u << 20);
        return o;
    }

    const char* OriginName(Origin o)
    {
        switch (o)
        {
        case Origin::BaseGame:     return "base game / DLC";
        case Origin::CreationClub: return "Creation Club";
        case Origin::Creations:    return "Bethesda.net Creation";
        default:                   return "mod";
        }
    }

    // =======================================================================
    //  HOST
    // =======================================================================

    struct HostManifest
    {
        bool ready = false;
        bool building = false;
        Manifest m;
        std::string text;                     // serialized manifest
        std::vector<std::wstring> full;       // host path per entry
        std::vector<uint64_t> mtime;          // last-write time when hashed
        std::vector<std::string> unreadable;  // loaded plugins the host could not read
    };
    HostManifest s_hm;
    std::atomic<int> s_hostHashed{ 0 }, s_hostToHash{ 0 };

    enum class HostSt { WaitManifest, WaitReport, Proving, Transferring, Verified, Blocked };

    struct HostConn
    {
        HostSt st = HostSt::WaitManifest;
        std::string nonce;
        int requests = 0;              // also the epoch of the current check
        BigRx rx;
        std::unordered_set<uint32_t> allowed;
        uint64_t allowedBytes = 0, served = 0;
        int queued = 0;
        int protocolErrors = 0;
        double blockedAt = 0.0;
        double lastWait = 0.0;
    };
    std::unordered_map<uint32_t, HostConn> s_conns;
    std::atomic<uint64_t> s_hostGen{ 1 };

    struct OutPkt { uint32_t conn; uint64_t gen; std::string data; };
    std::mutex s_outMx;
    std::deque<OutPkt> s_outbox;

    // Worker: builds the manifest from what the engine loaded.
    std::shared_ptr<HostManifest> BuildHostManifest(const std::vector<LoadedPlugin>& loaded)
    {
        auto hm = std::make_shared<HostManifest>();
        hm->m.build = CoSyncNet::GetBuildNumber();
        const Paths& p = PH();
        const OfficialLists off = LoadOfficialLists(p);
        CreateDirectories(p.state);
        HashCache cache;
        cache.Load(p.cache);

        // What to hash: plugins, companions, loose files.
        struct Want { Entry e; std::wstring full; };
        std::vector<Want> wants;
        std::unordered_set<std::string> seen;
        auto add = [&](Entry e) -> bool
        {
            std::string why;
            if (!ValidateEntryPath(e, why))
            {
                LOG_WARN("[ContentSync] Host: '%s' left out of the session content (%s)", e.relPath.c_str(), why.c_str());
                return false;
            }
            if (!seen.insert(Lower(e.relPath)).second)
                return false;
            std::wstring full;
            if (!ResolveInside(p.data, e.relPath, full, why))
            {
                LOG_WARN("[ContentSync] Host: '%s' left out (%s)", e.relPath.c_str(), why.c_str());
                return false;
            }
            wants.push_back({ std::move(e), std::move(full) });
            return true;
        };

        std::vector<std::string> modPlugins;
        int slot = 0;
        for (const LoadedPlugin& lp : loaded)
        {
            Entry e;
            e.kind = Kind::Plugin;
            e.relPath = lp.name;
            e.slot = slot++;
            e.light = lp.light;
            e.origin = ClassifyPlugin(lp.name, off.ccc, off.catalog);
            if (!add(e))
            {
                hm->unreadable.push_back(lp.name);
                continue;
            }
            if (e.origin != Origin::Mod)
                continue;
            modPlugins.push_back(lp.name);
            // Companion files the engine registers for this plugin.
            for (const std::string& c : CompanionNamesFor(lp.name))
            {
                if (GetFileAttributesW((p.data + L"\\" + Widen(c)).c_str()) == INVALID_FILE_ATTRIBUTES)
                    continue;
                Entry ce; ce.kind = Kind::Companion; ce.origin = Origin::Mod; ce.relPath = c; ce.owner = lp.name;
                add(ce);
            }
            WIN32_FIND_DATAW fd;
            const std::wstring pattern = p.data + L"\\" + Widen(FileBase(lp.name)) + L" - Voices_*.ba2";
            HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    Entry ce; ce.kind = Kind::Companion; ce.origin = Origin::Mod; ce.relPath = Narrow(fd.cFileName); ce.owner = lp.name;
                    add(ce);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }

        // Loose files: only where a mod manager says they were deployed by the
        // same mod as one of these plugins (Vortex). Everything else stays
        // local to each player (texture replacers, UI mods, ...).
        std::string json, why;
        std::vector<std::pair<std::string, std::string>> deployed;
        if (ReadWholeFile(p.data + L"\\vortex.deployment.json", json, 64u << 20) &&
            ParseVortexDeployment(json, p.dataUtf8, deployed, why))
        {
            std::unordered_map<std::string, std::string> sourceOf;   // lower relPath -> source
            for (const auto& f : deployed)
                sourceOf[Lower(f.first)] = f.second;
            std::unordered_map<std::string, std::string> ownerOfSource;   // source -> plugin
            for (const std::string& mp : modPlugins)
            {
                auto it = sourceOf.find(Lower(mp));
                if (it != sourceOf.end() && !ownerOfSource.count(it->second))
                    ownerOfSource[it->second] = mp;
            }
            size_t looseCount = 0;
            for (const auto& f : deployed)
            {
                auto own = ownerOfSource.find(f.second);
                if (own == ownerOfSource.end() || f.first.find('\\') == std::string::npos)
                    continue;
                std::string w;
                if (!IsAllowedLoosePath(f.first, w))
                {
                    if (Lower(f.first).rfind("f4se\\plugins\\", 0) == 0 && FileExtLower(f.first) == ".dll")
                        LOG_INFO("[ContentSync] Host: '%s' (with %s) is native code -- listed for players, never sent",
                            f.first.c_str(), own->second.c_str());
                    continue;
                }
                Entry le; le.kind = Kind::Loose; le.origin = Origin::Mod; le.relPath = f.first; le.owner = own->second;
                if (add(le)) ++looseCount;
            }
            LOG_INFO("[ContentSync] Host: Vortex deployment -> %zu loose file(s) belong to this session's plugins", looseCount);
        }
        else
        {
            LOG_INFO("[ContentSync] Host: no usable Vortex deployment file (%s) -- loose files are synced only from CoSyncContent\\include.txt",
                why.empty() ? "not found" : why.c_str());
        }

        // Loose files the host lists by hand (one path per line, relative to Data).
        std::string inc;
        if (ReadWholeFile(p.include, inc, 1u << 20))
            for (const std::string& rel : ParseNameList(inc))
            {
                Entry le; le.kind = Kind::Loose; le.origin = Origin::Mod; le.relPath = rel;
                for (char& c : le.relPath) if (c == '/') c = '\\';
                add(le);
            }

        if (wants.size() > kMaxEntries)
        {
            LOG_WARN("[ContentSync] Host: %zu files is over the %zu limit -- the last loose files are left out", wants.size(), kMaxEntries);
            wants.resize(kMaxEntries);
        }

        s_hostToHash.store(static_cast<int>(wants.size()));
        s_hostHashed.store(0);
        for (Want& w : wants)
        {
            uint64_t size = 0, mtime = 0;
            std::string sha;
            if (!GetFileSizeTime(w.full, size, mtime) || !HashFileCached(cache, w.full, w.e.relPath, sha, size))
            {
                LOG_ERROR("[ContentSync] Host: cannot read '%s' -- left out", w.e.relPath.c_str());
                if (w.e.kind == Kind::Plugin) hm->unreadable.push_back(w.e.relPath);
                s_hostHashed.fetch_add(1);
                continue;
            }
            if (size > kMaxFileBytes)
            {
                LOG_WARN("[ContentSync] Host: '%s' is %s, over the %s per-file limit -- left out",
                    w.e.relPath.c_str(), FormatBytes(size).c_str(), FormatBytes(kMaxFileBytes).c_str());
                if (w.e.kind == Kind::Plugin) hm->unreadable.push_back(w.e.relPath);
                s_hostHashed.fetch_add(1);
                continue;
            }
            w.e.size = size;
            w.e.sha256 = sha;
            if (w.e.kind == Kind::Plugin && w.e.origin == Origin::Mod)
            {
                const PluginHeader ph = ReadPluginHeader(w.full);
                if (ph.ok)
                    for (const std::string& mname : ph.masters)
                        if (IsPluginFileName(mname)) w.e.masters.push_back(mname);
            }
            hm->m.entries.push_back(w.e);
            hm->full.push_back(w.full);
            hm->mtime.push_back(mtime);
            s_hostHashed.fetch_add(1);
        }
        cache.Save(p.cache);

        // A companion or loose file whose plugin was left out (unreadable, too
        // large) goes too: the manifest may only name owners it contains.
        {
            std::unordered_set<std::string> kept;
            for (const Entry& e : hm->m.entries)
                if (e.kind == Kind::Plugin) kept.insert(Lower(e.relPath));
            std::vector<Entry> entries;
            std::vector<std::wstring> full;
            std::vector<uint64_t> mtimes;
            for (size_t i = 0; i < hm->m.entries.size(); ++i)
            {
                const Entry& e = hm->m.entries[i];
                if (e.kind != Kind::Plugin && !e.owner.empty() && !kept.count(Lower(e.owner)))
                {
                    LOG_WARN("[ContentSync] Host: '%s' left out with its plugin %s", e.relPath.c_str(), e.owner.c_str());
                    continue;
                }
                entries.push_back(e);
                full.push_back(hm->full[i]);
                mtimes.push_back(hm->mtime[i]);
            }
            hm->m.entries.swap(entries);
            hm->full.swap(full);
            hm->mtime.swap(mtimes);
        }

        // Native plugins: listed so players know what else the host runs.
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((p.data + L"\\F4SE\\Plugins\\*.dll").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const std::string n = Narrow(fd.cFileName);
                std::string w;
                if (hm->m.nativePlugins.size() < 256 && IsSafeRelativePath(n, w))
                    hm->m.nativePlugins.push_back(n);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }

        hm->text = SerializeManifest(hm->m);
        uint64_t modBytes = 0;
        size_t mods = 0;
        for (const Entry& e : hm->m.entries)
            if (e.Transferable()) { modBytes += e.size; ++mods; }
        LOG_INFO("[ContentSync] Host manifest: %zu file(s) (%zu transferable, %s), %zu native plugin(s) listed, %zu unreadable",
            hm->m.entries.size(), mods, FormatBytes(modBytes).c_str(), hm->m.nativePlugins.size(), hm->unreadable.size());
        for (const Entry& e : hm->m.entries)
            LOG_INFO("[ContentSync]   %c %-40s %10s  %s  (%s%s%s)", static_cast<char>(e.kind), e.relPath.c_str(),
                FormatBytes(e.size).c_str(), e.sha256.substr(0, 12).c_str(), OriginName(e.origin),
                e.owner.empty() ? "" : ", with ", e.owner.c_str());
        hm->ready = true;
        return hm;
    }

    void HostSendManifest(uint32_t conn, HostConn& hc)
    {
        SendBig(true, conn, "MAN", "NONCE|" + hc.nonce + "\n" + s_hm.text);
        hc.st = HostSt::WaitReport;
        LOG_INFO("[ContentSync] Host -> conn=%u: manifest (%zu files, %zu bytes)", conn, s_hm.m.entries.size(), s_hm.text.size());
    }

    void HostStartBuild()
    {
        if (s_hm.building || s_hm.ready)
            return;
        s_hm.building = true;
        const std::vector<LoadedPlugin> loaded = SnapshotLoadedPlugins(true);   // game thread
        LOG_INFO("[ContentSync] Host: building the session content list from %zu loaded plugin(s)", loaded.size());
        const uint64_t gen = s_hostGen.load();
        Disk().Post([loaded, gen]
        {
            std::shared_ptr<HostManifest> hm = BuildHostManifest(loaded);
            PostMain([hm, gen]
            {
                if (gen != s_hostGen.load())
                    return;   // hosting stopped meanwhile; the next session builds again
                s_hm = *hm;
                s_hm.building = false;
                for (auto& kv : s_conns)
                    if (kv.second.st == HostSt::WaitManifest)
                        HostSendManifest(kv.first, kv.second);
            });
        });
    }

    void HostBlock(uint32_t conn, HostConn& hc, const char* why)
    {
        hc.st = HostSt::Blocked;
        hc.blockedAt = NowMs();
        hc.allowed.clear();
        LOG_WARN("[ContentSync] Host: conn=%u refused (%s)", conn, why);
    }

    void HostFail(uint32_t conn, HostConn& hc, const std::string& reason)
    {
        HostSendTo(conn, "CSFAIL|" + reason);
        HostBlock(conn, hc, reason.c_str());
    }

    struct Claim { char state = 'X'; std::string proof; };

    void HostDecide(uint32_t conn, int epoch, const std::vector<Claim>& claims, const std::vector<std::string>& expected,
                    const std::vector<std::string>& clientLoaded)
    {
        auto it = s_conns.find(conn);
        if (it == s_conns.end() || it->second.st != HostSt::Proving || it->second.requests != epoch)
            return;
        HostConn& hc = it->second;
        const Manifest& m = s_hm.m;

        std::unordered_set<std::string> loaded;
        for (const std::string& n : clientLoaded) loaded.insert(n);

        std::string verdict;
        std::vector<uint32_t> need;
        std::vector<std::string> activate;
        std::vector<std::string> blocks, warns;
        uint64_t needBytes = 0;
        for (uint32_t i = 0; i < m.entries.size(); ++i)
        {
            const Entry& e = m.entries[i];
            bool ok = claims[i].state == 'M' && !expected[i].empty() && claims[i].proof == expected[i];
            if (claims[i].state == 'M' && !ok)
                LOG_WARN("[ContentSync] Host: conn=%u claims '%s' matches but the proof is %s -- treated as different",
                    conn, e.relPath.c_str(), expected[i].empty() ? "unverifiable" : "wrong");
            if (!ok)
            {
                if (!e.Transferable())
                {
                    const char* code = e.origin == Origin::BaseGame ? "official"
                                     : e.origin == Origin::CreationClub ? "creationclub" : "creations";
                    blocks.push_back(std::string(code) + (claims[i].state == 'X' ? "_missing" : "_different") + "|" + e.relPath);
                }
                else
                {
                    need.push_back(i);
                    needBytes += e.size;
                }
            }
            if (e.kind == Kind::Plugin && !loaded.count(Lower(e.relPath)))
            {
                if (!e.Transferable())
                {
                    if (ok) blocks.push_back("official_not_loaded|" + e.relPath);
                }
                else
                    activate.push_back(e.relPath);
            }
        }
        for (const std::string& u : s_hm.unreadable)
            if (!loaded.count(Lower(u)))
                blocks.push_back("host_unreadable|" + u);
        std::unordered_set<std::string> hostPlugins;
        std::vector<std::string> hostOrder;
        for (const Entry& e : m.entries)
            if (e.kind == Kind::Plugin) { hostPlugins.insert(Lower(e.relPath)); hostOrder.push_back(Lower(e.relPath)); }
        for (const std::string& u : s_hm.unreadable) hostPlugins.insert(Lower(u));
        for (const std::string& n : clientLoaded)
            if (!hostPlugins.count(n))
                warns.push_back("extra|" + n);
        {
            // Relative order of the plugins both load (CoSyncFormIdMap
            // translates it, but the player should know).
            std::vector<std::string> common;
            for (const std::string& n : clientLoaded) if (hostPlugins.count(n)) common.push_back(n);
            std::vector<std::string> hostCommon;
            for (const std::string& n : hostOrder) if (loaded.count(n)) hostCommon.push_back(n);
            if (common != hostCommon)
                warns.push_back("order|");
        }
        if (needBytes > kMaxTotalBytes)
            blocks.push_back("too_large|" + FormatBytes(needBytes));

        const char* status = !blocks.empty() ? "BLOCK" : !need.empty() ? "NEED" : !activate.empty() ? "ACTIVATE" : "OK";
        verdict = std::string("S|") + status + "\n";
        if (blocks.empty())
        {
            for (uint32_t i : need) verdict += "T|" + std::to_string(i) + "\n";
            for (const std::string& a : activate) verdict += "A|" + a + "\n";
        }
        for (const std::string& b : blocks) verdict += "B|" + b + "\n";
        size_t w = 0;
        for (const std::string& x : warns) { if (++w > 200) break; verdict += "W|" + x + "\n"; }
        verdict += "E\n";
        SendBig(true, conn, "VER", verdict);

        LOG_INFO("[ContentSync] Host verdict for conn=%u: %s (transfer %zu file(s) %s, activate %zu, blocked %zu, warnings %zu)",
            conn, status, need.size(), FormatBytes(needBytes).c_str(), activate.size(), blocks.size(), warns.size());
        for (const std::string& b : blocks)
            LOG_WARN("[ContentSync]   blocked: %s", b.c_str());

        if (std::strcmp(status, "OK") == 0)
        {
            hc.st = HostSt::Verified;
            LOG_INFO("[ContentSync] Host: conn=%u VERIFIED -- may request the world", conn);
        }
        else if (std::strcmp(status, "NEED") == 0)
        {
            hc.st = HostSt::Transferring;
            hc.allowed.clear();
            for (uint32_t i : need) hc.allowed.insert(i);
            // Retries and a restart from zero after a failed hash are allowed for.
            hc.allowedBytes = needBytes * 3 + (64ull << 20);
            hc.served = 0;
        }
        else if (std::strcmp(status, "ACTIVATE") == 0)
        {
            hc.st = HostSt::Blocked;   // nothing to serve; the client restarts
            hc.blockedAt = NowMs();
        }
        else
            HostBlock(conn, hc, "content mismatch that cannot be transferred");
    }

    void HostOnReport(uint32_t conn, HostConn& hc, const std::string& text)
    {
        const Manifest& m = s_hm.m;
        std::vector<Claim> claims(m.entries.size());
        std::vector<std::string> clientLoaded;
        bool ended = false, sawLo = false;
        std::vector<std::string> lines = Split(text, '\n');
        if (!lines.empty() && lines.back().empty()) lines.pop_back();
        for (const std::string& line : lines)
        {
            if (ended) { HostFail(conn, hc, "protocol"); return; }
            if (line == "E") { ended = true; continue; }
            if (line.rfind("LO|", 0) == 0 && !sawLo)
            {
                sawLo = true;
                for (const std::string& tok : Split(line.substr(3), '*'))
                    if (tok.size() > 2 && tok[1] == ':' && clientLoaded.size() < 4096)
                        clientLoaded.push_back(Lower(tok.substr(2)));
                continue;
            }
            const std::vector<std::string> f = Split(line, '|');
            uint64_t idx = 0;
            if (f.size() != 4 || f[0] != "R" || !ParseU64(f[1], idx) || idx >= m.entries.size() ||
                f[2].size() != 1 || std::strchr("MXD", f[2][0]) == nullptr ||
                (f[2] == "M" ? !IsSha256Hex(f[3]) : f[3] != "-"))
            { HostFail(conn, hc, "protocol"); return; }
            claims[idx].state = f[2][0];
            claims[idx].proof = f[3];
        }
        if (!ended || !sawLo) { HostFail(conn, hc, "protocol"); return; }

        // Prove every "I have it" claim against the host's own copy.
        struct ProofJob { uint32_t idx; std::wstring full; uint64_t size; };
        std::vector<ProofJob> jobs;
        for (uint32_t i = 0; i < claims.size(); ++i)
            if (claims[i].state == 'M')
                jobs.push_back({ i, s_hm.full[i], m.entries[i].size });
        hc.st = HostSt::Proving;
        const std::string nonce = hc.nonce;
        const size_t n = m.entries.size();
        const int epoch = hc.requests;
        LOG_INFO("[ContentSync] Host: conn=%u report received (%zu claimed present) -- verifying", conn, jobs.size());
        Disk().Post([conn, epoch, jobs, nonce, n, claims, clientLoaded]
        {
            std::vector<std::string> expected(n);
            for (const ProofJob& j : jobs)
                if (!ComputeProof(j.full, j.size, nonce, expected[j.idx]))
                    expected[j.idx].clear();
            PostMain([conn, epoch, claims, expected, clientLoaded] { HostDecide(conn, epoch, claims, expected, clientLoaded); });
        });
    }

    void HostOnGet(uint32_t conn, HostConn& hc, const std::string& msg)
    {
        // CSGET|<idx>|<offset>|<length>
        const std::vector<std::string> f = Split(msg, '|');
        uint64_t idx = 0, off = 0, len = 0;
        if (hc.st != HostSt::Transferring || f.size() != 4 || !ParseU64(f[1], idx) || !ParseU64(f[2], off) ||
            !ParseU64(f[3], len) || idx >= s_hm.m.entries.size() || !hc.allowed.count(static_cast<uint32_t>(idx)) ||
            len == 0 || len > kChunkBytes)
        {
            if (++hc.protocolErrors > 20) HostFail(conn, hc, "protocol");
            return;
        }
        const Entry& e = s_hm.m.entries[idx];
        if (off >= e.size || len > e.size - off)
        {
            if (++hc.protocolErrors > 20) HostFail(conn, hc, "protocol");
            return;
        }
        if (hc.queued >= kHostMaxQueued || hc.served + len > hc.allowedBytes)
        {
            HostFail(conn, hc, "flood");
            return;
        }
        ++hc.queued;
        hc.served += len;
        const std::wstring full = s_hm.full[idx];
        const uint64_t expSize = e.size, expTime = s_hm.mtime[idx];
        const uint64_t gen = s_hostGen.load();
        const uint32_t i32 = static_cast<uint32_t>(idx);
        Disk().Post([conn, gen, i32, off, len, full, expSize, expTime]
        {
            std::string pkt;
            uint64_t sz = 0, mt = 0;
            bool ok = GetFileSizeTime(full, sz, mt) && sz == expSize && mt == expTime;
            if (!ok)
                pkt = "CSERR|" + std::to_string(i32) + "|changed";
            else
            {
                std::string data(static_cast<size_t>(len), '\0');
                HANDLE h = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                DWORD got = 0;
                LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(off);
                ok = h != INVALID_HANDLE_VALUE && SetFilePointerEx(h, li, nullptr, FILE_BEGIN) &&
                     ::ReadFile(h, &data[0], static_cast<DWORD>(len), &got, nullptr) && got == len;
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
                if (!ok)
                    pkt = "CSERR|" + std::to_string(i32) + "|read";
                else
                {
                    char hdr[96];
                    const int hn = std::snprintf(hdr, sizeof(hdr), "CSDATA|%u|%llu|%08x|", i32,
                        static_cast<unsigned long long>(off), Crc32(data.data(), data.size()));
                    pkt.reserve(static_cast<size_t>(hn) + data.size());
                    pkt.assign(hdr, static_cast<size_t>(hn));
                    pkt += data;
                }
            }
            std::lock_guard<std::mutex> lk(s_outMx);
            s_outbox.push_back({ conn, gen, std::move(pkt) });
        });
    }

    void HostOnReceive(const std::string& msg, uint32_t conn)
    {
        if (conn == 0)
            return;
        HostConn& hc = s_conns[conn];

        if (msg.rfind("CSREQ|", 0) == 0)
        {
            const std::vector<std::string> f = Split(msg, '|');
            uint64_t build = 0, proto = 0;
            if (f.size() != 3 || !ParseU64(f[1], build) || !ParseU64(f[2], proto))
            { HostFail(conn, hc, "protocol"); return; }
            if (build != CoSyncNet::GetBuildNumber())
            {
                LOG_WARN("[ContentSync] Host: conn=%u runs build %llu, host %u", conn, static_cast<unsigned long long>(build), CoSyncNet::GetBuildNumber());
                HostFail(conn, hc, "version_mismatch");
                return;
            }
            if (proto != static_cast<uint64_t>(kProtocolVersion)) { HostFail(conn, hc, "protocol_version"); return; }
            if (++hc.requests > kMaxRequests) { HostFail(conn, hc, "too_many_requests"); return; }
            // 0.5.1 lobby: the content check no longer needs the host to be in
            // a game. The plugins and archives are loaded at the main menu;
            // the world itself follows once the host has one
            // (CoSyncWorldTransfer lobby).
            hc.nonce = RandomHex(16);
            hc.allowed.clear();
            hc.queued = 0;
            hc.rx = BigRx();
            LOG_INFO("[ContentSync] Host: conn=%u asked for the session content", conn);
            if (s_hm.ready)
                HostSendManifest(conn, hc);
            else
            {
                hc.st = HostSt::WaitManifest;
                hc.lastWait = NowMs();
                HostSendTo(conn, "CSWAIT|" + std::to_string(s_hostHashed.load()) + "|" + std::to_string(s_hostToHash.load()));
                HostStartBuild();
            }
            return;
        }
        if (msg.rfind("CSBIG|", 0) == 0)
        {
            if (hc.st != HostSt::WaitReport) { ++hc.protocolErrors; return; }
            std::string type, text;
            const int r = BigFeed(hc.rx, msg, type, text);
            if (r < 0 || (r == 1 && type != "REP")) { HostFail(conn, hc, "protocol"); return; }
            if (r == 1) HostOnReport(conn, hc, text);
            return;
        }
        if (msg.rfind("CSGET|", 0) == 0)
        {
            HostOnGet(conn, hc, msg);
            return;
        }
        if (msg.rfind("CSCANCEL|", 0) == 0)
        {
            LOG_INFO("[ContentSync] Host: conn=%u cancelled (%.40s)", conn, msg.c_str() + 9);
            HostBlock(conn, hc, "client cancelled");
            return;
        }
        if (msg.rfind("CSDONE|", 0) == 0)
        {
            LOG_INFO("[ContentSync] Host: conn=%u finished (%.40s) -- it restarts and joins again", conn, msg.c_str() + 7);
            HostBlock(conn, hc, "client restarting");
            return;
        }
        ++hc.protocolErrors;   // a host-to-client type from a client
    }

    void HostTick()
    {
        // Chunks the worker read: send within a per-frame budget.
        size_t budget = kHostSendBudget;
        for (;;)
        {
            OutPkt pkt;
            {
                std::lock_guard<std::mutex> lk(s_outMx);
                if (s_outbox.empty() || budget == 0) break;
                pkt = std::move(s_outbox.front());
                s_outbox.pop_front();
            }
            if (pkt.gen != s_hostGen.load()) continue;
            auto it = s_conns.find(pkt.conn);
            if (it == s_conns.end() || it->second.st != HostSt::Transferring) continue;
            if (it->second.queued > 0) --it->second.queued;
            budget = pkt.data.size() >= budget ? 0 : budget - pkt.data.size();
            HostSendTo(pkt.conn, pkt.data);
        }

        static double s_lastSweep = 0.0;
        const double now = NowMs();
        if (now - s_lastSweep < 1000.0)
            return;
        s_lastSweep = now;
        std::vector<uint32_t> gone, kick;
        for (auto& kv : s_conns)
        {
            if (!GNS_Session::Get().IsClientConnected(static_cast<HSteamNetConnection>(kv.first)))
                gone.push_back(kv.first);
            else if (kv.second.st == HostSt::Blocked && now - kv.second.blockedAt > kBlockedKickMs)
                kick.push_back(kv.first);
            else if (kv.second.st == HostSt::WaitManifest && now - kv.second.lastWait > 3000.0)
            {
                kv.second.lastWait = now;
                HostSendTo(kv.first, "CSWAIT|" + std::to_string(s_hostHashed.load()) + "|" + std::to_string(s_hostToHash.load()));
            }
        }
        for (uint32_t c : gone)
            s_conns.erase(c);
        for (uint32_t c : kick)
        {
            s_conns.erase(c);
            GNS_Session::Get().KickClient(static_cast<HSteamNetConnection>(c), CoSyncSessionConfig::kEndReason_ContentSync, "content_sync");
        }
    }

    // =======================================================================
    //  CLIENT
    // =======================================================================

    enum class CPhase { Idle, Requesting, Preparing, Checking, Waiting, Confirm, Downloading, Installing, Restart, Ready, Blocked, Failed, Cancelled };

    const char* PhaseName(CPhase p)
    {
        switch (p)
        {
        case CPhase::Requesting:  return "requesting";
        case CPhase::Preparing:   return "preparing";
        case CPhase::Checking:    return "checking";
        case CPhase::Waiting:     return "waiting";
        case CPhase::Confirm:     return "confirm";
        case CPhase::Downloading: return "downloading";
        case CPhase::Installing:  return "installing";
        case CPhase::Restart:     return "restart";
        case CPhase::Ready:       return "ready";
        case CPhase::Blocked:     return "blocked";
        case CPhase::Failed:      return "failed";
        case CPhase::Cancelled:   return "cancelled";
        default:                  return "idle";
        }
    }

    bool IsActivePhase(CPhase p)
    {
        return p == CPhase::Requesting || p == CPhase::Preparing || p == CPhase::Checking || p == CPhase::Waiting ||
               p == CPhase::Confirm || p == CPhase::Downloading;
    }

    struct LocalState
    {
        bool exists = false, match = false, officialHere = false;
        std::wstring full;
    };

    struct Outstanding { uint64_t len; double sentAt; int tries; };

    // A downloaded file, verified, waiting in staging for the next start.
    struct StagedFile { uint32_t idx; std::wstring path; uint64_t size; uint64_t mtime; };

    struct CurFile
    {
        bool active = false, opening = false, finalizing = false;
        uint32_t idx = 0;
        uint64_t size = 0, nextReq = 0, done = 0, metaAt = 0;
        int attempts = 0;
        std::string sha;
        std::wstring part;
        std::map<uint64_t, Outstanding> out;     // offset -> request
        std::map<uint64_t, uint64_t> written;     // offset -> length, beyond `done`
        int inflight = 0;                         // chunks handed to the worker
    };

    struct Client
    {
        CPhase phase = CPhase::Idle;
        uint64_t gen = 1;
        std::string error, errorDetail;
        double lastHeard = 0.0;
        BigRx rx;
        Manifest m;
        std::string nonce;
        std::vector<LocalState> local;
        std::vector<std::string> hostOrder;
        std::vector<std::string> nativeMissing;
        // verdict
        std::vector<uint32_t> need;
        std::vector<std::string> activate;
        std::vector<std::pair<std::string, std::string>> blocks, warns;
        uint64_t needBytes = 0;
        int needScripts = 0;
        // download
        size_t dlPos = 0;
        CurFile cur;
        uint64_t bytesCompleted = 0;
        double lastData = 0.0;
        int protocolErrors = 0;
        std::vector<StagedFile> staged;
        bool left = false;
        // join target (kept across attempts)
        std::string joinKind, joinValue, joinLabel;
    };
    Client s_c;
    std::atomic<uint64_t> s_checkDone{ 0 }, s_checkTotal{ 0 };

    // Rejoin armed at game start.
    bool        s_rejoinPending = false;
    double      s_rejoinAt = 0.0;
    std::string s_rejoinKind, s_rejoinValue, s_rejoinLabel;

    // Worker-only: the staging file being written.
    struct PartState { HANDLE h = INVALID_HANDLE_VALUE; uint64_t gen = 0; std::wstring path; };
    PartState s_part;

    void WorkerClosePart()
    {
        if (s_part.h != INVALID_HANDLE_VALUE)
        {
            FlushFileBuffers(s_part.h);
            CloseHandle(s_part.h);
        }
        s_part = PartState();
    }

    void ClientLeave(const char* why)
    {
        if (s_c.left) return;
        s_c.left = true;
        // LeaveWithReason does nothing unless this machine is a GNS client.
        if (CoSyncTransport::IsInitialized())
            GNS_Session::Get().LeaveWithReason(CoSyncSessionConfig::kEndReason_ContentSync, why);
    }

    void ClientFail(const std::string& code, const std::string& detail = std::string())
    {
        s_c.error = code;
        s_c.errorDetail = detail;
        s_c.phase = CPhase::Failed;
        ++s_c.gen;
        Disk().Post([] { WorkerClosePart(); });
        LOG_ERROR("[ContentSync] Client: content sync failed: %s %s", code.c_str(), detail.c_str());
        ClientLeave(code.c_str());
    }

    void ClientStartNextFile();

    // ── Check local files (worker) ─────────────────────────────────────────
    void ClientOnManifest(const std::string& text)
    {
        const size_t nl = text.find('\n');
        const std::string first = nl == std::string::npos ? text : text.substr(0, nl);
        if (first.rfind("NONCE|", 0) != 0 || first.size() != 6 + 32)
        { ClientFail("bad_manifest", "no nonce"); return; }
        std::string nonce = first.substr(6);
        for (char c : nonce)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { ClientFail("bad_manifest", "bad nonce"); return; }

        Manifest m;
        std::string why;
        if (!ParseManifest(text.substr(nl + 1), m, why))
        {
            // Nothing from a manifest that fails any rule is used -- not even
            // its safe entries.
            ClientFail("bad_manifest", why);
            return;
        }
        if (m.build != CoSyncNet::GetBuildNumber())
        { ClientFail("version_mismatch"); return; }

        s_c.m = m;
        s_c.nonce = nonce;
        s_c.hostOrder.clear();
        std::vector<std::pair<int, std::string>> order;
        for (const Entry& e : m.entries)
            if (e.kind == Kind::Plugin) order.push_back({ e.slot, e.relPath });
        std::sort(order.begin(), order.end());
        for (auto& o : order) s_c.hostOrder.push_back(o.second);

        const std::string lo = LoadOrderText(SnapshotLoadedPlugins(false));   // game thread
        s_c.phase = CPhase::Checking;
        s_c.lastHeard = NowMs();
        uint64_t total = 0;
        for (const Entry& e : m.entries) total += e.size;
        s_checkTotal.store(total);
        s_checkDone.store(0);
        LOG_INFO("[ContentSync] Client: host content = %zu file(s); checking this installation", m.entries.size());

        const uint64_t gen = s_c.gen;
        Disk().Post([gen, m, nonce, lo]
        {
            const Paths& p = P();
            const OfficialLists off = LoadOfficialLists(p);
            CreateDirectories(p.state);
            HashCache cache;
            cache.Load(p.cache);
            std::vector<LocalState> local(m.entries.size());
            std::string report = "LO|" + lo + "\n";
            std::string unsafe;
            std::unordered_map<std::string, bool> pluginOfficial;
            for (size_t i = 0; i < m.entries.size(); ++i)
            {
                const Entry& e = m.entries[i];
                LocalState& ls = local[i];
                if (e.kind == Kind::Plugin)
                {
                    // The client decides for itself what is official: a host
                    // calling Fallout4.esm a "mod" still never gets it replaced.
                    ls.officialHere = !e.Transferable() || ClassifyPlugin(e.relPath, off.ccc, off.catalog) != Origin::Mod;
                    pluginOfficial[Lower(e.relPath)] = ls.officialHere;
                }
                else
                {
                    auto it = pluginOfficial.find(Lower(e.owner));
                    ls.officialHere = !e.Transferable() || (it != pluginOfficial.end() && it->second);
                }
                std::string why;
                if (!ResolveInside(p.data, e.relPath, ls.full, why))
                {
                    unsafe = e.relPath + ": " + why;
                    break;
                }
                std::string sha, proof;
                uint64_t size = 0, mtime = 0;
                char state = 'X';
                if (GetFileSizeTime(ls.full, size, mtime))
                {
                    ls.exists = true;
                    state = 'D';
                    if (size == e.size && HashFileCached(cache, ls.full, e.relPath, sha, size, nullptr, &s_checkDone) &&
                        sha == e.sha256 && ComputeProof(ls.full, size, nonce, proof))
                    {
                        ls.match = true;
                        state = 'M';
                    }
                    else if (size != e.size)
                        s_checkDone.fetch_add(e.size);
                }
                else
                    s_checkDone.fetch_add(e.size);
                report += "R|" + std::to_string(i) + "|" + state + "|" + (state == 'M' ? proof : std::string("-")) + "\n";
            }
            report += "E\n";
            cache.Save(p.cache);

            std::vector<std::string> nativeMissing;
            for (const std::string& n : m.nativePlugins)
                if (GetFileAttributesW((p.data + L"\\F4SE\\Plugins\\" + Widen(n)).c_str()) == INVALID_FILE_ATTRIBUTES)
                    nativeMissing.push_back(n);

            PostMain([gen, local, report, unsafe, nativeMissing]
            {
                if (gen != s_c.gen || s_c.phase != CPhase::Checking) return;
                if (!unsafe.empty()) { ClientFail("unsafe_path", unsafe); return; }
                s_c.local = local;
                s_c.nativeMissing = nativeMissing;
                size_t matched = 0;
                for (const LocalState& l : local) if (l.match) ++matched;
                LOG_INFO("[ContentSync] Client: %zu of %zu file(s) match the host's -- report sent", matched, local.size());
                SendBig(false, 0, "REP", report);
                s_c.phase = CPhase::Waiting;
                s_c.lastHeard = NowMs();
            });
        });
    }

    // ── Verdict ─────────────────────────────────────────────────────────────
    void ClientStageInstall();

    void ClientOnVerdict(const std::string& text)
    {
        std::vector<std::string> lines = Split(text, '\n');
        if (!lines.empty() && lines.back().empty()) lines.pop_back();
        std::string status;
        std::vector<uint32_t> need;
        std::vector<std::string> activate;
        std::vector<std::pair<std::string, std::string>> blocks, warns;
        bool ended = false;
        for (const std::string& line : lines)
        {
            if (ended) { ClientFail("bad_verdict"); return; }
            if (line == "E") { ended = true; continue; }
            const std::vector<std::string> f = Split(line, '|');
            if (f[0] == "S" && f.size() == 2 && status.empty()) { status = f[1]; continue; }
            if (f[0] == "T" && f.size() == 2)
            {
                uint64_t i = 0;
                if (!ParseU64(f[1], i) || i >= s_c.m.entries.size()) { ClientFail("bad_verdict"); return; }
                need.push_back(static_cast<uint32_t>(i));
                continue;
            }
            if (f[0] == "A" && f.size() == 2) { activate.push_back(f[1]); continue; }
            if ((f[0] == "B" || f[0] == "W") && f.size() == 3)
            {
                auto& list = f[0] == "B" ? blocks : warns;
                if (list.size() < 200) list.push_back({ f[1], f[2] });
                continue;
            }
            ClientFail("bad_verdict");
            return;
        }
        if (!ended || (status != "OK" && status != "NEED" && status != "ACTIVATE" && status != "BLOCK"))
        { ClientFail("bad_verdict"); return; }

        s_c.blocks = blocks;
        s_c.warns = warns;
        for (const auto& w : warns)
            LOG_WARN("[ContentSync] Client: host note: %s %s", w.first.c_str(), w.second.c_str());

        if (status == "OK")
        {
            s_c.phase = CPhase::Ready;
            LOG_INFO("[ContentSync] Client: content verified by the host -- requesting the world");
            CoSyncWorldTransfer::ClientBegin(CoSyncProfiles::GetMyProfileKey(), CoSyncNet::GetMyName());
            return;
        }
        if (status == "BLOCK")
        {
            s_c.phase = CPhase::Blocked;
            for (const auto& b : blocks)
                LOG_WARN("[ContentSync] Client: cannot join: %s %s", b.first.c_str(), b.second.c_str());
            ClientLeave("content_blocked");
            return;
        }

        // NEED / ACTIVATE: check everything the host asks for against this
        // side's own rules before a single byte is written.
        s_c.need.clear();
        s_c.needBytes = 0;
        s_c.needScripts = 0;
        for (uint32_t i : need)
        {
            const Entry& e = s_c.m.entries[i];
            const LocalState& ls = s_c.local[i];
            if (!e.Transferable() || ls.officialHere)
            { ClientFail("refused_official", e.relPath); return; }
            if (ls.match)
                continue;   // already have it: nothing to fetch
            if (std::find(s_c.need.begin(), s_c.need.end(), i) != s_c.need.end())
                continue;
            s_c.need.push_back(i);
            s_c.needBytes += e.size;
            const std::string ext = FileExtLower(e.relPath);
            if (ext == ".pex" || ext == ".swf") ++s_c.needScripts;
        }
        s_c.activate.clear();
        for (const std::string& a : activate)
        {
            bool known = false;
            for (size_t i = 0; i < s_c.m.entries.size(); ++i)
            {
                const Entry& e = s_c.m.entries[i];
                if (e.kind == Kind::Plugin && IEquals(e.relPath, a))
                {
                    if (!e.Transferable() || s_c.local[i].officialHere) { ClientFail("refused_official", a); return; }
                    known = true;
                    break;
                }
            }
            if (!known) { ClientFail("bad_verdict", "activation of an unknown plugin"); return; }
            s_c.activate.push_back(a);
        }
        if (s_c.needBytes > kMaxTotalBytes)
        { ClientFail("too_large", FormatBytes(s_c.needBytes)); return; }

        if (s_c.need.empty())
        {
            LOG_INFO("[ContentSync] Client: every file matches; %zu plugin(s) must be enabled", s_c.activate.size());
            ClientStageInstall();
            return;
        }

        ULARGE_INTEGER freeBytes{};
        const bool haveFree = GetDiskFreeSpaceExW(P().data.c_str(), &freeBytes, nullptr, nullptr) != 0;
#ifdef COSYNC_CONTENT_OFFLINE_TEST
        if (g_contentTestFreeBytes) freeBytes.QuadPart = g_contentTestFreeBytes;
#endif
        if (haveFree &&
            freeBytes.QuadPart < s_c.needBytes + kDiskMargin)
        {
            ClientFail("disk_space", "needs " + FormatBytes(s_c.needBytes + kDiskMargin) + ", " +
                       FormatBytes(freeBytes.QuadPart) + " free");
            return;
        }
        LOG_INFO("[ContentSync] Client: %zu file(s) missing or different (%s) -- asking the player",
            s_c.need.size(), FormatBytes(s_c.needBytes).c_str());
        for (uint32_t i : s_c.need)
            LOG_INFO("[ContentSync]   need %s (%s, %s)", s_c.m.entries[i].relPath.c_str(),
                FormatBytes(s_c.m.entries[i].size).c_str(), s_c.local[i].exists ? "different here" : "missing here");

        const bool autoAccept = GetPrivateProfileIntW(L"Content", L"bAutoAcceptDownloads", 0,
            (P().data + L"\\F4SE\\Plugins\\CoSync.ini").c_str()) != 0;
        s_c.phase = CPhase::Confirm;
        if (autoAccept)
        {
            LOG_INFO("[ContentSync] Client: bAutoAcceptDownloads=1 -- downloading without asking");
            CoSyncContentSync::UiCommand("accept");
        }
    }

    // ── Download ────────────────────────────────────────────────────────────
    void ClientSendGet(uint64_t off, uint64_t len)
    {
        ClientSend("CSGET|" + std::to_string(s_c.cur.idx) + "|" + std::to_string(off) + "|" + std::to_string(len));
    }

    void ClientPump()
    {
        CurFile& c = s_c.cur;
        if (!c.active || c.opening || c.finalizing)
            return;
        const double now = NowMs();
        while (static_cast<int>(c.out.size()) + c.inflight < kWindow && c.nextReq < c.size)
        {
            const uint64_t len = (std::min<uint64_t>)(kChunkBytes, c.size - c.nextReq);
            c.out[c.nextReq] = { len, now, 1 };
            ClientSendGet(c.nextReq, len);
            c.nextReq += len;
        }
        for (auto& kv : c.out)
            if (now - kv.second.sentAt > kChunkTimeoutMs)
            {
                if (++kv.second.tries > kMaxChunkTries)
                { ClientFail("host_stalled", "no answer for a chunk"); return; }
                kv.second.sentAt = now;
                LOG_WARN("[ContentSync] Client: chunk %llu of '%s' timed out -- asking again (try %d)",
                    static_cast<unsigned long long>(kv.first), s_c.m.entries[c.idx].relPath.c_str(), kv.second.tries);
                ClientSendGet(kv.first, kv.second.len);
            }
    }

    void ClientFileVerified(bool ok, const std::string& shaGot, uint64_t mtime)
    {
        CurFile& c = s_c.cur;
        const Entry& e = s_c.m.entries[c.idx];
        c.finalizing = false;
        if (!ok)
        {
            LOG_ERROR("[ContentSync] Client: '%s' failed verification (got %s, expected %s)",
                e.relPath.c_str(), shaGot.substr(0, 16).c_str(), e.sha256.substr(0, 16).c_str());
            if (++c.attempts >= 2)
            { ClientFail("hash_mismatch", e.relPath); return; }
            // Start this file over from zero (the worker deleted the part).
            c.active = false;
            ClientStartNextFile();
            return;
        }
        LOG_INFO("[ContentSync] Client: '%s' downloaded and verified (SHA-256 %s)", e.relPath.c_str(), e.sha256.substr(0, 16).c_str());
        s_c.staged.push_back({ c.idx, c.part, e.size, mtime });
        s_c.bytesCompleted += e.size;
        c = CurFile();
        ++s_c.dlPos;
        ClientStartNextFile();
    }

    void ClientStartNextFile()
    {
        if (s_c.phase != CPhase::Downloading)
            return;
        if (!s_c.cur.active)
        {
            if (s_c.dlPos >= s_c.need.size())
            {
                ClientStageInstall();
                return;
            }
            const int attempts = s_c.cur.attempts;
            s_c.cur = CurFile();
            CurFile& c = s_c.cur;
            c.attempts = attempts;
            c.active = true;
            c.opening = true;
            c.idx = s_c.need[s_c.dlPos];
            const Entry& e = s_c.m.entries[c.idx];
            c.size = e.size;
            c.sha = e.sha256;
            // Named by content AND path: two identical files under different
            // names each get their own staged copy; stable across sessions
            // so an interrupted download resumes.
            c.part = P().staging + L"\\" + Widen(e.sha256.substr(0, 40) + "_" + Sha256Hex(Lower(e.relPath).data(), e.relPath.size()).substr(0, 12)) + L".part";
            s_c.lastData = NowMs();
            LOG_INFO("[ContentSync] Client: downloading '%s' (%s)", e.relPath.c_str(), FormatBytes(e.size).c_str());
            const uint64_t gen = s_c.gen;
            const std::wstring part = c.part;
            const std::string sha = c.sha;
            const uint64_t size = c.size;
            const bool fresh = attempts > 0;
            Disk().Post([gen, part, sha, size, fresh]
            {
                WorkerClosePart();
                CreateDirectories(P().staging);
                const std::wstring meta = part + L".meta";
                uint64_t resume = 0;
                std::string mtext;
                if (!fresh && ReadWholeFile(meta, mtext, 4096))
                {
                    const std::vector<std::string> f = Split(mtext, '|');
                    uint64_t msz = 0, mdone = 0, psz = 0, pmt = 0;
                    if (f.size() == 3 && f[0] == sha && ParseU64(f[1], msz) && ParseU64(f[2], mdone) && msz == size &&
                        mdone <= size && GetFileSizeTime(part, psz, pmt) && psz >= mdone)
                        resume = mdone;
                }
                std::string err;
                HANDLE h = CreateFileW(part.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                       resume > 0 ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                    err = "cannot create the staging file";
                else if (resume == 0)
                {
                    // Reserve the space now: a full disk fails here, not
                    // half-way through a multi-GB download.
                    LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(size);
                    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN) || !SetEndOfFile(h))
                        err = GetLastError() == ERROR_DISK_FULL ? "disk_full" : "cannot size the staging file";
                    DeleteFileW(meta.c_str());
                }
                if (!err.empty() && h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
                if (h != INVALID_HANDLE_VALUE) { s_part.h = h; s_part.gen = gen; s_part.path = part; }
                PostMain([gen, resume, err]
                {
                    if (gen != s_c.gen || s_c.phase != CPhase::Downloading) return;
                    if (!err.empty()) { ClientFail(err == "disk_full" ? "disk_space" : "write_failed", err); return; }
                    CurFile& c = s_c.cur;
                    c.opening = false;
                    c.done = c.nextReq = c.metaAt = resume;
                    if (resume > 0)
                        LOG_INFO("[ContentSync] Client: resuming '%s' at %s", s_c.m.entries[c.idx].relPath.c_str(), FormatBytes(resume).c_str());
                    ClientPump();
                    if (c.done >= c.size && c.out.empty())
                    {
                        // Already complete from an earlier attempt: verify it.
                        c.finalizing = true;
                        const uint64_t g = s_c.gen;
                        const std::string sha = c.sha;
                        const std::wstring part = c.part;
                        Disk().Post([g, sha, part]
                        {
                            WorkerClosePart();
                            std::string got; uint64_t sz = 0, psz = 0, pmt = 0;
                            const bool ok = HashFile(part, got, sz) && got == sha && GetFileSizeTime(part, psz, pmt);
                            if (!ok) { DeleteFileW(part.c_str()); DeleteFileW((part + L".meta").c_str()); }
                            PostMain([g, ok, got, pmt] { if (g == s_c.gen && s_c.phase == CPhase::Downloading) ClientFileVerified(ok, got, pmt); });
                        });
                    }
                });
            });
        }
    }

    void ClientOnChunkWritten(uint64_t off, uint64_t len)
    {
        CurFile& c = s_c.cur;
        --c.inflight;
        c.written[off] = len;
        // Advance the verified-contiguous prefix.
        for (auto it = c.written.find(c.done); it != c.written.end(); it = c.written.find(c.done))
        {
            c.done += it->second;
            c.written.erase(it);
        }
        const uint64_t gen = s_c.gen;
        if (c.done - c.metaAt >= kMetaEveryBytes && c.done < c.size)
        {
            c.metaAt = c.done;
            const std::wstring meta = c.part + L".meta";
            const std::string text = c.sha + "|" + std::to_string(c.size) + "|" + std::to_string(c.done);
            Disk().Post([gen, meta, text]
            {
                if (s_part.gen != gen || s_part.h == INVALID_HANDLE_VALUE) return;
                FlushFileBuffers(s_part.h);   // data first, then the resume point that trusts it
                WriteWholeFileAtomic(meta, text);
            });
        }
        if (c.done >= c.size && c.out.empty() && c.inflight == 0)
        {
            c.finalizing = true;
            const std::string sha = c.sha;
            const std::wstring part = c.part;
            const std::string text = c.sha + "|" + std::to_string(c.size) + "|" + std::to_string(c.size);
            s_c.lastData = NowMs();
            Disk().Post([gen, sha, part, text]
            {
                WorkerClosePart();
                std::string got; uint64_t sz = 0, psz = 0, pmt = 0;
                const bool ok = HashFile(part, got, sz) && got == sha && GetFileSizeTime(part, psz, pmt);
                if (ok)
                    WriteWholeFileAtomic(part + L".meta", text);   // a complete, verified part
                else
                {
                    DeleteFileW(part.c_str());
                    DeleteFileW((part + L".meta").c_str());
                }
                PostMain([gen, ok, got, pmt] { if (gen == s_c.gen && s_c.phase == CPhase::Downloading) ClientFileVerified(ok, got, pmt); });
            });
            return;
        }
        ClientPump();
    }

    void ClientOnData(const std::string& msg)
    {
        // CSDATA|<idx>|<offset>|<crc32 hex>|<bytes>
        size_t pos = 7;
        std::string f[3];
        for (int i = 0; i < 3; ++i)
        {
            const size_t bar = msg.find('|', pos);
            if (bar == std::string::npos || bar - pos > 24) { ++s_c.protocolErrors; return; }
            f[i] = msg.substr(pos, bar - pos);
            pos = bar + 1;
        }
        CurFile& c = s_c.cur;
        uint64_t idx = 0, off = 0;
        if (s_c.phase != CPhase::Downloading || !c.active || c.opening || !ParseU64(f[0], idx) || !ParseU64(f[1], off) ||
            f[2].size() != 8 || idx != c.idx)
        {
            if (++s_c.protocolErrors > 200) ClientFail("protocol", "malformed data");
            return;
        }
        auto it = c.out.find(off);
        const size_t n = msg.size() - pos;
        if (it == c.out.end() || n != it->second.len)
        {
            // A duplicate answer to a re-sent request, or junk.
            if (++s_c.protocolErrors > 200) ClientFail("protocol", "unexpected data");
            return;
        }
        const uint32_t crc = static_cast<uint32_t>(std::strtoul(f[2].c_str(), nullptr, 16));
        const int tries = it->second.tries;
        c.out.erase(it);
        ++c.inflight;
        s_c.lastData = NowMs();
        const uint64_t gen = s_c.gen;
        std::string data = msg.substr(pos);
        Disk().Post([gen, off, crc, tries, data]
        {
            if (s_part.gen != gen || s_part.h == INVALID_HANDLE_VALUE) return;
            if (Crc32(data.data(), data.size()) != crc)
            {
                PostMain([gen, off, tries, n = static_cast<uint64_t>(data.size())]
                {
                    if (gen != s_c.gen || s_c.phase != CPhase::Downloading) return;
                    CurFile& c = s_c.cur;
                    --c.inflight;
                    LOG_WARN("[ContentSync] Client: chunk at %llu of '%s' arrived damaged -- asking again",
                        static_cast<unsigned long long>(off), s_c.m.entries[c.idx].relPath.c_str());
                    if (tries + 1 > kMaxChunkTries) { ClientFail("corrupt_transfer", s_c.m.entries[c.idx].relPath); return; }
                    c.out[off] = { n, NowMs(), tries + 1 };
                    ClientSendGet(off, n);
                });
                return;
            }
            LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(off);
            DWORD wrote = 0;
            const bool ok = SetFilePointerEx(s_part.h, li, nullptr, FILE_BEGIN) &&
                            ::WriteFile(s_part.h, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr) &&
                            wrote == data.size();
            const DWORD err = ok ? 0 : GetLastError();
            PostMain([gen, off, ok, err, n = static_cast<uint64_t>(data.size())]
            {
                if (gen != s_c.gen || s_c.phase != CPhase::Downloading) return;
                if (!ok)
                {
                    ClientFail(err == ERROR_DISK_FULL ? "disk_space" : "write_failed", "write error " + std::to_string(err));
                    return;
                }
                ClientOnChunkWritten(off, n);
            });
        });
    }

    // ── Install ─────────────────────────────────────────────────────────────
    struct InstallItem { std::wstring staged; std::string rel; uint64_t size = 0; uint64_t mtime = 0; };

    std::string Timestamp()
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char b[32];
        std::strftime(b, sizeof(b), "%Y%m%d-%H%M%S", &tm);
        return b;
    }

    // Journal lines (UTF-8, '|' separated; Windows paths cannot contain '|'):
    //   B|<dest>|<backup>   dest is about to move to backup
    //   I|<dest>|<staged>   staged is about to move to dest
    //   T|<plugins.txt>|<backup copy>
    //   COMMIT
    void Rollback(const std::vector<std::string>& journal)
    {
        for (auto it = journal.rbegin(); it != journal.rend(); ++it)
        {
            const std::vector<std::string> f = Split(*it, '|');
            if (f.size() != 3) continue;
            const std::wstring a = Widen(f[1]), b = Widen(f[2]);
            if (f[0] == "I")
            {
                // New file back to staging (kept for the next attempt).
                if (GetFileAttributesW(a.c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(b.c_str()) == INVALID_FILE_ATTRIBUTES)
                    MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_WRITE_THROUGH);
            }
            else if (f[0] == "B")
            {
                if (GetFileAttributesW(b.c_str()) != INVALID_FILE_ATTRIBUTES)
                    MoveFileExW(b.c_str(), a.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            }
            else if (f[0] == "T")
            {
                if (GetFileAttributesW(b.c_str()) != INVALID_FILE_ATTRIBUTES)
                    CopyFileW(b.c_str(), a.c_str(), FALSE);
            }
            LOG_WARN("[ContentSync] Rolled back: %s", it->c_str());
        }
    }

    // All or nothing: every file is placed and Plugins.txt updated, or every
    // step taken is undone. Replaced files go to backup\<time>\, never
    // deleted. Returns "" on success, else the reason. activationChanged
    // tells whether Plugins.txt had to change.
    std::string InstallNow(const std::vector<InstallItem>& items, const std::vector<std::string>& activate,
                           const std::vector<std::string>& hostOrder, bool& activationChanged)
    {
        activationChanged = false;
        const Paths& p = P();
        const std::wstring backupDir = p.backup + L"\\" + Widen(Timestamp());
        std::vector<std::string> journal;
        auto flush = [&]() -> bool
        {
            std::string t;
            for (const std::string& l : journal) t += l + "\n";
            return WriteWholeFileAtomic(p.journal, t);
        };
        auto fail = [&](const std::string& why) -> std::string
        {
            LOG_ERROR("[ContentSync] Install failed: %s -- rolling back", why.c_str());
            Rollback(journal);
            DeleteFileW(p.journal.c_str());
            return why;
        };
        CreateDirectories(p.state);

        for (const InstallItem& it : items)
        {
            std::wstring dest;
            std::string why;
            if (!ResolveInside(p.data, it.rel, dest, why))
                return fail("unsafe destination " + it.rel + " (" + why + ")");
            // The staged copy must still be exactly the file that passed the
            // SHA-256 check (same size, not written since).
            uint64_t ssz = 0, smt = 0;
            if (!GetFileSizeTime(it.staged, ssz, smt))
                return fail("staged file missing for " + it.rel);
            if (ssz != it.size || smt != it.mtime)
                return fail("staged file for " + it.rel + " changed after it was verified");
            const size_t slash = dest.find_last_of(L'\\');
            if (!CreateDirectories(dest.substr(0, slash)))
                return fail("cannot create the folder for " + it.rel);
            // Re-check after creating folders: nothing may have become a link.
            std::wstring again;
            if (!ResolveInside(p.data, it.rel, again, why) || again != dest)
                return fail("destination changed for " + it.rel);

            if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                const std::wstring backup = backupDir + L"\\" + Widen(it.rel);
                CreateDirectories(backup.substr(0, backup.find_last_of(L'\\')));
                journal.push_back("B|" + Narrow(dest) + "|" + Narrow(backup));
                if (!flush()) return fail("cannot write the install journal");
                if (!MoveFileExW(dest.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH))
                    return fail("cannot move the old " + it.rel + " aside (error " + std::to_string(GetLastError()) + " -- is the game or a tool using it?)");
                LOG_INFO("[ContentSync] Install: kept the old %s in %s", it.rel.c_str(), Narrow(backup).c_str());
            }
            journal.push_back("I|" + Narrow(dest) + "|" + Narrow(it.staged));
            if (!flush()) return fail("cannot write the install journal");
            if (!MoveFileExW(it.staged.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return fail("cannot place " + it.rel + " (error " + std::to_string(GetLastError()) + ")");
            DeleteFileW((it.staged + L".meta").c_str());
            LOG_INFO("[ContentSync] Install: placed Data\\%s", it.rel.c_str());
        }

        if (!activate.empty())
        {
            if (p.pluginsTxt.empty())
                return fail("cannot find Plugins.txt");
            std::string text;
            ReadWholeFile(p.pluginsTxt, text, 4u << 20);   // a missing file is an empty list
            const std::string next = ActivateInPluginsTxt(text, activate, hostOrder, activationChanged);
            if (activationChanged)
            {
                const std::wstring copy = backupDir + L"\\Plugins.txt";
                CreateDirectories(backupDir);
                if (!text.empty())
                {
                    if (!WriteWholeFileAtomic(copy, text)) return fail("cannot back up Plugins.txt");
                    journal.push_back("T|" + Narrow(p.pluginsTxt) + "|" + Narrow(copy));
                    if (!flush()) return fail("cannot write the install journal");
                }
                if (!WriteWholeFileAtomic(p.pluginsTxt, next))
                    return fail("cannot write Plugins.txt");
                for (const std::string& a : activate)
                    LOG_INFO("[ContentSync] Install: enabled %s in Plugins.txt", a.c_str());
            }
        }

        journal.push_back("COMMIT");
        flush();
        DeleteFileW(p.journal.c_str());
        return std::string();
    }

    // ── Pending install ─────────────────────────────────────────────────────
    // The running game keeps every loaded plugin and archive open, so a file
    // it already has (a different version) cannot be replaced now, and a new
    // plugin would not load before a restart anyway: Plugins.txt and every
    // plugin/archive are read by InitGameDataThread::Run (0x140C41840 ->
    // sub_140C3EC50 -> sub_140C323F0 -> sub_1402E9020 / sub_1402E8020), which
    // F4SE only lets run after every plugin's F4SEPlugin_Load. So verified
    // files wait in staging, listed in pending.txt, and ApplyPendingInstall
    // moves them into place at the next start, before the engine opens them.
    //
    // pending.txt:
    //   v1
    //   F|<staged path>|<relpath>|<size>|<mtime>|<sha256>
    //   A|<plugin to enable>
    //   O|<host load order, '*'-separated>
    //   E

    void ClientStageInstall()
    {
        s_c.phase = CPhase::Installing;
        std::string pending = "v1\n";
        for (const StagedFile& st : s_c.staged)
        {
            const Entry& e = s_c.m.entries[st.idx];
            pending += "F|" + Narrow(st.path) + "|" + e.relPath + "|" + std::to_string(st.size) + "|" +
                       std::to_string(st.mtime) + "|" + e.sha256 + "\n";
        }
        for (const std::string& a : s_c.activate)
            pending += "A|" + a + "\n";
        std::string order;
        for (const std::string& o : s_c.hostOrder) { if (!order.empty()) order += "*"; order += o; }
        pending += "O|" + order + "\nE\n";

        const uint64_t gen = s_c.gen;
        const bool noFiles = s_c.staged.empty();
        const std::vector<std::string> activate = s_c.activate;
        const std::vector<std::string> hostOrder = s_c.hostOrder;
        const std::string rejoin = "v1|" + s_c.joinKind + "|" + s_c.joinValue + "|" + s_c.joinLabel + "|" +
                                   std::to_string(static_cast<long long>(std::time(nullptr)));
        const bool haveTarget = !s_c.joinKind.empty();
        LOG_INFO("[ContentSync] Client: %zu verified file(s) and %zu plugin(s) to enable are queued for the next start",
            s_c.staged.size(), activate.size());
        Disk().Post([gen, pending, noFiles, activate, hostOrder, rejoin, haveTarget]
        {
            WorkerClosePart();
            std::string err;
            if (noFiles)
            {
                // Only enabling: if Plugins.txt already enables them all and
                // the engine still did not load them, something else (a mod
                // manager) controls the load order -- a restart would not help.
                std::string text;
                bool changed = false;
                if (!P().pluginsTxt.empty())
                    ReadWholeFile(P().pluginsTxt, text, 4u << 20);
                ActivateInPluginsTxt(text, activate, hostOrder, changed);
                if (!changed)
                    err = "not_loaded";
            }
            if (err.empty() && !WriteWholeFileAtomic(P().pending, pending))
                err = "write_failed";
            if (err.empty() && haveTarget)
                WriteWholeFileAtomic(P().rejoin, rejoin);
            PostMain([gen, err]
            {
                if (gen != s_c.gen || s_c.phase != CPhase::Installing) return;
                if (err == "not_loaded")
                {
                    std::string names;
                    for (const std::string& a : s_c.activate) { if (!names.empty()) names += ", "; names += a; }
                    ClientFail("not_loaded", names);
                    return;
                }
                if (!err.empty()) { ClientFail("install_failed", "cannot write the install list"); return; }
                s_c.phase = CPhase::Restart;
                LOG_INFO("[ContentSync] Client: content verified and queued -- Fallout 4 must restart to install and load it");
                ClientSend("CSDONE|staged");
                ClientLeave("content_staged");
            });
        });
    }

    // Startup (F4SEPlugin_Load, before any data file is opened). Synchronous.
    void ApplyPendingInstallNow()
    {
        const Paths& p = P();

        // 1. An install that did not reach COMMIT (crash, power loss) is undone.
        std::string j;
        if (ReadWholeFile(p.journal, j, 4u << 20))
        {
            std::vector<std::string> lines = Split(j, '\n');
            while (!lines.empty() && lines.back().empty()) lines.pop_back();
            if (!lines.empty() && lines.back() == "COMMIT")
                LOG_INFO("[ContentSync] Startup: previous install completed");
            else
            {
                LOG_WARN("[ContentSync] Startup: previous install was interrupted -- rolling it back (%zu step(s))", lines.size());
                Rollback(lines);
            }
            DeleteFileW(p.journal.c_str());
        }

        // 2. Apply what the last session downloaded and verified.
        std::string text;
        if (!ReadWholeFile(p.pending, text, 4u << 20))
            return;
        DeleteFileW(p.pending.c_str());   // one attempt: a failure is reported, not retried forever
        std::vector<std::string> lines = Split(text, '\n');
        while (!lines.empty() && (lines.back().empty() || lines.back() == "\r")) lines.pop_back();
        if (lines.size() < 2 || lines.front() != "v1" || lines.back() != "E")
        {
            LOG_ERROR("[ContentSync] Startup: pending install list is damaged -- nothing installed");
            return;
        }
        std::vector<InstallItem> items;
        std::vector<std::string> activate, hostOrder;
        const std::wstring stagingPrefix = p.staging + L"\\";
        for (size_t i = 1; i + 1 < lines.size(); ++i)
        {
            const std::vector<std::string> f = Split(lines[i], '|');
            std::string why;
            if (f[0] == "F" && f.size() == 6)
            {
                InstallItem it;
                it.staged = Widen(f[1]);
                it.rel = f[2];
                const bool rootFile = it.rel.find('\\') == std::string::npos;
                const std::string ext = FileExtLower(it.rel);
                const bool rootOk = rootFile && (IsPluginFileName(it.rel) || ext == ".ba2" || ext == ".csg" || ext == ".psg" || ext == ".cdx");
                if (!ParseU64(f[3], it.size) || !ParseU64(f[4], it.mtime) || !IsSha256Hex(f[5]) ||
                    it.staged.compare(0, stagingPrefix.size(), stagingPrefix) != 0 ||
                    it.staged.find(L"..") != std::wstring::npos || !IsSafeRelativePath(it.rel, why) ||
                    !(rootOk || (!rootFile && IsAllowedLoosePath(it.rel, why))))
                {
                    LOG_ERROR("[ContentSync] Startup: pending entry refused: %s -- nothing installed", lines[i].c_str());
                    return;
                }
                items.push_back(it);
            }
            else if (f[0] == "A" && f.size() == 2 && IsPluginFileName(f[1]) && IsSafeRelativePath(f[1], why))
                activate.push_back(f[1]);
            else if (f[0] == "O" && f.size() == 2)
            {
                if (!f[1].empty())
                    hostOrder = Split(f[1], '*');
            }
            else
            {
                LOG_ERROR("[ContentSync] Startup: pending install list has a bad line -- nothing installed");
                return;
            }
        }
        LOG_INFO("[ContentSync] Startup: installing %zu downloaded file(s), enabling %zu plugin(s)", items.size(), activate.size());
        bool changed = false;
        const std::string err = InstallNow(items, activate, hostOrder, changed);
        if (err.empty())
            LOG_INFO("[ContentSync] Startup: install complete%s", changed ? " (Plugins.txt updated)" : "");
        else
            LOG_ERROR("[ContentSync] Startup: install failed and was rolled back: %s", err.c_str());
    }

    void ClientOnReceive(const std::string& msg)
    {
        s_c.lastHeard = NowMs();
        if (msg.rfind("CSDATA|", 0) == 0) { ClientOnData(msg); return; }
        if (msg.rfind("CSWAIT|", 0) == 0)
        {
            if (s_c.phase == CPhase::Requesting || s_c.phase == CPhase::Preparing)
            {
                s_c.phase = CPhase::Preparing;
                const std::vector<std::string> f = Split(msg, '|');
                uint64_t a = 0, b = 0;
                if (f.size() == 3 && ParseU64(f[1], a) && ParseU64(f[2], b)) { s_checkDone.store(a); s_checkTotal.store(b); }
            }
            return;
        }
        if (msg.rfind("CSFAIL|", 0) == 0)
        {
            if (IsActivePhase(s_c.phase))
                ClientFail(msg.substr(7, 40));
            return;
        }
        if (msg.rfind("CSERR|", 0) == 0)
        {
            if (s_c.phase == CPhase::Downloading)
                ClientFail("host_read_error", msg.substr(6, 60));
            return;
        }
        if (msg.rfind("CSBIG|", 0) == 0)
        {
            std::string type, text;
            const int r = BigFeed(s_c.rx, msg, type, text);
            if (r < 0) { if (IsActivePhase(s_c.phase)) ClientFail("protocol", "bad message part"); return; }
            if (r == 0) return;
            if (type == "MAN" && (s_c.phase == CPhase::Requesting || s_c.phase == CPhase::Preparing))
                ClientOnManifest(text);
            else if (type == "VER" && s_c.phase == CPhase::Waiting)
                ClientOnVerdict(text);
            return;
        }
    }

    void ClientTick()
    {
        const double now = NowMs();
        switch (s_c.phase)
        {
        case CPhase::Requesting:
        case CPhase::Preparing:
            if (now - s_c.lastHeard > kAnswerTimeoutMs)
                ClientFail("no_response");
            break;
        case CPhase::Waiting:
            if (now - s_c.lastHeard > kVerdictTimeoutMs)
                ClientFail("no_response");
            break;
        case CPhase::Downloading:
            if (s_c.cur.active && !s_c.cur.opening && !s_c.cur.finalizing && now - s_c.lastData > kStallMs)
                ClientFail("host_stalled");
            else
                ClientPump();
            break;
        default:
            break;
        }

        if (s_rejoinPending && now >= s_rejoinAt)
        {
            s_rejoinPending = false;
            if (!CoSyncTransport::IsInitialized() && !CoSyncWorld::IsWorldReady())
            {
                LOG_INFO("[ContentSync] Rejoining '%s' after the content restart", s_rejoinLabel.c_str());
                char note[160];
                std::snprintf(note, sizeof(note), "Rejoining %s", s_rejoinLabel.empty() ? "the host" : s_rejoinLabel.c_str());
                CoSyncGameAPI::QueueNotification(note);
                if (!CoSyncPrismaUI::StartJoin(s_rejoinKind, s_rejoinValue, s_rejoinLabel))
                    LOG_WARN("[ContentSync] Rejoin could not start -- join from the Multiplayer menu");
            }
            else
                LOG_INFO("[ContentSync] Rejoin skipped: already in a session or a game");
        }
    }

    void QuitGame()
    {
        const uintptr_t fn = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) + kRVA_QuitGame;
        if (std::memcmp(reinterpret_cast<const void*>(fn), kQuitGamePrologue, sizeof(kQuitGamePrologue)) != 0)
        {
            LOG_ERROR("[ContentSync] Debug.QuitGame prologue mismatch (not 1.11.240?) -- quit the game yourself");
            CoSyncGameAPI::QueueNotification("Restart Fallout 4 to finish installing.");
            return;
        }
        LOG_INFO("[ContentSync] Quitting so the new content loads at the next start");
        reinterpret_cast<void (*)()>(fn)();
    }
}

namespace CoSyncContentSync
{
    bool IsContentMessage(const std::string& msg)
    {
        if (msg.size() < 6 || msg[0] != 'C' || msg[1] != 'S')
            return false;
        static const char* const kTypes[] = { "CSREQ|", "CSWAIT|", "CSFAIL|", "CSBIG|", "CSGET|", "CSDATA|", "CSERR|", "CSCANCEL|", "CSDONE|" };
        for (const char* t : kTypes)
            if (msg.compare(0, std::strlen(t), t) == 0)
                return true;
        return false;
    }

    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole)
    {
        if (isHostRole) HostOnReceive(msg, conn);
        else ClientOnReceive(msg);
    }

    void Tick()
    {
        std::deque<std::function<void()>> q;
        {
            std::lock_guard<std::mutex> lk(s_mainMx);
            q.swap(s_mainQ);
        }
        for (auto& fn : q)
            fn();

        if (CoSyncTransport::IsInitialized() && CoSyncTransport::IsHost())
            HostTick();
        ClientTick();
    }

    void ApplyPendingInstall()
    {
        try { ApplyPendingInstallNow(); }
        catch (...) { LOG_ERROR("[ContentSync] Startup: pending install threw -- skipped"); }
    }

    void EnsureOwnPluginEnabled()
    {
        try
        {
            const Paths& p = P();
            if (p.pluginsTxt.empty())
                return;
            if (GetFileAttributesW((p.data + L"\\CoSync.esp").c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                LOG_WARN("[ContentSync] Startup: CoSync.esp is not in Data -- reinstall CoSync");
                return;
            }
            std::string text;
            ReadWholeFile(p.pluginsTxt, text, 4u << 20);   // a missing file is an empty list
            bool changed = false;
            const std::string next = ActivateInPluginsTxt(text, { "CoSync.esp" }, {}, changed);
            if (!changed)
                return;
            if (WriteWholeFileAtomic(p.pluginsTxt, next))
                LOG_INFO("[ContentSync] Startup: CoSync.esp was not enabled -- enabled it in Plugins.txt");
            else
                LOG_WARN("[ContentSync] Startup: could not enable CoSync.esp in Plugins.txt -- enable it in your mod manager");
        }
        catch (...) { LOG_ERROR("[ContentSync] Startup: enabling CoSync.esp threw -- skipped"); }
    }

    void OnGameLoaded()
    {
        Disk().Post([]
        {
            const Paths& p = P();
            std::string r;
            if (!ReadWholeFile(p.rejoin, r, 4096))
                return;
            DeleteFileW(p.rejoin.c_str());   // one attempt only
            while (!r.empty() && (r.back() == '\n' || r.back() == '\r')) r.pop_back();
            const std::vector<std::string> f = Split(r, '|');
            uint64_t when = 0;
            if (f.size() != 5 || f[0] != "v1" || !ParseU64(f[4], when))
                return;
            const double age = static_cast<double>(std::time(nullptr)) - static_cast<double>(when);
            if (age < 0 || age > kRejoinMaxAgeSec)
            {
                LOG_INFO("[ContentSync] Rejoin note is %.0f s old -- ignored", age);
                return;
            }
            const std::string kind = f[1], value = f[2], label = f[3];
            PostMain([kind, value, label]
            {
                s_rejoinPending = true;
                s_rejoinAt = NowMs() + kRejoinDelayMs;
                s_rejoinKind = kind;
                s_rejoinValue = value;
                s_rejoinLabel = label;
                LOG_INFO("[ContentSync] Rejoin armed: %s '%s'", kind.c_str(), label.c_str());
            });
        });
    }

    void OnSessionEnded()
    {
        // Host side. The content list is rebuilt for the next hosting session
        // (cheap: unchanged files come from the hash cache).
        s_conns.clear();
        s_hostGen.fetch_add(1);
        s_hm = HostManifest();
        {
            std::lock_guard<std::mutex> lk(s_outMx);
            s_outbox.clear();
        }
        // Client side: an unfinished check/download stops (resumable); an
        // install finishes; terminal states stay for the menu.
        if (IsActivePhase(s_c.phase))
        {
            s_c.left = true;   // the transport is already gone
            ClientFail("connection_lost");
        }
        else if (s_c.phase == CPhase::Ready)
            s_c.phase = CPhase::Idle;
    }

    void ClientBegin()
    {
        const std::string kind = s_c.joinKind, value = s_c.joinValue, label = s_c.joinLabel;
        const uint64_t gen = s_c.gen + 1;
        s_c = Client();
        s_c.gen = gen;
        s_c.joinKind = kind; s_c.joinValue = value; s_c.joinLabel = label;
        s_c.phase = CPhase::Requesting;
        s_c.lastHeard = NowMs();
        s_checkDone.store(0);
        s_checkTotal.store(0);
        Disk().Post([] { WorkerClosePart(); });
        ClientSend("CSREQ|" + std::to_string(CoSyncNet::GetBuildNumber()) + "|" + std::to_string(kProtocolVersion));
        LOG_INFO("[ContentSync] Client: connected -- asking the host for its session content");
    }

    void NoteJoinTarget(const std::string& kind, const std::string& value, const std::string& label)
    {
        auto clean = [](std::string s) { for (char& c : s) if (c == '|' || c == '\n' || c == '\r') c = '/'; return s.substr(0, 120); };
        s_c.joinKind = clean(kind);
        s_c.joinValue = clean(value);
        s_c.joinLabel = clean(label);
    }

    void UiCommand(const std::string& arg)
    {
        if (arg == "accept")
        {
            if (s_c.phase != CPhase::Confirm) return;
            LOG_INFO("[ContentSync] Client: player accepted the download (%zu file(s), %s)", s_c.need.size(), FormatBytes(s_c.needBytes).c_str());
            s_c.phase = CPhase::Downloading;
            s_c.dlPos = 0;
            s_c.bytesCompleted = 0;
            s_c.staged.clear();
            s_c.cur = CurFile();
            ClientStartNextFile();
        }
        else if (arg == "cancel")
        {
            if (!IsActivePhase(s_c.phase)) return;
            LOG_INFO("[ContentSync] Client: player cancelled the content sync");
            ClientSend("CSCANCEL|player");
            ++s_c.gen;
            Disk().Post([] { WorkerClosePart(); });
            s_c.phase = CPhase::Cancelled;
            ClientLeave("content_cancelled");
        }
        else if (arg == "quit")
        {
            if (s_c.phase == CPhase::Restart)
                QuitGame();
        }
    }

    std::string BuildUiJson()
    {
        const Client& c = s_c;
        std::string j = "{\"phase\":\"";
        j += PhaseName(c.phase);
        j += "\",\"error\":\"" + JsonEscape(c.error) + "\",\"detail\":\"" + JsonEscape(c.errorDetail) + "\"";
        j += ",\"checkDone\":" + std::to_string(s_checkDone.load()) + ",\"checkTotal\":" + std::to_string(s_checkTotal.load());
        j += ",\"fileCount\":" + std::to_string(c.need.size()) + ",\"fileIndex\":" + std::to_string(c.dlPos);
        std::string file;
        uint64_t fileDone = 0, fileSize = 0;
        if (c.cur.active && c.cur.idx < c.m.entries.size())
        {
            file = c.m.entries[c.cur.idx].relPath;
            fileDone = c.cur.done;
            fileSize = c.cur.size;
        }
        j += ",\"file\":\"" + JsonEscape(file) + "\",\"fileDone\":" + std::to_string(fileDone) + ",\"fileSize\":" + std::to_string(fileSize);
        j += ",\"done\":" + std::to_string(c.bytesCompleted + fileDone) + ",\"total\":" + std::to_string(c.needBytes);
        j += ",\"scripts\":" + std::to_string(c.needScripts);
        j += ",\"missing\":[";
        for (size_t i = 0; i < c.need.size() && i < 60; ++i)
        {
            const Entry& e = c.m.entries[c.need[i]];
            if (i) j += ",";
            j += "{\"name\":\"" + JsonEscape(e.relPath) + "\",\"size\":" + std::to_string(e.size) +
                 ",\"kind\":\"" + std::string(1, static_cast<char>(e.kind)) + "\",\"here\":" +
                 (c.local.size() > c.need[i] && c.local[c.need[i]].exists ? "true" : "false") + "}";
        }
        j += "],\"activate\":[";
        for (size_t i = 0; i < c.activate.size() && i < 60; ++i)
            j += (i ? ",\"" : "\"") + JsonEscape(c.activate[i]) + "\"";
        j += "],\"blocked\":[";
        for (size_t i = 0; i < c.blocks.size() && i < 60; ++i)
            j += std::string(i ? "," : "") + "{\"code\":\"" + JsonEscape(c.blocks[i].first) + "\",\"name\":\"" + JsonEscape(c.blocks[i].second) + "\"}";
        j += "],\"warnings\":[";
        for (size_t i = 0; i < c.warns.size() && i < 60; ++i)
            j += std::string(i ? "," : "") + "{\"code\":\"" + JsonEscape(c.warns[i].first) + "\",\"name\":\"" + JsonEscape(c.warns[i].second) + "\"}";
        j += "],\"native\":[";
        for (size_t i = 0; i < c.nativeMissing.size() && i < 60; ++i)
            j += (i ? ",\"" : "\"") + JsonEscape(c.nativeMissing[i]) + "\"";
        j += "]}";
        return j;
    }

    bool HostIsConnVerified(uint32_t conn)
    {
        auto it = s_conns.find(conn);
        return it != s_conns.end() && it->second.st == HostSt::Verified;
    }

    bool HostAllowsMessage(uint32_t conn, const std::string& msg)
    {
        if (IsContentMessage(msg) || msg.rfind("DISCONNECT|", 0) == 0 || msg.rfind("WORLDREQ|", 0) == 0)
            return true;
        if (HostIsConnVerified(conn))
            return true;
        static uint32_t s_dropped = 0;
        if ((s_dropped++ % 100) == 0)
            LOG_WARN("[ContentSync] Join gate: dropped '%.24s' from conn=%u -- its content is not verified (%u dropped)",
                msg.c_str(), conn, s_dropped);
        return false;
    }
}
