#include "CoSyncPrismaUI.h"

#include "PrismaUI_F4_API.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "Relocation.h"

#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSync_Main.h"
#include "CoSyncWorld.h"
#include "CoSyncOverlay.h"
#include "GNS_Session.h"
#include "CoSyncDiscovery.h"
#include "CoSyncSessionConfig.h"
#include "CoSyncPlayerAnimation.h"
#include "CoSyncWorldTransfer.h"
#include "CoSyncProfiles.h"
#include "CoSyncFactions.h"
#include "CoSyncWorkshopAuthority.h"
#include "CoSyncChat.h"
#include "CoSyncVoice.h"
#include "CoSyncNameplates.h"

#include <ShlObj.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <mutex>
#include "CoSyncThreadDiag.h"
#include "CoSyncSteamLobby.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace
{
    using PrismaAPI = PRISMA_UI_API::IVPrismaUI12;

    // Path is relative to Data\PrismaUI_F4\views\.
    constexpr const char* kMultiplayerViewPath = "DoxCoSync/multiplayer.html";

    // PlayUISound(const char* soundDescriptorEditorID). RVA from f4se 0.7.9
    // (ScaleformValue.cpp), whose CURRENT_RELEASE_RUNTIME is 1.11.240 -- the
    // runtime this plugin ships for. Verified 2026-09-25 against Fallout4.exe
    // 1.11.240: preceded by int3 padding, opens with a normal one-argument
    // prologue. NOT the vendored ThirdParty/Pap copy (0x01086220), which lands
    // mid-function on 1.11.240.
    using PlayUISound_t = void (*)(const char*);
    RelocAddr<PlayUISound_t> s_PlayUISound(0x01086570);

    PrismaAPI*        s_api = nullptr;
    PrismaView        s_view = 0;
    std::atomic<bool> s_domReady{ false };

    // Game thread only: the player wants the menu open. PrismaUI ignores
    // Focus() until the page is DOM-ready, so an early focus request is
    // re-issued from the DOM-ready handler.
    bool s_wantOpen = false;
    bool s_openedFromMainMenu = false;
    bool s_padBound = false;

    // Game thread only: what the player chose to join (game name or address),
    // shown while connecting / on failure. Cleared when the session ends.
    std::string s_joinTarget;

    // Readable from any thread (menu hooks, WndProc).
    std::atomic<bool> s_openForInput{ false };

    // Teammate HUD widget (game thread only, except the DOM-ready flag).
    constexpr const char* kHudViewPath = "DoxCoSync/hud.html";
    PrismaView        s_hud = 0;
    std::atomic<bool> s_hudDomReady{ false };
    bool              s_hudShown = false;
    std::string       s_hudLastJson;

    // ── Helpers ─────────────────────────────────────────────────────────────

    std::string JsonEscape(const char* s)
    {
        LOG_REQUIREMENT("(anonymous)::JsonEscape", "Doesn't touch the game. Safe with a null pointer (treated as an empty string).");
        std::string out;
        if (!s)
            return out;
        for (; *s; ++s)
        {
            const unsigned char c = static_cast<unsigned char>(*s);
            switch (c)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out += static_cast<char>(c);
                }
            }
        }
        return out;
    }

    // The vanilla HUD colour the player picked in Settings > Display, as
    // saved in Documents\My Games\Fallout4\Fallout4Prefs.ini [Interface].
    // Falls back to the game default (18,255,21) if anything is missing.
    void ReadHudColor(int& r, int& g, int& b)
    {
        LOG_REQUIREMENT("(anonymous)::ReadHudColor", "Doesn't touch the game. Reads the player's prefs INI from disk; always produces a valid colour.");
        r = 18; g = 255; b = 21;

        PWSTR docs = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) || !docs)
        {
            if (docs) CoTaskMemFree(docs);
            return;
        }
        std::wstring path = std::wstring(docs) + L"\\My Games\\Fallout4\\Fallout4Prefs.ini";
        CoTaskMemFree(docs);

        auto clamp = [](UINT v) { return static_cast<int>(v > 255 ? 255 : v); };
        r = clamp(GetPrivateProfileIntW(L"Interface", L"iHUDColorR", r, path.c_str()));
        g = clamp(GetPrivateProfileIntW(L"Interface", L"iHUDColorG", g, path.c_str()));
        b = clamp(GetPrivateProfileIntW(L"Interface", L"iHUDColorB", b, path.c_str()));
    }

    // Real multiplayer state, from the existing CoSync layers. No UI-owned
    // flags: every value here is read from the networking code itself.
    const char* NetStatus()
    {
        LOG_REQUIREMENT("(anonymous)::NetStatus", "Game thread. Only reads CoSync's own networking state.");
        if (!CoSyncTransport::IsInitialized())
        {
            switch (cosync::CoSync_Main::Get().GetNetState())
            {
            case cosync::CoSyncNetState::Busy:   return "STARTING";
            case cosync::CoSyncNetState::Failed: return "FAILED";
            default:                             return "OFFLINE";
            }
        }
        if (CoSyncTransport::IsHost())
            return CoSyncNet::IsConnected() ? "HOSTING" : "WAITING FOR PLAYERS";

        // Client: GNS's own view of the connection, including why it ended.
        const GNS_Session& gns = GNS_Session::Get();
        switch (gns.GetClientState())
        {
        case GNSClientState::Connected:
            return CoSyncNet::IsConnected() ? "CONNECTED" : "CONNECTING";
        case GNSClientState::Closed:
            if (gns.GetClientEndReason() == CoSyncSessionConfig::kEndReason_SessionFull)
                return "SESSION FULL";
            if (gns.GetClientEndReason() == CoSyncSessionConfig::kEndReason_VersionMismatch)
                return "VERSION MISMATCH";
            return gns.ClientWasEverConnected() ? "HOST DISCONNECTED" : "CONNECTION FAILED";
        default:
            return "CONNECTING";
        }
    }

    const char* NetRole()
    {
        LOG_REQUIREMENT("(anonymous)::NetRole", "Game thread. Only reads CoSync's own networking state.");
        if (!CoSyncTransport::IsInitialized())
            return "NONE";
        return CoSyncTransport::IsHost() ? "HOST" : "CLIENT";
    }

    // Custom faction banner images the player dropped into
    // Data\PrismaUI_F4\views\DoxCoSync\banners\custom (file names only;
    // the page loads them by relative path). Re-scanned every 5 s. Files that
    // cannot be used are listed with the reason, so the Banner screen can say
    // why an image is missing instead of silently skipping it.
    struct BannerReject { std::string file; std::string why; };
    std::vector<std::string>  s_bannerList;
    std::vector<BannerReject> s_bannerRejects;

    // What the first bytes say the file is: "png", "jpg", "webp", "svg", ""
    // (none of them) or "?" (could not open).
    std::string SniffImage(const std::string& path)
    {
        HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return "?";
        unsigned char b[4096] = {};
        DWORD got = 0;
        ReadFile(f, b, sizeof(b), &got, nullptr);
        CloseHandle(f);
        static const unsigned char kPng[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        if (got >= 8 && std::memcmp(b, kPng, 8) == 0) return "png";
        if (got >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "jpg";
        if (got >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WEBP", 4) == 0) return "webp";
        std::string head(reinterpret_cast<const char*>(b), got);
        for (char& c : head) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (head.find("<svg") != std::string::npos) return "svg";
        return "";
    }

    // FNV-1a of the whole file, cached by name + size + write time.
    uint64_t HashFile(const std::string& path, const std::string& cacheKey)
    {
        static std::unordered_map<std::string, uint64_t> s_cache;
        auto it = s_cache.find(cacheKey);
        if (it != s_cache.end())
            return it->second;
        uint64_t h = 1469598103934665603ull;
        HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            static unsigned char buf[65536];
            DWORD got = 0;
            while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0)
                for (DWORD i = 0; i < got; ++i) { h ^= buf[i]; h *= 1099511628211ull; }
            CloseHandle(f);
        }
        if (s_cache.size() > 512) s_cache.clear();
        s_cache[cacheKey] = h;
        return h;
    }

    void ScanCustomBanners()
    {
        static uint64_t s_nextScan = 0;
        const uint64_t now = GetTickCount64();
        if (now < s_nextScan)
            return;
        s_nextScan = now + 5000;
        s_bannerList.clear();
        s_bannerRejects.clear();

        char exe[MAX_PATH] = {};
        if (!GetModuleFileNameA(nullptr, exe, MAX_PATH))
            return;
        std::string dir(exe);
        dir = dir.substr(0, dir.find_last_of("\\/") + 1) + "Data\\PrismaUI_F4\\views\\DoxCoSync\\banners\\custom\\";

        struct Found { std::string name; uint64_t size; FILETIME mtime; };
        std::vector<Found> files;
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            return;
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                files.push_back({ fd.cFileName, (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow, fd.ftLastWriteTime });
        } while (FindNextFileA(h, &fd) && files.size() < 256);
        FindClose(h);
        std::sort(files.begin(), files.end(), [](const Found& a, const Found& b) { return a.name < b.name; });

        std::unordered_map<uint64_t, std::string> seen;   // content hash -> first file with it
        for (const Found& f : files)
        {
            const std::string& name = f.name;
            std::string lower = name;
            for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            const size_t dot = lower.find_last_of('.');
            const std::string ext = dot == std::string::npos ? "" : lower.substr(dot + 1);
            const bool supported = ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp" || ext == "svg";
            if (!supported)
            {
                static const char* const kOtherImages[] = { "gif", "bmp", "tga", "dds", "tif", "tiff", "avif", "heic", "ico", "jfif" };
                for (const char* e : kOtherImages)
                    if (ext == e)
                        s_bannerRejects.push_back({ name, "." + ext + " is not supported. Save it as PNG, JPG, WEBP or SVG." });
                continue;   // README.txt and other non-images: not banners, not errors
            }
            auto reject = [&](const std::string& why) { s_bannerRejects.push_back({ name, why }); };

            bool badChar = false, space = false;
            std::string suggest;
            for (char c : name)
            {
                const bool ok = isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-';
                if (c == ' ') space = true;
                else if (!ok) badChar = true;
                suggest += ok ? c : '_';
            }
            if (space || badChar)
            {
                reject(std::string(space ? "Spaces are" : "Some characters are") + " not allowed in the name. Rename it to " +
                       (suggest.size() <= 48 ? suggest : std::string("a shorter name")) + ".");
                continue;
            }
            if (name.size() > 48)            { reject("The name is longer than 48 characters."); continue; }
            if (f.size == 0)                 { reject("The file is empty."); continue; }
            if (f.size > 8ull * 1024 * 1024) { reject("The file is larger than 8 MB."); continue; }

            const std::string want = ext == "jpeg" ? "jpg" : ext;
            const std::string real = SniffImage(dir + name);
            if (real == "?") { reject("The file could not be opened."); continue; }
            if (real != want)
            {
                if (!real.empty())
                    reject("This is really a " + real + " image. Rename it to end in ." + real + ".");
                else
                    reject("Not a valid " + want + " image (damaged, or the wrong extension).");
                continue;
            }

            const std::string key = name + "|" + std::to_string(f.size) + "|" +
                std::to_string((static_cast<uint64_t>(f.mtime.dwHighDateTime) << 32) | f.mtime.dwLowDateTime);
            const uint64_t hash = HashFile(dir + name, key);
            auto dup = seen.find(hash);
            if (dup != seen.end()) { reject("The same image as " + dup->second + "."); continue; }
            seen.emplace(hash, name);

            if (s_bannerList.size() < 64)
                s_bannerList.push_back(name);
            else
                reject("Too many custom images (64 at most).");
        }
    }

    const std::vector<std::string>& CustomBanners()
    {
        ScanCustomBanners();
        return s_bannerList;
    }

    bool ViewIsLive()
    {
        LOG_REQUIREMENT("(anonymous)::ViewIsLive", "Needs the PrismaUI API. Game thread only.");
        return s_api && s_view != 0 && s_api->IsValid(s_view);
    }

    // C++ -> JS: window.cosyncState(json). Game thread only.
    std::string BuildVoiceJson();   // Voice screen state, defined below

    void PushState()
    {
        LOG_REQUIREMENT("(anonymous)::PushState", "Game thread only. Does nothing until the view exists and its DOM is ready.");
        if (!ViewIsLive() || !s_domReady.load())
            return;

        int r, g, b;
        ReadHudColor(r, g, b);

        const bool inGame = CoSyncWorld::IsWorldReady();

        std::string json;
        json.reserve(384);
        json += "{\"context\":\"";
        json += inGame ? "ingame" : "mainmenu";
        json += "\",\"playerName\":\"";
        json += JsonEscape(CoSyncNet::GetMyName());
        json += "\",\"hud\":[" + std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b) + "]";
        json += ",\"gamepad\":";
        json += s_api->IsUsingGamepad() ? "true" : "false";
        json += ",\"controllerStyle\":" + std::to_string(s_api->GetControllerStyle());
        json += ",\"animTesting\":";
        json += CoSyncPlayerAnimation::IsEnabled() ? "true" : "false";
        const bool hosting = CoSyncTransport::IsInitialized() && CoSyncTransport::IsHost();
        const bool inSession = CoSyncTransport::IsInitialized() && CoSyncNet::IsConnected();

        // Roster: only meaningful while a session exists (host or connected
        // client). Built from CoSyncNet/PlayerManager state -- never UI-owned.
        std::vector<CoSyncNet::RosterEntry> roster;
        if (hosting || inSession)
            roster = CoSyncNet::GetRoster();
        const int joining = hosting ? CoSyncNet::GetJoiningCount() : 0;

        json += ",\"net\":{\"status\":\"";
        json += NetStatus();
        json += "\",\"role\":\"";
        json += NetRole();
        json += "\",\"players\":";
        // Host: GNS's authoritative count (includes players still loading).
        // Client: the players actually present in this world.
        json += hosting ? std::to_string(1 + GNS_Session::Get().GetConnectedClientCount())
                        : std::to_string(inSession ? roster.size() : 0);
        json += ",\"port\":" + std::to_string(hosting ? GNS_Session::Get().GetBoundPort() : 0);
        json += ",\"joinTarget\":\"" + JsonEscape(s_joinTarget.c_str()) + "\"}";

        const CoSyncDiscovery::NetworkInfo netInfo = CoSyncDiscovery::GetPrimaryNetwork();
        json += ",\"network\":{\"available\":";
        json += netInfo.available ? "true" : "false";
        json += ",\"type\":\"" + JsonEscape(netInfo.type.c_str()) + "\"";
        json += ",\"adapter\":\"" + JsonEscape(netInfo.adapter.c_str()) + "\"";
        json += ",\"address\":\"" + JsonEscape(netInfo.address.c_str()) + "\"}";

        json += ",\"discovery\":\"";
        json += CoSyncDiscovery::IsAdvertising() ? "ADVERTISING" : (CoSyncDiscovery::IsSearching() ? "SEARCHING" : "OFF");
        json += "\"";

        // Found games. Every field came off the network: escaped here, and
        // escaped again by the page before it touches the DOM.
        // Steam: availability, our lobby, friends' games (names came off the
        // network -- escaped here and again by the page).
        {
            json += ",\"steam\":{\"available\":";
            json += CoSyncSteamLobby::IsAvailable() ? "true" : "false";
            json += ",\"backend\":";
            json += GNS_Session::Get().IsSteamBackend() && CoSyncTransport::IsInitialized() ? "true" : "false";
            json += ",\"lobby\":";
            json += CoSyncSteamLobby::IsHostingLobby() ? "true" : "false";
            json += ",\"error\":\"" + JsonEscape(CoSyncSteamLobby::LastError().c_str()) + "\"";
            json += ",\"games\":[";
            bool first = true;
            char idHex[32];
            for (const auto& g : CoSyncSteamLobby::FriendsGames())
            {
                if (!first) json += ",";
                first = false;
                std::snprintf(idHex, sizeof(idHex), "%llx", (unsigned long long)g.lobby);
                json += "{\"id\":\"" + std::string(idHex) + "\"";
                json += ",\"name\":\"" + JsonEscape(g.name.c_str()) + "\"";
                json += ",\"host\":\"" + JsonEscape(g.host.c_str()) + "\"";
                json += ",\"players\":" + std::to_string(g.players);
                json += ",\"build\":" + std::to_string(g.build);
                json += ",\"compatible\":";
                json += g.compatible ? "true" : "false";
                json += "}";
            }
            json += "]}";
        }

        const uint32_t myBuild = CoSyncNet::GetBuildNumber();
        json += ",\"search\":{\"seconds\":" + std::to_string(static_cast<int>(CoSyncDiscovery::SecondsSinceSearchStarted()));
        json += ",\"sessions\":[";
        bool first = true;
        for (const CoSyncDiscovery::Session& s : CoSyncDiscovery::GetSessions())
        {
            char id[24];
            std::snprintf(id, sizeof(id), "%016llX", static_cast<unsigned long long>(s.id));
            if (!first) json += ",";
            first = false;
            json += "{\"id\":\""; json += id;
            json += "\",\"name\":\"" + JsonEscape(s.gameName.c_str());
            json += "\",\"host\":\"" + JsonEscape(s.hostName.c_str());
            json += "\",\"players\":" + std::to_string(s.players);
            json += ",\"max\":" + std::to_string(s.maxPlayers);
            json += ",\"compatible\":"; json += (s.build == myBuild) ? "true" : "false";
            json += ",\"build\":" + std::to_string(s.build) + "}";
        }
        json += "]}";

        json += ",\"roster\":[";
        for (size_t i = 0; i < roster.size(); ++i)
        {
            const CoSyncNet::RosterEntry& e = roster[i];
            if (i) json += ",";
            json += "{\"name\":\"" + JsonEscape(e.name.c_str()) + "\"";
            json += ",\"host\":";  json += e.isHost ? "true" : "false";
            json += ",\"you\":";   json += e.isLocal ? "true" : "false";
            json += ",\"inGame\":"; json += e.inGame ? "true" : "false";
            json += ",\"talking\":"; json += CoSyncVoice::IsTalking(e.entityID) ? "true" : "false";
            json += "}";
        }
        json += "],\"joining\":" + std::to_string(joining);

        // Shared host save: where the client is in getting the host's world,
        // and restoring / creating its character.
        {
            const char* phase = "idle";
            switch (CoSyncWorldTransfer::GetClientPhase())
            {
            case CoSyncWorldTransfer::ClientPhase::Requested:   phase = "requested";   break;
            case CoSyncWorldTransfer::ClientPhase::Downloading: phase = "downloading"; break;
            case CoSyncWorldTransfer::ClientPhase::Loading:     phase = "loading";     break;
            case CoSyncWorldTransfer::ClientPhase::Ready:       phase = "ready";       break;
            case CoSyncWorldTransfer::ClientPhase::Failed:      phase = "failed";      break;
            default: break;
            }
            json += ",\"transfer\":{\"phase\":\"";
            json += phase;
            json += "\",\"percent\":" + std::to_string(CoSyncWorldTransfer::GetClientProgressPercent());
            json += ",\"error\":\"" + JsonEscape(CoSyncWorldTransfer::GetClientError().c_str()) + "\"}";
            json += ",\"profile\":\"";
            json += CoSyncProfiles::GetClientStatus();
            json += "\"";
        }

        // Factions, territory and workshop rules (CoSyncFactions /
        // CoSyncWorkshopAuthority -- the host's decisions, never UI-owned).
        json += ",\"factions\":" + CoSyncFactions::BuildUiJson();
        json += ",\"workshop\":" + CoSyncWorkshopAuthority::BuildUiJson();
        json += ",\"customBanners\":[";
        {
            bool firstBanner = true;
            for (const std::string& b : CustomBanners())
            {
                if (!firstBanner) json += ",";
                firstBanner = false;
                json += "\"" + JsonEscape(b.c_str()) + "\"";
            }
        }
        json += "],\"bannerRejects\":[";
        for (size_t i = 0; i < s_bannerRejects.size(); ++i)
        {
            if (i) json += ",";
            json += "{\"file\":\"" + JsonEscape(s_bannerRejects[i].file.c_str()) + "\",\"why\":\"" +
                    JsonEscape(s_bannerRejects[i].why.c_str()) + "\"}";
        }
        json += "]";

        json += ",\"voice\":" + BuildVoiceJson();
        json += ",\"build\":" + std::to_string(myBuild);
        json += ",\"maxPlayers\":" + std::to_string(CoSyncSessionConfig::kMaxPlayers) + "}";

        s_api->InteropCall(s_view, "cosyncState", json.c_str());
    }

    void PlayMenuSound(const char* which)
    {
        LOG_REQUIREMENT("(anonymous)::PlayMenuSound", "Game thread only. Only plays vanilla sound descriptors that exist in Fallout4.esm.");
        // Whitelist: the page's argument is untrusted text and must never be
        // passed straight into an engine lookup.
        const char* id = nullptr;
        if      (std::strcmp(which, "ok") == 0)       id = "UIMenuOK";
        else if (std::strcmp(which, "cancel") == 0)   id = "UIMenuCancel";
        else if (std::strcmp(which, "focus") == 0)    id = "UIMenuFocus";
        else if (std::strcmp(which, "prevnext") == 0) id = "UIMenuPrevNext";
        if (id)
            s_PlayUISound(id);
    }

    void CloseMultiplayer();

    // Controller: route the face buttons and D-pad to the page as named
    // actions ('prisma-controller-action' events) instead of PrismaUI's
    // default mapping (which turns A into a virtual mouse click). Must run
    // once the page is live: called straight after CreateView every bind was
    // rejected (2026-09-25 log: A=0 B=0 Up=0 Down=0). Game thread only.
    void BindPadActions()
    {
        LOG_REQUIREMENT("(anonymous)::BindPadActions", "Game thread only, after the view's DOM is ready.");
        if (s_padBound || !ViewIsLive())
            return;

        const bool a  = s_api->BindControllerAction(s_view, "A", "accept");
        const bool b  = s_api->BindControllerAction(s_view, "B", "back");
        const bool up = s_api->BindControllerAction(s_view, "DUp", "up");
        const bool dn = s_api->BindControllerAction(s_view, "DDown", "down");
        const bool lf = s_api->BindControllerAction(s_view, "DLeft", "left");
        const bool rt = s_api->BindControllerAction(s_view, "DRight", "right");
        s_padBound = a && b && up && dn && lf && rt;

        LOG_INFO("[PrismaUI] Controller actions bound: A=%d B=%d Up=%d Down=%d Left=%d Right=%d bridge=%d",
            a ? 1 : 0, b ? 1 : 0, up ? 1 : 0, dn ? 1 : 0, lf ? 1 : 0, rt ? 1 : 0,
            (int)s_api->GetControllerActionBridgeState(s_view));
    }

    // ── PrismaUI callbacks ──────────────────────────────────────────────────

    void OnDomReadyGameThread(void*)
    {
        LOG_REQUIREMENT("(anonymous)::OnDomReadyGameThread", "Runs on the game thread via PrismaUI's dispatcher, after the view's DOM is ready.");
        PushState();
        BindPadActions();

        if (s_wantOpen && ViewIsLive() && !s_api->HasFocus(s_view))
        {
            const bool focused = s_api->Focus(s_view, false, false);
            LOG_INFO("[PrismaUI] Multiplayer: deferred focus after DOM ready (focus=%d)", focused ? 1 : 0);
        }
    }

    // Thread not documented by PrismaUI -- only flag + marshal from here.
    void OnDomReady(PrismaView view)
    {
        LOG_REQUIREMENT("(anonymous)::OnDomReady", "May run on any thread: only sets a flag and hands the follow-up to the game-thread dispatcher.");
        s_domReady.store(true);
        LOG_INFO("[PrismaUI] Multiplayer view %llu DOM ready", (unsigned long long)view);

        if (s_api && !s_api->DispatchToGameThread(OnDomReadyGameThread, nullptr))
            LOG_WARN("[PrismaUI] DispatchToGameThread refused the DOM-ready follow-up");
    }

    void OnConsoleMessage(PrismaView view, PRISMA_UI_API::ConsoleMessageLevel level, const char* message)
    {
        LOG_REQUIREMENT("(anonymous)::OnConsoleMessage", "May run on any thread: only writes a log line.");
        using L = PRISMA_UI_API::ConsoleMessageLevel;
        const char* msg = message ? message : "";
        switch (level)
        {
        case L::Error:   LOG_ERROR("[PrismaUI:js %llu] %s", (unsigned long long)view, msg); break;
        case L::Warning: LOG_WARN("[PrismaUI:js %llu] %s", (unsigned long long)view, msg);  break;
        default:         LOG_INFO("[PrismaUI:js %llu] %s", (unsigned long long)view, msg);  break;
        }
    }

    // Ends any current session (host or client) through the same path the
    // pause-menu Disconnect uses. Game thread.
    void EndSession()
    {
        LOG_REQUIREMENT("(anonymous)::EndSession", "Game thread only. Safe when no session exists.");
        cosync::CoSync_Main::Get().Shutdown();
        CoSyncOverlay_Disconnect();
        cosync::CoSync_Main::Get().ResetNetState();
        s_joinTarget.clear();
    }

    // Connects to a GNS endpoint (discovered or typed). Game thread.
    void JoinEndpoint(const std::string& ipv4, uint16_t port, const std::string& label)
    {
        LOG_REQUIREMENT("(anonymous)::JoinEndpoint", "Game thread only. ipv4/port must already be validated.");
        CoSyncDiscovery::StopSearching();
        if (CoSyncTransport::IsInitialized())
            EndSession();   // leave a failed / previous attempt cleanly first
        cosync::CoSync_Main::Get().ResetNetState();

        s_joinTarget = label;
        const std::string endpoint = ipv4 + ":" + std::to_string(port);
        LOG_INFO("[PrismaUI] Joining '%s' at %s", label.c_str(), endpoint.c_str());
        cosync::CoSync_Main::Get().StartJoining(endpoint);
    }

    // Strict IPv4 dotted-quad check (the page's text is untrusted).
    bool IsValidIpv4(const std::string& s)
    {
        int parts = 0, value = -1;
        for (size_t i = 0; i <= s.size(); ++i)
        {
            if (i == s.size() || s[i] == '.')
            {
                if (value < 0 || value > 255) return false;
                ++parts;
                value = -1;
            }
            else if (s[i] >= '0' && s[i] <= '9')
            {
                value = (value < 0 ? 0 : value * 10) + (s[i] - '0');
                if (value > 255) return false;
            }
            else
                return false;
        }
        return parts == 4;
    }

    // JS -> C++: window.cosync_cmd("name" or "name|arg"). Bound with
    // BindGameThreadUIEvent, so this runs on the game thread. The argument is
    // untrusted page text: only exact, known commands are acted on.
    // 0.3.3 (Stage 8): the Voice screen's device lists. Enumerated when the
    // screen opens (voice_refresh) and cached, not on every state push.
    std::vector<CoSyncVoice::AudioDevice> s_voiceInputs;
    std::vector<CoSyncVoice::AudioDevice> s_voiceOutputs;
    bool s_voiceDevicesListed = false;

    void RefreshVoiceDevices()
    {
        s_voiceInputs  = CoSyncVoice::ListInputDevices();
        s_voiceOutputs = CoSyncVoice::ListOutputDevices();
        s_voiceDevicesListed = true;
    }

    std::string BuildVoiceJson()
    {
        if (!s_voiceDevicesListed)
            RefreshVoiceDevices();
        const CoSyncVoice::AudioSettings a = CoSyncVoice::GetAudioSettings();
        const CoSyncVoice::Status st = CoSyncVoice::GetStatus();
        auto devs = [](const std::vector<CoSyncVoice::AudioDevice>& list)
        {
            std::string j = "[";
            for (size_t i = 0; i < list.size(); ++i)
            {
                if (i) j += ",";
                j += "{\"id\":\"" + JsonEscape(list[i].id.c_str()) + "\",\"name\":\"" +
                     JsonEscape(list[i].name.c_str()) + "\"}";
            }
            return j + "]";
        };
        char nums[160];
        std::snprintf(nums, sizeof(nums),
            "\"gain\":%.2f,\"sensitivity\":%.2f,\"volume\":%.2f,\"ptt\":%s,\"pttKey\":%d,\"muted\":%s,\"micOk\":%s",
            a.micGain, a.sensitivity, a.volume, a.pushToTalk ? "true" : "false", a.pushToTalkKey,
            a.muted ? "true" : "false", st.micOk ? "true" : "false");
        return std::string("{\"input\":\"") + JsonEscape(a.inputDevice.c_str()) +
               "\",\"output\":\"" + JsonEscape(a.outputDevice.c_str()) +
               "\",\"inputs\":" + devs(s_voiceInputs) + ",\"outputs\":" + devs(s_voiceOutputs) +
               "," + nums + "}";
    }

    void OnCommand(const char* argument, void*)
    {
        LOG_REQUIREMENT("(anonymous)::OnCommand", "Runs on the game thread (BindGameThreadUIEvent). Treats the page's argument as untrusted text.");
        CoSyncThreadDiag::NoteWindowEntry();
        const std::string cmdLine = argument ? argument : "";
        const size_t bar = cmdLine.find('|');
        const std::string cmd = cmdLine.substr(0, bar);
        const std::string arg = (bar == std::string::npos) ? std::string() : cmdLine.substr(bar + 1);

        if (cmd == "sound")
        {
            PlayMenuSound(arg.c_str());
        }
        else if (cmd == "refresh")
        {
            PushState();
        }
        else if (cmd == "close")
        {
            LOG_INFO("[PrismaUI] Multiplayer: close requested by menu");
            CloseMultiplayer();
        }
        else if (cmd == "anim_mode")
        {
            // Developer switch carried over from the retired ImGui overlay's
            // Mod Config tab: Current (production) vs Testing animation sync.
            if (arg == "0" || arg == "1")
            {
                CoSyncPlayerAnimation::SetEnabled(arg == "1");
                LOG_INFO("[PrismaUI] Animation sync mode: %s", arg == "1" ? "Testing" : "Current");
                PushState();
            }
        }
        else if (cmd == "host")
        {
            // Hosting needs this player's world loaded: every player still
            // loads their own save, and clients are spawned at the host's
            // position. The menu disables HOST GAME on the main menu.
            if (!CoSyncWorld::IsWorldReady())
                LOG_WARN("[PrismaUI] Host ignored: no save loaded");
            else if (CoSyncTransport::IsInitialized())
                LOG_WARN("[PrismaUI] Host ignored: already in a session");
            else
            {
                LOG_INFO("[PrismaUI] Host Game");
                CoSyncDiscovery::StopSearching();
                cosync::CoSync_Main::Get().ResetNetState();
                cosync::CoSync_Main::Get().StartHosting("");   // all interfaces
            }
            PushState();
        }
        else if (cmd == "steam_host")
        {
            // Internet play: host on Steam's networking (P2P + Valve relay);
            // friends find the game through its friends-only Steam lobby.
            if (!CoSyncWorld::IsWorldReady())
                LOG_WARN("[PrismaUI] Steam host ignored: no save loaded");
            else if (CoSyncTransport::IsInitialized())
                LOG_WARN("[PrismaUI] Steam host ignored: already in a session");
            else if (!CoSyncSteamLobby::IsAvailable())
                LOG_WARN("[PrismaUI] Steam host ignored: Steam unavailable");
            else
            {
                LOG_INFO("[PrismaUI] Host Game (Steam friends)");
                CoSyncDiscovery::StopSearching();
                cosync::CoSync_Main::Get().ResetNetState();
                cosync::CoSync_Main::Get().StartHosting("steam");
                CoSyncSteamLobby::RequestHost();
            }
            PushState();
        }
        else if (cmd == "steam_invite")
        {
            CoSyncSteamLobby::OpenInvite();
        }
        else if (cmd == "steam_join")
        {
            uint64_t lobby = 0;
            try { lobby = std::stoull(arg, nullptr, 16); } catch (...) { lobby = 0; }
            std::string label;
            for (const auto& g : CoSyncSteamLobby::FriendsGames())
                if (g.lobby == lobby) { label = g.name; break; }
            if (lobby == 0 || label.empty())
                LOG_WARN("[PrismaUI] Steam join: game '%.20s' is no longer listed", arg.c_str());
            else
            {
                CoSyncDiscovery::StopSearching();
                if (CoSyncTransport::IsInitialized())
                    EndSession();
                s_joinTarget = label;
                LOG_INFO("[PrismaUI] Joining '%s' through Steam lobby %llx", label.c_str(), (unsigned long long)lobby);
                CoSyncSteamLobby::RequestJoin(lobby);
            }
            PushState();
        }
        else if (cmd == "steam_browse")
        {
            CoSyncSteamLobby::SetBrowsing(arg == "1");
            PushState();
        }
        else if (cmd == "stop")
        {
            LOG_INFO("[PrismaUI] Stop hosting / leave game");
            EndSession();
            PushState();
        }
        else if (cmd == "search_start")
        {
            CoSyncDiscovery::StartSearching();
            PushState();
        }
        else if (cmd == "search_stop")
        {
            CoSyncDiscovery::StopSearching();
        }
        else if (cmd == "join")
        {
            uint64_t id = 0;
            try { id = std::stoull(arg, nullptr, 16); } catch (...) { id = 0; }
            CoSyncDiscovery::Session s;
            if (id != 0 && CoSyncDiscovery::FindSession(id, s))
                JoinEndpoint(s.address, s.port, s.gameName);
            else
                LOG_WARN("[PrismaUI] Join: session '%.20s' is no longer available", arg.c_str());
            PushState();
        }
        else if (cmd == "direct")
        {
            // arg = "<ipv4>|<port>"
            const size_t sep = arg.find('|');
            const std::string ip = arg.substr(0, sep);
            const std::string portStr = (sep == std::string::npos) ? std::string() : arg.substr(sep + 1);
            int port = 0;
            bool portOk = !portStr.empty() && portStr.size() <= 5 &&
                          portStr.find_first_not_of("0123456789") == std::string::npos;
            if (portOk) { port = std::stoi(portStr); portOk = port >= 1 && port <= 65535; }

            if (IsValidIpv4(ip) && portOk)
                JoinEndpoint(ip, static_cast<uint16_t>(port), ip + ":" + portStr);
            else
                LOG_WARN("[PrismaUI] Direct connect rejected: invalid address '%.40s'", arg.c_str());
            PushState();
        }
        else if (cmd == "voice_refresh")
        {
            RefreshVoiceDevices();
            PushState();
        }
        else if (cmd == "voice")
        {
            // voice|<setting>|<value>. The value is untrusted text from the
            // page; every setter clamps and validates it.
            const size_t b2 = arg.find('|');
            const std::string key = arg.substr(0, b2);
            const std::string val = (b2 == std::string::npos) ? std::string() : arg.substr(b2 + 1);
            const float f = static_cast<float>(std::atof(val.c_str()));
            if (key == "input")            CoSyncVoice::SetInputDevice(val.size() <= 256 ? val : std::string());
            else if (key == "output")      CoSyncVoice::SetOutputDevice(val.size() <= 512 ? val : std::string());
            else if (key == "gain")        CoSyncVoice::SetMicGain(f);
            else if (key == "sensitivity") CoSyncVoice::SetSensitivity(f);
            else if (key == "volume")      CoSyncVoice::SetVolume(f);
            else if (key == "mute")        CoSyncVoice::SetMuted(val == "1");
            PushState();
        }
        else if (cmd.rfind("fac_", 0) == 0)
        {
            // Faction / territory / workshop-settings request. Queued for
            // CoSync's tick; the authority (host or solo) decides and the
            // answer comes back in the next state push.
            CoSyncFactions::QueueUiCommand(cmd, arg);
        }
        else if (cmd == "controller_style")
        {
            if (arg == "0" || arg == "1")
            {
                s_api->SetControllerStyle(arg == "1" ? 1 : 0);
                LOG_INFO("[PrismaUI] Controller style set to %s", arg == "1" ? "PlayStation" : "Xbox");
                PushState();
            }
        }
        else
        {
            LOG_WARN("[PrismaUI] Multiplayer: unknown command '%.60s'", cmdLine.c_str());
        }
    }

    // ── View lifecycle (game thread) ────────────────────────────────────────

    bool EnsureView()
    {
        LOG_REQUIREMENT("(anonymous)::EnsureView", "Game thread only, and only after PrismaUI's dispatcher is ready (i.e. from a user action, not plugin load).");
        if (!s_api)
            return false;
        if (ViewIsLive())
            return true;

        s_domReady.store(false);
        s_view = s_api->CreateView(kMultiplayerViewPath, OnDomReady);
        if (s_view == 0)
        {
            LOG_ERROR("[PrismaUI] CreateView('%s') failed -- see PrismaUI_F4.log", kMultiplayerViewPath);
            return false;
        }

        // Interactive panel: counted by other Prisma plugins' IsAnyPanelVisible.
        s_api->SetViewRole(s_view, PRISMA_UI_API::ViewRole::kPanel);
        // The page handles Escape itself (BACK / close), so the vanilla pause
        // menu must not also receive it.
        s_api->SetViewOwnsEscape(s_view, true);
        s_api->RegisterConsoleCallback(s_view, OnConsoleMessage);

        const bool cmdOk = s_api->BindGameThreadUIEvent(s_view, "cosync_cmd", OnCommand, nullptr);
        s_padBound = false;   // bound after DOM ready (BindPadActions)

        LOG_INFO("[PrismaUI] Created multiplayer view %llu ('%s') bindCmd=%d",
            (unsigned long long)s_view, kMultiplayerViewPath, cmdOk ? 1 : 0);
        return true;
    }

    void OpenMultiplayer(bool fromMainMenu)
    {
        LOG_REQUIREMENT("(anonymous)::OpenMultiplayer", "Game thread only.");
        if (!EnsureView())
        {
            s_openForInput.store(false);
            return;
        }

        s_wantOpen = true;
        s_openedFromMainMenu = fromMainMenu;
        s_openForInput.store(true);
        s_api->Show(s_view);

        if (!s_domReady.load())
        {
            LOG_INFO("[PrismaUI] Multiplayer opened (fromMainMenu=%d, focus deferred until DOM ready)", fromMainMenu ? 1 : 0);
            return;
        }

        const bool focused = s_api->Focus(s_view, false, false);
        LOG_INFO("[PrismaUI] Multiplayer opened (fromMainMenu=%d focus=%d)", fromMainMenu ? 1 : 0, focused ? 1 : 0);
        BindPadActions();   // retries if an earlier attempt was rejected

        // Page is already loaded: reset it to the root screen with fresh state.
        s_api->InteropCall(s_view, "cosyncOpen", "");
        PushState();
    }

    void CloseMultiplayer()
    {
        LOG_REQUIREMENT("(anonymous)::CloseMultiplayer", "Game thread only. Safe if the view was never created or is already closed.");
        const bool wasOpen = s_wantOpen;
        s_wantOpen = false;
        s_openForInput.store(false);
        if (!wasOpen || !ViewIsLive())
            return;
        s_api->Unfocus(s_view);
        s_api->Hide(s_view);
        LOG_INFO("[PrismaUI] Multiplayer closed");
    }

    void CloseOnGameThread(void*)
    {
        LOG_REQUIREMENT("(anonymous)::CloseOnGameThread", "Runs on the game thread via PrismaUI's dispatcher.");
        CloseMultiplayer();
    }

    void OpenFromMainMenuOnGameThread(void*)
    {
        LOG_REQUIREMENT("(anonymous)::OpenFromMainMenuOnGameThread", "Runs on the game thread via PrismaUI's dispatcher.");
        OpenMultiplayer(true);
    }

    void OpenFromHotkeyOnGameThread(void*)
    {
        LOG_REQUIREMENT("(anonymous)::OpenFromHotkeyOnGameThread", "Runs on the game thread via PrismaUI's dispatcher.");
        OpenMultiplayer(false);
    }

    void ToggleOnGameThread(void*)
    {
        LOG_REQUIREMENT("(anonymous)::ToggleOnGameThread", "Runs on the game thread via PrismaUI's dispatcher.");
        if (s_wantOpen)
            CloseMultiplayer();
        else
            OpenMultiplayer(false);
    }
}

namespace CoSyncPrismaUI
{
    bool Init()
    {
        LOG_REQUIREMENT("CoSyncPrismaUI::Init", "Game thread, after all F4SE plugins have loaded (kMessage_GameLoaded). Safe to call more than once.");
        if (s_api)
            return true;

        if (!PRISMA_UI_API::GetPrismaProviderModule())
        {
            LOG_WARN("[PrismaUI] PrismaUI_F4.dll not loaded -- CoSync's Multiplayer menu and HUD are unavailable");
            return false;
        }

        s_api = PRISMA_UI_API::RequestPluginAPI<PrismaAPI>();
        if (!s_api)
        {
            LOG_ERROR("[PrismaUI] RequestPluginAPI(V12) returned null -- PrismaUI too old? Multiplayer menu unavailable");
            return false;
        }

        LOG_INFO("[PrismaUI] V12 API acquired (MULTIPLAYER row / F10 open the PrismaUI menu)");
        return true;
    }

    bool IsAvailable()
    {
        LOG_REQUIREMENT("CoSyncPrismaUI::IsAvailable", "Doesn't touch the game. Safe any time.");
        return s_api != nullptr;
    }

    void RequestOpenMultiplayer(bool fromMainMenu)
    {
        LOG_REQUIREMENT("CoSyncPrismaUI::RequestOpenMultiplayer", "Safe from any thread: sets an atomic flag and queues the open on PrismaUI's game-thread dispatcher.");
        if (!s_api)
            return;

        // Set BEFORE dispatching: callers poll IsMultiplayerOpen() every frame
        // and must not see "closed" in the gap before the task runs.
        s_openForInput.store(true);

        if (!s_api->DispatchToGameThread(fromMainMenu ? OpenFromMainMenuOnGameThread : OpenFromHotkeyOnGameThread, nullptr))
        {
            s_openForInput.store(false);
            LOG_WARN("[PrismaUI] DispatchToGameThread refused the open (dispatcher not ready yet)");
        }
    }

    void RequestToggleMultiplayer()
    {
        LOG_REQUIREMENT("CoSyncPrismaUI::RequestToggleMultiplayer", "Safe from any thread: only queues work on PrismaUI's game-thread dispatcher.");
        if (!s_api)
        {
            LOG_WARN("[PrismaUI] Toggle ignored -- PrismaUI API not available");
            return;
        }
        if (!s_api->DispatchToGameThread(ToggleOnGameThread, nullptr))
            LOG_WARN("[PrismaUI] DispatchToGameThread refused the toggle (dispatcher not ready yet)");
    }

    bool IsMultiplayerOpen()
    {
        LOG_REQUIREMENT("CoSyncPrismaUI::IsMultiplayerOpen", "Safe from any thread: reads an atomic flag.");
        return s_openForInput.load();
    }

    void CloseMultiplayerNow()
    {
        LOG_REQUIREMENT("CoSyncPrismaUI::CloseMultiplayerNow", "Any thread. Closes synchronously only on PrismaUI's game/window thread; otherwise queues the close.");
        if (!s_api)
            return;
        if (s_api->IsGameThread())
        {
            LOG_INFO("[PrismaUI] Window closing -- releasing multiplayer menu focus first");
            CloseMultiplayer();
        }
        else if (!s_api->DispatchToGameThread(CloseOnGameThread, nullptr))
        {
            LOG_WARN("[PrismaUI] DispatchToGameThread refused the close");
        }
    }

    // Session state sampled by TickHud on CoSync's tick; read by the HUD
    // update on PrismaUI's thread.
    std::atomic<bool> s_hudSessionLive{ false };
    std::atomic<bool> s_hudUpdatePending{ false };

    // ── Chat box (PrismaUI's game thread) ──────────────────────────────────
    constexpr const char* kChatViewPath = "DoxCoSync/chat.html";
    PrismaView        s_chat = 0;
    std::atomic<bool> s_chatDomReady{ false };
    std::atomic<bool> s_chatOpen{ false };
    bool              s_chatShown = false;
    uint64_t          s_chatLastSeq = 0;
    uint64_t          s_chatNextColorMs = 0;

    void CloseChat()
    {
        if (!s_chatOpen.exchange(false))
            return;
        if (s_chat != 0 && s_api->IsValid(s_chat))
        {
            s_api->Unfocus(s_chat);
            s_api->SetViewRole(s_chat, PRISMA_UI_API::ViewRole::kWidget);
        }
        LOG_INFO("[PrismaUI] Chat closed");
    }

    // JS -> C++ (game thread): "send|G|text", "send|F|text", "close".
    void OnChatCommand(const char* argument, void*)
    {
        const std::string a = argument ? argument : "";
        if (a.rfind("send|", 0) == 0 && a.size() > 7 && (a[5] == 'G' || a[5] == 'F') && a[6] == '|')
            CoSyncChat::QueueSend(a[5], a.substr(7));
        else if (a == "close")
            CloseChat();
    }

    // Keeps the chat view alive during a session and pushes new messages.
    void ChatUpdateOnGameThread(bool sessionLive)
    {
        if (!sessionLive)
        {
            CloseChat();
            if (s_chatShown && s_chat != 0 && s_api->IsValid(s_chat))
                s_api->Hide(s_chat);
            s_chatShown = false;
            return;
        }
        if (s_chat == 0 || !s_api->IsValid(s_chat))
        {
            s_chatDomReady.store(false);
            s_chatLastSeq = 0;
            s_chat = s_api->CreateView(kChatViewPath, [](PrismaView v)
            {
                s_chatDomReady.store(true);
                LOG_INFO("[PrismaUI] Chat view %llu DOM ready", (unsigned long long)v);
            });
            if (s_chat == 0)
            {
                LOG_ERROR("[PrismaUI] CreateView('%s') failed", kChatViewPath);
                return;
            }
            s_api->SetViewRole(s_chat, PRISMA_UI_API::ViewRole::kWidget);
            s_api->RegisterConsoleCallback(s_chat, OnConsoleMessage);
            s_api->BindGameThreadUIEvent(s_chat, "cosync_chat", OnChatCommand, nullptr);
            LOG_INFO("[PrismaUI] Created chat view %llu", (unsigned long long)s_chat);
        }
        if (!s_chatShown)
        {
            s_api->Show(s_chat);
            s_chatShown = true;
        }
        if (!s_chatDomReady.load())
            return;
        const uint64_t nowMs = GetTickCount64();
        if (nowMs >= s_chatNextColorMs)
        {
            s_chatNextColorMs = nowMs + 5000;
            int r, g, b;
            ReadHudColor(r, g, b);
            const std::string rgb = "[" + std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b) + "]";
            s_api->InteropCall(s_chat, "cosyncChatHud", rgb.c_str());
        }
        // Who is talking (voice), pushed only when the set changes.
        {
            static std::string s_lastTalk;
            std::string talk = "[";
            bool first = true;
            for (const CoSyncHudPlayer& p : CoSyncOverlay_GetLobbySnapshot())
            {
                if (!CoSyncVoice::IsTalking(p.entityID))
                    continue;
                if (!first) talk += ",";
                first = false;
                talk += "\"" + JsonEscape(p.isLocal ? "You" : p.name.c_str()) + "\"";
            }
            talk += "]";
            if (talk != s_lastTalk)
            {
                s_lastTalk = talk;
                s_api->InteropCall(s_chat, "cosyncChatTalk", talk.c_str());
            }
        }
        // Push only when something new arrived (no polling in the page).
        if (CoSyncChat::LatestSeq() != s_chatLastSeq)
        {
            const std::string json = CoSyncChat::BuildJson(s_chatLastSeq);
            s_chatLastSeq = CoSyncChat::LatestSeq();
            s_api->InteropCall(s_chat, "cosyncChat", json.c_str());
        }
    }

    void OpenChatOnGameThread(void*)
    {
        if (!s_api || s_chat == 0 || !s_api->IsValid(s_chat) || !s_chatDomReady.load())
        {
            s_chatOpen.store(false);
            return;
        }
        s_api->SetViewRole(s_chat, PRISMA_UI_API::ViewRole::kPanel);
        const bool focused = s_api->Focus(s_chat, false, false);
        s_api->InteropCall(s_chat, "cosyncChatOpen", "G");
        LOG_INFO("[PrismaUI] Chat opened (focus=%d)", focused ? 1 : 0);
    }

    // PrismaUI's game thread only (all view calls below).
    void HudUpdateOnGameThread(void*)
    {
        s_hudUpdatePending.store(false);
        if (!s_api)
            return;
        ChatUpdateOnGameThread(s_hudSessionLive.load());
        const uint64_t nowMs = GetTickCount64();

        // Teammates only: the vanilla HUD already shows your own bars.
        std::vector<CoSyncHudPlayer> players = CoSyncOverlay_GetLobbySnapshot();
        players.erase(std::remove_if(players.begin(), players.end(),
            [](const CoSyncHudPlayer& p) { return p.isLocal; }), players.end());

        const bool want = s_hudSessionLive.load() && !s_wantOpen && !players.empty();
        if (!want)
        {
            if (s_hudShown && s_hud != 0 && s_api->IsValid(s_hud))
                s_api->Hide(s_hud);
            s_hudShown = false;
            return;
        }

        if (s_hud == 0 || !s_api->IsValid(s_hud))
        {
            s_hudDomReady.store(false);
            s_hudLastJson.clear();
            s_hud = s_api->CreateView(kHudViewPath, [](PrismaView v)
            {
                s_hudDomReady.store(true);
                LOG_INFO("[PrismaUI] HUD view %llu DOM ready", (unsigned long long)v);
            });
            if (s_hud == 0)
            {
                LOG_ERROR("[PrismaUI] CreateView('%s') failed", kHudViewPath);
                return;
            }
            // Passive overlay: never takes input, never counts as a panel.
            s_api->SetViewRole(s_hud, PRISMA_UI_API::ViewRole::kWidget);
            s_api->RegisterConsoleCallback(s_hud, OnConsoleMessage);
            LOG_INFO("[PrismaUI] Created HUD view %llu", (unsigned long long)s_hud);
        }

        if (!s_hudShown)
        {
            s_api->Show(s_hud);
            s_hudShown = true;
        }
        if (!s_hudDomReady.load())
            return;

        // HUD colour changes rarely; re-read the prefs file every 5 s.
        static int s_r = 18, s_g = 255, s_b = 21;
        static uint64_t s_nextColorMs = 0;
        if (nowMs >= s_nextColorMs)
        {
            s_nextColorMs = nowMs + 5000;
            ReadHudColor(s_r, s_g, s_b);
        }

        char num[64];
        std::string json;
        json.reserve(256 + players.size() * 160);
        json += "{\"hud\":[" + std::to_string(s_r) + "," + std::to_string(s_g) + "," + std::to_string(s_b) + "],\"players\":[";
        for (size_t i = 0; i < players.size(); ++i)
        {
            const CoSyncHudPlayer& p = players[i];
            if (i) json += ",";
            json += "{\"id\":" + std::to_string(p.entityID);
            json += ",\"name\":\"" + JsonEscape(p.name.c_str()) + "\"";
            json += ",\"level\":" + std::to_string(p.level);
            std::snprintf(num, sizeof(num), ",\"hp\":%.1f,\"maxHp\":%.1f,\"rads\":%.1f", p.hp, p.maxHp, p.rads);
            json += num;
            std::snprintf(num, sizeof(num), ",\"ap\":%.1f,\"maxAp\":%.1f", p.ap, p.maxAp);
            json += num;
            json += ",\"downed\":"; json += p.isDowned ? "true" : "false";
            json += ",\"talking\":"; json += CoSyncVoice::IsTalking(p.entityID) ? "true" : "false";
            json += ",\"hurt\":";   json += p.hurt ? "true" : "false";
            CoSyncFactions::Badge badge;
            if (CoSyncFactions::GetBadge(p.entityID, badge))
            {
                json += ",\"faction\":\"" + JsonEscape(badge.faction.c_str()) + "\"";
                json += ",\"rank\":\"" + JsonEscape(badge.rank.c_str()) + "\"";
                json += ",\"banner\":\"" + JsonEscape(badge.banner.c_str()) + "\"";
            }
            json += "}";
        }
        json += "]";
        const CoSyncVoice::Status vs = CoSyncVoice::GetStatus();
        json += ",\"voice\":{\"on\":"; json += vs.enabled ? "true" : "false";
        json += ",\"mic\":";   json += vs.micOk ? "true" : "false";
        json += ",\"mode\":\""; json += vs.mode; json += "\"";
        json += ",\"muted\":"; json += vs.muted ? "true" : "false";
        json += ",\"tx\":";    json += vs.transmitting ? "true" : "false";
        json += ",\"ptt\":";   json += vs.pushToTalk ? "true" : "false";
        json += "}}";

        if (json != s_hudLastJson)
        {
            s_hudLastJson = json;
            s_api->InteropCall(s_hud, "cosyncHud", json.c_str());
        }
    }

    // ── Overhead nameplates (PrismaUI's game thread) ───────────────────────
    constexpr const char* kPlatesViewPath = "DoxCoSync/nameplates.html";
    PrismaView        s_plates = 0;
    std::atomic<bool> s_platesDomReady{ false };
    std::atomic<bool> s_platesPending{ false };
    bool              s_platesShown = false;
    std::mutex        s_platesMx;
    std::string       s_platesJson = "[]";

    void PlatesOnGameThread(void*)
    {
        s_platesPending.store(false);
        if (!s_api)
            return;
        std::string json;
        {
            std::lock_guard<std::mutex> lk(s_platesMx);
            json = s_platesJson;
        }
        const bool any = json != "[]";
        if (!any)
        {
            // Nothing to draw: hide the view instead of rendering an empty page.
            if (s_platesShown && s_plates != 0 && s_api->IsValid(s_plates))
            {
                if (s_platesDomReady.load())
                    s_api->InteropCall(s_plates, "cosyncPlates", "[]");
                s_api->Hide(s_plates);
            }
            s_platesShown = false;
            return;
        }
        if (s_plates == 0 || !s_api->IsValid(s_plates))
        {
            s_platesDomReady.store(false);
            s_plates = s_api->CreateView(kPlatesViewPath, [](PrismaView v)
            {
                s_platesDomReady.store(true);
                LOG_INFO("[PrismaUI] Nameplates view %llu DOM ready", (unsigned long long)v);
            });
            if (s_plates == 0)
            {
                LOG_ERROR("[PrismaUI] CreateView('%s') failed", kPlatesViewPath);
                return;
            }
            s_api->SetViewRole(s_plates, PRISMA_UI_API::ViewRole::kWidget);
            s_api->RegisterConsoleCallback(s_plates, OnConsoleMessage);
            LOG_INFO("[PrismaUI] Created nameplates view %llu", (unsigned long long)s_plates);
        }
        if (!s_platesShown)
        {
            s_api->Show(s_plates);
            s_platesShown = true;
        }
        if (!s_platesDomReady.load())
            return;
        static uint64_t s_nextColorMs = 0;
        const uint64_t nowMs = GetTickCount64();
        if (nowMs >= s_nextColorMs)
        {
            s_nextColorMs = nowMs + 5000;
            int r, g, b;
            ReadHudColor(r, g, b);
            const std::string rgb = "[" + std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b) + "]";
            s_api->InteropCall(s_plates, "cosyncPlatesHud", rgb.c_str());
        }
        s_api->InteropCall(s_plates, "cosyncPlates", json.c_str());
    }

    void TickNameplates()
    {
        LOG_REQUIREMENT("CoSyncPrismaUI::TickNameplates", "CoSync tick, every frame. Projection on the tick; view work on PrismaUI's game thread, one update in flight.");
        if (!s_api)
            return;
        std::string json;
        const bool live = CoSyncTransport::IsInitialized() && CoSyncNet::IsConnected();
        const bool changed = CoSyncNameplates::Build(live, json);
        // Until the page is ready, keep re-sending so the first frame lands.
        if (!changed && s_platesDomReady.load())
            return;
        {
            std::lock_guard<std::mutex> lk(s_platesMx);
            s_platesJson = json;
        }
        if (s_platesPending.exchange(true))
            return;
        if (!s_api->DispatchToGameThread(PlatesOnGameThread, nullptr))
            s_platesPending.store(false);
    }

    void RequestOpenChat()
    {
        if (!s_api || s_chatOpen.exchange(true))
            return;
        if (!s_api->DispatchToGameThread(OpenChatOnGameThread, nullptr))
            s_chatOpen.store(false);
    }

    bool IsChatOpen()
    {
        return s_chatOpen.load();
    }

    // CoSync's tick. In game it runs on engine job threads, while PrismaUI's
    // game thread is the window thread (2026-09-26 logs: every UI event on
    // the window thread, the tick on 6-7 job threads). This used to bail out
    // whenever the two differed, so the teammate HUD switched itself off on
    // the first in-game tick ("TickHud called off PrismaUI's game thread")
    // and never showed. The update now goes to PrismaUI's thread.
    void TickHud()
    {
        LOG_REQUIREMENT("CoSyncPrismaUI::TickHud", "CoSync tick. Throttled to 10 Hz; the view work runs on PrismaUI's game thread and pushes to the page only when the data changed.");
        if (!s_api)
            return;

        static uint64_t s_nextMs = 0;
        const uint64_t nowMs = GetTickCount64();
        if (nowMs < s_nextMs)
            return;
        s_nextMs = nowMs + 100;

        s_hudSessionLive.store(CoSyncTransport::IsInitialized() && CoSyncNet::IsConnected());

        if (s_api->IsGameThread())
        {
            HudUpdateOnGameThread(nullptr);
            return;
        }
        // One update in flight at a time.
        if (s_hudUpdatePending.exchange(true))
            return;
        if (!s_api->DispatchToGameThread(HudUpdateOnGameThread, nullptr))
            s_hudUpdatePending.store(false);
    }
}
