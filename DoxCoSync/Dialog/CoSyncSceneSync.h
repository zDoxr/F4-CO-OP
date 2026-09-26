#pragma once
#include <string>
#include <cstdint>

namespace CoSyncSceneSync
{
    // Host-side: detect when the local player enters or leaves a BGSScene and
    // broadcast SCN|formID|1 (start) or SCN|formID|0 (stop).
    // Safe to call every tick; internally throttled to 0.25 s.
    void TickLocalScene(double now);

    // Packet routing.
    bool IsSceneSync(const std::string& msg);
    void OnReceiveSceneSync(const std::string& msg);

    // Drain queued Scene.Start/Stop Papyrus calls on the game thread.
    void FlushPendingSceneOps();
}
