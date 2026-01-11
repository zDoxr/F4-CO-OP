#include "ITypes.h"
#include "IErrors.h"
#include "IDebugLog.h"
#include <Windows.h>
#include "PluginAPI.h"
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
#include "CoSyncPlayerManager.h"
#include "CoSyncSpawnTasks.h"



extern bool RegisterCoSyncPapyrus(VirtualMachine* vm);




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
        { RUNTIME_VERSION_1_11_191, 0 },

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


bool RegisterCoSyncPapyrus(VirtualMachine* vm);




extern "C"
__declspec(dllexport)
bool F4SEPlugin_Load(const F4SEInterface* f4se)
{
    LOG_INFO("CoSync - F4SEPlugin_Load");

    // ------------------------------------------------------------
    // Steam hook
    // ------------------------------------------------------------
    if (!CoSyncSteam_Init())
    {
        LOG_ERROR("[CoSyncSteam] Init failed (Steam friends/invites disabled)");
    }

    g_pluginHandle = f4se->GetPluginHandle();

    // ------------------------------------------------------------
    // Messaging
    // ------------------------------------------------------------
    g_messaging = (F4SEMessagingInterface*)f4se->QueryInterface(kInterface_Messaging);
    if (!g_messaging)
    {
        LOG_ERROR("Messaging interface missing!");
        return false;
    }

    LOG_INFO("Registering GameLoaded listener...");
    g_messaging->RegisterListener(g_pluginHandle, "F4SE", OnF4SEMessage);

    // ------------------------------------------------------------
    // Papyrus
    // ------------------------------------------------------------
    auto* papyrus = (F4SEPapyrusInterface*)f4se->QueryInterface(kInterface_Papyrus);
    if (!papyrus)
    {
        LOG_ERROR("Papyrus interface missing!");
        return false;
    }

    if (!papyrus->Register(RegisterCoSyncPapyrus))
    {
        LOG_ERROR("Papyrus->Register(RegisterCoSyncPapyrus) failed!");
        return false;
    }

    LOG_INFO("Papyrus registration queued successfully.");

    // ------------------------------------------------------------
    // Task interface (CRITICAL)
    // ------------------------------------------------------------
    auto* taskIFace = (F4SETaskInterface*)f4se->QueryInterface(kInterface_Task);
    if (!taskIFace)
    {
        LOG_ERROR("Task interface missing!");
    }
    else
    {
        CoSyncSpawnTasks::Init(taskIFace);
    }

    LOG_INFO("Plugin load complete");
    return true;
}


