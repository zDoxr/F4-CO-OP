#pragma once

#include <cstdint>

// Session-wide constants shared by GNS_Session (transport), CoSyncDiscovery
// (LAN discovery) and the UI. One place, so the 8-player cap and the ports
// can never disagree between layers.
namespace CoSyncSessionConfig
{
    // Hard cap, host included. Enforced when GNS accepts a connection.
    constexpr int kMaxPlayers = 8;

    // GNS listen port. If it is taken (e.g. a second CoSync instance on the
    // same PC) the host tries the next ports up to kGamePortBase + kGamePortTries - 1
    // and advertises whichever one it actually bound.
    constexpr uint16_t kGamePortBase  = 48000;
    constexpr int      kGamePortTries = 8;

    // LAN discovery (UDP). Hosts listen here; searching clients send from an
    // ephemeral port and receive the hosts' unicast replies there.
    constexpr uint16_t kDiscoveryPort = 48001;

    // GNS application close reasons (k_ESteamNetConnectionEnd_App_Min..Max),
    // sent by the host when it rejects a connection so the client can tell the
    // player exactly why.
    constexpr int kEndReason_SessionFull     = 1001;
    constexpr int kEndReason_VersionMismatch = 1002;
}
