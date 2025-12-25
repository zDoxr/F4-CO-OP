#include "GameReferences.h"
#include "GameExtraData.h"
#include "GameRTTI.h"
#include "TickHook.h"
#include "LocalPlayerState.h"

// ============================================================
// F4SE global relocations (Fallout 4 runtime 1.11.191)
// ============================================================

// Player singleton
RelocPtr<PlayerCharacter*> g_player(0x032D2260);

// Native engine functions
RelocAddr<_HasDetectionLOS>        HasDetectionLOS(0x010E0120);
RelocAddr<_GetLinkedRef_Native>    GetLinkedRef_Native(0x00564580);
RelocAddr<_SetLinkedRef_Native>    SetLinkedRef_Native(0x005645A0);
RelocAddr<_MoveRefrToPosition>     MoveRefrToPosition(0x01180B30);

// ============================================================
// CoSync globals
// ============================================================

LocalPlayerState g_localPlayerState;
