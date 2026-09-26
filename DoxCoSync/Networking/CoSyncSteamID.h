#pragma once

#include <cstdint>
#include <string>

// Real Steam identity for CoSync.
//
// Background: CoSyncNet::GetMySteamID() historically returned a FAKE id --
// HashName(s_myName) ^ (pid * 2654435761) -- and s_myName defaulted to the
// literal "Player". Nothing in the tree ever touched the Steam API (the old
// CoSyncSteam.cpp was dropped in the folder reorg). The overlay name tag and
// the map-marker labels both want a real persona, so this module supplies one.
//
// Implementation note: the repo ships Lib/steam_api64.lib and the GNS *networking*
// headers, but NOT isteamfriends.h / isteamuser.h, so the C++ interface classes
// are unavailable. Rather than hand-write fake SDK headers, this binds the flat
// C API by name out of the steam_api64.dll the game itself has already loaded
// (verified present in the 2023 shipping DLL, 1196 exports):
//     SteamAPI_SteamFriends_v017        -> ISteamFriends*
//     SteamAPI_SteamUser_v023..v020     -> ISteamUser*   (newest first)
//     SteamAPI_ISteamFriends_GetPersonaName(ISteamFriends*) -> const char*
//     SteamAPI_ISteamUser_GetSteamID(ISteamUser*)          -> uint64
// That means no new link inputs, no new headers, and if Steam is absent or the
// export set changes, every call degrades to "unavailable" instead of crashing.
namespace CoSyncSteamID
{
    // Resolves steam_api64.dll + the flat exports. Idempotent; safe to call
    // before the world (or Steam) is ready -- it simply reports unavailable and
    // can be retried later. Returns true once the interfaces resolved.
    bool Init();

    // True once Init() has bound both interfaces.
    bool IsAvailable();

    // Real 64-bit SteamID, or 0 when unavailable. Cached after first success.
    uint64_t GetSteamID64();

    // Steam persona ("display name"), or an empty string when unavailable.
    // Cached after first success -- GetPersonaName returns an engine-owned
    // pointer that we must not hold on to.
    const std::string& GetPersonaName();
}
