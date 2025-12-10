#pragma once
#include <cstdint>
#include <string>

// Forward minimal interfaces so other files can hold pointers if needed
struct ISteamUser;
struct ISteamFriends;
struct ISteamUtils;

// ============================================================================
// Public API for Steam integration
// ============================================================================
bool      CoSyncSteam_Init();
void      CoSyncSteam_Shutdown();

// True once we've resolved ISteamUser / ISteamFriends at least once
bool      CoSyncSteam_IsReady();


// Former name: HasFriends() — keep for compatibility with overlay
inline bool CoSyncSteam_HasFriends() { return CoSyncSteam_IsReady(); }



// Identity
const char* CoSyncSteam_GetPersonaName();   // nullptr if not known
uint64_t    CoSyncSteam_GetSelfSteamID64(); // 0 if not known

// Friends (immediate friends list)
int         CoSyncSteam_GetFriendCount();   // -1 if unavailable

// Get friend info by index (0..friendCount-1)
// Returns false if index out of range or SteamFriends not ready
bool        CoSyncSteam_GetFriendInfo(
    int index,
    uint64_t& outSteamID,
    std::string& outName
);

// Invite a friend to our game using Steam’s InviteUserToGame.
// connectString is typically something like "connect 1.2.3.4:27020"
// Returns false if function not exported or SteamFriends not ready.
bool        CoSyncSteam_InviteFriendToGame(
    uint64_t friendSteamID,
    const char* connectString
);

// Low-level access (optional, but handy for debugging/overlay)
ISteamUser* CoSyncSteam_GetUser();
ISteamFriends* CoSyncSteam_GetFriends();
ISteamUtils* CoSyncSteam_GetUtils();
