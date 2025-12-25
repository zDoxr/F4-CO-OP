#pragma once

// ------------------------------------
// Windows macro hygiene
// ------------------------------------
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// ------------------------------------
// Standard library FIRST
// ------------------------------------
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <type_traits>
#include <iterator>

// ------------------------------------
// Windows types
// ------------------------------------
#include <Windows.h>

// ------------------------------------
// F4SE base headers (order matters)
// ------------------------------------
#include "f4se/PluginAPI.h"
#include "f4se_common/Utilities.h"
