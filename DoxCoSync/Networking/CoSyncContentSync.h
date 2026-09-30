#pragma once

// Pre-join content sync, part 2: the handshake, the transfer and the install.
// (Part 1, the rules and the manifest, is CoSyncContentManifest.)
//
// A client must hold the host's content BEFORE it asks for the host's world:
// the world is the host's save, and a save loaded without one of its plugins
// silently drops everything that plugin added. So the join is now
//
//   connect -> CSREQ -> host manifest -> client checks its files (hash +
//   possession proof) -> host verdict -> [consent -> download -> verify
//   (SHA-256) -> queue -> quit -> install + enable at the next start, before
//   the engine opens any data file -> automatic rejoin -> check again] ->
//   verified -> WORLDREQ (existing world transfer) -> HELLO (session start)
//
// and the host refuses WORLDREQ, HELLO and every other session message from
// a connection that has not been verified (HostAllowsMessage).
//
// Wire (text header; CSDATA carries raw bytes after its header):
//   C->H  CSREQ|<build>|<protocol>
//   H->C  CSWAIT|<hashed files>|<total files>     host still hashing its content
//   H->C  CSFAIL|<reason>                         refused (version, protocol, abuse)
//   *     CSBIG|<type>|<seq>|<count>|<text>       one part of a long text message:
//            MAN  H->C  "NONCE|<hex>\n" + manifest (CoSyncContent::SerializeManifest)
//            REP  C->H  the client's report   ("LO|<load order>", "R|<idx>|<M/X/D>|<proof>", "E")
//            VER  H->C  the host's verdict    ("S|OK|NEED|ACTIVATE|BLOCK", "T|<idx>", "A|<plugin>",
//                                              "B|<code>|<name>", "W|<code>|<name>", "E")
//   C->H  CSGET|<idx>|<offset>|<length>           request one chunk (<= 256 KB)
//   H->C  CSDATA|<idx>|<offset>|<crc32 hex>|<bytes>
//   H->C  CSERR|<idx>|<reason>                    chunk could not be served
//   C->H  CSCANCEL|<reason>                       client stops
//   C->H  CSDONE|<result>                         client installed (informational)
//
// Threading: OnReceive / Tick / ClientBegin / UI calls run on the game's main
// thread (the transport inbox is drained there). Hashing, file reads, file
// writes and the install run on one worker thread; results come back to the
// main thread through a queue drained by Tick. Nothing here blocks the game
// thread on disk or network I/O.

#include <cstdint>
#include <string>

namespace CoSyncContentSync
{
    // Routing (CoSyncNet::OnReceive, before anything else touches the text).
    bool IsContentMessage(const std::string& msg);
    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole);

    // Main thread, every frame (CoSyncRuntime).
    void Tick();

    // F4SEPlugin_Load, synchronously: rolls back an interrupted install, then
    // installs what the last session downloaded and verified. Runs before
    // InitGameDataThread reads Plugins.txt and opens plugins/archives, so
    // nothing it replaces is in use.
    void ApplyPendingInstall();

    // F4SE kMessage_GameLoaded: arms the automatic rejoin after a content
    // restart.
    void OnGameLoaded();

    // The transport is gone (either role). An install in progress still
    // finishes (it is local); a download stops and is resumed next join.
    void OnSessionEnded();

    // ── Client ──────────────────────────────────────────────────────────────
    // The transport connected: start the content check. When the host
    // verifies this client, CoSyncWorldTransfer::ClientBegin is called.
    void ClientBegin();

    // What the player joined (for the rejoin after a restart).
    //   kind "ip":    value "<ipv4>:<port>"
    //   kind "steam": value "<lobby id hex>"
    void NoteJoinTarget(const std::string& kind, const std::string& value, const std::string& label);

    // Menu commands ("content|<arg>"): accept, cancel, quit.
    void UiCommand(const std::string& arg);

    // {"phase":...} for the Multiplayer menu (game thread).
    std::string BuildUiJson();

    // ── Host ────────────────────────────────────────────────────────────────
    // True once this connection's content matched the host's.
    bool HostIsConnVerified(uint32_t conn);

    // Join gate: false for any session message from a connection that has
    // not been verified (content messages and DISCONNECT always pass;
    // WORLDREQ passes here and is refused inside CoSyncWorldTransfer).
    bool HostAllowsMessage(uint32_t conn, const std::string& msg);
}
