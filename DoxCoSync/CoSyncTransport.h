#pragma once

#include <string>
#include <functional>
#include <deque>
#include <mutex>

namespace CoSyncTransport
{
    using ReceiveCallback = std::function<void(const std::string&, double now)>;
    using ConnectedCallback = std::function<void()>;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------
    bool InitAsHost();
    bool InitAsClient(const std::string& connectStr);
    void Shutdown();

    bool IsInitialized();
    bool IsHost();

    // -------------------------------------------------------------------------
    // Game-thread tick
    // Drives network polling + pumps inbox
    // -------------------------------------------------------------------------
    void Tick(double now);

    // -------------------------------------------------------------------------
    // Outgoing
    // Thread-safe
    // -------------------------------------------------------------------------
    void Send(const std::string& msg);

    // -------------------------------------------------------------------------
    // Incoming (network thread → transport)
    // Thread-safe. MUST NOT touch game systems.
    // -------------------------------------------------------------------------
    void ForwardMessage(const std::string& msg);

    // -------------------------------------------------------------------------
    // Callbacks (game thread)
    // -------------------------------------------------------------------------

    // Called on game thread when messages are drained
    void SetReceiveCallback(ReceiveCallback fn);

    // Called ONCE when a connection is established
    // (host: when first client connects, client: when connected)
    void SetConnectedCallback(ConnectedCallback fn);

    // -------------------------------------------------------------------------
    // Optional pull-based API (game thread)
    // -------------------------------------------------------------------------
    size_t DrainInbox(std::deque<std::string>& out);

    // -------------------------------------------------------------------------
    // Diagnostics
    // -------------------------------------------------------------------------
    std::string GetHostConnectString();
}
