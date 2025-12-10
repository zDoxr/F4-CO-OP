#pragma once

#include <string>
#include "LocalPlayerState.h"

// Simple helpers for encoding/decoding LocalPlayerState to/from a
// compact string for LAN transport.
//
// Format (9 pipe-separated fields):
//
//   0: "STATE:formID,cellFormID"
//   1: "posX,posY,posZ"
//   2: "rotX,rotY,rotZ"
//   3: "velX,velY,velZ"
//   4: "health,maxHealth"
//   5: "ap,maxAP"
//   6: "isMoving,isSprinting,isCrouching,isJumping" (0/1)
//   7: "equippedWeaponFormID"
//   8: "username"
//
// Example:
//   STATE:20,56639|-77932.9,91515.9,7824.09|0.123171,-0,1.00837|0,0,0|0,0|0,0|0,0,0,0|0|Host
//
// For backward compatibility, Deserialize will also accept packets
// with only 8 fields (no username).

bool SerializePlayerStateToString(const LocalPlayerState& state, std::string& out);
bool DeserializePlayerStateFromString(const std::string& in, LocalPlayerState& out);

// Quick check whether a string looks like a player-state packet.
bool IsPlayerStateString(const std::string& in);
