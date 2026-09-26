#include "CoSyncOverlay.h"
#include "CoSyncRequirementsLog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

// ── Loading-screen state ────────────────────────────────────────────────────
// 2026-08-05: two callers used to write the SAME flag and fight each other
// (CoSyncNet's join request vs CoSyncRuntime's LoadingMenu poll), so the state
// only survived by coincidence. They are separate inputs; the visible state is
// their OR, so neither can erase the other. Atomics: the writers run on
// different threads.
namespace
{
    std::atomic<bool> s_gameLoadingMenuOpen{ false };   // written by the runtime poll
    std::atomic<bool> s_coSyncLoadingWanted{ false };   // written by the join flow
}

void CoSyncOverlay_ShowLoadingScreen(bool show)
{
    LOG_REQUIREMENT("(global)::CoSyncOverlay_ShowLoadingScreen", "Doesn't touch game state — records CoSync's own join-flow loading request; safe from any thread.");
    s_coSyncLoadingWanted.store(show);
}

void CoSyncOverlay_SetGameLoadingMenuOpen(bool open)
{
    LOG_REQUIREMENT("(global)::CoSyncOverlay_SetGameLoadingMenuOpen", "Doesn't touch game state — records whether the vanilla loading menu is open; must not clear a pending CoSync join request.");
    s_gameLoadingMenuOpen.store(open);
}

bool CoSyncOverlay_IsLoadingScreenActive()
{
    LOG_REQUIREMENT("(global)::CoSyncOverlay_IsLoadingScreenActive", "Doesn't touch game state — reads two flags; safe any time.");
    return s_gameLoadingMenuOpen.load() || s_coSyncLoadingWanted.load();
}

void CoSyncOverlay_SignalLoadingComplete()
{
    LOG_REQUIREMENT("(global)::CoSyncOverlay_SignalLoadingComplete", "Doesn't touch game state — clears both loading flags so the state can't get stuck on either one.");
    s_coSyncLoadingWanted.store(false);
    s_gameLoadingMenuOpen.store(false);
}

void CoSyncOverlay_Disconnect()
{
    LOG_REQUIREMENT("(global)::CoSyncOverlay_Disconnect", "Doesn't touch game state — clears the HUD's player list. Network shutdown is CoSync_Main::Shutdown()'s job.");
    CoSyncOverlay_ClearLobby();
}

// ── Per-player vitals ───────────────────────────────────────────────────────
// Pushed from the game thread (LocalPlayer tick for "me", HealthSync /
// ReviveSync for remote players), read by the PrismaUI HUD on the game-thread
// tick. Everything lives behind s_lobbyMutex.

namespace
{
    struct LobbyEntry
    {
        uint32_t entityID = 0;
        char     name[64] = {};
        float    hp       = 0.f;
        float    maxHp    = 0.f;
        float    rads     = 0.f;
        float    ap       = -1.f;   // -1 = never reported; AP bar hidden
        float    maxAp    = 0.f;
        int      level    = 0;
        bool     isLocal  = false;
        bool     isDowned = false;
        double   lastDamageTime = 0.0;  // seconds; drives the brief red flash
    };

    std::mutex              s_lobbyMutex;
    std::vector<LobbyEntry> s_lobbyEntries;

    constexpr double kHurtFlashSeconds = 0.6;

    double LobbyNowSeconds()
    {
        LOG_REQUIREMENT("(anonymous)::LobbyNowSeconds", "Doesn't touch game state — just reads a steady system clock.");
        using namespace std::chrono;
        return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
    }
}

void CoSyncOverlay_UpdateLobbyEntry(uint32_t entityID, const char* name,
                                    float hp, float maxHp, int level, bool isLocal,
                                    float rads)
{
    LOG_REQUIREMENT("(global)::CoSyncOverlay_UpdateLobbyEntry", "Doesn't touch game state — copies the given values into the lock-protected list; safe from any thread.");
    if (hp < 0.f)    hp = 0.f;
    if (maxHp < 0.f) maxHp = 0.f;
    if (rads < 0.f)  rads = 0.f;

    std::lock_guard<std::mutex> lk(s_lobbyMutex);

    for (LobbyEntry& e : s_lobbyEntries)
    {
        if (e.entityID != entityID)
            continue;

        if (hp < e.hp - 0.5f)
            e.lastDamageTime = LobbyNowSeconds();

        e.hp      = hp;
        e.maxHp   = maxHp;
        e.rads    = rads;
        e.isLocal = isLocal;
        if (level > 0)
            e.level = level;
        if (name && name[0])
            strncpy_s(e.name, name, sizeof(e.name) - 1);
        return;
    }

    LobbyEntry e{};
    e.entityID = entityID;
    e.hp       = hp;
    e.maxHp    = maxHp;
    e.rads     = rads;
    e.level    = level;
    e.isLocal  = isLocal;
    if (name && name[0])
        strncpy_s(e.name, name, sizeof(e.name) - 1);
    else
        strncpy_s(e.name, isLocal ? "You" : "Player", sizeof(e.name) - 1);
    s_lobbyEntries.push_back(e);

    // Local player always first.
    std::stable_sort(s_lobbyEntries.begin(), s_lobbyEntries.end(),
        [](const LobbyEntry& a, const LobbyEntry& b)
        {
            if (a.isLocal != b.isLocal) return a.isLocal;
            return a.entityID < b.entityID;
        });
}

void CoSyncOverlay_UpdateLobbyAP(uint32_t entityID, float ap, float apMax)
{
    LOG_REQUIREMENT("(global)::CoSyncOverlay_UpdateLobbyAP", "Doesn't touch game state — safe from any thread. Does nothing if the entity has no entry yet.");
    if (ap < 0.f)    ap = 0.f;
    if (apMax < 0.f) apMax = 0.f;

    std::lock_guard<std::mutex> lk(s_lobbyMutex);
    for (LobbyEntry& e : s_lobbyEntries)
    {
        if (e.entityID != entityID)
            continue;
        e.ap    = ap;
        e.maxAp = apMax;
        return;
    }
}

void CoSyncOverlay_UpdateLobbyDowned(uint32_t entityID, bool isDowned)
{
    LOG_REQUIREMENT("(global)::CoSyncOverlay_UpdateLobbyDowned", "Doesn't touch game state — safe from any thread. Does nothing if the entity has no entry yet.");
    std::lock_guard<std::mutex> lk(s_lobbyMutex);
    for (LobbyEntry& e : s_lobbyEntries)
    {
        if (e.entityID != entityID)
            continue;
        e.isDowned = isDowned;
        return;
    }
}

void CoSyncOverlay_RemoveLobbyEntry(uint32_t entityID)
{
    LOG_REQUIREMENT("(global)::CoSyncOverlay_RemoveLobbyEntry", "Doesn't touch game state — safe from any thread.");
    std::lock_guard<std::mutex> lk(s_lobbyMutex);
    s_lobbyEntries.erase(
        std::remove_if(s_lobbyEntries.begin(), s_lobbyEntries.end(),
            [entityID](const LobbyEntry& e) { return e.entityID == entityID; }),
        s_lobbyEntries.end());
}

void CoSyncOverlay_ClearLobby()
{
    LOG_REQUIREMENT("(global)::CoSyncOverlay_ClearLobby", "Doesn't touch game state — safe from any thread.");
    std::lock_guard<std::mutex> lk(s_lobbyMutex);
    s_lobbyEntries.clear();
}

std::vector<CoSyncHudPlayer> CoSyncOverlay_GetLobbySnapshot()
{
    LOG_REQUIREMENT("(global)::CoSyncOverlay_GetLobbySnapshot", "Doesn't touch game state — copies the list under its lock; safe from any thread.");
    const double now = LobbyNowSeconds();
    std::vector<CoSyncHudPlayer> out;

    std::lock_guard<std::mutex> lk(s_lobbyMutex);
    out.reserve(s_lobbyEntries.size());
    for (const LobbyEntry& e : s_lobbyEntries)
    {
        CoSyncHudPlayer p;
        p.entityID = e.entityID;
        p.name     = e.name;
        p.level    = e.level;
        p.hp       = e.hp;
        p.maxHp    = e.maxHp;
        p.rads     = e.rads;
        p.ap       = e.ap;
        p.maxAp    = e.maxAp;
        p.isLocal  = e.isLocal;
        p.isDowned = e.isDowned;
        p.hurt     = (now - e.lastDamageTime) < kHurtFlashSeconds;
        out.push_back(std::move(p));
    }
    return out;
}
