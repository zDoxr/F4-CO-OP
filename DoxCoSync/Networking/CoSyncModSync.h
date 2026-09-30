#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace CoSyncModSync {
    // (0.4.0: the old MODLIST/MODFILE plugin push was removed; content is
    // matched before the join by CoSyncContentSync.)

    // ── Load order check (0.3.3, Stage 9) ─────────────────────────────────
    // Every FormID CoSync puts on the wire is a runtime ID: for a full plugin
    // (modIndex << 24 | local), for a light one (0xFE000000 | lightIndex << 12
    // | local). Both machines therefore need the SAME ACTIVE load order, or an
    // ID means a different object on each side. The content check before the
    // join (CoSyncContentSync) makes sure the client HAS the host's plugins;
    // this compares the order the engine actually loaded them in
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
