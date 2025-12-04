
#include "GameAPI.h"

#include "ConsoleLogger.h"
#include "LocalPlayerState.h"

#include "f4se/GameReferences.h"
#include "f4se/GameObjects.h"
#include "f4se/GameFormComponents.h"
#include "f4se_common/Relocation.h"

// We only use raw memory offsets for transform updates, just like TickHook.cpp
// No engine-only helper calls that caused compile errors before.

namespace GameAPI
{
    //
    // 1. Safe accessor for the local player
    //
    PlayerCharacter* GetPlayer()
    {
        // g_player is a RelocPtr<PlayerCharacter*>, declared in GameReferences.h
        if (!g_player.GetPtr())
            return nullptr;

        return *g_player.GetPtr();
    }

    // ------------------------------------------------------------------------
    // 2. Offsets for position/rotation (same as in TickHook.cpp)
    // ------------------------------------------------------------------------

    // NOTE: These MUST match what you're using in TickHook.cpp.
    // If you ever change them there, change them here too.
    static constexpr uintptr_t kPosX_Offset = 0xD0;
    static constexpr uintptr_t kPosY_Offset = 0xD4;
    static constexpr uintptr_t kPosZ_Offset = 0xD8;

    static constexpr uintptr_t kRotX_Offset = 0xE0;
    static constexpr uintptr_t kRotY_Offset = 0xE4;
    static constexpr uintptr_t kRotZ_Offset = 0xE8;

    // ------------------------------------------------------------------------
    // 3. Apply transforms + basic movement flags to an arbitrary Actor
    // ------------------------------------------------------------------------

     void GameAPI::ApplyRemotePlayerStateToActor(Actor* actor, const LocalPlayerState& state)
    {
        if (!actor)
            return;

        uintptr_t base = reinterpret_cast<uintptr_t>(actor);

        // --- Position ---
        *reinterpret_cast<float*>(base + kPosX_Offset) = state.position.x;
        *reinterpret_cast<float*>(base + kPosY_Offset) = state.position.y;
        *reinterpret_cast<float*>(base + kPosZ_Offset) = state.position.z;

        // --- Rotation ---
        *reinterpret_cast<float*>(base + kRotX_Offset) = state.rotation.x;
        *reinterpret_cast<float*>(base + kRotY_Offset) = state.rotation.y;
        *reinterpret_cast<float*>(base + kRotZ_Offset) = state.rotation.z;

        // For now, we are NOT touching health/AP via ActorValueOwner,
        // because FO4's AV API is different and has been a source of compile errors.
        // We'll just log them so you can see the values coming in.

        LOG_DEBUG(
            "[GameAPI] Applied remote state to actor %p: "
            "pos=(%.2f, %.2f, %.2f) rot=(%.2f, %.2f, %.2f) "
            "hp=%.1f/%.1f ap=%.1f/%.1f moving=%d",
            actor,
            state.position.x, state.position.y, state.position.z,
            state.rotation.x, state.rotation.y, state.rotation.z,
            state.health, state.maxHealth,
            state.ap, state.maxActionPoints,
            state.isMoving ? 1 : 0
        );
    }

    // ------------------------------------------------------------------------
    // 4. "High-level" convenience: apply to *some* actor
    //
    // For now, this just reuses the local player so you can see it working.
    // Later we will:
    //   - Spawn / manage dedicated remote actors
    //   - Map SteamID -> Actor* and apply per-remote-player.
    // ------------------------------------------------------------------------

     void ApplyRemotePlayerState(const LocalPlayerState& state)
    {
        // TEMP: Just apply to local player so you can visually verify that
        // replication path works (you'll see yourself being "dragged" by
        // the incoming network state).
        PlayerCharacter* pc = GetPlayer();
        if (!pc)
            return;

        ApplyRemotePlayerStateToActor(pc, state);
    }




    Actor* GameAPI::SpawnActor(UInt32 formID)
    {
        LOG_WARN("[GameAPI] SpawnActor called but not implemented yet");
        return nullptr;
    }

    void GameAPI::MoveActorTo(Actor* actor, const NiPoint3& pos, const NiPoint3& rot)
    {
        LOG_WARN("[GameAPI] MoveActorTo called but not implemented yet");
    }

    void GameAPI::SetActorHealth(Actor* actor, float value, float maxValue)
    {
        LOG_WARN("[GameAPI] SetActorHealth called but not implemented yet");
    }

    void GameAPI::ApplyImpulse(Actor* actor, const NiPoint3& velocity)
    {
        LOG_WARN("[GameAPI] ApplyImpulse called but not implemented yet");
    }








} // namespace GameAPI
