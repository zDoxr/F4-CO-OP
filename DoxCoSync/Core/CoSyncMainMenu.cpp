#include "CoSyncMainMenu.h"
#include "PluginAPI.h"
#include "ScaleformCallbacks.h"
#include "ScaleformMovie.h"
#include "ConsoleLogger.h"
#include "CoSyncMenuInject.h"
#include "CoSyncRequirementsLog.h"

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <Windows.h>
#include <string>

#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "GNS_Session.h"

// AS3 calls: openMultiplayer(name:String, isHost:Boolean, ip:String)
class CoSync_OnMultiplayerClick : public GFxFunctionHandler
{
public:
    void Invoke(Args* args) override
    {
        LOG_REQUIREMENT("CoSync_OnMultiplayerClick::Invoke", "Needs at least 3 arguments, which is checked. Expects the main menu to send a name, a host flag, and an IP address.");
        if (!args || args->numArgs < 3)
        {
            LOG_WARN("[CoSyncMainMenu] openMultiplayer: expected 3 args, got %d",
                args ? (int)args->numArgs : -1);
            return;
        }

        const char* name   = args->args[0].IsString() ? args->args[0].GetString() : "Survivor";
        bool        isHost = args->args[1].GetBool();
        const char* ip     = args->args[2].IsString() ? args->args[2].GetString() : "";

        std::string sName = (name && *name) ? name : "Survivor";
        std::string sIP   = ip ? ip : "";

        LOG_INFO("[CoSyncMainMenu] openMultiplayer — isHost=%d  name='%s'  ip='%s'",
            isHost ? 1 : 0, sName.c_str(), sIP.c_str());

        CoSyncNet::SetMyName(sName);

        if (isHost)
        {
            LOG_INFO("[CoSyncMainMenu] HOST — binding all interfaces");
            CoSyncTransport::InitAsHost();
            GNS_Session::Get().StartHost("");
            CoSyncNet::ScheduleInit(true);
        }
        else
        {
            if (sIP.find(':') == std::string::npos)
                sIP += ":48000";
            LOG_INFO("[CoSyncMainMenu] CLIENT — connecting to %s", sIP.c_str());
            CoSyncTransport::InitAsClient(sIP);
            GNS_Session::Get().StartClient(sIP);
            CoSyncNet::ScheduleInit(false);
        }
    }
};

static bool OnScaleformRegister(GFxMovieView* view, GFxValue* f4seRoot)
{
    LOG_REQUIREMENT("(global)::OnScaleformRegister", "Safe to call with a missing view or root — it checks for that and just returns. Expects F4SE to call this once for each menu screen it registers.");
    if (!view || !view->movieRoot)
        return true;
    if (!f4seRoot || !f4seRoot->objectInterface)
        return true;

    RegisterFunction<CoSync_OnMultiplayerClick>(f4seRoot, view->movieRoot, "openMultiplayer");
    LOG_INFO("[CoSyncMainMenu] openMultiplayer registered");

    // This callback fires for every menu's movie as soon as F4SE injects
    // root.f4se into it -- including MainMenu, well before g_ui's menuTable
    // is populated. Piggyback the MULTIPLAYER row injection here; it's a
    // no-op (just logs) for any movie that isn't MainMenu.
    CoSyncMenuInject_OnScaleformView(view);

    return true;
}

void CoSyncMainMenu_Register(void* f4seScaleformRaw)
{
    LOG_REQUIREMENT("(global)::CoSyncMainMenu_Register", "Needs a valid Scaleform interface pointer from F4SE. Meant to be called once when the plugin loads.");
    auto* sf = reinterpret_cast<F4SEScaleformInterface*>(f4seScaleformRaw);
    if (!sf)
    {
        LOG_ERROR("[CoSyncMainMenu] Scaleform interface is NULL");
        return;
    }

    if (!sf->Register("DoxCoSync", OnScaleformRegister))
        LOG_ERROR("[CoSyncMainMenu] RegisterScaleformPlugin failed");
    else
        LOG_INFO("[CoSyncMainMenu] Scaleform plugin registered");
}
