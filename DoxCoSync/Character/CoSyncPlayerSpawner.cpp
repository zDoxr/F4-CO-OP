#include "CoSyncPlayerSpawner.h"
#include "CoSyncPlayerManager.h"

void CoSyncPlayerSpawner::HandleEntityCreate(const EntityCreatePacket& pkt)
{
    g_CoSyncPlayerManager.EnqueueEntityCreate(pkt);
}
