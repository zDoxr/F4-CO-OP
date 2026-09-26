#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// CoSyncDiscovery -- finds CoSync games on the local network (LAN / Wi-Fi).
//
// Discovery is NOT the multiplayer transport. It only tells a joining player
// which hosts exist and which GNS endpoint to connect to; the connection
// itself is GNS_Session's job.
//
// Protocol (UDP, CoSyncSessionConfig::kDiscoveryPort):
//   * A searching client broadcasts a QUERY once a second on every suitable
//     IPv4 interface (subnet-directed broadcast), plus 255.255.255.255 and
//     127.0.0.1 (second instance on the same PC), from an ephemeral port.
//   * A host listening on kDiscoveryPort answers each QUERY with a unicast
//     ANNOUNCE (game/host name, players, max, GNS port, build).
//   * The client connects to the SOURCE address of the ANNOUNCE -- it never
//     trusts an address inside a packet -- so no adapter ever has to be chosen.
//   * Sessions not heard from for kSessionTimeout are dropped. A host that
//     stops sends GOODBYE to recent searchers so they drop it immediately.
//   * Every packet is fixed-size and strictly validated; anything else is
//     ignored.
//
// Threading: all functions are safe from any thread. Work happens on one
// internal worker thread that only runs while advertising or searching.
namespace CoSyncDiscovery
{
    struct Session
    {
        uint64_t    id = 0;            // host's random session nonce
        std::string gameName;
        std::string hostName;
        std::string address;           // IPv4 the host answered from
        uint16_t    port = 0;          // GNS port
        int         players = 0;
        int         maxPlayers = 0;
        uint32_t    build = 0;         // host's CoSync build number
    };

    struct NetworkInfo
    {
        bool        available = false; // at least one usable LAN interface
        std::string type;              // "Ethernet", "Wi-Fi", "Other"
        std::string adapter;           // adapter friendly name
        std::string address;           // IPv4 on that adapter
    };

    // Host side. playerCount is called on the worker thread and must be
    // thread-safe (it reads GNS_Session's locked connection count).
    void StartAdvertising(uint16_t gamePort, const std::string& gameName,
                          const std::string& hostName, uint32_t build,
                          std::function<int()> playerCount);
    void StopAdvertising();
    bool IsAdvertising();

    // Client side.
    void StartSearching();
    void StopSearching();
    bool IsSearching();
    double SecondsSinceSearchStarted();

    std::vector<Session> GetSessions();
    bool FindSession(uint64_t id, Session& out);

    // Best local network for display (Ethernet/Wi-Fi with a gateway first).
    // Cached for a few seconds.
    NetworkInfo GetPrimaryNetwork();

    // Stops everything and joins the worker thread (plugin shutdown).
    void Shutdown();
}
