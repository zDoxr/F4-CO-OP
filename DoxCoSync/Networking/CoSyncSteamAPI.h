#pragma once

#include <cstdint>
#include <string>
#include <vector>

class ISteamNetworkingSockets;
class ISteamNetworkingUtils;
struct SteamNetConnectionStatusChangedCallback_t;

// Steam lobbies, friends and Steam's own networking sockets (SDR relay + P2P),
// bound at runtime from the steam_api64.dll the game already loaded -- the same
// approach as CoSyncSteamID (no new link inputs; everything degrades to
// "unavailable" if Steam or an export is missing).
//
// Proven 2026-09-27 against the shipping DLL (build 7405127):
//  * all 47 ISteamNetworkingSockets v012 and 25 ISteamNetworkingUtils v004
//    vtable slots match the vendored GNS headers (flat wrappers disassembled),
//    so Steam's interface pointers can be used through the same C++ classes
//    GNS_Session already uses;
//  * lobbies and the SDR relay work under Fallout 4's app ID 377160 (probes).
// Not possible (probe): a second process on the SAME Steam account cannot
// ConnectP2P to its own identity -- two accounts (or gbe_fork) are needed.
namespace CoSyncSteamAPI
{
    // Resolves every export. Idempotent; false until Steam is up.
    bool Init();
    bool IsAvailable();

    // Steam's networking interfaces (valid only when IsAvailable()).
    ISteamNetworkingSockets* Sockets();
    ISteamNetworkingUtils*   Utils();

    uint64_t    MySteamID();
    uint32_t    AppID();

    // SDR relay. InitRelay starts the ping measurement; availability is
    // ESteamNetworkingAvailability (100 = Current).
    void InitRelay();
    int  RelayAvailability();

    // ---- async call results (polled; no CCallResult needed) -------------
    // True once the call finished; out receives the result struct. failed is
    // set on an IO failure.
    bool PollCallResult(uint64_t call, void* out, int outSize, int callbackId, bool& failed);

    // ---- matchmaking ----------------------------------------------------
    enum LobbyType { kLobbyPrivate = 0, kLobbyFriendsOnly = 1, kLobbyPublic = 2 };
    uint64_t    CreateLobby(LobbyType type, int maxMembers);   // SteamAPICall_t (0 = failed)
    uint64_t    JoinLobby(uint64_t lobby);                     // SteamAPICall_t
    void        LeaveLobby(uint64_t lobby);
    bool        SetLobbyData(uint64_t lobby, const char* key, const char* value);
    std::string GetLobbyData(uint64_t lobby, const char* key);
    bool        RequestLobbyData(uint64_t lobby);
    uint64_t    GetLobbyOwner(uint64_t lobby);
    int         GetNumLobbyMembers(uint64_t lobby);
    bool        SetLobbyJoinable(uint64_t lobby, bool joinable);

    // ---- friends --------------------------------------------------------
    struct FriendGame
    {
        uint64_t    friendID = 0;
        std::string name;
        uint32_t    appID    = 0;
        uint64_t    lobby    = 0;
    };
    // Immediate friends currently in a game (any game; filter appID/lobby).
    std::vector<FriendGame> FriendsInGame();
    bool SetRichPresence(const char* key, const char* value);
    void ClearRichPresence();
    void OpenInviteDialog(uint64_t lobby);

    // ---- callbacks (Steam's CCallback mechanism) -------------------------
    // Delivered when the game calls SteamAPI_RunCallbacks. Valve's DLL also
    // supports the function-pointer path GNS_Session registers; gbe_fork
    // delivers connection changes ONLY through this mechanism.
    using ConnStatusFn   = void(*)(SteamNetConnectionStatusChangedCallback_t*);
    using LobbyInviteFn  = void(*)(uint64_t lobby, uint64_t friendID);
    bool RegisterConnectionStatus(ConnStatusFn fn);
    bool RegisterLobbyJoinRequested(LobbyInviteFn fn);

    // Callback IDs / result layouts used by callers (Steamworks SDK values).
    constexpr int kCallback_LobbyEnter            = 504;
    constexpr int kCallback_LobbyCreated          = 513;
    constexpr int kCallback_GameLobbyJoinRequested = 333;
    constexpr int kEResultOK                      = 1;
    constexpr uint32_t kChatRoomEnterSuccess      = 1;

#pragma pack(push, 8)
    struct LobbyCreated { int32_t result; uint64_t lobby; };
    struct LobbyEnter   { uint64_t lobby; uint32_t chatPermissions; bool locked; uint32_t response; };
#pragma pack(pop)
    static_assert(sizeof(LobbyCreated) == 16, "LobbyCreated_t layout");
    static_assert(sizeof(LobbyEnter) == 24, "LobbyEnter_t layout");
}
