#pragma once

#include <string>
#include <cstdint>

class Actor;

class CoSyncOutfitSync
{
public:

    static bool IsOutfit(const std::string& msg);


    static void TickLocalOutfit(uint32_t myEntityID, Actor* player);


    static void OnReceiveOutfit(const std::string& msg);


    static void FlushPendingOutfitOps();

    // Session start/teardown: clears the sent/applied caches so a fresh
    // session re-sends and re-applies full outfits (stale caches suppressed
    // the initial outfit for peers who joined after the last local change).
    static void Reset();

    // 2026-07-27: call when entityID's underlying proxy actor gets freed and
    // replaced (e.g. CoSyncPlayerManager's "freed (unloaded) — clearing"
    // path, hit whenever a save reload tears down and rebuilds every dynamic
    // actor in the world). s_appliedOutfits/s_inFlightOutfits are keyed by
    // entityID only, with no notion of "which actor object this was applied
    // to" -- so a rebuilt proxy inherits its predecessor's "already applied"
    // cache and the outfit silently never gets re-equipped onto the new
    // actor (confirmed: FLUSH logged "3 confirmed, 0 in-flight" every cycle
    // after a reload, with the new proxy visibly bare). Clearing here forces
    // the next OUTFIT packet (or periodic resend) to re-apply from scratch.
    static void ClearAppliedOutfits(uint32_t entityID);
};
