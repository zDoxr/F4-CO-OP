#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Steam lobbies for internet play: how friends find and join a Steam-hosted
// CoSync game (the connection itself is GNS_Session's Steam backend).
//
//  Host : after StartHosting("steam") the host opens a FRIENDS-ONLY lobby
//         tagged cosync=1, build=<network build>, host=<SteamID64>,
//         name=<game name>, players=<n>, and sets Steam rich presence.
//  Find : friends whose Steam status shows Fallout 4 in a lobby are checked
//         for the cosync tag (RequestLobbyData) and listed.
//  Join : JoinLobby -> LobbyEnter -> build check -> StartJoining("steam:<host>").
//  Invite: the host opens Steam's invite dialog; an accepted invite (or
//         "Join Game" in the friends list) arrives as GameLobbyJoinRequested.
//
// All Steam work happens in Tick() (CoSync's tick); the request functions may
// be called from the UI thread -- state is mutex-guarded.
namespace CoSyncSteamLobby
{
    struct Game
    {
        uint64_t    lobby = 0;
        std::string name;
        std::string host;        // host's Steam persona (friend list name)
        uint32_t    build = 0;
        int         players = 0;
        bool        compatible = false;
    };

    bool IsAvailable();          // Steam API bound

    void RequestHost();          // open the lobby once the Steam host is listening
    void RequestJoin(uint64_t lobby);
    void Leave();                // session ended: leave the lobby, clear presence
    void OpenInvite();           // Steam overlay invite dialog for our lobby

    void SetBrowsing(bool on);   // Join Game screen open
    std::vector<Game> FriendsGames();

    bool        IsHostingLobby();
    std::string LastError();     // "", "version_mismatch", "lobby_failed", "join_failed", "steam_unavailable"

    void Tick(double now);
}
