#include "ITypes.h"
#include "Relocation.h"
#include "NiTypes.h"
#include "GameReferences.h"
#include "GameObjects.h"
#include "GameTypes.h"
#include "LocalPlayerStateGlobals.h"
#include "ConsoleLogger.h"
#include "LocalPlayerState.h"
#include "CoSyncNet.h"
#include "CoSyncPlayerManager.h"
#include "TickHook.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// Provided by F4SE
extern RelocPtr<PlayerCharacter*> g_player;
extern LocalPlayerState g_localPlayerState;

// ------------------------------------------------------------
// Fallout 4 PlayerCharacter::Update hook
// ------------------------------------------------------------
using ActorUpdateFn = __int64(__fastcall*)(PlayerCharacter* actor, void* arg2, void* arg3);
static ActorUpdateFn g_ActorUpdate_Original = nullptr;

// ------------------------------------------------------------
// State
// ------------------------------------------------------------
static bool   g_hasPrevPos = false;
static double g_prevTime = 0.0;
static double g_lastNetSendTime = 0.0;

static bool     g_localEntityBound = false;
static uint32_t g_localEntityID = 0;

// IMPORTANT: aggregate init (NiPoint3 has no safe default ctor)
static NiPoint3 g_prevPos = { 0.f, 0.f, 0.f };

// ------------------------------------------------------------
// Timing helpers
// ------------------------------------------------------------
static double GetNowSeconds()
{
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

static float LengthSq(const NiPoint3& v)
{
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

// ------------------------------------------------------------
// Hook
// ------------------------------------------------------------
static __int64 __fastcall ActorUpdate_Hook(PlayerCharacter* actor, void* arg2, void* arg3)
{
    // Always call original first (F4MP does this)
    __int64 result = g_ActorUpdate_Original
        ? g_ActorUpdate_Original(actor, arg2, arg3)
        : 0;

    if (!actor)
        return result;

    PlayerCharacter* localPlayer = *g_player;
    if (!localPlayer || actor != localPlayer)
        return result;

    // ---- F4MP WORLD GATE ----
    if (!actor->parentCell || actor->parentCell->formID == 0)
        return result;

    if (!CALL_MEMBER_FN(actor, GetWorldspace)())
        return result;

    const NiPoint3 pos = actor->pos;
    const NiPoint3 rot = actor->rot;

    // Prevent bogus origin spam during early frames
    if (pos.x == 0.f && pos.y == 0.f && pos.z == 0.f)
        return result;

    const double now = GetNowSeconds();

    // ------------------------------------------------------------
    // Networking lifecycle (F4MP-style deferred init)
    // ------------------------------------------------------------
    if (!CoSyncNet::IsInitialized())
        CoSyncNet::PerformPendingInit();

    // Bind a stable local entity ID ONCE (does not spawn anything).
    // This MUST match what the host uses when emitting EntityCreate for this peer.
    if (!g_localEntityBound)
    {
        const uint64_t sid = CoSyncNet::GetMySteamID();
        g_localEntityID = static_cast<uint32_t>(sid & 0xFFFFFFFFu);
        g_CoSyncPlayerManager.SetLocalEntityID(g_localEntityID);

        g_localEntityBound = true;

        LOG_INFO("[TickHook] Local entity bound: steam/hash=%llu -> entityID=%u",
            (unsigned long long)sid, g_localEntityID);
    }

    // ------------------------------------------------------------
    // Velocity (client-side only, never authoritative)
    // ------------------------------------------------------------
    NiPoint3 vel{ 0.f, 0.f, 0.f };
    if (g_hasPrevPos)
    {
        const double dt = now - g_prevTime;
        if (dt > 0.0001)
        {
            vel.x = (pos.x - g_prevPos.x) / float(dt);
            vel.y = (pos.y - g_prevPos.y) / float(dt);
            vel.z = (pos.z - g_prevPos.z) / float(dt);
        }
    }

    g_prevPos = pos;
    g_prevTime = now;
    g_hasPrevPos = true;

    const bool isMoving = (LengthSq(vel) > 1.0f);

    // ------------------------------------------------------------
    // Fill LocalPlayerState (pure data, no side effects)
    // ------------------------------------------------------------
    g_localPlayerState.username = CoSyncNet::GetMyName();
    g_localPlayerState.timestamp = now;

    g_localPlayerState.position = pos;
    g_localPlayerState.rotation = rot;
    g_localPlayerState.velocity = vel;

    g_localPlayerState.isMoving = isMoving;
    g_localPlayerState.isSprinting = false;
    g_localPlayerState.isCrouching = false;
    g_localPlayerState.isJumping = false;

    // Keep these as local debug/telemetry identifiers (not network entity authority)
    g_localPlayerState.formID = actor->formID;
    g_localPlayerState.cellFormID = actor->parentCell->formID;

    // ------------------------------------------------------------
    // Drive networking ONCE per tick (F4MP rule)
    // ------------------------------------------------------------
    CoSyncNet::Tick(now);

    // ------------------------------------------------------------
    // Entity protocol (F4MP authority split)
    //   - Client sends UPDATE only
    //   - Host creates/broadcasts CREATE
    // ------------------------------------------------------------
    if (CoSyncNet::IsConnected() && CoSyncNet::IsInitialized())
    {
        // Throttle updates (~20 Hz)
        if (now - g_lastNetSendTime >= 0.05)
        {
            g_lastNetSendTime = now;

            CoSyncNet::SendMyEntityUpdate(
                g_localEntityID,                 // authoritative network entityID
                g_localPlayerState.position,
                g_localPlayerState.rotation,
                g_localPlayerState.velocity
            );
        }
    }

    return result;
}

// ------------------------------------------------------------
// Hook install
// ------------------------------------------------------------
void InstallTickHook()
{
    LOG_INFO("[TickHook] Installing PlayerCharacter update hook");

    PlayerCharacter* pc = *g_player;
    if (!pc)
    {
        LOG_ERROR("[TickHook] g_player is NULL");
        return;
    }

    uintptr_t* vtbl = *(uintptr_t**)pc;
    if (!vtbl)
    {
        LOG_ERROR("[TickHook] Player vtable is NULL");
        return;
    }

    constexpr UInt32 kIndex = 21; // FO4 PlayerCharacter::Update
    uintptr_t origFn = vtbl[kIndex];
    if (!origFn)
    {
        LOG_ERROR("[TickHook] vtbl[%u] is NULL", kIndex);
        return;
    }

    g_ActorUpdate_Original = reinterpret_cast<ActorUpdateFn>(origFn);

    DWORD oldProt{};
    VirtualProtect(&vtbl[kIndex], sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProt);
    vtbl[kIndex] = reinterpret_cast<uintptr_t>(&ActorUpdate_Hook);
    VirtualProtect(&vtbl[kIndex], sizeof(uintptr_t), oldProt, &oldProt);

    // Reset state
    g_hasPrevPos = false;
    g_prevTime = 0.0;
    g_lastNetSendTime = 0.0;
    g_prevPos = { 0.f, 0.f, 0.f };

    g_localEntityBound = false;
    g_localEntityID = 0;

    LOG_INFO("[TickHook] TickHook installed successfully (vtable index %u)", kIndex);
}
