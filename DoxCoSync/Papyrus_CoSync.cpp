// Papyrus_CoSync.cpp
// Implements the EXACT native contract from:
//   Scriptname CoSync Native Hidden
// Do NOT change the PSC. This file matches it 1:1.

#include "Papyrus_CoSync.h"


#include "PapyrusVM.h"
#include "PapyrusNativeFunctions.h"
#include "PapyrusArgs.h"          // VMArray, BSFixedString helpers/types
#include "GameForms.h"            // TESForm, TESObjectCELL (Cell)
#include "GameReferences.h"       // TESObjectREFR (ObjectReference), Actor
#include "GameObjects.h"          // ActorBase
#include "ConsoleLogger.h"
#include "Logger.h"
#include "CoSyncNet.h"            // CoSyncNet()
#include "GNS_Session.h"

#include <cmath>                  // atan2f

// ----------------------------------------------------------------------------
// Notes on types
// ----------------------------------------------------------------------------
// In Papyrus, "Action" may be a game form type or a script object type depending
// on your codebase. For a compile-first stub (and to satisfy the PSC contract),
// we expose it as TESForm*.
// If later you determine Action is a specific class with a real kTypeID,
// change the return type + implementation accordingly.
using PapyrusActionType = TESForm*;

// ----------------------------------------------------------------------------
// Minimal stub implementations (safe defaults)
// Add Console_Print to confirm calls are actually hitting C++.
// ----------------------------------------------------------------------------

static SInt32 CoSync_GetClientInstanceID(StaticFunctionTag*)
{
    // For now: use lower 32 bits of our ID as a “client instance id”
    
    
	return 0;
    
}


static void CoSync_SetClient(StaticFunctionTag*, SInt32 instanceID)
{
    Console_Print("[CoSync] SetClient(%d) (stub)", instanceID);
}



static bool CoSync_IsConnected(StaticFunctionTag*)
{
    const bool connected = CoSyncNet::IsConnected();
    Console_Print("[CoSync] IsConnected() -> %s", connected ? "true" : "false");
	LOG_DEBUG("[Papy-CoSync] IsConnected() -> %s", connected ? "true" : "false");
    return connected;
}



static bool CoSync_Connect(
    StaticFunctionTag*,
    Actor* player,
    TESNPC* playerActorBase,
    BSFixedString address,
    SInt32 port)

{
    Console_Print("[CoSync] Connect(player=%p base=%p addr=%s port=%d) -> false (stub)",
        player, playerActorBase, address.c_str(), port);

    LOG_DEBUG("[Papy-CoSync] Connect(player=%p base=%p addr=%s port=%d) -> false (stub)",
		player, playerActorBase, address.c_str(), port);
    return false;
}

static bool CoSync_Disconnect(StaticFunctionTag*)
{
	
    Console_Print("[CoSync] Disconnect() -> true (stub)");
    return true;
}

static void CoSync_Tick(StaticFunctionTag*)
{
    // Keep quiet by default; uncomment if you want spam.
    // Console_Print("[CoSync] Tick() (stub)");
}

static void CoSync_SyncWorld(StaticFunctionTag*)
{
    Console_Print("[CoSync] SyncWorld() (stub)");
}

static SInt32 CoSync_GetPlayerEntityID(StaticFunctionTag*)
{
    Console_Print("[CoSync] GetPlayerEntityID() -> -1 (stub)");
    return -1;
}

static SInt32 CoSync_GetEntityID(StaticFunctionTag*, TESObjectREFR* ref)
{
    Console_Print("[CoSync] GetEntityID(ref=%p) -> -1 (stub)", ref);
    return -1;
}

static void CoSync_SetEntityRef(StaticFunctionTag*, SInt32 entityID, TESObjectREFR* ref)
{
    Console_Print("[CoSync] SetEntityRef(id=%d ref=%p) (stub)", entityID, ref);
}

static bool CoSync_IsEntityValid(StaticFunctionTag*, SInt32 entityID)
{
    const bool ok = (entityID >= 0);
    Console_Print("[CoSync] IsEntityValid(%d) -> %s (stub)", entityID, ok ? "true" : "false");
    return ok;
}

static VMArray<float> CoSync_GetEntityPosition(StaticFunctionTag*, SInt32 entityID)
{
    Console_Print("[CoSync] GetEntityPosition(%d) -> [0,0,0] (stub)", entityID);

    VMArray<float> arr;
    float z = 0.0f;
    arr.Push(&z);
    arr.Push(&z);
    arr.Push(&z);
    return arr;
}

static void CoSync_SetEntityPosition(StaticFunctionTag*, SInt32 entityID, float x, float y, float z)
{
    Console_Print("[CoSync] SetEntityPosition(id=%d x=%.3f y=%.3f z=%.3f) (stub)", entityID, x, y, z);
}

static void CoSync_SetEntVarNum(StaticFunctionTag*, SInt32 entityID, BSFixedString name, float value)
{
    Console_Print("[CoSync] SetEntVarNum(id=%d name=%s val=%.3f) (stub)", entityID, name.c_str(), value);
}

static void CoSync_SetEntVarAnim(StaticFunctionTag*, SInt32 entityID, BSFixedString animState)
{
    Console_Print("[CoSync] SetEntVarAnim(id=%d anim=%s) (stub)", entityID, animState.c_str());
}

static float CoSync_GetEntVarNum(StaticFunctionTag*, SInt32 entityID, BSFixedString name)
{
    Console_Print("[CoSync] GetEntVarNum(id=%d name=%s) -> 0 (stub)", entityID, name.c_str());
    return 0.0f;
}

static BSFixedString CoSync_GetEntVarAnim(StaticFunctionTag*, SInt32 entityID)
{
    Console_Print("[CoSync] GetEntVarAnim(id=%d) -> \"\" (stub)", entityID);
    return BSFixedString("");
}

static VMArray<TESObjectREFR*> CoSync_GetRefsInCell(StaticFunctionTag*, TESObjectCELL* cell)
{
    Console_Print("[CoSync] GetRefsInCell(cell=%p) -> [] (stub)", cell);

    VMArray<TESObjectREFR*> arr;
    return arr;
}

static float CoSync_Atan2(StaticFunctionTag*, float y, float x)
{
    const float r = atan2f(y, x);
    // Don’t spam by default.
    return r;
}

static BSFixedString CoSync_GetWalkDir(StaticFunctionTag*, float dX, float dY, float angleZ)
{
    // Stub: return empty. You can implement your F4MP logic later.
    (void)dX; (void)dY; (void)angleZ;
    Console_Print("[CoSync] GetWalkDir(dx=%.3f dy=%.3f ang=%.3f) -> \"\" (stub)", dX, dY, angleZ);
    return BSFixedString("");
}

static PapyrusActionType CoSync_GetAction(StaticFunctionTag*, BSFixedString name)
{
    // Stub: return None (null)
    Console_Print("[CoSync] GetAction(%s) -> None (stub)", name.c_str());
    return nullptr;
}

static void CoSync_Inspect(StaticFunctionTag*, VMArray<TESForm*> forms)
{
    Console_Print("[CoSync] Inspect(forms=%u) (stub)", forms.Length());
}

static bool CoSync_AnimLoops(StaticFunctionTag*, BSFixedString animState)
{
    Console_Print("[CoSync] AnimLoops(%s) -> false (stub)", animState.c_str());
    return false;
}

static void CoSync_CopyAppearance(StaticFunctionTag*, TESNPC* src, TESNPC* dest)
{
    Console_Print("[CoSync] CopyAppearance(src=%p dest=%p) (stub)", src, dest);
}

static void CoSync_CopyWornItems(StaticFunctionTag*, Actor* src, Actor* dest)
{
    Console_Print("[CoSync] CopyWornItems(src=%p dest=%p) (stub)", src, dest);
}

static void CoSync_PlayerHit(StaticFunctionTag*, SInt32 hitter, SInt32 hittee, float damage)
{
    Console_Print("[CoSync] PlayerHit(hitter=%d hittee=%d dmg=%.3f) (stub)", hitter, hittee, damage);
}

static void CoSync_PlayerFireWeapon(StaticFunctionTag*)
{
    Console_Print("[CoSync] PlayerFireWeapon() (stub)");
}

static void CoSync_TopicInfoBegin(StaticFunctionTag*, TESForm* info, TESObjectREFR* speaker)
{
    Console_Print("[CoSync] TopicInfoBegin(info=%p speaker=%p) (stub)", info, speaker);
}

static VMArray<TESObjectREFR*> CoSync_GetEntitySyncRefs(StaticFunctionTag*, bool clear)
{
    Console_Print("[CoSync] GetEntitySyncRefs(clear=%s) -> [] (stub)", clear ? "true" : "false");
    VMArray<TESObjectREFR*> arr;
    return arr;
}

static VMArray<float> CoSync_GetEntitySyncTransforms(StaticFunctionTag*, bool clear)
{
    Console_Print("[CoSync] GetEntitySyncTransforms(clear=%s) -> [] (stub)", clear ? "true" : "false");
    VMArray<float> arr;
    return arr;
}

// ----------------------------------------------------------------------------
// Registration (must register ALL natives declared in CoSync.psc)
// ----------------------------------------------------------------------------
bool RegisterCoSyncPapyrus(VirtualMachine* vm)
{
    if (!vm)
        return false;

    constexpr const char* kClass = "CoSync";

    // int Function GetClientInstanceID() global native
    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, SInt32>(
        "GetClientInstanceID", kClass, CoSync_GetClientInstanceID, vm));

    // Function SetClient(int instanceID) global native
    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, void, SInt32>(
        "SetClient", kClass, CoSync_SetClient, vm));

    // bool Function IsConnected() global native
    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, bool>(
        "IsConnected", kClass, CoSync_IsConnected, vm));

    // bool Function Connect(Actor player, ActorBase playerActorBase, string address, int port) global native
    vm->RegisterFunction(new NativeFunction4<StaticFunctionTag, bool, Actor*, TESNPC*, BSFixedString, SInt32>(
        "Connect", kClass, CoSync_Connect, vm));

    // bool Function Disconnect() global native
    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, bool>(
        "Disconnect", kClass, CoSync_Disconnect, vm));

    // Function Tick() global native
    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, void>(
        "Tick", kClass, CoSync_Tick, vm));

    // Function SyncWorld() global native
    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, void>(
        "SyncWorld", kClass, CoSync_SyncWorld, vm));

    // int Function GetPlayerEntityID() global native
    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, SInt32>(
        "GetPlayerEntityID", kClass, CoSync_GetPlayerEntityID, vm));

    // int Function GetEntityID(ObjectReference ref) global native
    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, SInt32, TESObjectREFR*>(
        "GetEntityID", kClass, CoSync_GetEntityID, vm));

    // Function SetEntityRef(int entityID, ObjectReference ref) global native
    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, void, SInt32, TESObjectREFR*>(
        "SetEntityRef", kClass, CoSync_SetEntityRef, vm));

    // bool Function IsEntityValid(int entityID) global native
    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, bool, SInt32>(
        "IsEntityValid", kClass, CoSync_IsEntityValid, vm));

    // float[] Function GetEntityPosition(int entityID) global native
    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, VMArray<float>, SInt32>(
        "GetEntityPosition", kClass, CoSync_GetEntityPosition, vm));

    // Function SetEntityPosition(int entityID, float x, float y, float z) global native
    vm->RegisterFunction(new NativeFunction4<StaticFunctionTag, void, SInt32, float, float, float>(
        "SetEntityPosition", kClass, CoSync_SetEntityPosition, vm));

    // Function SetEntVarNum(int entityID, string name, float value) global native
    vm->RegisterFunction(new NativeFunction3<StaticFunctionTag, void, SInt32, BSFixedString, float>(
        "SetEntVarNum", kClass, CoSync_SetEntVarNum, vm));

    // Function SetEntVarAnim(int entityID, string animState) global native
    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, void, SInt32, BSFixedString>(
        "SetEntVarAnim", kClass, CoSync_SetEntVarAnim, vm));

    // float Function GetEntVarNum(int entityID, string name) global native
    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, float, SInt32, BSFixedString>(
        "GetEntVarNum", kClass, CoSync_GetEntVarNum, vm));

    // string Function GetEntVarAnim(int entityID) global native
    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, BSFixedString, SInt32>(
        "GetEntVarAnim", kClass, CoSync_GetEntVarAnim, vm));

    // ObjectReference[] Function GetRefsInCell(Cell cell) global native
    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, VMArray<TESObjectREFR*>, TESObjectCELL*>(
        "GetRefsInCell", kClass, CoSync_GetRefsInCell, vm));

    // float Function Atan2(float y, float x) global native
    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, float, float, float>(
        "Atan2", kClass, CoSync_Atan2, vm));

    // string Function GetWalkDir(float dX, float dY, float angleZ) global native
    vm->RegisterFunction(new NativeFunction3<StaticFunctionTag, BSFixedString, float, float, float>(
        "GetWalkDir", kClass, CoSync_GetWalkDir, vm));

    // Action Function GetAction(string name) global native
    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, PapyrusActionType, BSFixedString>(
        "GetAction", kClass, CoSync_GetAction, vm));

    // Function Inspect(Form[] forms) global native
    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, void, VMArray<TESForm*>>(
        "Inspect", kClass, CoSync_Inspect, vm));

    // bool Function AnimLoops(string animState) global native
    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, bool, BSFixedString>(
        "AnimLoops", kClass, CoSync_AnimLoops, vm));

    // Function CopyAppearance(ActorBase src, ActorBase dest) global native
    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, void, TESNPC*, TESNPC*>(
        "CopyAppearance", kClass, CoSync_CopyAppearance, vm));

    // Function CopyWornItems(Actor src, Actor dest) global native
    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, void, Actor*, Actor*>(
        "CopyWornItems", kClass, CoSync_CopyWornItems, vm));

    // Function PlayerHit(int hitter, int hittee, float damage) global native
    vm->RegisterFunction(new NativeFunction3<StaticFunctionTag, void, SInt32, SInt32, float>(
        "PlayerHit", kClass, CoSync_PlayerHit, vm));

    // Function PlayerFireWeapon() global native
    vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, void>(
        "PlayerFireWeapon", kClass, CoSync_PlayerFireWeapon, vm));

    // Function TopicInfoBegin(Form info, ObjectReference speaker) global native
    vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, void, TESForm*, TESObjectREFR*>(
        "TopicInfoBegin", kClass, CoSync_TopicInfoBegin, vm));

    // ObjectReference[] Function GetEntitySyncRefs(bool clear) global native
    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, VMArray<TESObjectREFR*>, bool>(
        "GetEntitySyncRefs", kClass, CoSync_GetEntitySyncRefs, vm));

    // float[] Function GetEntitySyncTransforms(bool clear) global native
    vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, VMArray<float>, bool>(
        "GetEntitySyncTransforms", kClass, CoSync_GetEntitySyncTransforms, vm));

    // Flags: only set NoWait if you are 100% sure the function is thread-safe.
    // For now, keep it conservative. You can loosen later.
    vm->SetFunctionFlags(kClass, "GetClientInstanceID", IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "IsConnected", IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "GetPlayerEntityID", IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "IsEntityValid", IFunction::kFunctionFlag_NoWait);
    vm->SetFunctionFlags(kClass, "Atan2", IFunction::kFunctionFlag_NoWait);

    LOG_INFO("Papyrus_CoSync: Registered FULL CoSync native contract (stubs).");
    return true;
}
