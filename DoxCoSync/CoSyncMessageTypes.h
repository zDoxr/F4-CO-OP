#pragma once
#include <cstdint>

enum class CoSyncMessageType : uint8_t
{
    Hello = 1,
    EntityCreate = 2,
    EntityUpdate = 3,
    EntityRemove = 4,
};
