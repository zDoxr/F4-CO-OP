#pragma once

#include <string>
#include <cstdint>

namespace CoSyncVatsSync
{

    bool IsVatsEnter(const std::string& msg);
    bool IsVatsExit (const std::string& msg);


    std::string SerializeVatsEnter(uint32_t entityID, float apCurrent);
    std::string SerializeVatsExit (uint32_t entityID, float apCurrent);


    void OnReceiveVatsEnter(const std::string& msg);
    void OnReceiveVatsExit (const std::string& msg);


    void FlushPendingVatsOps();

    void TickLocalVats(uint32_t myEntityID);


    bool IsEntityInVats(uint32_t entityID);
}
