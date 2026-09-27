#pragma once

#include <unordered_map>
#include <cstdint>
#include <mutex>

class Actor;

class CoSyncEntityRegistry
{
public:
    struct Entry
    {
        uint32_t entityID = 0;
        Actor* actor = nullptr;
    };

public:

    Actor* GetActor(uint32_t entityID) const;


    uint32_t GetEntityID(Actor* actor) const;


    bool Register(uint32_t entityID, Actor* actor);


    void Remove(uint32_t entityID);


    void Clear();

private:
    // P3 (2026-09-26): the Papyrus native GetActorByEntityID (flagged NoWait,
    // so it runs on a Papyrus VM thread) reads this map while the game thread
    // registers/removes entries. Every method takes this lock.
    mutable std::mutex m_mutex;
    std::unordered_map<uint32_t, Entry> entities;
};

extern CoSyncEntityRegistry g_CoSyncEntities;
