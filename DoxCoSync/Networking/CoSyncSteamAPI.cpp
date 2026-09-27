#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncSteamAPI.h"

#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"

#include "steam/steamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"

#include <atomic>

namespace
{
    // ---- flat signatures --------------------------------------------------
    using FnAccessor        = void*    (__cdecl*)();
    using FnGetSteamID      = uint64_t (__cdecl*)(void*);
    using FnGetAppID        = uint32_t (__cdecl*)(void*);
    using FnIsCallCompleted = bool     (__cdecl*)(void*, uint64_t, bool*);
    using FnGetCallResult   = bool     (__cdecl*)(void*, uint64_t, void*, int, int, bool*);
    using FnCreateLobby     = uint64_t (__cdecl*)(void*, int, int);
    using FnJoinLobby       = uint64_t (__cdecl*)(void*, uint64_t);
    using FnLeaveLobby      = void     (__cdecl*)(void*, uint64_t);
    using FnSetLobbyData    = bool     (__cdecl*)(void*, uint64_t, const char*, const char*);
    using FnGetLobbyData    = const char* (__cdecl*)(void*, uint64_t, const char*);
    using FnLobbyBool       = bool     (__cdecl*)(void*, uint64_t);
    using FnLobbyOwner      = uint64_t (__cdecl*)(void*, uint64_t);
    using FnLobbyInt        = int      (__cdecl*)(void*, uint64_t);
    using FnSetJoinable     = bool     (__cdecl*)(void*, uint64_t, bool);
    using FnFriendCount     = int      (__cdecl*)(void*, int);
    using FnFriendByIndex   = uint64_t (__cdecl*)(void*, int, int);
    using FnFriendName      = const char* (__cdecl*)(void*, uint64_t);
    using FnFriendGame      = bool     (__cdecl*)(void*, uint64_t, void*);
    using FnSetRichPresence = bool     (__cdecl*)(void*, const char*, const char*);
    using FnClearRP         = void     (__cdecl*)(void*);
    using FnInviteDialog    = void     (__cdecl*)(void*, uint64_t);
    using FnRegisterCB      = void     (__cdecl*)(void*, int);
    using FnGetRelayStatus  = int      (__cdecl*)(void*, void*);
    using FnCheckPing       = bool     (__cdecl*)(void*, float);

    struct Api
    {
        void* sockets = nullptr;    // ISteamNetworkingSockets* (Steam's)
        void* utils   = nullptr;    // ISteamNetworkingUtils*   (Steam's)
        void* mm      = nullptr;    // ISteamMatchmaking*
        void* friends = nullptr;    // ISteamFriends*
        void* user    = nullptr;    // ISteamUser*
        void* sutils  = nullptr;    // ISteamUtils*

        FnGetSteamID      getSteamID = nullptr;
        FnGetAppID        getAppID = nullptr;
        FnIsCallCompleted isCallCompleted = nullptr;
        FnGetCallResult   getCallResult = nullptr;
        FnCreateLobby     createLobby = nullptr;
        FnJoinLobby       joinLobby = nullptr;
        FnLeaveLobby      leaveLobby = nullptr;
        FnSetLobbyData    setLobbyData = nullptr;
        FnGetLobbyData    getLobbyData = nullptr;
        FnLobbyBool       requestLobbyData = nullptr;
        FnLobbyOwner      lobbyOwner = nullptr;
        FnLobbyInt        numMembers = nullptr;
        FnSetJoinable     setJoinable = nullptr;
        FnFriendCount     friendCount = nullptr;
        FnFriendByIndex   friendByIndex = nullptr;
        FnFriendName      friendName = nullptr;
        FnFriendGame      friendGame = nullptr;
        FnSetRichPresence setRichPresence = nullptr;
        FnClearRP         clearRichPresence = nullptr;
        FnInviteDialog    inviteDialog = nullptr;
        FnRegisterCB      registerCallback = nullptr;
        FnGetRelayStatus  relayStatus = nullptr;
        FnCheckPing       checkPing = nullptr;
    };
    Api g;
    std::atomic<bool> g_available{ false };

    template <typename T>
    bool Bind(HMODULE mod, T& out, const char* name)
    {
        out = reinterpret_cast<T>(::GetProcAddress(mod, name));
        if (!out)
            LOG_WARN("[SteamAPI] export missing: %s", name);
        return out != nullptr;
    }

    __declspec(noinline) void* SafeAccessor(FnAccessor fn)
    {
        __try { return fn ? fn() : nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    // ---- Steam's CCallbackBase, declared exactly as steam_api_common.h does
    // (the overloaded Run pair's vtable order is MSVC's, same compiler as the
    // DLL's). SteamAPI_RegisterCallback sets the registered flag.
    class CCallbackBase
    {
    public:
        CCallbackBase() { m_nCallbackFlags = 0; m_iCallback = 0; }
        virtual void Run(void* pvParam) = 0;
        virtual void Run(void* pvParam, bool bIOFailure, uint64_t hSteamAPICall) = 0;
        int GetICallback() { return m_iCallback; }
        virtual int GetCallbackSizeBytes() = 0;
    protected:
        uint8_t m_nCallbackFlags;
        int     m_iCallback;
    };

    template <typename Handler>
    class StaticCallback final : public CCallbackBase
    {
    public:
        StaticCallback(int id, int size, Handler h) : m_size(size), m_handler(h) { m_iCallback = id; }
        void Run(void* p) override { if (m_handler) m_handler(p); }
        void Run(void* p, bool, uint64_t) override { if (m_handler) m_handler(p); }
        int  GetCallbackSizeBytes() override { return m_size; }
    private:
        int     m_size;
        Handler m_handler;
    };
    using RawHandler = void(*)(void*);

    CoSyncSteamAPI::ConnStatusFn  g_connStatusFn = nullptr;
    CoSyncSteamAPI::LobbyInviteFn g_lobbyInviteFn = nullptr;

    void OnConnStatusRaw(void* p)
    {
        if (g_connStatusFn)
            g_connStatusFn(static_cast<SteamNetConnectionStatusChangedCallback_t*>(p));
    }
    void OnLobbyJoinRequestedRaw(void* p)
    {
        // GameLobbyJoinRequested_t { CSteamID lobby; CSteamID friend; }
        const uint64_t* v = static_cast<const uint64_t*>(p);
        if (g_lobbyInviteFn)
            g_lobbyInviteFn(v[0], v[1]);
    }

    StaticCallback<RawHandler> g_connStatusCB(
        SteamNetConnectionStatusChangedCallback_t::k_iCallback,
        static_cast<int>(sizeof(SteamNetConnectionStatusChangedCallback_t)), &OnConnStatusRaw);
    StaticCallback<RawHandler> g_lobbyJoinCB(
        CoSyncSteamAPI::kCallback_GameLobbyJoinRequested, 16, &OnLobbyJoinRequestedRaw);

    // ---- SEH-guarded calls (every one crosses into Steam's DLL) ----------
    __declspec(noinline) bool SafeRegister(CCallbackBase* cb, int id)
    {
        __try { g.registerCallback(cb, id); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    __declspec(noinline) uint64_t SafeU64Lobby(FnLobbyOwner fn, void* self, uint64_t a)
    {
        __try { return fn ? fn(self, a) : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    __declspec(noinline) const char* SafeLobbyData(uint64_t lobby, const char* key)
    {
        __try { return g.getLobbyData ? g.getLobbyData(g.mm, lobby, key) : nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    __declspec(noinline) const char* SafeFriendName(uint64_t id)
    {
        __try { return g.friendName ? g.friendName(g.friends, id) : nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    __declspec(noinline) bool SafeFriendGame(uint64_t id, void* out)
    {
        __try { return g.friendGame ? g.friendGame(g.friends, id, out) : false; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

bool CoSyncSteamAPI::Init()
{
    LOG_REQUIREMENT("CoSyncSteamAPI::Init", "Needs steam_api64.dll already loaded and initialised by the game. Idempotent; returns false (retry later) until every export and interface is ready.");
    if (g_available.load())
        return true;

    HMODULE mod = ::GetModuleHandleA("steam_api64.dll");
    if (!mod)
        return false;

    FnAccessor aSockets = nullptr, aUtils = nullptr, aMM = nullptr, aFriends = nullptr, aUser = nullptr, aSUtils = nullptr;
    bool ok = true;
    ok &= Bind(mod, aSockets, "SteamAPI_SteamNetworkingSockets_SteamAPI_v012");
    ok &= Bind(mod, aUtils,   "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    ok &= Bind(mod, aMM,      "SteamAPI_SteamMatchmaking_v009");
    ok &= Bind(mod, aFriends, "SteamAPI_SteamFriends_v017");
    ok &= Bind(mod, aUser,    "SteamAPI_SteamUser_v021");
    ok &= Bind(mod, aSUtils,  "SteamAPI_SteamUtils_v010");
    ok &= Bind(mod, g.getSteamID,       "SteamAPI_ISteamUser_GetSteamID");
    ok &= Bind(mod, g.getAppID,         "SteamAPI_ISteamUtils_GetAppID");
    ok &= Bind(mod, g.isCallCompleted,  "SteamAPI_ISteamUtils_IsAPICallCompleted");
    ok &= Bind(mod, g.getCallResult,    "SteamAPI_ISteamUtils_GetAPICallResult");
    ok &= Bind(mod, g.createLobby,      "SteamAPI_ISteamMatchmaking_CreateLobby");
    ok &= Bind(mod, g.joinLobby,        "SteamAPI_ISteamMatchmaking_JoinLobby");
    ok &= Bind(mod, g.leaveLobby,       "SteamAPI_ISteamMatchmaking_LeaveLobby");
    ok &= Bind(mod, g.setLobbyData,     "SteamAPI_ISteamMatchmaking_SetLobbyData");
    ok &= Bind(mod, g.getLobbyData,     "SteamAPI_ISteamMatchmaking_GetLobbyData");
    ok &= Bind(mod, g.requestLobbyData, "SteamAPI_ISteamMatchmaking_RequestLobbyData");
    ok &= Bind(mod, g.lobbyOwner,       "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    ok &= Bind(mod, g.numMembers,       "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    ok &= Bind(mod, g.setJoinable,      "SteamAPI_ISteamMatchmaking_SetLobbyJoinable");
    ok &= Bind(mod, g.friendCount,      "SteamAPI_ISteamFriends_GetFriendCount");
    ok &= Bind(mod, g.friendByIndex,    "SteamAPI_ISteamFriends_GetFriendByIndex");
    ok &= Bind(mod, g.friendName,       "SteamAPI_ISteamFriends_GetFriendPersonaName");
    ok &= Bind(mod, g.friendGame,       "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    ok &= Bind(mod, g.setRichPresence,  "SteamAPI_ISteamFriends_SetRichPresence");
    ok &= Bind(mod, g.clearRichPresence,"SteamAPI_ISteamFriends_ClearRichPresence");
    ok &= Bind(mod, g.inviteDialog,     "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    ok &= Bind(mod, g.registerCallback, "SteamAPI_RegisterCallback");
    ok &= Bind(mod, g.relayStatus,      "SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus");
    ok &= Bind(mod, g.checkPing,        "SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate");
    if (!ok)
    {
        LOG_WARN("[SteamAPI] exports missing -- Steam hosting/joining unavailable");
        return false;
    }

    g.sockets = SafeAccessor(aSockets);
    g.utils   = SafeAccessor(aUtils);
    g.mm      = SafeAccessor(aMM);
    g.friends = SafeAccessor(aFriends);
    g.user    = SafeAccessor(aUser);
    g.sutils  = SafeAccessor(aSUtils);
    if (!g.sockets || !g.utils || !g.mm || !g.friends || !g.user || !g.sutils)
    {
        LOG_WARN("[SteamAPI] interfaces not ready yet (sockets=%p utils=%p mm=%p friends=%p user=%p sutils=%p) -- will retry",
            g.sockets, g.utils, g.mm, g.friends, g.user, g.sutils);
        return false;
    }

    g_available.store(true);
    LOG_INFO("[SteamAPI] bound: steamID=%llu appID=%u", (unsigned long long)MySteamID(), AppID());
    return true;
}

bool CoSyncSteamAPI::IsAvailable() { return g_available.load(); }

ISteamNetworkingSockets* CoSyncSteamAPI::Sockets()
{
    return g_available.load() ? static_cast<ISteamNetworkingSockets*>(g.sockets) : nullptr;
}

ISteamNetworkingUtils* CoSyncSteamAPI::Utils()
{
    return g_available.load() ? static_cast<ISteamNetworkingUtils*>(g.utils) : nullptr;
}

uint64_t CoSyncSteamAPI::MySteamID()
{
    return (g.getSteamID && g.user) ? g.getSteamID(g.user) : 0;
}

uint32_t CoSyncSteamAPI::AppID()
{
    return g.getAppID && g.sutils ? g.getAppID(g.sutils) : 0;
}

void CoSyncSteamAPI::InitRelay()
{
    // ISteamNetworkingUtils::InitRelayNetworkAccess() is an inline wrapper for
    // CheckPingDataUpToDate(1e10f) in both the SDK and GNS headers.
    if (g_available.load() && g.checkPing)
        g.checkPing(g.utils, 1e10f);
}

int CoSyncSteamAPI::RelayAvailability()
{
    if (!g_available.load() || !g.relayStatus)
        return 0;
    SteamRelayNetworkStatus_t st{};
    return g.relayStatus(g.utils, &st);
}

bool CoSyncSteamAPI::PollCallResult(uint64_t call, void* out, int outSize, int callbackId, bool& failed)
{
    failed = false;
    if (!g_available.load() || call == 0)
        return false;
    bool ioFailed = false;
    if (!g.isCallCompleted(g.sutils, call, &ioFailed))
        return false;
    bool resFailed = false;
    const bool got = g.getCallResult(g.sutils, call, out, outSize, callbackId, &resFailed);
    failed = ioFailed || resFailed || !got;
    return true;
}

uint64_t CoSyncSteamAPI::CreateLobby(LobbyType type, int maxMembers)
{
    return g_available.load() ? g.createLobby(g.mm, static_cast<int>(type), maxMembers) : 0;
}

uint64_t CoSyncSteamAPI::JoinLobby(uint64_t lobby)
{
    return g_available.load() ? g.joinLobby(g.mm, lobby) : 0;
}

void CoSyncSteamAPI::LeaveLobby(uint64_t lobby)
{
    if (g_available.load() && lobby)
        g.leaveLobby(g.mm, lobby);
}

bool CoSyncSteamAPI::SetLobbyData(uint64_t lobby, const char* key, const char* value)
{
    return g_available.load() && lobby && g.setLobbyData(g.mm, lobby, key, value ? value : "");
}

std::string CoSyncSteamAPI::GetLobbyData(uint64_t lobby, const char* key)
{
    if (!g_available.load() || !lobby)
        return std::string();
    const char* v = SafeLobbyData(lobby, key);
    return v ? std::string(v) : std::string();
}

bool CoSyncSteamAPI::RequestLobbyData(uint64_t lobby)
{
    return g_available.load() && lobby && g.requestLobbyData(g.mm, lobby);
}

uint64_t CoSyncSteamAPI::GetLobbyOwner(uint64_t lobby)
{
    return g_available.load() ? SafeU64Lobby(g.lobbyOwner, g.mm, lobby) : 0;
}

int CoSyncSteamAPI::GetNumLobbyMembers(uint64_t lobby)
{
    return g_available.load() && lobby ? g.numMembers(g.mm, lobby) : 0;
}

bool CoSyncSteamAPI::SetLobbyJoinable(uint64_t lobby, bool joinable)
{
    return g_available.load() && lobby && g.setJoinable(g.mm, lobby, joinable);
}

std::vector<CoSyncSteamAPI::FriendGame> CoSyncSteamAPI::FriendsInGame()
{
    std::vector<FriendGame> out;
    if (!g_available.load())
        return out;
    constexpr int kFriendFlagImmediate = 0x04;
    const int n = g.friendCount(g.friends, kFriendFlagImmediate);
    for (int i = 0; i < n && i < 1000; ++i)
    {
        const uint64_t id = g.friendByIndex(g.friends, i, kFriendFlagImmediate);
        // FriendGameInfo_t { CGameID gameID; uint32 ip; uint16 port; uint16 qport; CSteamID lobby; }
        struct { uint64_t gameID; uint32_t ip; uint16_t port; uint16_t qport; uint64_t lobby; } info{};
        static_assert(sizeof(info) == 24, "FriendGameInfo_t layout");
        if (!SafeFriendGame(id, &info))
            continue;
        FriendGame fg;
        fg.friendID = id;
        fg.appID    = static_cast<uint32_t>(info.gameID & 0xFFFFFFu);   // CGameID: app ID in the low 24 bits
        fg.lobby    = info.lobby;
        const char* nm = SafeFriendName(id);
        fg.name = nm ? nm : "";
        out.push_back(std::move(fg));
    }
    return out;
}

bool CoSyncSteamAPI::SetRichPresence(const char* key, const char* value)
{
    return g_available.load() && g.setRichPresence(g.friends, key, value);
}

void CoSyncSteamAPI::ClearRichPresence()
{
    if (g_available.load())
        g.clearRichPresence(g.friends);
}

void CoSyncSteamAPI::OpenInviteDialog(uint64_t lobby)
{
    if (g_available.load() && lobby)
        g.inviteDialog(g.friends, lobby);
}

bool CoSyncSteamAPI::RegisterConnectionStatus(ConnStatusFn fn)
{
    if (!g_available.load())
        return false;
    g_connStatusFn = fn;
    static bool s_registered = false;
    if (!s_registered)
        s_registered = SafeRegister(&g_connStatusCB, g_connStatusCB.GetICallback());
    return s_registered;
}

bool CoSyncSteamAPI::RegisterLobbyJoinRequested(LobbyInviteFn fn)
{
    if (!g_available.load())
        return false;
    g_lobbyInviteFn = fn;
    static bool s_registered = false;
    if (!s_registered)
        s_registered = SafeRegister(&g_lobbyJoinCB, g_lobbyJoinCB.GetICallback());
    return s_registered;
}
