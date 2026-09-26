#pragma once

#include <string>
#include <cstdint>

class Actor;

namespace CoSyncConsumableSync
{

    bool IsConsumable(const std::string& msg);
    bool IsSpell(const std::string& msg);


    std::string SerializeConsumable(uint32_t entityID, uint32_t itemFormID);
    std::string SerializeSpell(uint32_t entityID, uint32_t effectFormID);


    void OnReceiveConsumable(const std::string& msg);
    void OnReceiveSpell(const std::string& msg);


    void FlushPendingConsumableOps();
}
