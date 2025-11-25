#pragma once
#include <windows.h>
#include <shlobj.h>        // <-- REQUIRED for SHGetFolderPathA, CSIDL_*, SHGFP_*
#include <cstdio>
#include <cstdarg>
#include <string>

// ------------------------------------------------------
// Get log file path:  Documents\My Games\Fallout4\F4MP.log
// ------------------------------------------------------
inline std::string GetLogPath()
{
    char docPath[MAX_PATH]{};

    // This is the missing function + constants
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docPath)))
    {
        std::string path(docPath);
        path += "\\My Games\\Fallout4\\F4MP.log";
        return path;
    }

    // fallback: game directory
    return "F4MP.log";
}

// ------------------------------------------------------
// Thread-safe file logger
// ------------------------------------------------------
inline void SafeLog(const char* fmt, ...)
{
    static CRITICAL_SECTION cs;
    static bool initialized = false;

    if (!initialized)
    {
        InitializeCriticalSection(&cs);
        initialized = true;
    }

    EnterCriticalSection(&cs);

    char buffer[2048];

    va_list args;
    va_start(args, fmt);
    vsprintf_s(buffer, sizeof(buffer), fmt, args);   // <-- defined now
    va_end(args);

    FILE* f = nullptr;
    std::string logPath = GetLogPath();

    fopen_s(&f, logPath.c_str(), "a");
    if (f)
    {
        fprintf(f, "%s\n", buffer);
        fclose(f);
    }

    LeaveCriticalSection(&cs);
}

// ------------------------------------------------------
// Macro for convenience:
// F4MP_LOG("Value = %d", someVar);
// ------------------------------------------------------
#define F4MP_LOG(fmt, ...) SafeLog("[F4MP] " fmt, __VA_ARGS__)
