#include "CoSyncEntityRegistry.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"

CoSyncEntityRegistry g_CoSyncEntities;

Actor* CoSyncEntityRegistry::GetActor(uint32_t entityID) const
{
    LOG_REQUIREMENT("CoSyncEntityRegistry::GetActor", "Safe to call with any entity ID -- just returns nothing if it is not registered.");
    auto it = entities.find(entityID);
    return (it != entities.end()) ? it->second.actor : nullptr;
}

uint32_t CoSyncEntityRegistry::GetEntityID(Actor* actor) const
{
    LOG_REQUIREMENT("CoSyncEntityRegistry::GetEntityID", "Safe to call with a null actor -- just returns 0. Otherwise only needs the registry itself.");
    if (!actor)
        return 0;

    for (const auto& pair : entities)
    {
        if (pair.second.actor == actor)
            return pair.second.entityID;
    }
    return 0;
}

bool CoSyncEntityRegistry::Register(uint32_t entityID, Actor* actor)
{
    LOG_REQUIREMENT("CoSyncEntityRegistry::Register", "Needs a valid actor and an entity ID that is not already registered, or the call fails.");
    if (!actor)
    {
        LOG_ERROR(
            "[EntityRegistry] Register FAILED: entityID=%u actor=NULL",
            entityID
        );
        return false;
    }

    auto it = entities.find(entityID);
    if (it != entities.end())
    {
        LOG_WARN(
            "[EntityRegistry] entityID=%u already registered (actor=%p)",
            entityID,
            it->second.actor
        );
        return false;
    }

    entities.emplace(entityID, Entry{ entityID, actor });

    LOG_DEBUG(
        "[EntityRegistry] Registered entityID=%u actor=%p",
        entityID,
        actor
    );

    return true;
}

void CoSyncEntityRegistry::Remove(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncEntityRegistry::Remove", "Safe to call anytime -- does nothing if the entity ID is not in the registry.");
    auto it = entities.find(entityID);
    if (it == entities.end())
        return;

    LOG_DEBUG(
        "[EntityRegistry] Removed entityID=%u actor=%p",
        entityID,
        it->second.actor
    );

    entities.erase(it);
}

void CoSyncEntityRegistry::Clear()
{
    LOG_REQUIREMENT("CoSyncEntityRegistry::Clear", "Safe to call anytime -- just empties the whole registry.");
    LOG_INFO(
        "[EntityRegistry] Clearing %zu entities",
        entities.size()
    );

    entities.clear();
}
