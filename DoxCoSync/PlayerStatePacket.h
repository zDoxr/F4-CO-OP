#pragma once
#include <string>
#include <vector>
#include "LocalPlayerState.h"

// Text-prefix to identify a player state packet
static constexpr const char* kPlayerStatePrefix = "STATE:";

// Returns true if a string begins with our packet prefix
bool IsPlayerStateString(const std::string& msg);

// Serialize LocalPlayerState → utf-8 string
bool SerializePlayerStateToString(const LocalPlayerState& state, std::string& out);

// Deserialize utf-8 string → LocalPlayerState
bool DeserializePlayerStateFromString(const std::string& text, LocalPlayerState& out);

// (Optional later) binary versions remain available if you want to keep them
bool SerializePlayerState(const LocalPlayerState& state, std::vector<UInt8>& out);
bool DeserializePlayerState(const UInt8* data, size_t size, LocalPlayerState& out);
bool IsPlayerStatePacket(const UInt8* data, size_t size);
