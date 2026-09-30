// Offline end-to-end tests for CoSyncContentSync: the REAL host and client
// state machines (Networking/CoSyncContentSync.cpp, compiled with
// COSYNC_CONTENT_OFFLINE_TEST) talk to each other through an in-memory wire,
// each with its own game folder on disk. No Fallout 4, no GNS.
// Build + run: Tools\ContentSyncTests\run_tests.cmd
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncContentSync.h"
#include "CoSyncContentManifest.h"
#include "GNS_Session.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncWorldTransfer.h"
#include "CoSyncProfiles.h"
#include "CoSyncGameAPI.h"
#include "CoSyncPrismaUI.h"
#include "CoSyncWorld.h"
#include "GameData.h"

#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace CoSyncContent;

// ── Hooks exported by CoSyncContentSync.cpp in the test build ─────────────
extern std::mutex   g_contentTestMx;
extern std::wstring g_contentTestHostRoot, g_contentTestClientRoot;
extern uint64_t     g_contentTestFreeBytes;
extern std::vector<std::pair<std::string, bool>> g_contentTestHostLoaded, g_contentTestClientLoaded;

bool g_testVerboseLog = false;
RelocPtr<DataHandler*> g_dataHandler;

// ── In-memory wire + stubs ──────────────────────────────────────────────────
static std::deque<std::string> g_toHost, g_toClient;
static uint32_t g_conn = 7;
static bool g_hostConnected = true;
static bool g_transportUp = true;
static std::function<bool(std::string&)> g_tamperToClient, g_tamperToHost;   // false = drop
static bool g_worldRequested = false, g_left = false, g_kicked = false, g_rejoinStarted = false;
static int  g_leftReason = 0;
static std::string g_rejoinValue;

GNS_Session& GNS_Session::Get() { static GNS_Session s; return s; }
void GNS_Session::SendText(const std::string& t, bool)
{
    std::string m = t;
    if (g_tamperToHost && !g_tamperToHost(m)) return;
    g_toHost.push_back(m);
}
void GNS_Session::SendTextTo(HSteamNetConnection, const std::string& t, bool)
{
    std::string m = t;
    if (g_tamperToClient && !g_tamperToClient(m)) return;
    g_toClient.push_back(m);
}
void GNS_Session::LeaveWithReason(int reason, const char*) { g_left = true; g_leftReason = reason; }
void GNS_Session::KickClient(HSteamNetConnection, int, const char*) { g_kicked = true; }
bool GNS_Session::IsClientConnected(HSteamNetConnection) const { return g_hostConnected; }
uint32_t CoSyncNet::GetBuildNumber() { return 400; }
const char* CoSyncNet::GetMyName() { return "Tester"; }
bool CoSyncTransport::IsInitialized() { return g_transportUp; }
bool CoSyncTransport::IsHost() { return true; }   // runs HostTick; the client side needs no role check
void CoSyncWorldTransfer::ClientBegin(const std::string&, const std::string&) { g_worldRequested = true; }
std::string CoSyncProfiles::GetMyProfileKey() { return "key"; }
void CoSyncGameAPI::QueueNotification(const char*) {}
bool CoSyncPrismaUI::StartJoin(const std::string&, const std::string& value, const std::string&) { g_rejoinStarted = true; g_rejoinValue = value; return true; }
bool CoSyncWorld::IsWorldReady() { return g_hostConnected; }   // host in game; the rejoin check wants "not in a world"

// ── Test plumbing ───────────────────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;
#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::string Json() { return CoSyncContentSync::BuildUiJson(); }
static std::string JStr(const char* key)
{
    const std::string j = Json(), k = std::string("\"") + key + "\":\"";
    const size_t a = j.find(k);
    if (a == std::string::npos) return std::string();
    const size_t b = j.find('"', a + k.size());
    return j.substr(a + k.size(), b - a - k.size());
}
static long long JNum(const char* key)
{
    const std::string j = Json(), k = std::string("\"") + key + "\":";
    const size_t a = j.find(k);
    return a == std::string::npos ? -1 : std::atoll(j.c_str() + a + k.size());
}
static std::string Phase() { return JStr("phase"); }

static bool Pump(const std::function<bool()>& done, DWORD ms = 30000)
{
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < ms)
    {
        while (!g_toHost.empty()) { std::string m = g_toHost.front(); g_toHost.pop_front(); CoSyncContentSync::OnReceive(m, g_conn, true); }
        while (!g_toClient.empty()) { std::string m = g_toClient.front(); g_toClient.pop_front(); CoSyncContentSync::OnReceive(m, 0, false); }
        CoSyncContentSync::Tick();
        if (done()) return true;
        Sleep(1);
    }
    return false;
}
static bool PumpPhase(const char* phase, DWORD ms = 30000) { return Pump([&] { return Phase() == phase; }, ms); }

// Runs Tick (host sends, worker results) WITHOUT delivering messages, so a
// test can look at what the host put on the wire.
static void TickOnly(DWORD ms)
{
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < ms) { CoSyncContentSync::Tick(); Sleep(1); }
}
static bool ToClientHas(const std::string& exact)
{
    for (const std::string& m : g_toClient) if (m == exact) return true;
    return false;
}

static std::string RandBytes(size_t n, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>(rng() & 0xFF);
    return s;
}

static std::string Plugin(uint32_t seed, size_t extra, uint32_t flags = 0)
{
    std::string rec;
    auto sub = [&](const char* type, const std::string& d)
    {
        rec.append(type, 4);
        const uint16_t sz = static_cast<uint16_t>(d.size());
        rec.append(reinterpret_cast<const char*>(&sz), 2);
        rec += d;
    };
    sub("HEDR", std::string(12, '\0'));
    sub("MAST", std::string("Fallout4.esm\0", 13));
    sub("DATA", std::string(8, '\0'));
    std::string f = "TES4";
    const uint32_t ds = static_cast<uint32_t>(rec.size());
    f.append(reinterpret_cast<const char*>(&ds), 4);
    f.append(reinterpret_cast<const char*>(&flags), 4);
    f += std::string(12, '\0');
    return f + rec + RandBytes(extra, seed);
}

static void Put(const fs::path& p, const std::string& data)
{
    fs::create_directories(p.parent_path());
    FILE* f = nullptr;
    _wfopen_s(&f, p.c_str(), L"wb");
    if (f) { fwrite(data.data(), 1, data.size(), f); fclose(f); }
}
static std::string Get(const fs::path& p)
{
    std::string s;
    ReadWholeFile(p.wstring(), s, 1u << 30);
    return s;
}
static std::string Sha(const fs::path& p)
{
    std::string sha; uint64_t sz = 0;
    return HashFile(p.wstring(), sha, sz) ? sha : std::string("<unreadable>");
}

struct Env
{
    fs::path host, client;
    fs::path hd() const { return host / "Data"; }
    fs::path cd() const { return client / "Data"; }
};

static std::string JsonPath(const fs::path& p)
{
    std::string s = Narrow(p.wstring()), out;
    for (char c : s) { if (c == '\\') out += "\\\\"; else out += c; }
    return out;
}

// The host's installation: two mods (one with archives and a loose texture
// deployed by Vortex, one plain plugin), the base game, a native plugin.
static Env Setup(const char* name, size_t texturesBytes = 1u << 20)
{
    Env e;
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const fs::path base = fs::path(tmp) / L"cosync_sync_tests" / name;
    std::error_code ec;
    fs::remove_all(base, ec);
    e.host = base / "host";
    e.client = base / "client";
    fs::create_directories(e.hd());
    fs::create_directories(e.cd());
    fs::create_directories(e.host / "UserDir");
    fs::create_directories(e.client / "UserDir");

    const std::string fo4 = Plugin(1, 200000, 1);
    Put(e.hd() / "Fallout4.esm", fo4);
    Put(e.hd() / "HOWA.esp", Plugin(2, 100000));
    Put(e.hd() / "HOWA - Main.ba2", RandBytes(3500000, 3));
    Put(e.hd() / "HOWA - Textures.ba2", RandBytes(texturesBytes, 4));
    Put(e.hd() / "Mod2.esp", Plugin(5, 50000));
    Put(e.hd() / "Textures" / "HOWA" / "a_d.dds", RandBytes(300000, 6));
    Put(e.hd() / "Textures" / "Replacer" / "x.dds", RandBytes(1000, 7));
    Put(e.hd() / "F4SE" / "Plugins" / "hostonly.dll", RandBytes(1000, 8));
    const std::string vortex = std::string("{\"gameId\":\"fallout4\",\"targetPath\":\"") + JsonPath(e.hd()) + "\",\"files\":["
        "{\"relPath\":\"HOWA.esp\",\"source\":\"HOWA 20\"},"
        "{\"relPath\":\"HOWA - Main.ba2\",\"source\":\"HOWA 20\"},"
        "{\"relPath\":\"Textures\\\\HOWA\\\\a_d.dds\",\"source\":\"HOWA 20\"},"
        "{\"relPath\":\"F4SE\\\\Plugins\\\\hostonly.dll\",\"source\":\"HOWA 20\"},"
        "{\"relPath\":\"Textures\\\\Replacer\\\\x.dds\",\"source\":\"Replacer\"}]}";
    Put(e.hd() / "vortex.deployment.json", vortex);
    g_contentTestHostLoaded = { { "Fallout4.esm", false }, { "HOWA.esp", false }, { "Mod2.esp", false } };

    // The client: same base game, an OLD Mod2.esp, an extra mod of its own.
    Put(e.cd() / "Fallout4.esm", fo4);
    Put(e.cd() / "Mod2.esp", Plugin(55, 40000));
    Put(e.cd() / "Extra.esp", Plugin(9, 1000));
    Put(e.client / "UserDir" / "Plugins.txt", "# This file is used by Fallout 4 to keep track of your downloaded content.\r\n*Mod2.esp\r\n*Extra.esp\r\n");
    g_contentTestClientLoaded = { { "Fallout4.esm", false }, { "Mod2.esp", false }, { "Extra.esp", false } };

    {
        std::lock_guard<std::mutex> lk(g_contentTestMx);
        g_contentTestHostRoot = e.host.wstring();
        g_contentTestClientRoot = e.client.wstring();
    }
    g_contentTestFreeBytes = 0;
    return e;
}

static void NewSession(uint32_t conn)
{
    CoSyncContentSync::OnSessionEnded();
    Pump([] { return false; }, 50);   // let stale worker results drain
    g_toHost.clear();
    g_toClient.clear();
    g_tamperToClient = nullptr;
    g_tamperToHost = nullptr;
    g_conn = conn;
    g_hostConnected = true;
    g_transportUp = true;
    g_worldRequested = g_left = g_kicked = false;
    g_leftReason = 0;
    CoSyncContentSync::NoteJoinTarget("ip", "10.0.0.5:48000", "Host's Game");
    CoSyncContentSync::ClientBegin();
}

static bool SameAsHost(const Env& e, const char* rel) { return Sha(e.cd() / rel) == Sha(e.hd() / rel); }

// ── Scenarios ───────────────────────────────────────────────────────────────

// 2/3/4/5/11/12: missing plugins, missing archives, a different version, a
// file that already matches, multiple mods -> download, restart-install, join.
static void TestFullFlow()
{
    std::printf("[full flow: missing + different + matching, multiple mods, install at startup, rejoin, join]\n");
    Env e = Setup("full");
    NewSession(7);
    CHECK(PumpPhase("confirm"));
    CHECK(JNum("fileCount") == 5);   // HOWA.esp, HOWA - Main/Textures.ba2, Textures\HOWA\a_d.dds, Mod2.esp
    const std::string j = Json();
    CHECK(j.find("\"HOWA.esp\"") != std::string::npos);                 // listed and to be enabled
    CHECK(j.find("Fallout4.esm") == std::string::npos);                 // already matches -> not listed
    CHECK(j.find("x.dds") == std::string::npos);                        // replacer of another mod: not session content
    CHECK(j.find("\"native\":[\"hostonly.dll\"]") != std::string::npos);// native plugin: listed, not sent
    CHECK(j.find("\"code\":\"extra\",\"name\":\"extra.esp\"") != std::string::npos);
    CHECK(!g_worldRequested);

    CoSyncContentSync::UiCommand("accept");
    CHECK(PumpPhase("restart"));
    CHECK(g_left && g_leftReason == 1003);
    // Nothing is placed while the game runs: files wait, verified, in staging.
    CHECK(!fs::exists(e.cd() / "HOWA.esp"));
    CHECK(fs::exists(e.cd() / "CoSyncContent" / "pending.txt"));
    CHECK(fs::exists(e.cd() / "CoSyncContent" / "rejoin.txt"));
    CHECK(!fs::exists(e.cd() / "F4SE" / "Plugins" / "hostonly.dll"));
    const std::string oldMod2 = Get(e.cd() / "Mod2.esp");

    // "Restart": F4SEPlugin_Load applies the install.
    CoSyncContentSync::ApplyPendingInstall();
    CHECK(SameAsHost(e, "HOWA.esp"));
    CHECK(SameAsHost(e, "HOWA - Main.ba2"));
    CHECK(SameAsHost(e, "HOWA - Textures.ba2"));
    CHECK(SameAsHost(e, "Textures\\HOWA\\a_d.dds"));
    CHECK(SameAsHost(e, "Mod2.esp"));
    CHECK(SameAsHost(e, "Fallout4.esm"));
    CHECK(fs::exists(e.cd() / "Extra.esp"));                             // the player's own mod is untouched
    CHECK(!fs::exists(e.cd() / "F4SE" / "Plugins" / "hostonly.dll"));
    CHECK(!fs::exists(e.cd() / "Textures" / "Replacer" / "x.dds"));
    CHECK(!fs::exists(e.cd() / "CoSyncContent" / "pending.txt"));
    CHECK(!fs::exists(e.cd() / "CoSyncContent" / "install.journal"));
    bool backedUp = false;
    for (auto& it : fs::recursive_directory_iterator(e.cd() / "CoSyncContent" / "backup"))
        if (it.path().filename() == "Mod2.esp" && Get(it.path()) == oldMod2) backedUp = true;
    CHECK(backedUp);                                                      // replaced file kept, not deleted
    const std::string ptxt = Get(e.client / "UserDir" / "Plugins.txt");
    CHECK(ptxt.find("*HOWA.esp\r\n") != std::string::npos && ptxt.find("*Extra.esp") != std::string::npos);

    // Game reaches the main menu: the rejoin is armed and fires ~5 s later.
    g_hostConnected = false;   // "not in a world" for the rejoin check
    g_transportUp = false;     // and not in a session
    g_rejoinStarted = false;
    CoSyncContentSync::OnGameLoaded();
    CHECK(Pump([] { return g_rejoinStarted; }, 9000));
    CHECK(g_rejoinValue == "10.0.0.5:48000");
    CHECK(!fs::exists(e.cd() / "CoSyncContent" / "rejoin.txt"));

    // 12: the rejoin -- the engine now loads HOWA; the host verifies; world.
    g_contentTestClientLoaded = { { "Fallout4.esm", false }, { "HOWA.esp", false }, { "Mod2.esp", false }, { "Extra.esp", false } };
    NewSession(8);
    CHECK(PumpPhase("ready"));
    CHECK(g_worldRequested);
    CHECK(CoSyncContentSync::HostIsConnVerified(8));
    CHECK(CoSyncContentSync::HostAllowsMessage(8, "HELLO|x|1|0|0|0"));
    CHECK(!CoSyncContentSync::HostAllowsMessage(9, "HELLO|x|1|0|0|0"));   // join gate
    CHECK(CoSyncContentSync::HostAllowsMessage(9, "CSREQ|400|1"));
}

// 1: identical content -> straight to the world, no prompt, nothing written.
static void TestIdentical()
{
    std::printf("[identical content]\n");
    Env e = Setup("identical");
    for (const char* f : { "HOWA.esp", "HOWA - Main.ba2", "HOWA - Textures.ba2", "Mod2.esp" })
        fs::copy_file(e.hd() / f, e.cd() / f, fs::copy_options::overwrite_existing);
    fs::create_directories(e.cd() / "Textures" / "HOWA");
    fs::copy_file(e.hd() / "Textures" / "HOWA" / "a_d.dds", e.cd() / "Textures" / "HOWA" / "a_d.dds", fs::copy_options::overwrite_existing);
    g_contentTestClientLoaded = g_contentTestHostLoaded;
    NewSession(7);
    CHECK(PumpPhase("ready"));
    CHECK(g_worldRequested);
    CHECK(!fs::exists(e.cd() / "CoSyncContent" / "pending.txt"));
}

// 6 + 10: a large archive, the connection drops mid-download, the rejoin
// resumes from the saved point instead of starting over.
static void TestInterruptResume()
{
    std::printf("[large file, interrupted transfer, resume]\n");
    Env e = Setup("resume", 48u << 20);
    NewSession(7);
    CHECK(PumpPhase("confirm"));
    CoSyncContentSync::UiCommand("accept");
    // Drop the "connection" once 24 MB of the textures archive went through.
    const ULONGLONG t0 = GetTickCount64();
    CHECK(Pump([] { return JStr("file") == "HOWA - Textures.ba2" && JNum("fileDone") >= (24ll << 20); }, 60000));
    std::printf("      48 MB archive: 24 MB in %llu ms\n", GetTickCount64() - t0);
    CoSyncContentSync::OnSessionEnded();
    CHECK(Phase() == "failed" && JStr("error") == "connection_lost");
    Pump([] { return false; }, 200);
    // The resume point on disk (written every 16 MB, after a flush).
    bool metaOk = false;
    for (auto& it : fs::directory_iterator(e.cd() / "CoSyncContent" / "staging"))
        if (it.path().extension() == ".meta")
        {
            const std::string m = Get(it.path());
            if (m.find("|16777216") != std::string::npos) metaOk = true;
        }
    CHECK(metaOk);

    uint64_t requested = 0;
    NewSession(8);
    g_tamperToHost = [&](std::string& m)
    {
        if (m.rfind("CSGET|", 0) == 0) requested += std::strtoull(m.c_str() + m.rfind('|') + 1, nullptr, 10);
        return true;
    };
    CHECK(PumpPhase("confirm"));
    CoSyncContentSync::UiCommand("accept");
    CHECK(PumpPhase("restart", 60000));
    const uint64_t total = static_cast<uint64_t>(JNum("total"));
    std::printf("      second session requested %llu of %llu bytes\n", (unsigned long long)requested, (unsigned long long)total);
    CHECK(requested < total && total - requested >= (16ull << 20) - kChunkBytes);   // earlier files done + 16 MB resumed
    CoSyncContentSync::ApplyPendingInstall();
    CHECK(SameAsHost(e, "HOWA - Textures.ba2"));
}

// 7: a damaged chunk is detected (CRC) and fetched again; a host that sends
// different bytes with a matching CRC is caught by the SHA-256 and nothing
// is installed.
static void TestCorruption()
{
    std::printf("[corrupted chunk -> re-request]\n");
    Env e = Setup("corrupt");
    NewSession(7);
    int n = 0, damaged = 0;
    g_tamperToClient = [&](std::string& m)
    {
        if (m.rfind("CSDATA|", 0) == 0 && ++n == 3)
        {
            m.back() ^= 0x5A;   // payload damaged, header CRC untouched
            ++damaged;
        }
        return true;
    };
    CHECK(PumpPhase("confirm"));
    CoSyncContentSync::UiCommand("accept");
    CHECK(PumpPhase("restart"));
    CHECK(damaged == 1);
    CoSyncContentSync::ApplyPendingInstall();
    CHECK(SameAsHost(e, "HOWA - Main.ba2") && SameAsHost(e, "HOWA.esp"));

    std::printf("[host sends wrong bytes with a valid CRC -> SHA-256 mismatch, nothing installed]\n");
    Env e2 = Setup("lyinghost");
    NewSession(7);
    g_tamperToClient = [&](std::string& m)
    {
        if (m.rfind("CSDATA|", 0) != 0) return true;
        // Recompute the CRC over altered bytes: only the file hash can tell.
        size_t pos = 0;
        for (int i = 0; i < 4; ++i) pos = m.find('|', pos) + 1;
        std::string data = m.substr(pos);
        data[0] ^= 0x01;
        char crc[16]; std::snprintf(crc, sizeof(crc), "%08x", Crc32(data.data(), data.size()));
        const size_t c0 = m.rfind('|', pos - 2) + 1;
        m = m.substr(0, c0) + crc + "|" + data;
        return true;
    };
    CHECK(PumpPhase("confirm"));
    CoSyncContentSync::UiCommand("accept");
    CHECK(PumpPhase("failed"));
    CHECK(JStr("error") == "hash_mismatch");
    CHECK(!fs::exists(e2.cd() / "CoSyncContent" / "pending.txt"));
    CHECK(!fs::exists(e2.cd() / "HOWA.esp"));
}

// 8: not enough disk space -> refused before a byte is written.
static void TestDiskSpace()
{
    std::printf("[insufficient disk space]\n");
    Env e = Setup("disk");
    g_contentTestFreeBytes = 1000;
    NewSession(7);
    CHECK(PumpPhase("failed"));
    CHECK(JStr("error") == "disk_space");
    CHECK(!fs::exists(e.cd() / "CoSyncContent" / "staging") || fs::is_empty(e.cd() / "CoSyncContent" / "staging"));
    g_contentTestFreeBytes = 0;
}

// 9: malicious or broken input from the host, the client, or the disk.
static void TestHostile()
{
    std::printf("[hostile manifest paths]\n");
    for (const char* evil : {
             "C|M|10|%s|..\\..\\evil.dll|HOWA.esp",
             "L|M|10|%s|F4SE\\Plugins\\evil.dll|HOWA.esp",
             "L|M|10|%s|C:\\Windows\\evil.dds|HOWA.esp",
             "L|M|10|%s|PrismaUI_F4\\views\\DoxCoSync\\multiplayer.js|HOWA.esp",
             "P|M|10|%s|Evil.exe|0|0|" })
    {
        Env e = Setup("evil");
        NewSession(7);
        char line[300];
        std::snprintf(line, sizeof(line), evil, std::string(64, 'a').c_str());
        const std::string man = "NONCE|00112233445566778899aabbccddeeff\nV|1|400\nP|M|10|" + std::string(64, 'b') + "|HOWA.esp|0|0|\n" + line + "\nE\n";
        g_toHost.clear();
        CoSyncContentSync::OnReceive("CSBIG|MAN|0|1|" + man, 0, false);
        Pump([] { return Phase() == "failed"; }, 3000);
        CHECK(Phase() == "failed" && JStr("error") == "bad_manifest");
        CHECK(!fs::exists(e.cd() / "CoSyncContent" / "staging"));
        g_toClient.clear();
    }

    std::printf("[host asks to send official content]\n");
    {
        Env e = Setup("official_push");
        NewSession(7);
        g_tamperToClient = [](std::string& m)
        {
            if (m.rfind("CSBIG|VER|", 0) == 0) m = "CSBIG|VER|0|1|S|NEED\nT|0\nE\n";   // entry 0 = Fallout4.esm
            return true;
        };
        CHECK(PumpPhase("failed"));
        CHECK(JStr("error") == "refused_official");
    }

    std::printf("[client lies about having a file]\n");
    {
        Env e = Setup("liar");
        NewSession(7);
        g_tamperToHost = [](std::string& m)
        {
            if (m.rfind("CSBIG|REP|", 0) == 0)
            {
                // Claim entry 1 (HOWA.esp) matches, with a made-up proof.
                const size_t a = m.find("R|1|X|-");
                if (a != std::string::npos) m.replace(a, 7, "R|1|M|" + std::string(64, 'e'));
            }
            return true;
        };
        CHECK(PumpPhase("confirm"));
        CHECK(Json().find("\"name\":\"HOWA.esp\"") != std::string::npos);   // host still sends it
        CHECK(!CoSyncContentSync::HostIsConnVerified(7));
    }

    std::printf("[client requests chunks it was not granted / floods]\n");
    {
        Env e = Setup("badget");
        NewSession(7);
        CHECK(PumpPhase("confirm"));
        g_toClient.clear();
        // Entry 0 (Fallout4.esm) matches, so it is not granted.
        CoSyncContentSync::OnReceive("CSGET|0|0|1024", g_conn, true);
        CoSyncContentSync::OnReceive("CSGET|1|999999999|1024", g_conn, true);   // past the end
        CoSyncContentSync::OnReceive("CSGET|1|0|9999999", g_conn, true);        // over the chunk limit
        CoSyncContentSync::OnReceive("CSGET|junk", g_conn, true);
        TickOnly(300);
        bool anyData = false;
        for (const std::string& m : g_toClient) if (m.rfind("CSDATA|", 0) == 0) anyData = true;
        CHECK(!anyData);
        // One granted request IS served (the check above is not vacuous).
        CoSyncContentSync::OnReceive("CSGET|1|0|1024", g_conn, true);
        TickOnly(300);
        for (const std::string& m : g_toClient) if (m.rfind("CSDATA|1|0|", 0) == 0) anyData = true;
        CHECK(anyData);
        for (int i = 0; i < 40; ++i) CoSyncContentSync::OnReceive("CSGET|1|0|1024", g_conn, true);
        CHECK(ToClientHas("CSFAIL|flood"));
    }

    std::printf("[malformed messages]\n");
    {
        Env e = Setup("malformed");
        NewSession(7);
        CHECK(PumpPhase("waiting") || PumpPhase("confirm"));
        for (const char* m : { "CSBIG|", "CSBIG|MAN|5|2|x", "CSBIG|ZZZ|0|1|x", "CSDATA|", "CSDATA|1|2|3", "CSDATA|99999999999999999999|0|00000000|x",
                               "CSWAIT|a|b", "CSERR|", "CSFAIL|" })
            CoSyncContentSync::OnReceive(m, 0, false);
        for (const char* m : { "CSREQ|", "CSREQ|abc|1", "CSBIG|REP|0|99999|x", "CSCANCEL|", "CSDATA|1|0|0|x" })
            CoSyncContentSync::OnReceive(m, 11, true);
        CHECK(true);   // reaching here = no crash
    }

    std::printf("[wrong build]\n");
    {
        Env e = Setup("build");
        NewSession(7);
        g_toHost.clear();
        g_toClient.clear();
        CoSyncContentSync::OnReceive("CSREQ|306|1", g_conn, true);
        CHECK(ToClientHas("CSFAIL|version_mismatch"));
    }

    std::printf("[tampered pending install list]\n");
    {
        Env e = Setup("pending");
        fs::create_directories(e.cd() / "CoSyncContent" / "staging");
        Put(e.cd() / "CoSyncContent" / "staging" / "x.part", "data");
        Put(e.client / "victim.txt", "keep");
        const std::string staged = Narrow((e.cd() / "CoSyncContent" / "staging" / "x.part").wstring());
        Put(e.cd() / "CoSyncContent" / "pending.txt",
            "v1\nF|" + staged + "|..\\victim.txt|4|0|" + std::string(64, 'a') + "\nE\n");
        CoSyncContentSync::ApplyPendingInstall();
        CHECK(Get(e.client / "victim.txt") == "keep");
        CHECK(fs::exists(e.cd() / "CoSyncContent" / "staging" / "x.part"));
        Put(e.cd() / "CoSyncContent" / "pending.txt",
            "v1\nF|C:\\Windows\\win.ini|Evil.esp|4|0|" + std::string(64, 'a') + "\nE\n");
        CoSyncContentSync::ApplyPendingInstall();
        CHECK(!fs::exists(e.cd() / "Evil.esp"));
        // A staged file changed after verification is not installed.
        uint64_t sz = 0, mt = 0;
        GetFileSizeTime((e.cd() / "CoSyncContent" / "staging" / "x.part").wstring(), sz, mt);
        Put(e.cd() / "CoSyncContent" / "pending.txt",
            "v1\nF|" + staged + "|Good.esp|4|" + std::to_string(mt + 1) + "|" + std::string(64, 'a') + "\nE\n");
        CoSyncContentSync::ApplyPendingInstall();
        CHECK(!fs::exists(e.cd() / "Good.esp"));
    }
}

// Interrupted install (crash between steps): the journal is rolled back.
static void TestJournalRollback()
{
    std::printf("[interrupted install rolled back at startup]\n");
    Env e = Setup("journal");
    fs::create_directories(e.cd() / "CoSyncContent" / "backup" / "t");
    Put(e.cd() / "CoSyncContent" / "backup" / "t" / "Mod2.esp", "OLD");
    fs::remove(e.cd() / "Mod2.esp");
    Put(e.cd() / "New.esp", "NEW");
    const std::string dest = Narrow((e.cd() / "Mod2.esp").wstring());
    const std::string bak = Narrow((e.cd() / "CoSyncContent" / "backup" / "t" / "Mod2.esp").wstring());
    const std::string ndest = Narrow((e.cd() / "New.esp").wstring());
    const std::string nstaged = Narrow((e.cd() / "CoSyncContent" / "staging" / "n.part").wstring());
    fs::create_directories(e.cd() / "CoSyncContent" / "staging");
    Put(e.cd() / "CoSyncContent" / "install.journal", "B|" + dest + "|" + bak + "\nI|" + ndest + "|" + nstaged + "\n");
    CoSyncContentSync::ApplyPendingInstall();
    CHECK(Get(e.cd() / "Mod2.esp") == "OLD");                 // original restored
    CHECK(!fs::exists(e.cd() / "New.esp"));                    // half-placed file taken back
    CHECK(Get(e.cd() / "CoSyncContent" / "staging" / "n.part") == "NEW");
    CHECK(!fs::exists(e.cd() / "CoSyncContent" / "install.journal"));
}

// Official content the host has and the client lacks: blocked, never sent.
static void TestOfficialMissing()
{
    std::printf("[official DLC missing on the client -> blocked]\n");
    Env e = Setup("dlc");
    Put(e.hd() / "DLCCoast.esm", Plugin(10, 5000, 1));
    g_contentTestHostLoaded.insert(g_contentTestHostLoaded.begin() + 1, { "DLCCoast.esm", false });
    NewSession(7);
    CHECK(PumpPhase("blocked"));
    CHECK(Json().find("\"code\":\"official_missing\",\"name\":\"DLCCoast.esm\"") != std::string::npos);
    CHECK(g_left);
    CHECK(!fs::exists(e.cd() / "DLCCoast.esm"));
}

// Present but disabled: only Plugins.txt changes; and if Plugins.txt already
// enables it yet the engine did not load it, a mod manager is in charge.
static void TestActivation()
{
    std::printf("[files present, plugin not enabled -> enable only]\n");
    Env e = Setup("activate");
    for (const char* f : { "HOWA.esp", "HOWA - Main.ba2", "HOWA - Textures.ba2", "Mod2.esp" })
        fs::copy_file(e.hd() / f, e.cd() / f, fs::copy_options::overwrite_existing);
    fs::create_directories(e.cd() / "Textures" / "HOWA");
    fs::copy_file(e.hd() / "Textures" / "HOWA" / "a_d.dds", e.cd() / "Textures" / "HOWA" / "a_d.dds");
    NewSession(7);
    CHECK(PumpPhase("restart"));
    CoSyncContentSync::ApplyPendingInstall();
    CHECK(Get(e.client / "UserDir" / "Plugins.txt").find("*HOWA.esp") != std::string::npos);

    std::printf("[enabled in Plugins.txt but not loaded -> mod manager warning]\n");
    NewSession(8);   // engine still does not load HOWA (e.g. MO2 profile)
    CHECK(PumpPhase("failed"));
    CHECK(JStr("error") == "not_loaded");
}

// Cancel mid-download: clean stop, nothing installed, host notified.
static void TestCancel()
{
    std::printf("[player cancels]\n");
    Env e = Setup("cancel", 16u << 20);
    NewSession(7);
    CHECK(PumpPhase("confirm"));
    CoSyncContentSync::UiCommand("accept");
    CHECK(PumpPhase("downloading"));
    CoSyncContentSync::UiCommand("cancel");
    CHECK(Phase() == "cancelled" && g_left);
    bool sawCancel = false;
    for (const std::string& m : g_toHost) if (m.rfind("CSCANCEL|", 0) == 0) sawCancel = true;
    CHECK(sawCancel);
    Pump([] { return false; }, 200);
    CHECK(!fs::exists(e.cd() / "CoSyncContent" / "pending.txt"));
}

int main(int argc, char** argv)
{
    g_testVerboseLog = argc > 1 && std::string(argv[1]) == "-v";
    TestFullFlow();
    TestIdentical();
    TestInterruptResume();
    TestCorruption();
    TestDiskSpace();
    TestHostile();
    TestJournalRollback();
    TestOfficialMissing();
    TestActivation();
    TestCancel();
    std::printf("\nSyncTests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
