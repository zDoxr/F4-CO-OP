#pragma once

#include <windows.h>
#include <cstdio>
#include <cstring>

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
    static FILE* s_file  = nullptr;
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
        MoveFileExA("Data\\F4SE\\Plugins\\CoSyncRequirements.log",
                    "Data\\F4SE\\Plugins\\CoSyncRequirements.prev.log",
                    MOVEFILE_REPLACE_EXISTING);
        fopen_s(&s_file, "Data\\F4SE\\Plugins\\CoSyncRequirements.log", "w");
    }

    if (s_file)
    {
        fwrite(functionName, 1, strlen(functionName), s_file);
        fwrite(": ", 1, 2, s_file);
        fwrite(requirement, 1, strlen(requirement), s_file);
        fwrite("\n", 1, 1, s_file);
        fflush(s_file);
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
