#pragma once

#include <string>
#include <cstdint>

namespace CoSyncMapSync
{

    bool IsMapMarker(const std::string& msg);


    void BroadcastDiscovery(uint32_t markerFormID);


    void OnReceiveMapMarker(const std::string& msg);


    void FlushPendingMapOps();
}
