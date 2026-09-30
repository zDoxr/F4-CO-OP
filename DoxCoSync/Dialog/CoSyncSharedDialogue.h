#pragma once

#include <string>
#include <cstdint>

// Shared dialogue (0.3.5): the host owns every conversation; nearby clients
// see it and may choose.
//
// ENGINE LIMIT (IDA 1.11.240, 2026-09-29): Fallout 4 player dialogue is
// scene-driven and bound to the one local PlayerCharacter. MenuTopicManager
// (singleton 0x30E6E50: speaker handle +0x14) shows the four choices of the
// current scene's player-dialogue action; a conversation therefore only exists
// in the host's engine, around the host's player. A client cannot be the
// "player" of a scene without re-implementing the dialogue system. What the
// engine does allow -- and what this implements -- is:
//
//   host   DialogueMenu::ProcessMessage (vtable 0x25AD118 slot 3,
//          sub_141043460) receives the four option texts as
//          "DialogueSetButtonText" UI data (index +0x18, text +0x20); captured
//          and broadcast with the speaker:
//            DLGM|speakerEid|seq|text0|text1|text2|text3
//            DLGX|speakerEid                       (menu closed)
//   client within kShowRange of that NPC sees the NPC and the options in the
//          chat panel and picks one with F1-F4:
//            DLGP|clientEid|speakerEid|seq|choice
//   host   validates -- sender owns clientEid (host message check), the menu
//          is still open on the same speaker and option set, the choice exists,
//          the client's player is within kChooseRange of the NPC -- then runs
//          the engine's own button path, MenuTopicManager::SelectOption
//          sub_140BAF7C0(manager, choice) (what DialogueMenu::Call does for a
//          click, sub_141043C10 case 1). Result:
//            DLGR|clientEid|ok|text-or-reason
//          The NPC's spoken reply, quest stages and world changes then follow
//          from the host's engine and reach everyone through DLG|L, QS and
//          world-state sync.
//   client activating an NPC is still refused locally (no local
//          conversation); the host is told who wants to talk to whom
//          (DLGQ|clientEid|npcEid) so the host can open it.
namespace CoSyncSharedDialogue
{
    // Plugin load: verified vtable slot swap on DialogueMenu.
    void Install();

    bool IsSharedDialogueMsg(const std::string& msg);
    // Game thread.
    void OnReceive(const std::string& msg, bool isHostRole);

    // Game thread, every CoSync tick.
    void Tick(double now);

    // Client: the local player tried to talk to this actor ref (refused).
    void OnClientTalkAttempt(uint32_t refFormID);

    void Reset();
}
