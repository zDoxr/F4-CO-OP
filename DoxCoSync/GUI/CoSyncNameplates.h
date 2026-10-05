#pragma once

#include <string>

// Overhead name / team / level tags for the other players.
//
// Screen positions come from the engine's own world-to-screen projection
// (1.11.240 RVA 0xA37C90: F4SE 0.7.9 NiObjects.cpp WorldToScreen_Internal;
// IDA: projects through the global camera at 0x3279F80 under its own lock, so
// it is safe from CoSync's tick; out.x = 0..1 from the left, 1 - out.y = 0..1
// from the top, out.z < 0 = behind the camera -- as its HUD-marker caller at
// 0x9CC840 uses it).
//
// A tag is shown only for a player whose proxy is in the local player's
// location, enabled, in front of the camera and within [Nameplates]
// fMaxDistance; none while a game menu, the Multiplayer menu or the character
// editor is open. Not hidden behind walls: no engine line-of-sight call is
// verified for this yet (UNVERIFIED -- see docs/BUGFIX_INVESTIGATION.md).
namespace CoSyncNameplates
{
    // CoSync tick. Builds the tag list as JSON
    //   [{"id":N,"name":"...","level":N,"team":"...","talking":b,"x":f,"y":f,"d":f}]
    // Returns true when it differs from the last call (the caller pushes it).
    bool Build(bool sessionLive, std::string& json);
}
