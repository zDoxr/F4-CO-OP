#pragma once

#include <string>
#include <cstdint>

// Explosion sync (item #9).
//
// Covers every explosion the engine spawns as a reference: thrown grenades and
// mines, explosive weapon impacts (Fat Man, missile launcher, Broadsider,
// explosive legendary rounds), exploding cars and barrels, and creature attack
// explosions (Deathclaw AOE, Behemoth rock/slam/stomp, Bloodbug spit,
// Assaultron limb bursts, ...). Fallout4.esm ships 238 EXPL records and 139
// PROJ records; we key off the EXPL base rather than enumerating a hand-written
// weapon list, so modded and DLC explosives sync for free.
//
// Detection is deliberately POLL-based rather than a hook. An active explosion
// is a TESObjectREFR whose baseForm is a BGSExplosion (form type 90 / EXPL),
// and it lives in the cell for long enough that the fast tick reliably sees it.
// Hooking the engine's ExplosionFactory instead would mean a new RVA or vtable
// patch on a hot combat path -- this codebase has been burned repeatedly by
// stale RVAs (see GameGlobals.cpp), so polling is the cheaper risk here.
//
// NOTE ON DAMAGE: the replay spawns a real explosion on the peer, so the peer's
// own actors take its damage locally. That is intentional for visual/audio
// parity, but it means damage is NOT authoritative -- do not layer a damage
// relay on top of this without deduping, or hits will count twice.
namespace CoSyncExplosionSync
{
    // "EXP|baseFormID|x|y|z"
    bool IsExplosion(const std::string& msg);

    // Scans the loaded cell for newly-spawned explosion refs and broadcasts
    // them. Runs on BOTH host and client -- an explosion is caused by whoever
    // triggered it, not by the host. Call once per tick.
    void Tick(double now);

    void OnReceiveExplosion(const std::string& msg);

    // Applies queued remote explosions. Call once per game tick, never from
    // the render thread (spawns via PlaceAtMe_Native).
    void FlushPendingExplosions();
}
