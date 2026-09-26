#pragma once

#include <cstdint>

struct EntityDestroyPacket
{

    uint32_t entityID = 0;


    uint32_t reasonFlags = 0;
};
