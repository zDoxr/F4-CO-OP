#pragma once

#include <string>

// Door activate sync. Hooks the engine's "Activate worker" function
// (sub_140514180, RVA 0x00514180) which fires on every player E-key
// activation. Filters to door-like form types and broadcasts
// "DOOR|refFormID" to all peers.
//
// Receiver calls the same engine function via LookupFormByID so both
// sides toggle state in sync. TLS guard prevents echo.

void InstallDoorHook();
void UninstallDoorHook();

class TESObjectREFR;

namespace CoSyncDoorHook
{
    bool IsDoork(const std::string& msg);
    void OnReceiveDoor(const std::string& msg);
    void FlushPendingDoorOps();

    // True when `refr` is a currently-LOCKED reference. Reads the engine's
    // lock data via the same entry point CoSyncLockHook already uses
    // (kLockDataGet_RVA 0x00563170, locked = bit 0 of the flags byte at +0x10).
    // Returns false for anything without lock data, and on any fault.
    //
    // Used to stop a refused door activation being replayed on peers: without
    // it, walking into a locked door broadcast an open that succeeded remotely
    // while failing locally, leaving the two sides permanently disagreeing.
    bool IsRefLocked(void* refr);

    // True while this thread is inside a local activation (whose door change
    // goes out as DOOR) or applying a received DOOR. CoSyncWorldStateSync's
    // SetOpenState capture skips those so a toggle and an absolute state
    // never both describe the same change.
    bool InLocalActivation();

    // 0.3.6: `activator` activates `target` through the engine's own
    // activation (TESObjectREFR::ActivateRef, 0x1405144B0; CommonLibF4
    // signature (activator, objectToGet, count, defaultOnly, fromScript,
    // looping)), bypassing CoSync's detour so nothing is re-broadcast. Used to
    // put a player's proxy into its power armor frame with the normal animated
    // entry. Main thread. False if the hook is not installed or it faulted.
    bool ActivateAs(TESObjectREFR* target, TESObjectREFR* activator);
}
