#include "PapyrusWeapon.h"

#include "GameObjects.h"
#include "GameData.h"

#include "PapyrusStruct.h"

namespace papyrusWeapon
{
	BGSMod::Attachment::Mod * GetEmbeddedMod(TESObjectWEAP* thisWeapon)
	{
		return thisWeapon ? thisWeapon->weapData.embeddedMod : nullptr;
	}

	void SetEmbeddedMod(TESObjectWEAP* thisWeapon, BGSMod::Attachment::Mod * mod)
	{
		if(thisWeapon) {
			thisWeapon->weapData.embeddedMod = mod;
		}
	}
}

#include "PapyrusVM.h"
#include "PapyrusNativeFunctions.h"

void papyrusWeapon::RegisterFuncs(VirtualMachine* vm)
{
	vm->RegisterFunction(
		new NativeFunction0 <TESObjectWEAP, BGSMod::Attachment::Mod*>("GetEmbeddedMod", "Weapon", papyrusWeapon::GetEmbeddedMod, vm));

	vm->RegisterFunction(
		new NativeFunction1 <TESObjectWEAP, void, BGSMod::Attachment::Mod*>("SetEmbeddedMod", "Weapon", papyrusWeapon::SetEmbeddedMod, vm));
}
