#pragma once

#include <cstdint>
#include <string>

// Load-order-independent FormIDs on the wire (0.3.3, sync-stability Stage 9).
//
// A runtime FormID encodes the plugin's position in THIS machine's active load
// order: full plugins (modIndex << 24 | local), light plugins
// (0xFE000000 | lightIndex << 12 | local). The host's load order is the
// canonical space. A client learns it at join (LOADORDERHOST|, sent by the
// host in answer to LOADORDER|) and then:
//   * Outbound: every FormID field of a message it sends is mapped from its
//     own plugin index to the host's (CoSyncTransport::Send).
//   * Inbound: every FormID field of a message it receives is mapped from the
//     host's index to its own (top of CoSyncNet::OnReceive).
// The host never translates. Dynamic 0xFF IDs and Fallout4.esm (index 0) pass
// through unchanged; a form from a plugin the other side does not have
// becomes 0, so it is rejected instead of resolving to the wrong object.
// Which fields are FormIDs is a per-message schema (CoSyncFormIdMap.cpp); a
// message not in the schema is passed through untouched. With identical load
// orders the tables are identity and translation is skipped entirely.
//
// Not translated on purpose: PROFILEUP (the client's own profile, read back
// only by that client from the host's co-save, so it stays in the client's
// space) and free-text messages (chat, mod events).
//
// Threading: tables are built on the CoSync tick; Outbound may be called from
// the capture/voice threads only for VOICE (not in the schema, fast return).
namespace CoSyncFormIdMap
{
    // Client: the host's load order ("F:a.esm,...;L:c.esl,..." from
    // CoSyncModSync::BuildLoadOrder) arrived. Builds both tables.
    void SetHostLoadOrder(const std::string& hostLoadOrder);
    void Reset();

    bool IsActive();   // a non-identity mapping is in force (client only)

    uint32_t ToHost(uint32_t localFormID);
    uint32_t ToLocal(uint32_t hostFormID);

    std::string Outbound(const std::string& msg);
    std::string Inbound(const std::string& msg);
}
