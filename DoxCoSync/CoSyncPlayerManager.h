#pragma once

#include <unordered_map>
#include <string>

#include "CoSyncPlayer.h"
#include "PlayerStatePacket.h"

class CoSyncPlayerManager
{
public:
    void ProcessIncomingState(const std::string& msg);
    void Tick();

private:
    CoSyncPlayer& GetOrCreate(const std::string& username);

    std::unordered_map<std::string, CoSyncPlayer> m_players;
};

// Global instance
extern CoSyncPlayerManager g_CoSyncPlayerManager;
