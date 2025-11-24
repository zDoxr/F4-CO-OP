#pragma once

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include "F4MP_Librg.h"

namespace f4mp {
    namespace librg {

        class SteamConnectionHandler
        {
        public:
            // Global callback entry point registered with SteamNetworkingUtils
            static void SteamNetConnectionStatusChanged(
                SteamNetConnectionStatusChangedCallback_t* info
            );

        private:
            // Internal handlers for each connection state
            static void HandleConnecting(
                Librg& net,
                SteamNetConnectionStatusChangedCallback_t* info
            );

            static void HandleConnected(
                Librg& net,
                SteamNetConnectionStatusChangedCallback_t* info
            );

            static void HandleClosed(
                Librg& net,
                SteamNetConnectionStatusChangedCallback_t* info
            );
        };

    } // namespace librg
} // namespace f4mp
