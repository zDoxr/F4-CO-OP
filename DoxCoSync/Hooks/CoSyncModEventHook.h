#pragma once
#include <string>
#include <cstdint>

// Hooks ScriptObject.SendCustomEvent (FO4 native, RVA 0x01134E00).
// Intercepts every cross-mod custom event fired on the local machine and relays
// it to connected peers. On receive, re-fires it on the game main thread via
// FlushPendingCustomEvents() so the Papyrus VM is never called off-thread.

void InstallModEventHook();
void UninstallModEventHook();

// Called from CoSyncNet::OnReceive (network/transport thread)
void QueueRemoteCustomEvent(uint32_t senderFormID,
                            const std::string& eventName,
                            const std::string& serializedArgs);

// Called from CoSyncNet::Tick (game main thread)
void FlushPendingCustomEvents();
