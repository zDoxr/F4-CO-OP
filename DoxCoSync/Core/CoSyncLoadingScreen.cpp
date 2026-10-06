#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")

#include "CoSyncLoadingScreen.h"
#include "resource.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"

static HMODULE GetSelfModule()
{
    LOG_REQUIREMENT("(global)::GetSelfModule", "No FO4 engine state involved -- relies on GetModuleHandleExA resolving the module containing this function's own address; returns null if that fails.");
    HMODULE h = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&GetSelfModule), &h);
    return h;
}

static bool MakeDirRecursive(const wchar_t* path)
{
    LOG_REQUIREMENT("(global)::MakeDirRecursive", "No FO4 engine state involved -- requires path to be a valid, null-terminated wide string no longer than MAX_PATH (wcscpy_s would fault/throw otherwise); recurses on its own parent directory.");
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        return true;

    wchar_t parent[MAX_PATH];
    wcscpy_s(parent, path);
    PathRemoveFileSpecW(parent);
    if (parent[0] && !MakeDirRecursive(parent))
        return false;

    return CreateDirectoryW(path, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

void InjectLoadingScreenTexture()
{
    LOG_REQUIREMENT("(global)::InjectLoadingScreenTexture", "Requires GetSelfModule() to resolve and the IDR_LOADING_BG RT_RCDATA resource to be embedded in this module (both explicitly checked); assumes the exe directory is writable for the Data\\Textures\\Interface output path.");
    HMODULE hSelf = GetSelfModule();
    if (!hSelf)
    {
        LOG_ERROR("[LoadingScreen] GetSelfModule failed");
        return;
    }

    HRSRC hRes = FindResource(hSelf, MAKEINTRESOURCE(IDR_LOADING_BG), RT_RCDATA);
    if (!hRes)
    {
        LOG_WARN("[LoadingScreen] No embedded texture — add LoadingMenuBG.dds to CoSync.rc");
        return;
    }

    HGLOBAL hGlob = LoadResource(hSelf, hRes);
    const void* pData = LockResource(hGlob);
    DWORD size = SizeofResource(hSelf, hRes);
    if (!pData || size == 0)
    {
        LOG_ERROR("[LoadingScreen] Resource lock failed");
        return;
    }

    // Build output path from exe directory
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);

    wchar_t outPath[MAX_PATH];
    swprintf_s(outPath, L"%s\\Data\\Textures\\Interface\\LoadingMenuBG.DDS", exePath);

    if (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES)
    {
        LOG_INFO("[LoadingScreen] Texture already present, skipping inject");
        return;
    }

    wchar_t dirPath[MAX_PATH];
    wcscpy_s(dirPath, outPath);
    PathRemoveFileSpecW(dirPath);
    MakeDirRecursive(dirPath);

    HANDLE hFile = CreateFileW(outPath, GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        LOG_ERROR("[LoadingScreen] CreateFile failed: %u", GetLastError());
        return;
    }

    DWORD written = 0;
    WriteFile(hFile, pData, size, &written, nullptr);
    CloseHandle(hFile);

    LOG_INFO("[LoadingScreen] Injected LoadingMenuBG.DDS (%u bytes)", written);
}

// ════════════════════════════════════════════════════════════════════════════
// 0.5.1 (Bug #9): the one CoSync loading screen (see CoSyncLoadingScreen.h)
// ════════════════════════════════════════════════════════════════════════════
#include "CoSyncPrismaUI.h"
#include "CoSyncWorldTransfer.h"
#include "Relocation.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace
{
    constexpr uintptr_t kLoadingMenuVtableRVA = 0x025B0458;   // ??_7LoadingMenu@@6B@
    constexpr size_t    kSlotDtor  = 0x0;                     // scalar deleting destructor
    constexpr size_t    kSlotFrame = 0x10;                    // per-frame update (same base slot MainMenu's hook uses)

    using Frame_t = bool(__fastcall*)(void* self);
    using Dtor_t  = void*(__fastcall*)(void* self, unsigned int flags);
    Frame_t g_origFrame = nullptr;
    Dtor_t  g_origDtor  = nullptr;

    std::atomic<bool>     s_menuAlive{ false };
    // 0.5.2: a load is often several LoadingMenus back to back (2026-10-05
    // client log: closed 21:02:29.132, up again .162). Hiding on each close
    // let the vanilla screen show until the next show arrived -- the reported
    // flicker. The screen stays up this long after the last one closes.
    std::atomic<uint64_t> s_menuClosedAtMs{ 0 };
    constexpr uint64_t    kLingerMs = 1500;
    bool                  s_viewPrimed = false;   // the page is created (hidden) before the first load
    std::atomic<uint64_t> s_reasonUntilMs[static_cast<int>(CoSyncLoading::Reason::Count)] = {};
    std::atomic<bool>     s_showing{ false };
    std::mutex            s_applyMx;
    std::string           s_lastStatus;
    bool                  s_lastShow = false;
    uint64_t              s_nextRefreshMs = 0;

    uint64_t NowMs() { return GetTickCount64(); }

    bool ReasonActive(CoSyncLoading::Reason r)
    {
        const uint64_t until = s_reasonUntilMs[static_cast<int>(r)].load(std::memory_order_relaxed);
        return until != 0 && NowMs() < until;
    }

    // Decide, and push to the view only when something changed (or every
    // 0.5 s while it is up, so a view created mid-load gets its text).
    void Apply()
    {
        using P = CoSyncWorldTransfer::ClientPhase;
        const P phase = CoSyncWorldTransfer::GetClientPhase();
        const bool joining = phase == P::Downloading || phase == P::Loading;
        const bool respawn = ReasonActive(CoSyncLoading::Reason::Respawn);
        const bool newGame = ReasonActive(CoSyncLoading::Reason::NewGame);
        const uint64_t closedAt = s_menuClosedAtMs.load();
        const bool lingering = closedAt != 0 && GetTickCount64() - closedAt < kLingerMs;
        const bool show = s_menuAlive.load() || lingering || joining || respawn || newGame;

        std::string status;
        char buf[64];
        if (respawn)
            status = "Respawning";
        else if (phase == P::Downloading)
        {
            std::snprintf(buf, sizeof(buf), "Downloading the host's world %d%%", CoSyncWorldTransfer::GetClientProgressPercent());
            status = buf;
        }
        else if (phase == P::Loading)
            status = "Joining the host's world";
        else if (newGame)
            status = "Waiting for the host's game";
        else
            status = "Syncing Commonwealth";

        std::lock_guard<std::mutex> lk(s_applyMx);
        const uint64_t now = NowMs();
        if (!s_viewPrimed && !show)
        {
            // Build the page once, hidden, so a load never waits for it.
            s_viewPrimed = CoSyncPrismaUI::SetLoadingView(false, status.c_str());
            return;
        }
        if (show == s_lastShow && status == s_lastStatus && (!show || now < s_nextRefreshMs))
            return;
        s_lastShow = show;
        s_lastStatus = status;
        s_nextRefreshMs = now + 500;
        s_showing.store(show);
        CoSyncPrismaUI::SetLoadingView(show, status.c_str());
    }

    bool __fastcall LoadingMenu_Frame_Hook(void* self)
    {
        const bool r = g_origFrame ? g_origFrame(self) : false;
        if (!s_menuAlive.exchange(true))
            LOG_INFO("[Loading] LoadingMenu up -- CoSync loading screen shown");
        Apply();
        return r;
    }

    void* __fastcall LoadingMenu_Dtor_Hook(void* self, unsigned int flags)
    {
        if (s_menuAlive.exchange(false))
            LOG_INFO("[Loading] LoadingMenu closed");
        s_menuClosedAtMs.store(GetTickCount64());
        void* r = g_origDtor ? g_origDtor(self, flags) : nullptr;
        Apply();
        return r;
    }

    template <typename T>
    bool PatchSlot(size_t index, void* detour, T& orig, const char* what)
    {
        uintptr_t* slot = reinterpret_cast<uintptr_t*>(RelocationManager::s_baseAddr + kLoadingMenuVtableRVA + index * sizeof(uintptr_t));
        const uintptr_t cur = *slot;
        if (!cur || cur == reinterpret_cast<uintptr_t>(detour))
            return false;
        orig = reinterpret_cast<T>(cur);
        DWORD oldProt{};
        VirtualProtect(slot, sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProt);
        *slot = reinterpret_cast<uintptr_t>(detour);
        VirtualProtect(slot, sizeof(uintptr_t), oldProt, &oldProt);
        LOG_INFO("[Loading] LoadingMenu %s hooked (vtable slot %zu, orig=%p)", what, index, reinterpret_cast<void*>(cur));
        return true;
    }
}

namespace CoSyncLoading
{
    void InstallLoadingMenuHooks()
    {
        static bool s_done = false;
        if (s_done)
            return;
        s_done = true;
        // Sanity: the vtable's first slot must point into the game's code.
        const uintptr_t first = *reinterpret_cast<uintptr_t*>(RelocationManager::s_baseAddr + kLoadingMenuVtableRVA);
        if (first < RelocationManager::s_baseAddr || first > RelocationManager::s_baseAddr + 0x4000000)
        {
            LOG_ERROR("[Loading] LoadingMenu vtable not where 1.11.240 has it -- CoSync loading screen limited to CoSync's own loads");
            return;
        }
        PatchSlot(kSlotFrame, reinterpret_cast<void*>(&LoadingMenu_Frame_Hook), g_origFrame, "frame update");
        PatchSlot(kSlotDtor, reinterpret_cast<void*>(&LoadingMenu_Dtor_Hook), g_origDtor, "destructor");
    }

    void Begin(Reason reason, double seconds)
    {
        s_reasonUntilMs[static_cast<int>(reason)].store(NowMs() + static_cast<uint64_t>(seconds * 1000.0));
        Apply();
    }

    void End(Reason reason)
    {
        s_reasonUntilMs[static_cast<int>(reason)].store(0);
        Apply();
    }

    void Tick()
    {
        Apply();
    }

    bool IsShowing()
    {
        return s_showing.load();
    }
}
