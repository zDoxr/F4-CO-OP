#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Voice chat: proximity and faction, with no Steam dependency (GOG players
// must be able to use it too), so it carries its own pipeline:
//
//   microphone (waveIn, 48 kHz mono 16-bit, 20 ms frames)
//     -> Opus encoder (VOIP, 24 kbit/s, in-band FEC)            capture thread
//     -> VOICE|<P|F>|<seq>|<opus bytes>     client -> host, UNRELIABLE
//     -> host picks the recipients and the gain for each one
//     -> VOICEM|<eid>|<P|F>|<seq>|<gain>|<opus bytes>  host -> each, UNRELIABLE
//     -> Opus decoder (loss concealment / FEC) -> XAudio2 voice  playback thread
//
// The HOST decides who hears what. The speaker is the identity bound to the
// sending connection (never a field in the packet). Proximity: only players in
// the same location (same exterior worldspace, or the same interior cell) and
// within the HOST's [Voice] fRange, with the volume falling off from
// fFullVolumeRange to fRange. Faction: only the speaker's faction's online
// members, at full volume, anywhere. A client plays what the host sent it and
// nothing else.
//
// Payloads are raw bytes after the last header field ('|' inside the payload
// is fine: only the header fields are split). The packets bypass the message
// log (50 per second per speaker).
//
// THREADING: OnReceive / Tick run on CoSync's tick. Capture and playback have
// their own threads. The status getters and key actions are safe from any
// thread.
namespace CoSyncVoice
{
    void OnSessionStart();
    void OnSessionEnd();

    bool IsVoiceMessage(const std::string& msg);
    // senderEid: the host's identity for `conn` (0 = unknown / not the host).
    void OnReceive(const std::string& msg, uint32_t conn, uint32_t senderEid, bool isHostRole);

    // Sends captured frames (CoSync tick).
    void Tick(double now);

    // Keys ([Voice] in CoSync.ini). 0 = unbound.
    int  ModeKey();
    int  MuteKey();
    void CycleMode();     // proximity <-> faction (faction only while in one)
    void ToggleMute();

    struct Status
    {
        bool enabled      = false;   // [Voice] bEnabled and a session is running
        bool micOk        = false;   // the microphone opened
        char mode         = 'P';     // 'P' proximity, 'F' faction
        bool muted        = false;
        bool transmitting = false;   // sending right now
        bool pushToTalk   = false;   // false = voice activation
        bool inFaction    = false;
    };
    Status GetStatus();

    // Heard this player within the last ~350 ms (only players the host lets
    // you hear, so the indicator never leaks a faction conversation).
    bool IsTalking(uint32_t entityID);
}
