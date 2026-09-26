#pragma once

#include <string>
#include "LocalPlayerState.h"

bool IsPlayerStateString(const std::string& msg);


bool SerializePlayerStateToString(const LocalPlayerState& st, std::string& out);
bool DeserializePlayerStateFromString(const std::string& msg, LocalPlayerState& out);
