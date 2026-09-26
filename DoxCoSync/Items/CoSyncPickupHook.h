#pragma once

// Engine-level world item pickup hook. Hooks sub_140500430
// (INV_ADD_FROM_WORLD, RVA 0x00500430) — the "real" inventory-add helper
// called by PlayerCharacter::PickUp and world-item absorption paths.
//
// Filters: destActor must be local player (formID 0x14), srcREFR must
// have a parentCell (world-placed item, not container withdraw).
//
// On match: sends "PU|refFormID" which CoSyncWorldSync::OnReceivePickup
// already handles — the receiver disables the world REFR so it disappears.
//
// This replaces/supplements the polling-based pickup detection in
// CoSyncWorldSync::Tick which tracked lootable form IDs.

void InstallPickupHook();
void UninstallPickupHook();
