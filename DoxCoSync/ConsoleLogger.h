#pragma once




#include <windows.h>
#include <cstdio>
#include <cstdarg>

enum class LogColor
{
    WHITE = 7,
    GREEN = 10,
    YELLOW = 14,
    RED = 12,
    CYAN = 11
};

// ======================================================
// FORCE CONSOLE FOR FALLOUT 4 NG
// ======================================================
inline void EnsureConsole()
{
    static bool initialized = false;
    if (initialized) return;

    // Detach from F4's fake console
    FreeConsole();

    // Create an actual new console
    AllocConsole();

    // Attach input/output
    FILE* fDummy;
    freopen_s(&fDummy, "CONIN$", "r", stdin);
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONOUT$", "w", stderr);

    SetConsoleTitleA("Doxr's CoSync Debug Console");

    initialized = true;
}

// ======================================================
// Thread-safe colored printer
// ======================================================
inline void ConsoleLogColor(LogColor color, const char* fmt, ...)
{
    EnsureConsole();

    static CRITICAL_SECTION cs;
    static bool cs_init = false;

    if (!cs_init)
    {
        InitializeCriticalSection(&cs);
        cs_init = true;
    }

    EnterCriticalSection(&cs);

    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, (int)color);

    char buffer[4096];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    printf("%s\n", buffer);

    SetConsoleTextAttribute(hConsole, 7);

    LeaveCriticalSection(&cs);
}

// ======================================================
// Logging macros
// ======================================================
#define LOG_INFO(fmt, ...)   ConsoleLogColor(LogColor::GREEN,  "[INFO] " fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)   ConsoleLogColor(LogColor::YELLOW, "[WARN] " fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)  ConsoleLogColor(LogColor::RED,    "[ERROR] " fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)  ConsoleLogColor(LogColor::CYAN,   "[DEBUG] " fmt, ##__VA_ARGS__)
