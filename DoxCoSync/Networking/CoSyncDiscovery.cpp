#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <iphlpapi.h>
#include <mstcpip.h>

// mstcpip.h hides this behind a _WIN32_WINNT guard that the project prelude
// sets low; value per Microsoft's documentation of SIO_UDP_CONNRESET.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Iphlpapi.lib")

#include "CoSyncDiscovery.h"
#include "CoSyncSessionConfig.h"
#include "ConsoleLogger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>

namespace
{
    // ── Wire format ─────────────────────────────────────────────────────────
    // All packets start with a 16-byte header:
    //   [0..3]  magic 'C','S','Y','N'
    //   [4]     protocol version (kProtoVersion)
    //   [5]     type (kQuery / kAnnounce / kGoodbye)
    //   [6..7]  reserved, must be 0
    //   [8..15] nonce (u64 LE): searcher nonce for QUERY, session id otherwise
    // QUERY and GOODBYE are exactly the header. ANNOUNCE appends:
    //   [16..17] GNS port (u16 LE)
    //   [18]     current players
    //   [19]     max players
    //   [20..23] CoSync build (u32 LE)
    //   [24]     host name length, [25..56] host name (UTF-8, zero padded)
    //   [57]     game name length, [58..105] game name (UTF-8, zero padded)
    constexpr uint8_t kMagic[4]    = { 'C', 'S', 'Y', 'N' };
    constexpr uint8_t kProtoVersion = 1;
    constexpr uint8_t kQuery = 1, kAnnounce = 2, kGoodbye = 3;

    constexpr size_t kHeaderSize   = 16;
    constexpr size_t kHostNameMax  = 32;
    constexpr size_t kGameNameMax  = 48;
    constexpr size_t kAnnounceSize = kHeaderSize + 2 + 1 + 1 + 4 + 1 + kHostNameMax + 1 + kGameNameMax;  // 106

    constexpr double kSessionTimeout      = 4.0;   // drop a host not heard from
    constexpr double kQueryInterval       = 1.0;   // searcher QUERY cadence
    constexpr double kQuerierMemory       = 10.0;  // who gets a GOODBYE
    constexpr double kInterfaceCacheTime  = 3.0;
    constexpr size_t kMaxSessions         = 32;
    constexpr int    kMaxRepliesPerSecond = 32;

    double Now()
    {
        using namespace std::chrono;
        return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
    }

    void PutU16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
    void PutU32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
    void PutU64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i)); }
    uint16_t GetU16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
    uint32_t GetU32(const uint8_t* p) { uint32_t v = 0; for (int i = 3; i >= 0; --i) v = (v << 8) | p[i]; return v; }
    uint64_t GetU64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v; }

    // Truncate to maxBytes without splitting a UTF-8 sequence.
    std::string Utf8Truncate(const std::string& s, size_t maxBytes)
    {
        if (s.size() <= maxBytes)
            return s;
        size_t n = maxBytes;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
            --n;
        return s.substr(0, n);
    }

    // Untrusted network text: printable ASCII or UTF-8 continuation/lead
    // bytes only -- no control characters.
    bool ReadName(const uint8_t* lenPtr, size_t maxBytes, std::string& out)
    {
        const size_t len = *lenPtr;
        if (len == 0 || len > maxBytes)
            return false;
        const uint8_t* p = lenPtr + 1;
        for (size_t i = 0; i < len; ++i)
            if (p[i] < 0x20 || p[i] == 0x7F)
                return false;
        out.assign(reinterpret_cast<const char*>(p), len);
        return true;
    }

    void WriteHeader(uint8_t* buf, uint8_t type, uint64_t nonce)
    {
        std::memcpy(buf, kMagic, 4);
        buf[4] = kProtoVersion;
        buf[5] = type;
        buf[6] = buf[7] = 0;
        PutU64(buf + 8, nonce);
    }

    bool ReadHeader(const uint8_t* buf, int len, uint8_t& type, uint64_t& nonce)
    {
        if (len < static_cast<int>(kHeaderSize))
            return false;
        if (std::memcmp(buf, kMagic, 4) != 0 || buf[4] != kProtoVersion || buf[6] != 0 || buf[7] != 0)
            return false;
        type = buf[5];
        nonce = GetU64(buf + 8);
        if (type == kQuery || type == kGoodbye)
            return len == static_cast<int>(kHeaderSize);
        if (type == kAnnounce)
            return len == static_cast<int>(kAnnounceSize);
        return false;
    }

    std::string WideToUtf8(const wchar_t* w)
    {
        if (!w || !*w)
            return std::string();
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1)
            return std::string();
        std::string s(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
        return s;
    }

    std::string Ipv4ToString(uint32_t addrNetOrder)
    {
        in_addr a{};
        a.S_un.S_addr = addrNetOrder;
        char buf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &a, buf, sizeof(buf));
        return buf;
    }

    // ── Network interfaces ──────────────────────────────────────────────────

    struct Iface
    {
        std::string name;
        std::string type;      // "Ethernet", "Wi-Fi", "Other"
        uint32_t    addr = 0;  // network byte order
        uint32_t    bcast = 0; // network byte order, subnet-directed broadcast
        bool        physical = false;
        bool        gateway = false;
    };

    // Adapters that are never the player's real LAN. They are still used as a
    // last resort when nothing physical is up (so e.g. a VPN-only setup can
    // still find a host), but never preferred.
    bool LooksVirtual(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        static const char* kWords[] = {
            "virtual", "vethernet", "hyper-v", "vmware", "virtualbox", "hamachi",
            "tap-", "tap ", "wireguard", "zerotier", "tailscale", "npcap",
            "bluetooth", "wsl", "vpn", "loopback"
        };
        for (const char* w : kWords)
            if (text.find(w) != std::string::npos)
                return true;
        return false;
    }

    std::vector<Iface> EnumerateInterfaces()
    {
        std::vector<Iface> out;

        ULONG size = 16 * 1024;
        std::vector<unsigned char> buffer;
        ULONG ret = ERROR_BUFFER_OVERFLOW;
        for (int attempt = 0; attempt < 3 && ret == ERROR_BUFFER_OVERFLOW; ++attempt)
        {
            buffer.resize(size);
            ret = GetAdaptersAddresses(AF_INET,
                GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER |
                GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST,
                nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        }
        if (ret != NO_ERROR)
            return out;

        for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next)
        {
            if (a->OperStatus != IfOperStatusUp)
                continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL)
                continue;

            const std::string name = WideToUtf8(a->FriendlyName);
            const std::string desc = WideToUtf8(a->Description);

            std::string type = "Other";
            if (a->IfType == IF_TYPE_IEEE80211)          type = "Wi-Fi";
            else if (a->IfType == IF_TYPE_ETHERNET_CSMACD) type = "Ethernet";

            const bool isVirtual = LooksVirtual(name) || LooksVirtual(desc);

            for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
            {
                if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET)
                    continue;
                const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
                const uint32_t addr = sin->sin_addr.S_un.S_addr;
                const unsigned prefix = u->OnLinkPrefixLength;
                if (prefix == 0 || prefix > 30)
                    continue;
                const uint32_t mask = htonl(0xFFFFFFFFu << (32 - prefix));

                Iface f;
                f.name     = name;
                f.type     = type;
                f.addr     = addr;
                f.bcast    = addr | ~mask;
                f.physical = !isVirtual && type != "Other";
                f.gateway  = a->FirstGatewayAddress != nullptr;
                out.push_back(f);
            }
        }
        return out;
    }

    // ── Shared state ────────────────────────────────────────────────────────

    struct Entry
    {
        CoSyncDiscovery::Session session;
        double lastSeen = 0.0;
        bool   loopback = false;
    };

    std::mutex  g_mx;              // guards everything below
    bool        g_run = false;     // worker thread alive (or about to be)

    bool        g_advertise = false;
    uint64_t    g_advNonce = 0;
    uint16_t    g_advPort = 0;
    std::string g_advGame, g_advHost;
    uint32_t    g_advBuild = 0;
    std::function<int()> g_advCount;

    bool        g_search = false;
    uint64_t    g_searchNonce = 0;
    double      g_searchStart = 0.0;
    std::unordered_map<uint64_t, Entry> g_sessions;

    std::vector<Iface> g_ifaces;
    double             g_ifacesTime = -1000.0;

    std::mutex  g_lifeMx;          // thread start/join

    // The worker exits on its own once advertising and searching both stop,
    // and nothing joins it until the next Start*() (EnsureWorker) or
    // CoSyncDiscovery::Shutdown() -- neither runs when the player just quits
    // the game. A still-joinable std::thread destroyed during the CRT's
    // static teardown calls std::terminate: confirmed 2026-09-26 by a WER
    // crash dump of a normal quit (DllMain detach -> ~thread -> terminate ->
    // abort, 0xC0000409). Release it without blocking instead: at process
    // exit Windows has already ended every other thread, and detach() can
    // never wait on the loader lock the way join() could.
    struct WorkerThread
    {
        std::thread t;
        ~WorkerThread()
        {
            if (t.joinable())
                t.detach();
        }
    };
    WorkerThread g_thread;

    uint64_t NewNonce()
    {
        static std::mt19937_64 rng{ std::random_device{}() ^
            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) };
        uint64_t v = 0;
        while (v == 0) v = rng();
        return v;
    }

    // Caller holds g_mx.
    const std::vector<Iface>& InterfacesLocked()
    {
        const double now = Now();
        if (now - g_ifacesTime > kInterfaceCacheTime)
        {
            g_ifaces = EnumerateInterfaces();
            g_ifacesTime = now;
        }
        return g_ifaces;
    }

    // ── Socket helpers (worker thread) ──────────────────────────────────────

    SOCKET OpenSocket(uint16_t port)
    {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
            return INVALID_SOCKET;

        BOOL on = TRUE;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
        if (port != 0)   // let a second CoSync on the same PC host too
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));

        // Windows reports a previous send's ICMP "port unreachable" (e.g. the
        // 127.0.0.1 query when nobody hosts locally) as WSAECONNRESET on the
        // next recvfrom. Turn that off.
        BOOL noReset = FALSE;
        DWORD bytes = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &noReset, sizeof(noReset), nullptr, 0, &bytes, nullptr, nullptr);

        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(port);
        if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
        {
            LOG_ERROR("[Discovery] bind(UDP %u) failed: WSA %d", (unsigned)port, WSAGetLastError());
            closesocket(s);
            return INVALID_SOCKET;
        }
        return s;
    }

    void SendTo(SOCKET s, const uint8_t* data, int len, uint32_t addrNetOrder, uint16_t port)
    {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = addrNetOrder;
        to.sin_port = htons(port);
        sendto(s, reinterpret_cast<const char*>(data), len, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }

    void SendQueries(SOCKET s)
    {
        uint8_t pkt[kHeaderSize];
        std::vector<uint32_t> targets;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            WriteHeader(pkt, kQuery, g_searchNonce);

            const auto& ifs = InterfacesLocked();
            const bool anyPhysical = std::any_of(ifs.begin(), ifs.end(), [](const Iface& f) { return f.physical; });
            for (const Iface& f : ifs)
                if (f.physical || !anyPhysical)
                    targets.push_back(f.bcast);
        }
        targets.push_back(htonl(INADDR_BROADCAST));   // limited broadcast
        targets.push_back(htonl(INADDR_LOOPBACK));    // second instance on this PC

        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (uint32_t t : targets)
            SendTo(s, pkt, sizeof(pkt), t, CoSyncSessionConfig::kDiscoveryPort);
    }

    // Host: answer one QUERY. Caller must NOT hold g_mx.
    void SendAnnounce(SOCKET s, const sockaddr_in& to)
    {
        uint8_t pkt[kAnnounceSize] = {};
        std::function<int()> countFn;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (!g_advertise)
                return;
            WriteHeader(pkt, kAnnounce, g_advNonce);
            PutU16(pkt + 16, g_advPort);
            pkt[19] = static_cast<uint8_t>(CoSyncSessionConfig::kMaxPlayers);
            PutU32(pkt + 20, g_advBuild);

            const std::string host = Utf8Truncate(g_advHost, kHostNameMax);
            pkt[24] = static_cast<uint8_t>(host.size());
            std::memcpy(pkt + 25, host.data(), host.size());

            const std::string game = Utf8Truncate(g_advGame, kGameNameMax);
            pkt[57] = static_cast<uint8_t>(game.size());
            std::memcpy(pkt + 58, game.data(), game.size());

            countFn = g_advCount;
        }

        int players = countFn ? countFn() : 1;
        if (players < 1) players = 1;
        if (players > CoSyncSessionConfig::kMaxPlayers) players = CoSyncSessionConfig::kMaxPlayers;
        pkt[18] = static_cast<uint8_t>(players);

        sendto(s, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
               reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }

    void HandleAnnounce(const uint8_t* buf, uint64_t nonce, const sockaddr_in& from)
    {
        CoSyncDiscovery::Session s;
        s.id         = nonce;
        s.port       = GetU16(buf + 16);
        s.players    = buf[18];
        s.maxPlayers = buf[19];
        s.build      = GetU32(buf + 20);

        if (s.port == 0 || s.maxPlayers < 1 || s.maxPlayers > CoSyncSessionConfig::kMaxPlayers ||
            s.players < 1 || s.players > s.maxPlayers)
            return;
        if (!ReadName(buf + 24, kHostNameMax, s.hostName) || !ReadName(buf + 57, kGameNameMax, s.gameName))
            return;

        s.address = Ipv4ToString(from.sin_addr.s_addr);
        const bool loopback = (ntohl(from.sin_addr.s_addr) >> 24) == 127;

        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_search || (g_advertise && nonce == g_advNonce))
            return;   // not searching, or our own session

        auto it = g_sessions.find(nonce);
        if (it == g_sessions.end())
        {
            if (g_sessions.size() >= kMaxSessions)
                return;
            LOG_INFO("[Discovery] Found '%s' at %s:%u (%d/%d, build %u)",
                s.gameName.c_str(), s.address.c_str(), (unsigned)s.port, s.players, s.maxPlayers, s.build);
            g_sessions[nonce] = Entry{ s, Now(), loopback };
            return;
        }

        // Same host heard on several interfaces: keep a LAN address over
        // loopback, but always refresh the live fields.
        Entry& e = it->second;
        if (e.loopback && !loopback)
        {
            e.session.address = s.address;
            e.loopback = false;
        }
        const std::string keepAddress = e.session.address;
        e.session = s;
        e.session.address = keepAddress;
        e.lastSeen = Now();
    }


    // ── Worker thread ───────────────────────────────────────────────────────

    struct Querier
    {
        sockaddr_in addr{};
        double lastSeen = 0.0;
    };

    uint64_t AddrKey(const sockaddr_in& a)
    {
        return (static_cast<uint64_t>(a.sin_addr.s_addr) << 16) | a.sin_port;
    }

    void Worker()
    {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            LOG_ERROR("[Discovery] WSAStartup failed");
            std::lock_guard<std::mutex> lk(g_mx);
            g_run = false;
            return;
        }
        LOG_INFO("[Discovery] Worker started");

        SOCKET hostSock = INVALID_SOCKET;     // bound to kDiscoveryPort while advertising
        SOCKET clientSock = INVALID_SOCKET;   // ephemeral port while searching
        uint64_t advNonceForGoodbye = 0;
        std::unordered_map<uint64_t, Querier> queriers;
        double lastQuery = -1000.0;
        double replyWindowStart = 0.0;
        int repliesInWindow = 0;

        for (;;)
        {
            bool advertise, search;
            {
                std::lock_guard<std::mutex> lk(g_mx);
                advertise = g_advertise;
                search = g_search;
                if (advertise)
                    advNonceForGoodbye = g_advNonce;
            }

            // Host socket lifecycle.
            if (advertise && hostSock == INVALID_SOCKET)
            {
                hostSock = OpenSocket(CoSyncSessionConfig::kDiscoveryPort);
                if (hostSock != INVALID_SOCKET)
                    LOG_INFO("[Discovery] Advertising on UDP %u", (unsigned)CoSyncSessionConfig::kDiscoveryPort);
            }
            if (!advertise && hostSock != INVALID_SOCKET)
            {
                uint8_t bye[kHeaderSize];
                WriteHeader(bye, kGoodbye, advNonceForGoodbye);
                for (const auto& kv : queriers)
                    sendto(hostSock, reinterpret_cast<const char*>(bye), sizeof(bye), 0,
                           reinterpret_cast<const sockaddr*>(&kv.second.addr), sizeof(kv.second.addr));
                LOG_INFO("[Discovery] Stopped advertising (GOODBYE sent to %zu searcher(s))", queriers.size());
                queriers.clear();
                closesocket(hostSock);
                hostSock = INVALID_SOCKET;
            }

            // Client socket lifecycle.
            if (search && clientSock == INVALID_SOCKET)
            {
                clientSock = OpenSocket(0);
                lastQuery = -1000.0;
                if (clientSock != INVALID_SOCKET)
                    LOG_INFO("[Discovery] Searching for games");
            }
            if (!search && clientSock != INVALID_SOCKET)
            {
                closesocket(clientSock);
                clientSock = INVALID_SOCKET;
                LOG_INFO("[Discovery] Stopped searching");
            }

            // Nothing left to do: exit, unless a Start*() raced in meanwhile.
            if (hostSock == INVALID_SOCKET && clientSock == INVALID_SOCKET)
            {
                std::lock_guard<std::mutex> lk(g_mx);
                if (!g_advertise && !g_search)
                {
                    g_run = false;
                    break;
                }
                if ((g_advertise && !advertise) || (g_search && !search))
                    continue;   // flags just changed -- reopen sockets
                // Sockets failed to open: back off instead of spinning.
            }

            const double now = Now();

            if (clientSock != INVALID_SOCKET && now - lastQuery >= kQueryInterval)
            {
                SendQueries(clientSock);
                lastQuery = now;
            }

            // Expire stale sessions and searchers.
            {
                std::lock_guard<std::mutex> lk(g_mx);
                for (auto it = g_sessions.begin(); it != g_sessions.end();)
                {
                    if (now - it->second.lastSeen > kSessionTimeout)
                    {
                        LOG_INFO("[Discovery] Lost '%s' (%s)", it->second.session.gameName.c_str(),
                            it->second.session.address.c_str());
                        it = g_sessions.erase(it);
                    }
                    else
                        ++it;
                }
            }
            for (auto it = queriers.begin(); it != queriers.end();)
                it = (now - it->second.lastSeen > kQuerierMemory) ? queriers.erase(it) : std::next(it);

            // Wait for traffic (100 ms keeps flag changes responsive).
            fd_set rd;
            FD_ZERO(&rd);
            if (hostSock != INVALID_SOCKET)   FD_SET(hostSock, &rd);
            if (clientSock != INVALID_SOCKET) FD_SET(clientSock, &rd);
            timeval tv{ 0, 100 * 1000 };
            if (rd.fd_count == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            if (select(0, &rd, nullptr, nullptr, &tv) <= 0)
                continue;

            uint8_t buf[256];
            for (SOCKET s : { hostSock, clientSock })
            {
                if (s == INVALID_SOCKET || !FD_ISSET(s, &rd))
                    continue;
                for (int burst = 0; burst < 64; ++burst)
                {
                    sockaddr_in from{};
                    int fromLen = sizeof(from);
                    const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                           reinterpret_cast<sockaddr*>(&from), &fromLen);
                    if (n == SOCKET_ERROR)
                        break;   // WSAEWOULDBLOCK: drained

                    uint8_t type = 0;
                    uint64_t nonce = 0;
                    if (!ReadHeader(buf, n, type, nonce))
                        continue;   // not ours / malformed: ignore silently

                    if (s == hostSock && type == kQuery)
                    {
                        const double t = Now();
                        if (t - replyWindowStart >= 1.0) { replyWindowStart = t; repliesInWindow = 0; }
                        if (++repliesInWindow > kMaxRepliesPerSecond)
                            continue;
                        queriers[AddrKey(from)] = Querier{ from, t };
                        SendAnnounce(hostSock, from);
                    }
                    else if (s == clientSock && type == kAnnounce)
                    {
                        HandleAnnounce(buf, nonce, from);
                    }
                    else if (s == clientSock && type == kGoodbye)
                    {
                        std::lock_guard<std::mutex> lk(g_mx);
                        auto it = g_sessions.find(nonce);
                        if (it != g_sessions.end())
                        {
                            LOG_INFO("[Discovery] '%s' stopped hosting", it->second.session.gameName.c_str());
                            g_sessions.erase(it);
                        }
                    }
                }
            }
        }

        if (hostSock != INVALID_SOCKET)   closesocket(hostSock);
        if (clientSock != INVALID_SOCKET) closesocket(clientSock);
        WSACleanup();
        LOG_INFO("[Discovery] Worker stopped");
    }

    void EnsureWorker()
    {
        std::lock_guard<std::mutex> life(g_lifeMx);
        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (g_run)
                return;
        }
        if (g_thread.t.joinable())
            g_thread.t.join();   // previous worker already exited
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_run = true;
        }
        g_thread.t = std::thread(Worker);
    }
}

namespace CoSyncDiscovery
{
    void StartAdvertising(uint16_t gamePort, const std::string& gameName,
                          const std::string& hostName, uint32_t build,
                          std::function<int()> playerCount)
    {
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_advertise = true;
            g_advNonce  = NewNonce();
            g_advPort   = gamePort;
            g_advGame   = gameName.empty() ? std::string("CoSync Game") : gameName;
            g_advHost   = hostName.empty() ? std::string("Host") : hostName;
            g_advBuild  = build;
            g_advCount  = std::move(playerCount);
            LOG_INFO("[Discovery] StartAdvertising '%s' GNS port %u session %016llX",
                g_advGame.c_str(), (unsigned)gamePort, (unsigned long long)g_advNonce);
        }
        EnsureWorker();
    }

    void StopAdvertising()
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_advertise = false;
        g_advCount = nullptr;
    }

    bool IsAdvertising()
    {
        std::lock_guard<std::mutex> lk(g_mx);
        return g_advertise;
    }

    void StartSearching()
    {
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_search = true;
            g_searchNonce = NewNonce();
            g_searchStart = Now();
            g_sessions.clear();
        }
        EnsureWorker();
    }

    void StopSearching()
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_search = false;
        g_sessions.clear();
    }

    bool IsSearching()
    {
        std::lock_guard<std::mutex> lk(g_mx);
        return g_search;
    }

    double SecondsSinceSearchStarted()
    {
        std::lock_guard<std::mutex> lk(g_mx);
        return g_search ? Now() - g_searchStart : 0.0;
    }

    std::vector<Session> GetSessions()
    {
        std::lock_guard<std::mutex> lk(g_mx);
        std::vector<Session> out;
        out.reserve(g_sessions.size());
        for (const auto& kv : g_sessions)
            out.push_back(kv.second.session);
        std::sort(out.begin(), out.end(), [](const Session& a, const Session& b) { return a.gameName < b.gameName; });
        return out;
    }

    bool FindSession(uint64_t id, Session& out)
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_sessions.find(id);
        if (it == g_sessions.end())
            return false;
        out = it->second.session;
        return true;
    }

    NetworkInfo GetPrimaryNetwork()
    {
        std::lock_guard<std::mutex> lk(g_mx);
        const auto& ifs = InterfacesLocked();

        const Iface* best = nullptr;
        auto score = [](const Iface& f) {
            int s = 0;
            if (f.physical) s += 4;
            if (f.gateway)  s += 2;
            if (f.type == "Ethernet") s += 1;
            return s;
        };
        for (const Iface& f : ifs)
            if (!best || score(f) > score(*best))
                best = &f;

        NetworkInfo info;
        if (best)
        {
            info.available = true;
            info.type      = best->physical ? best->type : "Other";
            info.adapter   = best->name;
            info.address   = Ipv4ToString(best->addr);
        }
        return info;
    }

    void Shutdown()
    {
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_advertise = false;
            g_search = false;
            g_sessions.clear();
        }
        std::lock_guard<std::mutex> life(g_lifeMx);
        if (g_thread.t.joinable())
            g_thread.t.join();
    }
}
