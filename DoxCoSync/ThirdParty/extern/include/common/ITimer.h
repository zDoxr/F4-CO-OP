#pragma once

#include "common/ITypes.h"


class ITimer
{
	public:
		ITimer();
		~ITimer();

		static void	Init(void);
		static void DeInit(void);

		void	Start(void);

		double	GetElapsedTime(void);

	private:
		UInt64	m_qpcBase;
		UInt32	m_tickBase;

		static double	s_secondsPerCount;
		static TIMECAPS	s_timecaps;
		static bool		s_setTime;


		static UInt64	GetQPC(void);

		static UInt64	s_lastQPC;
		static UInt64	s_qpcWrapMargin;
		static bool		s_hasLastQPC;

		static UInt32	s_qpcWrapCount;
		static UInt32	s_qpcInaccurateCount;
};
