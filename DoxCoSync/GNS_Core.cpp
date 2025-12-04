#include "GNS_Core.h"
#include "ConsoleLogger.h"

#include "steam/steamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"

static bool g_gnsInitialized = false;

bool GNS_Init()
{
    if (g_gnsInitialized)
        return true;

    SteamDatagramErrMsg errMsg;

    if (!GameNetworkingSockets_Init(nullptr, errMsg))
    {
        LOG_ERROR("[GNS] GameNetworkingSockets_Init FAILED: %s", errMsg);
        return false;
    }

    SteamNetworkingUtils()->SetDebugOutputFunction(
        k_ESteamNetworkingSocketsDebugOutputType_Msg,
        [](ESteamNetworkingSocketsDebugOutputType, const char* msg) {
            LOG_INFO("[GNS] %s", msg);
        }
    );

    g_gnsInitialized = true;
    LOG_INFO("[GNS] Initialized OK");
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
