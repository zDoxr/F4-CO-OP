#pragma once
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <string>


#include "ConsoleLogger.h"

// 0.5.7: the old plain-text CoSync.log (Documents\My Games) and its
// OutputDebugString copy are gone; anything logged here goes into the
// encrypted main log.
inline void CoSync_LogInternal(const char* fmt, ...)
{
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    const bool ok = SafeVsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (ok)
        CoSyncUpdatedLog_Printf(LogColor::WHITE, "%s", buffer);
}


#define COSYNC_LOG(fmt, ...) \
    CoSync_LogInternal("[CoSync] " fmt, ##__VA_ARGS__)
