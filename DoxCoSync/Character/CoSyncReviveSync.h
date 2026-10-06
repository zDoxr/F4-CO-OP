#pragma once

#include <string>
#include <cstdint>

// Networked player bleedout / revive (0.5.0 rework).
//
// AUTHORITY: the HOST decides. It owns every downed player's clock, grants a
// revive to exactly one reviver at a time, accepts its completion only from
// that reviver, and declares death when the clock runs out. The downed
// player's machine owns its own actor (bleedout, heal, respawn); a reviver's
// machine owns its own Stimpak.
//
//   target  -> all     DOWN|<t>                  entered bleedout (host starts the clock)
//   reviver -> host    RVCLAIM|<t>|<r>           "I want to revive t"
//   host    -> all     RVGRANT|<t>|<r>           r is reviving t (everyone plays the animation)
//   host    -> r       RVDENY|<t>|<r>|<reason>   refused (already being revived, expired, ...)
//   r / host -> all    RVCANCEL|<t>|<r>          the revive stopped (moved away, no Stimpak, left)
//   reviver -> host    RVDONE|<t>|<r>            the channel finished
//   host    -> all     REVIVED|<t>|<r>           t is revived by r; r's machine now spends
//                                                exactly ONE Stimpak (never earlier, never twice)
//   target  -> all     REVIVED|<t>|0             back on their feet after dying (respawn)
//   host    -> all     DIED|<t>                  the clock ran out: t died, no more revives
//   host    -> all     SVPT|<host>|0             the host saved the world: everyone records
//                                                where they stand as their respawn point
//
// ANIMATION: the engine's own "use a Stimpak on someone" activation idle --
// the dyn_Activation idles 3rdPUseStimpakOnTarget / 1stPStimPakOnTarget /
// PA*UseStimpakOnTarget (Fallout4.esm), which only play on a target that is
// HumanRace with keyword playerCanStimpak (DefaultObject
// KeywordPlayerCanStimpak: "an actor that can get a stimpak from the player
// when in bleedout"). Every machine gives that keyword to a downed player
// (proxy or its own player) and plays the idle through the engine's
// activation action, exactly as PlayerCharacter's companion heal does
// (sub_140D777D0 -> sub_140CFAE10(process, actor, 43, target base, target)),
// so the engine itself picks the first-person / third-person / Power Armor
// clip. On other machines the reviver's proxy plays it with bone
// replication paused for the channel, so everyone sees it.
namespace CoSyncReviveSync
{
    bool IsReviveMessage(const std::string& msg);
    // senderEid: the entity bound to `conn` (host role), 0 when unknown.
    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole, uint32_t senderEid);

    std::string SerializeDown(uint32_t entityID);
    std::string SerializeRevived(uint32_t entityID, uint32_t reviverEntityID);

    // Game thread, every tick: queued network ops, host clock, the local
    // revive channel.
    void FlushPendingReviveOps();
    void TickReviveChannel(double now);

    // The local player activated a downed player's proxy.
    void BeginReviveAttempt(uint32_t targetEntityID);

    // ── Local downed player (CoSynclocalplayer owns the actor) ──────────────
    void OnLocalDown();                   // just entered bleedout
    bool ConsumePendingLocalRevive(uint32_t& outReviverEntityID);
    // True once the player must die now: the host's DIED arrived, or no
    // answer from the host long after the clock ran out (host gone).
    bool ConsumeLocalDeath(double secondsDown);
    void OnLocalUp();                     // standing again (revive or respawn)
    // 0.5.1: tell everyone "on my feet without a revive" (REVIVED|me|0).
    void AnnounceLocalUp();
    // 0.5.1: respawn = back to where this player stood at the shared world's
    // most recent save. The host records it on every save and tells the
    // clients (SVPT|hostEid|0) to record theirs; a load records it too.
    void RecordRespawnPoint(const char* why);
    void SetRespawnPoint(bool interior, uint32_t space, const float pos[3], float rotZ, const char* why);
    void HostOnWorldSaved();
    bool MoveToRespawnPoint();
    // Seconds the player stays dead before respawning.
    float RespawnSeconds();

    // HUD overlay for the local player: state 0 ok / 1 downed / 2 dead.
    struct LocalStatus
    {
        int         state = 0;
        int         secondsLeft = 0;      // downed and dead: until the respawn
        int         reviveSecondsLeft = 0;// downed: the revive window left
        std::string reviver;              // downed: who is reviving you ("" = nobody)
    };
    LocalStatus GetLocalStatus(double now);

    // Session end while downed: let the player recover instead of staying
    // stuck in bleedout with the session's essential flag gone.
    void ReleaseLocalDownOnSessionEnd();

    // Host: a peer left -- its down clock and any revive it was part of end.
    void HostOnPeerLeft(uint32_t entityID);

    // "Revive" prompt over a downed player (TESNPC activate text hook).
    void InstallActivateTextHook();

    void Reset();

    // [Revive] settings in CoSync.ini.
    bool  SettingEnabled();
    float SettingDownedSeconds();     // fDownedSeconds, default 30
    float SettingHealthPercent();     // fReviveHealthPercent, default 50
    float SettingChannelSeconds();    // fReviveChannelSeconds, default 3
}
