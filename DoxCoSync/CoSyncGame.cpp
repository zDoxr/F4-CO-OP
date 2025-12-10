#include "ConsoleLogger.h"

extern void CoSync_GameFrameTick()
{
    // TickHook calls all per-frame logic.
    // Transport needs this symbol to link, but we don't use it yet.
    LOG_DEBUG("[CoSync] GameFrameTick()");
}
