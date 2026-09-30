#pragma once

#include <cstdint>
#include <string>

// Global and faction text chat.
//
//   client -> host   CHAT|<G|F>|<text>                     (text percent-encoded)
//   host -> each     CHATM|<G|F>|<senderEid>|<name>|<faction>|<text>
//
// The HOST decides who a message reaches: the sender is the identity bound to
// its connection (never a field in the message), faction messages go only to
// the sender's faction's online members (CoSyncFactions), text must be valid
// UTF-8 without control characters and at most kMaxBytes, and every sender is
// rate limited. Clients never filter for privacy -- they only receive what
// the host sent them.
//
// THREADING: QueueSend / BuildJson are called from the PrismaUI (window)
// thread; OnReceive / Tick run on CoSync's tick. One internal mutex.
namespace CoSyncChat
{
    constexpr size_t kMaxBytes = 200;

    void OnSessionStart();
    void OnSessionEnd();

    bool IsChatMessage(const std::string& msg);
    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole);

    // Sends queued messages (CoSync tick).
    void Tick(double now);

    // From the chat box. scope 'G' = everyone, 'F' = my faction.
    void QueueSend(char scope, const std::string& text);

    // Messages newer than `afterSeq` as JSON {"seq":N,"msgs":[...]}.
    std::string BuildJson(uint64_t afterSeq);
    uint64_t LatestSeq();
    // A line shown only on this machine (no network), e.g. shared dialogue.
    void AddLocalLine(const std::string& name, const std::string& text);
}
