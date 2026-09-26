#pragma once

#include <string>
#include <cstdint>

namespace CoSyncDialogSync
{

    bool IsDialogue(const std::string& msg);

    void TickLocalDialogue(double now, uint32_t myEntityID);

    void OnReceiveDialogue(const std::string& msg);

    void FlushPendingDialogOps();

    // Installs a MinHook detour on the engine's BuildDialogueItem
    // (RVA 0x006984F0) -- the single chokepoint every spoken line passes
    // through, carrying (topic, speaker). The host uses it to broadcast
    // DLG|L so clients can replay the line on the matching NPC proxy.
    // Safe to call more than once; guarded internally.
    void InstallDialogueLineHook();

}
