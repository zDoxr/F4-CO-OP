#pragma once

#include "CoSyncInventorySync.h"

#include <string>
#include <cstdint>
#include <vector>

namespace CoSyncLootSync
{

    void     SetSessionSalt(uint32_t salt);
    uint32_t GetSessionSalt();

    std::vector<CoSyncInventorySync::ItemEntry> GeneratePersonalLoot(
        const std::vector<CoSyncInventorySync::ItemEntry>& baseItems,
        uint32_t npcEntityID,
        uint32_t targetPlayerEntityID);

    void HostBroadcastPersonalLoot(
        uint32_t npcEntityID,
        const std::vector<CoSyncInventorySync::ItemEntry>& baseItems);

    bool IsSalt        (const std::string& msg);
    bool IsPersonalLoot(const std::string& msg);

    std::string SerializeSalt(uint32_t salt);

    std::string SerializePersonalLoot(
        uint32_t targetEntityID,
        uint32_t sourceEntityID,
        const std::vector<CoSyncInventorySync::ItemEntry>& items);

    void OnReceiveSalt        (const std::string& msg);
    void OnReceivePersonalLoot(const std::string& msg);

    void FlushPendingLootOps();

    void OnContainerFirstOpen(uint32_t containerRefFormID, uint32_t playerEntityID);

}
