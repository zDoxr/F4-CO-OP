#pragma once

#include <unordered_map>
#include <cstdint>

class Actor;

// -----------------------------------------------------------------------------
// CoSyncEntityRegistry
// Purpose:
//   Authoritative mapping of entityID -> Actor*
//   NO spawning, NO transforms, NO ownership logic
// -----------------------------------------------------------------------------
class CoSyncEntityRegistry
{
public:
    struct Entry
    {
        uint32_t entityID = 0;
        Actor* actor = nullptr;
    };

public:
    // Lookup (read-only)
    Actor* GetActor(uint32_t entityID) const;

    // Registration (called ONLY after successful spawn)
    bool Register(uint32_t entityID, Actor* actor);

    // Removal (timeout / disconnect)
    void Remove(uint32_t entityID);

    // Full clear (shutdown / unload)
    void Clear();

private:
    std::unordered_map<uint32_t, Entry> entities;
};

extern CoSyncEntityRegistry g_CoSyncEntities;
