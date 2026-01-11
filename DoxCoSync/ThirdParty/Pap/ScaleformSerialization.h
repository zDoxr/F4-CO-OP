#pragma once

#include "Serialization.h"
#include "PluginAPI.h"

class GFxValue;

namespace Serialization
{
	template <>
	bool WriteData<GFxValue>(const F4SESerializationInterface* intfc, const GFxValue* val);

	template <>
	bool ReadData<GFxValue>(const F4SESerializationInterface* intfc, GFxValue* val);
};
