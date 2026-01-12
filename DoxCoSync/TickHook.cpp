#include "ITypes.h"
#include "Relocation.h"
#include "NiTypes.h"
#include "GameReferences.h"
#include "GameObjects.h"
#include "GameTypes.h"

#include "ConsoleLogger.h"
#include "LocalPlayerStateGlobals.h"
#include "LocalPlayerState.h"

#include "CoSyncNet.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// Provided by F4SE
extern RelocPtr<PlayerCharacter*> g_player;
extern LocalPlayerState g_localPlayerState;

using ActorUpdateFn = __int64(__fastcall*)(PlayerCharacter* actor, void* arg2, void* arg3);
static ActorUpdateFn g_ActorUpdate_Original = nullptr;

// -----------------------------------------------------------------------------
// Local tracking (LOCAL PLAYER ONLY)
// -----------------------------------------------------------------------------
static bool   g_hasPrevPos = false;
static double g_prevTime = 0.0;
static double g_lastNetSendTime = 0.0;

static bool     g_localEntityBound = false;
static uint32_t g_localEntityID = 0;

static NiPoint3 g_prevPos{ 0.f, 0.f, 0.f };

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// PlayerCharacter update hook (LOCAL PLAYER ONLY)
// -----------------------------------------------------------------------------
static __int64 __fastcall ActorUpdate_Hook(
    PlayerCharacter* actor,
    void* arg2,
    void* arg3)
{
    __int64 result =
        g_ActorUpdate_Original
        ? g_ActorUpdate_Original(actor, arg2, arg3)
        : 0;

    if (!actor)
        return result;

    PlayerCharacter* localPlayer = *g_player;
    if (!localPlayer || actor != localPlayer)
        return result;

    const double now = GetNowSeconds();

    // Deferred init (safe)
    if (!CoSyncNet::IsInitialized())
        CoSyncNet::PerformPendingInit();
    if (!CoSyncNet::IsConnected())
		return result;
    else 
		CoSyncNet::Tick(now);

    // Bind local entity ID ONCE
    if (!g_localEntityBound)
    {
        g_localEntityID = CoSyncNet::GetMyEntityID();
        g_localEntityBound = true;

        LOG_INFO(
            "[TickHook] Local entity bound: entityID=%u",
            g_localEntityID
        );
    }

    // World validity checks
    if (!actor->parentCell || actor->parentCell->formID == 0)
        return result;

    if (!CALL_MEMBER_FN(actor, GetWorldspace)())
        return result;

    const NiPoint3 pos = actor->pos;
    const NiPoint3 rot = actor->rot;

    if (pos.x == 0.f && pos.y == 0.f && pos.z == 0.f)
        return result;

    // Velocity
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

    // Populate local state (diagnostic + future use)
    g_localPlayerState.timestamp = now;
    g_localPlayerState.position = pos;
    g_localPlayerState.rotation = rot;
    g_localPlayerState.velocity = vel;

    g_localPlayerState.formID = actor->formID;
    g_localPlayerState.cellFormID = actor->parentCell->formID;

    // Network send (CLIENT → HOST)
    if (CoSyncNet::IsConnected())
    {
        if (now - g_lastNetSendTime >= 0.05) // ~20 Hz
        {
            g_lastNetSendTime = now;

            CoSyncNet::SendMyEntityUpdate(
                g_localEntityID,
                pos,
                rot,
                vel
            );
        }
    }

    return result;
}

// -----------------------------------------------------------------------------
// Hook installation
// -----------------------------------------------------------------------------
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

    constexpr UInt32 kIndex = 21;
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

    // Reset tracking
    g_hasPrevPos = false;
    g_prevTime = 0.0;
    g_lastNetSendTime = 0.0;
    g_prevPos = { 0.f, 0.f, 0.f };

    g_localEntityBound = false;
    g_localEntityID = 0;

    LOG_INFO("[TickHook] TickHook installed successfully (vtable index %u)", kIndex);
}
