#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

#include "PlayerStatePacket.h"
#include "CoSyncPlayer.h"

class CoSyncPlayerManager
{
public:
    CoSyncPlayer& GetOrCreate(const std::string& username);

    // Network-facing (thread-safe)
    void EnqueueIncoming(const std::string& msg);

    // GAME THREAD ONLY
    void ProcessInbox();

    void ProcessIncomingState(const std::string& msg);
    void Tick();

private:
    std::unordered_map<std::string, CoSyncPlayer> m_players;

    // Incoming network queue (cross-thread safe)
    std::vector<std::string> m_inbox;
    std::mutex               m_inboxMutex;
};

// Global instance
extern CoSyncPlayerManager g_CoSyncPlayerManager;
