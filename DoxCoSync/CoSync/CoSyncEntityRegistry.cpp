#include "CoSyncEntityRegistry.h"
#include "ConsoleLogger.h"

CoSyncEntityRegistry g_CoSyncEntities;

Actor* CoSyncEntityRegistry::GetActor(uint32_t entityID) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = entities.find(entityID);
    return (it != entities.end()) ? it->second.actor : nullptr;
}

uint32_t CoSyncEntityRegistry::GetEntityID(Actor* actor) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
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
    std::lock_guard<std::mutex> lk(m_mutex);
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
    std::lock_guard<std::mutex> lk(m_mutex);
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
    std::lock_guard<std::mutex> lk(m_mutex);
    LOG_INFO(
        "[EntityRegistry] Clearing %zu entities",
        entities.size()
    );

    entities.clear();
}
