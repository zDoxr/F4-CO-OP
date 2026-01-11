#pragma once

class VirtualMachine;
struct StaticFunctionTag;

#include "GameTypes.h"

namespace papyrusForm
{
	void RegisterFuncs(VirtualMachine* vm);
}
