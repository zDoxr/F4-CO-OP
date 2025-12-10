#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct CoSyncFriendInfo
{
    uint64_t    steamID = 0;
    std::string name;
};

namespace CoSyncSteamManager
{
    // Call once from your F4SE GameLoaded handler 
    // (e.g., in OnF4SEMessage when msg->type == kMessage_GameLoaded).
    void OnGameLoaded();

    // Call on plugin shutdown if you want to be clean.
    void Shutdown();

    // Returns true once we've successfully pulled identity from Steam.
    bool HasIdentity();

    // Returns cached persona + self SteamID if available.
    bool GetIdentity(std::string& outPersona, uint64_t& outSteamID);

    // Returns cached friend list snapshot (may be empty if not ready).
    std::vector<CoSyncFriendInfo> GetFriendsSnapshot();
}
