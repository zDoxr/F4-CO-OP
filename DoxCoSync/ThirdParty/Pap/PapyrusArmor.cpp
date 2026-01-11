#include "PapyrusVM.h"
#include "PapyrusNativeFunctions.h"

#include "GameObjects.h"
#include "PapyrusArmor.h"

namespace papyrusArmor
{
	VMArray<TESObjectARMA*> GetArmorAddons(TESObjectARMO* thisArmor)
	{
		VMArray<TESObjectARMA*> results;
		if(!thisArmor)
			return results;

		for(UInt32 i = 0; i < thisArmor->addons.count; ++i)
		{
			TESObjectARMO::ArmorAddons addon;
			if(thisArmor->addons.GetNthItem(i, addon))
				results.Push(&addon.armorAddon);
		}
		
		return results;
	}
}

void papyrusArmor::RegisterFuncs(VirtualMachine* vm)
{
	// Armor Addons
	vm->RegisterFunction(
		new NativeFunction0 <TESObjectARMO, VMArray<TESObjectARMA*>>("GetArmorAddons", "Armor", papyrusArmor::GetArmorAddons, vm));
}
