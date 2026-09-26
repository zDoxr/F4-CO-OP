#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"

extern void CoSync_GameFrameTick()
{
    LOG_REQUIREMENT("CoSync_GameFrameTick", "No safety check needed here. This just runs once every game frame.");


    LOG_DEBUG("[CoSync] GameFrameTick()");
}
