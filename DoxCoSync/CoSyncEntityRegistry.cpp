#include "CoSyncEntityRegistry.h"
#include "ConsoleLogger.h"

CoSyncEntityRegistry g_CoSyncEntities;

// -----------------------------------------------------------------------------
// GetActor
// -----------------------------------------------------------------------------
Actor* CoSyncEntityRegistry::GetActor(uint32_t entityID) const
{
    auto it = entities.find(entityID);
    return (it != entities.end()) ? it->second.actor : nullptr;
}

// -----------------------------------------------------------------------------
// Register
// -----------------------------------------------------------------------------
bool CoSyncEntityRegistry::Register(uint32_t entityID, Actor* actor)
{
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

// -----------------------------------------------------------------------------
// Remove
// -----------------------------------------------------------------------------
void CoSyncEntityRegistry::Remove(uint32_t entityID)
{
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

// -----------------------------------------------------------------------------
// Clear
// -----------------------------------------------------------------------------
void CoSyncEntityRegistry::Clear()
{
    LOG_INFO(
        "[EntityRegistry] Clearing %zu entities",
        entities.size()
    );

    entities.clear();
}
