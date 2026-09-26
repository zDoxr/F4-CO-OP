#pragma once

#include <cstdint>
#include <string>

namespace CoSyncTimeSync
{
    void Init();
    void Shutdown();


    void Tick(double now);

    void ForceOverrideOnGameThread();


    void OnReceiveTime(const std::string& msg);


    void OnReceiveWeather(const std::string& msg);

    uint32_t GetLockedWeatherFormID();
}
