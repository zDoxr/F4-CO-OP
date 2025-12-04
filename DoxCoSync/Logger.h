#pragma once
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <string>

// ---------- Create full log path ----------
inline std::string GetLogPath()
{
    char docPath[MAX_PATH]{};

    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docPath)))
    {
        std::string folder = std::string(docPath) + "\\My Games\\Fallout4\\F4SE";
        CreateDirectoryA((std::string(docPath) + "\\My Games\\Fallout4").c_str(), NULL);
        CreateDirectoryA(folder.c_str(), NULL);

        return folder + "\\F4MP.log";
    }

    // Fallback: write next to Fallout4.exe
    return "F4MP.log";
}

// ---------- Thread-safe internal logger ----------
inline void F4MP_LogInternal(const char* fmt, ...)
{
    static CRITICAL_SECTION cs;
    static bool cs_init = false;

    if (!cs_init) {
        InitializeCriticalSection(&cs);
        cs_init = true;
    }

    EnterCriticalSection(&cs);

    char buffer[4096];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    // Output to debugger
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");

    // Output to log file
    std::string path = GetLogPath();
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "a") == 0 && f)
    {
        fprintf(f, "%s\n", buffer);
        fclose(f);
    }

    LeaveCriticalSection(&cs);
}

// ---------- Macro wrapper ----------
#define F4MP_LOG(fmt, ...) \
    F4MP_LogInternal("[F4MP] " fmt, ##__VA_ARGS__)
