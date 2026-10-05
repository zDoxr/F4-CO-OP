#pragma once

#include <cstdint>
#include <string>

class TESObjectREFR;

// Elevators for every player (0.5.0).
//
// Fallout 4 has two kinds, both written for ONE player (vanilla scripts,
// Fallout4.esm 1.11.240):
//
//   LOAD elevators  LoadElevatorPanelScript / LoadElevatorDoorScript /
//                   LoadElevatorMasterScript. The car is a load door into a
//                   transition cell; the scripts open, close and block the
//                   doors and compare akActionRef with Game.GetPlayer(). Each
//                   player's ride is a cell transition of their own.
//   MOVING elevators ElevatorMasterScript / SimpleElevatorMasterScript. A
//                   master ref links its call buttons, panel and doors
//                   (LinkCustom01-10) and registers for their remote
//                   OnActivate; it moves the car only when
//                   akActionRef == Game.GetPlayer().
//
// What went wrong: every DOOR-type activation was replayed on the peers with
// a NULL activator. On a load elevator that made the peer's
// LoadElevatorDoorScript take "activated from the button" and close / block
// its doors, so after one player rode, the elevator no longer worked for the
// others. A moving elevator's buttons (ACTI) were not replicated at all, so
// the car moved only on the machine of the player who pressed.
//
// Now:
//   - a load-elevator control or door is never replicated (DOOR suppressed):
//     each player rides their own transition, the scripts stay consistent.
//   - pressing a moving elevator's control sends
//         ELEV|<senderEntity>|<controlRefFormID>
//     (through the host) and every other machine whose player is in the same
//     cell activates the same control AS ITS OWN PLAYER, so its master script
//     moves the car there too. Its DOOR replay is suppressed.
//
// THREADING: Classify is any-thread (sets under a mutex); Tick, OnReceive's
// apply and the cell scans are game thread.
namespace CoSyncElevatorSync
{
    enum class Kind { None, LoadElevator, MovingControl };

    // What `ref` is, from the last scan of the local player's cell.
    Kind Classify(uint32_t refFormID);

    // The local player just activated a moving-elevator control (door hook,
    // after the engine accepted it).
    void OnLocalControlActivated(uint32_t refFormID);

    bool IsElevatorMessage(const std::string& msg);
    void OnReceive(const std::string& msg, bool isHostRole);

    void Tick(double now);
    void Reset();
}
