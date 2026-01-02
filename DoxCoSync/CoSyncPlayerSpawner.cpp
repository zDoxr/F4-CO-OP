#include "CoSyncPlayerSpawner.h"
#include "CoSyncPlayerManager.h"

// Compatibility wrapper. Do NOT spawn here.
// Spawn authority is CoSyncPlayerManager.
void CoSyncPlayerSpawner::HandleEntityCreate(const EntityCreatePacket& pkt)
{
    g_CoSyncPlayerManager.EnqueueEntityCreate(pkt);
}
