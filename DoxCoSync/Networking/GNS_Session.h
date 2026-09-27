
#pragma once

#include <string>
#include <unordered_set>
#include <unordered_map>
#include <cstdint>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

#include "steam/steamnetworkingsockets.h"

enum class GNSRole
{
    None,
    Host,
    Client
};

// Client-side connection lifecycle, as reported by GNS itself. Lets the UI
// show the real outcome (session full / failed / host left) instead of
// guessing from "not connected".
enum class GNSClientState
{
    Idle,
    Connecting,
    Connected,
    Closed
};

class GNS_Session
{
public:
    static GNS_Session& Get();


    // ip: "" / "0.0.0.0" / ":port" = listen on ALL interfaces (LAN, Wi-Fi,
    // ...); otherwise a specific local address. If the port is taken, the next
    // ports are tried (CoSyncSessionConfig::kGamePortTries).
    bool StartHost(const char* ip);
    bool StartClient(const std::string& connectStr);


    void HandleConnectionStatusChanged(const SteamNetConnectionStatusChangedCallback_t* info);

    // Stops the background network thread and closes every socket. Must run
    // BEFORE GNS_Shutdown(): the thread calls into the library every 16 ms,
    // and GameNetworkingSockets_Kill() underneath it is a use-after-free.
    void EndSession() { ResetSession(); }

    // Host: disconnect ONE client with an application reason (e.g. version
    // mismatch). Does the same per-peer cleanup as a client closing on its
    // own: its entity is destroyed everywhere and CoSyncNet gets PEERLEFT.
    void KickClient(HSteamNetConnection conn, int reason, const char* debug);

    // Client: leave the host with an application reason (reported to the UI
    // exactly like a host-side rejection).
    void LeaveWithReason(int reason, const char* debug);

    void Tick();


    // reliable=false sends k_nSteamNetworkingSend_Unreliable instead. Only
    // the bone-pose stream uses it: a dropped pose frame must be DISCARDED,
    // not retransmitted, because a fresher one is already 67ms behind it, and
    // on the shared reliable stream a lost 1.4KB pose frame head-of-line
    // blocks position/health/quest traffic behind it. Everything else keeps
    // the reliable default.
    void SendText(const std::string& text, bool reliable = true);

    // Host: send to ONE client only (e.g. that client's copy of the world).
    void SendTextTo(HSteamNetConnection conn, const std::string& text, bool reliable = true);

    // Host: send to every connected client except `except` (the connection a
    // relayed message came from). Client role: same as SendText.
    void SendTextExcept(const std::string& text, HSteamNetConnection except, bool reliable = true);


    std::string GetHostConnectString() const;

    GNSRole GetRole() const { return m_role; }

    bool IsConnected() const { return m_connected.load(std::memory_order_relaxed); }

    // Host: port the listen socket actually bound (0 when not hosting).
    uint16_t GetBoundPort() const { return m_boundPort.load(); }

    // Host: fully connected clients (thread-safe).
    int GetConnectedClientCount() const;

    // Client: lifecycle + the GNS end reason once Closed (thread-safe).
    GNSClientState GetClientState() const { return m_clientState.load(); }
    int  GetClientEndReason() const { return m_clientEndReason.load(); }
    bool ClientWasEverConnected() const { return m_clientWasConnected.load(); }

private:
    GNS_Session();
    ~GNS_Session();

    void ResetSession();
    void CloseConnection(HSteamNetConnection conn, const char* reason);

    void ProcessMessagesOnPollGroup();
    void ProcessMessagesOnConnection(HSteamNetConnection conn);

    void Host_TrackPending(HSteamNetConnection conn);
    void Host_PromoteToConnected(HSteamNetConnection conn);
    void Host_RemoveAny(HSteamNetConnection conn);

    void StartNetworkThread();
    void StopNetworkThread();
    void NetworkThreadProc();

private:
    GNSRole m_role = GNSRole::None;


    std::atomic<bool> m_connected{false};

    HSteamListenSocket  m_listenSocket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup  m_pollGroup    = k_HSteamNetPollGroup_Invalid;


    HSteamNetConnection m_serverConn = k_HSteamNetConnection_Invalid;

    mutable std::mutex m_connsMutex;
    std::unordered_set<HSteamNetConnection> m_pendingClientConns;
    std::unordered_set<HSteamNetConnection> m_clientConns;
    std::unordered_map<HSteamNetConnection, uint64_t> m_peerSteamIDs;


    std::thread       m_networkThread;
    std::atomic<bool> m_networkThreadRunning{false};

    std::atomic<uint16_t>       m_boundPort{0};
    std::atomic<GNSClientState> m_clientState{GNSClientState::Idle};
    std::atomic<int>            m_clientEndReason{0};
    std::atomic<bool>           m_clientWasConnected{false};
};
