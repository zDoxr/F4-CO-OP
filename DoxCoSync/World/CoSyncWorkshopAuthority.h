#pragma once

#include <cstdint>
#include <string>

struct F4SESerializationInterface;

// Authoritative, synchronized workshop building.
//
// GATE (every machine): the real engine operations are hooked
// (docs/WORKSHOP_RE.md) -- PlaceCurrentReference (build, move, wire),
// ScrapReference, Store, and the Repair confirmation. Before each runs, the
// player's settlement permission (CoSyncTeams: team / personal settlements)
// and the build limit are checked; a refused operation never happens.
//
// AUTHORITY (host): a client's operation is only a REQUEST (WKQ|). The host
// re-validates it against the sender's connection identity, the object's
// real settlement link and position, and every assigned settlement the
// position lies in (so "remove world build restrictions" never bypasses a
// team or personal settlement). Accepted -> the host applies it to its world (the persistent
// one) and broadcasts the RESULT (WKA/WKV/WKX/WKR) to everyone; refused ->
// the sender is told (WKN|) and its local change is reverted.
//
// IDENTITY: every replicated object has a host-assigned network id. Objects
// that already existed when a client joined are matched by form ID (the
// client loaded the host's save), checked against base form and position.
//
// Also owns the host's build rules: build limit (Fallout 4 default /
// custom object count / unlimited) and "remove world build restrictions".
namespace CoSyncWorkshopAuthority
{
    // CoSync runtime tick (in and out of sessions): installs the engine hooks
    // once a world is loaded, turns off the legacy cell-scan workshop sync,
    // sends queued operations, applies deferred remote results.
    void Tick(double now);

    void OnSessionStart(bool isHost);
    void OnSessionEnd();

    bool IsWorkshopMessage(const std::string& msg);
    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole);

    // Host: a client finished joining -- send it every replicated object.
    void HostOnPeerJoined(uint32_t conn);

    bool HooksActive();

    // Objects built through CoSync in a settlement (the custom build limit
    // counts these).
    uint32_t GetBuiltCount(uint32_t workshopFormID);

    // {"hooks":bool,"counts":{"<ws>":n,...}} for the Multiplayer menu.
    std::string BuildUiJson();

    // Host save: the object registry (record 'WSHP').
    void SaveRecord(const F4SESerializationInterface* ser);
    bool LoadRecord(const F4SESerializationInterface* ser, uint32_t type, uint32_t version, uint32_t length);
    void Revert();
}
