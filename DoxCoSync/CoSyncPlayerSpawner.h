#pragma once

#include "Packets_EntityCreate.h"

namespace CoSyncPlayerSpawner
{
    // Compatibility wrapper: manager is authoritative.
    void HandleEntityCreate(const EntityCreatePacket& pkt);
}
