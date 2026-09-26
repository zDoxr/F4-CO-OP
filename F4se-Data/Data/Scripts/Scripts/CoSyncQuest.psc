Scriptname CoSyncQuest extends Quest

; ==============================================================================
; CoSyncQuest — wires Papyrus game events to CoSync native sync functions.
; Animation control is handled entirely in C++ via behavior graph injection.
; ==============================================================================

; Timer IDs
int tickTimerID        = 10
int updateTimerID      = 20
int weatherTimerID     = 30
int doorScanTimerID    = 40
int companionTimerID   = 50

; Properties
Actor       Property playerRef  Auto
ActorValue  Property healthAV   Auto

; Weather state (host-side change detection)
Weather lastWeather = None

; Client-side: the last host weather formID we actually forced. ForceActive
; restarts a full sky transition, so it must fire ONCE per weather change --
; forcing it every poll re-rendered the sky on a 5s loop.
Int lastAppliedWeatherID = 0

; Power armor state — tracks whether the player is currently in PA so that
; Actor.OnGetUp can distinguish a PA exit from sitting in regular furniture.
bool bIsInPowerArmor = False

; Companion poll state — snapshot of followers from last poll cycle.
; Compared against Game.GetCurrentFollowers() each tick to detect
; recruit and dismiss events without requiring a dedicated event sink.
Actor[] trackedFollowers


; ==============================================================================
; INITIALIZATION
; ==============================================================================

; Shared setup — called from both OnInit (new game) and OnPlayerLoadGame (save load).
Function SetupEventHooks()
    ; ---- Workshop events -------------------------------------------------------
    WorkshopParentScript wsParent = Game.GetFormFromFile(0xD62E, "Fallout4.esm") as WorkshopParentScript
    If wsParent != None
        RegisterForCustomEvent(wsParent, "WorkshopObjectBuilt")
        RegisterForCustomEvent(wsParent, "WorkshopObjectDestroyed")
        RegisterForCustomEvent(wsParent, "WorkshopEnterMenu")
        Debug.Trace("[CoSyncQuest] Registered for WorkshopParent build/destroy/enter events")
    Else
        Debug.Trace("[CoSyncQuest] WARNING: WorkshopParent (0xD62E) not found")
    EndIf

    ; Workshop exit: detect when WorkshopMenu HUD closes
    RegisterForMenuOpenCloseEvent("WorkshopMenu")

    ; ---- Weather polling timer (cancel first to avoid duplicates) --------------
    CancelTimer(weatherTimerID)
    StartTimer(5.0, weatherTimerID)
    Debug.Trace("[CoSyncQuest] Weather timer started")

    ; ---- Map marker discovery — register for player location changes ----------
    ; Only the host broadcasts discoveries; clients receive via MM| packet.
    If CoSync.IsHost()
        RegisterForRemoteEvent(playerRef, "OnLocationChange")
        Debug.Trace("[CoSyncQuest] Registered for OnLocationChange (map sync)")
    EndIf

    ; ---- Consumable use — fires when any item is equipped by the player -------
    ; Both host and client need this to broadcast their own consumable use.
    RegisterForRemoteEvent(playerRef, "OnItemEquipped")
    Debug.Trace("[CoSyncQuest] Registered for Actor.OnItemEquipped (consumable sync)")

    ; ---- Power armor enter / exit -------------------------------------------
    ; OnEnterPowerArmor / OnExitPowerArmor are F4SE extension events and cannot
    ; be used in vanilla Papyrus compilation. Instead we use Actor.OnSit /
    ; Actor.OnGetUp — power armor chassis is furniture the player sits in.
    ; IsInPowerArmor() on OnSit confirms it is a PA enter; a bool flag tracks
    ; state so OnGetUp can distinguish PA exit from regular furniture.
    RegisterForRemoteEvent(playerRef, "OnSit")
    RegisterForRemoteEvent(playerRef, "OnGetUp")
    Debug.Trace("[CoSyncQuest] Registered for OnSit / OnGetUp (PA enter/exit detection)")

    ; ---- Pip-boy menu open/close ---------------------------------------------
    ; Already registered for WorkshopMenu above; extend to PipboyMenu.
    RegisterForMenuOpenCloseEvent("PipboyMenu")
    Debug.Trace("[CoSyncQuest] Registered for PipboyMenu open/close")

    ; ---- Dialogue restriction: host-only conversation ------------------------
    ; User requirement: quest NPCs stay synced, but only the host may hold
    ; conversations (avoids two players independently advancing/derailing the
    ; same dialogue tree). Reactive close (let it open, then immediately close
    ; on the client) rather than trying to block Activate itself — UI.CloseMenu
    ; is a standard, always-safe vanilla call; a one-frame flicker is an
    ; acceptable tradeoff for not touching the Activate/dialogue engine path.
    RegisterForMenuOpenCloseEvent("DialogueMenu")
    Debug.Trace("[CoSyncQuest] Registered for DialogueMenu open/close (host-only conversation)")

    ; ---- Door activation sync ------------------------------------------------
    ; Scan the player's current cell for DOOR refs and register OnActivate on
    ; each.  The timer re-scans on cell transitions (every 10 s) to catch new
    ; doors after fast-travel or cell loads.
    ScanCellDoors()
    CancelTimer(doorScanTimerID)
    StartTimer(10.0, doorScanTimerID)
    Debug.Trace("[CoSyncQuest] Door scan started")

    ; ---- Companion recruit / dismiss poll ------------------------------------
    ; Game.GetCurrentFollowers() (F4SE) returns a live snapshot of all current
    ; followers.  We diff against trackedFollowers each cycle to detect changes.
    ; 3-second interval balances responsiveness against Papyrus VM overhead.
    CancelTimer(companionTimerID)
    StartTimer(3.0, companionTimerID)
    Debug.Trace("[CoSyncQuest] Companion poll started")
EndFunction

Event OnInit()
    ; Fires only once when the quest is first created (new game).
    ; For save loads, SetupEventHooks() is called via C++ NotifyGameLoaded().
    Debug.Trace("[CoSyncQuest] OnInit")
    SetupEventHooks()
EndEvent

; ==============================================================================
; TIMERS
; ==============================================================================

Event OnTimer(int aiTimerID)
    If aiTimerID == tickTimerID
        CoSync.Tick()
        StartTimer(0.016, tickTimerID)      ; ~60 fps

    ElseIf aiTimerID == updateTimerID
        int playerEntityID = CoSync.GetPlayerEntityID()
        If CoSync.IsEntityValid(playerEntityID)
            CoSync.SetEntVarNum(playerEntityID, "health", playerRef.GetValuePercentage(healthAV))
        EndIf
        StartTimer(0.1, updateTimerID)      ; 10 Hz

    ElseIf aiTimerID == weatherTimerID
        If CoSync.IsHost()
            ; HOST: detect weather changes and broadcast to clients
            Weather currentWeather = Weather.GetCurrentWeather()
            If currentWeather != lastWeather
                lastWeather = currentWeather
                If currentWeather != None
                    CoSync.NotifyWeatherChanged(currentWeather as Form)
                    Debug.Trace("[CoSyncQuest] Weather changed -> NotifyWeatherChanged")
                EndIf
            EndIf
        Else
            ; CLIENT: apply weather locked from host via native poll
            ; CoSync.GetPendingWeatherFormID() reads s_lockedWeatherFormID from C++
            ; (set when WX| packet is received). This avoids CallFunctionNoWait entirely.
            Int pendingFormID = CoSync.GetPendingWeatherFormID()
            If pendingFormID != 0 && pendingFormID != lastAppliedWeatherID
                ; Only force when the host's weather actually CHANGED. ForceActive
                ; restarts a full sky/lighting transition; calling it every 5s on
                ; the already-active weather re-rendered the world in a loop.
                Weather w = Game.GetForm(pendingFormID) as Weather
                If w != None
                    w.ForceActive(True)
                    lastAppliedWeatherID = pendingFormID
                    Debug.Trace("[CoSyncQuest] CLIENT Applied host weather 0x" + pendingFormID)
                Else
                    Debug.Trace("[CoSyncQuest] CLIENT host weather form 0x" + pendingFormID + " not found")
                EndIf
            EndIf
        EndIf
        StartTimer(5.0, weatherTimerID)

    ElseIf aiTimerID == doorScanTimerID
        ; Re-scan current cell for doors after potential cell transition.
        ; Registration is idempotent — calling RegisterForRemoteEvent on an
        ; already-registered ref is a no-op, so no unregister step needed.
        ScanCellDoors()
        StartTimer(10.0, doorScanTimerID)

    ElseIf aiTimerID == companionTimerID
        ; Poll for companion recruit / dismiss changes.
        ; Only the local player's own followers matter — each player broadcasts
        ; their own companion events to the network.
        PollCompanionChanges()
        StartTimer(3.0, companionTimerID)
    EndIf
EndEvent

; ==============================================================================
; WORKSHOP BUILD — fires when the local player places an object
; Arg layout: akArgs[0] = placed ObjectReference, akArgs[1] = WorkshopScript ref
; ==============================================================================

Event WorkshopParentScript.WorkshopObjectBuilt(WorkshopParentScript akSender, Var[] akArgs)
    If !CoSync.IsHost()
        Return
    EndIf
    If akArgs.Length < 2
        Return
    EndIf

    ObjectReference placedRef   = akArgs[0] as ObjectReference
    ObjectReference workshopRef = akArgs[1] as ObjectReference

    If workshopRef != None && placedRef != None
        CoSync.NotifyWorkshopBuild(workshopRef, placedRef)
        Debug.Trace("[CoSyncQuest] WorkshopObjectBuilt -> NotifyWorkshopBuild")
    EndIf
EndEvent

; ==============================================================================
; WORKSHOP DESTROY — fires when the local player scraps an object
; Arg layout: akArgs[0] = scrapped ObjectReference, akArgs[1] = WorkshopScript ref
; ==============================================================================

Event WorkshopParentScript.WorkshopObjectDestroyed(WorkshopParentScript akSender, Var[] akArgs)
    If !CoSync.IsHost()
        Return
    EndIf
    If akArgs.Length < 2
        Return
    EndIf

    ObjectReference scrappedRef  = akArgs[0] as ObjectReference
    ObjectReference workshopRef  = akArgs[1] as ObjectReference

    If workshopRef != None && scrappedRef != None
        CoSync.NotifyWorkshopDestroy(workshopRef, scrappedRef)
        Debug.Trace("[CoSyncQuest] WorkshopObjectDestroyed -> NotifyWorkshopDestroy")
    EndIf
EndEvent

; ==============================================================================
; WORKSHOP MODE — enter via WorkshopEnterMenu, exit via OnMenuOpenCloseEvent
; akArgs[0]=None, akArgs[1]=workshop ObjectReference (the workbench)
; Using Papyrus events bypasses the broken C++ LookupREFRByHandle RVA.
; ==============================================================================

Event WorkshopParentScript.WorkshopEnterMenu(WorkshopParentScript akSender, Var[] akArgs)
    If !CoSync.IsHost()
        Return
    EndIf
    If akArgs.Length < 2
        Return
    EndIf
    ObjectReference workshopRef = akArgs[1] as ObjectReference
    If workshopRef != None
        CoSync.NotifyWorkshopMode(workshopRef, true)
        Debug.Trace("[CoSyncQuest] WorkshopEnterMenu -> NotifyWorkshopMode ENTER")
    EndIf
EndEvent

Event OnMenuOpenCloseEvent(string asMenuName, bool abOpening)
    If asMenuName == "WorkshopMenu"
        If abOpening
            Return  ; handled by WorkshopEnterMenu (has the ref)
        EndIf
        If CoSync.IsHost()
            CoSync.NotifyWorkshopMode(None, false)
            Debug.Trace("[CoSyncQuest] WorkshopMenu closed -> NotifyWorkshopMode EXIT")
        EndIf

    ElseIf asMenuName == "PipboyMenu"
        ; Pip-boy open/close — sync to all clients so remote players can mirror
        ; the arm-raise animation and HUD state.
        CoSync.NotifyPipboyOpen(abOpening)
        Debug.Trace("[CoSyncQuest] PipboyMenu " + (abOpening as string) + " -> NotifyPipboyOpen")

    ElseIf asMenuName == "DialogueMenu"
        If abOpening && !CoSync.IsHost()
            UI.CloseMenu("DialogueMenu")
            Debug.Notification("Only the Host can talk to NPCs")
            Debug.Trace("[CoSyncQuest] DialogueMenu blocked on CLIENT (host-only conversation)")
        EndIf
    EndIf
EndEvent

; ==============================================================================
; CONSUMABLE USE SYNC
; Fires when the local player equips any item.  If it is an AlchemyItem
; (stimpak, Rad-Away, chem, etc.) we broadcast the use so remote actors
; can mirror the effect via EquipItem in C++.
; ==============================================================================

Event Actor.OnItemEquipped(Actor akActor, Form akBaseObject, ObjectReference akReference)
    ; Filter: only AlchemyItem (formType 46 = kFormType_ALCH)
    If akBaseObject == None
        Return
    EndIf
    If !(akBaseObject is Potion)
        Return
    EndIf
    CoSync.NotifyConsumableUsed(akBaseObject)
    Debug.Trace("[CoSyncQuest] OnItemEquipped(consumable) -> NotifyConsumableUsed 0x" + akBaseObject.GetFormID())
EndEvent

; ==============================================================================
; WEATHER FORCE — called from C++ on the client to apply host weather
; Receives the weather formID as an Int (from CoSyncTimeSync via CallFunctionNoWait).
; ==============================================================================

Function ForceHostWeather(Int weatherFormID)
    Weather w = Game.GetForm(weatherFormID) as Weather
    If w != None
        w.ForceActive(True)
        Debug.Trace("[CoSyncQuest] ForceHostWeather applied 0x" + weatherFormID)
    Else
        Debug.Trace("[CoSyncQuest] ForceHostWeather: form 0x" + weatherFormID + " not found or not Weather")
    EndIf
EndFunction

; ==============================================================================
; TIMER CONTROL
; ==============================================================================

Function StartTimers()
    StartTimer(0.016, tickTimerID)
    StartTimer(0.1,   updateTimerID)
    CancelTimer(weatherTimerID)
    StartTimer(5.0,   weatherTimerID)
    CancelTimer(doorScanTimerID)
    StartTimer(10.0,  doorScanTimerID)
    CancelTimer(companionTimerID)
    StartTimer(3.0,   companionTimerID)
    Debug.Trace("[CoSyncQuest] Timers started")
EndFunction

Function StopTimers()
    CancelTimer(tickTimerID)
    CancelTimer(updateTimerID)
    CancelTimer(weatherTimerID)
    CancelTimer(doorScanTimerID)
    CancelTimer(companionTimerID)
EndFunction

; ==============================================================================
; REMOTE PLAYER SPAWN / DESPAWN
; Called from C++ (CoSyncPapyrusHelper) via CallFunctionNoWait after the
; actor has been spawned and registered in CoSyncEntityRegistry.
; SpawnRemotePlayer handles any Papyrus-side post-spawn setup (enable, init).
; DespawnRemotePlayer cleans up the proxy actor on disconnect or entity destroy.
; ==============================================================================

; ==============================================================================
; PROXY ANIMATION DISPATCH
; Called from C++ via CallFunctionNoWait(questRef, "SendProxyAnimEvent", [formID, event])
;
; Routes a behavior graph animation event to a proxy actor WITHOUT requiring
; CoSyncPlayer.psc to be attached to the NPC form in the ESP.
; Game.GetForm() retrieves the dynamic (0xFF...) actor from its runtime formID.
; proxy.PlaySubGraphAnimation() is the FO4 native for sending behavior graph events.
; ==============================================================================

Function SendProxyAnimEvent(int aiActorFormID, string asEventName)
    Actor proxy = Game.GetForm(aiActorFormID) as Actor
    If proxy == None
        Return
    EndIf
    proxy.PlaySubGraphAnimation(asEventName)
    Debug.Trace("[CoSyncQuest] SendProxyAnimEvent: formID=0x" + aiActorFormID + " event='" + asEventName + "'")
EndFunction

Function SpawnRemotePlayer(int entityID)
    ObjectReference ref = CoSync.GetActorByEntityID(entityID)
    If ref == None
        Debug.Trace("[CoSyncQuest] SpawnRemotePlayer: no actor for entityID=" + entityID)
        Return
    EndIf
    Actor remoteActor = ref as Actor
    If remoteActor == None
        Debug.Trace("[CoSyncQuest] SpawnRemotePlayer: ref is not an Actor for entityID=" + entityID)
        Return
    EndIf
    ; C++ placed the actor in the world — enable it here so it becomes visible.
    ; Passing false skips the fade-in (instant appear, matches network timing).
    remoteActor.Enable(false)
    Debug.Trace("[CoSyncQuest] SpawnRemotePlayer: enabled actor for entityID=" + entityID)
EndFunction

Function DespawnRemotePlayer(int entityID)
    ObjectReference ref = CoSync.GetActorByEntityID(entityID)
    If ref == None
        Debug.Trace("[CoSyncQuest] DespawnRemotePlayer: no actor for entityID=" + entityID)
        Return
    EndIf
    Actor remoteActor = ref as Actor
    If remoteActor == None
        Debug.Trace("[CoSyncQuest] DespawnRemotePlayer: ref not an Actor for entityID=" + entityID)
        Return
    EndIf
    ; Disable first (removes from render) then Delete (frees the ref).
    ; C++ removes from CoSyncEntityRegistry immediately after this call returns.
    remoteActor.Disable(false)
    remoteActor.Delete()
    Debug.Trace("[CoSyncQuest] DespawnRemotePlayer: cleaned up actor for entityID=" + entityID)
EndFunction

; ==============================================================================
; DOOR CELL SCAN
; Iterates all refs in the player's current cell, registers OnActivate on
; every DOOR ref.  Safe to call repeatedly — re-registration is idempotent.
; GetRefsInCell() is a CoSync native that wraps cell->objectList.
; GetType() returns the base form's formType integer (32 = kFormType_DOOR).
; ==============================================================================

Function ScanCellDoors()
    Cell currentCell = Game.GetPlayer().GetParentCell()
    If currentCell == None
        Return
    EndIf
    ObjectReference[] refs = CoSync.GetRefsInCell(currentCell)
    If refs == None
        Return
    EndIf
    Int doorCount = 0
    Int i = 0
    While i < refs.Length
        ObjectReference ref = refs[i]
        If ref != None && CoSync.IsDoor(ref)
            RegisterForRemoteEvent(ref, "OnActivate")
            doorCount += 1
        EndIf
        i += 1
    EndWhile
    Debug.Trace("[CoSyncQuest] Door scan: " + doorCount + " door(s) registered in cell")
EndFunction

; ==============================================================================
; MAP MARKER DISCOVERY SYNC
; Fires whenever the player (or any tracked actor) moves to a new location.
; We check the new location's map marker — if it's visible and not yet seen,
; we broadcast it to all clients via CoSync.NotifyMapMarkerDiscovered.
; ==============================================================================

Event Actor.OnLocationChange(Actor akActor, Location akOldLoc, Location akNewLoc)
    If !CoSync.IsHost()
        Return
    EndIf
    If akNewLoc == None
        Return
    EndIf

    ; TODO: map marker discovery broadcast requires GetMapMarker() which is not
    ; available in vanilla Papyrus.  The C++ receive path (CoSyncMapSync) is
    ; in place; a future update can hook this via a custom F4SE event or polling.
    Debug.Trace("[CoSyncQuest] OnLocationChange (map sync stub)")
EndEvent

; ==============================================================================
; DOOR ACTIVATION SYNC
; Fires when any ref we registered via RegisterForRemoteEvent("OnActivate")
; is activated.  akSender = the DOOR ref, akActionRef = who activated it.
; Only broadcasts when the local player is the activator — other activators
; (NPCs, scripts) are silently ignored to avoid spurious syncs.
; The C++ suppression guard (s_suppressed in CoSyncActivateSync.cpp) prevents
; this from echoing back when the activation came FROM the network.
; ==============================================================================

Event ObjectReference.OnActivate(ObjectReference akSender, ObjectReference akActionRef)
    If akActionRef != Game.GetPlayer()
        Return
    EndIf
    If CoSync.IsDoor(akSender)
        Int openState = akSender.GetOpenState()
        CoSync.NotifyActivate(akSender, openState)
        Debug.Trace("[CoSyncQuest] Door activated: ref=0x" + akSender.GetFormID() + " state=" + openState)
    EndIf
EndEvent

; ==============================================================================
; POWER ARMOR ENTER SYNC — via Actor.OnSit
; PA chassis is furniture; OnSit fires when the player sits in it.
; IsInPowerArmor() at the moment of OnSit == True confirms this is a PA enter.
; akFurniture is the chassis ObjectReference — same data C++ needs.
; ==============================================================================

Event Actor.OnSit(Actor akTarget, ObjectReference akFurniture)
    If akTarget != playerRef
        Return
    EndIf
    If !akTarget.IsInPowerArmor()
        Return  ; Regular furniture sit — not a PA enter, ignore
    EndIf
    If akFurniture == None
        Debug.Trace("[CoSyncQuest] OnSit(PA): chassis ref is None")
        Return
    EndIf
    bIsInPowerArmor = True
    CoSync.NotifyPowerArmor(akTarget, akFurniture, True)
    Debug.Trace("[CoSyncQuest] OnSit(PA enter) -> NotifyPowerArmor ENTER chassis=0x" + akFurniture.GetFormID())
EndEvent

; ==============================================================================
; POWER ARMOR EXIT SYNC — via Actor.OnGetUp
; OnGetUp fires when the player exits any furniture.  bIsInPowerArmor guards
; against triggering on regular chairs — only fires after a confirmed PA enter.
; ==============================================================================

Event Actor.OnGetUp(Actor akTarget, ObjectReference akFurniture)
    If akTarget != playerRef
        Return
    EndIf
    If !bIsInPowerArmor
        Return  ; Was not in PA — regular furniture exit, ignore
    EndIf
    bIsInPowerArmor = False
    If akFurniture == None
        Debug.Trace("[CoSyncQuest] OnGetUp(PA exit): chassis ref is None")
        Return
    EndIf
    CoSync.NotifyPowerArmor(akTarget, akFurniture, False)
    Debug.Trace("[CoSyncQuest] OnGetUp(PA exit) -> NotifyPowerArmor EXIT chassis=0x" + akFurniture.GetFormID())
EndEvent

; ==============================================================================
; COMPANION POLL
; Diffs Game.GetCurrentFollowers() (F4SE) against the last known follower list.
; New entries  → NotifyCompanionRecruited (C++ broadcasts a COMP_REQ)
; Lost entries → NotifyCompanionDismissed (C++ broadcasts COMP_DISMISS)
;
; NOTE: Game.GetCurrentFollowers() is provided by F4SE.  If it returns None
; on a given tick (e.g., before the player fully loads), we skip that cycle
; and retry next tick to avoid false dismissals.
; ==============================================================================

Function PollCompanionChanges()
    Actor[] currentFollowers = Game.GetCurrentFollowers()

    ; Skip the cycle if the follower list is unavailable — avoids spurious dismissals
    ; during cell transitions or save loads before the player is fully initialised.
    If currentFollowers == None
        Return
    EndIf

    ; ---- Detect newly recruited companions ----------------------------------
    ; Any actor present in currentFollowers but absent from trackedFollowers
    ; has just been recruited and needs a network broadcast.
    Int i = 0
    While i < currentFollowers.Length
        Actor akComp = currentFollowers[i]
        If akComp != None
            If !CompanionArrayContains(trackedFollowers, akComp)
                CoSync.NotifyCompanionRecruited(akComp)
                Debug.Trace("[CoSyncQuest] Companion recruited -> NotifyCompanionRecruited " + akComp.GetBaseObject().GetFormID())
            EndIf
        EndIf
        i += 1
    EndWhile

    ; ---- Detect dismissed companions ----------------------------------------
    ; Any actor present in trackedFollowers but absent from currentFollowers
    ; has just been dismissed.
    If trackedFollowers != None
        i = 0
        While i < trackedFollowers.Length
            Actor akComp = trackedFollowers[i]
            If akComp != None
                If !CompanionArrayContains(currentFollowers, akComp)
                    CoSync.NotifyCompanionDismissed(akComp)
                    Debug.Trace("[CoSyncQuest] Companion dismissed -> NotifyCompanionDismissed " + akComp.GetBaseObject().GetFormID())
                EndIf
            EndIf
            i += 1
        EndWhile
    EndIf

    ; Update snapshot for next cycle
    trackedFollowers = currentFollowers
EndFunction

; Linear search helper — returns True if akTarget appears in akArr.
; Used by PollCompanionChanges to diff follower snapshots.
Bool Function CompanionArrayContains(Actor[] akArr, Actor akTarget)
    If akArr == None || akTarget == None
        Return False
    EndIf
    Int i = 0
    While i < akArr.Length
        If akArr[i] == akTarget
            Return True
        EndIf
        i += 1
    EndWhile
    Return False
EndFunction

