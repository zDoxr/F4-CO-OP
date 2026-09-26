#pragma once

#include <cstdint>
#include <string>

// Shared host save: the host's save IS the world.
//
// When a client connects, it asks for the world (WORLDREQ). The host saves
// "CoSync_World" (CoSync proxies are filtered out of it), then streams that
// save's .fos and F4SE co-save (.f4se) to that one client in chunks with a
// CRC32 each. The client writes them as "CoSync_Joined" in its save folder
// and loads it. Only then does the normal session start (HELLO / Init) --
// every player is in a copy of the same world.
//
// Wire (text prefix; chunks carry raw bytes after the header):
//   C->H  WORLDREQ|<build>|<profileKey>|<name>
//   H->C  WORLDBEGIN|<id>|<fosSize>|<coSize>|<fosCrc>|<coCrc>
//   H->C  WORLDCHUNK|<id>|<file 0=fos,1=co>|<offset>|<bytes...>
//   H->C  WORLDEND|<id>
//   H->C  WORLDERR|<reason>
//
// Threading: OnReceive/Tick/OnPostLoadGame run on the main thread (the
// transport inbox is drained there). Status getters are safe from any thread.
namespace CoSyncWorldTransfer
{
    enum class ClientPhase : int
    {
        Idle = 0,
        Requested,     // WORLDREQ sent, waiting for the host
        Downloading,   // receiving chunks
        Loading,       // files written, engine loading them
        Ready,         // host world loaded -- session may start
        Failed,
    };

    // True for every WORLD* message (so the caller can route it here).
    bool IsTransferMessage(const std::string& msg);

    // Main thread. conn = sender (host side) / 0 (client side is implicit).
    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole);

    // Main thread, every frame: host streams queued chunks; client drives
    // nothing (loading is event-driven).
    void Tick();

    // Client: called when the transport connects (main thread).
    void ClientBegin(const std::string& profileKey, const std::string& name);

    // F4SE kMessage_PostLoadGame (main thread).
    void OnPostLoadGame(bool success);

    // F4SE kMessage_NewGame (main thread): a new game is never the host's world.
    void OnNewGame();

    // Session over (either role). A client that was playing in the host's
    // world is sent back to the main menu (next Tick): that copy of the world
    // is not theirs to keep playing or saving.
    void Reset();

    // True from the moment a client's copy of the host's world finishes
    // loading until that player loads another save or starts a new game --
    // including after the session ended, while the game fades to the main
    // menu. Client saves are blocked while this is true. Any thread.
    bool IsInHostWorld();

    // Client status for the UI and the HELLO/Init gate.
    ClientPhase GetClientPhase();
    bool        IsClientWorldReady();
    int         GetClientProgressPercent();   // 0..100 while downloading
    std::string GetClientError();              // reason when Failed
}
