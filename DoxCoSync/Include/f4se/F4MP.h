#pragma once

#include "Networking.h"

#include <f4se/PluginAPI.h>
#include <f4se/Utilities.h>

#define F4MP_VERSION "0.001"

namespace f4mp
{
	using FormID =std::uint32_t;

	class F4MP
	{
		friend class Entity;
		friend class InputHandler;

	public:
		F4MP(const F4SEInterface* f4se);
		virtual ~F4MP();

	private:
		networking::Networking* networking;

		PluginHandle pluginHandle;
	};
}