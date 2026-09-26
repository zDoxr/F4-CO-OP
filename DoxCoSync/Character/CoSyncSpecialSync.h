#pragma once

#include <string>
#include <cstdint>

class Actor;

namespace CoSyncSpecialSync
{

    bool IsSpecial(const std::string& msg);


    std::string SerializeSpecial(uint32_t entityID, uint32_t avFormID, float value);


    void OnReceiveSpecial(const std::string& msg);


    void FlushPendingSpecialOps();

    void TickLocalSpecial(uint32_t myEntityID, Actor* player);
}
