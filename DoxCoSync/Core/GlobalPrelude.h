#pragma once
// Force-included into every TU via ForcedIncludeFiles in the vcxproj.
// Provides the minimum prerequisites that F4SE and ThirdParty headers
// assume are already in scope but don't include themselves.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include "ITypes.h"
#include "IDebugLog.h"
