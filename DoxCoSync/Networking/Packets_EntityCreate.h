#pragma once

#include <cstdint>
#include <string>

#include "NiTypes.h"
#include "CoSyncEntityTypes.h"

struct EntityCreatePacket
{

    uint32_t entityID = 0;

    CoSyncEntityType type = CoSyncEntityType::Player;


    uint32_t baseFormID = 0;

    uint32_t ownerEntityID = 0;

    enum SpawnFlags : uint32_t
    {
        None = 0,
        RemoteControlled = 1 << 0,
        HiddenOnSpawn = 1 << 1,
        Persistent = 1 << 2,
    };

    uint32_t spawnFlags = HiddenOnSpawn | RemoteControlled;


    static bool HasFlag(uint32_t flags, SpawnFlags f)
    {
        return (flags & static_cast<uint32_t>(f)) != 0;
    }

    NiPoint3 spawnPos{ 0.f, 0.f, 0.f };


    NiPoint3 spawnRot{ 0.f, 0.f, 0.f };

    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;

    std::string username;
};
