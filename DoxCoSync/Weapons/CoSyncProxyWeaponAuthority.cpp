#include <Windows.h>
#include <intrin.h>

#include "CoSyncProxyWeaponAuthority.h"

#include "ConsoleLogger.h"
#include "MinHook.h"

#include <atomic>
#include <unordered_map>

#pragma intrinsic(_ReturnAddress)

namespace
{
    // Actor::DrawWeaponMagicHands(bool draw), Actor vtable (0x25683B0) slot
    // 0xC9. PlayerCharacter overrides the slot (sub_140DA13D0), so hooking
    // this body touches non-player actors only. 1.11.240, IDA-verified.
    constexpr uintptr_t kRVA_ActorDrawWeaponMagicHands = 0x00C5D5A0;

    // The middle-high AI process update that holsters an idle actor's weapon
    // (four calls through [vtable+0x648] inside it; the holster decision is
    // gated on "not the player", no combat, no alert and weapon state >= 3).
    constexpr uintptr_t kRVA_AiProcessUpdate      = 0x00D12010;
    constexpr uintptr_t kAiProcessUpdateSize      = 0x26FC;

    using DrawWeapon_t = char(__fastcall*)(Actor*, bool);
    DrawWeapon_t g_orig = nullptr;
    uintptr_t    g_base = 0;
    bool         g_installed = false;

    // Written on the game thread, read from whatever thread runs the AI
    // update: a reader/writer lock around a small map.
    SRWLOCK                          g_lock = SRWLOCK_INIT;
    std::unordered_map<Actor*, bool> g_drawn;   // actor -> replicated drawn

    thread_local int t_ownCall = 0;

    std::atomic<uint32_t> g_refused{ 0 };
    std::atomic<uint32_t> g_otherContradicting{ 0 };
    std::atomic<uintptr_t> g_lastOtherCallerRVA{ 0 };
    std::atomic<uint32_t> g_lastRefusedFormID{ 0 };

    bool Lookup(Actor* actor, bool& drawn)
    {
        AcquireSRWLockShared(&g_lock);
        auto it = g_drawn.find(actor);
        const bool found = (it != g_drawn.end());
        if (found) drawn = it->second;
        ReleaseSRWLockShared(&g_lock);
        return found;
    }

    __declspec(noinline) uint32_t SafeFormID(Actor* actor)
    {
        __try { return *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(actor) + 0x14); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    char __fastcall Detour_DrawWeaponMagicHands(Actor* actor, bool draw)
    {
        if (t_ownCall == 0 && actor)
        {
            bool wanted = false;
            if (Lookup(actor, wanted) && wanted != draw)
            {
                const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
                const uintptr_t lo  = g_base + kRVA_AiProcessUpdate;
                if (ret >= lo && ret < lo + kAiProcessUpdateSize)
                {
                    g_refused.fetch_add(1, std::memory_order_relaxed);
                    g_lastRefusedFormID.store(SafeFormID(actor), std::memory_order_relaxed);
                    return 0;
                }
                g_otherContradicting.fetch_add(1, std::memory_order_relaxed);
                g_lastOtherCallerRVA.store(ret - g_base, std::memory_order_relaxed);
            }
        }
        return g_orig(actor, draw);
    }
}

namespace CoSyncProxyWeaponAuthority
{
    void Install()
    {
        if (g_installed)
            return;
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));

        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        {
            LOG_ERROR("[WeaponAuthority] MH_Initialize failed: %d", (int)st);
            return;
        }
        void* target = reinterpret_cast<void*>(g_base + kRVA_ActorDrawWeaponMagicHands);
        st = MH_CreateHook(target, reinterpret_cast<void*>(&Detour_DrawWeaponMagicHands),
                           reinterpret_cast<void**>(&g_orig));
        if (st != MH_OK)
        {
            LOG_ERROR("[WeaponAuthority] MH_CreateHook(DrawWeaponMagicHands) failed: %d target=%p", (int)st, target);
            return;
        }
        st = MH_EnableHook(target);
        if (st != MH_OK)
        {
            LOG_ERROR("[WeaponAuthority] MH_EnableHook(DrawWeaponMagicHands) failed: %d", (int)st);
            return;
        }
        g_installed = true;
        LOG_INFO("[WeaponAuthority] Actor::DrawWeaponMagicHands hooked at base+0x%zX (AI update base+0x%zX..+0x%zX)",
            (size_t)kRVA_ActorDrawWeaponMagicHands, (size_t)kRVA_AiProcessUpdate,
            (size_t)(kRVA_AiProcessUpdate + kAiProcessUpdateSize));
    }

    void SetReplicatedDrawn(Actor* actor, bool drawn)
    {
        if (!actor)
            return;
        bool cur = false;
        if (Lookup(actor, cur) && cur == drawn)
            return;
        AcquireSRWLockExclusive(&g_lock);
        g_drawn[actor] = drawn;
        ReleaseSRWLockExclusive(&g_lock);
    }

    void Forget(Actor* actor)
    {
        if (!actor)
            return;
        AcquireSRWLockExclusive(&g_lock);
        g_drawn.erase(actor);
        ReleaseSRWLockExclusive(&g_lock);
    }

    void Reset()
    {
        AcquireSRWLockExclusive(&g_lock);
        g_drawn.clear();
        ReleaseSRWLockExclusive(&g_lock);
    }

    __declspec(noinline) bool IsEngineHolstered(Actor* actor)
    {
        if (!actor)
            return false;
        __try
        {
            const uint32_t flags = *reinterpret_cast<const uint32_t*>(
                reinterpret_cast<const uint8_t*>(actor) + 0x134);
            return ((flags >> 1) & 7u) == 0u;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    ScopedOwnCall::ScopedOwnCall()  { ++t_ownCall; }
    ScopedOwnCall::~ScopedOwnCall() { --t_ownCall; }

    void TickDiagnostics(double now)
    {
        static double s_next = 0.0;
        if (now < s_next)
            return;
        s_next = now + 10.0;
        const uint32_t refused = g_refused.exchange(0);
        const uint32_t other   = g_otherContradicting.exchange(0);
        if (refused)
            LOG_UPDATED("[WeaponAuthority] last 10 s: refused %u AI holster/draw call(s) that contradicted the owner's weapon state (last actor 0x%08X)",
                refused, g_lastRefusedFormID.load());
        if (other)
            LOG_UPDATED_WARN("[WeaponAuthority] last 10 s: %u engine call(s) OUTSIDE the AI update contradicted a proxy's replicated weapon state "
                "and were allowed (last caller base+0x%zX) -- investigate if the weapon still vanishes",
                other, (size_t)g_lastOtherCallerRVA.load());
    }
}
