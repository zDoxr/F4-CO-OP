#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncSteamID.h"

#include "ConsoleLogger.h"

#include <atomic>

namespace
{
    // Flat-API signatures. The interface pointers are opaque to us -- we only
    // ever hand them straight back to the matching flat accessor, so void* is
    // both sufficient and safer than inventing a fake class layout.
    using FnSteamFriends   = void*       (__cdecl*)();
    using FnSteamUser      = void*       (__cdecl*)();
    using FnGetPersonaName = const char* (__cdecl*)(void* self);
    using FnGetSteamID     = uint64_t    (__cdecl*)(void* self);
    using FnIsSteamRunning = bool        (__cdecl*)();

    FnGetPersonaName g_getPersonaName = nullptr;
    FnGetSteamID     g_getSteamID     = nullptr;
    void*            g_friends        = nullptr;
    void*            g_user           = nullptr;

    std::atomic<bool> g_initialized{ false };
    std::atomic<bool> g_available{ false };

    uint64_t    g_cachedID = 0;
    std::string g_cachedName;

    // ISteamUser has shipped several versions; bind the newest the loaded DLL
    // actually exports rather than pinning one that may not exist.
    const char* const kUserAccessors[] = {
        "SteamAPI_SteamUser_v023",
        "SteamAPI_SteamUser_v022",
        "SteamAPI_SteamUser_v021",
        "SteamAPI_SteamUser_v020",
    };

    // Every call below crosses into Steam's DLL with pointers we did not
    // allocate, so each is SEH-wrapped. noinline + no noexcept is load-bearing
    // under this project's /GL+/LTCG Release config -- see the long note in
    // CoSyncPickupHook.cpp's identical helpers.
    __declspec(noinline) void* SafeCallAccessor(FnSteamFriends fn)
    {
        __try { return fn ? fn() : nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    __declspec(noinline) const char* SafeGetPersonaName()
    {
        __try
        {
            if (!g_getPersonaName || !g_friends) return nullptr;
            return g_getPersonaName(g_friends);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    __declspec(noinline) uint64_t SafeGetSteamID()
    {
        __try
        {
            if (!g_getSteamID || !g_user) return 0;
            return g_getSteamID(g_user);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
}

bool CoSyncSteamID::Init()
{

    if (g_initialized.load())
        return g_available.load();

    // Deliberately GetModuleHandle, not LoadLibrary: Fallout 4 is a Steam title
    // and has already loaded and initialised this DLL. Loading our own copy
    // (redistributable_bin\win64) would create a second, uninitialised Steam
    // context whose accessors return null at best.
    HMODULE mod = ::GetModuleHandleA("steam_api64.dll");
    if (!mod)
    {
        LOG_WARN("[SteamID] steam_api64.dll not loaded in this process — "
                 "falling back to the CoSync display name");
        g_initialized.store(true);
        return false;
    }

    auto friendsAccessor = reinterpret_cast<FnSteamFriends>(
        ::GetProcAddress(mod, "SteamAPI_SteamFriends_v017"));
    g_getPersonaName = reinterpret_cast<FnGetPersonaName>(
        ::GetProcAddress(mod, "SteamAPI_ISteamFriends_GetPersonaName"));
    g_getSteamID = reinterpret_cast<FnGetSteamID>(
        ::GetProcAddress(mod, "SteamAPI_ISteamUser_GetSteamID"));

    FnSteamUser userAccessor = nullptr;
    const char* boundUser = nullptr;
    for (const char* name : kUserAccessors)
    {
        userAccessor = reinterpret_cast<FnSteamUser>(::GetProcAddress(mod, name));
        if (userAccessor) { boundUser = name; break; }
    }

    if (!friendsAccessor || !userAccessor || !g_getPersonaName || !g_getSteamID)
    {
        LOG_WARN("[SteamID] flat exports missing (friends=%p user=%p "
                 "getPersonaName=%p getSteamID=%p) — falling back",
            (void*)friendsAccessor, (void*)userAccessor,
            (void*)g_getPersonaName, (void*)g_getSteamID);
        g_initialized.store(true);
        return false;
    }

    g_friends = SafeCallAccessor(friendsAccessor);
    g_user    = SafeCallAccessor(reinterpret_cast<FnSteamFriends>(userAccessor));

    if (!g_friends || !g_user)
    {
        // Not fatal and not necessarily permanent: this happens when Steam is
        // still starting. Leave g_initialized false so a later call retries.
        LOG_WARN("[SteamID] interfaces not ready yet (friends=%p user=%p) — will retry",
            g_friends, g_user);
        return false;
    }

    g_initialized.store(true);
    g_available.store(true);
    LOG_INFO("[SteamID] bound OK (user accessor '%s') steamID=%llu persona='%s'",
        boundUser ? boundUser : "?",
        (unsigned long long)GetSteamID64(),
        GetPersonaName().c_str());
    return true;
}

bool CoSyncSteamID::IsAvailable()
{
    return g_available.load();
}

uint64_t CoSyncSteamID::GetSteamID64()
{
    if (g_cachedID != 0)
        return g_cachedID;
    if (!g_available.load() && !Init())
        return 0;

    const uint64_t id = SafeGetSteamID();
    if (id != 0)
        g_cachedID = id;
    return id;
}

const std::string& CoSyncSteamID::GetPersonaName()
{
    if (!g_cachedName.empty())
        return g_cachedName;
    if (!g_available.load() && !Init())
        return g_cachedName;

    const char* n = SafeGetPersonaName();
    if (n && *n)
        g_cachedName.assign(n);
    return g_cachedName;
}
