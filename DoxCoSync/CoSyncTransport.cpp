#include "CoSyncTransport.h"

#include "Logger.h"
#include "ConsoleLogger.h"
#include "GNS_Session.h"

#include <utility>

namespace
{
    bool s_initialized = false;
    bool s_isHost = false;

    CoSyncTransport::ReceiveCallback   s_receiveCallback;
    CoSyncTransport::ConnectedCallback s_connectedCallback; // NEW

    // Thread-safe inbound queue (network thread -> game thread)
    std::mutex s_inboxMutex;
    std::deque<CoSyncTransport::InboxMessage> s_inbox;

    // Throttle spam
    double s_lastTickLog = 0.0;

    // Hard safety cap to prevent runaway memory usage if something floods.
    constexpr size_t kInboxHardCap = 4096;

    bool s_connectionNotified = false; // NEW (fire once)
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
bool CoSyncTransport::InitAsHost()
{
    if (s_initialized && s_isHost)
    {
        LOG_INFO("[Transport] InitAsHost: already initialized as host");
        return true;
    }

    s_initialized = true;
    s_isHost = true;
    s_connectionNotified = false;

    {
        std::lock_guard<std::mutex> lk(s_inboxMutex);
        s_inbox.clear();
    }

    LOG_INFO("[Transport] InitAsHost OK");
    return true;
}

bool CoSyncTransport::InitAsClient(const std::string& connectStr)
{
    if (s_initialized && !s_isHost)
    {
        LOG_INFO("[Transport] InitAsClient: already initialized as client");
        return true;
    }

    s_initialized = true;
    s_isHost = false;
    s_connectionNotified = false;

    {
        std::lock_guard<std::mutex> lk(s_inboxMutex);
        s_inbox.clear();
    }

    LOG_INFO("[Transport] InitAsClient OK (%s)", connectStr.c_str());
    return true;
}

void CoSyncTransport::Shutdown()
{
    if (!s_initialized)
        return;

    LOG_INFO("[Transport] Shutdown");

    s_initialized = false;
    s_isHost = false;
    s_receiveCallback = nullptr;
    s_connectedCallback = nullptr; // NEW
    s_connectionNotified = false;

    {
        std::lock_guard<std::mutex> lk(s_inboxMutex);
        s_inbox.clear();
    }
}

bool CoSyncTransport::IsInitialized() { return s_initialized; }
bool CoSyncTransport::IsHost() { return s_isHost; }

// -----------------------------------------------------------------------------
// Send
// -----------------------------------------------------------------------------
void CoSyncTransport::Send(const std::string& msg)
{
    if (!s_initialized)
    {
        LOG_WARN("[Transport] Send called while not initialized");
        return;
    }

    GNS_Session::Get().SendText(msg);
    LOG_DEBUG("[CoSyncTransport] SEND %zu bytes", msg.size());
}

// -----------------------------------------------------------------------------
// Incoming: called from GNS receive path (may be non-game-thread)
// -----------------------------------------------------------------------------
void CoSyncTransport::ForwardMessage(const std::string& msg, HSteamNetConnection conn)
{
    if (!s_initialized)
        return;

    LOG_INFO("[Transport] ForwardMessage %zu bytes (conn=%u): %.80s",
        msg.size(), conn, msg.c_str());

    std::lock_guard<std::mutex> lk(s_inboxMutex);

    if (s_inbox.size() >= kInboxHardCap)
        s_inbox.pop_front();

    s_inbox.push_back({ msg, conn });
}

// -----------------------------------------------------------------------------
// Optional pull-based drain
// -----------------------------------------------------------------------------
size_t CoSyncTransport::DrainInbox(std::deque<InboxMessage>& out)
{
    out.clear();

    std::lock_guard<std::mutex> lk(s_inboxMutex);
    if (s_inbox.empty())
        return 0;

    out.swap(s_inbox);
    return out.size();
}

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------
void CoSyncTransport::SetReceiveCallback(ReceiveCallback fn)
{
    s_receiveCallback = std::move(fn);
    LOG_INFO("[Transport] Receive callback set");
}

void CoSyncTransport::SetConnectedCallback(ConnectedCallback fn) // NEW
{
    s_connectedCallback = std::move(fn);
    LOG_INFO("[Transport] Connected callback set");
}

// -----------------------------------------------------------------------------
// Tick (game thread)
// -----------------------------------------------------------------------------
void CoSyncTransport::Tick(double now)
{
    if (!s_initialized)
        return;

    GNS_Session::Get().Tick();

    // Detect connection once
    if (!s_connectionNotified && GNS_Session::Get().IsConnected()) // NEW
    {
        s_connectionNotified = true;
        LOG_INFO("[Transport] Connection established");

        if (s_connectedCallback)
            s_connectedCallback();
    }

    std::deque<InboxMessage> drained;
    const size_t count = DrainInbox(drained);

    if (count > 0)
    {
        LOG_DEBUG("[Transport] Drained %zu inbound messages", count);

        if (s_receiveCallback)
        {
            for (auto& m : drained)
                s_receiveCallback(m.text, now, m.conn);
        }
        else
        {
            LOG_WARN("[Transport] Dropped %zu inbound messages (no receive callback)", count);
        }
    }

    if (now - s_lastTickLog > 1.0)
    {
        s_lastTickLog = now;
        LOG_DEBUG("[CoSyncTransport] Tick (inbox=%zu)", count);
    }
}

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------
std::string CoSyncTransport::GetHostConnectString()
{
    return GNS_Session::Get().GetHostConnectString();
}
