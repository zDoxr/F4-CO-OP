#pragma once

#include <cstdint>
#include <string>

#include "NiTypes.h"

// SteamNetworkingSockets type (forward-declare as uint32_t compatible handle)
using HSteamNetConnection = uint32_t;

class CoSyncNet
{
public:
    // Lifecycle
    static void Init(bool isHost);
    static void ScheduleInit(bool isHost);
    static void PerformPendingInit();
    static void Shutdown();

    // Tick
    static void Tick(double now);

    // Identity
    static uint64_t GetMySteamID();
    static uint32_t GetMyEntityID();
    static const char* GetMyName();
    static void SetMyName(const std::string& name);
    static void SetMySteamID(uint64_t sid);

    // State
    static bool IsHost();
    static bool IsInitialized();
    static bool IsConnected();

    // Network send
    static void SendMyEntityUpdate(
        uint32_t entityID,
        const NiPoint3& pos,
        const NiPoint3& rot,
        const NiPoint3& vel,
        double now);

    // Host-only spawn
    static void HostSpawnNpc(
        uint32_t entityID,
        uint32_t baseFormID,
        const NiPoint3& pos,
        const NiPoint3& rot);

    // Transport callbacks
    static void OnReceive(const std::string& msg, double now, HSteamNetConnection conn);
    static void OnGNSConnected();
    static void OnGNSDisconnected();
};
