#pragma once

#include "NiTypes.h"
#include "GameReferences.h"
#include "PluginAPI.h"
#include "CoSyncAnimVarSystem.h"

extern AnimVarSnapshot g_latestLocalAnimVars;

void InstallTickHook();

// kMessage_GameLoaded fires once game data is loaded (main menu reachable),
// not once a save/new-game has actually put a PlayerCharacter in the world.
// g_player is reliably NULL at that point, so InstallTickHook() called there
// fails permanently with no retry. Call this once per frame (e.g. from the
// DX11 Present hook) until it reports installed; it no-ops cheaply once done.
void TickHook_RetryInstallIfNeeded();

bool TickHook_IsInstalled();
bool TickHook_HasFired();
double TickHook_SecondsSinceInstall();
double TickHook_LastFireSeconds();

class CoSyncPlayer;

void InstallRemoteActorHook(Actor* actor);
void RegisterRemoteActor  (Actor* actor, CoSyncPlayer* player);
void UnregisterRemoteActor(Actor* actor);

// 2026-08-20: true once InstallRemoteActorHook has successfully patched the
// remote-actor vtable slot (or found it already shared with the local
// player's). Bone-replication mode 2 uses this to decide whether
// RemoteActorUpdate_Hook's own post-orig ApplySkeleton call (write-last,
// mirrors FO4_Wrld's detour_actor_update_perframe) will run for every
// registered proxy this frame -- if so, CoSyncPlayer::TickAnimVars must NOT
// also call ApplySkeleton directly, or the pose gets applied twice per frame
// (double-fire, same defect class as round 97's TickAnimVars double-call).
bool IsRemoteActorHookInstalled();

void InstallNpcAnimSyncHook();

void TickHook_CaptureLocalAnimVars();

void TickHook_ScheduleBoneTasks();
