#pragma once
// Stand-ins for the game/network modules CoSyncContentSync.cpp talks to, so
// the real host and client state machines can run against each other in one
// process (SyncTests.cpp implements these).
#include <cstdint>
#include <cstdio>
#include <string>

typedef uint32_t UInt32;
typedef uint32_t HSteamNetConnection;
constexpr HSteamNetConnection k_HSteamNetConnection_Invalid = 0;

extern bool g_testVerboseLog;
#define COSYNC_TEST_LOG(level, fmt, ...) do { if (g_testVerboseLog) std::printf("      " level " " fmt "\n", ##__VA_ARGS__); } while (0)
