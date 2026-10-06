#pragma once

#include <windows.h>
#include <cstdio>
#include <cstring>

#include "CoSyncLogCrypt.h"   // 0.5.7: encrypted like the main log

// Separate from CoSyncUpdated.log on purpose -- this is a static contract
// list ("what does this function need to be true to work correctly"),
// not a live trace. One line per function, written the first time that
// function is ever called, never again. No printf-style formatting at all
// (fwrite of fixed strings only) so this can never hit the CRT
// vsnprintf/format_validation_base_char_ crash class documented in
// ConsoleLogger.h -- function name and requirement text are always
// string literals supplied at the call site.
inline void LogFunctionRequirement(const char* functionName, const char* requirement)
{
    static CRITICAL_SECTION cs;
    static bool  cs_init = false;
    static CoSyncCryptLog* s_file = nullptr;
    static bool  s_tried = false;

    if (!cs_init)
    {
        InitializeCriticalSection(&cs);
        cs_init = true;
    }

    EnterCriticalSection(&cs);

    if (!s_tried)
    {
        s_tried = true;
        // Keep the previous session's log (see ConsoleLogger.h — same rationale).
        MoveFileExA("Data\\F4SE\\Plugins\\CoSyncRequirements.cslog",
                    "Data\\F4SE\\Plugins\\CoSyncRequirements.prev.cslog",
                    MOVEFILE_REPLACE_EXISTING);
        s_file = CoSyncCryptLog_Open("Data\\F4SE\\Plugins\\CoSyncRequirements.cslog");
    }

    if (s_file)
    {
        CoSyncCryptLog_Write(s_file, functionName, strlen(functionName));
        CoSyncCryptLog_Write(s_file, ": ", 2);
        CoSyncCryptLog_Write(s_file, requirement, strlen(requirement));
        CoSyncCryptLog_Write(s_file, "\n", 1);
    }

    LeaveCriticalSection(&cs);
}

// Insert as the first statement in a function body. Fires exactly once
// per function (function-local static), regardless of how many times the
// function is actually called.
#define LOG_REQUIREMENT(name, text) \
    do { \
        static bool s_reqLogged = false; \
        if (!s_reqLogged) { s_reqLogged = true; LogFunctionRequirement(name, text); } \
    } while (0)
