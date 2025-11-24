#include "Version.h"
#include "RuntimeVersion.h"
#include "F4MP_Main.h"
#include "Logger.h"

#include "f4se/PluginAPI.h"        // minimal and required only


static PluginHandle g_pluginHandle = kPluginHandle_Invalid;
static const F4SEMessagingInterface* g_messaging = nullptr;

extern "C" {

    bool F4SEPlugin_Query(const F4SEInterface* f4se, PluginInfo* info)
    {
        info->infoVersion = PluginInfo::kInfoVersion;
        info->name = "F4MP_SteamBackend";
        info->version = F4MP_VERSION;

        g_pluginHandle = f4se->GetPluginHandle();

        uint32_t runtime = f4se->runtimeVersion;
        F4MP_LOG("Detected runtime %u", runtime);

        if (!F4MP_IsRuntimeCompatible(runtime))
        {
            F4MP_LOG("Unsupported Fallout 4 runtime.");
            return false;
        }

        return true;
    }

    bool F4SEPlugin_Load(const F4SEInterface* f4se)
    {
        F4MP_LOG("Steam backend F4SE plugin loading...");

        g_messaging = (F4SEMessagingInterface*)f4se->QueryInterface(kInterface_Messaging);

        if (g_messaging)
        {
            g_messaging->RegisterListener(g_pluginHandle, "F4SE",
                [](F4SEMessagingInterface::Message* msg)
                {
                    if (msg->type == F4SEMessagingInterface::kMessage_GameLoaded)
                    {
                        F4MP_LOG("Game Loaded -> init F4MP");
                        f4mp::F4MP_Main::Init();
                    }
                });
        }

        return true;
    }
}
