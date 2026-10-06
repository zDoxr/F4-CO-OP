#pragma once

#include <string>
#include <functional>
#include <deque>
#include <mutex>
#include "steam/steamnetworkingsockets.h"

namespace CoSyncTransport
{
    using ReceiveCallback      = std::function<void(const std::string&, double now, HSteamNetConnection conn)>;
    using ConnectedCallback    = std::function<void()>;
    using DisconnectedCallback = std::function<void()>;

    bool InitAsHost();
    bool InitAsClient(const std::string& connectStr);
    void Shutdown();

    bool IsInitialized();
    bool IsHost();

    void Tick(double now);

    // reliable=false drops to unreliable delivery. Reserved for the bone-pose
    // stream -- see GNS_Session::SendText. Default is unchanged for every
    // other packet type.
    void Send(const std::string& msg, bool reliable = true);

    // 0.5.1 lobby: a client that waits in the lobby, plays its own New Game
    // intro, or waits for the host's intro is not in the host's world.
    // While set, Send() passes only the join protocol (content check, WORLD*,
    // the character upload, chat, the version / disconnect handshake) -- the
    // client's hooks (quest stages, doors, equips...) must not reach the
    // host's world from a different one.
    void SetClientOutsideSession(bool outside);

    // Host: one client only.
    void SendTo(HSteamNetConnection conn, const std::string& msg, bool reliable = true);

    // Host: every client except the one a relayed message came from.
    void SendExcept(const std::string& msg, HSteamNetConnection except, bool reliable = true);

    void ForwardMessage(const std::string& msg, HSteamNetConnection conn);

    void SetReceiveCallback(ReceiveCallback fn);

    void SetConnectedCallback(ConnectedCallback fn);


    void SetDisconnectedCallback(DisconnectedCallback fn);

    struct InboxMessage
    {
        std::string text;
        HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
    };

    size_t DrainInbox(std::deque<InboxMessage>& out);

    std::string GetHostConnectString();
}
