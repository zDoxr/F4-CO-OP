#pragma once

#include <cstdint>
#include <string>

// Shared settlement state for team and personal settlements (0.5.0).
//
// MODEL: the host's world is the only persistent one, so the host's copy of a
// settlement is authoritative. Built objects already replicate to everyone
// (CoSyncWorkshopAuthority); what this module adds is the settlement STATE
// that Fallout 4 keeps on the workbench and that each machine would otherwise
// compute on its own:
//   - ratings: the actor values WorkshopParentScript maintains on the
//     workshop ref (Food, Water, Power, Defense/Safety, Beds, Happiness,
//     Population, and the rest of its WorkshopRatings table, read from the
//     WorkshopParent quest's own property data);
//   - resources: the workbench container's inventory (components, junk ...).
//
// AUDIENCE: only a settlement's participants (CoSyncTeams): every online
// member of the team for a team settlement, the owner for a personal one.
// Unassigned settlements are not streamed (each machine keeps its own view,
// the pre-0.5.0 behaviour), and nobody else receives another player's state.
//
// FLOW (text messages, ids decimal):
//   host -> participant  SSR|<ws>|<av>:<value>;...          ratings (on change)
//   host -> participant  SSI|<ws>|<ack>|<form>:<count>;...  workbench snapshot
//   participant -> host  SSD|<ws>|<seq>|<form>:<delta>;...  local workbench change
// A participant reports what changed in ITS workbench (taking or storing
// components, building, scrapping); the host validates the sender is a
// participant, applies the change to the authoritative workbench and sends
// the new snapshot to everyone, with the sender's last applied <seq> as
// <ack> so an older snapshot never undoes a change still in flight.
//
// THREADING: everything runs on CoSync's tick.
namespace CoSyncSettlementSync
{
    void OnSessionStart(bool isHost);
    void OnSessionEnd();
    void Tick(double now);

    bool IsSettlementMessage(const std::string& msg);
    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole);

    // Host: a client joined; it gets its settlements' state on the next pass.
    void HostOnPeerJoined(uint32_t conn);

    // {"id":..,"food":..,...,"synced":bool} for the settlement the player
    // stands in (Multiplayer menu).
    std::string BuildUiJson(uint32_t workshopFormID);
}
