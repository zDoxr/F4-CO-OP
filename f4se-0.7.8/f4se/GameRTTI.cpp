#include "GameRTTI.h"
#include "f4se_common/Relocation.h"

typedef void * (* _Runtime_DynamicCast_Internal)(void * srcObj, UInt32 arg1, const void * fromType, const void * toType, UInt32 arg4);

// RVA CORRECTED round 106: the old 0x022BCC7E lands on "jmp cs:__imp_fclose"
// -- confirmed via IDA disassembly, it's part of a row of unrelated CRT
// import thunks (strncmp/vsprintf_s/fopen_s/fclose/fgets/strcpy_s/vsscanf/
// tolower/isupper). Every DYNAMIC_CAST() call in this codebase has been
// calling fclose(srcObj) instead of performing an RTTI cast. The real
// __RTDynamicCast thunk ("jmp cs:__imp___RTDynamicCast", confirmed by IDA
// name lookup AND disassembly, sitting in its own correct row alongside
// _purecall/memset/memcpy/memmove) is 0x022BCB9E, 0xE0 bytes earlier.
RelocAddr <_Runtime_DynamicCast_Internal> Runtime_DynamicCast_Internal(0x022BCB9E);	// __RTDynamicCast

void * Runtime_DynamicCast(void * srcObj, const void * fromType, const void * toType)
{
	uintptr_t fromTypeAddr = uintptr_t(fromType) + RelocationManager::s_baseAddr;
	uintptr_t toTypeAddr = uintptr_t(toType) + RelocationManager::s_baseAddr;

	return Runtime_DynamicCast_Internal(srcObj, 0, (void *)fromTypeAddr, (void *)toTypeAddr, 0);
}

#include "GameRTTI.inl"
