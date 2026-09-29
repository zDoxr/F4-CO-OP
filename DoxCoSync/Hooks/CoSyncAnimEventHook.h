#pragma once


void InstallAnimEventHook();


void UninstallAnimEventHook();


// GAME THREAD ONLY. Drains queued local-player animation events and forwards
// them to the network. Call once per frame from the game-thread tick -- the
// ReceiveEvent hook only enqueues (its thread is unverified), this does the Send.
void DrainLocalPlayerAnimEvents();

// GAME THREAD ONLY. Forwards the combat/animation graph events of the world
// NPCs THIS machine simulates (AE|), so their puppets on every other machine
// play the same hit reactions, staggers, melee attacks, sighted/ready states,
// blocks and sneaks. The hook only enqueues (formID, tag); ownership is
// checked here.
void DrainNpcAnimEvents();


namespace CoSyncAnimEventHook
{
    // Set around the receive-side replay of a remote animation event onto a
    // proxy so the capture hook ignores whatever the graph emits back into the
    // sink during that call. Same echo-guard shape as CoSyncEquipHook's
    // tls_cs_equip_applying; thread-local, and every true must be paired with
    // a false.
    void SetApplyingRemote(bool applying);
}
