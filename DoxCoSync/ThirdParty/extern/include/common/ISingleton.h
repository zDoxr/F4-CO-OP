#pragma once

#include "common/IErrors.h"

#pragma warning(push)
#pragma warning(disable: 4311 4312)


template <typename T>
class ISingleton
{
	static T * ms_Singleton;

	public:
		ISingleton()
		{
			ASSERT(!ms_Singleton);
			intptr_t offset = (intptr_t)(T *)1 - (intptr_t)(ISingleton <T> *)(T *)1;
			ms_Singleton = (T *)((intptr_t)this + offset);
		}

		virtual ~ISingleton()
		{
			ASSERT(ms_Singleton);
			ms_Singleton = 0;
		}


		static T& GetSingleton(void)
		{
			ASSERT(ms_Singleton);
			return *ms_Singleton;
		}


		static T * GetSingletonPtr(void)
		{
			return ms_Singleton;
		}
};

template <typename T> T * ISingleton <T>::ms_Singleton = 0;

#pragma warning(pop)
