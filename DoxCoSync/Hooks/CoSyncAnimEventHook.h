#pragma once


void InstallAnimEventHook();


void UninstallAnimEventHook();


// GAME THREAD ONLY. Drains queued local-player animation events and forwards
// them to the network. Call once per frame from the game-thread tick -- the
// ReceiveEvent hook only enqueues (its thread is unverified), this does the Send.
void DrainLocalPlayerAnimEvents();


namespace CoSyncAnimEventHook
{
    // Set around the receive-side replay of a remote animation event onto a
    // proxy so the capture hook ignores whatever the graph emits back into the
    // sink during that call. Same echo-guard shape as CoSyncEquipHook's
    // tls_cs_equip_applying; thread-local, and every true must be paired with
    // a false.
    void SetApplyingRemote(bool applying);
}
