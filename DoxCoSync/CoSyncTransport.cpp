#include "CoSyncTransport.h"
#include "ConsoleLogger.h"
#include "GNS_Session.h"

namespace
{
    bool s_initialized = false;
    bool s_isHost = false;

    std::function<void(const std::string&)> s_receiveCallback;

    // throttle spam
    double s_lastTickLog = 0.0;
}

// -----------------------------------------------------------------------------
// InitAsHost
// -----------------------------------------------------------------------------
bool CoSyncTransport::InitAsHost()
{
    if (s_initialized && s_isHost)
    {
        LOG_INFO("[Transport] InitAsHost called but already initialized as host");
        return true;
    }

    s_initialized = true;
    s_isHost = true;

    LOG_INFO("[Transport] InitAsHost OK");
    return true;
}

// -----------------------------------------------------------------------------
// InitAsClient
// -----------------------------------------------------------------------------
bool CoSyncTransport::InitAsClient(const std::string& connectStr)
{
    if (s_initialized && !s_isHost)
    {
        LOG_INFO("[Transport] InitAsClient called but already initialized as client");
        return true;
    }

    s_initialized = true;
    s_isHost = false;

    LOG_INFO("[Transport] InitAsClient OK (%s)", connectStr.c_str());
    return true;
}

// -----------------------------------------------------------------------------
// Shutdown
// -----------------------------------------------------------------------------
void CoSyncTransport::Shutdown()
{
    if (!s_initialized)
        return;

    LOG_INFO("[Transport] Shutdown");

    s_initialized = false;
    s_isHost = false;
    s_receiveCallback = nullptr;
}

// -----------------------------------------------------------------------------
// Tick (ONLY place we drive GNS now)
// -----------------------------------------------------------------------------
void CoSyncTransport::Tick(double now)
{
    if (!s_initialized)
        return;

    GNS_Session::Get().Tick();

    // Throttle spam to ~1Hz if you still want it.
    if (now - s_lastTickLog > 1.0)
    {
        s_lastTickLog = now;
        LOG_DEBUG("[CoSyncTransport] Tick");
    }
}

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
    LOG_DEBUG("[CoSyncTransport] SEND %zu bytes: %s", msg.size(), msg.c_str());
}

// -----------------------------------------------------------------------------
// ForwardMessage
// -----------------------------------------------------------------------------
void CoSyncTransport::ForwardMessage(const std::string& msg)
{
    if (s_receiveCallback)
        s_receiveCallback(msg);
    else
        LOG_WARN("[Transport] Received message but no callback is set");
}

// -----------------------------------------------------------------------------
// SetReceiveCallback
// -----------------------------------------------------------------------------
void CoSyncTransport::SetReceiveCallback(std::function<void(const std::string&)> fn)
{
    s_receiveCallback = std::move(fn);
    LOG_INFO("[Transport] Receive callback set");
}

// -----------------------------------------------------------------------------
// GetHostConnectString
// -----------------------------------------------------------------------------
std::string CoSyncTransport::GetHostConnectString()
{
    return GNS_Session::Get().GetHostConnectString();
}
