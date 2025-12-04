#include "RemoteActorManager.h"
#include "ConsoleLogger.h"
#include "GameAPI.h"   // You can add helpers here for spawning/updating actors
#include "f4se/GameReferences.h"
#include "f4se/GameObjects.h"

namespace RemoteActors
{
    // For now, we assume exactly ONE remote player
    static Actor* g_remoteActor = nullptr;
    static bool        g_spawnRequested = false;
    static LocalPlayerState g_lastRemoteState{};

    // Internal helper to spawn the remote actor in the world
    static Actor* SpawnRemoteActor(const LocalPlayerState& st)
    {
        // TODO: Replace with your actual logic.
        // This is just a placeholder that logs.
        LOG_INFO("[RemoteActors] SpawnRemoteActor requested at pos=(%.2f %.2f %.2f)",
            st.position.x, st.position.y, st.position.z);

        // Example idea:
        //  - Clone a template NPC
        //  - Or use GameAPI::SpawnRemotePlayer(st)
        // For now, we just return nullptr (no actual actor yet).
        return nullptr;
    }

    // Internal helper to update an existing remote actor
    static void UpdateRemoteActor(Actor* actor, const LocalPlayerState& st)
    {
        if (!actor)
            return;

        // TODO: Write actual transform/movement.
        // Example: write to actor->pos, rot or use GameAPI helpers.
        LOG_DEBUG("[RemoteActors] UpdateRemoteActor: pos=(%.2f %.2f %.2f)",
            st.position.x, st.position.y, st.position.z);
    }

    void OnRemotePlayerState(const LocalPlayerState& remote)
    {
        g_lastRemoteState = remote;

        // If no actor yet, attempt to spawn one
        if (!g_remoteActor)
        {
            g_remoteActor = SpawnRemoteActor(remote);
            if (!g_remoteActor)
            {
                // If spawn fails, don’t spam – just log once per update
                LOG_WARN("[RemoteActors] SpawnRemoteActor returned null (no visual yet)");
                return;
            }

            LOG_INFO("[RemoteActors] Remote actor spawned");
        }

        // Update transform
        UpdateRemoteActor(g_remoteActor, remote);
    }

    void Shutdown()
    {
        // If you create real remote Actor instances,
        // you should clean them up here (disable, delete, etc.)
        g_remoteActor = nullptr;
    }

} // namespace RemoteActors
