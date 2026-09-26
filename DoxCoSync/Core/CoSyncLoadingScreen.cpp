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
