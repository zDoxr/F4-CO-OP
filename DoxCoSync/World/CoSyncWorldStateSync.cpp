#define WIN32_LEAN_AND_MEAN
#include "CoSyncVault111.h"
#include <Windows.h>

#include "CoSyncWorldStateSync.h"
#include "CoSyncDoorHook.h"
#include "CoSyncPowerArmorSync.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "GameReferences.h"
#include "GameForms.h"
#include "GameHandle.h"
#include "GameObjects.h"
#include "MinHook.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

extern PlayerCharacter* CoSyncGetPlayer();

namespace
{
    // ── Engine (1.11.240, IDA-verified 2026-09-29) ──────────────────────────
    constexpr uintptr_t kRVA_DestructionEventSend = 0x0052C7F0;   // (NiPointer<ref>*, old, new)
    constexpr uintptr_t kRVA_GetDestructionStage  = 0x00300730;   // (ref) -> stage or -1
    constexpr uintptr_t kRVA_ExtraListHealth      = 0x0028CFD0;   // (ExtraDataList*) -> health or -1
    constexpr uintptr_t kRVA_DestructibleData     = 0x002FE110;   // (TESForm* base) -> BGSDestructibleObjectForm*
    constexpr uintptr_t kRVA_DestructibleMaxHP    = 0x002FE330;   // (data, ref) -> max health
    constexpr uintptr_t kRVA_QueueDamageObject    = 0x00C461E0;   // TaskQueueInterface task 7
    constexpr uintptr_t kRVA_TaskQueueInterface   = 0x032FF808;   // TaskQueueInterface* global
    constexpr uintptr_t kRVA_ClearDestruction     = 0x003014C0;   // (ref)
    constexpr uintptr_t kRVA_SetOpenState         = 0x00305A90;   // (ref, open, snap)
    constexpr uintptr_t kRVA_GetOpenState         = 0x00305CE0;   // (ref) -> 0 none,1 open,2 opening,3 closed,4 closing
    constexpr uintptr_t kRVA_RefEnable            = 0x00515970;   // TESObjectREFR::Enable(ref, bool)
    constexpr uintptr_t kRVA_EnableFunctorVtbl    = 0x025CCA58;
    constexpr uintptr_t kRVA_EnableFunctorRun     = 0x01150560;
    constexpr uintptr_t kRVA_DisableFunctorVtbl   = 0x025CCAF8;
    constexpr uintptr_t kRVA_DisableFunctorRun    = 0x011508C0;
    constexpr uintptr_t kRef_ExtraList            = 0x100;        // TESObjectREFR::extraDataList
    constexpr uint32_t  kRefFlag_Disabled         = 0x800;

    struct Prologue { uintptr_t rva; uint8_t bytes[12]; size_t n; };
    const Prologue kPrologues[] = {
        { kRVA_DestructionEventSend, { 0x48, 0x83, 0xEC, 0x38, 0x48, 0x8B, 0x01, 0x48, 0x85, 0xC0, 0x74, 0x04 }, 12 },
        { kRVA_GetDestructionStage,  { 0x40, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF1, 0x48, 0x85, 0xC9 }, 12 },
        { kRVA_ExtraListHealth,      { 0x48, 0x83, 0xEC, 0x28, 0xB2, 0x55 }, 6 },
        { kRVA_DestructibleData,     { 0x45, 0x33, 0xC0, 0x48, 0x85, 0xC9, 0x0F, 0x84 }, 8 },
        { kRVA_DestructibleMaxHP,    { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x85, 0xD2 }, 9 },
        { kRVA_QueueDamageObject,    { 0x48, 0x85, 0xD2, 0x0F, 0x84, 0xEF, 0x00, 0x00, 0x00 }, 9 },
        { kRVA_ClearDestruction,     { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x50, 0xF6, 0x41, 0x13, 0x01 }, 10 },
        { kRVA_SetOpenState,         { 0x40, 0x55, 0x53, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24 }, 12 },
        { kRVA_GetOpenState,         { 0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x33, 0xDB, 0x48, 0x8B, 0xF9 }, 12 },
        { kRVA_RefEnable,            { 0x40, 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x58, 0x8B, 0x41, 0x10 }, 11 },
    };

    using DestrSend_t   = int64_t(__fastcall*)(TESObjectREFR** ref, int oldStage, int newStage);
    using GetStage_t    = int(__fastcall*)(TESObjectREFR* ref);
    using ListHealth_t  = float(__fastcall*)(void* extraList);
    using DestrData_t   = void*(__fastcall*)(TESForm* base);
    using MaxHP_t       = int(__fastcall*)(void* data, TESObjectREFR* ref);
    using QueueDamage_t = void(__fastcall*)(void* taskQueue, TESObjectREFR* ref, float damage, int64_t a4, char a5);
    using ClearDestr_t  = void(__fastcall*)(TESObjectREFR* ref);
    using SetOpen_t     = uint64_t(__fastcall*)(TESObjectREFR* ref, uint8_t open, uint8_t snap);
    using GetOpen_t     = int(__fastcall*)(TESObjectREFR* ref);
    using Enable_t      = void(__fastcall*)(TESObjectREFR* ref, bool resetInventory);
    using Disable_t     = void(__fastcall*)(TESObjectREFR* ref);
    using FunctorRun_t  = void*(__fastcall*)(void* functor, void* out);

    uintptr_t     g_base = 0;
    bool          g_installed = false;
    bool          g_engineOk = false;
    DestrSend_t   g_origDestrSend = nullptr;
    SetOpen_t     g_origSetOpen = nullptr;
    FunctorRun_t  g_origEnableRun = nullptr;
    FunctorRun_t  g_origDisableRun = nullptr;

    thread_local int t_applying = 0;   // >0 while this module applies a remote change

    template <class T> T Fn(uintptr_t rva) { return reinterpret_cast<T>(g_base + rva); }

    // ── Captured local changes (hooks, any thread) ──────────────────────────
    enum class Kind : uint8_t { Destruction, Open, Enable };
    struct Change
    {
        Kind     kind;
        uint32_t refFormID;
        int32_t  a;   // Destruction: old stage | Open: open | Enable: enabled
        int32_t  b;   // Destruction: new stage
    };
    std::mutex          g_capMx;
    std::vector<Change> g_captured;

    void Capture(const Change& c)
    {
        std::lock_guard<std::mutex> lk(g_capMx);
        if (g_captured.size() < 512)
            g_captured.push_back(c);
    }

    // ── Queued remote changes (applied on the game thread) ──────────────────
    struct Remote
    {
        Kind     kind;
        uint32_t refFormID;
        uint32_t baseFormID;
        float    x, y, z;
        int32_t  a, b;
        float    health;
        int      tries;
        double   nextTry;
    };
    std::mutex          g_rxMx;
    std::vector<Remote> g_rx;
    std::vector<Remote> g_retry;   // game thread only

    // Diagnostics
    std::atomic<uint32_t> g_capDestr{ 0 }, g_capOpen{ 0 }, g_capEnable{ 0 };
    uint32_t g_txCount = 0, g_rxCount = 0, g_applied = 0, g_already = 0, g_unresolved = 0, g_gaveUp = 0;

    double NowSec()
    {
        static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
    }

    __declspec(noinline) uint32_t SafeU32(const void* p, uintptr_t off)
    {
        __try { return p ? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p) + off) : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    TESObjectREFR* RefFromHandle(uint32_t handle)
    {
        if (handle == 0)
            return nullptr;
        NiPointer<TESObjectREFR> ref;
        if (!LookupREFRByHandle(handle, ref))
            return nullptr;
        return ref.m_pObject;
    }

    bool IsDynamic(uint32_t formID) { return (formID >> 24) == 0xFF; }

    // ── Hooks ────────────────────────────────────────────────────────────────
    int64_t __fastcall Detour_DestructionSend(TESObjectREFR** refPtr, int oldStage, int newStage)
    {
        if (t_applying == 0 && refPtr)
        {
            const uint32_t fid = SafeU32(*refPtr, 0x14);
            if (fid != 0)
            {
                Capture({ Kind::Destruction, fid, oldStage, newStage });
                ++g_capDestr;
            }
        }
        return g_origDestrSend(refPtr, oldStage, newStage);
    }

    int SafeGetOpenState(TESObjectREFR* ref)
    {
        __try { return Fn<GetOpen_t>(kRVA_GetOpenState)(ref); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    uint64_t __fastcall Detour_SetOpenState(TESObjectREFR* ref, uint8_t open, uint8_t snap)
    {
        // Only animated (snap == 0) changes that nobody else is already
        // replicating: CoSync's own replay, and player activations (DOOR),
        // pass straight through. Snapped calls are state restores (cell
        // load, Lock's forced close -- LOCK is synced by CoSyncLockHook).
        if (t_applying != 0 || snap != 0 || !ref || CoSyncDoorHook::InLocalActivation() || !CoSyncNet::IsConnected())
            return g_origSetOpen(ref, open, snap);
        const int prev = SafeGetOpenState(ref);
        const uint64_t r = g_origSetOpen(ref, open, snap);
        if (prev != 0)
        {
            const bool wasOpen = (prev == 1 || prev == 2);
            if (wasOpen != (open != 0))
            {
                const uint32_t fid = SafeU32(ref, 0x14);
                if (fid != 0)
                {
                    Capture({ Kind::Open, fid, open ? 1 : 0, 0 });
                    ++g_capOpen;
                }
            }
        }
        return r;
    }

    // Papyrus Enable/Disable functors: the ref is resolved from the handle
    // the functor stores (+0x20 Enable, +0x18 Disable -- read by the run
    // functions themselves, sub_14021E490(&ref, functor + off)).
    void* __fastcall Detour_EnableRun(void* functor, void* out)
    {
        TESObjectREFR* ref = CoSyncNet::IsHost() ? RefFromHandle(SafeU32(functor, 0x20)) : nullptr;
        const bool wasDisabled = ref && (SafeU32(ref, 0x10) & kRefFlag_Disabled);
        void* r = g_origEnableRun(functor, out);
        if (ref && wasDisabled && !(SafeU32(ref, 0x10) & kRefFlag_Disabled))
        {
            Capture({ Kind::Enable, SafeU32(ref, 0x14), 1, 0 });
            ++g_capEnable;
        }
        return r;
    }

    void* __fastcall Detour_DisableRun(void* functor, void* out)
    {
        TESObjectREFR* ref = CoSyncNet::IsHost() ? RefFromHandle(SafeU32(functor, 0x18)) : nullptr;
        const bool wasEnabled = ref && !(SafeU32(ref, 0x10) & kRefFlag_Disabled);
        void* r = g_origDisableRun(functor, out);
        if (ref && wasEnabled && (SafeU32(ref, 0x10) & kRefFlag_Disabled))
        {
            Capture({ Kind::Enable, SafeU32(ref, 0x14), 0, 0 });
            ++g_capEnable;
        }
        return r;
    }

    bool HookFn(uintptr_t rva, void* detour, void** orig, const char* name)
    {
        void* target = reinterpret_cast<void*>(g_base + rva);
        MH_STATUS st = MH_CreateHook(target, detour, orig);
        if (st == MH_OK)
            st = MH_EnableHook(target);
        if (st != MH_OK)
        {
            LOG_ERROR("[WorldState] hooking %s failed: %d", name, (int)st);
            MH_RemoveHook(target);
            *orig = nullptr;
            return false;
        }
        LOG_INFO("[WorldState] %s hooked at base+0x%llX", name, (unsigned long long)rva);
        return true;
    }

    bool SwapSlot(uintptr_t vtblRva, uintptr_t expectRva, void* detour, void** orig, const char* name)
    {
        void** slot = reinterpret_cast<void**>(g_base + vtblRva + sizeof(void*));
        if (reinterpret_cast<uintptr_t>(*slot) != g_base + expectRva)
        {
            LOG_ERROR("[WorldState] %s slot 1 = %p, expected base+0x%llX -- not installed", name, *slot, (unsigned long long)expectRva);
            return false;
        }
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
            return false;
        *orig = *slot;
        *slot = detour;
        VirtualProtect(slot, sizeof(void*), old, &old);
        LOG_INFO("[WorldState] %s run slot swapped (vtable base+0x%llX)", name, (unsigned long long)vtblRva);
        return true;
    }

    // ── Game-thread engine helpers ──────────────────────────────────────────
    __declspec(noinline) int SafeStage(TESObjectREFR* ref)
    {
        __try { return Fn<GetStage_t>(kRVA_GetDestructionStage)(ref); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    __declspec(noinline) float SafeHealth(TESObjectREFR* ref, bool orMax)
    {
        __try
        {
            void* list = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ref) + kRef_ExtraList);
            float h = list ? Fn<ListHealth_t>(kRVA_ExtraListHealth)(list) : -1.0f;
            if (h < 0.0f && orMax)
            {
                void* data = Fn<DestrData_t>(kRVA_DestructibleData)(ref->baseForm);
                h = data ? static_cast<float>(Fn<MaxHP_t>(kRVA_DestructibleMaxHP)(data, ref)) : -1.0f;
            }
            return h;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1.0f; }
    }

    __declspec(noinline) bool SafeDamage(TESObjectREFR* ref, float dmg)
    {
        __try
        {
            void* tq = *reinterpret_cast<void**>(g_base + kRVA_TaskQueueInterface);
            if (!tq)
                return false;
            ++t_applying;
            Fn<QueueDamage_t>(kRVA_QueueDamageObject)(tq, ref, dmg, 0, 1);
            --t_applying;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { t_applying = 0; return false; }
    }

    __declspec(noinline) bool SafeClear(TESObjectREFR* ref)
    {
        __try { ++t_applying; Fn<ClearDestr_t>(kRVA_ClearDestruction)(ref); --t_applying; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { t_applying = 0; return false; }
    }

    __declspec(noinline) bool SafeSetOpen(TESObjectREFR* ref, bool open)
    {
        __try { ++t_applying; g_origSetOpen(ref, open ? 1 : 0, 0); --t_applying; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { t_applying = 0; return false; }
    }

    __declspec(noinline) bool SafeEnable(TESObjectREFR* ref, bool enable)
    {
        __try
        {
            ++t_applying;
            if (enable)
                Fn<Enable_t>(kRVA_RefEnable)(ref, false);
            else
                reinterpret_cast<Disable_t>((*reinterpret_cast<void***>(ref))[0xB0])(ref);
            --t_applying;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { t_applying = 0; return false; }
    }

    TESObjectREFR* ResolveRef(uint32_t refFormID, uint32_t baseFormID, float x, float y, float z, bool allowDynamic)
    {
        if (!IsDynamic(refFormID))
        {
            TESForm* f = LookupFormByID(refFormID);
            if (f && f->formType == kFormType_REFR)
                return static_cast<TESObjectREFR*>(f);
            return nullptr;
        }
        if (!allowDynamic)
            return nullptr;
        // A runtime ref: same base, same place, in the local player's cell.
        PlayerCharacter* pc = CoSyncGetPlayer();
        TESObjectCELL* cell = pc ? reinterpret_cast<TESObjectREFR*>(pc)->parentCell : nullptr;
        if (!cell)
            return nullptr;
        for (UInt32 i = 0; i < cell->objectList.count; ++i)
        {
            TESObjectREFR* r = cell->objectList.entries[i];
            if (!r || !r->baseForm || r->baseForm->formID != baseFormID)
                continue;
            const float dx = r->pos.x - x, dy = r->pos.y - y, dz = r->pos.z - z;
            if (dx * dx + dy * dy + dz * dz <= 96.0f * 96.0f)
                return r;
        }
        return nullptr;
    }

    // 1 = applied/nothing to do, 0 = retry later, -1 = drop
    int ApplyOne(const Remote& op)
    {
        const bool dynamicOk = op.kind != Kind::Enable;
        TESObjectREFR* ref = ResolveRef(op.refFormID, op.baseFormID, op.x, op.y, op.z, dynamicOk);
        if (!ref)
        {
            ++g_unresolved;
            return 0;   // not loaded here (yet)
        }
        if (op.kind == Kind::Destruction)
        {
            const int local = SafeStage(ref);
            if (local < 0)
                return -1;   // not destructible here
            if (op.b == 0)
            {
                if (local == 0) { ++g_already; return 1; }
                if (op.a > 0 && SafeClear(ref)) { ++g_applied; return 1; }
                return -1;
            }
            if (local >= op.b) { ++g_already; return 1; }   // monotonic: never undone by a stale report
            const float cur = SafeHealth(ref, true);
            float dmg = (cur >= 0.0f && op.health >= 0.0f) ? cur - op.health : 0.0f;
            if (!(dmg > 0.0f))
                dmg = 1.0f;
            SafeDamage(ref, dmg);
            if (SafeStage(ref) >= op.b) { ++g_applied; return 1; }
            return 0;        // unloaded refs may not transition until their 3D exists: retry
        }
        // 0.5.6: Vault 111's own refs (the two intro cells, the exit's
        // triggers and markers) follow this machine's scripts until this
        // player is out: the host's exit disabled the client's elevator
        // trigger (WSE|134202|0, 2026-10-05 22:30:03) and its magic door to
        // the post-war vault (WSE|1346408|0, 22:24:09). CoSyncVault111.
        if ((op.kind == Kind::Open || op.kind == Kind::Enable) && CoSyncVault111::IgnoreWorldStateFor(ref))
        {
            ++g_already;
            return -1;
        }
        if (op.kind == Kind::Open)
        {
            const int st = SafeGetOpenState(ref);
            if (st == 0)
                return -1;
            const bool isOpen = (st == 1 || st == 2);
            if (isOpen == (op.a != 0)) { ++g_already; return 1; }
            return SafeSetOpen(ref, op.a != 0) ? (++g_applied, 1) : -1;
        }
        // Enable / Disable (from the host's scripts)
        if (CoSyncPowerArmorSync::IsCoSyncManagedFrame(ref->formID))
            return -1;       // a frame CoSync is representing here
        const bool disabled = (ref->flags & kRefFlag_Disabled) != 0;
        if (disabled != (op.a != 0)) { ++g_already; return 1; }
        return SafeEnable(ref, op.a != 0) ? (++g_applied, 1) : -1;
    }

    void SendCaptured()
    {
        std::vector<Change> caps;
        {
            std::lock_guard<std::mutex> lk(g_capMx);
            if (g_captured.empty())
                return;
            caps.swap(g_captured);
        }
        if (!CoSyncNet::IsConnected())
            return;
        for (const Change& c : caps)
        {
            TESForm* f = LookupFormByID(c.refFormID);
            if (!f || f->formType != kFormType_REFR)
                continue;   // actors are the NPC sync's business
            TESObjectREFR* ref = static_cast<TESObjectREFR*>(f);
            const uint32_t base = ref->baseForm ? ref->baseForm->formID : 0;
            char buf[192];
            if (c.kind == Kind::Destruction)
            {
                std::snprintf(buf, sizeof(buf), "WSD|%u|%u|%.1f|%.1f|%.1f|%d|%d|%.2f",
                    c.refFormID, base, ref->pos.x, ref->pos.y, ref->pos.z, c.a, c.b, SafeHealth(ref, false));
            }
            else if (c.kind == Kind::Open)
            {
                std::snprintf(buf, sizeof(buf), "WSO|%u|%u|%.1f|%.1f|%.1f|%d",
                    c.refFormID, base, ref->pos.x, ref->pos.y, ref->pos.z, c.a);
            }
            else
            {
                if (!CoSyncNet::IsHost() || IsDynamic(c.refFormID))
                    continue;
                std::snprintf(buf, sizeof(buf), "WSE|%u|%d", c.refFormID, c.a);
            }
            CoSyncTransport::Send(std::string(buf));
            ++g_txCount;
            if (g_txCount <= 30 || g_txCount % 100 == 0)
                LOG_UPDATED("[WorldState] TX %s", buf);
        }
    }

    void ApplyQueued(double now)
    {
        {
            std::lock_guard<std::mutex> lk(g_rxMx);
            for (Remote& r : g_rx)
            {
                // The newest report for a ref replaces an older pending one of the same kind.
                for (auto it = g_retry.begin(); it != g_retry.end(); )
                    it = (it->refFormID == r.refFormID && it->kind == r.kind && it->baseFormID == r.baseFormID)
                        ? g_retry.erase(it) : std::next(it);
                g_retry.push_back(r);
            }
            g_rx.clear();
        }
        for (auto it = g_retry.begin(); it != g_retry.end(); )
        {
            if (now < it->nextTry) { ++it; continue; }
            const int res = ApplyOne(*it);
            if (res != 0) { it = g_retry.erase(it); continue; }
            // Not loaded yet: keep it for up to ~10 minutes (the player may walk there).
            if (++it->tries > 300) { ++g_gaveUp; it = g_retry.erase(it); continue; }
            it->nextTry = now + 2.0;
            ++it;
        }
        if (g_retry.size() > 2048)
            g_retry.erase(g_retry.begin(), g_retry.begin() + (g_retry.size() - 2048));
    }
}

namespace CoSyncWorldStateSync
{
    void Install()
    {
        LOG_REQUIREMENT("CoSyncWorldStateSync::Install", "Plugin load. Every engine function is prologue-checked and every vtable slot compared before anything is hooked; any mismatch disables that part.");
        if (g_installed)
            return;
        g_installed = true;
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        for (const Prologue& p : kPrologues)
        {
            if (std::memcmp(reinterpret_cast<void*>(g_base + p.rva), p.bytes, p.n) != 0)
            {
                LOG_ERROR("[WorldState] base+0x%llX does not match 1.11.240 -- world-state sync disabled",
                    (unsigned long long)p.rva);
                return;
            }
        }
        g_engineOk = true;
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
            return;
        HookFn(kRVA_DestructionEventSend, reinterpret_cast<void*>(&Detour_DestructionSend),
               reinterpret_cast<void**>(&g_origDestrSend), "TESDestructionStageChangedEvent send");
        HookFn(kRVA_SetOpenState, reinterpret_cast<void*>(&Detour_SetOpenState),
               reinterpret_cast<void**>(&g_origSetOpen), "BGSOpenCloseForm::SetOpenState");
        SwapSlot(kRVA_EnableFunctorVtbl, kRVA_EnableFunctorRun, reinterpret_cast<void*>(&Detour_EnableRun),
                 reinterpret_cast<void**>(&g_origEnableRun), "Papyrus EnableFunctor");
        SwapSlot(kRVA_DisableFunctorVtbl, kRVA_DisableFunctorRun, reinterpret_cast<void*>(&Detour_DisableRun),
                 reinterpret_cast<void**>(&g_origDisableRun), "Papyrus DisableFunctor");
    }

    bool IsWorldStateMsg(const std::string& msg)
    {
        return msg.size() > 4 && msg[0] == 'W' && msg[1] == 'S' && (msg[2] == 'D' || msg[2] == 'O' || msg[2] == 'E') && msg[3] == '|';
    }

    void OnReceive(const std::string& msg, bool isHostRole)
    {
        LOG_REQUIREMENT("CoSyncWorldStateSync::OnReceive", "Parses peer text defensively and only queues; the engine work happens in Tick on the game thread.");
        Remote r{};
        const char k = msg[2];
        const char* s = msg.c_str() + 4;
        char* e = nullptr;
        r.refFormID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
        if (!e || e == s || r.refFormID == 0)
            return;
        if (k == 'E')
        {
            // Script enable/disable is the host's word only.
            if (isHostRole || *e != '|')
                return;
            r.kind = Kind::Enable;
            r.a = std::atoi(e + 1) ? 1 : 0;
        }
        else
        {
            if (*e != '|') return; s = e + 1;
            r.baseFormID = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return; s = e + 1;
            r.x = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
            r.y = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
            r.z = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
            r.a = static_cast<int32_t>(std::strtol(s, &e, 10));
            if (k == 'D')
            {
                if (!e || *e != '|') return; s = e + 1;
                r.b = static_cast<int32_t>(std::strtol(s, &e, 10)); if (!e || *e != '|') return; s = e + 1;
                r.health = std::strtof(s, &e);
                r.kind = Kind::Destruction;
            }
            else
            {
                r.kind = Kind::Open;
            }
        }
        ++g_rxCount;
        std::lock_guard<std::mutex> lk(g_rxMx);
        if (g_rx.size() < 1024)
            g_rx.push_back(r);
    }

    void Tick(double now)
    {
        if (!g_engineOk)
            return;
        SendCaptured();
        ApplyQueued(now);

        static double s_lastDiag = 0.0;
        if (now - s_lastDiag >= 10.0)
        {
            s_lastDiag = now;
            const uint32_t cd = g_capDestr.exchange(0), co = g_capOpen.exchange(0), ce = g_capEnable.exchange(0);
            if (cd || co || ce || g_txCount || g_rxCount || g_applied)
                LOG_UPDATED("[WorldState] last 10 s: captured destruction=%u open=%u enable=%u | tx=%u rx=%u applied=%u already=%u unresolved=%u gaveUp=%u pending=%u",
                    cd, co, ce, g_txCount, g_rxCount, g_applied, g_already, g_unresolved, g_gaveUp, (unsigned)g_retry.size());
            g_txCount = g_rxCount = g_applied = g_already = g_unresolved = g_gaveUp = 0;
        }
    }

    void Reset()
    {
        {
            std::lock_guard<std::mutex> lk(g_capMx);
            g_captured.clear();
        }
        {
            std::lock_guard<std::mutex> lk(g_rxMx);
            g_rx.clear();
        }
        g_retry.clear();
    }
}
