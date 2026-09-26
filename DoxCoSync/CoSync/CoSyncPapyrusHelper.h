#pragma once

#include <cstdint>

class TESQuest;
class Actor;
class TESNPC;

namespace CoSyncPapyrusHelper
{

    void Init();


    void SpawnRemotePlayer(uint32_t entityID);


    void DespawnRemotePlayer(uint32_t entityID);


    Actor* GetRemotePlayerActor(uint32_t entityID);


    TESQuest* GetCoSyncQuest();

    TESNPC* GetRemotePlayerBase();

    // Round 134: runtime-resolved formID of the proxy base NPC. Returns
    // CoSync.esp's own authored CoSyncRemotePlayerBase (local ID 0x003599,
    // complete record: TPLT/TPTA, perks, keywords, DOFT, facegen) with the
    // esp's actual load-order slot in the top byte; falls back to the vanilla
    // raider 0x00020749 only if CoSync.esp is not loaded. Switched back off
    // the vanilla raider because 0x20749 spawns as a permanent A-pose even
    // via console with NO plugin loaded (user-verified; Dogmeat base 0x1D15C
    // animates fine the same way) — that form has no usable animation set
    // when spawned directly.
    UInt32 GetRemotePlayerBaseFormID();

    void SendProxyAnimEvent(Actor* actorRef, const char* eventName);
}
