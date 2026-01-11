#pragma once

class VirtualMachine;
struct StaticFunctionTag;

#include "GameTypes.h"

namespace papyrusGame
{
	void RegisterFuncs(VirtualMachine* vm);
}
