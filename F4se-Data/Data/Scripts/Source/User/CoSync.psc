Scriptname CoSync Native Hidden

; ==============================================================================
; This script must EXACTLY match the native functions in Papyrus_CoSync.cpp
; Signatures must match or functions won't bind!
; ==============================================================================

; ==============================================================================
; CONNECTION & SESSION MANAGEMENT
; ==============================================================================

int Function GetClientInstanceID() global native
Function SetClient(int instanceID) global native

bool Function IsConnected() global native
bool Function Connect(Actor player, ActorBase playerActorBase, string address, int port) global native
bool Function Disconnect() global native
Function Tick() global native
Function SyncWorld() global native

; ==============================================================================
; ENTITY MANAGEMENT
; ==============================================================================

int Function GetPlayerEntityID() global native

; C++ signature: NativeFunction1<StaticFunctionTag, SInt32, TESObjectREFR*>
int Function GetEntityID(ObjectReference ref) global native

; C++ signature: NativeFunction2<StaticFunctionTag, void, SInt32, TESObjectREFR*>
Function SetEntityRef(int entityID, ObjectReference ref) global native

bool Function IsEntityValid(int entityID) global native

; C++ signature: NativeFunction0<StaticFunctionTag, VMArray<SInt32>>
int[] Function GetAllEntityIDs() global native

; C++ signature: NativeFunction1<StaticFunctionTag, TESObjectREFR*, SInt32>
; IMPORTANT: Returns ObjectReference, NOT Actor!
ObjectReference Function GetActorByEntityID(int entityID) global native

; C++ signature: NativeFunction1<StaticFunctionTag, SInt32, TESObjectREFR*>
; IMPORTANT: Takes ObjectReference, NOT Actor!
int Function GetEntityIDForActor(ObjectReference akRef) global native

; ==============================================================================
; ENTITY POSITION & STATE
; ==============================================================================

float[] Function GetEntityPosition(int entityID) global native
Function SetEntityPosition(int entityID, float x, float y, float z) global native
Function SetEntVarNum(int entityID, string name, float value) global native
Function SetEntVarAnim(int entityID, string animState) global native
float Function GetEntVarNum(int entityID, string name) global native
string Function GetEntVarAnim(int entityID) global native



; ==============================================================================
; UTILITY FUNCTIONS
; ==============================================================================

ObjectReference[] Function GetRefsInCell(Cell cell) global native
float Function Atan2(float y, float x) global native
string Function GetWalkDir(float dX, float dY, float angleZ) global native

; C++ signature: NativeFunction1<StaticFunctionTag, PapyrusActionType, BSFixedString>
; PapyrusActionType is TESForm* in C++, so use Form in Papyrus
Form Function GetAction(string name) global native

Function Inspect(Form[] forms) global native
bool Function AnimLoops(string animState) global native

; ==============================================================================
; ACTOR CUSTOMIZATION
; ==============================================================================

Function CopyAppearance(ActorBase src, ActorBase dest) global native
Function CopyWornItems(Actor src, Actor dest) global native

; ==============================================================================
; COMBAT & ACTIONS
; ==============================================================================

Function PlayerHit(int hitter, int hittee, float damage) global native
Function PlayerFireWeapon() global native
Function TopicInfoBegin(Form info, ObjectReference speaker) global native

; ==============================================================================
; SYNCHRONIZATION
; ==============================================================================

ObjectReference[] Function GetEntitySyncRefs(bool clear) global native
float[] Function GetEntitySyncTransforms(bool clear) global native

; ==============================================================================
; NPC SPAWNING
; ==============================================================================

bool Function SpawnNpc(ObjectReference ref) global native

; ==============================================================================
; WORLD / WEATHER / WORKSHOP SYNC
; Called from CoSyncQuest.psc when game events fire.
; ==============================================================================

bool Function IsHost() global native

; Returns the weather formID locked from the host (0 = none / not connected).
; Called from the CoSyncQuest weather timer on the CLIENT to apply host weather
; via w.ForceActive(True) — avoids unreliable C++→Papyrus CallFunctionNoWait.
int Function GetPendingWeatherFormID() global native

; Pass Weather.GetCurrentWeather() cast as Form
Function NotifyWeatherChanged(Form akWeather) global native

; Workshop build: pass the workshop marker ref and the newly placed ref
Function NotifyWorkshopBuild(ObjectReference akWorkshop, ObjectReference akPlaced) global native

; Workshop destroy: pass the workshop marker ref and the scrapped ref
Function NotifyWorkshopDestroy(ObjectReference akWorkshop, ObjectReference akScrapped) global native

; Workshop power toggle: pass the power-switch ref and new powered state
Function NotifyWorkshopPower(ObjectReference akRef, bool isPowered) global native

; Workshop mode enter/exit — call with the actual workshop ref (fixes formID=0 from C++ side)
Function NotifyWorkshopMode(ObjectReference akWorkshop, bool isEntering) global native

; ==============================================================================
; QUEST / MAP MARKER SYNC
; ==============================================================================

Function NotifyQuestStage(Quest akQuest, int aiStage) global native
Function NotifyMapMarkerDiscovered(ObjectReference akMarker) global native

; ==============================================================================
; POWER ARMOR SYNC
; ==============================================================================

Function NotifyPowerArmor(Actor akActor, ObjectReference akPowerArmor, bool abEntering) global native

; ==============================================================================
; CONSUMABLE / MAGIC SYNC
; ==============================================================================

; Call when the local player uses an AlchemyItem (stimpaks, Rad-Away, etc.)
Function NotifyConsumableUsed(Form akItem) global native

; Call when a spell or persistent magic effect is applied to the local player
Function NotifySpellApplied(Form akSpell) global native

; ==============================================================================
; PIP-BOY SYNC
; ==============================================================================

; Call when the Pip-boy menu opens (abOpening=true) or closes (abOpening=false)
Function NotifyPipboyOpen(bool abOpening) global native

; ==============================================================================
; COMPANION SYNC
; ==============================================================================

Function NotifyCompanionRecruited(Actor akCompanion) global native
Function NotifyCompanionDismissed(Actor akCompanion) global native

; ==============================================================================
; DOOR / ACTIVATABLE STATE SYNC
; ==============================================================================

; Returns true if akRef's base form is a DOOR (formType 32).
; FO4 Papyrus has no ObjectReference.GetFormType() — this native fills the gap.
bool Function IsDoor(ObjectReference akRef) global native

; Call when the local player activates a DOOR ref.
; aiOpenState = akDoorRef.GetOpenState() — 0=None, 1=Open, 2=Opening, 3=Closed, 4=Closing
; C++ applies a suppression guard so network-applied activations do not echo back.
Function NotifyActivate(ObjectReference akRef, int aiOpenState) global native

