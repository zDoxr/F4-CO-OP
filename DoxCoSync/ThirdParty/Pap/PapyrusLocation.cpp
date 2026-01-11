#include "PapyrusLocation.h"
 
#include "GameForms.h"
#include "PapyrusArgs.h"

namespace papyrusLocation
{
	BGSLocation * GetParent(BGSLocation * thisLocation)
	{
		return thisLocation ? thisLocation->parent : nullptr;
	}

	void SetParent(BGSLocation * thisLocation, BGSLocation * newLoc)
	{
		if(thisLocation) {
			thisLocation->parent = newLoc;
		}
	}

	BGSEncounterZone * GetEncounterZone(BGSLocation * thisLocation, bool recursive)
	{
		BGSEncounterZone * result = nullptr;
		while(!result && thisLocation)
		{
			result = thisLocation->encounterZone;
			if(!recursive)
				break;
			thisLocation = thisLocation->parent;
		}
		return result;
	}

	void SetEncounterZone(BGSLocation * thisLocation, BGSEncounterZone * encounterZone)
	{
		if(thisLocation) {
			thisLocation->encounterZone = encounterZone;
		}
	}
}

#include "PapyrusVM.h"
#include "PapyrusNativeFunctions.h"

void papyrusLocation::RegisterFuncs(VirtualMachine* vm)
{
	vm->RegisterFunction(
		new NativeFunction0 <BGSLocation, BGSLocation*>("GetParent", "Location", papyrusLocation::GetParent, vm));

	vm->RegisterFunction(
		new NativeFunction1 <BGSLocation, void, BGSLocation*>("SetParent", "Location", papyrusLocation::SetParent, vm));

	vm->RegisterFunction(
		new NativeFunction1 <BGSLocation, BGSEncounterZone*, bool>("GetEncounterZone", "Location", papyrusLocation::GetEncounterZone, vm));

	vm->RegisterFunction(
		new NativeFunction1 <BGSLocation, void, BGSEncounterZone*>("SetEncounterZone", "Location", papyrusLocation::SetEncounterZone, vm));
}
