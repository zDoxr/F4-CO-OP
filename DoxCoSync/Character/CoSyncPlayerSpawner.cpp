#include "CoSyncPlayerSpawner.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncRequirementsLog.h"

void CoSyncPlayerSpawner::HandleEntityCreate(const EntityCreatePacket& pkt)
{
    LOG_REQUIREMENT("CoSyncPlayerSpawner::HandleEntityCreate", "Needs the player manager to already exist before this is called.");
    g_CoSyncPlayerManager.EnqueueEntityCreate(pkt);
}
