#include "CoSyncSteam.h"
#include "ConsoleLogger.h"

#include <Windows.h>
#include <cstdint>
#include <string>
#include "MinHook.h"

// ============================================================================
// Minimal Steam types & interfaces
// ============================================================================

typedef uint32_t HSteamUser;
typedef uint32_t HSteamPipe;

static const HSteamUser k_HSteamUserInvalid = 0;

// Minimal CSteamID stand-in
struct CSteamID
{
    uint64_t id = 0;
    uint64_t ConvertToUint64() const { return id; }
    CSteamID() = default;
    explicit CSteamID(uint64_t v) : id(v) {}
};

// Minimal interfaces: we never call virtuals, only flat exports.
struct ISteamUser {};
struct ISteamFriends {};
struct ISteamUtils {};

// ============================================================================
// Globals
// ============================================================================

static HMODULE g_hSteamAPI = nullptr;

// Function pointer types
using SteamAPI_GetHSteamUser_t = HSteamUser(__cdecl*)();
using SteamAPI_SteamUser_v021_t = ISteamUser * (__cdecl*)();
using SteamAPI_SteamFriends_v017_t = ISteamFriends * (__cdecl*)();
using SteamAPI_SteamUtils_v010_t = ISteamUtils * (__cdecl*)();
using SteamAPI_ISteamFriends_GetPersonaName_t = const char* (__cdecl*)(ISteamFriends* self);
using SteamAPI_ISteamFriends_GetFriendCount_t = int(__cdecl*)(ISteamFriends* self, int flags);
using SteamAPI_ISteamUser_GetSteamID_t = CSteamID(__cdecl*)(ISteamUser* self);
using SteamAPI_ISteamFriends_GetFriendByIndex_t = CSteamID(__cdecl*)(ISteamFriends* self, int idx, int flags);
using SteamAPI_ISteamFriends_GetFriendPersonaName_t = const char* (__cdecl*)(ISteamFriends* self, CSteamID steamID);
using SteamAPI_ISteamFriends_InviteUserToGame_t = void(__cdecl*)(ISteamFriends* self, CSteamID steamIDFriend, const char* connectString);

// Pointers to Steam exports
static SteamAPI_GetHSteamUser_t                      g_Orig_GetHSteamUser = nullptr;
static SteamAPI_SteamUser_v021_t                     g_SteamAPI_SteamUser_v021 = nullptr;
static SteamAPI_SteamFriends_v017_t                  g_SteamAPI_SteamFriends_v017 = nullptr;
static SteamAPI_SteamUtils_v010_t                    g_SteamAPI_SteamUtils_v010 = nullptr;
static SteamAPI_ISteamFriends_GetPersonaName_t       g_SteamAPI_ISteamFriends_GetPersonaName = nullptr;
static SteamAPI_ISteamFriends_GetFriendCount_t       g_SteamAPI_ISteamFriends_GetFriendCount = nullptr;
static SteamAPI_ISteamUser_GetSteamID_t              g_SteamAPI_ISteamUser_GetSteamID = nullptr;
static SteamAPI_ISteamFriends_GetFriendByIndex_t     g_SteamAPI_ISteamFriends_GetFriendByIndex = nullptr;
static SteamAPI_ISteamFriends_GetFriendPersonaName_t g_SteamAPI_ISteamFriends_GetFriendPersonaName = nullptr;
static SteamAPI_ISteamFriends_InviteUserToGame_t     g_SteamAPI_ISteamFriends_InviteUserToGame = nullptr;

// Hook target
static void* g_Target_GetHSteamUser = nullptr;

// Resolved interfaces
static ISteamUser* g_pSteamUser = nullptr;
static ISteamFriends* g_pSteamFriends = nullptr;
static ISteamUtils* g_pSteamUtils = nullptr;

// Cached identity
static bool        g_ready = false;   // interfaces resolved & valid
static bool        g_identityInit = false;   // persona / friends / SteamID populated
static uint64_t    g_selfSteamID64 = 0;
static std::string g_personaName;
static int         g_lastFriendCount = -1;

// ============================================================================
// Export dump (for debugging)
// ============================================================================

static void DumpSteamExports(HMODULE hSteam)
{
    if (!hSteam)
    {
        LOG_DEBUG("[CoSyncSteam] DumpSteamExports: hSteam is null");
        return;
    }

    LOG_DEBUG("========== CoSyncSteam: Export map ==========");

    auto dump = [hSteam](const char* name) -> FARPROC
        {
            FARPROC p = GetProcAddress(hSteam, name);
            LOG_DEBUG("[CoSyncSteam] [Export] %-40s = %p", name, p);
            return p;
        };

    g_SteamAPI_SteamUser_v021 =
        reinterpret_cast<SteamAPI_SteamUser_v021_t>(
            dump("SteamAPI_SteamUser_v021")
            );

    g_SteamAPI_SteamFriends_v017 =
        reinterpret_cast<SteamAPI_SteamFriends_v017_t>(
            dump("SteamAPI_SteamFriends_v017")
            );

    g_SteamAPI_SteamUtils_v010 =
        reinterpret_cast<SteamAPI_SteamUtils_v010_t>(
            dump("SteamAPI_SteamUtils_v010")
            );

    g_SteamAPI_ISteamFriends_GetPersonaName =
        reinterpret_cast<SteamAPI_ISteamFriends_GetPersonaName_t>(
            dump("SteamAPI_ISteamFriends_GetPersonaName")
            );

    g_SteamAPI_ISteamFriends_GetFriendCount =
        reinterpret_cast<SteamAPI_ISteamFriends_GetFriendCount_t>(
            dump("SteamAPI_ISteamFriends_GetFriendCount")
            );

    g_SteamAPI_ISteamUser_GetSteamID =
        reinterpret_cast<SteamAPI_ISteamUser_GetSteamID_t>(
            dump("SteamAPI_ISteamUser_GetSteamID")
            );

    g_SteamAPI_ISteamFriends_GetFriendByIndex =
        reinterpret_cast<SteamAPI_ISteamFriends_GetFriendByIndex_t>(
            dump("SteamAPI_ISteamFriends_GetFriendByIndex")
            );

    g_SteamAPI_ISteamFriends_GetFriendPersonaName =
        reinterpret_cast<SteamAPI_ISteamFriends_GetFriendPersonaName_t>(
            dump("SteamAPI_ISteamFriends_GetFriendPersonaName")
            );

    g_SteamAPI_ISteamFriends_InviteUserToGame =
        reinterpret_cast<SteamAPI_ISteamFriends_InviteUserToGame_t>(
            dump("SteamAPI_ISteamFriends_InviteUserToGame")
            );

    dump("SteamAPI_Init");
    dump("SteamAPI_Shutdown");
    dump("SteamAPI_GetHSteamUser");
    dump("SteamAPI_GetHSteamPipe");

    LOG_DEBUG("=============================================");
}

// ============================================================================
// Lazy interface resolution (NO calls from inside hook)
// ============================================================================

static bool CoSyncSteam_EnsureInterfaces()
{
    if (g_ready)
        return true;

    if (!g_SteamAPI_SteamUser_v021 ||
        !g_SteamAPI_SteamFriends_v017 ||
        !g_SteamAPI_SteamUtils_v010)
    {
        LOG_ERROR("[CoSyncSteam] EnsureInterfaces: required exports missing");
        return false;
    }

    ISteamUser* user = g_SteamAPI_SteamUser_v021();
    ISteamFriends* friends = g_SteamAPI_SteamFriends_v017();
    ISteamUtils* utils = g_SteamAPI_SteamUtils_v010();

    LOG_DEBUG("[CoSyncSteam] EnsureInterfaces: ISteamUser    = %p", user);
    LOG_DEBUG("[CoSyncSteam] EnsureInterfaces: ISteamFriends = %p", friends);
    LOG_DEBUG("[CoSyncSteam] EnsureInterfaces: ISteamUtils   = %p", utils);

    if (!user || !friends)
    {
        LOG_ERROR("[CoSyncSteam] EnsureInterfaces: ISteamUser/ISteamFriends null");
        return false;
    }

    g_pSteamUser = user;
    g_pSteamFriends = friends;
    g_pSteamUtils = utils;

    g_ready = true;
    g_identityInit = false;

    LOG_INFO("[CoSyncSteam] Interfaces resolved successfully");
    return true;
}

// ============================================================================
// Identity refresh (persona, friend count, self SteamID)
// ============================================================================

static void CoSyncSteam_RefreshIdentity()
{
    if (!CoSyncSteam_EnsureInterfaces())
        return;

    if (g_identityInit)
        return;

    g_identityInit = true;

    if (g_SteamAPI_ISteamFriends_GetPersonaName)
    {
        const char* n = g_SteamAPI_ISteamFriends_GetPersonaName(g_pSteamFriends);
        g_personaName = n ? n : "";
    }

    if (g_SteamAPI_ISteamFriends_GetFriendCount)
    {
        const int k_EFriendFlagImmediate = 0x01;
        g_lastFriendCount = g_SteamAPI_ISteamFriends_GetFriendCount(
            g_pSteamFriends,
            k_EFriendFlagImmediate
        );
    }

    if (g_SteamAPI_ISteamUser_GetSteamID)
    {
        CSteamID sid = g_SteamAPI_ISteamUser_GetSteamID(g_pSteamUser);
        g_selfSteamID64 = sid.ConvertToUint64();
    }

    LOG_DEBUG(
        "[CoSyncSteam] Identity OK – friendCount=%d, persona='%s', steamID=%llu",
        g_lastFriendCount,
        g_personaName.empty() ? "<null>" : g_personaName.c_str(),
        static_cast<unsigned long long>(g_selfSteamID64)
    );
}

// ============================================================================
// Hook: SteamAPI_GetHSteamUser
//  - NOW: only logs, DOES NOT touch interfaces.
// ============================================================================

static HSteamUser __cdecl Hook_GetHSteamUser()
{
    HSteamUser user =
        g_Orig_GetHSteamUser ? g_Orig_GetHSteamUser() : k_HSteamUserInvalid;

    // You can throttle this later if it's too spammy.
    LOG_DEBUG("[CoSyncSteam] SteamAPI_GetHSteamUser intercepted – Steam init OK (user=%u)", user);

    // CRASH-FREE: we DO NOT call CoSyncSteam_EnsureInterfaces() here.

    return user;
}

// ============================================================================
// Public API
// ============================================================================

bool CoSyncSteam_Init()
{
    if (g_hSteamAPI)
        return true;

    g_hSteamAPI = GetModuleHandleA("steam_api64.dll");
    if (!g_hSteamAPI)
    {
        LOG_ERROR("[CoSyncSteam] steam_api64.dll NOT loaded – Steam overlay probably disabled");
        return false;
    }

    DumpSteamExports(g_hSteamAPI);

    FARPROC pGetHSteamUser = GetProcAddress(g_hSteamAPI, "SteamAPI_GetHSteamUser");
    if (!pGetHSteamUser)
    {
        LOG_ERROR("[CoSyncSteam] SteamAPI_GetHSteamUser not found in steam_api64.dll");
        return false;
    }

    g_Target_GetHSteamUser = reinterpret_cast<void*>(pGetHSteamUser);
    g_Orig_GetHSteamUser = reinterpret_cast<SteamAPI_GetHSteamUser_t>(pGetHSteamUser);

    // MinHook setup
    MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[CoSyncSteam] MH_Initialize failed (%d)", (int)mh);
        return false;
    }

    mh = MH_CreateHook(
        g_Target_GetHSteamUser,
        reinterpret_cast<LPVOID>(&Hook_GetHSteamUser),
        reinterpret_cast<LPVOID*>(&g_Orig_GetHSteamUser)
    );

    if (mh != MH_OK)
    {
        LOG_ERROR("[CoSyncSteam] MH_CreateHook(SteamAPI_GetHSteamUser) failed (%d)", (int)mh);
        return false;
    }

    mh = MH_EnableHook(g_Target_GetHSteamUser);
    if (mh != MH_OK)
    {
        LOG_ERROR("[CoSyncSteam] MH_EnableHook(SteamAPI_GetHSteamUser) failed (%d)", (int)mh);
        return false;
    }

    LOG_INFO("[CoSyncSteam] Hook installed successfully");
    return true;
}

void CoSyncSteam_Shutdown()
{
    LOG_INFO("[CoSyncSteam] Shutdown");

    if (g_Target_GetHSteamUser)
    {
        MH_DisableHook(g_Target_GetHSteamUser);
        g_Target_GetHSteamUser = nullptr;
    }

    MH_Uninitialize();

    g_pSteamUser = nullptr;
    g_pSteamFriends = nullptr;
    g_pSteamUtils = nullptr;

    g_hSteamAPI = nullptr;

    g_ready = false;
    g_identityInit = false;
    g_selfSteamID64 = 0;
    g_personaName.clear();
    g_lastFriendCount = -1;
}

// ---------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------

bool CoSyncSteam_IsReady()
{
    // Keep this: it just reflects whether we resolved interfaces.
    return g_ready;
}

const char* CoSyncSteam_GetPersonaName()
{
    
    CoSyncSteam_RefreshIdentity();
    if (g_personaName.empty())
        return nullptr;
    return g_personaName.c_str();
}

uint64_t CoSyncSteam_GetSelfSteamID64()
{
    CoSyncSteam_RefreshIdentity();
    return g_selfSteamID64;
}

int CoSyncSteam_GetFriendCount()
{
    LOG_WARN("[CoSyncSteam] GetFriendCount stubbed out (no Steam calls)");
    return 0;
}

bool CoSyncSteam_GetFriendInfo(int index, uint64_t& outSteamID, std::string& outName)
{
    (void)index;
    outSteamID = 0;
    outName.clear();

    LOG_WARN("[CoSyncSteam] GetFriendInfo stubbed out (no Steam calls)");
    return false;
}

bool CoSyncSteam_InviteFriendToGame(uint64_t friendSteamID, const char* connectString)
{
    (void)friendSteamID;
    (void)connectString;

    LOG_WARN("[CoSyncSteam] InviteFriendToGame stubbed out (no Steam calls)");
    return false;
}


// Low-level access
ISteamUser* CoSyncSteam_GetUser()
{
    CoSyncSteam_EnsureInterfaces();
    return g_pSteamUser;
}

ISteamFriends* CoSyncSteam_GetFriends()
{
    CoSyncSteam_EnsureInterfaces();
    return g_pSteamFriends;
}

ISteamUtils* CoSyncSteam_GetUtils()
{
    CoSyncSteam_EnsureInterfaces();
    return g_pSteamUtils;
}
