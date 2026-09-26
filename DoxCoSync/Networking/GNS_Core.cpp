#include "GNS_Core.h"
#include "ConsoleLogger.h"

#include "steam/steamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"

static bool g_gnsInitialized = false;

// Round 109: GameNetworkingSockets was never given a debug-output callback.
// Per Valve's own integration guidance, k_ESteamNetworkingSocketsDebugOutputType_Bug
// is reserved for internal-assertion-style failures, and WITHOUT a registered
// callback the library's own default handler for Bug-severity messages is a
// hard, low-level abort -- one that bypasses normal exception dispatch
// entirely (no SEH frame ever sees it, no VEH first-chance notification, no
// WER dump, even with a registry LocalDumps collector active). That exactly
// matches this round's mystery crash: dies right after "[GNS] Initialized OK"
// every single test, with zero forensic trace anywhere (no CoSync VEH log
// entry, no minidump, no Windows Event Log entry). Registering this callback
// makes GNS log through our own logger instead of silently killing the
// process, AND (per the library's own documented behavior) having ANY
// callback registered changes a Bug-severity message from "auto-abort" to
// "deliver to the app and let it decide" -- so this should also stop the
// actual crash, not just explain it.
static void GNS_DebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg)
{
    switch (eType)
    {
    case k_ESteamNetworkingSocketsDebugOutputType_Bug:
    case k_ESteamNetworkingSocketsDebugOutputType_Error:
        LOG_ERROR("[GNS-debug] %s", pszMsg);
        break;
    case k_ESteamNetworkingSocketsDebugOutputType_Important:
    case k_ESteamNetworkingSocketsDebugOutputType_Warning:
        LOG_WARN("[GNS-debug] %s", pszMsg);
        break;
    default:
        LOG_INFO("[GNS-debug] %s", pszMsg);
        break;
    }
}

bool GNS_Init()
{
    if (g_gnsInitialized)
        return true;

    SteamDatagramErrMsg errMsg{};

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

    utils->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, GNS_DebugOutput);

    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_Unencrypted, 1);

    // Shared host save: a 4-6 MB world is streamed to each joining client in
    // 128 KB reliable chunks. The default 512 KB send/1 MB receive buffers
    // would reject most of it (k_EResultLimitExceeded), so give GNS room.
    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendBufferSize, 32 * 1024 * 1024);
    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_RecvBufferSize, 32 * 1024 * 1024);
    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_RecvBufferMessages, 16384);
    // GNS paces each connection at ~256 KB/s by default: a 4 MB world took 17 s
    // to arrive (2026-09-25). LAN can take far more; let the rate go up to
    // 32 MB/s (GNS still backs off on loss).
    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendRateMin, 1024 * 1024);
    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendRateMax, 32 * 1024 * 1024);

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
