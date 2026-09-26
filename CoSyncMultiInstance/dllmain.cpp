#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ShlObj.h>
#include <TlHelp32.h>
#include <cstdio>
#include <cwchar>
#pragma comment(lib, "Shell32.lib")

static HANDLE s_log = INVALID_HANDLE_VALUE;

static void Log(const char* fmt, ...)
{
    if (s_log == INVALID_HANDLE_VALUE) return;
    char buf[512];
    va_list va; va_start(va, fmt); int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, va); va_end(va);
    if (n < 0) n = (int)sizeof(buf) - 1;
    buf[n++] = '\n';
    DWORD written;
    WriteFile(s_log, buf, (DWORD)n, &written, NULL);
    FlushFileBuffers(s_log);
}

// ── VEH — catches informational & fatal exceptions before WER swallows them ───
static LONG WINAPI VEH_Handler(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Skip pure informational/debug exceptions to keep the log readable
    if (code == 0x40010006 || // OutputDebugString
        code == 0x406D1388)   // SetThreadName
        return EXCEPTION_CONTINUE_SEARCH;

    void* addr = ep->ExceptionRecord->ExceptionAddress;
    char modName[MAX_PATH] = "<unknown>";
    HMODULE hm = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &hm) && hm)
    {
        if (GetModuleFileNameA(hm, modName, sizeof(modName)))
        {
            char* slash = strrchr(modName, '\\');
            if (slash) memmove(modName, slash + 1, strlen(slash));
        }
    }
    Log("!!! VEH FATAL EXCEPTION 0x%08X at %p in %s", code, addr, modName);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ── ExitProcess hook ──────────────────────────────────────────────────────────
static void(WINAPI* s_RealExitProcess)(UINT) = nullptr;
static VOID WINAPI HookExitProcess(UINT code)
{
    Log("ExitProcess called with code=%u", code);
    if (s_log != INVALID_HANDLE_VALUE) { FlushFileBuffers(s_log); CloseHandle(s_log); s_log = INVALID_HANDLE_VALUE; }
    s_RealExitProcess(code);
}

// ── FindWindow hooks — suppress the "game already running" window check ───────
static HWND(WINAPI* s_RealFindWindowA)(LPCSTR, LPCSTR) = nullptr;
static HWND(WINAPI* s_RealFindWindowW)(LPCWSTR, LPCWSTR) = nullptr;

static HWND WINAPI HookFindWindowA(LPCSTR cls, LPCSTR title)
{
    HWND real = s_RealFindWindowA(cls, title);
    Log("FindWindowA(cls=\"%s\", title=\"%s\") -> %p (returning NULL to bypass single-instance check)",
        cls ? cls : "<null>", title ? title : "<null>", real);
    return NULL; // pretend no game window exists
}

static HWND WINAPI HookFindWindowW(LPCWSTR cls, LPCWSTR title)
{
    HWND real = s_RealFindWindowW(cls, title);
    Log("FindWindowW(cls=\"%S\", title=\"%S\") -> %p (returning NULL to bypass single-instance check)",
        cls ? cls : L"<null>", title ? title : L"<null>", real);
    return NULL;
}

// ── SteamAPI_RestartAppIfNecessary — never call real, log before ──────────────
static bool __cdecl HookRestartApp(unsigned int appId)
{
    Log("SteamAPI_RestartAppIfNecessary(appId=%u) -> forcing false", appId);
    return false;
}

// ── Separate Saves folder for "my" (the second-launched) instance ────────────
// Both game instances share the same physical Documents\My Games\Fallout4\Saves
// folder (Goldberg doesn't separate them, and this proxy previously had no
// per-instance path redirection) -- confirmed 2026-07-21 by three separate
// crashes inside vanilla BGSSaveLoadGame::SaveGame() (Fallout4.exe+0xBEDD20),
// each leaving an orphaned Autosave1_..._DiamondCity...fos.tmp at the exact
// crash timestamp, always the SAME slot number across independent sessions --
// consistent with both instances racing to write the identical filename.
// A first attempt renamed the written file's extension to .co, but FO4's own
// Load menu hardcodes scanning for .fos/.f4se, so those saves never showed up
// there -- reverted per user request. This is the same idea done properly:
// redirect the SAVES FOLDER itself (not the extension) to a distinct,
// parallel "Saves_P2" directory for "my" instance only. Files inside it are
// still real, normal .fos/.f4se saves -- since the enumeration (FindFirstFileW,
// used to build the Load list) and every file open (CreateFileW, used to read/
// write/rename a save) both get redirected consistently, the game sees a
// completely normal, fully menu-visible Saves folder; it just happens to live
// somewhere the other instance never touches. Only paths containing "\Saves\"
// are touched -- Fallout4Custom.ini/Prefs.ini, mods, and everything else stay
// shared exactly as before. "My" instance = whichever one hit the mutex-
// collision bypass below (i.e. launched second) -- reuses that existing
// signal instead of adding new detection.
static bool s_isMyInstance = false; // becomes true once this process hits ERROR_ALREADY_EXISTS on a mutex

// In-place-safe: builds the redirected path into `out` (size outCap). Returns
// true if `path` contained a "Saves" path COMPONENT (bounded by '\' or '/' on
// both sides, either slash direction, e.g. "\Saves\", "/Saves/", "\Saves/")
// and was rewritten. Broadened 2026-07-21 after confirming case-insensitive
// backslash-only matching never fired even once against a real save.
static bool IsPathSep(wchar_t c) { return c == L'\\' || c == L'/'; }

static bool RedirectSavesPath(const wchar_t* path, wchar_t* out, size_t outCap)
{
    if (!s_isMyInstance || !path) return false;

    const wchar_t word[] = L"Saves";
    size_t wordLen = wcslen(word); // 5
    size_t pathLen = wcslen(path);
    if (pathLen >= outCap) return false;
    if (pathLen < wordLen + 2) return false;

    for (size_t i = 1; i + wordLen + 1 <= pathLen; ++i)
    {
        if (!IsPathSep(path[i - 1])) continue;
        if (_wcsnicmp(path + i, word, wordLen) != 0) continue;
        if (!IsPathSep(path[i + wordLen])) continue;

        // path[0..i) + "<sameSep>Saves_P2<sameSep>" + path[i+wordLen+1..end)
        wchar_t sep = path[i - 1];
        wchar_t replacement[16];
        _snwprintf_s(replacement, 16, _TRUNCATE, L"%cSaves_P2%c", sep, sep);
        size_t prefixLen = i - 1;
        size_t replLen = wcslen(replacement);
        size_t suffixLen = pathLen - (i + wordLen + 1);
        if (prefixLen + replLen + suffixLen >= outCap) return false;

        wmemcpy(out, path, prefixLen);
        wmemcpy(out + prefixLen, replacement, replLen);
        wmemcpy(out + prefixLen + replLen, path + i + wordLen + 1, suffixLen);
        out[prefixLen + replLen + suffixLen] = L'\0';
        return true;
    }
    return false;
}

static HANDLE(WINAPI* s_RealCreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) = nullptr;
static HANDLE(WINAPI* s_RealFindFirstFileW)(LPCWSTR, LPWIN32_FIND_DATAW) = nullptr;
static HANDLE(WINAPI* s_RealCreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) = nullptr;

// 2026-07-21: [0.0.48]-[0.0.50] confirmed instance detection AND the IAT
// patches both work (22 slots patched across every loaded module), yet a real
// save produced ZERO "redirected" log lines. Since the redirect itself never
// even LOOKED wrong (never logged, meaning RedirectSavesPath never matched at
// all, not that it matched and failed) -- two live possibilities: (a) the
// actual path string uses forward slashes or a \\?\ long-path prefix in a way
// the plain "\Saves\" substring check misses, or (b) the call happens through
// CreateFileA (ANSI) or a module loaded after this DLL's one-time DllMain
// snapshot. Broadened the match to accept either slash direction, and this
// diagnostic logs EVERY call whose path plausibly touches a save (by
// extension or the word "saves" anywhere, independent of RedirectSavesPath's
// own stricter check) so the next real save shows the exact literal string
// the engine actually uses, instead of guessing a third time.
static bool LooksSaveRelated(const wchar_t* path)
{
    if (!path) return false;
    for (const wchar_t* p = path; *p; ++p)
    {
        if ((p[0] == L's' || p[0] == L'S') && _wcsnicmp(p, L"saves", 5) == 0)
            return true;
    }
    size_t len = wcslen(path);
    if (len >= 4 && _wcsicmp(path + len - 4, L".fos") == 0) return true;
    if (len >= 5 && _wcsicmp(path + len - 5, L".f4se") == 0) return true;
    if (len >= 8 && _wcsicmp(path + len - 8, L".fos.tmp") == 0) return true;
    return false;
}

static HANDLE WINAPI HookCreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
    DWORD disposition, DWORD flags, HANDLE templateFile)
{
    if (LooksSaveRelated(path))
        Log("CreateFileW (save-related, diag): \"%S\" redirect-eligible=%d", path, s_isMyInstance ? 1 : 0);

    wchar_t redirected[MAX_PATH];
    if (RedirectSavesPath(path, redirected, MAX_PATH))
    {
        Log("CreateFileW redirected: \"%S\" -> \"%S\"", path, redirected);
        return s_RealCreateFileW(redirected, access, share, sa, disposition, flags, templateFile);
    }
    return s_RealCreateFileW(path, access, share, sa, disposition, flags, templateFile);
}

static HANDLE WINAPI HookCreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
    DWORD disposition, DWORD flags, HANDLE templateFile)
{
    if (path && (strstr(path, "aves") || strstr(path, "AVES") || strstr(path, ".fos")))
        Log("CreateFileA (save-related, diag): \"%s\"", path);
    return s_RealCreateFileA(path, access, share, sa, disposition, flags, templateFile);
}

static HANDLE WINAPI HookFindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW findData)
{
    if (LooksSaveRelated(path))
        Log("FindFirstFileW (save-related, diag): \"%S\" redirect-eligible=%d", path, s_isMyInstance ? 1 : 0);

    wchar_t redirected[MAX_PATH];
    if (RedirectSavesPath(path, redirected, MAX_PATH))
    {
        Log("FindFirstFileW redirected: \"%S\" -> \"%S\"", path, redirected);
        return s_RealFindFirstFileW(redirected, findData);
    }
    return s_RealFindFirstFileW(path, findData);
}

// Ensures the redirected Saves_P2 folder exists before first use -- CreateFileW
// won't create missing parent directories on its own. Best-effort: derives the
// path from the real Saves folder via SHGetFolderPathW rather than hardcoding
// a drive/username, so it works on any machine.
static void EnsureSavesP2FolderExists()
{
    wchar_t documents[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, documents)))
        return;

    wchar_t target[MAX_PATH];
    _snwprintf_s(target, MAX_PATH, _TRUNCATE, L"%s\\My Games\\Fallout4\\Saves_P2", documents);

    if (CreateDirectoryW(target, NULL) || GetLastError() == ERROR_ALREADY_EXISTS)
        Log("Saves_P2 folder ready: \"%S\"", target);
    else
        Log("Saves_P2 folder CreateDirectoryW failed for \"%S\", err=%lu", target, GetLastError());
}

// 2026-07-21: confirmed via every cosync_proxy_*.log today (dozens of
// sessions) that CreateMutexA/W NEVER once fires from the game itself --
// Fallout4.exe's single-instance check is FindWindow-only, no named mutex.
// The original design detected "my instance" by piggybacking on a collision
// of the GAME's own mutex, which never happens, so s_isMyInstance never
// flipped and the whole Saves_P2 redirect stayed permanently inert. Fixed by
// having the proxy create its OWN dedicated, private mutex directly (not
// through the hooked/patched EXE IAT -- this is a plain direct call from our
// own code, unaffected by the IAT patches below) and checking for the
// collision itself. Whichever process's DllMain runs second reliably sees
// ERROR_ALREADY_EXISTS. The handle is deliberately never closed so it stays
// held for the process's entire lifetime.
static void DetectMyInstance()
{
    HANDLE h = CreateMutexW(NULL, FALSE, L"CoSyncMultiInstance_FirstInstance");
    DWORD err = GetLastError();
    Log("DetectMyInstance: dedicated mutex CreateMutexW err=%lu", err);
    if (err == ERROR_ALREADY_EXISTS)
    {
        s_isMyInstance = true;
        Log("  -> this is \"my\" instance (launched second) -- Saves folder redirected to Saves_P2");
        EnsureSavesP2FolderExists();
    }
    else
    {
        Log("  -> this is the FIRST instance -- Saves folder unchanged");
    }
}

// ── Mutex bypass ──────────────────────────────────────────────────────────────
static HANDLE(WINAPI* s_RealCreateMutexA)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR)  = nullptr;
static HANDLE(WINAPI* s_RealCreateMutexW)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR) = nullptr;
static HANDLE(WINAPI* s_RealOpenMutexA)(DWORD, BOOL, LPCSTR)                    = nullptr;
static HANDLE(WINAPI* s_RealOpenMutexW)(DWORD, BOOL, LPCWSTR)                   = nullptr;

static char    s_bypassedA[256] = {};
static wchar_t s_bypassedW[256] = {};

static HANDLE WINAPI HookCreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCSTR name)
{
    HANDLE h = s_RealCreateMutexA(sa, owner, name);
    DWORD err = GetLastError();
    if (name) Log("CreateMutexA(\"%s\") err=%lu", name, err); // only log named ones now
    if (err == ERROR_ALREADY_EXISTS && h != NULL && name)
    {
        Log("  -> ERROR_ALREADY_EXISTS, bypassing");
        _snprintf_s(s_bypassedA, sizeof(s_bypassedA), _TRUNCATE, "%s", name);
        char unique[512];
        _snprintf_s(unique, sizeof(unique), _TRUNCATE, "CoSync_P2_%s_%lu", name, GetCurrentProcessId());
        CloseHandle(h);
        h = s_RealCreateMutexA(sa, owner, unique);
        SetLastError(0);
        if (!s_isMyInstance)
        {
            s_isMyInstance = true;
            Log("  -> this process is \"my\" instance (launched second) -- Saves folder redirected to Saves_P2");
            EnsureSavesP2FolderExists();
        }
    }
    return h;
}

static HANDLE WINAPI HookCreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCWSTR name)
{
    HANDLE h = s_RealCreateMutexW(sa, owner, name);
    DWORD err = GetLastError();
    if (name) Log("CreateMutexW(\"%S\") err=%lu", name, err);
    if (err == ERROR_ALREADY_EXISTS && h != NULL && name)
    {
        Log("  -> ERROR_ALREADY_EXISTS, bypassing");
        _snwprintf_s(s_bypassedW, 256, _TRUNCATE, L"%s", name);
        wchar_t unique[512];
        _snwprintf_s(unique, 512, _TRUNCATE, L"CoSync_P2_%s_%lu", name, GetCurrentProcessId());
        CloseHandle(h);
        h = s_RealCreateMutexW(sa, owner, unique);
        SetLastError(0);
        if (!s_isMyInstance)
        {
            s_isMyInstance = true;
            Log("  -> this process is \"my\" instance (launched second) -- Saves folder redirected to Saves_P2");
            EnsureSavesP2FolderExists();
        }
    }
    return h;
}

static HANDLE WINAPI HookOpenMutexA(DWORD access, BOOL inherit, LPCSTR name)
{
    Log("OpenMutexA(\"%s\")", name ? name : "<null>");
    if (name && s_bypassedA[0] && _stricmp(name, s_bypassedA) == 0)
    {
        Log("  -> suppressing");
        SetLastError(ERROR_FILE_NOT_FOUND);
        return NULL;
    }
    return s_RealOpenMutexA(access, inherit, name);
}

static HANDLE WINAPI HookOpenMutexW(DWORD access, BOOL inherit, LPCWSTR name)
{
    Log("OpenMutexW(\"%S\")", name ? name : L"<null>");
    if (name && s_bypassedW[0] && _wcsicmp(name, s_bypassedW) == 0)
    {
        Log("  -> suppressing");
        SetLastError(ERROR_FILE_NOT_FOUND);
        return NULL;
    }
    return s_RealOpenMutexW(access, inherit, name);
}

// ── IAT patcher ──────────────────────────────────────────────────────────────
static int PatchIAT(HMODULE hMod, const char* dll, const char* fn, void* hook)
{
    auto base = reinterpret_cast<BYTE*>(hMod);
    auto dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt   = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);

    int patched = 0;
    for (; desc->Name; ++desc)
    {
        const char* dllName = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(dllName, dll) != 0) continue;
        if (!desc->OriginalFirstThunk) continue;

        auto thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
        auto orig  = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk);

        for (DWORD i = 0; thunk[i].u1.Function; ++i)
        {
            if (orig[i].u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig[i].u1.AddressOfData);
            if (_stricmp(ibn->Name, fn) != 0) continue;

            DWORD old;
            VirtualProtect(&thunk[i].u1.Function, sizeof(ULONGLONG), PAGE_READWRITE, &old);
            thunk[i].u1.Function = reinterpret_cast<ULONGLONG>(hook);
            VirtualProtect(&thunk[i].u1.Function, sizeof(ULONGLONG), old, &old);
            ++patched;
        }
    }
    return patched;
}

// Patch ExitProcess in every loaded DLL's IAT so we can't miss it
static void PatchExitProcessAllModules()
{
    HMODULE mods[256] = {};
    DWORD needed = 0;
    // Use EnumProcessModules equivalent via snapshot; but simplest from DllMain:
    // enumerate well-known CRT hosts that call ExitProcess after WinMain returns
    const char* crtHosts[] = {
        "VCRUNTIME140.dll", "vcruntime140.dll",
        "ucrtbase.dll",     "UCRTBASE.DLL",
        "msvcrt.dll",       "MSVCRT.DLL",
    };
    for (int i = 0; i < 6; ++i)
    {
        HMODULE h = GetModuleHandleA(crtHosts[i]);
        if (!h) continue;
        int n = PatchIAT(h, "KERNEL32.dll", "ExitProcess", reinterpret_cast<void*>(HookExitProcess));
        n    += PatchIAT(h, "KERNEL32.DLL", "ExitProcess", reinterpret_cast<void*>(HookExitProcess));
        if (n) Log("Patched ExitProcess in %s (%d slots)", crtHosts[i], n);
    }
}

// Patches (dll, fn) -> hook across EVERY currently-loaded module's own IAT,
// not just the main EXE's. Needed because the actual call can originate from
// inside a system DLL's own implementation (e.g. ucrtbase.dll's fopen/_wrename
// internally calling kernel32's CreateFileW/MoveFileW through UCRTBASE's OWN
// import table) rather than directly from the EXE -- a plain PatchIAT(exe,...)
// never sees those. Confirmed necessary 2026-07-21: the EXE imports several
// api-ms-win-crt-*.dll api-sets (stdio, filesystem), meaning CRT-level file
// I/O is plausible here, and a real PreSaveGame fire produced zero
// "CreateFileW redirected" log lines despite the IAT patch succeeding.
static int PatchIATAllModules(const char* dll, const char* fn, void* hook)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return 0;

    int totalPatched = 0;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me))
    {
        do
        {
            int n = PatchIAT(me.hModule, dll, fn, hook);
            if (n) totalPatched += n;
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return totalPatched;
}

static void LogImportNames(HMODULE hMod)
{
    auto base = reinterpret_cast<BYTE*>(hMod);
    auto dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt   = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) { Log("  no import dir"); return; }
    auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc)
        Log("  import: %s", reinterpret_cast<const char*>(base + desc->Name));
}

// Which copy of the game this is: 0 = first, 1 = second (the one that uses
// Saves_P2). CoSync reads it so two instances on ONE account get different
// characters in the shared host save (CoSyncProfiles::GetMyProfileKey).
extern "C" int CoSyncMultiInstance_Index()
{
    return s_isMyInstance ? 1 : 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        // Log alongside the DLL itself, one file per process so P1 and P2 don't clobber each other
        char logPath[MAX_PATH] = {};
        GetModuleFileNameA(hinstDLL, logPath, sizeof(logPath));
        char* slash = strrchr(logPath, '\\');
        if (slash) ++slash; else slash = logPath;
        _snprintf_s(slash, (DWORD)(sizeof(logPath) - (slash - logPath)), _TRUNCATE,
                    "cosync_proxy_%lu.log", GetCurrentProcessId());

        s_log = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

        Log("nvToolsExt64_1 proxy DllMain fired (pid=%lu)", GetCurrentProcessId());
        AddVectoredExceptionHandler(1, VEH_Handler);
        Log("VEH installed");

        DetectMyInstance();

        HMODULE k32   = GetModuleHandleA("kernel32.dll");
        HMODULE user32 = GetModuleHandleA("user32.dll");

        s_RealCreateMutexA = reinterpret_cast<decltype(s_RealCreateMutexA)>(GetProcAddress(k32, "CreateMutexA"));
        s_RealCreateMutexW = reinterpret_cast<decltype(s_RealCreateMutexW)>(GetProcAddress(k32, "CreateMutexW"));
        s_RealOpenMutexA   = reinterpret_cast<decltype(s_RealOpenMutexA)>(GetProcAddress(k32, "OpenMutexA"));
        s_RealOpenMutexW   = reinterpret_cast<decltype(s_RealOpenMutexW)>(GetProcAddress(k32, "OpenMutexW"));
        s_RealExitProcess  = reinterpret_cast<decltype(s_RealExitProcess)>(GetProcAddress(k32, "ExitProcess"));
        s_RealFindWindowA  = reinterpret_cast<decltype(s_RealFindWindowA)>(GetProcAddress(user32, "FindWindowA"));
        s_RealFindWindowW  = reinterpret_cast<decltype(s_RealFindWindowW)>(GetProcAddress(user32, "FindWindowW"));
        s_RealCreateFileW    = reinterpret_cast<decltype(s_RealCreateFileW)>(GetProcAddress(k32, "CreateFileW"));
        s_RealFindFirstFileW = reinterpret_cast<decltype(s_RealFindFirstFileW)>(GetProcAddress(k32, "FindFirstFileW"));
        s_RealCreateFileA    = reinterpret_cast<decltype(s_RealCreateFileA)>(GetProcAddress(k32, "CreateFileA"));

        HMODULE exe = GetModuleHandleA(NULL);
        Log("exe base = %p", exe);
        LogImportNames(exe);

        int n = 0;
        // Mutex / instance-guard hooks
        n += PatchIAT(exe, "KERNEL32.dll", "CreateMutexA",  reinterpret_cast<void*>(HookCreateMutexA));
        n += PatchIAT(exe, "KERNEL32.DLL", "CreateMutexA",  reinterpret_cast<void*>(HookCreateMutexA));
        n += PatchIAT(exe, "KERNEL32.dll", "CreateMutexW",  reinterpret_cast<void*>(HookCreateMutexW));
        n += PatchIAT(exe, "KERNEL32.DLL", "CreateMutexW",  reinterpret_cast<void*>(HookCreateMutexW));
        n += PatchIAT(exe, "KERNEL32.dll", "OpenMutexA",    reinterpret_cast<void*>(HookOpenMutexA));
        n += PatchIAT(exe, "KERNEL32.DLL", "OpenMutexA",    reinterpret_cast<void*>(HookOpenMutexA));
        n += PatchIAT(exe, "KERNEL32.dll", "OpenMutexW",    reinterpret_cast<void*>(HookOpenMutexW));
        n += PatchIAT(exe, "KERNEL32.DLL", "OpenMutexW",    reinterpret_cast<void*>(HookOpenMutexW));
        // FindWindow — single-instance window check
        n += PatchIAT(exe, "USER32.dll",   "FindWindowA",   reinterpret_cast<void*>(HookFindWindowA));
        n += PatchIAT(exe, "USER32.DLL",   "FindWindowA",   reinterpret_cast<void*>(HookFindWindowA));
        n += PatchIAT(exe, "USER32.dll",   "FindWindowW",   reinterpret_cast<void*>(HookFindWindowW));
        n += PatchIAT(exe, "USER32.DLL",   "FindWindowW",   reinterpret_cast<void*>(HookFindWindowW));
        // ExitProcess in EXE IAT + all CRT hosts
        n += PatchIAT(exe, "KERNEL32.dll", "ExitProcess",   reinterpret_cast<void*>(HookExitProcess));
        n += PatchIAT(exe, "KERNEL32.DLL", "ExitProcess",   reinterpret_cast<void*>(HookExitProcess));
        // Steam restart check
        n += PatchIAT(exe, "steam_api64.dll", "SteamAPI_RestartAppIfNecessary",
                      reinterpret_cast<void*>(HookRestartApp));
        Log("EXE IAT patches: %d", n);

        PatchExitProcessAllModules();

        // Saves folder redirection for "my" instance -- patched across EVERY
        // loaded module's own IAT, not just the EXE's (see PatchIATAllModules
        // comment: the actual call may originate from inside ucrtbase.dll's
        // own CRT file I/O rather than directly from the EXE).
        int nSaves = 0;
        nSaves += PatchIATAllModules("KERNEL32.dll", "CreateFileW",    reinterpret_cast<void*>(HookCreateFileW));
        nSaves += PatchIATAllModules("KERNEL32.DLL", "CreateFileW",    reinterpret_cast<void*>(HookCreateFileW));
        nSaves += PatchIATAllModules("KERNEL32.dll", "FindFirstFileW", reinterpret_cast<void*>(HookFindFirstFileW));
        nSaves += PatchIATAllModules("KERNEL32.DLL", "FindFirstFileW", reinterpret_cast<void*>(HookFindFirstFileW));
        nSaves += PatchIATAllModules("KERNEL32.dll", "CreateFileA",    reinterpret_cast<void*>(HookCreateFileA));
        nSaves += PatchIATAllModules("KERNEL32.DLL", "CreateFileA",    reinterpret_cast<void*>(HookCreateFileA));
        Log("Saves-redirect all-module IAT patches: %d", nSaves);
    }
    return TRUE;
}
