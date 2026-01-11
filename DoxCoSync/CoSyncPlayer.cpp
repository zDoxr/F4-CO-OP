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
// F4MP-style constants
// -----------------------------------------------------------------------------

// Render slightly in the past to absorb jitter
static constexpr double kInterpDelay = 0.10; // 100 ms

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static NiPoint3 VoidPos()
{
    return NiPoint3{ 0.f, 0.f, -20000.f };
}

static NiPoint3 ZeroRot()
{
    return NiPoint3{ 0.f, 0.f, 0.f };
}

static TESObjectREFR* GetAnchor()
{
    PlayerCharacter* pc = *g_player;
    return pc ? static_cast<TESObjectREFR*>(pc) : nullptr;
}

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
// Spawn (CREATE → GAME THREAD ONLY)
// -----------------------------------------------------------------------------
bool CoSyncPlayer::SpawnInWorld(TESObjectREFR* anchor, TESNPC* npcBase)
{
    if (hasSpawned)
        return true;

    if (!anchor)
        anchor = GetAnchor();

    if (!anchor || !npcBase)
    {
        LOG_ERROR("[CoSyncPlayer] Spawn failed entity=%u (anchor/base null)", entityID);
        return false;
    }

    LOG_INFO("[CoSyncPlayer] Spawn START entity=%u base=0x%08X", entityID, npcBase->formID);

    Actor* actor = CoSyncGameAPI::SpawnRemoteActor(npcBase);
    if (!actor)
    {
        LOG_ERROR("[CoSyncPlayer] SpawnRemoteActor FAILED entity=%u", entityID);
        return false;
    }

    actorRef = actor;

    if (!g_CoSyncEntities.Register(entityID, actorRef))
    {
        LOG_ERROR("[CoSyncPlayer] Failed to register entity=%u actor=%p", entityID, actorRef);
    }



    // Initialize F4MP buffer
    m_tfBuffer.clear();

    NetTransform init{};
    init.pos = actorRef->pos;
    init.rot = actorRef->rot;
    init.scale = 1.0f;
    init.t = 0.0;

    m_tfBuffer.push_back(init);
    m_hasAny = true;
    m_lastHostT = 0.0;

    hasSpawned = true;

    // Apply UPDATE-before-CREATE safely
    if (hasPendingTransform)
        ApplyPendingTransformIfAny();

    LOG_INFO("[CoSyncPlayer] Spawn END entity=%u actor=%p", entityID, actorRef);
    return true;
}

// -----------------------------------------------------------------------------
// Receive UPDATE (authoritative, host-timestamped)
// -----------------------------------------------------------------------------
void CoSyncPlayer::ApplyUpdate(const EntityUpdatePacket& u)
{
    lastPacketTime = u.timestamp;

    // Cache always (UPDATE-before-spawn safe)
    pendingPos = u.pos;
    pendingRot = u.rot;
    pendingVel = u.vel;
    hasPendingTransform = true;

    if (!hasSpawned || !actorRef)
        return;

    NetTransform tf{};
    tf.pos = u.pos;
    tf.rot = u.rot;
    tf.scale = 1.0f;
    tf.t = u.timestamp;

    // Drop out-of-order packets
    if (m_hasAny && tf.t < m_lastHostT)
        return;

    m_lastHostT = tf.t;
    m_hasAny = true;

    m_tfBuffer.push_back(tf);

    // Cap buffer (F4MP keeps small windows)
    while (m_tfBuffer.size() > 8)
        m_tfBuffer.pop_front();

    LOG_DEBUG("[CoSyncPlayer] UPDATE entity=%u t=%f", entityID, u.timestamp);


}

// -----------------------------------------------------------------------------
// First placement / snap (ONCE ONLY)
// -----------------------------------------------------------------------------
void CoSyncPlayer::ApplyPendingTransformIfAny()
{
    if (!hasSpawned || !actorRef || !hasPendingTransform)
        return;

    LOG_DEBUG("[CoSyncPlayer] Applying PENDING TRANSFORM entity=%u pos=(%.2f,%.2f,%.2f) rot=(%.2f,%.2f,%.2f)",
        entityID,
        pendingPos.x, pendingPos.y, pendingPos.z,
        pendingRot.x, pendingRot.y, pendingRot.z
	);

    // Hard snap exactly once
    CoSyncGameAPI::PositionRemoteActor(actorRef, pendingPos, pendingRot);

    m_tfBuffer.clear();

    NetTransform tf{};
    tf.pos = pendingPos;
    tf.rot = pendingRot;
    tf.scale = 1.0f;
    tf.t = (lastPacketTime > 0.0) ? lastPacketTime : 0.0;

    m_tfBuffer.push_back(tf);
    m_lastHostT = tf.t;
    m_hasAny = true;

    hasPendingTransform = false;

	LOG_DEBUG("[CoSyncPlayer] SNAP entity=%u", entityID);
}

// -----------------------------------------------------------------------------
// Per-frame smoothing (F4MP transform buffer)
// -----------------------------------------------------------------------------
void CoSyncPlayer::TickSmoothing(double /*now*/)
{
    if (!hasSpawned || !actorRef || m_tfBuffer.size() < 2)
        return;

    const double renderT = m_lastHostT - kInterpDelay;
    if (renderT <= 0.0)
        return;

    NetTransform a = m_tfBuffer.front();
    NetTransform b = m_tfBuffer.back();

    for (size_t i = 1; i < m_tfBuffer.size(); i++)
    {
        if (m_tfBuffer[i].t >= renderT)
        {
            a = m_tfBuffer[i - 1];
            b = m_tfBuffer[i];
            break;
        }
    }

    double denom = (b.t - a.t);
    double t = (denom > 0.000001) ? (renderT - a.t) / denom : 0.0;
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
	LOG_DEBUG("[CoSyncPlayer] TICK entity=%u t=%f interpT=%f pos=(%.2f,%.2f,%.2f)", entityID, renderT, t, pos.x, pos.y, pos.z);

    // Drop old samples
    while (m_tfBuffer.size() >= 2 && m_tfBuffer[1].t < renderT)
        m_tfBuffer.pop_front();
}
