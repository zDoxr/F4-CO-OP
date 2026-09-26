#pragma once

class TESQuest;

// Hooks three Quest objective Papyrus natives (FO4 1.11.221):
//   CompleteAllObjectives   RVA 0x01188990  — thunk: mov rcx,r8; jmp impl
//   SetObjectiveCompleted   RVA 0x01185BA0  — void(VM*,u32,TESQuest*,i32,bool)
//   SetObjectiveDisplayed   RVA 0x01185CF0  — void(VM*,u32,TESQuest*,i32,bool,bool)
// Intercepts every Papyrus-level objective change and relays it to peers.
// Loopback suppressed via CoSyncQuestSync::IsRemoteApplying().

void InstallQuestObjectiveHooks();
void UninstallQuestObjectiveHooks();

// Set true before calling objective fns via Papyrus to suppress outbound echo.
extern thread_local bool g_objHookSuppressed;

// Apply incoming remote objective changes via the confirmed-correct native
// RVAs these hooks already detour, instead of F4SE's CallFunctionNoWait --
// same stale-internal-RVA hazard as CoSyncQuestHook_ApplySetCurrentStageID.
// SEH-guarded; returns false if the call faults.
bool CoSyncQuestObjectiveHook_ApplyCompleteAll(TESQuest* quest);
bool CoSyncQuestObjectiveHook_ApplySetObjectiveCompleted(TESQuest* quest, int32_t idx, bool state);
bool CoSyncQuestObjectiveHook_ApplySetObjectiveDisplayed(TESQuest* quest, int32_t idx, bool displayed, bool force);
