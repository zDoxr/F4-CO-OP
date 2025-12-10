#include "GNS_Core.h"
#include "ConsoleLogger.h"

#include "steam/steamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"

static bool g_gnsInitialized = false;

bool GNS_Init()
{
    if (g_gnsInitialized)
        return true;

    SteamDatagramErrMsg errMsg{};

    //
    // Correct call: GameNetworkingSockets_Init(settings, errMsg)
    // We pass nullptr for the settings pointer.
    //
    if (!GameNetworkingSockets_Init(nullptr, errMsg))
    {
        LOG_ERROR("[GNS] GameNetworkingSockets_Init failed: %s", errMsg);
        return false;
    }

    auto* utils = SteamNetworkingUtils();
    if (!utils)
    {
        LOG_ERROR("[GNS] SteamNetworkingUtils is NULL");
        return false;
    }

    //
    // Required for Hamachi / LAN / direct IP use:
    //
    //  - Allow un-authenticated peer connections
    //  - Disable all encryption (performance gain)
    //  - Disable Steam relay routing (not needed)
    //
    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_Unencrypted, 1);
    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SDRClient_ForceRelayCluster, 0);

    g_gnsInitialized = true;

    LOG_INFO("[GNS] Initialized OK (force insecure mode)");
    return true;
}

void GNS_Shutdown()
{
    if (!g_gnsInitialized)
        return;

    LOG_INFO("[GNS] Shutdown");
    GameNetworkingSockets_Kill();
    g_gnsInitialized = false;
}
