#pragma once

#include <cstdint>
#include <string>

// Objects the HOST places with the console (placeatme) appear for everyone.
//
// Choke point (IDA 1.11.240, 2026-09-29): the console command table entry for
// "PlaceAtMe" (0x142EEACB0: name 0x24CA2F0, execute sub_1405C5110) and the
// other console placement command at sub_1405EF8C0 both create the reference
// through sub_1405E0900(outHandle, atRef, base, count, distance, direction,
// ...) -> sub_1405E0B30 -> TESDataHandler::CreateReferenceAtLocation
// (sub_1402E5110). Nothing else calls sub_1405E0900.
//
// Why not CreateReferenceAtLocation itself: it has ~30 callers -- workshop
// placement (already synced by CoSyncWorkshopAuthority), dropped items
// (CoSyncItemDropSync), the Power Armor exit, CoSync's own proxy/chassis
// spawns, and Papyrus PlaceAtMe (sub_141159E80 -> sub_141181600), which runs
// on every machine where the script runs. Replicating that level would
// duplicate objects other systems already sync and echo CoSync's own spawns.
// A console placement happens on exactly one machine, so it is replicated.
//
// Actor bases are left to CoSyncNpcWorldSync (it already replicates a new
// actor on the host as an NPC entity); replicating them here too would
// create a second copy on every client.
//
// Wire (host -> clients):
//   WSPAWN|hostRef|base|x|y|z|rx|ry|rz|ws|cell
//   WSDESPAWN|hostRef
// The host re-sends every live spawn every 10 s (late joiners, clients that
// were elsewhere); clients dedupe by hostRef and create the object once they
// are in the same worldspace (exterior) / cell (interior).
namespace CoSyncWorldSpawnSync
{
    void Install();                 // plugin load (fixed-address hook)
    void Tick(double now);          // CoSync tick, both roles
    bool IsSpawnMsg(const std::string& msg);
    void OnReceive(const std::string& msg, bool isHostRole);
    void Reset();                   // session end
}
