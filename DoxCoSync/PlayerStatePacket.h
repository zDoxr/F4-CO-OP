#pragma once

#include <string>
#include "LocalPlayerState.h"

// PlayerState packets are TRANSIENT ONLY.
// They NEVER define identity or existence.

bool IsPlayerStateString(const std::string& msg);

// Serialize / deserialize
bool SerializePlayerStateToString(const LocalPlayerState& st, std::string& out);
bool DeserializePlayerStateFromString(const std::string& msg, LocalPlayerState& out);
