#pragma once

#include <cstdint>
#include <string>

// Session rules decided by the HOST (0.5.1, Bug #8).
//
//   PVE (default): players cannot damage each other. A hit whose aggressor is
//                  a player (or something a player fired) on another player
//                  is refused at the engine's Actor::DoHitMe (no damage, no
//                  stagger), and no player-to-player DMG is sent or applied.
//   PVP:           player damage is on; the victim's own machine applies it
//                  (DMG|<victim>|<amount>|<attacker>), the victim's health is
//                  that machine's alone.
//
// The host's choice lives in CoSync.ini ([Session] bPVP, default 0) and is
// set from the Multiplayer menu (host only).
//   host -> all   RULES|<pvp 0/1>      on change and every 5 s (late joiners)
namespace CoSyncSessionRules
{
    bool IsPvpEnabled();

    // Host: change the rule (UI). Saved to CoSync.ini, sent to everyone.
    void HostSetPvp(bool pvp);
    bool CanEdit();   // this machine is the host of a running session

    bool IsRulesMessage(const std::string& msg);
    void OnReceive(const std::string& msg, bool isHostRole);

    // CoSync tick: the host re-sends the rules every 5 s.
    void Tick(double now);

    // Session start / end (host reloads its saved choice; a client returns
    // to the PVE default until the host says otherwise).
    void Reset();
}
