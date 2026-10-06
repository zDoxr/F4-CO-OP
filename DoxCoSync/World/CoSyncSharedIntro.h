#pragma once

#include <cstdint>
#include <string>

class NiPoint3;

// The New Game intro played together (0.5.3).
//
// Everyone is in the host's world from the bathroom mirror on (CoSyncWorld-
// Transfer lobby). Fallout4.esm, read from the ESM: MQ101 "War Never Changes"
// 0x1ED86; the mirror spot MQ101PlayerStartMarker01 0xA7D33 at
// (-80303, 90828, 7856) rz 2.54 in SanctuaryHillsWorld 0xA7FF4;
// PrewarVault111 0xCA260 (the freeze) and Vault111Cryo 0x16D8 (the wake-up)
// with the same pod layout: player pod (640,-288), spouse pod (640,-736),
// resident pods (384/128/-64,-288) and (832/384/128,-736).
//
//   Story lockstep: while the intro runs, a client sets no quest stage of its
//     own (its copy of MQ101's fragments chained 70/65/75/85 off the host's 30
//     and, forwarded as QSREQ, pushed the host's intro past the Vault-Tec
//     rep -- 2026-10-05 21:33:01). The host's stages reach it as QS; the host
//     refuses intro QSREQ.
//   SPECIAL / name: in a shared intro a client gets the SPECIAL menu when the
//     host's does (the Vault-Tec rep), or at once after its mirror if the host
//     is already past it.          host -> all  INTRO|special
//                                  host -> all  INTRO|state|<special 0/1> (5 s)
//   Vault 111: the first player inside PrewarVault111 brings everyone still
//     outside.                     any  -> all  INTRO|vault|<eid>|x|y|z
//   From the vault on (MQ101 600+) this module steps back: the cryopod
//     sequence is each machine's own vanilla one, the pods and the way out are
//     CoSyncVault111's (0.5.6).
namespace CoSyncSharedIntro
{
    // MQ101 running and not completed (any session state).
    bool IsIntroRunning();
    // ...and a CoSync session is live.
    bool IsSharedIntro();

    // Quest hook: a client must not set this stage itself (pre-war lockstep).
    bool BlockClientStage(uint32_t questFormID, int stage);
    // QuestSync: the host refuses this client stage request.
    bool HostRefusesRequest(uint32_t questFormID);

    // Profiles: where the character editor puts a new player in the intro.
    bool GetMirrorSpot(uint32_t& worldspace, NiPoint3& pos, float& rotZ);
    // Profiles: the player's character was just made (mirror done).
    void OnLocalCharacterMade();

    bool IsIntroMessage(const std::string& msg);
    void OnReceive(const std::string& msg, bool isHostRole);
    void Tick(double now);
    void Reset();
}
