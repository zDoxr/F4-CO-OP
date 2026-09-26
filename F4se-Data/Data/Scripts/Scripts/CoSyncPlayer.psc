; ==============================================================================
; CoSyncPlayer.psc
;
; Script attached to the CoSync remote-proxy Actor form in CreationKit.
;
; This script runs on EVERY spawned proxy actor (one per remote player).
; C++ (CoSyncPlayerManager) spawns a copy of the custom proxy NPC form via
; PlaceAtMe, then this OnInit fires and registers the actor with the C++ layer.
;
; Responsibilities:
;   - Map this actor back to its network entity ID (reverse lookup via C++)
;   - Provide helper functions for C++ to call via CallFunctionNoWait:
;       InitProxy(entityID)     — called by C++ after spawn, sets entityID
;       SetProxyName(name)      — sets a display string for debugging
;       OnProxyDied()           — called when entity death packet arrives
;   - Handle appearance sync on first spawn (CopyAppearance from player snapshot)
;
; CreationKit setup:
;   1. Create a new NPC form (e.g. "CoSyncProxyNPC").
;      Race: HumanRace (or the same race as the host player).
;      Skeleton: DefaultMale.nif (gives standard weapon attach nodes).
;      Behavior Graph: leave default — behavior graph injection happens in C++.
;   2. Attach this script to the CoSyncProxyNPC form via the Script panel.
;   3. The script has no CK-side Properties — all data arrives via C++ calls.
;
; NOTE: Weapon and armor sync are handled entirely in C++ (CoSyncWeaponSync /
; CoSyncOutfitSync) via BGSInventoryList polling. Papyrus here only manages
; initialization and death/cleanup.
; ==============================================================================

Scriptname CoSyncPlayer extends Actor

; ============================================================================
; State
; ============================================================================
int     myEntityID   = -1       ; Network entity ID (set by C++ via InitProxy)
string  myUsername   = ""       ; Display name (set by C++ via SetProxyName)
bool    isInitialized = false   ; True once InitProxy has been called


; ============================================================================
; INITIALIZATION — called once by Papyrus VM when actor is created
; ============================================================================
Event OnInit()
    ; C++ calls InitProxy(entityID) via CallFunctionNoWait immediately after
    ; spawning. Papyrus event delivery is asynchronous, so InitProxy may arrive
    ; before or after OnInit. The isInitialized guard handles both orderings.
    Debug.Trace("[CoSyncPlayer] OnInit — waiting for C++ InitProxy call")
EndEvent


; ============================================================================
; InitProxy — called by C++ (CallFunctionNoWait) right after PlaceAtMe
; ============================================================================
Function InitProxy(int entityID)
    If isInitialized
        ; Duplicate call from a reconnect / re-spawn — update entityID only
        myEntityID = entityID
        Debug.Trace("[CoSyncPlayer] InitProxy re-init: entityID=" + entityID)
        Return
    EndIf

    myEntityID    = entityID
    isInitialized = true

    ; ---- Disable vanilla AI so C++ can drive movement via anim vars ----
    ; EnableAI(false) is the FO4 Papyrus equivalent of "disable AI" on an actor.
    ; C++ also calls this after spawn, but doing it here provides a fallback
    ; in case the C++ call arrives out-of-order relative to OnInit.
    self.EnableAI(False)
    self.StopCombat()

    ; Disable VATS targeting — proxy should not be attackable via VATS
    ; (no SetValue available pre-NG; set via SetActorValue if needed)

    Debug.Trace("[CoSyncPlayer] InitProxy complete: entityID=" + entityID)
EndFunction


; ============================================================================
; SetProxyName — called by C++ to label the proxy with the remote player's name
; ============================================================================
Function SetProxyName(string username)
    myUsername = username
    Debug.Trace("[CoSyncPlayer] SetProxyName: entityID=" + myEntityID + " name='" + username + "'")
EndFunction


; ============================================================================
; OnProxyDied — called by C++ when the remote entity's death packet arrives
; ============================================================================
Function OnProxyDied()
    Debug.Trace("[CoSyncPlayer] OnProxyDied: entityID=" + myEntityID + " name='" + myUsername + "'")
    ; Trigger the actor's normal death so ragdoll / death animation plays
    ; KillSilent leaves no corpse marker visible to the local player
    self.Kill(None)
EndFunction


; ============================================================================
; ANIMATION DISPATCH — called by C++ via CallFunctionNoWait
;
; All behavior graph events for this proxy go through here.
; Debug.SendAnimationEvent(self, name) is the reliable Papyrus path:
;   - Validates actor state before dispatch (no crash on half-loaded actors)
;   - Goes through full BSAnimationGraphManager dispatch (all graphs, correct one)
;   - Appears in Papyrus trace logs for debugging
;
; C++ CallFunctionNoWait(actorRef, "SendAnimEvent", [eventName]) routes here
; because CoSyncPlayer.psc is attached to every proxy actor form.
; ============================================================================

; Dispatch a behavior graph event on this proxy actor.
; PlaySubGraphAnimation is the FO4 Papyrus equivalent of Skyrim's
; Debug.SendAnimationEvent — it dispatches a named behavior graph event
; on the calling actor's animation graph manager, validated by the engine.
Function SendAnimEvent(string asEventName)
    self.PlaySubGraphAnimation(asEventName)
    Debug.Trace("[CoSyncPlayer] SendAnimEvent: eid=" + myEntityID + " '" + asEventName + "'")
EndFunction

; Set behavior graph variable by name — used for one-time identity setup.
; Per-frame variables are still written by C++ LoadAnimationVariables (60fps).
Function SetAnimBool(string asVarName, bool abValue)
    self.SetAnimationVariableBool(asVarName, abValue)
EndFunction

Function SetAnimFloat(string asVarName, float afValue)
    self.SetAnimationVariableFloat(asVarName, afValue)
EndFunction

Function SetAnimInt(string asVarName, int aiValue)
    self.SetAnimationVariableInt(asVarName, aiValue)
EndFunction

; ============================================================================
; Accessors — read-only state for external callers
; ============================================================================
int Function GetEntityID()
    Return myEntityID
EndFunction

string Function GetProxyName()
    Return myUsername
EndFunction

bool Function IsInitialized()
    Return isInitialized
EndFunction
