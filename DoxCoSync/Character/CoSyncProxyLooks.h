#pragma once

#include <cstdint>
#include <string>

class TESNPC;

// Other players see YOUR face and body (Phase 7d).
//
// Every remote-player proxy used to spawn from one shared NPC record
// (CoSync.esp "RMPlayer"), so all of them wore the same stock face. A face
// lives on the NPC base record, so CoSync.esp now has seven identical bases --
// RMPlayer (0x00A797) and RMPlayer2..7 (0x00B6CF..0x00B6D4) -- one per remote
// player (an 8-player session has at most 7 other players on any screen).
//
//   * Each proxy gets its own base slot when it spawns, released on despawn.
//   * Every player sends its appearance, LOOKS|<entityID>|<blob>, just before
//     HELLO and again after finishing the character editor. The host relays
//     it and, when someone joins, re-sends its own and everyone else's.
//     HELLO is what makes the host announce the proxy, so the look arrives
//     first and is written onto the base BEFORE the proxy is created.
//   * A look that changes later (editor) is written onto the base and the
//     live proxy's face is rebuilt (full 3D reload if male/female changed).
//
// Threading: everything runs on the main thread (spawn tasks, the transport
// inbox and the runtime tick all do).
namespace CoSyncProxyLooks
{
    // Base NPC formID for this entity's proxy (acquires a slot). Falls back to
    // RMPlayer when all seven are taken or an older CoSync.esp lacks them.
    // 0 if CoSync.esp is not loaded at all.
    uint32_t BaseFormForEntity(uint32_t entityID);

    // Any of the seven proxy bases (load-order resolved).
    bool IsProxyBaseFormID(uint32_t formID);

    // Spawn path, right before PlaceAtMe: write the known look onto the base.
    void ApplyToBaseBeforeSpawn(uint32_t entityID, TESNPC* base);

    // Proxy gone: its slot can be reused.
    void OnProxyDespawned(uint32_t entityID);

    bool IsLooksMessage(const std::string& msg);
    void OnReceive(const std::string& msg, bool isHostRole);

    // Send this player's current look (to the host, which relays / to all).
    void SendMine();

    // Host, on a HELLO: re-send every look it knows (its own included) so the
    // newcomer has them before the CREATEs that follow.
    void HostSendAll();

    // Main thread, every frame: applies looks that arrived for proxies that
    // were still spawning.
    void Tick();

    void Reset();
}
