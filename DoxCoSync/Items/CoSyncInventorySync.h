#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace CoSyncInventorySync
{
    struct ItemEntry
    {
        uint32_t formID = 0;
        int32_t  count  = 0;
    };


    bool IsInventory(const std::string& msg);

    std::string SerializeInventory(uint32_t entityID, const std::vector<ItemEntry>& items);

    std::vector<ItemEntry> SnapshotInventory(void* actor);


    void OnReceiveInventory(const std::string& msg);


    void FlushPendingInventoryOps();
}
