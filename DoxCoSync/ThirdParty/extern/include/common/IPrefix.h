#pragma once

#pragma warning(disable: 4018 4200 4244 4267 4305 4288 4312 4311)


#define _WIN32_WINNT	0x0602

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include "common/ITypes.h"
#include "common/IErrors.h"
#include "common/IDynamicCreate.h"
#include "common/IDebugLog.h"
#include "common/ISingleton.h"


#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <Windows.h>
