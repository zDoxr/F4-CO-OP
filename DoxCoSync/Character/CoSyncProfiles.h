#pragma once

#include <cstdint>
#include <string>

struct F4SEInterface;
class TESNPC;
class Actor;

// Player profiles for the shared host save (Phase 7b).
//
// The HOST's save is the world. Every other player's character -- level, XP,
// SPECIAL, perks, inventory (+ what is equipped) and appearance (face/body) --
// is kept by the host INSIDE that save's F4SE co-save (record 'PROF'), so it
// rolls back and forward with the world and nothing gets out of step.
//
//   * Clients upload their profile (PROFILEUP) every 30 s and when leaving.
//   * The host writes every profile into the co-save whenever it saves --
//     including the "CoSync_World" save it sends to joining clients.
//   * A joining client loads that save; F4SE hands it the co-save, and the
//     client finds its own profile by key and applies it. No profile = a
//     fresh level-1 character, and the face/body editor opens.
//
// Identity: the real SteamID64, plus "-P<n>" for the second instance of the
// same account on one PC (CoSyncMultiInstance test proxy), so a returning
// player always gets the same character back.
namespace CoSyncProfiles
{
    // Plugin load: registers the co-save callbacks. Unique ID 'CSYN'.
    void RegisterSerialization(const F4SEInterface* f4se, uint32_t pluginHandle);

    std::string GetMyProfileKey();

    // Host: a client identified itself (from WORLDREQ).
    void HostOnPlayerKey(uint32_t conn, const std::string& key, const std::string& name);
    // Host: the profile key a connection identified itself with ("" if none
    // yet). Stable across sessions -- teams use it as the player identity.
    std::string HostGetConnKey(uint32_t conn);
    // Host, 0.5.1: the world already holds a finished character for this key.
    bool HostHasFinishedCharacter(const std::string& key);
    // Host: about to write the save that will be sent to a joining client.
    void HostBeforeWorldSave();

    bool IsProfileMessage(const std::string& msg);
    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole);

    // Client, main thread, every frame: applies the pending profile once the
    // host's world is loaded, then uploads snapshots every 30 s.
    void Tick(double now);

    // Client: upload one last snapshot right now (before leaving).
    void ClientUploadNow();
    // Client, 0.5.1: upload the character made in this player's own New Game
    // intro (lobby flow), marked finished.
    void ClientUploadIntroCharacter();

    // UI: "", "RESTORING", "CREATING", "CUSTOMIZING", "READY".
    const char* GetClientStatus();

    // Host: always. Client: once its saved (or fresh) character is applied --
    // before that the local player still wears the HOST's face and gear.
    bool IsCharacterApplied();

    // True while the character editor flow owns the player (Vault 111 room).
    bool IsEditorBusy();

    // Client, main thread, the moment the host's world has loaded: wipe the
    // inventory the save gave this player (the HOST's) so it is never seen.
    void ClientStripInheritedInventory();

    // ── Appearance for other players' proxies (CoSyncProxyLooks) ────────────
    // The local player's current face/body as a serialized blob ("" if unreadable).
    std::string CaptureMyLooks();
    // Writes a blob onto a proxy's NPC base (sex included). sexChanged = the
    // proxy's 3D must be fully reloaded; otherwise RebuildFace is enough.
    // MAIN THREAD.
    bool WriteLooksToBase(TESNPC* base, const std::string& blob, bool& sexChanged);
    void RebuildFace(Actor* actor);

    void Reset();
}
