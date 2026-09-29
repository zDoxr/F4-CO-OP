#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace CoSyncModSync {
    // Client calls this right after connecting to send its plugin list to the host
    void ClientSendModList();

    // Host receives MODLIST| packet, compares with its own plugins, sends the
    // missing ones to THAT client only (conn = the requester's connection).
    void OnReceiveModList(const std::string& msg, uint32_t conn);

    // Client receives MODFILE| or MODSKIP| packet from host
    void OnReceiveModFile(const std::string& msg);

    // Packet type predicates
    bool IsModList(const std::string& msg);
    bool IsModFile(const std::string& msg);

    // Returns sorted list of .esp/.esm/.esl filenames in the Data folder
    std::vector<std::string> GetInstalledPlugins();

    // ── Load order check (0.3.3, Stage 9) ─────────────────────────────────
    // Every FormID CoSync puts on the wire is a runtime ID: for a full plugin
    // (modIndex << 24 | local), for a light one (0xFE000000 | lightIndex << 12
    // | local). Both machines therefore need the SAME ACTIVE load order, or an
    // ID means a different object on each side. MODLIST above only compares
    // files present in Data\; this compares what the engine actually loaded
    // (DataHandler::modList, in order).
    //
    // Client: sends LOADORDER|<list> right after HELLO.
    // Host:   compares with its own and answers that client only with
    //         LOADORDEROK or LOADORDERBAD|<n differences>|<summary>; the host
    //         also logs and shows who differs.
    std::string BuildLoadOrder();   // "F:name,F:name,...;L:name,..." (lower case)
    void ClientSendLoadOrder();
    bool IsLoadOrderMessage(const std::string& msg);
    void OnReceiveLoadOrderMessage(const std::string& msg, uint32_t conn, bool isHostRole,
                                   const std::string& senderName);
}
