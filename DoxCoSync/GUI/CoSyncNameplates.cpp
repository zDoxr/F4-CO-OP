#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncNameplates.h"

#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncEntityTypes.h"
#include "CoSyncOverlay.h"
#include "CoSyncTeams.h"
#include "CoSyncVoice.h"
#include "CoSyncProfiles.h"
#include "CoSyncPrismaUI.h"

#include "GameReferences.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern PlayerCharacter* CoSyncGetPlayer();
extern bool CoSyncRuntime_IsMenuOpen(const char* menuName);

namespace
{
    constexpr uintptr_t kWorldToScreenRva = 0xA37C90;   // see the header
    typedef void (*WorldToScreenFn)(NiPoint3* in, NiPoint3* out);

    float s_maxDistance = 3500.f;
    float s_headOffset  = 140.f;
    bool  s_enabled     = true;
    std::once_flag s_cfgOnce;

    void LoadConfig()
    {
        std::call_once(s_cfgOnce, []
        {
            char exe[MAX_PATH] = {};
            if (!GetModuleFileNameA(nullptr, exe, MAX_PATH))
                return;
            std::string ini = exe;
            ini = ini.substr(0, ini.find_last_of("\\/") + 1) + "Data\\F4SE\\Plugins\\CoSync.ini";
            char buf[64] = {};
            s_enabled = GetPrivateProfileIntA("Nameplates", "bEnabled", 1, ini.c_str()) != 0;
            GetPrivateProfileStringA("Nameplates", "fMaxDistance", "3500", buf, sizeof(buf), ini.c_str());
            const float d = static_cast<float>(std::atof(buf));
            if (d >= 200.f && d <= 20000.f)
                s_maxDistance = d;
            GetPrivateProfileStringA("Nameplates", "fHeadOffset", "140", buf, sizeof(buf), ini.c_str());
            const float h = static_cast<float>(std::atof(buf));
            if (h >= 0.f && h <= 400.f)
                s_headOffset = h;
            LOG_INFO("[Nameplates] enabled=%d max distance=%.0f head offset=%.0f",
                s_enabled ? 1 : 0, s_maxDistance, s_headOffset);
        });
    }

    struct Proj { float x, y, z; };

    // The proxy's position and the projection, SEH-guarded: the proxy is
    // engine-owned and the camera may not be set up yet.
    bool Project(Actor* actor, float headOffset, float& px, float& py, float& pz, Proj& out)
    {
        __try
        {
            TESObjectREFR* r = reinterpret_cast<TESObjectREFR*>(actor);
            if ((r->flags & TESObjectREFR::kFlag_IsDisabled) || !r->parentCell)
                return false;
            px = r->pos.x; py = r->pos.y; pz = r->pos.z;
            NiPoint3 in;
            in.x = px; in.y = py; in.z = pz + headOffset;
            NiPoint3 o;
            o.x = o.y = 0.f; o.z = -1.f;
            const auto fn = reinterpret_cast<WorldToScreenFn>(
                reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) + kWorldToScreenRva);
            fn(&in, &o);
            out.x = o.x; out.y = o.y; out.z = o.z;
            return std::isfinite(o.x) && std::isfinite(o.y) && std::isfinite(o.z);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool MenusHide()
    {
        static const char* const kHide[] = {
            "PipboyMenu", "PauseMenu", "Console", "LooksMenu", "MainMenu", "LoadingMenu", "TerminalMenu",
            "ContainerMenu", "BarterMenu", "ExamineMenu", "FavoritesMenu", "LockpickingMenu", "BookMenu",
            "SleepWaitMenu", "CookingMenu", "LevelUpMenu", "MessageBoxMenu", "PowerArmorModMenu",
            "SPECIALMenu", "CreditsMenu",
        };
        for (const char* m : kHide)
            if (CoSyncRuntime_IsMenuOpen(m))
                return true;
        return CoSyncPrismaUI::IsMultiplayerOpen() || CoSyncProfiles::IsEditorBusy();
    }

    std::string JsonEscape(const std::string& s)
    {
        std::string o;
        for (unsigned char c : s)
        {
            if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
            else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
            else o += static_cast<char>(c);
        }
        return o;
    }

    std::string s_last = "[]";
}

namespace CoSyncNameplates
{
    bool Build(bool sessionLive, std::string& json)
    {
        LOG_REQUIREMENT("CoSyncNameplates::Build", "CoSync tick (same thread as CoSyncPlayerManager). Reads proxy positions and the engine camera projection.");
        LoadConfig();
        json = "[";
        PlayerCharacter* lp = CoSyncGetPlayer();
        if (s_enabled && sessionLive && lp && !MenusHide())
        {
            std::unordered_map<uint32_t, CoSyncHudPlayer> vitals;
            for (const CoSyncHudPlayer& p : CoSyncOverlay_GetLobbySnapshot())
                vitals[p.entityID] = p;

            const float lx = lp->pos.x, ly = lp->pos.y, lz = lp->pos.z;
            const uint32_t me = g_CoSyncPlayerManager.GetLocalEntityID();
            bool first = true;
            char num[96];
            // 0.3.6: one plate per proxy actor, whatever the entity map holds.
            std::vector<const Actor*> drawn;
            for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
            {
                const uint32_t eid = kv.first;
                const CoSyncPlayer* p = kv.second.get();
                if (!p || eid == me || p->isLocalPlayer || p->createType != CoSyncEntityType::Player || !p->actorRef)
                    continue;
                if (reinterpret_cast<const TESObjectREFR*>(p->actorRef) == reinterpret_cast<const TESObjectREFR*>(lp))
                    continue;
                if (std::find(drawn.begin(), drawn.end(), p->actorRef) != drawn.end())
                    continue;
                // 0.5.1 (Bug #6): names (and levels) only over teammates.
                if (!CoSyncTeams::IsLocalTeammate(eid))
                    continue;
                uint32_t ws, cell;
                if (g_CoSyncPlayerManager.GetEntityLocation(eid, ws, cell) &&
                    !CoSyncPlayerManager::IsLocalLocation(ws, cell))
                    continue;
                float px, py, pz;
                Proj s;
                if (!Project(p->actorRef, s_headOffset, px, py, pz, s))
                    continue;
                const float dx = px - lx, dy = py - ly, dz = pz - lz;
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                const float sy = 1.f - s.y;
                if (d > s_maxDistance || s.z < 0.f || s.x < -0.05f || s.x > 1.05f || sy < -0.05f || sy > 1.05f)
                    continue;

                auto v = vitals.find(eid);
                const std::string name = v != vitals.end() ? v->second.name : std::string("Player");
                const int level = v != vitals.end() ? v->second.level : 0;
                CoSyncTeams::Badge badge;
                const bool hasBadge = CoSyncTeams::GetBadge(eid, badge) && !badge.team.empty();

                drawn.push_back(p->actorRef);
                if (!first) json += ",";
                first = false;
                json += "{\"id\":" + std::to_string(eid);
                json += ",\"name\":\"" + JsonEscape(name) + "\"";
                json += ",\"level\":" + std::to_string(level);
                if (hasBadge)
                    json += ",\"team\":\"" + JsonEscape(badge.team) + "\"";
                json += ",\"talking\":"; json += CoSyncVoice::IsTalking(eid) ? "true" : "false";
                // 0.001 of the screen is under a pixel at 1080p: rounding keeps a
                // still scene from re-pushing identical frames.
                std::snprintf(num, sizeof(num), ",\"x\":%.4f,\"y\":%.4f,\"d\":%.0f}", s.x, sy, d);
                json += num;
            }
        }
        json += "]";
        if (json == s_last)
            return false;
        s_last = json;
        return true;
    }
}
