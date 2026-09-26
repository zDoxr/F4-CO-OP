#pragma once
#include <string>

namespace CoSyncModEventBus
{
    // Route MODEVENT| packets from CoSyncNet::OnReceive into per-mod queues
    void OnReceive(const std::string& msg);

    // Papyrus: broadcast a mod event to connected peer(s)
    void Send(const std::string& modID,
              const std::string& eventName,
              const std::string& payload);

    // Papyrus: check whether an inbound event is waiting for this mod
    bool HasPendingEvent(const std::string& modID);

    // Papyrus: pop the oldest inbound event for this mod.
    // Returns true + fills outEventName/outPayload, false if queue is empty.
    bool PopEvent(const std::string& modID,
                  std::string& outEventName,
                  std::string& outPayload);
}
