#pragma once

#include "Networking.h"
//#include "F4MP.h"

namespace f4mp
{
	class Entity : public networking::Entity
	{
	public:
		using Vector3 = networking::Vector3;

		Entity(const Vector3& position);
	};
}