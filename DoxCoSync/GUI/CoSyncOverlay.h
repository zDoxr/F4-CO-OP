#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Multiplayer HUD STATE (no rendering).
//
// Gameplay systems (HealthSync, ReviveSync, LocalPlayer, PlayerManager,
// CoSyncNet) push per-player vitals and loading state here from the game
// thread; the PrismaUI HUD widget (CoSyncPrismaUI) reads a snapshot and draws
// it. Until Phase 6 this module also drew an ImGui overlay -- that is gone;
// the function names are kept so the feeding systems did not have to change.
// Every function is thread-safe.

// Session ended: clears every player's HUD entry.
void CoSyncOverlay_Disconnect();

// CoSync's own "we are joining a session" request.
void CoSyncOverlay_ShowLoadingScreen(bool show);

// The vanilla LoadingMenu's open/closed state, polled by CoSyncRuntime.
// Kept SEPARATE from the request above: they used to share one flag and the
// poll kept erasing the join request (~1 try in 5-9 survived).
void CoSyncOverlay_SetGameLoadingMenuOpen(bool open);
bool CoSyncOverlay_IsLoadingScreenActive();
void CoSyncOverlay_SignalLoadingComplete();

// ── Per-player vitals (name, level, health, rads, AP, downed) ──────────────
void CoSyncOverlay_UpdateLobbyAP(uint32_t entityID, float ap, float apMax);
void CoSyncOverlay_UpdateLobbyDowned(uint32_t entityID, bool isDowned);
void CoSyncOverlay_UpdateLobbyEntry(uint32_t entityID, const char* name,
                                    float hp, float maxHp, int level, bool isLocal,
                                    float rads = 0.f);
void CoSyncOverlay_RemoveLobbyEntry(uint32_t entityID);
void CoSyncOverlay_ClearLobby();

struct CoSyncHudPlayer
{
    uint32_t    entityID = 0;
    std::string name;
    int         level    = 0;
    float       hp       = 0.f;
    float       maxHp    = 0.f;
    float       rads     = 0.f;
    float       ap       = -1.f;   // -1 = never reported (no AP bar)
    float       maxAp    = 0.f;
    bool        isLocal  = false;
    bool        isDowned = false;
    bool        hurt     = false;  // took damage in the last 0.6 s (flash)
};

// Snapshot of every player's vitals, local player first.
std::vector<CoSyncHudPlayer> CoSyncOverlay_GetLobbySnapshot();
