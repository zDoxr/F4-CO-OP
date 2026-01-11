#pragma once

class VirtualMachine;
struct StaticFunctionTag;

#include "GameTypes.h"

namespace papyrusActor
{
	void RegisterFuncs(VirtualMachine* vm);
}
