#include "CoSyncWorldTransfer.h"

#include "CoSyncSaveLoad.h"
#include "CoSyncSpawnTasks.h"
#include "CoSyncTransport.h"
#include "CoSyncNet.h"
#include "CoSyncWorld.h"
#include "CoSyncProfiles.h"
#include "CoSyncGameAPI.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncContentSync.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_set>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    constexpr const char* kHostSaveName   = "CoSync_World";
    constexpr const char* kClientSaveName = "CoSync_Joined";
    constexpr size_t kChunkBytes     = 128 * 1024;        // well under GNS's 512 KB message cap
    constexpr size_t kBytesPerTick   = 1024 * 1024;       // host send budget per frame
    constexpr size_t kMaxFileBytes   = 256 * 1024 * 1024; // sanity cap on a declared size

    // ── CRC32 (IEEE) ────────────────────────────────────────────────────────
    uint32_t Crc32(const std::vector<uint8_t>& data)
    {
        static uint32_t table[256];
        static bool init = false;
        if (!init)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            init = true;
        }
        uint32_t crc = 0xFFFFFFFFu;
        for (uint8_t b : data)
            crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }

    // Files are read/written through the CRT on purpose: the multi-instance
    // test proxy redirects "\Saves\" paths by patching the CRT's CreateFileW,
    // so CRT I/O lands in the same folder the engine uses.
    bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
    {
        out.clear();
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") != 0 || !f)
            return false;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size < 0 || static_cast<size_t>(size) > kMaxFileBytes) { std::fclose(f); return false; }
        out.resize(static_cast<size_t>(size));
        const size_t n = size ? std::fread(out.data(), 1, out.size(), f) : 0;
        std::fclose(f);
        return n == out.size();
    }

    bool WriteFile(const std::string& path, const std::vector<uint8_t>& data)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "wb") != 0 || !f)
            return false;
        const size_t n = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
        return n == data.size();
    }

    std::vector<std::string> SplitHeader(const std::string& msg, size_t fields, size_t& payloadOffset)
    {
        std::vector<std::string> out;
        size_t pos = 0;
        payloadOffset = std::string::npos;
        for (size_t i = 0; i < fields; ++i)
        {
            const size_t bar = msg.find('|', pos);
            if (bar == std::string::npos)
            {
                out.push_back(msg.substr(pos));
                return out;
            }
            out.push_back(msg.substr(pos, bar - pos));
            pos = bar + 1;
        }
        payloadOffset = pos;
        return out;
    }

    bool ParseU64(const std::string& s, uint64_t& v)
    {
        if (s.empty() || s.size() > 20) return false;
        v = 0;
        for (char c : s) { if (c < '0' || c > '9') return false; v = v * 10 + (c - '0'); }
        return true;
    }

    // ── Host state ──────────────────────────────────────────────────────────
    struct Outgoing
    {
        uint32_t conn = 0;
        uint32_t id = 0;
        std::vector<uint8_t> files[2];
        size_t   fileIdx = 0;
        size_t   offset = 0;
        bool     begun = false;
    };
    std::deque<uint32_t> s_pendingRequests;   // conns waiting for a fresh save
    bool                 s_saveQueued = false;
    std::vector<Outgoing> s_outgoing;
    uint32_t             s_nextId = 1;
    // S-SEC2 (2026-09-26): a client requests the world exactly once per
    // connection (CoSyncNet::OnGNSConnected -> ClientBegin); a reconnect is a
    // new connection handle. Every extra WORLDREQ used to queue another full
    // host save plus a multi-MB transfer, so one peer could spam the host.
    std::unordered_set<uint32_t> s_worldRequestedConns;

    // ── Client state ────────────────────────────────────────────────────────
    std::atomic<int> s_phase{ static_cast<int>(CoSyncWorldTransfer::ClientPhase::Idle) };
    std::atomic<int> s_percent{ 0 };
    std::mutex       s_errMutex;
    std::string      s_error;

    std::atomic<bool> s_inHostWorld{ false };   // see IsInHostWorld
    bool              s_returnToMenu = false;   // session ended while in the host's world

    // Papyrus Game.QuitToMainMenu native (registered at 0x1411263D7): static,
    // no arguments -- queues GameScript's FadeThenMainMenuCallback, the same
    // fade-out-to-main-menu scripts use. IDA 1.11.240, 2026-09-26.
    constexpr uintptr_t kRVA_QuitToMainMenu = 0x01123870;
    constexpr uint8_t   kQuitToMainMenuPrologue[10] = { 0x48, 0x83, 0xEC, 0x28, 0x83, 0x3D, 0x15, 0x24, 0xD5, 0x02 };

    void QuitToMainMenu()
    {
        const uintptr_t fn = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) + kRVA_QuitToMainMenu;
        if (std::memcmp(reinterpret_cast<const void*>(fn), kQuitToMainMenuPrologue, sizeof(kQuitToMainMenuPrologue)) != 0)
        {
            LOG_ERROR("[WorldXfer] QuitToMainMenu prologue mismatch (not 1.11.240?) -- staying in the world");
            return;
        }
        LOG_INFO("[WorldXfer] Session ended -- returning to the main menu");
        reinterpret_cast<void (*)()>(fn)();
    }

    double   s_loadStartedMs = 0.0;   // client: when LoadNamed was issued
    constexpr double kLoadTimeoutMs = 90000.0;

    uint32_t s_inId = 0;
    std::vector<uint8_t> s_inFiles[2];
    uint64_t s_inSize[2] = { 0, 0 };
    uint32_t s_inCrc[2] = { 0, 0 };
    uint64_t s_inReceived = 0;

    void SetPhase(CoSyncWorldTransfer::ClientPhase p) { s_phase.store(static_cast<int>(p)); }

    void Fail(const char* reason)
    {
        {
            std::lock_guard<std::mutex> lk(s_errMutex);
            s_error = reason ? reason : "unknown";
        }
        SetPhase(CoSyncWorldTransfer::ClientPhase::Failed);
        LOG_ERROR("[WorldXfer] Client world transfer failed: %s", reason ? reason : "unknown");
    }

    // Host: runs as an F4SE main-thread task (the same point the console
    // saves from). Saves the world, then queues it for every waiting client.
    void HostSaveAndQueue()
    {
        LOG_REQUIREMENT("(anonymous)::HostSaveAndQueue", "Main thread (F4SE task).");
        s_saveQueued = false;
        if (s_pendingRequests.empty())
            return;

        // Everyone's latest character goes into the co-save of this save.
        CoSyncProfiles::HostBeforeWorldSave();

        if (!CoSyncSaveLoad::SaveNamed(kHostSaveName))
        {
            for (uint32_t conn : s_pendingRequests)
                CoSyncTransport::SendTo(conn, "WORLDERR|save_failed");
            s_pendingRequests.clear();
            return;
        }

        const std::string folder = CoSyncSaveLoad::GetSaveFolder();
        Outgoing base;
        if (!ReadFile(folder + kHostSaveName + ".fos", base.files[0]) || base.files[0].empty())
        {
            LOG_ERROR("[WorldXfer] Could not read '%s%s.fos'", folder.c_str(), kHostSaveName);
            for (uint32_t conn : s_pendingRequests)
                CoSyncTransport::SendTo(conn, "WORLDERR|save_unreadable");
            s_pendingRequests.clear();
            return;
        }
        // The F4SE co-save carries plugin data -- including CoSync's player
        // profiles. It can legitimately be absent (no plugin wrote anything).
        ReadFile(folder + kHostSaveName + ".f4se", base.files[1]);

        LOG_INFO("[WorldXfer] World saved: fos=%zu bytes co-save=%zu bytes -> %zu client(s)",
            base.files[0].size(), base.files[1].size(), s_pendingRequests.size());

        for (uint32_t conn : s_pendingRequests)
        {
            Outgoing o;
            o.conn = conn;
            o.id = s_nextId++;
            o.files[0] = base.files[0];
            o.files[1] = base.files[1];
            s_outgoing.push_back(std::move(o));
        }
        s_pendingRequests.clear();
    }

    // Client: F4SE main-thread task.
    void ClientLoadWorld()
    {
        LOG_REQUIREMENT("(anonymous)::ClientLoadWorld", "Main thread (F4SE task), like the console 'load' command.");
        LOG_INFO("[WorldXfer] Loading the host's world ('%s')", kClientSaveName);
        s_loadStartedMs = static_cast<double>(GetTickCount64());
        if (!CoSyncSaveLoad::LoadNamed(kClientSaveName))
        {
            // Not necessarily fatal: the engine returns false when it shows a
            // prompt first (e.g. a load-order warning) and loads after the
            // player confirms. Stay in Loading; OnPostLoadGame(true) completes
            // the join, Tick() gives up after kLoadTimeoutMs.
            LOG_WARN("[WorldXfer] Load did not start immediately -- waiting (a game prompt may need confirming)");
        }
        // Success is confirmed by kMessage_PostLoadGame (OnPostLoadGame).
    }
}

namespace CoSyncWorldTransfer
{
    bool IsTransferMessage(const std::string& msg)
    {
        return msg.compare(0, 5, "WORLD") == 0;
    }

    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole)
    {
        LOG_REQUIREMENT("CoSyncWorldTransfer::OnReceive", "Main thread (transport inbox). Treats every field as untrusted.");
        size_t payload = 0;

        if (isHostRole)
        {
            if (msg.rfind("WORLDREQ|", 0) != 0 || conn == 0)
                return;
            auto f = SplitHeader(msg, 4, payload);
            uint64_t build = 0;
            if (f.size() < 2 || !ParseU64(f[1], build) || build != CoSyncNet::GetBuildNumber())
            {
                LOG_WARN("[WorldXfer] WORLDREQ from conn=%u with wrong build -- refused", conn);
                CoSyncTransport::SendTo(conn, "WORLDERR|version_mismatch");
                return;
            }
            if (!CoSyncWorld::IsWorldReady())
            {
                CoSyncTransport::SendTo(conn, "WORLDERR|host_not_in_game");
                return;
            }
            // 0.4.0 join gate: the world (the host's save) goes only to a
            // client whose plugins/archives the host has verified -- a save
            // loaded without one of its plugins drops that plugin's content.
            if (!CoSyncContentSync::HostIsConnVerified(conn))
            {
                LOG_WARN("[WorldXfer] WORLDREQ from conn=%u before its content was verified -- refused", conn);
                CoSyncTransport::SendTo(conn, "WORLDERR|content_not_verified");
                return;
            }
            if (!s_worldRequestedConns.insert(conn).second)
            {
                LOG_WARN("[WorldXfer] Ignoring a repeated WORLDREQ from conn=%u (one world per connection)", conn);
                return;
            }
            const std::string key  = f.size() > 2 ? f[2] : std::string();
            const std::string name = f.size() > 3 ? f[3] : std::string();
            CoSyncProfiles::HostOnPlayerKey(conn, key, name);

            LOG_INFO("[WorldXfer] World requested by conn=%u ('%s')", conn, name.c_str());
            s_pendingRequests.push_back(conn);
            if (!s_saveQueued)
            {
                s_saveQueued = CoSyncSpawnTasks::EnqueueMainThread([] { HostSaveAndQueue(); });
                if (!s_saveQueued)
                {
                    for (uint32_t c : s_pendingRequests)
                        CoSyncTransport::SendTo(c, "WORLDERR|host_busy");
                    s_pendingRequests.clear();
                }
            }
            return;
        }

        // ── Client ──
        if (msg.rfind("WORLDERR|", 0) == 0)
        {
            Fail(msg.substr(9, 40).c_str());
            return;
        }

        if (msg.rfind("WORLDBEGIN|", 0) == 0)
        {
            auto f = SplitHeader(msg, 6, payload);
            uint64_t id = 0, sz0 = 0, sz1 = 0, c0 = 0, c1 = 0;
            if (f.size() < 6 || !ParseU64(f[1], id) || !ParseU64(f[2], sz0) || !ParseU64(f[3], sz1) ||
                !ParseU64(f[4], c0) || !ParseU64(f[5], c1) || sz0 == 0 ||
                sz0 > kMaxFileBytes || sz1 > kMaxFileBytes ||
                sz0 + sz1 > kMaxFileBytes)   // S-SEC2: both buffers are allocated up front

            {
                Fail("bad_header");
                return;
            }
            s_inId = static_cast<uint32_t>(id);
            s_inSize[0] = sz0; s_inSize[1] = sz1;
            s_inCrc[0] = static_cast<uint32_t>(c0); s_inCrc[1] = static_cast<uint32_t>(c1);
            s_inFiles[0].assign(static_cast<size_t>(sz0), 0);
            s_inFiles[1].assign(static_cast<size_t>(sz1), 0);
            s_inReceived = 0;
            s_percent.store(0);
            SetPhase(ClientPhase::Downloading);
            LOG_INFO("[WorldXfer] Downloading world: fos=%llu co-save=%llu bytes",
                (unsigned long long)sz0, (unsigned long long)sz1);
            return;
        }

        if (msg.rfind("WORLDCHUNK|", 0) == 0)
        {
            if (GetClientPhase() != ClientPhase::Downloading)
                return;
            auto f = SplitHeader(msg, 4, payload);
            uint64_t id = 0, fileIdx = 0, offset = 0;
            if (f.size() < 4 || payload == std::string::npos || !ParseU64(f[1], id) ||
                !ParseU64(f[2], fileIdx) || !ParseU64(f[3], offset) ||
                id != s_inId || fileIdx > 1)
            {
                Fail("bad_chunk");
                return;
            }
            const size_t n = msg.size() - payload;
            std::vector<uint8_t>& dst = s_inFiles[fileIdx];
            if (offset > dst.size() || n > dst.size() - offset)
            {
                Fail("chunk_out_of_range");
                return;
            }
            std::memcpy(dst.data() + offset, msg.data() + payload, n);
            s_inReceived += n;
            const uint64_t total = s_inSize[0] + s_inSize[1];
            s_percent.store(total ? static_cast<int>((s_inReceived * 100) / total) : 100);
            return;
        }

        if (msg.rfind("WORLDEND|", 0) == 0)
        {
            if (GetClientPhase() != ClientPhase::Downloading)
                return;
            if (Crc32(s_inFiles[0]) != s_inCrc[0] || Crc32(s_inFiles[1]) != s_inCrc[1])
            {
                Fail("checksum");
                return;
            }
            const std::string folder = CoSyncSaveLoad::GetSaveFolder();
            const std::string fos = folder + kClientSaveName + ".fos";
            const std::string co  = folder + kClientSaveName + ".f4se";
            if (!WriteFile(fos, s_inFiles[0]))
            {
                Fail("write_failed");
                return;
            }
            if (!s_inFiles[1].empty())
                WriteFile(co, s_inFiles[1]);
            else
                std::remove(co.c_str());   // never pair the new save with a stale co-save
            s_inFiles[0].clear(); s_inFiles[0].shrink_to_fit();
            s_inFiles[1].clear(); s_inFiles[1].shrink_to_fit();
            s_percent.store(100);
            LOG_INFO("[WorldXfer] World received and verified -> %s", fos.c_str());

            SetPhase(ClientPhase::Loading);
            if (!CoSyncSpawnTasks::EnqueueMainThread([] { ClientLoadWorld(); }))
                Fail("task_queue");
            return;
        }
    }

    void Tick()
    {
        // Client: the session ended while playing in the host's world.
        if (s_returnToMenu)
        {
            s_returnToMenu = false;
            CoSyncGameAPI::QueueNotification("The co-op session ended. Returning to the main menu.");
            QuitToMainMenu();
        }

        // Client: a load that never completed (prompt declined, file missing).
        if (GetClientPhase() == ClientPhase::Loading && s_loadStartedMs > 0.0 &&
            static_cast<double>(GetTickCount64()) - s_loadStartedMs > kLoadTimeoutMs)
        {
            s_loadStartedMs = 0.0;
            Fail("load_failed");
        }

        // Host: stream queued files within a per-frame budget.
        size_t budget = kBytesPerTick;
        for (auto it = s_outgoing.begin(); it != s_outgoing.end() && budget > 0;)
        {
            Outgoing& o = *it;
            if (!o.begun)
            {
                char hdr[160];
                std::snprintf(hdr, sizeof(hdr), "WORLDBEGIN|%u|%zu|%zu|%u|%u",
                    o.id, o.files[0].size(), o.files[1].size(), Crc32(o.files[0]), Crc32(o.files[1]));
                CoSyncTransport::SendTo(o.conn, hdr);
                o.begun = true;
            }
            while (budget > 0 && o.fileIdx < 2)
            {
                const std::vector<uint8_t>& file = o.files[o.fileIdx];
                if (o.offset >= file.size())
                {
                    ++o.fileIdx;
                    o.offset = 0;
                    continue;
                }
                const size_t n = (std::min)(kChunkBytes, file.size() - o.offset);
                char hdr[96];
                const int h = std::snprintf(hdr, sizeof(hdr), "WORLDCHUNK|%u|%zu|%zu|", o.id, o.fileIdx, o.offset);
                std::string packet(hdr, static_cast<size_t>(h));
                packet.append(reinterpret_cast<const char*>(file.data() + o.offset), n);
                CoSyncTransport::SendTo(o.conn, packet);
                o.offset += n;
                budget = budget > n ? budget - n : 0;
            }
            if (o.fileIdx >= 2)
            {
                CoSyncTransport::SendTo(o.conn, "WORLDEND|" + std::to_string(o.id));
                LOG_INFO("[WorldXfer] World sent to conn=%u", o.conn);
                it = s_outgoing.erase(it);
            }
            else
                ++it;
        }
    }

    void ClientBegin(const std::string& profileKey, const std::string& name)
    {
        LOG_REQUIREMENT("CoSyncWorldTransfer::ClientBegin", "Main thread, once the transport is connected.");
        {
            std::lock_guard<std::mutex> lk(s_errMutex);
            s_error.clear();
        }
        s_percent.store(0);
        SetPhase(ClientPhase::Requested);
        // Keep the name free of the field separator.
        std::string safeName = name;
        for (char& c : safeName) if (c == '|') c = '/';
        CoSyncTransport::Send("WORLDREQ|" + std::to_string(CoSyncNet::GetBuildNumber()) + "|" +
                              profileKey + "|" + safeName);
        LOG_INFO("[WorldXfer] Requested the host's world (key '%s')", profileKey.c_str());
    }

    void OnPostLoadGame(bool success)
    {
        LOG_REQUIREMENT("CoSyncWorldTransfer::OnPostLoadGame", "Main thread (F4SE message).");
        if (GetClientPhase() != ClientPhase::Loading)
        {
            // Any other load (their own save, from the main menu) is theirs again.
            if (success && s_inHostWorld.exchange(false))
                LOG_INFO("[WorldXfer] Another save was loaded -- saving is allowed again");
            return;
        }
        if (!success)
        {
            // An attempt ended without loading -- often a prompt came first.
            // Keep waiting for the real load (or the timeout in Tick).
            LOG_WARN("[WorldXfer] Load attempt reported failure -- still waiting for the load");
            return;
        }
        s_loadStartedMs = 0.0;
        s_inHostWorld.store(true);
        // A joining player starts with nothing: the host's gear goes now, not
        // seconds later when the character is applied.
        CoSyncProfiles::ClientStripInheritedInventory();
        SetPhase(ClientPhase::Ready);
        LOG_INFO("[WorldXfer] Host world loaded -- joining the session");
    }

    void OnNewGame()
    {
        if (s_inHostWorld.exchange(false))
            LOG_INFO("[WorldXfer] New game started -- saving is allowed again");
    }

    bool IsInHostWorld() { return s_inHostWorld.load(); }

    void Reset()
    {
        // Only once per session end; s_inHostWorld stays set (saves stay
        // blocked) until the main menu's next load / new game clears it.
        if (s_inHostWorld.load() && GetClientPhase() == ClientPhase::Ready)
            s_returnToMenu = true;
        s_pendingRequests.clear();
        s_outgoing.clear();
        s_worldRequestedConns.clear();
        s_saveQueued = false;
        s_inFiles[0].clear(); s_inFiles[1].clear();
        s_inReceived = 0;
        s_percent.store(0);
        SetPhase(ClientPhase::Idle);
    }

    ClientPhase GetClientPhase()        { return static_cast<ClientPhase>(s_phase.load()); }
    bool        IsClientWorldReady()    { return GetClientPhase() == ClientPhase::Ready; }
    int         GetClientProgressPercent() { return s_percent.load(); }
    std::string GetClientError()
    {
        std::lock_guard<std::mutex> lk(s_errMutex);
        return s_error;
    }
}
