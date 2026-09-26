#pragma once

#include <unordered_map>
#include <cstdint>

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
    std::unordered_map<uint32_t, Entry> entities;
};

extern CoSyncEntityRegistry g_CoSyncEntities;
