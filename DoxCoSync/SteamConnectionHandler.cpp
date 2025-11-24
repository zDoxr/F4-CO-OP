#include "SteamConnectionHandler.h"
#include "F4MP_Librg.h"

#include <steam/steamnetworkingtypes.h>
#include <algorithm>
#include <cstdint>

namespace f4mp {
    namespace librg {

        // ---------------------------------------------------------------------------
        // SimpleEvent: lightweight wrapper to fire connection-related callbacks
        // ---------------------------------------------------------------------------
        class SimpleEvent : public Event
        {
        public:
            SimpleEvent(Event::Type t, Librg* owner)
                : Event(owner, t)   // Event(Librg* owner, Type type)
            {
            }
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
            net.Steam()->SetConnectionUserData(
                info->m_hConn,
                (std::uint64_t)&net
            );

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
                net.SetServerConn(info->m_hConn);


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
                    net.SetServerConn(k_HSteamNetConnection_Invalid);

                }
            }

            if (net.onConnectionRefuse)
            {
                SimpleEvent e(3, &net);
                net.onConnectionRefuse(e);
            }

            net.Steam()->CloseConnection(info->m_hConn, 0, nullptr, false);
        }

    } // namespace librg
} // namespace f4mp
