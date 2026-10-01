#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncPuppetAuthority.h"
#include "CoSyncEnemySync.h"
#include "CoSyncGameAPI.h"
#include "TickHook.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "GameReferences.h"
#include "MinHook.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
    // ── Engine addresses (Fallout4.exe 1.11.240, IDA 2026-10-01) ────────────
    // docs/NPC_AUTHORITY_REPORT.md section 2 has the decompiles and callers.
    constexpr uintptr_t kRVA_CombatEntry  = 0x00D00F80;   // (AIProcess*, Actor*, Actor* target, int64) -> char; 26 callers
    constexpr uintptr_t kRVA_SetSpeed     = 0x00D35520;   // (AIProcess*, float) -> writes middleHigh+0x428
    constexpr uintptr_t kRVA_SetDirection = 0x00D355C0;   // (AIProcess*, float) -> writes middleHigh+0xC0
    constexpr uintptr_t kRVA_QueueStartCombat = 0x00C46B90;  // (taskQueue, actor, target, char) -> task 97
    constexpr uintptr_t kRVA_TaskQueue    = 0x032FF808;   // qword: the engine task queue (kill core uses it too)

    constexpr uint8_t kPro_CombatEntry[20] = { 0x48,0x8B,0xC4,0x53,0x56,0x57,0x41,0x56,0x48,0x81,0xEC,0x48,0x01,0x00,0x00,0x48,0x83,0x79,0x10,0x00 };
    constexpr uint8_t kPro_SetSpeed[18]    = { 0x48,0x8B,0x41,0x08,0x48,0x85,0xC0,0x74,0x08,0xF3,0x0F,0x11,0x88,0x28,0x04,0x00,0x00,0xC3 };
    constexpr uint8_t kPro_SetDir[18]      = { 0x48,0x8B,0x41,0x08,0x48,0x85,0xC0,0x74,0x08,0xF3,0x0F,0x11,0x88,0xC0,0x00,0x00,0x00,0xC3 };
    constexpr uint8_t kPro_QueueStart[19]  = { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x81,0xEC };

    constexpr uintptr_t kActor_Process      = 0x300;   // AIProcess*
    constexpr uintptr_t kProcess_MiddleHigh = 0x08;
    constexpr uintptr_t kMH_Speed           = 0x428;
    constexpr uintptr_t kMH_Direction       = 0xC0;
    constexpr uintptr_t kActor_CombatTarget = 0x380;   // BSPointerHandle (GetCombatTarget native)

    using CombatEntry_t = char(__fastcall*)(void*, Actor*, Actor*, int64_t);
    using SetChannel_t  = int64_t(__fastcall*)(void*, float);
    using QueueStart_t  = void(__fastcall*)(void*, Actor*, Actor*, char);

    uintptr_t     g_base = 0;
    bool          g_installed = false;
    bool          g_cfgCombatGate = true;
    bool          g_cfgMovement = true;
    CombatEntry_t g_origCombatEntry = nullptr;
    SetChannel_t  g_origSetSpeed = nullptr;
    SetChannel_t  g_origSetDir = nullptr;
    QueueStart_t  g_queueStart = nullptr;

    // ── Combat gate state (hook: any thread; writers: game thread) ──────────
    std::mutex                             g_combatMx;
    std::unordered_map<Actor*, Actor*>     g_allowedTarget;   // puppet -> the only target it may fight

    // Game-thread bookkeeping per puppet.
    struct CombatApply
    {
        bool     active = false;
        Actor*   target = nullptr;
        double   lastStart = -100.0;
        double   lastStop  = -100.0;
    };
    std::unordered_map<Actor*, CombatApply> g_combatApplied;

    // ── Movement override (hook: movement job; writers: game thread) ────────
    struct MoveOverride { float speed; float direction; uint64_t expiresMs; };
    SRWLOCK                                  g_moveLock = SRWLOCK_INIT;
    std::unordered_map<void*, MoveOverride>  g_moveByProcess;   // AIProcess* -> owner channels
    std::unordered_map<Actor*, void*>        g_processOfPuppet; // game thread only

    // ── Diagnostics ─────────────────────────────────────────────────────────
    std::atomic<uint32_t> g_dRefusedEntries{ 0 }, g_dAllowedEntries{ 0 }, g_dSpeedOverrides{ 0 }, g_dDirOverrides{ 0 };
    uint32_t g_dStarts = 0, g_dStops = 0;

    uint64_t NowMs() { return GetTickCount64(); }

    __declspec(noinline) void* SafeReadPtr(const void* p, uintptr_t off)
    {
        __try { return p ? *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(p) + off) : nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    __declspec(noinline) bool SafeReadFloat(const void* p, uintptr_t off, float& out)
    {
        __try
        {
            if (!p) return false;
            out = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(p) + off);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    __declspec(noinline) uint32_t SafeReadU32(const void* p, uintptr_t off)
    {
        __try { return p ? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p) + off) : 0u; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
    }

    bool PrologueOk(uintptr_t rva, const uint8_t* bytes, size_t n, const char* what)
    {
        if (std::memcmp(reinterpret_cast<const void*>(g_base + rva), bytes, n) == 0)
            return true;
        LOG_ERROR("[PuppetAuthority] %s prologue mismatch at base+0x%llX (not 1.11.240?) -- that part is disabled",
            what, static_cast<unsigned long long>(rva));
        return false;
    }

    bool HookOne(uintptr_t rva, void* detour, void** orig, const char* name)
    {
        void* target = reinterpret_cast<void*>(g_base + rva);
        MH_STATUS st = MH_CreateHook(target, detour, orig);
        if (st != MH_OK)
        {
            LOG_ERROR("[PuppetAuthority] MH_CreateHook(%s) failed: %d", name, static_cast<int>(st));
            *orig = nullptr;
            return false;
        }
        st = MH_EnableHook(target);
        if (st != MH_OK)
        {
            LOG_ERROR("[PuppetAuthority] MH_EnableHook(%s) failed: %d", name, static_cast<int>(st));
            MH_RemoveHook(target);
            *orig = nullptr;
            return false;
        }
        LOG_INFO("[PuppetAuthority] %s hooked at base+0x%llX", name, static_cast<unsigned long long>(rva));
        return true;
    }

    void LoadConfig()
    {
        char exe[MAX_PATH] = {};
        if (!GetModuleFileNameA(nullptr, exe, MAX_PATH))
            return;
        std::string ini = exe;
        ini = ini.substr(0, ini.find_last_of("\\/") + 1) + "Data\\F4SE\\Plugins\\CoSync.ini";
        g_cfgCombatGate = GetPrivateProfileIntA("Puppet", "bCombatGate", 1, ini.c_str()) != 0;
        g_cfgMovement   = GetPrivateProfileIntA("Puppet", "bMovementChannels", 1, ini.c_str()) != 0;
    }

    // ── Detours ─────────────────────────────────────────────────────────────
    char __fastcall Detour_CombatEntry(void* process, Actor* actor, Actor* target, int64_t a4)
    {
        if (actor && CoSyncEnemySync::IsGuardedPuppet(actor))
        {
            Actor* allowed = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_combatMx);
                auto it = g_allowedTarget.find(actor);
                if (it != g_allowedTarget.end())
                    allowed = it->second;
            }
            // Only the owner's fight may exist on a puppet: same target, or nothing.
            if (!target || target != allowed)
            {
                g_dRefusedEntries.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            g_dAllowedEntries.fetch_add(1, std::memory_order_relaxed);
        }
        return g_origCombatEntry(process, actor, target, a4);
    }

    bool LookupMove(void* process, MoveOverride& out)
    {
        AcquireSRWLockShared(&g_moveLock);
        bool found = false;
        auto it = g_moveByProcess.find(process);
        if (it != g_moveByProcess.end() && it->second.expiresMs > NowMs())
        {
            out = it->second;
            found = true;
        }
        ReleaseSRWLockShared(&g_moveLock);
        return found;
    }

    int64_t __fastcall Detour_SetSpeed(void* process, float value)
    {
        MoveOverride m;
        if (process && LookupMove(process, m))
        {
            value = m.speed;
            g_dSpeedOverrides.fetch_add(1, std::memory_order_relaxed);
        }
        return g_origSetSpeed(process, value);
    }

    int64_t __fastcall Detour_SetDirection(void* process, float value)
    {
        MoveOverride m;
        if (process && LookupMove(process, m))
        {
            value = m.direction;
            g_dDirOverrides.fetch_add(1, std::memory_order_relaxed);
        }
        return g_origSetDir(process, value);
    }

    __declspec(noinline) bool SafeQueueStartCombat(Actor* puppet, Actor* target)
    {
        if (!g_queueStart || !puppet || !target)
            return false;
        __try
        {
            void* queue = *reinterpret_cast<void**>(g_base + kRVA_TaskQueue);
            if (!queue)
                return false;
            g_queueStart(queue, puppet, target, 0);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Local combat target of `a` (Actor+0x380 handle) resolved to an actor.
    Actor* LocalCombatTarget(Actor* a)
    {
        const uint32_t h = SafeReadU32(a, kActor_CombatTarget);
        if (h == 0 || h == 0xFFFFFFFFu)
            return nullptr;
        NiPointer<TESObjectREFR> ref;
        if (!LookupREFRByHandle(h, ref) || !ref.m_pObject)
            return nullptr;
        return reinterpret_cast<Actor*>(ref.m_pObject);
    }
}

namespace CoSyncPuppetAuthority
{
    void Install()
    {
        LOG_REQUIREMENT("CoSyncPuppetAuthority::Install", "Plugin load. Each target is prologue-checked against 1.11.240; a mismatch disables only that part.");
        if (g_installed)
            return;
        g_installed = true;
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        LoadConfig();
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        {
            LOG_ERROR("[PuppetAuthority] MH_Initialize failed: %d", static_cast<int>(st));
            return;
        }

        if (g_cfgCombatGate)
        {
            const bool qOk = PrologueOk(kRVA_QueueStartCombat, kPro_QueueStart, sizeof(kPro_QueueStart), "StartCombat task");
            if (qOk && PrologueOk(kRVA_CombatEntry, kPro_CombatEntry, sizeof(kPro_CombatEntry), "combat entry"))
            {
                g_queueStart = reinterpret_cast<QueueStart_t>(g_base + kRVA_QueueStartCombat);
                if (!HookOne(kRVA_CombatEntry, reinterpret_cast<void*>(&Detour_CombatEntry),
                             reinterpret_cast<void**>(&g_origCombatEntry), "combat entry"))
                    g_queueStart = nullptr;
            }
        }
        if (g_cfgMovement &&
            PrologueOk(kRVA_SetSpeed, kPro_SetSpeed, sizeof(kPro_SetSpeed), "Speed channel setter") &&
            PrologueOk(kRVA_SetDirection, kPro_SetDir, sizeof(kPro_SetDir), "Direction channel setter"))
        {
            if (HookOne(kRVA_SetSpeed, reinterpret_cast<void*>(&Detour_SetSpeed), reinterpret_cast<void**>(&g_origSetSpeed), "Speed channel setter"))
            {
                if (!HookOne(kRVA_SetDirection, reinterpret_cast<void*>(&Detour_SetDirection), reinterpret_cast<void**>(&g_origSetDir), "Direction channel setter"))
                {
                    MH_DisableHook(reinterpret_cast<void*>(g_base + kRVA_SetSpeed));
                    g_origSetSpeed = nullptr;
                }
            }
        }
        LOG_INFO("[PuppetAuthority] ready: combatGate=%d movementChannels=%d (ini bCombatGate=%d bMovementChannels=%d)",
            CombatGateActive() ? 1 : 0, MovementOverrideActive() ? 1 : 0, g_cfgCombatGate ? 1 : 0, g_cfgMovement ? 1 : 0);
    }

    void Reset()
    {
        {
            std::lock_guard<std::mutex> lk(g_combatMx);
            g_allowedTarget.clear();
        }
        g_combatApplied.clear();
        AcquireSRWLockExclusive(&g_moveLock);
        g_moveByProcess.clear();
        ReleaseSRWLockExclusive(&g_moveLock);
        g_processOfPuppet.clear();
    }

    bool CombatGateActive()
    {
        return g_origCombatEntry != nullptr && g_queueStart != nullptr;
    }

    bool MovementOverrideActive()
    {
        return g_origSetSpeed != nullptr && g_origSetDir != nullptr;
    }

    bool ReadMoveChannels(Actor* actor, float& speed, float& direction)
    {
        void* process = SafeReadPtr(actor, kActor_Process);
        void* mh = SafeReadPtr(process, kProcess_MiddleHigh);
        if (!mh)
            return false;
        return SafeReadFloat(mh, kMH_Speed, speed) && SafeReadFloat(mh, kMH_Direction, direction) &&
               std::isfinite(speed) && std::isfinite(direction);
    }

    void SetPuppetMovement(Actor* puppet, float speed, float direction, bool fresh)
    {
        if (!MovementOverrideActive() || !puppet)
            return;
        void* process = SafeReadPtr(puppet, kActor_Process);
        auto known = g_processOfPuppet.find(puppet);
        AcquireSRWLockExclusive(&g_moveLock);
        if (known != g_processOfPuppet.end() && known->second != process)
            g_moveByProcess.erase(known->second);   // the engine gave it a new process
        if (process && fresh)
            g_moveByProcess[process] = MoveOverride{ speed, direction, NowMs() + 400 };
        else if (process)
            g_moveByProcess.erase(process);
        ReleaseSRWLockExclusive(&g_moveLock);
        if (process)
            g_processOfPuppet[puppet] = process;
        else
            g_processOfPuppet.erase(puppet);
    }

    void ApplyOwnerCombat(uint32_t entityID, Actor* puppet, bool active, Actor* localTarget, double now)
    {
        if (!CombatGateActive() || !puppet)
            return;
        if (!active)
            localTarget = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_combatMx);
            g_allowedTarget[puppet] = localTarget;
        }
        CombatApply& c = g_combatApplied[puppet];
        const bool changed = (c.active != (localTarget != nullptr)) || (c.target != localTarget);
        c.active = (localTarget != nullptr);
        c.target = localTarget;

        if (CoSyncGameAPI::IsDeadOrDying(puppet))
            return;

        const bool inCombat = puppet->IsInCombat();
        if (localTarget)
        {
            // Start (or re-target) once per change, re-checked once a second in
            // case the engine dropped the combat on its own (target out of
            // reach, a script StopCombat).
            const bool onTarget = inCombat && LocalCombatTarget(puppet) == localTarget;
            if (!onTarget && (changed || now - c.lastStart >= 1.0))
            {
                if (SafeQueueStartCombat(puppet, localTarget))
                {
                    c.lastStart = now;
                    ++g_dStarts;
                    if (changed)
                        LOG_UPDATED("[PuppetAuthority] entity=%u: owner fights %p -- engine combat started on the puppet with the same target",
                            entityID, static_cast<void*>(localTarget));
                }
            }
        }
        else if (inCombat && (changed || now - c.lastStop >= 1.0))
        {
            // The owner is not fighting (or fights something not synced here):
            // no local fight may exist. Any local entry is refused by the gate,
            // so this only ends a fight the owner just ended.
            CoSyncGameAPI::StopActorCombat(puppet);
            c.lastStop = now;
            ++g_dStops;
            if (changed)
                LOG_UPDATED("[PuppetAuthority] entity=%u: owner's combat ended -- puppet combat stopped", entityID);
        }
    }

    void ForgetPuppet(Actor* puppet)
    {
        if (!puppet)
            return;
        {
            std::lock_guard<std::mutex> lk(g_combatMx);
            g_allowedTarget.erase(puppet);
        }
        g_combatApplied.erase(puppet);
        auto it = g_processOfPuppet.find(puppet);
        if (it != g_processOfPuppet.end())
        {
            AcquireSRWLockExclusive(&g_moveLock);
            g_moveByProcess.erase(it->second);
            ReleaseSRWLockExclusive(&g_moveLock);
            g_processOfPuppet.erase(it);
        }
    }

    void TickDiagnostics(double now)
    {
        static double s_next = 0.0;
        if (now < s_next)
            return;
        s_next = now + 10.0;

        // Puppets that left the guard set (freed, unbound, now owned here) must
        // not keep a gate entry or a channel override.
        for (auto it = g_combatApplied.begin(); it != g_combatApplied.end(); )
        {
            if (!CoSyncEnemySync::IsGuardedPuppet(it->first))
            {
                Actor* gone = it->first;
                ++it;
                ForgetPuppet(gone);
            }
            else
                ++it;
        }

        const uint32_t refused = g_dRefusedEntries.exchange(0), allowed = g_dAllowedEntries.exchange(0),
                       sp = g_dSpeedOverrides.exchange(0), dr = g_dDirOverrides.exchange(0);
        if (refused || allowed || sp || dr || g_dStarts || g_dStops)
            LOG_UPDATED("[PuppetAuthority] last 10 s: local combat entries refused=%u allowed(owner's target)=%u | owner combat started=%u stopped=%u | "
                        "locomotion channel writes replaced: speed=%u direction=%u | puppets steered=%zu",
                refused, allowed, g_dStarts, g_dStops, sp, dr, g_processOfPuppet.size());
        g_dStarts = g_dStops = 0;
    }
}
