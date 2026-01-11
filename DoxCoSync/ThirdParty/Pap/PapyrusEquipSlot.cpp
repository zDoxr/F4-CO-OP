#include "PapyrusEquipSlot.h"
 
#include "GameForms.h"
#include "PapyrusArgs.h"

namespace papyrusEquipSlot
{
	VMArray<BGSEquipSlot*> GetParents(BGSEquipSlot* equipSlot)
	{
		VMArray<BGSEquipSlot*> results;
		if(equipSlot)
		{
			for(UInt32 i = 0; i < equipSlot->parentSlots.count; i++)
			{
				BGSEquipSlot * slot = nullptr;
				equipSlot->parentSlots.GetNthItem(i, slot);
				results.Push(&slot);
			}
		}

		return results;
	}
}

#include "PapyrusVM.h"
#include "PapyrusNativeFunctions.h"

void papyrusEquipSlot::RegisterFuncs(VirtualMachine* vm)
{
	vm->RegisterForm(BGSEquipSlot::kTypeID, "EquipSlot");

	vm->RegisterFunction(
		new NativeFunction0 <BGSEquipSlot, VMArray<BGSEquipSlot*>>("GetParents", "EquipSlot", papyrusEquipSlot::GetParents, vm));
}
