#include "ConsoleLogger.h"
#include <Windows.h>

static void TryResolve(const char* name)
{
    static HMODULE hSteam = GetModuleHandleA("steam_api64.dll");
    if (!hSteam)
    {
        LOG_DEBUG("[SteamDiag] steam_api64.dll NOT loaded!");
        return;
    }

    FARPROC fn = GetProcAddress(hSteam, name);
    if (fn)
        LOG_DEBUG("[SteamDiag] FOUND  %-35s  @ %p", name, fn);
    else
        LOG_DEBUG("[SteamDiag] MISSING %-35s", name);
}

void RunSteamDiagnostics()
{
    LOG_DEBUG("========== SteamAPI Diagnostics ==========");

    HMODULE h = GetModuleHandleA("steam_api64.dll");
    LOG_DEBUG("[SteamDiag] steam_api64.dll = %p", h);

    // Core API
    TryResolve("SteamAPI_Init");
    TryResolve("SteamAPI_Shutdown");
    TryResolve("SteamAPI_RestartAppIfNecessary");

    // Common interfaces
    TryResolve("SteamInternal_ContextInit");
    TryResolve("SteamInternal_CreateInterface");
    TryResolve("SteamAPI_GetHSteamUser");
    TryResolve("SteamAPI_GetHSteamPipe");

    // Possible exposed Steamworks interfaces
    TryResolve("SteamUser");
    TryResolve("SteamFriends");
    TryResolve("SteamUtils");
    TryResolve("SteamNetworking");
    TryResolve("SteamMatchmaking");
    TryResolve("SteamMatchmakingServers");
    TryResolve("SteamGameServer");
    TryResolve("SteamNetworkingSockets");
    TryResolve("SteamNetworkingUtils");
    TryResolve("SteamInventory");

    LOG_DEBUG("==========================================");
}
