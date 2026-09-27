#include "CoSyncTransport.h"

#include "Logger.h"
#include "ConsoleLogger.h"
#include "GNS_Session.h"

#include <algorithm>
#include <utility>

namespace
{
    bool s_initialized = false;
    bool s_isHost = false;

    CoSyncTransport::ReceiveCallback      s_receiveCallback;
    CoSyncTransport::ConnectedCallback    s_connectedCallback;
    CoSyncTransport::DisconnectedCallback s_disconnectedCallback;


    std::mutex s_inboxMutex;
    std::deque<CoSyncTransport::InboxMessage> s_inbox;


    double s_lastTickLog = 0.0;


    // P1 (2026-09-26): the game thread stops ticking for 8-32 s during loading
    // screens (26 Sep logs) while the network thread keeps queueing. One peer
    // queued 1237 messages in a 32 s stall; 8 players project to ~9000, and
    // the old 4096 cap then dropped the OLDEST message -- reliable CREATE,
    // DESTROY, KILL and quest events included. The cap now covers that load
    // with margin (~30 MB worst case), and when it is ever reached a
    // superseded state message is evicted first (see ForwardMessage).
    constexpr size_t kInboxHardCap = 65536;

    // Position (EU) and pose (BONES) messages are pure state: the next one
    // for the same entity replaces them completely, so evicting one loses
    // nothing permanent. Every other message is an event.
    bool IsSupersededState(const std::string& m)
    {
        return m.rfind("EU|", 0) == 0 || m.rfind("BONES|", 0) == 0;
    }

    size_t s_evictedState = 0;
    size_t s_droppedEvents = 0;


    bool s_wasConnected = false;
}

bool CoSyncTransport::InitAsHost()
{
    if (s_initialized && s_isHost)
    {
        LOG_INFO("[Transport] InitAsHost: already initialized as host");
        return true;
    }

    s_initialized = true;
    s_isHost = true;
    s_wasConnected = false;

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
    s_wasConnected = false;

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
    s_receiveCallback      = nullptr;
    s_connectedCallback    = nullptr;
    s_disconnectedCallback = nullptr;
    s_wasConnected = false;

    {
        std::lock_guard<std::mutex> lk(s_inboxMutex);
        s_inbox.clear();
    }
}

bool CoSyncTransport::IsInitialized() { return s_initialized; }
bool CoSyncTransport::IsHost() { return s_isHost; }

void CoSyncTransport::Send(const std::string& msg, bool reliable)
{
    if (!s_initialized)
    {
        LOG_WARN("[Transport] Send called while not initialized");
        return;
    }

    GNS_Session::Get().SendText(msg, reliable);
    LOG_DEBUG("[CoSyncTransport] SEND %zu bytes%s", msg.size(), reliable ? "" : " (unreliable)");
}

void CoSyncTransport::SendTo(HSteamNetConnection conn, const std::string& msg, bool reliable)
{
    if (!s_initialized)
        return;
    GNS_Session::Get().SendTextTo(conn, msg, reliable);
}

void CoSyncTransport::ForwardMessage(const std::string& msg, HSteamNetConnection conn)
{
    if (!s_initialized)
        return;

    // World chunks are raw save bytes; profiles are large -- log the size only.
    if (msg.rfind("WORLDCHUNK|", 0) == 0 || msg.rfind("PROFILEUP|", 0) == 0)
        LOG_DEBUG("[Transport] ForwardMessage %zu bytes (conn=%u): %.10s...", msg.size(), conn, msg.c_str());
    // EU: 80 chars cut off the sender timestamp (field 6), which is what a
    // send-vs-arrival jitter measurement needs (Phase 5 audit). 170 covers
    // entity|flags|pos|rot|vel|timestamp for any in-world coordinate.
    else if (msg.rfind("EU|", 0) == 0)
        LOG_INFO("[Transport] ForwardMessage %zu bytes (conn=%u): %.170s",
            msg.size(), conn, msg.c_str());
    else
        LOG_INFO("[Transport] ForwardMessage %zu bytes (conn=%u): %.80s",
            msg.size(), conn, msg.c_str());

    std::lock_guard<std::mutex> lk(s_inboxMutex);

    if (s_inbox.size() >= kInboxHardCap)
    {
        // Evict the oldest superseded state message; only if the whole queue
        // is events does an event get dropped, and that is logged as an error.
        auto victim = std::find_if(s_inbox.begin(), s_inbox.end(),
            [](const CoSyncTransport::InboxMessage& m) { return IsSupersededState(m.text); });
        if (victim != s_inbox.end())
        {
            s_inbox.erase(victim);
            if ((s_evictedState++ % 1000) == 0)
                LOG_WARN("[Transport] Inbox full (%zu) -- evicting stale position/pose updates (%zu so far)",
                    kInboxHardCap, s_evictedState);
        }
        else
        {
            LOG_ERROR("[Transport] Inbox full (%zu) of events only -- dropping the oldest event '%.40s' (%zu dropped)",
                kInboxHardCap, s_inbox.front().text.c_str(), ++s_droppedEvents);
            s_inbox.pop_front();
        }
    }

    s_inbox.push_back({ msg, conn });
}

size_t CoSyncTransport::DrainInbox(std::deque<InboxMessage>& out)
{
    out.clear();

    std::lock_guard<std::mutex> lk(s_inboxMutex);
    if (s_inbox.empty())
        return 0;

    out.swap(s_inbox);
    return out.size();
}

void CoSyncTransport::SetReceiveCallback(ReceiveCallback fn)
{
    s_receiveCallback = std::move(fn);
    LOG_INFO("[Transport] Receive callback set");
}

void CoSyncTransport::SetConnectedCallback(ConnectedCallback fn)
{
    s_connectedCallback = std::move(fn);
    LOG_INFO("[Transport] Connected callback set");
}

void CoSyncTransport::SetDisconnectedCallback(DisconnectedCallback fn)
{
    s_disconnectedCallback = std::move(fn);
    LOG_INFO("[Transport] Disconnected callback set");
}

void CoSyncTransport::Tick(double now)
{
    if (!s_initialized)
        return;

    GNS_Session::Get().Tick();

    const bool connectedNow = GNS_Session::Get().IsConnected();

    if (connectedNow && !s_wasConnected)
    {
        s_wasConnected = true;
        LOG_INFO("[Transport] Connection established (edge)");

        if (s_connectedCallback)
            s_connectedCallback();
    }
    else if (!connectedNow && s_wasConnected)
    {
        s_wasConnected = false;
        LOG_WARN("[Transport] Connection lost (edge) — firing disconnect callback");
        if (s_disconnectedCallback)
            s_disconnectedCallback();
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
        LOG_DEBUG("[CoSyncTransport] Tick (drained=%zu connected=%d)", count, connectedNow ? 1 : 0);
    }
}

std::string CoSyncTransport::GetHostConnectString()
{
    return GNS_Session::Get().GetHostConnectString();
}
