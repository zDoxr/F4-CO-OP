#include <Windows.h>
#include "f4se/PluginAPI.h"
#include "f4se_version.h"
#include "Logger.h"
#include "ConsoleLogger.h"
#include "F4MP_Main.h"
#include "TickHook.h"
#include "DX11Hook.h"
#include "CoSyncOverlay.h"
#include "SteamDiagnostics.h"
#include "CoSyncSteam.h"
#include "CoSyncSteamManager.h"





// NG-required version export
extern "C"
{
    __declspec(dllexport) F4SEPluginVersionData F4SEPlugin_Version =
    {
        F4SEPluginVersionData::kVersion,

        1,                // Plugin version
        "DoxCoSync",      // Name
        "Doxr",           // Author

        0,
        0,
        { RUNTIME_VERSION_1_11_169, 0 },

        0,
    };
}

static PluginHandle g_pluginHandle = kPluginHandle_Invalid;
static F4SEMessagingInterface* g_messaging = nullptr;

void OnF4SEMessage(F4SEMessagingInterface::Message* msg)
{
    if (!msg)
        return;

    switch (msg->type)
    {
    case F4SEMessagingInterface::kMessage_GameLoaded:
        LOG_INFO("GameLoaded::Initializing CoSync...");

        CoSyncSteamManager::OnGameLoaded();

        f4mp::F4MP_Main::Get().Init();


        LOG_INFO("CoSync: Installing TickHook...");
        InstallTickHook();

        // Safe point to touch Steam.
            if (!CoSyncSteam_IsReady())
            {
                LOG_WARN("[CoSyncSteam] Not ready on GameLoaded (Steam overlay disabled or failure)");
            }

        LOG_INFO("[DX11Hook] Installing Present hook...");
        if (!InitDX11Hook())
        {
            LOG_ERROR("[DX11Hook] FAILED to install Present hook!");
        }
        else
        {
            LOG_INFO("[DX11Hook] Present hook installed successfully.");
        }

        break;

        
    }
}


extern "C"
__declspec(dllexport)
bool F4SEPlugin_Load(const F4SEInterface* f4se)
{
    LOG_INFO("CoSync - F4SEPlugin_Load");


    if (!CoSyncSteam_Init())

    {
        LOG_ERROR("[CoSyncSteam] Init failed (Steam friends/invites disabled)");
    }

    g_pluginHandle = f4se->GetPluginHandle();

    g_messaging = (F4SEMessagingInterface*)f4se->QueryInterface(kInterface_Messaging);
    if (!g_messaging)
    {
        LOG_ERROR("Messaging interface missing!");
        return false;
    }
    


    LOG_INFO("Registering GameLoaded listener...");
    g_messaging->RegisterListener(g_pluginHandle, "F4SE", OnF4SEMessage);

    LOG_INFO("Plugin load complete");
    return true;
}
