// F4MP_Plugin.cpp – DoxCoSync / F4MP_Main integration via F4SE messaging

#include <Windows.h>
#include <ShlObj.h>     // SHGetFolderPathA, CSIDL_*
#include <cstdio>
#include <cstdarg>

#include "f4se/PluginAPI.h"        // F4SE core interfaces


#include "F4MP_Main.h"             // your main multiplayer manager

#pragma comment(lib, "Shell32.lib")

// ---------------------------------------------------------------------
// Small local logger -> Documents\My Games\Fallout4\F4SE\DoxCoSync.log
// ---------------------------------------------------------------------
static void SafeLog(const char* fmt, ...)
{
    static bool s_pathInit = false;
    static char s_logPath[MAX_PATH] = {};

    if (!s_pathInit) {
        char docsPath[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathA(
            nullptr,
            CSIDL_PERSONAL,      // "My Documents"
            nullptr,
            SHGFP_TYPE_CURRENT,
            docsPath)))
        {
            // docsPath: C:\Users\<User>\Documents
            std::snprintf(
                s_logPath,
                sizeof(s_logPath),
                "%s\\My Games\\Fallout4\\F4SE\\DoxCoSync.log",
                docsPath
            );
        }
        else {
            // Fallback: current dir
            std::snprintf(s_logPath, sizeof(s_logPath), "DoxCoSync.log");
        }
        s_pathInit = true;
    }

    FILE* f = nullptr;
    if (fopen_s(&f, s_logPath, "a") != 0 || !f)
        return;

    char buffer[2048];

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    std::fprintf(f, "%s\n", buffer);
    std::fclose(f);
}

// Convenience macro, similar to F4MP_LOG
#define F4MP_LOG(...) SafeLog(__VA_ARGS__)

// ---------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------
static PluginHandle            g_pluginHandle = kPluginHandle_Invalid;
static F4SEMessagingInterface* g_messaging = nullptr;

// ---------------------------------------------------------------------
// F4SE message callback
// ---------------------------------------------------------------------
static void OnF4SEMessage(F4SEMessagingInterface::Message* msg)
{
    if (!msg)
        return;

    switch (msg->type) {
    case F4SEMessagingInterface::kMessage_PostLoad:
        F4MP_LOG("OnF4SEMessage: PostLoad");
        break;

    case F4SEMessagingInterface::kMessage_GameLoaded:
        F4MP_LOG("OnF4SEMessage: GameLoaded -> calling F4MP_Main::Init()");
        f4mp::F4MP_Main::Init();
        break;

    default:
        F4MP_LOG("OnF4SEMessage: type = %u (ignored)", msg->type);
        break;
    }
}

// ---------------------------------------------------------------------
// F4SE exports
// ---------------------------------------------------------------------
extern "C"
{
    __declspec(dllexport)
        bool F4SEPlugin_Query(const F4SEInterface* f4se, PluginInfo* info)
    {
        F4MP_LOG("F4SEPlugin_Query: entered");

        if (!f4se || !info) {
            F4MP_LOG("F4SEPlugin_Query: f4se or info is null -> fail");
            return false;
        }

        info->infoVersion = PluginInfo::kInfoVersion;
        info->name = "DoxCoSync_F4MP";
        info->version = 1;  // your plugin version

        g_pluginHandle = f4se->GetPluginHandle();

        F4MP_LOG("F4SEPlugin_Query: pluginHandle = %u", g_pluginHandle);
        F4MP_LOG("F4SEPlugin_Query: runtimeVersion = %u", f4se->runtimeVersion);

        // For now: accept any runtime (you're on next-gen)
        F4MP_LOG("F4SEPlugin_Query: returning true (no runtime check)");
        return true;
    }

    __declspec(dllexport)
        bool F4SEPlugin_Load(const F4SEInterface* f4se)
    {
        F4MP_LOG("F4SEPlugin_Load: entered");

        if (!f4se) {
            F4MP_LOG("F4SEPlugin_Load: f4se is null -> fail");
            return false;
        }

        g_messaging = (F4SEMessagingInterface*)f4se->QueryInterface(kInterface_Messaging);

        if (!g_messaging) {
            F4MP_LOG("F4SEPlugin_Load: ERROR - messaging interface unavailable");
            return true; // plugin can still load, just no messages
        }

        // Register to listen to "F4SE" messages (same as original F4MP)
        if (!g_messaging->RegisterListener(g_pluginHandle, "F4SE", OnF4SEMessage)) {
            F4MP_LOG("F4SEPlugin_Load: ERROR - RegisterListener failed");
        }
        else {
            F4MP_LOG("F4SEPlugin_Load: Registered F4SE messaging listener");
        }

        F4MP_LOG("F4SEPlugin_Load: finished");
        return true;
    }
}
