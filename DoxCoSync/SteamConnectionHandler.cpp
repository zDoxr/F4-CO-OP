#include "F4MP_Librg.h"
#include "SteamConnectionHandler.h"

#include <steam/steamnetworkingtypes.h>
#include <algorithm>

using namespace f4mp::librg;

// ---------------------------------------------------------------------------
// SimpleEvent: lightweight wrapper to fire connection-related callbacks
// ---------------------------------------------------------------------------
class SimpleEvent : public f4mp::librg::Event {
public:
    SimpleEvent(f4mp::librg::Event::Type t, f4mp::librg::Librg* owner)
        : f4mp::librg::Event(t, owner) {}  // uses (Type, Librg*) ctor
};


// ---------------------------------------------------------------------------
// SteamNet connection state dispatcher
// ---------------------------------------------------------------------------
void SteamConnectionHandler::SteamNetConnectionStatusChanged(
    SteamNetConnectionStatusChangedCallback_t* info)
{
    if (!info)
        return;

    Librg* net = reinterpret_cast<Librg*>(info->m_info.m_nUserData);
    if (!net)
        return;

    switch (info->m_info.m_eState)
    {
    case k_ESteamNetworkingConnectionState_Connecting:
        HandleConnecting(*net, info);
        break;

    case k_ESteamNetworkingConnectionState_Connected:
        HandleConnected(*net, info);
        break;

    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        HandleClosed(*net, info);
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Server receives a new incoming connection
// ---------------------------------------------------------------------------
void SteamConnectionHandler::HandleConnecting(
    Librg& net, SteamNetConnectionStatusChangedCallback_t* info)
{
    if (!net.IsServer())
        return;

    net.Steam()->AcceptConnection(info->m_hConn);
    net.Steam()->SetConnectionUserData(info->m_hConn, (uint64)&net);

    if (net.onConnectionRequest)
    {
        SimpleEvent e(1, &net);
        net.onConnectionRequest(e);
    }
}

// ---------------------------------------------------------------------------
// Connection fully established
// ---------------------------------------------------------------------------
void SteamConnectionHandler::HandleConnected(
    Librg& net, SteamNetConnectionStatusChangedCallback_t* info)
{
    if (net.IsServer())
        net.Clients().push_back(info->m_hConn);
    else
        const_cast<HSteamNetConnection&>(net.ServerConn()) = info->m_hConn;

    if (net.onConnectionAccept)
    {
        SimpleEvent e(2, &net);
        net.onConnectionAccept(e);
    }
}

// ---------------------------------------------------------------------------
// Connection closed
// ---------------------------------------------------------------------------
void SteamConnectionHandler::HandleClosed(
    Librg& net, SteamNetConnectionStatusChangedCallback_t* info)
{
    if (net.IsServer())
    {
        auto& cl = net.Clients();
        auto it = std::find(cl.begin(), cl.end(), info->m_hConn);
        if (it != cl.end())
            cl.erase(it);
    }
    else
    {
        if (net.ServerConn() == info->m_hConn)
        {
            const_cast<HSteamNetConnection&>(net.ServerConn()) =
                k_HSteamNetConnection_Invalid;
        }
    }

    if (net.onConnectionRefuse)
    {
        SimpleEvent e(3, &net);
        net.onConnectionRefuse(e);
    }

    net.Steam()->CloseConnection(info->m_hConn, 0, nullptr, false);
}
