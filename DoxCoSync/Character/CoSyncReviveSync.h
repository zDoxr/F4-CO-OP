#pragma once

#include <string>
#include <cstdint>

// Networked player bleedout/revive. Mirrors CoSyncHealthSync's shape: parse
// incoming "DOWN|"/"REVIVEREQ|"/"REVIVED|" strings into a queue, apply on the
// game thread in FlushPendingReviveOps(). Local-player down/give-up/revive-
// apply logic lives in CoSynclocalplayer.cpp (it already owns the dead-player
// gate this has to cooperate with); this module owns the wire format, the
// remote-proxy presentation (isDowned flag + best-effort collapse/get-up
// anim), and the revive-interaction channel started from CoSyncDoorHook.
namespace CoSyncReviveSync
{
    bool IsDown(const std::string& msg);
    bool IsReviveReq(const std::string& msg);
    bool IsRevived(const std::string& msg);

    std::string SerializeDown(uint32_t entityID);
    std::string SerializeReviveReq(uint32_t entityID, uint32_t reviverEntityID);
    std::string SerializeRevived(uint32_t entityID, uint32_t reviverEntityID);

    void OnReceiveDown(const std::string& msg);
    void OnReceiveReviveReq(const std::string& msg);
    void OnReceiveRevived(const std::string& msg);

    // Applies queued down/revived ops to remote proxies (and retries the
    // local-player revive-apply once CoSynclocalplayer's respawn debounce
    // says it's safe to touch actor values again). Call once per tick from
    // the game thread, alongside CoSyncHealthSync::FlushPendingHealthOps.
    void FlushPendingReviveOps();

    // Starts a local revive channel on `targetEntityID` (a downed proxy the
    // local player just activated). Broadcasts REVIVEREQ immediately, then
    // REVIVED after fReviveChannelSeconds if not interrupted. Safe to call
    // repeatedly -- no-ops if a channel is already running or the target
    // isn't actually marked down.
    void BeginReviveAttempt(uint32_t targetEntityID);

    // Per-frame upkeep for an in-progress local revive channel (distance/
    // interrupt checks, completion). Call once per tick from the game thread.
    void TickReviveChannel(double now);

    // True once, then clears itself: a REVIVED packet targeting the LOCAL
    // player has arrived and is waiting to be applied. This module never
    // touches the local player's own actor (health AV, SetNoBleedoutRecovery)
    // itself -- CoSynclocalplayer.cpp owns that actor and its dead-player/
    // respawn-debounce gate, so it calls this at the one point it has already
    // confirmed actor-value access is safe again, and applies the heal there.
    bool ConsumePendingLocalRevive(uint32_t& outReviverEntityID);

    // Session teardown -- clears queues/channel state. Call alongside
    // CoSyncHealthSync::Reset().
    void Reset();

    // [Revive] ini settings, read once from CoSync.ini and cached, same
    // convention as CoSyncNet.cpp's ReadSaltInstanceIDSetting.
    bool  SettingEnabled();
    float SettingGiveUpSeconds();
    float SettingHealthPercent();
    float SettingChannelSeconds();
}
