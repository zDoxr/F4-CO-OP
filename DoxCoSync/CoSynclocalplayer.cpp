// CoSyncLocalPlayer.cpp
#include "CoSyncLocalPlayer.h"

#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncGameAPI.h"
#include "GameReferences.h"
#include "GameObjects.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cmath>

// External reference to the local player
extern RelocPtr<PlayerCharacter*> g_player;

namespace
{
    // State tracking
    bool s_initialized = false;

    // Last sent state
    NiPoint3 s_lastSentPos{ 0.f, 0.f, 0.f };
    NiPoint3 s_lastSentRot{ 0.f, 0.f, 0.f };
    NiPoint3 s_lastSentVel{ 0.f, 0.f, 0.f };
    double   s_lastSendTime = 0.0;

    // Previous frame state (for velocity calculation)
    NiPoint3 s_prevPos{ 0.f, 0.f, 0.f };
    double   s_prevTime = 0.0;
    bool     s_hasPrevious = false;

    // Configuration
    float s_updateRate = 20.0f;              // 20 Hz default
    float s_movementThreshold = 0.01f;       // 1cm movement threshold
    float s_rotationThreshold = 0.01f;       // ~0.57 degrees rotation threshold

    // Timing helper
    double NowSeconds()
    {
        static double s_freq = [] {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return double(f.QuadPart);
            }();

        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        return double(c.QuadPart) / s_freq;
    }

    // Vector helpers
    float VectorLength(const NiPoint3& v)
    {
        return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    }

    NiPoint3 VectorSubtract(const NiPoint3& a, const NiPoint3& b)
    {
        return NiPoint3{ a.x - b.x, a.y - b.y, a.z - b.z };
    }

    NiPoint3 VectorScale(const NiPoint3& v, float scale)
    {
        return NiPoint3{ v.x * scale, v.y * scale, v.z * scale };
    }

    // Calculate velocity from position delta
    NiPoint3 CalculateVelocity(const NiPoint3& currentPos, const NiPoint3& prevPos, double deltaTime)
    {
        if (deltaTime < 0.0001)
            return NiPoint3{ 0.f, 0.f, 0.f };

        NiPoint3 delta = VectorSubtract(currentPos, prevPos);
        float scale = static_cast<float>(1.0 / deltaTime);

        return VectorScale(delta, scale);
    }

    // Check if position has changed significantly
    bool HasMovedSignificantly(const NiPoint3& current, const NiPoint3& previous)
    {
        NiPoint3 delta = VectorSubtract(current, previous);
        float distance = VectorLength(delta);
        return distance > s_movementThreshold;
    }

    // Check if rotation has changed significantly
    bool HasRotatedSignificantly(const NiPoint3& current, const NiPoint3& previous)
    {
        // Check each axis
        float deltaX = fabsf(current.x - previous.x);
        float deltaY = fabsf(current.y - previous.y);
        float deltaZ = fabsf(current.z - previous.z);

        return (deltaX > s_rotationThreshold) ||
            (deltaY > s_rotationThreshold) ||
            (deltaZ > s_rotationThreshold);
    }
}

// =============================================================================
// PUBLIC API
// =============================================================================

void CoSyncLocalPlayer::Init()
{
    if (s_initialized)
        return;

    s_initialized = true;

    // Reset state
    s_lastSendTime = 0.0;
    s_hasPrevious = false;

    LOG_INFO("[LocalPlayer] Initialized (updateRate=%.1f Hz)", s_updateRate);
}

void CoSyncLocalPlayer::Shutdown()
{
    if (!s_initialized)
        return;

    s_initialized = false;
    s_hasPrevious = false;

    LOG_INFO("[LocalPlayer] Shutdown");
}

Actor* CoSyncLocalPlayer::GetLocalPlayerActor()
{
    return *g_player;
}

bool CoSyncLocalPlayer::GetLocalTransform(NiPoint3& outPos, NiPoint3& outRot)
{
    Actor* player = GetLocalPlayerActor();
    if (!player)
        return false;

    return CoSyncGameAPI::GetActorWorldTransform(player, outPos, outRot);
}

void CoSyncLocalPlayer::ForceUpdate()
{
    if (!s_initialized)
        return;

    // Reset last send time to force an update on next tick
    s_lastSendTime = 0.0;

    LOG_INFO("[LocalPlayer] Force update requested");
}

void CoSyncLocalPlayer::SetUpdateRate(float updatesPerSecond)
{
    if (updatesPerSecond < 1.0f)
        updatesPerSecond = 1.0f;
    if (updatesPerSecond > 60.0f)
        updatesPerSecond = 60.0f;

    s_updateRate = updatesPerSecond;

    LOG_INFO("[LocalPlayer] Update rate set to %.1f Hz", s_updateRate);
}

void CoSyncLocalPlayer::SetMovementThreshold(float distance)
{
    if (distance < 0.0f)
        distance = 0.0f;

    s_movementThreshold = distance;

    LOG_INFO("[LocalPlayer] Movement threshold set to %.3f", distance);
}

void CoSyncLocalPlayer::SetRotationThreshold(float radians)
{
    if (radians < 0.0f)
        radians = 0.0f;

    s_rotationThreshold = radians;

    LOG_INFO("[LocalPlayer] Rotation threshold set to %.3f rad", radians);
}

void CoSyncLocalPlayer::Tick(double now)
{
    // Must be initialized and connected
    if (!s_initialized)
        return;

    if (!CoSyncNet::IsInitialized() || !CoSyncNet::IsConnected())
        return;

    // Get local player
    Actor* player = GetLocalPlayerActor();
    if (!player)
        return;

    // Get current transform
    NiPoint3 currentPos{ 0.f, 0.f, 0.f };
    NiPoint3 currentRot{ 0.f, 0.f, 0.f };

    if (!CoSyncGameAPI::GetActorWorldTransform(player, currentPos, currentRot))
        return;

    // Calculate velocity
    NiPoint3 velocity{ 0.f, 0.f, 0.f };
    if (s_hasPrevious && s_prevTime > 0.0)
    {
        double deltaTime = now - s_prevTime;
        velocity = CalculateVelocity(currentPos, s_prevPos, deltaTime);
    }

    // Update previous state
    s_prevPos = currentPos;
    s_prevTime = now;
    s_hasPrevious = true;

    // Check if enough time has passed
    const double updateInterval = 1.0 / static_cast<double>(s_updateRate);
    const double timeSinceLastSend = now - s_lastSendTime;

    if (timeSinceLastSend < updateInterval)
        return;

    // Check if we've moved or rotated enough to warrant an update
    const bool hasMoved = HasMovedSignificantly(currentPos, s_lastSentPos);
    const bool hasRotated = HasRotatedSignificantly(currentRot, s_lastSentRot);

    if (!hasMoved && !hasRotated)
    {
        // Still send periodic updates even if not moving (heartbeat)
        // But reduce frequency when stationary
        if (timeSinceLastSend < (updateInterval * 3.0))
            return;
    }

    // Send the update
    const uint32_t myEntityID = CoSyncNet::GetMyEntityID();

    CoSyncNet::SendMyEntityUpdate(
        myEntityID,
        currentPos,
        currentRot,
        velocity,
        now
    );

    // Update last sent state
    s_lastSentPos = currentPos;
    s_lastSentRot = currentRot;
    s_lastSentVel = velocity;
    s_lastSendTime = now;

    // Debug logging (only when actually moving)
    if (hasMoved || hasRotated)
    {
        LOG_DEBUG("[LocalPlayer] Sent UPDATE entity=%u pos=(%.2f,%.2f,%.2f) vel=(%.2f,%.2f,%.2f)",
            myEntityID,
            currentPos.x, currentPos.y, currentPos.z,
            velocity.x, velocity.y, velocity.z);
    }
}