#include "papyrusMiscObject.h"

#include "PapyrusArgs.h"
#include "PapyrusStruct.h"

#include "GameExtraData.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "GameRTTI.h"

namespace papyrusMiscObject
{
	DECLARE_STRUCT(MiscComponent, "MiscObject")

	VMArray<MiscComponent> GetMiscComponents(TESObjectMISC * thisObject)
	{
		VMArray<MiscComponent> result;
		if(!thisObject)
			return result;

		if(!thisObject->components)
			return result;

		for(UInt32 i = 0; i < thisObject->components->count; i++)
		{
			TESObjectMISC::Component cp;
			thisObject->components->GetNthItem(i, cp);

			MiscComponent comp;
			comp.Set("object", cp.component);
			comp.Set("count", (UInt32)cp.count);
			result.Push(&comp);
		}

		return result;
	}

	void SetMiscComponents(TESObjectMISC * thisObject, VMArray<MiscComponent> components)
	{
		if(thisObject) {
			if(!thisObject->components)
				thisObject->components = new tArray<TESObjectMISC::Component>();

			thisObject->components->Clear();

			for(UInt32 i = 0; i < components.Length(); i++)
			{
				MiscComponent comp;
				components.Get(&comp, i);

				UInt32 count;
				TESObjectMISC::Component cp;
				comp.Get("object", &cp.component);
				comp.Get("count", &count);
				cp.count = count;
				thisObject->components->Push(cp);
			}
		}
	}
}

#include "PapyrusVM.h"
#include "PapyrusNativeFunctions.h"

void papyrusMiscObject::RegisterFuncs(VirtualMachine* vm)
{
	vm->RegisterFunction(
		new NativeFunction0 <TESObjectMISC, VMArray<MiscComponent>>("GetMiscComponents", "MiscObject", papyrusMiscObject::GetMiscComponents, vm));

	vm->RegisterFunction(
		new NativeFunction1 <TESObjectMISC, void, VMArray<MiscComponent>>("SetMiscComponents", "MiscObject", papyrusMiscObject::SetMiscComponents, vm));
}
