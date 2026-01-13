#include "CoSyncPlayer.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncEntityRegistry.h"
#include "ConsoleLogger.h"
#include "GameObjects.h"
#include "GameForms.h"
#include "GameReferences.h"
#include "CoSyncGameAPI.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

extern RelocPtr<PlayerCharacter*> g_player;

// -----------------------------------------------------------------------------
// Timing helpers
// -----------------------------------------------------------------------------
static double NowSeconds()
{
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

// Render slightly in the past to absorb jitter
static constexpr double kInterpDelay = 0.10;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static NiPoint3 Lerp(const NiPoint3& a, const NiPoint3& b, double t)
{
    return NiPoint3{
        a.x + float((b.x - a.x) * t),
        a.y + float((b.y - a.y) * t),
        a.z + float((b.z - a.z) * t)
    };
}

// -----------------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------------
CoSyncPlayer::CoSyncPlayer(const std::string& name)
    : username(name)
{
}

// -----------------------------------------------------------------------------
// Spawn
// -----------------------------------------------------------------------------
bool CoSyncPlayer::SpawnInWorld(TESObjectREFR* anchor, TESNPC* npcBase)
{
    if (hasSpawned)
        return true;

    if (!anchor || !npcBase)
        return false;

    Actor* actor = CoSyncGameAPI::SpawnRemoteActor(npcBase);
    if (!actor)
        return false;

    actorRef = actor;
    g_CoSyncEntities.Register(entityID, actorRef);

    m_tfBuffer.clear();
    m_lastHostTime = 0.0;
    m_lastRecvLocalTime = NowSeconds();
    m_hasAny = false;

    hasSpawned = true;

    if (hasPendingTransform)
        ApplyPendingTransformIfAny();

    LOG_INFO("[CoSyncPlayer] Spawn END entity=%u actor=%p", entityID, actorRef);
    return true;
}

// -----------------------------------------------------------------------------
// Receive UPDATE
// -----------------------------------------------------------------------------
void CoSyncPlayer::ApplyUpdate(const EntityUpdatePacket& u)
{
    lastPacketTime = u.timestamp;
    m_lastRecvLocalTime = NowSeconds();

    pendingPos = u.pos;
    pendingRot = u.rot;
    pendingVel = u.vel;
    hasPendingTransform = true;

    if (!hasSpawned || !actorRef)
        return;

    if (m_hasAny && u.timestamp < m_lastHostTime)
        return;

    NetTransform tf{};
    tf.pos = u.pos;
    tf.rot = u.rot;
    tf.scale = 1.0f;
    tf.hostTime = u.timestamp;

    m_tfBuffer.push_back(tf);
    m_lastHostTime = tf.hostTime;
    m_hasAny = true;

    while (m_tfBuffer.size() > 8)
        m_tfBuffer.pop_front();
}

// -----------------------------------------------------------------------------
// First snap
// -----------------------------------------------------------------------------
void CoSyncPlayer::ApplyPendingTransformIfAny()
{
    if (!hasSpawned || !actorRef || !hasPendingTransform)
        return;

    CoSyncGameAPI::PositionRemoteActor(actorRef, pendingPos, pendingRot);

    m_tfBuffer.clear();

    NetTransform tf{};
    tf.pos = pendingPos;
    tf.rot = pendingRot;
    tf.scale = 1.0f;
    tf.hostTime = lastPacketTime;

    m_tfBuffer.push_back(tf);
    m_lastHostTime = tf.hostTime;
    m_hasAny = true;

    hasPendingTransform = false;
}

// -----------------------------------------------------------------------------
// Per-frame smoothing
// -----------------------------------------------------------------------------
void CoSyncPlayer::TickSmoothing(double now)
{
    if (!hasSpawned || !actorRef || m_tfBuffer.size() < 2)
        return;

    const double renderTime = m_lastHostTime - kInterpDelay;
    if (renderTime <= 0.0)
        return;

    NetTransform a = m_tfBuffer.front();
    NetTransform b = m_tfBuffer.back();

    for (size_t i = 1; i < m_tfBuffer.size(); ++i)
    {
        if (m_tfBuffer[i].hostTime >= renderTime)
        {
            a = m_tfBuffer[i - 1];
            b = m_tfBuffer[i];
            break;
        }
    }

    const double denom = (b.hostTime - a.hostTime);
    double t = (denom > 0.000001) ? (renderTime - a.hostTime) / denom : 0.0;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    NiPoint3 pos = Lerp(a.pos, b.pos, t);
    NiPoint3 rot = Lerp(a.rot, b.rot, t);

    UInt32 dummy = 0;
    MoveRefrToPosition(
        actorRef,
        &dummy,
        actorRef->parentCell,
        nullptr,
        &pos,
        &rot
    );
}
