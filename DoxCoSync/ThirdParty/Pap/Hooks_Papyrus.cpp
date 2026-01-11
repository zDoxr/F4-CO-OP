#include "Hooks_Papyrus.h"
#include "Relocation.h"
#include "BranchTrampoline.h"

#include "PapyrusVM.h"
#include "PapyrusEvents.h"

#include "PapyrusF4SE.h"
#include "PapyrusForm.h"
#include "PapyrusMath.h"
#include "PapyrusActor.h"
#include "PapyrusActorBase.h"
#include "PapyrusHeadPart.h"
#include "PapyrusObjectReference.h"
#include "PapyrusGame.h"
#include "PapyrusScriptObject.h"
#include "PapyrusDelayFunctors.h"
#include "PapyrusUtility.h"
#include "PapyrusUI.h"
#include "PapyrusEquipSlot.h"
#include "PapyrusWeapon.h"
#include "PapyrusObjectMod.h"
#include "PapyrusInstanceData.h"
#include "PapyrusWaterType.h"
#include "PapyrusCell.h"
#include "PapyrusPerk.h"
#include "PapyrusLocation.h"
#include "PapyrusEncounterZone.h"
#include "PapyrusInput.h"
#include "PapyrusDefaultObject.h"
#include "PapyrusConstructibleObject.h"
#include "PapyrusComponent.h"
#include "PapyrusMiscObject.h"
#include "PapyrusMaterialSwap.h"
#include "PapyrusFavoritesManager.h"
#include "PapyrusArmorAddon.h"
#include "PapyrusArmor.h"

#include "Serialization.h"

#include "xbyak.h"

RelocAddr <uintptr_t> RegisterPapyrusFunctions_Start(0x0113E770 + 0x461);
RelocAddr <uintptr_t> DelayFunctorQueue_Start(0x010F0300 + 0x66);

typedef bool (* _SaveRegistrationHandles)(void * unk1, void * vm, void * handleReaderWriter, void * saveStorageWrapper);
RelocAddr <_SaveRegistrationHandles> SaveRegistrationHandles(0x011A09A0);
_SaveRegistrationHandles SaveRegistrationHandles_Original = nullptr;

typedef bool (* _LoadRegistrationHandles)(void * unk1, void * vm, void * handleReaderWriter, UInt16 version, void * loadStorageWrapper, void * unk2);
RelocAddr <_LoadRegistrationHandles> LoadRegistrationHandles(0x011A0B20);
_LoadRegistrationHandles LoadRegistrationHandles_Original = nullptr;

typedef void (* _RevertGlobalData)(void * vm);
RelocAddr <_RevertGlobalData> RevertGlobalData(0x010EE540);
_RevertGlobalData RevertGlobalData_Original = nullptr;

typedef std::list <F4SEPapyrusInterface::RegisterFunctions> PapyrusPluginList;
static PapyrusPluginList s_pap_plugins;

bool RegisterPapyrusPlugin(F4SEPapyrusInterface::RegisterFunctions callback)
{
	s_pap_plugins.push_back(callback);
	return true;
}

void GetExternalEventRegistrations(const char * eventName, void * data, F4SEPapyrusInterface::RegistrantFunctor functor)
{
	g_externalEventRegs.ForEach(BSFixedString(eventName), [&functor, &data](const EventRegistration<ExternalEventParameters> & reg)
	{
		functor(reg.handle, reg.scriptName.c_str(), reg.params.callbackName.c_str(), data);
	});
}

void Hooks_Papyrus_Init()
{

}

void RegisterPapyrusFunctions_Hook(VirtualMachine ** vmPtr)
{
	_MESSAGE("RegisterPapyrusFunctions_Hook");

	VirtualMachine * vm = (*vmPtr);

	// ScriptObject
	papyrusScriptObject::RegisterFuncs(vm);

	// EquipSlot
	papyrusEquipSlot::RegisterFuncs(vm);

	// WaterType
	papyrusWaterType::RegisterFuncs(vm);

	// MaterialSwap
	papyrusMaterialSwap::RegisterFuncs(vm);

	// F4SE
	papyrusF4SE::RegisterFuncs(vm);

	// Math
	papyrusMath::RegisterFuncs(vm);

	// Form
	papyrusForm::RegisterFuncs(vm);

	// ObjectReference
	papyrusObjectReference::RegisterFuncs(vm);

	// ActorBase
	papyrusActorBase::RegisterFuncs(vm);

	// Actor
	papyrusActor::RegisterFuncs(vm);

	// HeadPart
	papyrusHeadPart::RegisterFuncs(vm);

	// Game
	papyrusGame::RegisterFuncs(vm);

	// Utility
	papyrusUtility::RegisterFuncs(vm);

	// UI
	papyrusUI::RegisterFuncs(vm);

	// Weapon
	papyrusWeapon::RegisterFuncs(vm);

	// ObjectMod
	papyrusObjectMod::RegisterFuncs(vm);

	// InstanceData
	papyrusInstanceData::RegisterFuncs(vm);

	// Cell
	papyrusCell::RegisterFuncs(vm);

	// Perk
	papyrusPerk::RegisterFuncs(vm);

	// EncounterZone
	papyrusEncounterZone::RegisterFuncs(vm);

	// Location
	papyrusLocation::RegisterFuncs(vm);

	// Input
	papyrusInput::RegisterFuncs(vm);

	// DefaultObject
	papyrusDefaultObject::RegisterFuncs(vm);

	// ConstructibleObject
	papyrusConstructibleObject::RegisterFuncs(vm);

	// Component
	papyrusComponent::RegisterFuncs(vm);

	// MiscObject
	papyrusMiscObject::RegisterFuncs(vm);

	// FavoritesManager
	papyrusFavoritesManager::RegisterFuncs(vm);

	// ArmorAddon
	papyrusArmorAddon::RegisterFuncs(vm);

	// Armor
	papyrusArmor::RegisterFuncs(vm);

	GetEventDispatcher<TESFurnitureEvent>()->AddEventSink(&g_furnitureEventSink);

	// Plugins
	for(PapyrusPluginList::iterator iter = s_pap_plugins.begin(); iter != s_pap_plugins.end(); ++iter)
	{
		(*iter)(vm);
	}
}

bool SaveRegistrationHandles_Hook(void * unk1, void * vm, void * handleReaderWriter, void * loadStorageWrapper)
{
	bool ret = SaveRegistrationHandles_Original(unk1, vm, handleReaderWriter, loadStorageWrapper);
	Serialization::HandleSaveGlobalData();
	return ret;
}

bool LoadRegistrationHandles_Hook(void * unk1, void * vm, void * handleReaderWriter, UInt16 version, void * loadStorageWrapper, void * unk2)
{
	bool ret = LoadRegistrationHandles_Original(unk1, vm, handleReaderWriter, version, loadStorageWrapper, unk2);
	Serialization::HandleLoadGlobalData();
	return ret;
}

void RevertGlobalData_Hook(void * vm)
{
	RevertGlobalData_Original(vm);
	Serialization::HandleRevertGlobalData();
}

LONGLONG DelayFunctorQueue_Hook(float budget)
{
	F4SEDelayFunctorManagerInstance().OnPreTick();

	LARGE_INTEGER startTime;
	QueryPerformanceCounter(&startTime);

	// Sharing budget with papyrus queue
	F4SEDelayFunctorManagerInstance().OnTick(startTime.QuadPart, budget);

	return startTime.QuadPart;
}

void Hooks_Papyrus_Commit()
{
	struct RegisterPapyrusFunctions_Code : Xbyak::CodeGenerator {
		RegisterPapyrusFunctions_Code(void * buf) : Xbyak::CodeGenerator(4096, buf)
		{
			call((void *)&RegisterPapyrusFunctions_Hook);
			mov(rcx, rbx);
			add(rsp, 0x20);
			pop(rbx);
			ret();
		}
	};

	void * codeBuf = g_localTrampoline.StartAlloc();
	RegisterPapyrusFunctions_Code code(codeBuf);
	g_localTrampoline.EndAlloc(code.getCurr());

	g_branchTrampoline.Write6Branch(RegisterPapyrusFunctions_Start, uintptr_t(code.getCode()));

	// save registration handles hook
	{
		struct SaveRegistrationHandles_Code : Xbyak::CodeGenerator {
			SaveRegistrationHandles_Code(void * buf) : Xbyak::CodeGenerator(4096, buf)
			{
				Xbyak::Label retnLabel;

				mov(rax, rsp);
				push(rdi);
				push(r13);

				jmp(ptr [rip + retnLabel]);

				L(retnLabel);
				dq(SaveRegistrationHandles.GetUIntPtr() + 6);
			}
		};

		void * codeBuf = g_localTrampoline.StartAlloc();
		SaveRegistrationHandles_Code code(codeBuf);
		g_localTrampoline.EndAlloc(code.getCurr());

		SaveRegistrationHandles_Original = (_SaveRegistrationHandles)codeBuf;

		g_branchTrampoline.Write6Branch(SaveRegistrationHandles.GetUIntPtr(), (uintptr_t)SaveRegistrationHandles_Hook);
	}


	// load registration handles hook
	{
		struct LoadRegistrationHandles_Code : Xbyak::CodeGenerator {
			LoadRegistrationHandles_Code(void * buf) : Xbyak::CodeGenerator(4096, buf)
			{
				Xbyak::Label retnLabel;

				mov(rax, rsp);
				push(rdi);
				push(r13);

				jmp(ptr [rip + retnLabel]);

				L(retnLabel);
				dq(LoadRegistrationHandles.GetUIntPtr() + 6);
			}
		};

		void * codeBuf = g_localTrampoline.StartAlloc();
		LoadRegistrationHandles_Code code(codeBuf);
		g_localTrampoline.EndAlloc(code.getCurr());

		LoadRegistrationHandles_Original = (_LoadRegistrationHandles)codeBuf;

		g_branchTrampoline.Write6Branch(LoadRegistrationHandles.GetUIntPtr(), (uintptr_t)LoadRegistrationHandles_Hook);
	}
	
	// revert global data hook
	{
		struct RevertGlobalData_Code : Xbyak::CodeGenerator {
			RevertGlobalData_Code(void * buf) : Xbyak::CodeGenerator(4096, buf)
			{
				Xbyak::Label retnLabel;

				mov(ptr [rsp + 0x10], rbx);

				jmp(ptr [rip + retnLabel]);

				L(retnLabel);
				dq(RevertGlobalData.GetUIntPtr() + 5);
			}
		};

		void * codeBuf = g_localTrampoline.StartAlloc();
		RevertGlobalData_Code code(codeBuf);
		g_localTrampoline.EndAlloc(code.getCurr());

		RevertGlobalData_Original = (_RevertGlobalData)codeBuf;

		g_branchTrampoline.Write5Branch(RevertGlobalData.GetUIntPtr(), (uintptr_t)RevertGlobalData_Hook);
	}

	// hook ProcessVMTick
	{
		struct DelayFunctorQueue_Code : Xbyak::CodeGenerator {
			DelayFunctorQueue_Code(void * buf, uintptr_t funcAddr) : Xbyak::CodeGenerator(4096, buf)
			{
				Xbyak::Label funcLabel;
				Xbyak::Label retnLabel;

				movss(xmm0, ptr[rsp + 0x98 + 0x28]);
				call(ptr [rip + funcLabel]);
				jmp(ptr [rip + retnLabel]);

				L(funcLabel);
				dq(funcAddr);

				L(retnLabel);
				dq(DelayFunctorQueue_Start.GetUIntPtr() + 0x5);
			}
		};

		void * codeBuf = g_localTrampoline.StartAlloc();
		DelayFunctorQueue_Code code(codeBuf, (uintptr_t)DelayFunctorQueue_Hook);
		g_localTrampoline.EndAlloc(code.getCurr());

		g_branchTrampoline.Write5Branch(DelayFunctorQueue_Start.GetUIntPtr(), uintptr_t(code.getCode()));
	}
}
