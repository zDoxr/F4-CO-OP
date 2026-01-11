#pragma once

#include "Serialization.h"
#include "PluginAPI.h"
#include "PapyrusArgs.h"

class VMVariable;

namespace Serialization
{
	bool WriteVMData(const F4SESerializationInterface* intfc, const VMValue * val);
	bool ReadVMData(const F4SESerializationInterface* intfc, VMValue * val);
};
