#include "ITypes.h"
#include "Relocation.h"
#include "NiTypes.h"
#include "GameReferences.h"
#include "GameObjects.h"
#include "GameTypes.h"
#include "ConsoleLogger.h"
#include "LocalPlayerState.h"
#include "GameAPI.h"
#include "CoSyncNet.h"
#include "CoSyncSteam.h"
#include "F4MP_Main.h"
#include "TickHook.h"
#include "CoSyncPlayer.h"






#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// ============================================================================
//   PlayerCharacter::vtbl (FO4 NG 1.11.169)
// ============================================================================

// Provided by F4SE
extern RelocPtr<PlayerCharacter*> g_player;

// FO4 NG: the vtable entry we hook (0x140D636C0) uses a 3-arg __fastcall and
// returns 64-bit (not char). We must match this EXACTLY or we crash.
using ActorUpdateFn = __int64(__fastcall*)(PlayerCharacter* actor, void* arg2, void* arg3);
static ActorUpdateFn g_ActorUpdate_Original = nullptr;

// Timing
static double g_lastLogTime = 0.0;
static double g_lastNetSendTime = 0.0;
static bool   g_worldLoadedAnnounced = false;

// Previous frame (for velocity)
static NiPoint3 g_prevPos{ 0.f, 0.f, 0.f };
static bool     g_hasPrevPos = false;
static double   g_prevTime = 0.0;



// ============================================================================
// Time Helpers
// ============================================================================
static double GetNowSeconds()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

static float LengthSq(const NiPoint3& v)
{
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

// ============================================================================
//   HOOK — PlayerCharacter::Update-like entry at vtbl[21]
//   (sub_140D636C0 on 1.11.169 NG)
// ============================================================================
static __int64 __fastcall ActorUpdate_Hook(
    PlayerCharacter* actor,
    void* arg2,
    void* arg3)
{
    // Call original first so we preserve expected behavior
    __int64 result = 0;
    if (g_ActorUpdate_Original)
        result = g_ActorUpdate_Original(actor, arg2, arg3);

    if (!actor)
        return result;

    PlayerCharacter* localPlayer = *g_player;
    if (!localPlayer || actor != localPlayer)
        return result;

    NiPoint3 pos = actor->pos;
    NiPoint3 rot = actor->rot;

    // ========================================================================
    // VELOCITY CALCULATION
    // ========================================================================
    double  now = GetNowSeconds();
    double  dt = 0.0;
    NiPoint3 vel{ 0.f, 0.f, 0.f };

    if (g_hasPrevPos)
    {
        dt = now - g_prevTime;
        if (dt > 0.0001)
        {
            vel.x = (pos.x - g_prevPos.x) / static_cast<float>(dt);
            vel.y = (pos.y - g_prevPos.y) / static_cast<float>(dt);
            vel.z = (pos.z - g_prevPos.z) / static_cast<float>(dt);
        }
    }

    g_prevPos = pos;
    g_prevTime = now;
    g_hasPrevPos = true;

    bool isMoving = (LengthSq(vel) > 1.0f);

    // ========================================================================
    // WORLD READY CHECK
    // ========================================================================
    bool hasCell = (actor->parentCell != nullptr);
    bool hasValidCell = hasCell && (actor->parentCell->formID != 0);
    bool hasValidPos = !(pos.x == 0.f && pos.y == 0.f && pos.z == 0.f);

    bool worldReady = hasValidCell && hasValidPos;
    if (!worldReady)
    {
        if (!g_worldLoadedAnnounced)
            LOG_WARN("[CoSync] WORLD NOT READY (player/cell not valid)");
        return result;
    }

    if (!g_worldLoadedAnnounced)
    {
        g_worldLoadedAnnounced = true;
        LOG_INFO("[CoSync] WORLD LOADED — Player and cell valid!");
    }

    // Make sure CoSyncNet delayed init runs once the world actually exists
    CoSyncNet::PerformPendingInit();

    // ========================================================================
    // FILL LOCAL PLAYER STATE
    // ========================================================================
    g_localPlayerState.position = pos;
    g_localPlayerState.rotation = rot;
    g_localPlayerState.velocity = vel;
    g_localPlayerState.isMoving = isMoving;
    g_localPlayerState.isSprinting = false;  // TODO: hook sprint flag
    g_localPlayerState.isCrouching = false;  // TODO: hook crouch flag
    g_localPlayerState.isJumping = false;  // TODO: hook jump flag

    g_localPlayerState.formID = actor->formID;
    g_localPlayerState.cellFormID = actor->parentCell ? actor->parentCell->formID : 0;

    // Placeholder stats for now
    g_localPlayerState.health = 0.f;
    g_localPlayerState.maxHealth = 0.f;
    g_localPlayerState.ap = 0.f;
    g_localPlayerState.maxActionPoints = 0.f;

    // Periodic debug log
    if (now - g_lastLogTime > 0.5)
    {
        g_lastLogTime = now;
        LOG_DEBUG("[CoSync] PlayerState pos=(%.2f %.2f %.2f) rot=(%.2f %.2f %.2f)",
            pos.x, pos.y, pos.z, rot.x, rot.y, rot.z);
    }

    // ========================================================================
    // NETWORK SEND (ONLY when connected & initialized)
    // ========================================================================
    if (CoSyncNet::IsConnected() && CoSyncNet::IsInitialized())
    {
        // ~20Hz send rate (50ms)
        if (now - g_lastNetSendTime > 0.05)
        {
            g_lastNetSendTime = now;
            LOG_DEBUG("[CoSync] TickHook sending LocalPlayerState");
            CoSyncNet::SendLocalPlayerState(g_localPlayerState);
        }
    }

    // ========================================================================
    // Optional: global tick (for future game-level sync)
    // ========================================================================
    double frameDt = dt;
    if (frameDt <= 0.0 || frameDt > 1.0)
        frameDt = 1.0 / 60.0;

    // If/when you want this back:
    // f4mp::F4MP_Main::Get().GameTick(static_cast<float>(frameDt));

    return result;
}

// ============================================================================
//   INSTALL HOOK
// ============================================================================
void InstallTickHook()
{
    LOG_INFO("[CoSync] ========== InstallTickHook START ==========");

    PlayerCharacter* pc = *g_player;
    if (!pc)
    {
        LOG_ERROR("[CoSync] Cannot install hook — g_player is NULL");
        return;
    }

    LOG_INFO("[CoSync] PlayerCharacter instance = %p", pc);

    uintptr_t* vtbl = *(uintptr_t**)pc;
    if (!vtbl)
    {
        LOG_ERROR("[CoSync] Player vtable is NULL");
        return;
    }

    LOG_INFO("[CoSync] PlayerCharacter vtable @ %p", vtbl);

    // New index for FO4 NG: 21 (entry pointing to sub_140D636C0)
    constexpr UInt32 kIndex = 21;

    uintptr_t origFn = vtbl[kIndex];
    if (!origFn)
    {
        LOG_ERROR("[CoSync] vtbl[%u] is NULL – cannot hook", kIndex);
        return;
    }

    LOG_INFO("[CoSync] PlayerCharacter::Update-like (vtbl[%u]) = %p", kIndex, (void*)origFn);

    

    g_ActorUpdate_Original = reinterpret_cast<ActorUpdateFn>(origFn);

    // Patch vtable entry
    DWORD oldProt;
    if (!VirtualProtect(&vtbl[kIndex], sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProt))
    {
        LOG_ERROR("[CoSync] VirtualProtect (unlock) failed, GetLastError=%u", GetLastError());
        return;
    }

    vtbl[kIndex] = reinterpret_cast<uintptr_t>(&ActorUpdate_Hook);

    DWORD dummy;
    VirtualProtect(&vtbl[kIndex], sizeof(uintptr_t), oldProt, &dummy);

    // Reset timing helpers on install (optional but clean)
    g_hasPrevPos = false;
    g_worldLoadedAnnounced = false;
    g_lastLogTime = 0.0;
    g_lastNetSendTime = 0.0;

    LOG_INFO("[CoSync] TickHook installed successfully (index %u)", kIndex);
    LOG_INFO("[CoSync] ========== InstallTickHook END ==========");
}
