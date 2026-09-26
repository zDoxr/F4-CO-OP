#pragma once
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <string>


inline std::string GetLogPath()
{
    char docPath[MAX_PATH]{};

    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docPath)))
    {
        std::string folder = std::string(docPath) + "\\My Games\\Fallout4\\F4SE";
        CreateDirectoryA((std::string(docPath) + "\\My Games\\Fallout4").c_str(), NULL);
        CreateDirectoryA(folder.c_str(), NULL);

        return folder + "\\CoSync.log";
    }


    return "CoSync.log";
}


inline void CoSync_LogInternal(const char* fmt, ...)
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


    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");


    std::string path = GetLogPath();
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "a") == 0 && f)
    {
        fprintf(f, "%s\n", buffer);
        fclose(f);
    }

    LeaveCriticalSection(&cs);
}


#define COSYNC_LOG(fmt, ...) \
    CoSync_LogInternal("[CoSync] " fmt, ##__VA_ARGS__)
