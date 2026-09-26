#pragma once

#include "GameReferences.h"
#include <cstdint>

// Tracks the remote player proxy's position as a map marker on the Pip-Boy
// map. One pre-placed REFR in CoSync.esp (local formID kMarkerLocalFormID) is
// revealed on connect and teleported to the proxy's world position once per
// second via Papyrus ObjectReference.MoveTo so the local player always sees
// the other player on the map.
namespace CoSyncPlayerMarker
{
    // Look up and cache the marker REFR from CoSync.esp.
    // No-op if called more than once; safe to call before world ready (will
    // warn and become a no-op until the REFR actually exists at runtime).
    void Init();

    // Called when a remote player proxy is fully spawned. Enables and reveals
    // the marker REFR, then begins tracking the proxy's position each Tick.
    void ShowForProxy(Actor* proxyActor, uint32_t entityID);

    // Called when a specific remote player proxy despawns. Hides that player's
    // marker and frees its slot for another peer.
    void HideForEntity(uint32_t entityID);

    // Hides EVERY assigned marker. Session teardown / disconnect only --
    // per-player removal is HideForEntity.
    void HideMarker();

    // Called once per manager tick; moves the marker to the proxy's current
    // world position at a throttled interval. No-op when not tracking.
    void Tick(double now);
}
