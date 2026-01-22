#include "PapyrusEvents.h"

// Fallout 4 1.10.163 / F4SE 0.6.23+
// These addresses are stable for this runtime

RelocAddr<_SendCustomEvent> SendCustomEvent_Internal(0x01131F40);
RelocAddr<_CallFunctionNoWait> CallFunctionNoWait_Internal(0x01132340);
RelocAddr<_CallGlobalFunctionNoWait> CallGlobalFunctionNoWait_Internal(0x01132510);
