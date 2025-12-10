#include "f4se/GameObjects.h"
#include "f4se_common/Relocation.h"
#include "f4se/GameForms.h"
#include "ConsoleLogger.h"


RelocAddr<_PlaceAtMe_Native> PlaceAtMe_Native(0x0045A310);


typedef TESForm* (*_LookupFormByID)(UInt32 formID);

// Version 1.10.163 / 1.10.169 absolute RVA
// This is universal for post-1.10 FO4 builds.
RelocAddr<_LookupFormByID> LookupFormByID(0x003C6E90);
