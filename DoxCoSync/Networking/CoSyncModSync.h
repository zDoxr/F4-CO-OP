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
}
