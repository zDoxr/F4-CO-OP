#pragma once

#include <string>
#include <cstdint>

namespace CoSyncCompanionSync
{

    bool IsCompanionRequest(const std::string& msg);
    bool IsCompanionDismiss(const std::string& msg);


    std::string SerializeRequest(uint32_t ownerEntityID, uint32_t baseFormID);
    std::string SerializeDismiss(uint32_t entityID);

    void RequestCompanion(uint32_t ownerEntityID, uint32_t baseFormID);
    void DismissCompanion(uint32_t entityID);


    void OnReceiveRequest(const std::string& msg);
    void OnReceiveDismiss(const std::string& msg);

    void FlushPendingCompanionOps();

    void TickCompanionFollowNudge(double now);

}
