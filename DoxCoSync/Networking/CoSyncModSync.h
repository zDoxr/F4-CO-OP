#pragma once
#include <string>
#include <vector>

namespace CoSyncModSync {
    // Client calls this right after connecting to send its plugin list to the host
    void ClientSendModList();

    // Host receives MODLIST| packet, compares with its own plugins, sends missing ones
    void OnReceiveModList(const std::string& msg);

    // Client receives MODFILE| or MODSKIP| packet from host
    void OnReceiveModFile(const std::string& msg);

    // Packet type predicates
    bool IsModList(const std::string& msg);
    bool IsModFile(const std::string& msg);

    // Returns sorted list of .esp/.esm/.esl filenames from Data\
    std::vector<std::string> GetInstalledPlugins();
}
