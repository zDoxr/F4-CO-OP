// CoSyncSteamManager.cpp
// SAFE STUB VERSION: does NOT call any Steam APIs.
// Keeps the structure for future use but avoids crashes entirely.

#include "CoSyncSteamManager.h"
#include "ConsoleLogger.h"

#include <Windows.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>

// ============================================================
// Internal State
// ============================================================

namespace
{
    std::atomic<bool> g_started{ false };
    std::atomic<bool> g_shutdownRequested{ false };

    std::thread g_workerThread;

    std::mutex g_dataMutex;

    bool        g_identityReady = false;
    std::string g_personaName;
    uint64_t    g_selfSteamID = 0;

    std::vector<CoSyncFriendInfo> g_friends;
}

// ============================================================
// Worker thread (no Steam calls)
// ============================================================

static void Worker_ThreadMain()
{
    LOG_INFO("[CoSyncSteamMgr] Worker thread started (SAFE STUB, no Steam calls)");

    // Just wait a bit so we know we're alive, then exit.
    constexpr DWORD kInitialDelayMs = 3000;
    Sleep(kInitialDelayMs);

    LOG_INFO("[CoSyncSteamMgr] Worker thread exiting (no identity fetched)");
}

// ============================================================
// Public API
// ============================================================

namespace CoSyncSteamManager
{
    void OnGameLoaded()
    {
        bool expected = false;
        if (!g_started.compare_exchange_strong(expected, true))
        {
            LOG_INFO("[CoSyncSteamMgr] OnGameLoaded called but manager already started");
            return;
        }

        g_shutdownRequested.store(false);

        try
        {
            g_workerThread = std::thread(&Worker_ThreadMain);
        }
        catch (...)
        {
            LOG_ERROR("[CoSyncSteamMgr] FAILED to create worker thread");
            g_started.store(false);
            return;
        }

        LOG_INFO("[CoSyncSteamMgr] Manager started (SAFE STUB)");
    }

    void Shutdown()
    {
        if (!g_started.load())
            return;

        LOG_INFO("[CoSyncSteamMgr] Shutdown requested");

        g_shutdownRequested.store(true);

        if (g_workerThread.joinable())
        {
            g_workerThread.join();
        }

        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            g_identityReady = false;
            g_personaName.clear();
            g_selfSteamID = 0;
            g_friends.clear();
        }

        g_started.store(false);
        LOG_INFO("[CoSyncSteamMgr] Shutdown complete");
    }

    bool HasIdentity()
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        return g_identityReady; // always false in stub
    }

    bool GetIdentity(std::string& outPersona, uint64_t& outSteamID)
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);

        if (!g_identityReady)
            return false;

        outPersona = g_personaName;
        outSteamID = g_selfSteamID;
        return true;
    }

    std::vector<CoSyncFriendInfo> GetFriendsSnapshot()
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        return g_friends; // will be empty in stub
    }
}
