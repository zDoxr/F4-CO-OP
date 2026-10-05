#pragma once

#include <cstdint>
#include <string>

class Actor;

// Player-to-player pickpocketing (0.5.0), host -> client and client -> host.
//
// Fallout 4 pickpockets an ACTOR: sneaking + activate opens ContainerMenu in
// pickpocket mode on the target's inventory (TESNPC::Activate sub_140655D60
// -> sub_14103E540(target, 2), 1.11.240) and the engine moves what you take.
// A player proxy only carries copies of what its player wears, and the real
// items live on that player's own machine, so:
//
//   thief  -> victim  PPREQ|<thief>|<victim>|<token>
//   victim -> thief   PPINV|<thief>|<victim>|<token>|<form>:<count>;...
//                     (what may be taken: carried, not worn, no quest items)
//   the thief's machine puts those items on the proxy for the length of the
//   menu and opens the engine's own pickpocket menu (the engine runs the
//   odds, the detection and the transfer exactly as for an NPC)
//   thief  -> victim  PPTAKE|<thief>|<victim>|<token>|<form>:<count>;...
//   victim -> thief   PPRES|<thief>|<victim>|<token>|<form>:<count>;...
//                     (what the victim's machine really removed -- the thief
//                     gives back anything it no longer had)
//
// Every message goes through the host, which forwards it only when the
// sending connection is the thief (PPREQ/PPTAKE) or the victim (PPINV/PPRES)
// it names. The victim's machine is the only one that changes its inventory.
//
// The "Talk" prompt on other players is gone: player proxies get the engine's
// own "cannot talk to the player" flag (ExtraCanTalkToPlayer, set through the
// AllowPCDialogue native's body sub_14028C290), so the prompt is "Pickpocket"
// while sneaking and nothing otherwise; activating one without sneaking does
// nothing.
//
// THREADING: everything on CoSync's tick, except IsPlayerSneaking (any thread).
namespace CoSyncPickpocket
{
    bool IsPickpocketMessage(const std::string& msg);
    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole);

    // The local player activated another player's proxy while sneaking.
    void BeginAttempt(uint32_t victimEntityID);

    // Engine "is the actor sneaking" (sub_140666050, the same test the
    // activation prompt uses). False on any fault.
    bool IsPlayerSneaking();

    // Player proxies: no "Talk" (ExtraCanTalkToPlayer = false). Idempotent.
    void ApplyNoTalk(Actor* proxy);

    void Tick(double now);
    void Reset();
}
