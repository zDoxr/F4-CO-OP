#pragma once

#include <cstdint>

// Test-only teleport helper wired to the overlay's Players tab: lets the
// local player teleport to a connected player's proxy, or pull that proxy to
// the local player, entirely via entityID (no direct Actor* handling outside
// this module). Requests are queued on whatever thread the overlay button
// runs on and applied once per game tick, matching every other Flush*
// pending-op module in this codebase (CoSyncMapSync, CoSyncHealthSync, etc.).
// Eventually replaced by a map-marker-driven teleport; this is the rough cut
// used to prove the MoveTo plumbing works.
namespace CoSyncTeleport
{
    // Move the local (real) player onto targetEntityID's current position.
    void RequestTeleportLocalToEntity(uint32_t targetEntityID);

    // Move targetEntityID's proxy onto the local (real) player's position.
    void RequestTeleportEntityToLocal(uint32_t targetEntityID);

    // Applies the most recently queued request, if any. Call once per game
    // tick (CoSyncPlayerManager::Tick) -- never from the render/present thread.
    void FlushPendingTeleport();

    // F6 = teleport the local player to the other connected player's proxy.
    // Shift+F6 = pull that proxy to the local player instead. Resolves "the
    // other player" as the first non-local, spawned entry in the player
    // manager -- fine for the 2-player testing this exists for; a real target
    // picker (map marker, etc.) replaces this later. Uses raw GetAsyncKeyState
    // edge-detection so it works during real gameplay, independent of any
    // menu's input capture. Call once per frame from the main-thread tick
    // (CoSyncRuntime_TickGameThread).
    void PollHotkeys();
}
