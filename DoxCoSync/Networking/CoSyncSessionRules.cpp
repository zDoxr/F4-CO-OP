#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncSessionRules.h"

#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncGameAPI.h"
#include "ConsoleLogger.h"

#include <atomic>
#include <string>

namespace
{
    std::atomic<bool> s_pvp{ false };
    double            s_nextSend = 0.0;

    std::string IniPath()
    {
        char exe[MAX_PATH] = {};
        if (!GetModuleFileNameA(nullptr, exe, MAX_PATH))
            return std::string();
        std::string ini = exe;
        ini = ini.substr(0, ini.find_last_of("\\/") + 1) + "Data\\F4SE\\Plugins\\CoSync.ini";
        return ini;
    }

    bool ReadSavedPvp()
    {
        const std::string ini = IniPath();
        return !ini.empty() && GetPrivateProfileIntA("Session", "bPVP", 0, ini.c_str()) != 0;
    }

    void Broadcast()
    {
        if (CoSyncTransport::IsInitialized() && CoSyncTransport::IsHost() && CoSyncNet::IsConnected())
            CoSyncTransport::Send(std::string("RULES|") + (s_pvp.load() ? "1" : "0"));
    }
}

namespace CoSyncSessionRules
{
    bool IsPvpEnabled() { return s_pvp.load(std::memory_order_relaxed); }

    bool CanEdit()
    {
        return CoSyncTransport::IsInitialized() && CoSyncTransport::IsHost();
    }

    void HostSetPvp(bool pvp)
    {
        if (!CanEdit())
        {
            LOG_WARN("[Rules] Only the host can change PVE / PVP");
            return;
        }
        const bool was = s_pvp.exchange(pvp);
        const std::string ini = IniPath();
        if (!ini.empty())
            WritePrivateProfileStringA("Session", "bPVP", pvp ? "1" : "0", ini.c_str());
        Broadcast();
        if (was != pvp)
        {
            LOG_UPDATED("[Rules] Host set the session to %s", pvp ? "PVP (player damage ON)" : "PVE (player damage OFF)");
            CoSyncGameAPI::QueueNotification(pvp ? "Friendly fire on"
                                                 : "Friendly fire off");
        }
    }

    bool IsRulesMessage(const std::string& msg)
    {
        return msg.rfind("RULES|", 0) == 0;
    }

    void OnReceive(const std::string& msg, bool isHostRole)
    {
        if (isHostRole || msg.size() < 7)
            return;
        const bool pvp = msg[6] == '1';
        if (s_pvp.exchange(pvp) != pvp)
        {
            LOG_UPDATED("[Rules] Host's session rule: %s", pvp ? "PVP" : "PVE");
            CoSyncGameAPI::QueueNotification(pvp ? "Friendly fire on"
                                                 : "Friendly fire off");
        }
    }

    void Tick(double now)
    {
        if (!CoSyncTransport::IsInitialized() || !CoSyncTransport::IsHost() || !CoSyncNet::IsConnected())
            return;
        if (now < s_nextSend)
            return;
        s_nextSend = now + 5.0;
        Broadcast();
    }

    void Reset()
    {
        // The host's saved choice applies to its next session; a client is
        // PVE until the host's RULES arrives.
        s_pvp.store(CoSyncTransport::IsInitialized() && CoSyncTransport::IsHost() ? ReadSavedPvp() : false);
        s_nextSend = 0.0;
        LOG_INFO("[Rules] Session rules reset: %s", s_pvp.load() ? "PVP" : "PVE");
    }
}
