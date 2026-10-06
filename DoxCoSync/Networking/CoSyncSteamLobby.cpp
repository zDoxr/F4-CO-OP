#include "CoSyncSteamLobby.h"

#include "CoSyncSteamAPI.h"
#include "CoSyncNet.h"
#include "CoSyncSessionConfig.h"
#include "CoSyncTransport.h"
#include "GNS_Session.h"
#include "CoSync_Main.h"
#include "ConsoleLogger.h"

#include <mutex>
#include <unordered_map>

namespace
{
    std::mutex s_mutex;

    // host side
    bool     s_wantHost = false;
    uint64_t s_createCall = 0;
    uint64_t s_lobby = 0;          // lobby we own or joined
    bool     s_ownLobby = false;
    int      s_lastPlayers = -1;
    double   s_nextPlayersUpdate = 0.0;

    // join side
    uint64_t s_joinLobbyWanted = 0;
    uint64_t s_joinCall = 0;
    uint64_t s_joiningLobby = 0;

    // browse
    bool   s_browsing = false;
    double s_nextScan = 0.0;
    std::unordered_map<uint64_t, double> s_requestedAt;   // lobby -> last RequestLobbyData
    std::vector<CoSyncSteamLobby::Game> s_games;

    std::string s_error;
    bool s_inviteHandlerSet = false;

    // Steam delivers an accepted invite / friends-list "Join Game" here (from
    // the thread that runs SteamAPI_RunCallbacks).
    void OnLobbyJoinRequested(uint64_t lobby, uint64_t friendID)
    {
        LOG_INFO("[SteamLobby] Steam join request: lobby=%llu from friend %llu",
            (unsigned long long)lobby, (unsigned long long)friendID);
        CoSyncSteamLobby::RequestJoin(lobby);
    }

    bool EnsureSteam()
    {
        if (!CoSyncSteamAPI::Init())
            return false;
        if (!s_inviteHandlerSet)
            s_inviteHandlerSet = CoSyncSteamAPI::RegisterLobbyJoinRequested(&OnLobbyJoinRequested);
        return true;
    }

    std::string GameName()
    {
        const std::string me = CoSyncNet::GetMyName();
        return (me.empty() ? std::string("CoSync") : me) + "'s Game";
    }
}

bool CoSyncSteamLobby::IsAvailable()
{
    return CoSyncSteamAPI::Init();
}

void CoSyncSteamLobby::RequestHost()
{
    std::lock_guard<std::mutex> lk(s_mutex);
    s_wantHost = true;
    s_error.clear();
}

void CoSyncSteamLobby::RequestJoin(uint64_t lobby)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    if (lobby == 0)
        return;
    s_joinLobbyWanted = lobby;
    s_error.clear();
}

void CoSyncSteamLobby::Leave()
{
    std::lock_guard<std::mutex> lk(s_mutex);
    if (s_lobby != 0)
    {
        CoSyncSteamAPI::LeaveLobby(s_lobby);
        LOG_INFO("[SteamLobby] Left lobby %llu", (unsigned long long)s_lobby);
    }
    if (CoSyncSteamAPI::IsAvailable())
        CoSyncSteamAPI::ClearRichPresence();
    s_wantHost = false;
    s_createCall = 0;
    s_lobby = 0;
    s_ownLobby = false;
    s_lastPlayers = -1;
    s_joinLobbyWanted = 0;
    s_joinCall = 0;
    s_joiningLobby = 0;
}

void CoSyncSteamLobby::OpenInvite()
{
    std::lock_guard<std::mutex> lk(s_mutex);
    if (s_lobby != 0 && s_ownLobby)
        CoSyncSteamAPI::OpenInviteDialog(s_lobby);
}

void CoSyncSteamLobby::SetBrowsing(bool on)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    s_browsing = on;
    if (on)
        s_nextScan = 0.0;
}

std::vector<CoSyncSteamLobby::Game> CoSyncSteamLobby::FriendsGames()
{
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_games;
}

bool CoSyncSteamLobby::IsHostingLobby()
{
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_lobby != 0 && s_ownLobby;
}

std::string CoSyncSteamLobby::LastError()
{
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_error;
}

void CoSyncSteamLobby::Tick(double now)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    const bool anythingToDo = s_wantHost || s_createCall || s_lobby || s_joinLobbyWanted || s_joinCall || s_browsing;
    if (!anythingToDo)
        return;
    if (!EnsureSteam())
    {
        if (s_wantHost || s_joinLobbyWanted)
        {
            s_error = "steam_unavailable";
            s_wantHost = false;
            s_joinLobbyWanted = 0;
        }
        return;
    }

    // ---- host: create the lobby once the Steam host socket is up --------
    GNS_Session& gns = GNS_Session::Get();
    const bool steamHosting = gns.IsSteamBackend() && gns.GetRole() == GNSRole::Host;
    if (s_wantHost && !s_createCall && !s_lobby && steamHosting)
    {
        s_createCall = CoSyncSteamAPI::CreateLobby(CoSyncSteamAPI::kLobbyFriendsOnly, CoSyncSessionConfig::kMaxPlayers);
        LOG_INFO("[SteamLobby] Creating a friends-only lobby (call %llu)", (unsigned long long)s_createCall);
        if (!s_createCall) { s_error = "lobby_failed"; s_wantHost = false; }
    }
    if (s_createCall)
    {
        CoSyncSteamAPI::LobbyCreated res{};
        bool failed = false;
        if (CoSyncSteamAPI::PollCallResult(s_createCall, &res, sizeof(res), CoSyncSteamAPI::kCallback_LobbyCreated, failed))
        {
            s_createCall = 0;
            s_wantHost = false;
            if (failed || res.result != CoSyncSteamAPI::kEResultOK || res.lobby == 0)
            {
                LOG_ERROR("[SteamLobby] Lobby creation failed (result=%d ioFailed=%d)", res.result, failed ? 1 : 0);
                s_error = "lobby_failed";
            }
            else
            {
                s_lobby = res.lobby;
                s_ownLobby = true;
                const std::string build = std::to_string(CoSyncNet::GetBuildNumber());
                const std::string host  = std::to_string(CoSyncSteamAPI::MySteamID());
                CoSyncSteamAPI::SetLobbyData(s_lobby, "cosync", "1");
                CoSyncSteamAPI::SetLobbyData(s_lobby, "build", build.c_str());
                CoSyncSteamAPI::SetLobbyData(s_lobby, "host", host.c_str());
                CoSyncSteamAPI::SetLobbyData(s_lobby, "name", GameName().c_str());
                CoSyncSteamAPI::SetLobbyData(s_lobby, "players", "1");
                CoSyncSteamAPI::SetRichPresence("status", "Hosting a CoSync game");
                LOG_INFO("[SteamLobby] Lobby %llu open (host %s, build %s)",
                    (unsigned long long)s_lobby, host.c_str(), build.c_str());
            }
        }
    }
    if (s_lobby && s_ownLobby)
    {
        if (!steamHosting)
        {
            // Hosting ended without Leave() (e.g. a failed start) -- close it.
            CoSyncSteamAPI::LeaveLobby(s_lobby);
            s_lobby = 0;
            s_ownLobby = false;
        }
        else if (now >= s_nextPlayersUpdate)
        {
            s_nextPlayersUpdate = now + 5.0;
            const int players = 1 + static_cast<int>(gns.GetConnectedClientCount());
            if (players != s_lastPlayers)
            {
                s_lastPlayers = players;
                CoSyncSteamAPI::SetLobbyData(s_lobby, "players", std::to_string(players).c_str());
                CoSyncSteamAPI::SetLobbyJoinable(s_lobby, players < CoSyncSessionConfig::kMaxPlayers);
            }
        }
    }

    // ---- join: enter the lobby, check the build, connect to its host -----
    if (s_joinLobbyWanted && !s_joinCall)
    {
        if (CoSyncTransport::IsInitialized())
        {
            LOG_WARN("[SteamLobby] Join ignored: already in a session");
            s_joinLobbyWanted = 0;
        }
        else
        {
            s_joiningLobby = s_joinLobbyWanted;
            s_joinLobbyWanted = 0;
            s_joinCall = CoSyncSteamAPI::JoinLobby(s_joiningLobby);
            LOG_INFO("[SteamLobby] Joining lobby %llu (call %llu)",
                (unsigned long long)s_joiningLobby, (unsigned long long)s_joinCall);
            if (!s_joinCall) { s_error = "join_failed"; s_joiningLobby = 0; }
        }
    }
    if (s_joinCall)
    {
        CoSyncSteamAPI::LobbyEnter res{};
        bool failed = false;
        if (CoSyncSteamAPI::PollCallResult(s_joinCall, &res, sizeof(res), CoSyncSteamAPI::kCallback_LobbyEnter, failed))
        {
            s_joinCall = 0;
            const uint64_t lobby = s_joiningLobby;
            s_joiningLobby = 0;
            if (failed || res.response != CoSyncSteamAPI::kChatRoomEnterSuccess)
            {
                LOG_ERROR("[SteamLobby] Could not enter lobby %llu (response=%u ioFailed=%d)",
                    (unsigned long long)lobby, res.response, failed ? 1 : 0);
                s_error = "join_failed";
            }
            else
            {
                uint64_t host = 0;
                try { host = std::stoull(CoSyncSteamAPI::GetLobbyData(lobby, "host")); } catch (...) { host = 0; }
                if (host == 0)
                    host = CoSyncSteamAPI::GetLobbyOwner(lobby);
                uint32_t build = 0;
                try { build = static_cast<uint32_t>(std::stoul(CoSyncSteamAPI::GetLobbyData(lobby, "build"))); } catch (...) { build = 0; }

                if (build != CoSyncNet::GetBuildNumber())
                {
                    LOG_WARN("[SteamLobby] Lobby %llu runs build %u, ours is %u -- not joining",
                        (unsigned long long)lobby, build, CoSyncNet::GetBuildNumber());
                    CoSyncSteamAPI::LeaveLobby(lobby);
                    s_error = "version_mismatch";
                }
                else if (host == 0 || host == CoSyncSteamAPI::MySteamID())
                {
                    LOG_ERROR("[SteamLobby] Lobby %llu has no usable host ID", (unsigned long long)lobby);
                    CoSyncSteamAPI::LeaveLobby(lobby);
                    s_error = "join_failed";
                }
                else
                {
                    s_lobby = lobby;
                    s_ownLobby = false;
                    const std::string target = "steam:" + std::to_string(host);
                    LOG_INFO("[SteamLobby] In lobby %llu -- connecting to host %llu over Steam",
                        (unsigned long long)lobby, (unsigned long long)host);
                    cosync::CoSync_Main::Get().ResetNetState();
                    cosync::CoSync_Main::Get().StartJoining(target);
                }
            }
        }
    }

    // ---- browse: friends in a Fallout 4 lobby tagged cosync --------------
    if (s_browsing && now >= s_nextScan)
    {
        s_nextScan = now + 3.0;
        const uint32_t app = CoSyncSteamAPI::AppID();
        const uint32_t myBuild = CoSyncNet::GetBuildNumber();
        std::vector<CoSyncSteamLobby::Game> games;
        for (const auto& f : CoSyncSteamAPI::FriendsInGame())
        {
            if (f.lobby == 0 || f.appID != app)
                continue;
            auto rq = s_requestedAt.find(f.lobby);
            if (rq == s_requestedAt.end() || now - rq->second > 10.0)
            {
                CoSyncSteamAPI::RequestLobbyData(f.lobby);   // data arrives async; read next scan
                s_requestedAt[f.lobby] = now;
            }
            if (CoSyncSteamAPI::GetLobbyData(f.lobby, "cosync") != "1")
                continue;
            CoSyncSteamLobby::Game g;
            g.lobby = f.lobby;
            g.name  = CoSyncSteamAPI::GetLobbyData(f.lobby, "name");
            g.host  = f.name;
            try { g.build = static_cast<uint32_t>(std::stoul(CoSyncSteamAPI::GetLobbyData(f.lobby, "build"))); } catch (...) { g.build = 0; }
            try { g.players = std::stoi(CoSyncSteamAPI::GetLobbyData(f.lobby, "players")); } catch (...) { g.players = 0; }
            g.compatible = (g.build == myBuild);
            if (g.name.empty())
                g.name = f.name + "'s Game";
            games.push_back(std::move(g));
        }
        if (games.size() != s_games.size())
            LOG_INFO("[SteamLobby] %zu friend game(s) found", games.size());
        s_games = std::move(games);
    }
}
