#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdarg>

enum class LogColor
{
    WHITE  = 7,
    GREEN  = 10,
    YELLOW = 14,
    RED    = 12,
    CYAN   = 11,
    PURPLE = 13,   // bright magenta — reserved for animation testing logs
};

inline void EnsureConsole()
{
    static bool initialized = false;
    if (initialized) return;


    FreeConsole();


    AllocConsole();


    FILE* fDummy;
    freopen_s(&fDummy, "CONIN$", "r", stdin);
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONOUT$", "w", stderr);

    SetConsoleTitleA("Doxr's CoSync Debug Console");

    initialized = true;
}

// UPDATE 2026-06-17: SEH-guarded formatting helpers, hoisted above
// ConsoleLogColor so both it and CoSyncUpdatedLog_Printf can share them.
// Can't share a frame with va_list/other unwind-needing objects -- same
// constraint noted elsewhere in this codebase, e.g. CoSyncVatsSync's
// SafeReadVatsCameraState. A real, reproducible crash (3 occurrences,
// confirmed via a proper RtlVirtualUnwind-based stack walk, not guessing
// from raw stack bytes) lands inside the CRT's own vsnprintf/_snprintf_s
// internals (format_validation_base_char_, an unaligned SSE store) when
// called from this exact logging path -- root cause not fully nailed down
// (moving msgbuf/buffer to thread_local, the previous fix attempt, did not
// resolve it -- ruled out stack-overflow-on-a-small-thread). The crash
// shape (AV type=READ, fault=0xFFFFFFFFFFFFFFFF -- a sentinel/uninitialized
// value, not a real faulting address) is consistent with a CPU #GP fault
// from a misaligned SSE instruction being reported as a generic AV by
// Windows, not a corrupted pointer. Whatever the exact mechanism, every
// single LOG_* call in the entire codebase funnels through
// CoSyncUpdatedLog_Printf AND ConsoleLogColor, so both formatting calls
// need the same crash containment -- this needs to degrade (skip that one
// log line) rather than crash the whole game, same as every other "can't
// fully prove safe, guard defensively" spot in this codebase.
// UPDATE 2026-06-17: __declspec(noinline) is load-bearing here, not a perf
// hint. Verified via raw exception-data inspection (parsed the compiled
// DLL's .pdata/.xdata for CoSyncUpdatedLog_Printf) that under /GL+/LTCG the
// optimizer was fully inlining these two functions into their caller AND
// dropping the __try/__except's exception-dispatch data in the process --
// the caller's resolved ExceptionHandler was plain __GSHandlerCheck (pure
// /GS stack-cookie validator, no __C_specific_handler scope table chained
// at all), not an EH-carrying handler. That's why the 4th live crash still
// went uncaught even though this code looked correctly guarded -- the
// guard was silently optimized away. Forcing these to stay real,
// non-inlined functions (called via `call`, not pasted in) guarantees each
// gets its own UNWIND_INFO with a real scope table for its __try/__except.
__declspec(noinline) static bool SafeVsnprintf(char* dest, size_t destsz, const char* fmt, va_list args)
{
    __try
    {
        vsnprintf(dest, destsz, fmt, args);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Returns the per-process role tag — "[H]" for host, "[C]" for client,
// "[?]" before CoSyncNet::Init has run. Set once; never changes after that.
// Inlined accessor so ConsoleLogger.h remains header-only.
inline const char*& CoSyncLogRoleTag()
{
    static const char* s_tag = "[?]";
    return s_tag;
}

__declspec(noinline) static bool SafeTimestampWrap(char* dest, size_t destsz, const char* tsbuf, const char* msgbuf)
{
    __try
    {
        _snprintf_s(dest, destsz, _TRUNCATE, "[%s]%s %s", tsbuf, CoSyncLogRoleTag(), msgbuf);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// UPDATE 2026-06-17: crash #6 proved fallback/recovery paths must not call
// back into the CRT's printf-formatting machinery at all -- see the fuller
// explanation further down next to where this is first used for literal
// fallback strings. Hoisted above ConsoleLogColor so it can use this too.
static void RawFallbackCopy(char* dest, size_t destsz, const char* a, const char* b = nullptr, const char* c = nullptr)
{
    size_t pos = 0;
    auto append = [&](const char* s)
    {
        if (!s) return;
        size_t len = strlen(s);
        if (pos + len >= destsz)
            len = (destsz > pos + 1) ? (destsz - pos - 1) : 0;
        if (len > 0) { memcpy(dest + pos, s, len); pos += len; }
    };
    append(a);
    append(b);
    append(c);
    dest[pos] = '\0';
}

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

    // Moved off the stack -- see CoSyncUpdatedLog_Printf's comment, same reasoning.
    thread_local char buffer[4096];

    va_list args;
    va_start(args, fmt);
    const bool fmtOk = SafeVsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (!fmtOk)
    {
        RawFallbackCopy(buffer, sizeof(buffer), "<log formatting crashed -- recovered>");
    }

    // UPDATE 2026-06-17: crash #7 was a cascade of 5 successive faults, each
    // landing at a different offset, with the game surviving briefly
    // between each one. That meant the underlying corruption persists
    // across the whole thread once triggered, not just one call -- and the
    // unconditional `printf`/`fprintf` calls below (which run on every
    // single log line, success or recovery) were the ones still hitting
    // the same broken CRT printf-formatting machinery, completely
    // unguarded. They never needed format parsing at all -- `buffer`/`line`
    // are already fully-formed strings -- so route them through
    // `WriteConsoleA`/`fwrite` instead, a different CRT/Win32 code path
    // that bypasses `__stdio_common_vfprintf`/format validation entirely.
    WriteConsoleA(hConsole, buffer, (DWORD)strlen(buffer), nullptr, nullptr);
    WriteConsoleA(hConsole, "\n", 1, nullptr, nullptr);

    SetConsoleTextAttribute(hConsole, 7);

    LeaveCriticalSection(&cs);
}

inline void CoSyncUpdatedLog_Write(const char* line)
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
        // PID-based filename so two Goldberg instances sharing the same game
        // folder each get their own log file instead of clobbering each other.
        // Example: CoSyncUpdated_12345.log / CoSyncUpdated_12345.prev.log
        char logPath[MAX_PATH];
        char prevPath[MAX_PATH];
        const DWORD pid = GetCurrentProcessId();
        snprintf(logPath,  sizeof(logPath),  "Data\\F4SE\\Plugins\\CoSyncUpdated_%lu.log",      (unsigned long)pid);
        snprintf(prevPath, sizeof(prevPath), "Data\\F4SE\\Plugins\\CoSyncUpdated_%lu.prev.log", (unsigned long)pid);

        MoveFileExA(logPath, prevPath, MOVEFILE_REPLACE_EXISTING);
        fopen_s(&s_file, logPath, "w");
        if (s_file)
        {
            char openMsg[128];
            snprintf(openMsg, sizeof(openMsg),
                "=== CoSyncUpdated_%lu.log opened (role set after CoSyncNet::Init) ===\n",
                (unsigned long)pid);
            fwrite(openMsg, 1, strlen(openMsg), s_file);
        }
    }

    if (s_file)
    {
        fwrite(line, 1, strlen(line), s_file);
        fwrite("\n", 1, 1, s_file);
        fflush(s_file);
    }

    LeaveCriticalSection(&cs);
}

// Separate log file for animation testing — keeps anim event spam out of the
// main CoSyncUpdated.log so both logs stay readable. Only CoSyncPlayerAnimation.cpp
// writes here via LOG_ANIMTEST. Like CoSyncUpdatedLog_Write, it rotates the
// previous session to .prev.log on first open.
inline void CoSyncAnimTestLog_Write(const char* line)
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
        char logPath[MAX_PATH];
        char prevPath[MAX_PATH];
        const DWORD pid = GetCurrentProcessId();
        snprintf(logPath,  sizeof(logPath),  "Data\\F4SE\\Plugins\\CoSyncAnimTest_%lu.log",      (unsigned long)pid);
        snprintf(prevPath, sizeof(prevPath), "Data\\F4SE\\Plugins\\CoSyncAnimTest_%lu.prev.log", (unsigned long)pid);
        MoveFileExA(logPath, prevPath, MOVEFILE_REPLACE_EXISTING);
        fopen_s(&s_file, logPath, "w");
        if (s_file)
        {
            static const char kOpenMsg[] = "=== CoSyncAnimTest log opened ===\n";
            fwrite(kOpenMsg, 1, sizeof(kOpenMsg) - 1, s_file);
        }
    }

    if (s_file)
    {
        fwrite(line, 1, strlen(line), s_file);
        fwrite("\n", 1, 1, s_file);
        fflush(s_file);
    }

    LeaveCriticalSection(&cs);
}

// UPDATE 2026-06-17: this exact call is the REAL, confirmed crash site --
// found by disassembling the compiled CoSyncUpdatedLog_Printf and matching
// the crash's logged return address (DoxCoSync.dll+0xB735) to the
// instruction right after THIS _snprintf_s call (CoSyncTimestamp is a tiny
// `inline` function, so it gets pasted directly into
// CoSyncUpdatedLog_Printf -- this call site is NOT SafeVsnprintf or
// SafeTimestampWrap, those are further down in the function and were never
// actually involved in any of the 4 live crashes despite being the ones
// previously suspected and guarded). Same noinline+SEH treatment as those,
// for the same reason: a noinline function is required so its __try/__except
// actually survives in the compiled binary's exception-dispatch data
// instead of being silently eaten by inlining under /GL+/LTCG.
__declspec(noinline) static bool SafeTimestampFormat(char* buf, int bufsz, const SYSTEMTIME& st)
{
    __try
    {
        _snprintf_s(buf, bufsz, _TRUNCATE, "%02d:%02d:%02d.%03d",
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// UPDATE 2026-06-17: crash #6 caught the fault inside SafeTimestampFormat
// (proving the SEH guard mechanism itself works -- it returned false
// cleanly), but the recovery path then called straight back into
// `_snprintf_s` for the literal "??:??:??.???" fallback string and THAT
// ALSO faulted, unguarded, killing the process anyway. That a hardcoded
// literal with zero format specifiers faults inside the exact same
// `_snprintf_s -> __stdio_common_vsnprintf_s -> format_validation_base_char_`
// chain rules out bad arguments/format strings as the cause entirely --
// whatever is wrong, it's a property of this thread's CRT
// printf-formatting machinery itself (state that's durably broken once it
// first faults, not a one-off blip), not anything about what's being
// printed. Fallback/recovery paths must not call back into that same
// machinery at all -- copying a fixed literal string never needed
// printf-style format parsing in the first place. RawFallbackCopy (defined
// above, near ConsoleLogColor) uses only `strlen`/`memcpy`, a completely
// different (and far simpler) CRT code path than the one confirmed broken.
inline const char* CoSyncTimestamp(char* buf, int bufsz)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (!SafeTimestampFormat(buf, bufsz, st))
    {
        RawFallbackCopy(buf, (size_t)bufsz, "??:??:??.???");
    }
    return buf;
}

inline void CoSyncUpdatedLog_Printf(LogColor color, const char* fmt, ...)
{
    char tsbuf[20];
    CoSyncTimestamp(tsbuf, sizeof(tsbuf));

    thread_local char msgbuf[4096];
    va_list args;
    va_start(args, fmt);
    const bool fmtOk = SafeVsnprintf(msgbuf, sizeof(msgbuf), fmt, args);
    va_end(args);
    if (!fmtOk)
    {
        RawFallbackCopy(msgbuf, sizeof(msgbuf), "<log formatting crashed -- recovered>");
    }

    thread_local char buffer[4128];
    if (!SafeTimestampWrap(buffer, sizeof(buffer), tsbuf, msgbuf))
    {
        RawFallbackCopy(buffer, sizeof(buffer), "[", tsbuf, "] <log formatting crashed -- recovered>");
    }


    ConsoleLogColor(color, "%s", buffer);


    CoSyncUpdatedLog_Write(buffer);
}

// Purple console + CoSyncAnimTest.log only. Does NOT write to CoSyncUpdated.log.
// Use this for all animation testing output so the main log stays clean.
inline void CoSyncAnimTestLog_Printf(const char* fmt, ...)
{
    char tsbuf[20];
    CoSyncTimestamp(tsbuf, sizeof(tsbuf));

    thread_local char msgbuf[4096];
    va_list args;
    va_start(args, fmt);
    const bool fmtOk = SafeVsnprintf(msgbuf, sizeof(msgbuf), fmt, args);
    va_end(args);
    if (!fmtOk)
        RawFallbackCopy(msgbuf, sizeof(msgbuf), "<log formatting crashed -- recovered>");

    thread_local char buffer[4128];
    if (!SafeTimestampWrap(buffer, sizeof(buffer), tsbuf, msgbuf))
        RawFallbackCopy(buffer, sizeof(buffer), "[", tsbuf, "] <log formatting crashed -- recovered>");

    ConsoleLogColor(LogColor::PURPLE, "%s", buffer);
    CoSyncAnimTestLog_Write(buffer);
}

#define LOG_ANIMTEST(fmt, ...)  CoSyncAnimTestLog_Printf("[ANIM]  " fmt, ##__VA_ARGS__)

#define LOG_INFO(fmt, ...)   CoSyncUpdatedLog_Printf(LogColor::GREEN,  "[INFO]  " fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)   CoSyncUpdatedLog_Printf(LogColor::YELLOW, "[WARN]  " fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)  CoSyncUpdatedLog_Printf(LogColor::RED,    "[ERROR] " fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)  CoSyncUpdatedLog_Printf(LogColor::CYAN,   "[DEBUG] " fmt, ##__VA_ARGS__)

#define LOG_UPDATED(fmt, ...)      CoSyncUpdatedLog_Printf(LogColor::CYAN,   "[UPDATED] " fmt, ##__VA_ARGS__)
#define LOG_UPDATED_WARN(fmt, ...) CoSyncUpdatedLog_Printf(LogColor::YELLOW, "[WARN]    " fmt, ##__VA_ARGS__)
#define LOG_UPDATED_ERR(fmt, ...)  CoSyncUpdatedLog_Printf(LogColor::RED,    "[ERROR]   " fmt, ##__VA_ARGS__)

#ifdef COSYNC_TRACE
#  define LOG_TRACE(fmt, ...)  CoSyncUpdatedLog_Printf(LogColor::WHITE, "[TRACE]   " fmt, ##__VA_ARGS__)
#else
#  define LOG_TRACE(fmt, ...)  ((void)0)
#endif

#define LOG_SECTION(name)  CoSyncUpdatedLog_Printf(LogColor::WHITE, \
    "[UPDATED] ======== " name " ========")


#define LOG_SUBSYSTEM_INIT(name) \
    CoSyncUpdatedLog_Printf(LogColor::GREEN, "[INIT]    " name " subsystem initialised")
