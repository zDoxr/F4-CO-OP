#pragma once

#include <string>
#include <cstdint>

class ActorValueInfo;

namespace CoSyncHealthSync
{

    bool IsHP(const std::string& msg);
    bool IsKill(const std::string& msg);
    bool IsDmg(const std::string& msg);


    // level is optional (0 = "not sent"); only player entities carry a real level.
    // rads is optional (0 = "not sent"); drives the red radiation chunk on the
    // lobby health bar. rads rides after level in the packet, so a non-zero rads
    // always emits level too.
    std::string SerializeHP(uint32_t entityID, float current, float maxHP, uint32_t level = 0, float rads = 0.f,
                            float ap = -1.f, float apMax = 0.f);

    // Rads ActorValueInfo, resolved once by scanning arrAVIF for the "Rads" AV
    // (no stable formID). May return nullptr if unresolved. Cached.
    ActorValueInfo* GetRadsActorValue();
    std::string SerializeKill(uint32_t entityID);
    std::string SerializeDmg(uint32_t entityID, float amount);


    void OnReceiveHP(const std::string& msg);
    void OnReceiveKill(const std::string& msg);
    void OnReceiveDmg(const std::string& msg);


    void FlushPendingHealthOps();

    // Damage detection: polls the HP of every remote-owned actor rendered in
    // THIS world (player proxies always; bound world NPCs only on the client)
    // and broadcasts a DMG packet when a drop is observed, so the authoritative
    // owner of that entity takes the damage for real. This is what makes
    // "host shoots P2's proxy -> P2 loses health" (and vice versa) work.
    void TickDamageDetection(double now);

    // Session teardown — clears baselines so a reconnect starts fresh.
    void Reset();
}
