#pragma once

class TESQuest;

// Hooks Quest.SetCurrentStageID Papyrus dispatch (FO4 1.11.221  RVA 0x01185F70).
// Intercepts every Papyrus-level stage change on the local machine and relays it
// to connected peers via CoSyncQuestSync::NotifyQuestStage.
// Loopback is suppressed when CoSync is the caller (FlushPendingQuestOps path).

void InstallQuestStageHook();
void UninstallQuestStageHook();

// Set true before calling SetCurrentStageID via Papyrus to suppress the outbound hook.
extern thread_local bool g_questHookSuppressed;

// Applies an incoming remote stage change via the same confirmed-correct native
// RVA this hook already detours (g_origSetStage), instead of going through
// F4SE's CallFunctionNoWait/CallFunctionNoWait_Internal -- that internal RVA
// (vendored in f4se-0.7.8, never independently confirmed against this NG
// 1.11.221 build) is stale and jumps through a null pointer (RIP=0, AV
// type=EXEC). SEH-guarded; returns false if the call still faults.
bool CoSyncQuestHook_ApplySetCurrentStageID(TESQuest* quest, int32_t stage);
