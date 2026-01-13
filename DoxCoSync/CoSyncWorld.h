#pragma once

#include "GameReferences.h"
#include "Relocation.h"

// Provided by F4SE
extern RelocPtr<PlayerCharacter*> g_player;

// -----------------------------------------------------------------------------
// CoSyncWorld
//
// Purpose (F4MP-aligned):
//   Gate *ALL* spawning and world mutation until the game world is valid.
//   Conservative checks only — false negatives OK, false positives NOT OK.
// -----------------------------------------------------------------------------
namespace CoSyncWorld
{
    bool IsWorldReady();


}





