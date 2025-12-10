#include "CoSyncTransport.h"
#include "TickHook.h"
#include "ConsoleLogger.h"
#include "GNS_Session.h"

namespace
{
    bool s_initialized = false;
    bool s_isHost = false;

    std::function<void(const std::string&)> s_receiveCallback;
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

    // GNS_Session::StartHost() is called by F4MP_Main::StartHosting() BEFORE this.
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

    // GNS_Session::StartClient() is called by F4MP_Main::StartJoining() BEFORE this.
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
// Tick
//
// Currently the real socket polling is done in GNS_Session::Tick(), which you
// are already calling from your DX11 Present hook. This function exists for
// future flexibility and so CoSyncNet has a stable interface.
// -----------------------------------------------------------------------------
void CoSyncTransport::Tick(double /*now*/)
{
    
    GNS_Session::Get().Tick();
    
	LOG_DEBUG("[CoSyncTransport] Tick");
    
    
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
//
// This is called by GNS_Session when it receives a complete text message.
// It forwards the payload into the higher-level CoSyncNet callback.
// -----------------------------------------------------------------------------
void CoSyncTransport::ForwardMessage(const std::string& msg)
{
    if (s_receiveCallback)
    {
        s_receiveCallback(msg);
    }
    else
    {
        LOG_WARN("[Transport] Received message but no callback is set");
    }
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
