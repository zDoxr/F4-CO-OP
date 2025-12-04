#include "common/ITypes.h"
#include "f4se_common/Relocation.h"
#include "f4se/NiTypes.h"
#include "f4se/GameReferences.h"
#include "f4se/GameObjects.h"

#include "ConsoleLogger.h"
#include "LocalPlayerState.h"
#include "GameAPI.h"
#include "F4MP_Main.h"
#include "TickHook.h"
#include "CoSyncNet.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

// ===========================================================================
// VTABLE LOCATION (your verified, working one)
// ============================================================================
static RelocAddr<uintptr_t> g_ActorVTable_RVA(0x02555320);

using ActorUpdateFn = char (*)(Actor* actor, __int64 a2);
static ActorUpdateFn g_ActorUpdate_Original = nullptr;

// Logging / network throttles
static double g_lastLogTime = 0.0;
static double g_lastNetSendTime = 0.0;
static bool   g_worldLoadedAnnounced = false;

// ============================================================================
// Time helper
// ============================================================================
static double GetNowSeconds()
{
    LARGE_INTEGER freq{};
    LARGE_INTEGER counter{};

    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0)
        return 0.0;

    if (!QueryPerformanceCounter(&counter))
        return 0.0;

    return static_cast<double>(counter.QuadPart) /
        static_cast<double>(freq.QuadPart);
}

// ============================================================================
// Position / rotation offsets (valid for FO4 v1.10.163)
// ============================================================================
static constexpr uintptr_t kPosX_Offset = 0xD0;
static constexpr uintptr_t kPosY_Offset = 0xD4;
static constexpr uintptr_t kPosZ_Offset = 0xD8;

static constexpr uintptr_t kRotX_Offset = 0xE0;
static constexpr uintptr_t kRotY_Offset = 0xE4;
static constexpr uintptr_t kRotZ_Offset = 0xE8;

// ============================================================================
// Velocity tracking
// ============================================================================
static NiPoint3 g_prevPos{ 0.f, 0.f, 0.f };
static bool     g_hasPrevPos = false;
static double   g_prevTime = 0.0;

static float LengthSq(const NiPoint3& v)
{
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

// ============================================================================
// ActorUpdate Hook
// ============================================================================
static char ActorUpdate_Hook(PlayerCharacter* actor, __int64 a2)
{
    LOG_WARN("[CoSync] TOP OF HOOK REACHED (1)");

    char result = g_ActorUpdate_Original(actor, a2);
    LOG_WARN("[CoSync] AFTER ORIGINAL CALLED (2)");

    if (!actor)
        return result;

    PlayerCharacter* localPlayer = *g_player;
    if (!localPlayer || actor != localPlayer)
        return result;

    LOG_WARN("[CoSync] PASSED LOCAL PLAYER CHECK (3)");

    uintptr_t base = reinterpret_cast<uintptr_t>(actor);

    // ----------------------------------------------------------------------
    // POSITION + ROTATION
    // ----------------------------------------------------------------------
    NiPoint3 pos{
        *reinterpret_cast<float*>(base + kPosX_Offset),
        *reinterpret_cast<float*>(base + kPosY_Offset),
        *reinterpret_cast<float*>(base + kPosZ_Offset)
    };

    NiPoint3 rot{
        *reinterpret_cast<float*>(base + kRotX_Offset),
        *reinterpret_cast<float*>(base + kRotY_Offset),
        *reinterpret_cast<float*>(base + kRotZ_Offset)
    };

    // ----------------------------------------------------------------------
    // VELOCITY
    // ----------------------------------------------------------------------
    double   now = GetNowSeconds();
    NiPoint3 vel{ 0.f, 0.f, 0.f };

    if (g_hasPrevPos)
    {
        double dt = now - g_prevTime;
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

    // ----------------------------------------------------------------------
    // WORLD READY CHECKS
    // ----------------------------------------------------------------------
    LOG_WARN("[CoSync] ABOUT TO CHECK WORLD READY (4)");
    LOG_WARN("[CoSync] now=%.6f last=%.6f diff=%.6f",
        now, g_lastLogTime, now - g_lastLogTime);

    bool hasCell = (actor->parentCell != nullptr);
    bool hasValidCell = hasCell && (actor->parentCell->formID != 0);

    bool hasMovementController =
        (*reinterpret_cast<uintptr_t*>(base + 0x148) != 0) ||
        (*reinterpret_cast<uintptr_t*>(base + 0x1A0) != 0);

    bool hasValidPos =
        !(pos.x == 0.f && pos.y == 0.f && pos.z == 0.f);

    bool worldReady = hasValidCell && hasMovementController && hasValidPos;

    if (!worldReady)
    {
        if (!g_worldLoadedAnnounced)
            LOG_WARN("[CoSync] WORLD NOT READY (5)");
        return result;
    }

    if (!g_worldLoadedAnnounced)
    {
        g_worldLoadedAnnounced = true;
        LOG_INFO("[CoSync] WORLD LOADED — Player and cell fully valid!");
    }

    // ----------------------------------------------------------------------
    // TEMP placeholders (hook up real HP/AP later via GameAPI)
    // ----------------------------------------------------------------------
    float  hp = 0.f;
    float  maxHp = 0.f;
    float  ap = 0.f;
    float  maxAp = 0.f;
    UInt32 weaponFormID = 0;
    bool   isSprinting = false;
    bool   isCrouching = false;
    bool   isJumping = false;

    // ----------------------------------------------------------------------
    // Fill global local player state
    // ----------------------------------------------------------------------
    g_localPlayerState.position = pos;
    g_localPlayerState.rotation = rot;
    g_localPlayerState.velocity = vel;
    g_localPlayerState.isMoving = isMoving;
    g_localPlayerState.isSprinting = isSprinting;
    g_localPlayerState.isCrouching = isCrouching;
    g_localPlayerState.isJumping = isJumping;
    g_localPlayerState.equippedWeaponFormID = weaponFormID;
    g_localPlayerState.formID = actor->formID;
    g_localPlayerState.cellFormID = actor->parentCell ? actor->parentCell->formID : 0;
    g_localPlayerState.health = hp;
    g_localPlayerState.maxHealth = maxHp;
    g_localPlayerState.ap = ap;
    g_localPlayerState.maxActionPoints = maxAp;

    LOG_WARN("[CoSync] ABOUT TO PRINT FINAL STATE LOG (6)");

    // ----------------------------------------------------------------------
    // LOG THROTTLE
    // ----------------------------------------------------------------------
    if (now - g_lastLogTime > 0.5)
    {
        g_lastLogTime = now;

        LOG_DEBUG(
            "[CoSync] PlayerState: pos=(%.2f %.2f %.2f) "
            "rot=(%.2f %.2f %.2f) vel=(%.2f %.2f %.2f) "
            "hp=%.1f/%.1f ap=%.1f/%.1f move=%d",
            pos.x, pos.y, pos.z,
            rot.x, rot.y, rot.z,
            vel.x, vel.y, vel.z,
            hp, maxHp,
            ap, maxAp,
            isMoving
        );
    }

    // ----------------------------------------------------------------------
    // NETWORK SEND THROTTLE (~20 FPS for network)
    // ----------------------------------------------------------------------
    if (now - g_lastNetSendTime > 0.05)   // 50ms
    {
        g_lastNetSendTime = now;
        CoSyncNet::SendLocalPlayerState(g_localPlayerState);
    }

    return result;
}

// ============================================================================
// InstallTickHook
// ============================================================================
void InstallTickHook()
{
    LOG_DEBUG("[CoSync] InstallTickHook: starting hook");

    PlayerCharacter* pc = *g_player;
    if (!pc)
    {
        LOG_ERROR("[CoSync] Cannot install hook — g_player is NULL");
        return;
    }

    uintptr_t* vtbl = *(uintptr_t**)pc;
    if (!vtbl)
    {
        LOG_ERROR("[CoSync] Player vtable NULL");
        return;
    }

    LOG_INFO("[CoSync] PlayerCharacter vtable @ %p", vtbl);

    constexpr UInt32 kUpdateIndex = 19;

    g_ActorUpdate_Original =
        reinterpret_cast<ActorUpdateFn>(vtbl[kUpdateIndex]);

    LOG_DEBUG("[CoSync] Original PlayerCharacter::Update = %p",
        (void*)g_ActorUpdate_Original);

    DWORD oldProt = 0;
    VirtualProtect(&vtbl[kUpdateIndex], sizeof(uintptr_t),
        PAGE_EXECUTE_READWRITE, &oldProt);

    vtbl[kUpdateIndex] = (uintptr_t)&ActorUpdate_Hook;

    DWORD dummy = 0;
    VirtualProtect(&vtbl[kUpdateIndex], sizeof(uintptr_t),
        oldProt, &dummy);

    LOG_DEBUG("[CoSync] Hook installed on Update (index 19)");
}
