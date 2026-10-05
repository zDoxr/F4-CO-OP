#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncEnemySync.h"
#include "CoSyncNpcWorldSync.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncEntityState.h"
#include "CoSyncHealthSync.h"
#include "CoSyncActorValueSafety.h"
#include "CoSyncGameAPI.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "TickHook.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "GameReferences.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "GameHandle.h"
#include "PapyrusVM.h"
#include "MinHook.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern PlayerCharacter*    CoSyncGetPlayer();
extern CoSyncPlayerManager g_CoSyncPlayerManager;

namespace
{
    // ── Engine addresses (Fallout4.exe 1.11.240, IDA 2026-09-30) ────────────
    // docs/ENEMY_SYNC_REPORT.md section 8 has callers, xrefs and how each was read.
    constexpr uintptr_t kRVA_KillCore      = 0x00C61800;   // (victim, killer, float dmg, bool silent, bool ragdollInstant)
    constexpr uintptr_t kRVA_KillImpl      = 0x00CA00C0;   // Actor vfunc 0x117
    constexpr uintptr_t kRVA_ModActorValue = 0x00CC9B70;   // (actor, mode, avi, float delta, attacker)
    constexpr uintptr_t kRVA_SpellCast     = 0x0113F810;   // Papyrus Spell.Cast native (vm, stackId, spell, source, target)
    constexpr uintptr_t kRVA_HitCtor       = 0x00E36380;   // HitData::HitData
    constexpr uintptr_t kRVA_HitInit       = 0x00E37510;   // (hit, aggressor, target, float dmg)
    constexpr uintptr_t kRVA_HitDtor       = 0x00E366A0;
    constexpr uintptr_t kRVA_HitEventSend  = 0x0052D0B0;   // TESHitEvent from HitData (DoHitMe 0xCD2E3A)
    constexpr uintptr_t kRVA_HasKeyword    = 0x01143FC0;   // Papyrus Form.HasKeyword native (vm, stackId, form, kw)
    constexpr uintptr_t kRVA_HasLocRefType = 0x01158090;   // Papyrus ObjectReference.HasLocRefType native (vm, stackId, ref, lcrt)
    constexpr uintptr_t kRVA_CombatState   = 0x00CCF410;   // GetCombatState impl (actor) -> 0/1/2

    constexpr uint8_t kPro_KillCore[15]   = { 0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x40 };
    constexpr uint8_t kPro_KillImpl[13]   = { 0x44,0x88,0x4C,0x24,0x20,0x48,0x89,0x54,0x24,0x10,0x55,0x53,0x56 };
    constexpr uint8_t kPro_ModAV[15]      = { 0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x6C,0x24,0x20,0x57,0x41,0x56,0x41,0x57 };
    constexpr uint8_t kPro_SpellCast[13]  = { 0x40,0x53,0x48,0x83,0xEC,0x40,0x83,0x3D,0x73,0x64,0xD3,0x02,0x02 };
    constexpr uint8_t kPro_HitCtor[15]    = { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18 };
    constexpr uint8_t kPro_HitInit[15]    = { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x30 };
    constexpr uint8_t kPro_HitDtor[13]    = { 0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xF9 };
    constexpr uint8_t kPro_HitEvent[9]    = { 0x40,0x53,0x48,0x81,0xEC,0x30,0x01,0x00,0x00 };
    constexpr uint8_t kPro_HasKeyword[15] = { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7C,0x24,0x18 };
    constexpr uint8_t kPro_HasLocRef[12]  = { 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x40,0x8B,0xFA };
    constexpr uint8_t kPro_CombatState[12]= { 0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0x01,0x48,0x8B,0xD9 };

    constexpr uintptr_t kActor_CombatTarget = 0x380;   // BSPointerHandle (GetCombatTarget native 0x10F8E00)
    constexpr uint32_t  kUiFlag_RuntimeEssential = 0x40000;
    constexpr uint32_t  kUiFlag_RuntimeProtected = 0x80000;

    // Fallout4.esm forms (docs/ENEMY_SYNC_REPORT.md 1.4-1.6).
    constexpr uint32_t kFID_Health            = 0x000002D4;
    constexpr uint32_t kFID_EncTypeLegendary  = 0x001CF26C;
    constexpr uint32_t kFID_BossLocRefTypes   = 0x000B6EE9;   // FLST BossLocRefTypeList
    constexpr uint32_t kFID_LocRefBoss        = 0x00003956;   // LCRT Boss
    constexpr uint32_t kFID_ActorTypeNPC      = 0x00013794;
    constexpr uint32_t kFID_ActorTypeCreature = 0x00013795;
    constexpr uint32_t kFID_ActorTypeAnimal   = 0x00013798;
    constexpr uint32_t kFID_ActorTypeRobot    = 0x0002CB73;
    constexpr uint32_t kFID_ActorTypeTurret   = 0x000B2BF3;

    using KillCore_t   = void*   (__fastcall*)(Actor*, Actor*, float, bool, bool);
    using KillImpl_t   = uint64_t(__fastcall*)(Actor*, Actor*, float, bool, bool);
    using ModAV_t      = uint64_t(__fastcall*)(Actor*, int, void*, float, Actor*);
    using SpellCast_t  = uint8_t (__fastcall*)(void*, uint32_t, TESForm*, TESObjectREFR*, TESObjectREFR*);
    using HitCtor_t    = void*   (__fastcall*)(void*);
    using HitInit_t    = uint64_t(__fastcall*)(void*, Actor*, Actor*, float);
    using HitDtor_t    = uint64_t(__fastcall*)(void*);
    using HitEvent_t   = uint64_t(__fastcall*)(void*);
    using HasKeyword_t = uint8_t (__fastcall*)(void*, uint32_t, TESForm*, TESForm*);
    using HasLocRef_t  = uint8_t (__fastcall*)(void*, uint32_t, TESObjectREFR*, TESForm*);
    using CombatState_t= int32_t (__fastcall*)(Actor*);

    uintptr_t g_base = 0;
    bool      g_installed = false;

    // Hook trampolines (null = that hook is not installed).
    KillCore_t  g_origKillCore = nullptr;
    KillImpl_t  g_origKillImpl = nullptr;
    ModAV_t     g_origModAV    = nullptr;
    SpellCast_t g_origCast     = nullptr;
    // Direct callables (prologue-checked; null = unavailable).
    KillCore_t    g_killCore    = nullptr;   // trampoline if hooked, else the function itself
    ModAV_t       g_modAV       = nullptr;
    SpellCast_t   g_spellCast   = nullptr;
    HitCtor_t     g_hitCtor     = nullptr;
    HitInit_t     g_hitInit     = nullptr;
    HitDtor_t     g_hitDtor     = nullptr;
    HitEvent_t    g_hitEvent    = nullptr;
    HasKeyword_t  g_hasKeyword  = nullptr;
    HasLocRef_t   g_hasLocRef   = nullptr;
    CombatState_t g_combatState = nullptr;

    std::atomic<void*> g_healthAVI{ nullptr };   // published by the tick; read by the ModAV hook

    // ── Guard set: puppets this machine must not let die, mutate or report ──
    // Rebuilt every tick from the player manager (world-actor entities this
    // machine renders but does not own). Hooks run on the main, job and VM
    // threads, so they only ever test this set, never the game-thread maps.
    std::mutex                 g_guardMx;
    std::unordered_set<Actor*> g_guard;

    // Player proxies (0.4.3): this machine's copies of the other players.
    std::unordered_set<Actor*> g_playerProxies;   // under g_guardMx

    // 0.4.3 authoritative puppet health (see the header). Thread-local depth:
    // CoSync's own writes set it; everything else on a puppet is refused.
    thread_local int tls_healthWriteDepth = 0;
    bool g_cfgAuthoritativeHealth = true;
    std::atomic<uint32_t> g_dRefusedHealthWrites{ 0 };

    bool IsGuarded(Actor* a)
    {
        if (!a)
            return false;
        {
            std::lock_guard<std::mutex> lk(g_guardMx);
            if (g_guard.find(a) == g_guard.end())
                return false;
        }
        return IsRegisteredRemoteActor(a);   // freed/reused pointers leave that set at once
    }

    // ── Captures from the hooks (any thread) ────────────────────────────────
    struct CapKill { uint32_t victimFid; uint32_t killerFid; bool ragdoll; };
    struct CapHit  { uint32_t targetFid; uint32_t attackerFid; float damage; };
    struct CapCast { uint32_t spellFid;  uint32_t sourceFid; };
    std::mutex           g_capMx;
    std::vector<CapKill> g_capKills;
    std::vector<CapHit>  g_capHits;
    std::vector<CapCast> g_capCasts;

    // ── Received (game thread, queued by OnReceive) ─────────────────────────
    struct RxHit { uint32_t target, attacker, seq; float damage; uint32_t flags; };
    struct RxState
    {
        uint32_t eid = 0, ver = 0, combat = 0, target = 0, cls = 0, life = 0;
        std::vector<uint32_t> casts;
    };
    std::mutex             g_rxMx;
    std::vector<RxHit>     g_rxHits;
    std::vector<RxState>   g_rxStates;

    // ── Owner-side state per entity ─────────────────────────────────────────
    struct OwnerState
    {
        uint32_t ver = 0, combat = 0, target = 0, cls = 0, life = 0;
        std::vector<uint32_t> casts;
        double   lastSent = -1.0, clsAt = -1.0;
        bool     dirty = true;
        float    lastHp = -1.f;        // 0.4.3: HP last sent from here
        double   lastHpSent = -1.0;
    };
    std::unordered_map<uint32_t, OwnerState> g_owned;
    uint32_t g_dTxHp = 0;

    // ── Receiver-side state per entity ──────────────────────────────────────
    struct RemoteState
    {
        RxState s;
        Actor*  appliedActor = nullptr;
        std::unordered_set<uint32_t> appliedCasts;
        double  lastRx = 0.0;
    };
    std::unordered_map<uint32_t, RemoteState> g_remote;

    // Death bookkeeping (owner): who killed it, how.
    struct DeathInfo { uint32_t killer = 0; uint32_t flags = 0; };
    std::unordered_map<uint32_t, DeathInfo> g_deaths;   // announced deaths

    // Replayed kills waiting for the engine task (receiver).
    struct PendingKill { uint32_t eid; Actor* actor; double since; };
    std::vector<PendingKill> g_pendingKills;
    std::unordered_set<uint32_t> g_killReplayedEids;   // entities whose death already played here

    // Hit sequencing.
    std::unordered_map<uint32_t, uint32_t> g_txHitSeq;                    // attacker -> last sent
    std::unordered_map<uint32_t, uint32_t> g_rxHitSeq;                    // attacker -> last applied
    std::unordered_map<uint32_t, std::pair<double, int>> g_hitBudget;     // attacker -> (window start, count)

    // Diagnostics (10 s window).
    std::atomic<uint32_t> g_dRefusedKills{ 0 }, g_dRefusedCasts{ 0 }, g_dCapHits{ 0 }, g_dCapKills{ 0 }, g_dCapCasts{ 0 };
    uint32_t g_dTxHit = 0, g_dRxHitApplied = 0, g_dRxHitRejected = 0, g_dTxKill = 0, g_dTxState = 0,
             g_dRxState = 0, g_dReplayKill = 0, g_dReplayKillFallback = 0, g_dReplayCast = 0, g_dTxBytes = 0,
             g_dRejDup = 0, g_dRejDead = 0, g_dRejNoActor = 0;

    std::atomic<uint32_t> g_localPlayerFid{ 0x14 };

    // ── Small SEH helpers (no unwinding objects inside) ─────────────────────
    __declspec(noinline) uint32_t SafeU32(const void* p, uintptr_t off)
    {
        __try { return p ? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p) + off) : 0u; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
    }
    __declspec(noinline) uint8_t SafeFormType(const TESForm* f)
    {
        __try { return f ? f->formType : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    uint32_t FormIdOf(const void* f) { return SafeU32(f, 0x14); }

    bool PrologueOk(uintptr_t rva, const uint8_t* bytes, size_t n, const char* what)
    {
        if (std::memcmp(reinterpret_cast<const void*>(g_base + rva), bytes, n) == 0)
            return true;
        LOG_ERROR("[EnemySync] %s prologue mismatch at base+0x%llX (not 1.11.240?) -- that part is disabled",
            what, static_cast<unsigned long long>(rva));
        return false;
    }

    bool HookOne(uintptr_t rva, void* detour, void** orig, const char* name)
    {
        void* target = reinterpret_cast<void*>(g_base + rva);
        MH_STATUS st = MH_CreateHook(target, detour, orig);
        if (st != MH_OK)
        {
            LOG_ERROR("[EnemySync] MH_CreateHook(%s) failed: %d", name, static_cast<int>(st));
            *orig = nullptr;
            return false;
        }
        st = MH_EnableHook(target);
        if (st != MH_OK)
        {
            LOG_ERROR("[EnemySync] MH_EnableHook(%s) failed: %d", name, static_cast<int>(st));
            MH_RemoveHook(target);
            *orig = nullptr;
            return false;
        }
        LOG_INFO("[EnemySync] %s hooked at base+0x%llX", name, static_cast<unsigned long long>(rva));
        return true;
    }

    // ── Detours ─────────────────────────────────────────────────────────────
    void* __fastcall Detour_KillCore(Actor* victim, Actor* killer, float dmg, bool silent, bool ragdoll)
    {
        // A puppet's death is its owner's decision: a local lethal hit only
        // leaves it at <= 0 health until the owner's KILL (or HP) arrives.
        // Refused here, before the engine marks it "kill queued" -- a gate in
        // KillImpl would leave that mark and swallow the real kill later.
        if (IsGuarded(victim))
        {
            g_dRefusedKills.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        return g_origKillCore(victim, killer, dmg, silent, ragdoll);
    }

    uint64_t __fastcall Detour_KillImpl(Actor* victim, Actor* killer, float dmg, bool silent, bool ragdoll)
    {
        const bool wasAlive = victim && !CoSyncGameAPI::IsDeadOrDying(victim);
        const uint64_t r = g_origKillImpl(victim, killer, dmg, silent, ragdoll);
        if (wasAlive && CoSyncGameAPI::IsDeadOrDying(victim) && !IsGuarded(victim))
        {
            CapKill k{ FormIdOf(victim), killer ? FormIdOf(killer) : 0u, ragdoll };
            if (k.victimFid != 0 && k.victimFid != g_localPlayerFid.load(std::memory_order_relaxed))
            {
                std::lock_guard<std::mutex> lk(g_capMx);
                if (g_capKills.size() < 512)
                    g_capKills.push_back(k);
                g_dCapKills.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return r;
    }

    uint64_t __fastcall Detour_ModAV(Actor* actor, int mode, void* avi, float delta, Actor* attacker)
    {
        const bool healthOfPuppet = mode == 2 && actor && avi != nullptr &&
            avi == g_healthAVI.load(std::memory_order_relaxed) && IsGuarded(actor);

        // Damage (mode 2) to Health, with an attacker, on a puppet: if the
        // attacker is simulated here, its owner needs this hit.
        if (healthOfPuppet && delta < 0.f && attacker && attacker != actor)
        {
            PlayerCharacter* pc = CoSyncGetPlayer();
            const bool attackerHere = (reinterpret_cast<Actor*>(pc) == attacker) || !IsRegisteredRemoteActor(attacker);
            if (attackerHere)
            {
                CapHit h{ FormIdOf(actor), FormIdOf(attacker), -delta };
                if (h.targetFid && h.attackerFid)
                {
                    std::lock_guard<std::mutex> lk(g_capMx);
                    if (g_capHits.size() < 1024)
                        g_capHits.push_back(h);
                    g_dCapHits.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // 0.4.3: the puppet's Health is the owner's. A local write (this hit,
        // damage over time, regeneration, an aggressor-less explosion replica)
        // is not applied here; the owner applies the reported hit and its HP
        // comes back. Without this the client showed its own prediction first
        // and the owner's correction then pulled the bar back (Bug #3).
        if (healthOfPuppet && g_cfgAuthoritativeHealth && tls_healthWriteDepth == 0)
        {
            g_dRefusedHealthWrites.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        return g_origModAV(actor, mode, avi, delta, attacker);
    }

    uint8_t __fastcall Detour_SpellCast(void* vm, uint32_t stackId, TESForm* spell, TESObjectREFR* source, TESObjectREFR* target)
    {
        if (spell && source && source == target && SafeFormType(source) == kFormType_ACHR)
        {
            Actor* a = static_cast<Actor*>(source);
            if (IsGuarded(a))
            {
                // A puppet's own script (e.g. AbLegendaryScript) deciding to
                // mutate locally. The owner's decision arrives as ENS casts.
                g_dRefusedCasts.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            if (!IsRegisteredRemoteActor(a))
            {
                CapCast c{ FormIdOf(spell), FormIdOf(a) };
                if (c.spellFid && c.sourceFid)
                {
                    std::lock_guard<std::mutex> lk(g_capMx);
                    if (g_capCasts.size() < 256)
                        g_capCasts.push_back(c);
                    g_dCapCasts.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        return g_origCast(vm, stackId, spell, source, target);
    }

    // ── Game-thread helpers ─────────────────────────────────────────────────
    void* NativeVM()
    {
        GameVM* gvm = *g_gameVM;
        return gvm ? static_cast<void*>(gvm->m_virtualMachine) : nullptr;
    }

    Actor* ActorByFormID(uint32_t fid)
    {
        if (fid == 0)
            return nullptr;
        TESForm* f = LookupFormByID(fid);
        return (f && f->formType == kFormType_ACHR) ? static_cast<Actor*>(f) : nullptr;
    }

    const CoSyncEntityState* StateOf(uint32_t eid)
    {
        const auto& states = g_CoSyncPlayerManager.GetStates();
        auto it = states.find(eid);
        return (it != states.end() && it->second.hasCreate) ? &it->second : nullptr;
    }

    bool OwnedHere(uint32_t eid)
    {
        const CoSyncEntityState* st = StateOf(eid);
        return st && CoSyncNpcWorldSync::IsWorldActorEntityID(eid) && st->isLocallyOwned;
    }

    Actor* ActorOfEntity(uint32_t eid)
    {
        if (eid == 0)
            return nullptr;
        if (eid == CoSyncNet::GetMyEntityID())
            return reinterpret_cast<Actor*>(CoSyncGetPlayer());
        CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(eid);
        if (!p || !p->hasSpawned || !p->actorRef)
            return nullptr;
        return CoSyncGameAPI::IsFormStillLive(static_cast<TESForm*>(p->actorRef)) ? p->actorRef : nullptr;
    }

    // Local attacker simulated on this machine: our player, or a world actor we own.
    bool SimulatedHere(uint32_t eid)
    {
        return eid != 0 && (eid == CoSyncNet::GetMyEntityID() || OwnedHere(eid));
    }

    void SendText(const std::string& s)
    {
        CoSyncTransport::Send(s);
        g_dTxBytes += static_cast<uint32_t>(s.size());
    }

    __declspec(noinline) uint32_t SafeTargetHandle(Actor* a) { return SafeU32(a, kActor_CombatTarget); }

    uint32_t TargetEntity(Actor* a)
    {
        const uint32_t h = SafeTargetHandle(a);
        if (h == 0 || h == 0xFFFFFFFFu)
            return 0;
        NiPointer<TESObjectREFR> ref;
        if (!LookupREFRByHandle(h, ref) || !ref.m_pObject || SafeFormType(ref.m_pObject) != kFormType_ACHR)
            return 0;
        return CoSyncEnemySync::EntityIdOf(static_cast<Actor*>(ref.m_pObject));
    }

    __declspec(noinline) int32_t SafeCombatState(Actor* a)
    {
        if (!g_combatState || !a)
            return 0;
        __try { return g_combatState(a); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    __declspec(noinline) bool SafeHasKeyword(void* vm, Actor* a, TESForm* kw)
    {
        if (!g_hasKeyword || !vm || !a || !kw)
            return false;
        __try { return g_hasKeyword(vm, 0, a, kw) != 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    __declspec(noinline) bool SafeHasLocRefType(void* vm, Actor* a, TESForm* lcrt)
    {
        if (!g_hasLocRef || !vm || !a || !lcrt)
            return false;
        __try { return g_hasLocRef(vm, 0, a, lcrt) != 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    uint32_t Classify(Actor* a)
    {
        uint32_t c = CoSyncEnemySync::kClass_Known;
        void* vm = NativeVM();
        if (SafeHasKeyword(vm, a, LookupFormByID(kFID_EncTypeLegendary))) c |= CoSyncEnemySync::kClass_Legendary;
        if (SafeHasKeyword(vm, a, LookupFormByID(kFID_ActorTypeRobot)))   c |= CoSyncEnemySync::kClass_Robot;
        if (SafeHasKeyword(vm, a, LookupFormByID(kFID_ActorTypeTurret)))  c |= CoSyncEnemySync::kClass_Turret;
        if (SafeHasKeyword(vm, a, LookupFormByID(kFID_ActorTypeCreature)) ||
            SafeHasKeyword(vm, a, LookupFormByID(kFID_ActorTypeAnimal)))  c |= CoSyncEnemySync::kClass_Creature;
        if (SafeHasKeyword(vm, a, LookupFormByID(kFID_ActorTypeNPC)))     c |= CoSyncEnemySync::kClass_Humanoid;

        // Boss = a location ref type from BossLocRefTypeList (mods may extend
        // the list); Boss itself when the list is missing.
        bool boss = false;
        TESForm* lf = LookupFormByID(kFID_BossLocRefTypes);
        if (lf && lf->formType == kFormType_FLST)
        {
            BGSListForm* list = static_cast<BGSListForm*>(lf);
            for (UInt32 i = 0; i < list->forms.count && !boss; ++i)
                boss = SafeHasLocRefType(vm, a, list->forms[i]);
        }
        else
            boss = SafeHasLocRefType(vm, a, LookupFormByID(kFID_LocRefBoss));
        if (boss) c |= CoSyncEnemySync::kClass_Boss;

        if (a->baseForm && a->baseForm->formType == kFormType_NPC_)
        {
            const uint32_t f = CoSyncGameAPI::ActorBaseFlags32(static_cast<TESNPC*>(a->baseForm));
            if (f & 0x20u) c |= CoSyncEnemySync::kClass_Unique;
            if (f & 0x02u) c |= CoSyncEnemySync::kClass_Essential;
        }
        if (CoSyncGameAPI::GetActorPowerArmorExtraState(a) == 1)
            c |= CoSyncEnemySync::kClass_PowerArmor;
        return c;
    }

    std::string ClassText(uint32_t c)
    {
        std::string s;
        auto add = [&](uint32_t bit, const char* n) { if (c & bit) { if (!s.empty()) s += '+'; s += n; } };
        add(CoSyncEnemySync::kClass_Legendary, "legendary");
        add(CoSyncEnemySync::kClass_Boss, "boss");
        add(CoSyncEnemySync::kClass_Unique, "unique");
        add(CoSyncEnemySync::kClass_Essential, "essential");
        add(CoSyncEnemySync::kClass_Robot, "robot");
        add(CoSyncEnemySync::kClass_Turret, "turret");
        add(CoSyncEnemySync::kClass_Creature, "creature");
        add(CoSyncEnemySync::kClass_Humanoid, "humanoid");
        add(CoSyncEnemySync::kClass_PowerArmor, "power-armor");
        return s.empty() ? std::string("normal") : s;
    }

    // Owner: hit arriving from another machine, applied the way the engine's
    // own DoHitMe does it -- TESHitEvent first (Papyrus OnHit: legendary
    // mutation, quest scripts), then Health damage with the attacker, which
    // runs CheckForDeath(attacker) and so the real kill with the real killer.
    __declspec(noinline) bool ApplyHitEngine(Actor* target, Actor* attacker, float damage, bool& hitEventSent)
    {
        hitEventSent = false;
        void* health = g_healthAVI.load(std::memory_order_relaxed);
        if (!target || !health || !g_modAV)
            return false;
        __try
        {
            if (g_hitCtor && g_hitInit && g_hitDtor && g_hitEvent)
            {
                alignas(16) uint8_t hit[0x100];
                std::memset(hit, 0, sizeof(hit));
                g_hitCtor(hit);
                g_hitInit(hit, attacker, target, damage);
                g_hitEvent(hit);
                g_hitDtor(hit);
                hitEventSent = true;
            }
            g_modAV(target, 2, health, -damage, attacker);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    __declspec(noinline) bool EngineKill(Actor* puppet, Actor* killer, bool ragdoll)
    {
        if (!g_killCore || !puppet)
            return false;
        __try
        {
            // The runtime-essential bit would turn Dying into essential-down.
            puppet->uiFlags &= ~(kUiFlag_RuntimeEssential | kUiFlag_RuntimeProtected);
            ++tls_healthWriteDepth;   // the owner's death: any Health write it makes is authoritative
            g_killCore(puppet, killer, 0.f, true, ragdoll);
            --tls_healthWriteDepth;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            tls_healthWriteDepth = 0;
            return false;
        }
    }

    __declspec(noinline) bool EngineSelfCast(void* vm, TESForm* spell, Actor* puppet)
    {
        if (!g_spellCast || !vm || !spell || !puppet)
            return false;
        __try
        {
            return g_spellCast(vm, 0, spell, puppet, puppet) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::string CastListText(const std::vector<uint32_t>& casts)
    {
        if (casts.empty())
            return "-";
        std::string s;
        for (size_t i = 0; i < casts.size(); ++i)
        {
            if (i) s += ',';
            s += std::to_string(casts[i]);
        }
        return s;
    }

    void SendState(uint32_t eid, OwnerState& o, double now)
    {
        ++o.ver;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "ENS|%u|%u|%u|%u|%u|%u|", eid, o.ver, o.combat, o.target, o.cls, o.life);
        SendText(std::string(buf) + CastListText(o.casts));
        o.lastSent = now;
        o.dirty = false;
        ++g_dTxState;
    }

    // ── Tick stages ─────────────────────────────────────────────────────────
    void EnsureForms()
    {
        if (!g_healthAVI.load(std::memory_order_relaxed))
        {
            TESForm* f = LookupFormByID(kFID_Health);
            if (f && f->formType == kFormType_AVIF)
                g_healthAVI.store(f, std::memory_order_relaxed);
        }
        if (PlayerCharacter* pc = CoSyncGetPlayer())
            g_localPlayerFid.store(pc->formID, std::memory_order_relaxed);
    }

    void RebuildGuard()
    {
        std::unordered_set<Actor*> next, proxies;
        for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
        {
            const CoSyncPlayer* p = kv.second.get();
            if (!p || !p->hasSpawned || !p->actorRef || p->isLocalPlayer)
                continue;
            if (!CoSyncNpcWorldSync::IsWorldActorEntityID(p->entityID))
            {
                if (p->createType == CoSyncEntityType::Player && IsRegisteredRemoteActor(p->actorRef))
                    proxies.insert(p->actorRef);
                continue;
            }
            if (OwnedHere(p->entityID))
                continue;
            if (IsRegisteredRemoteActor(p->actorRef))
                next.insert(p->actorRef);
        }
        std::lock_guard<std::mutex> lk(g_guardMx);
        g_guard.swap(next);
        g_playerProxies.swap(proxies);
    }

    void ProcessCapturedKills()
    {
        std::vector<CapKill> kills;
        {
            std::lock_guard<std::mutex> lk(g_capMx);
            kills.swap(g_capKills);
        }
        for (const CapKill& k : kills)
        {
            Actor* victim = ActorByFormID(k.victimFid);
            const uint32_t eid = victim ? CoSyncEnemySync::EntityIdOf(victim) : 0;
            if (eid == 0 || !OwnedHere(eid) || CoSyncEnemySync::WasDeathAnnounced(eid))
                continue;
            DeathInfo d;
            d.killer = CoSyncEnemySync::EntityIdOf(ActorByFormID(k.killerFid));
            d.flags  = k.ragdoll ? 1u : 0u;
            g_deaths[eid] = d;
            SendText(CoSyncEnemySync::BuildKill(eid));
            CoSyncNpcWorldSync::MarkEntityKilled(eid);
            ++g_dTxKill;
            auto o = g_owned.find(eid);
            if (o != g_owned.end()) { o->second.life = 2; o->second.dirty = true; }
            LOG_UPDATED("[EnemySync] TX KILL entity=%u killer=%u ragdoll=%d (engine death, fid 0x%08X)",
                eid, d.killer, k.ragdoll ? 1 : 0, k.victimFid);
        }
    }

    void ProcessCapturedHits(double now)
    {
        std::vector<CapHit> hits;
        {
            std::lock_guard<std::mutex> lk(g_capMx);
            hits.swap(g_capHits);
        }
        if (hits.empty())
            return;

        // One EHIT per (target, attacker) per tick: shotgun pellets and a
        // hit plus its damage-over-time in the same frame add up.
        struct Key { uint32_t t, a; };
        std::vector<std::pair<Key, float>> merged;
        for (const CapHit& h : hits)
        {
            Actor* target   = ActorByFormID(h.targetFid);
            Actor* attacker = ActorByFormID(h.attackerFid);
            const uint32_t tEid = target ? CoSyncEnemySync::EntityIdOf(target) : 0;
            const uint32_t aEid = attacker ? CoSyncEnemySync::EntityIdOf(attacker) : 0;
            if (tEid == 0 || !CoSyncNpcWorldSync::IsWorldActorEntityID(tEid) || OwnedHere(tEid))
                continue;
            if (!SimulatedHere(aEid))
                continue;   // an unsynced local critter, or not ours to report
            if (!(h.damage > 0.f) || !std::isfinite(h.damage))
                continue;
            bool found = false;
            for (auto& m : merged)
                if (m.first.t == tEid && m.first.a == aEid) { m.second += h.damage; found = true; break; }
            if (!found)
                merged.push_back({ Key{ tEid, aEid }, h.damage });
        }
        for (const auto& m : merged)
        {
            auto& b = g_hitBudget[m.first.a];
            if (now - b.first >= 1.0) { b.first = now; b.second = 0; }
            if (++b.second > 60)
                continue;
            const uint32_t seq = ++g_txHitSeq[m.first.a];
            char buf[128];
            std::snprintf(buf, sizeof(buf), "EHIT|%u|%u|%u|%.2f|0", m.first.t, m.first.a, seq, m.second);
            SendText(buf);
            ++g_dTxHit;
            if (g_dTxHit <= 20 || g_dTxHit % 200 == 0)
                LOG_UPDATED("[EnemySync] TX EHIT target=%u attacker=%u seq=%u damage=%.1f", m.first.t, m.first.a, seq, m.second);
        }
    }

    void ProcessCapturedCasts(double now)
    {
        std::vector<CapCast> casts;
        {
            std::lock_guard<std::mutex> lk(g_capMx);
            casts.swap(g_capCasts);
        }
        for (const CapCast& c : casts)
        {
            Actor* src = ActorByFormID(c.sourceFid);
            const uint32_t eid = src ? CoSyncEnemySync::EntityIdOf(src) : 0;
            if (eid == 0 || !OwnedHere(eid))
                continue;
            OwnerState& o = g_owned[eid];
            bool known = false;
            for (uint32_t s : o.casts) if (s == c.spellFid) { known = true; break; }
            if (known)
                continue;
            if (o.casts.size() >= 4)
                o.casts.erase(o.casts.begin());
            o.casts.push_back(c.spellFid);
            o.dirty = true;
            o.lastSent = -1.0;   // send at once: this is the mutation moment
            LOG_UPDATED("[EnemySync] captured self-cast entity=%u spell=0x%08X (owner script decision)", eid, c.spellFid);
        }
        (void)now;
    }

    void ApplyReceivedHits()
    {
        std::vector<RxHit> hits;
        {
            std::lock_guard<std::mutex> lk(g_rxMx);
            hits.swap(g_rxHits);
        }
        for (const RxHit& h : hits)
        {
            if (!OwnedHere(h.target))
                continue;   // relayed to everyone; only the owner applies
            uint32_t& last = g_rxHitSeq[h.attacker];
            if (h.seq != 0 && h.seq <= last && last - h.seq < 0x10000000u)
            {
                ++g_dRxHitRejected; ++g_dRejDup;
                continue;   // duplicate / replayed
            }
            last = h.seq;
            Actor* target = ActorOfEntity(h.target);
            if (!target)
            {
                ++g_dRxHitRejected; ++g_dRejNoActor;
                continue;
            }
            if (CoSyncGameAPI::IsDeadOrDying(target) || CoSyncEnemySync::WasDeathAnnounced(h.target))
            {
                ++g_dRxHitRejected; ++g_dRejDead;
                continue;   // a hit on a corpse changes nothing (shots in flight when it died)
            }
            if (!(h.damage > 0.f) || h.damage > 100000.f)
            {
                ++g_dRxHitRejected;
                continue;
            }
            Actor* attacker = ActorOfEntity(h.attacker);
            if (attacker)
            {
                const float dx = attacker->pos.x - target->pos.x, dy = attacker->pos.y - target->pos.y, dz = attacker->pos.z - target->pos.z;
                if (dx * dx + dy * dy + dz * dz > 20000.f * 20000.f)
                {
                    ++g_dRxHitRejected;
                    LOG_UPDATED_WARN("[EnemySync] EHIT target=%u attacker=%u rejected: %.0f units apart",
                        h.target, h.attacker, std::sqrt(dx * dx + dy * dy + dz * dz));
                    continue;
                }
            }
            bool hitEvent = false;
            if (ApplyHitEngine(target, attacker, h.damage, hitEvent))
            {
                ++g_dRxHitApplied;
                if (g_dRxHitApplied <= 20 || g_dRxHitApplied % 200 == 0)
                    LOG_UPDATED("[EnemySync] applied EHIT target=%u attacker=%u%s damage=%.1f hitEvent=%d",
                        h.target, h.attacker, attacker ? "" : " (attacker not here: unattributed)", h.damage, hitEvent ? 1 : 0);
            }
            else
            {
                ++g_dRxHitRejected;
                LOG_UPDATED_WARN("[EnemySync] EHIT target=%u attacker=%u could not be applied (engine call failed)", h.target, h.attacker);
            }
        }
    }

    void OwnerStateTick(double now)
    {
        static double s_nextSample = 0.0;
        static double s_nextSnapshot = 0.0;
        const bool sample   = now >= s_nextSample;
        const bool snapshot = now >= s_nextSnapshot;
        if (sample)   s_nextSample   = now + 0.2;
        if (snapshot) s_nextSnapshot = now + 5.0;

        std::unordered_set<uint32_t> seen;
        for (const auto& kv : g_CoSyncPlayerManager.GetStates())
        {
            const uint32_t eid = kv.first;
            if (!CoSyncNpcWorldSync::IsWorldActorEntityID(eid) || !kv.second.hasCreate || !kv.second.isLocallyOwned)
                continue;
            Actor* a = ActorOfEntity(eid);
            if (!a)
                continue;
            seen.insert(eid);
            OwnerState& o = g_owned[eid];
            if (sample || o.dirty)
            {
                const int ls = CoSyncGameAPI::GetLifeState(a);
                const uint32_t life = ls < 0 ? o.life : static_cast<uint32_t>(ls);
                const bool alive = (life == 0);
                const uint32_t combat = alive ? static_cast<uint32_t>(SafeCombatState(a)) : 0u;
                const uint32_t target = (alive && combat != 0) ? TargetEntity(a) : 0u;
                if (!(o.cls & CoSyncEnemySync::kClass_Known) || now - o.clsAt >= 30.0)
                {
                    const uint32_t cls = Classify(a);
                    o.clsAt = now;
                    if (cls != o.cls) { o.cls = cls; o.dirty = true;
                        LOG_UPDATED("[EnemySync] entity=%u class=%s", eid, ClassText(cls).c_str()); }
                }
                if (combat != o.combat || target != o.target || life != o.life)
                {
                    if (target != o.target && combat != 0)
                        LOG_UPDATED("[EnemySync] entity=%u target %u -> %u (combat %u)", eid, o.target, target, combat);
                    o.combat = combat; o.target = target; o.life = life; o.dirty = true;
                }
            }
            // 0.4.3: the owner's health, as soon as it changes (5 Hz), not only
            // from NpcWorldSync's 1 Hz scan -- the enemy meter on every other
            // machine shows exactly this value (puppet Health is owner-only).
            if (sample && o.life == 0)
            {
                ActorValueInfo* hav = static_cast<ActorValueInfo*>(g_healthAVI.load(std::memory_order_relaxed));
                float cur = 0.f, mx = 0.f;
                if (hav && CoSyncAVSafety::SafeGetValue(a, hav, cur) && CoSyncAVSafety::SafeGetMaximum(a, hav, mx) &&
                    cur > 0.f && (o.lastHp < 0.f || std::fabs(cur - o.lastHp) >= 0.5f || now - o.lastHpSent >= 4.0))
                {
                    SendText(CoSyncHealthSync::SerializeHP(eid, cur, mx));
                    o.lastHp = cur;
                    o.lastHpSent = now;
                    ++g_dTxHp;
                }
            }

            // State, at most 5 sends/s/entity (trailing edge: the newest value wins).
            if (o.dirty && now - o.lastSent >= 0.2)
                SendState(eid, o, now);
            else if (snapshot)
                SendState(eid, o, now);   // late joiners and anyone who missed a change
        }
        for (auto it = g_owned.begin(); it != g_owned.end(); )
            it = seen.count(it->first) ? std::next(it) : g_owned.erase(it);
    }

    void ApplyRemoteStates(double now)
    {
        std::vector<RxState> states;
        {
            std::lock_guard<std::mutex> lk(g_rxMx);
            states.swap(g_rxStates);
        }
        for (RxState& s : states)
        {
            if (OwnedHere(s.eid))
                continue;   // our own state relayed back
            RemoteState& r = g_remote[s.eid];
            const bool first = (r.lastRx == 0.0);
            if (first || s.target != r.s.target || s.combat != r.s.combat || s.cls != r.s.cls || s.life != r.s.life)
                LOG_UPDATED("[EnemySync] RX ENS entity=%u ver=%u combat=%u target=%u life=%u class=%s casts=%zu",
                    s.eid, s.ver, s.combat, s.target, s.life, ClassText(s.cls).c_str(), s.casts.size());
            r.s = std::move(s);
            r.lastRx = now;
            ++g_dRxState;
        }

        // Scripted self-casts (mutation): the same engine cast on the puppet,
        // once per puppet instance -- also when the puppet binds after the state.
        static double s_nextApply = 0.0;
        if (now < s_nextApply)
            return;
        s_nextApply = now + 0.25;
        void* vm = NativeVM();
        for (auto it = g_remote.begin(); it != g_remote.end(); )
        {
            RemoteState& r = it->second;
            const uint32_t eid = it->first;
            if (!StateOf(eid) && now - r.lastRx > 30.0)
            {
                it = g_remote.erase(it);   // entity gone for good
                continue;
            }
            ++it;
            if (r.s.casts.empty())
                continue;
            Actor* puppet = ActorOfEntity(eid);
            if (!puppet || OwnedHere(eid) || CoSyncGameAPI::IsDeadOrDying(puppet) || CoSyncNpcWorldSync::IsEntityDead(eid))
                continue;
            if (puppet != r.appliedActor)
            {
                r.appliedActor = puppet;
                r.appliedCasts.clear();
            }
            for (uint32_t spellFid : r.s.casts)
            {
                if (r.appliedCasts.count(spellFid))
                    continue;
                TESForm* spell = LookupFormByID(spellFid);
                if (!spell || spell->formType != kFormType_SPEL)
                {
                    r.appliedCasts.insert(spellFid);   // not in this load order: never retry
                    LOG_UPDATED_WARN("[EnemySync] entity=%u cast 0x%08X is not a spell here -- skipped", eid, spellFid);
                    continue;
                }
                if (EngineSelfCast(vm, spell, puppet))
                {
                    r.appliedCasts.insert(spellFid);
                    ++g_dReplayCast;
                    LOG_UPDATED("[EnemySync] replayed cast entity=%u spell=0x%08X on the puppet (owner's mutation/self-cast)", eid, spellFid);
                }
            }
        }
    }

    void KillWatchdog(double now)
    {
        for (auto it = g_pendingKills.begin(); it != g_pendingKills.end(); )
        {
            Actor* a = it->actor;
            if (!a || !CoSyncGameAPI::IsFormStillLive(static_cast<TESForm*>(a)) || CoSyncGameAPI::IsDeadOrDying(a))
            {
                it = g_pendingKills.erase(it);
                continue;
            }
            if (now - it->since >= 3.0)
            {
                ++g_dReplayKillFallback;
                LOG_UPDATED_WARN("[EnemySync] entity=%u engine kill did not take after 3 s -- falling back to KillPuppet", it->eid);
                {
                    CoSyncEnemySync::ScopedHealthWrite authoritative;
                    CoSyncGameAPI::KillPuppet(a);
                }
                it = g_pendingKills.erase(it);
                continue;
            }
            ++it;
        }
    }

    void Diagnostics(double now)
    {
        static double s_next = 0.0;
        if (now < s_next)
            return;
        s_next = now + 10.0;
        const uint32_t rk = g_dRefusedKills.exchange(0), rc = g_dRefusedCasts.exchange(0),
                       ch = g_dCapHits.exchange(0), ck = g_dCapKills.exchange(0), cc = g_dCapCasts.exchange(0),
                       rh = g_dRefusedHealthWrites.exchange(0);
        if (rk || rc || ch || ck || cc || rh || g_dTxHit || g_dRxHitApplied || g_dRxHitRejected || g_dTxKill ||
            g_dTxState || g_dRxState || g_dReplayKill || g_dReplayCast || g_dTxHp)
        {
            LOG_UPDATED("[EnemySync] last 10 s: owned=%zu remote=%zu | TX EHIT=%u KILL=%u ENS=%u HP=%u bytes=%u | RX EHIT applied=%u rejected=%u (dup %u, dead %u, no actor %u) ENS=%u | "
                        "replayed kills=%u (fallback %u) casts=%u | hooks: hits seen=%u deaths seen=%u self-casts seen=%u | refused puppet deaths=%u casts=%u local health writes=%u",
                g_owned.size(), g_remote.size(), g_dTxHit, g_dTxKill, g_dTxState, g_dTxHp, g_dTxBytes, g_dRxHitApplied, g_dRxHitRejected, g_dRejDup, g_dRejDead, g_dRejNoActor, g_dRxState,
                g_dReplayKill, g_dReplayKillFallback, g_dReplayCast, ch, ck, cc, rk, rc, rh);
        }
        g_dTxHit = g_dRxHitApplied = g_dRxHitRejected = g_dTxKill = g_dTxState = g_dRxState =
            g_dReplayKill = g_dReplayKillFallback = g_dReplayCast = g_dTxBytes = g_dTxHp = 0;
        g_dRejDup = g_dRejDead = g_dRejNoActor = 0;
    }

    template <size_t N>
    bool Resolve(uintptr_t rva, const uint8_t (&pro)[N], const char* what)
    {
        return PrologueOk(rva, pro, N, what);
    }
}

namespace CoSyncEnemySync
{
    void Install()
    {
        LOG_REQUIREMENT("CoSyncEnemySync::Install", "Plugin load. Every target is prologue-checked against 1.11.240; a mismatch disables only that part.");
        if (g_installed)
            return;
        g_installed = true;
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        {
            char exe[MAX_PATH] = {};
            if (GetModuleFileNameA(nullptr, exe, MAX_PATH))
            {
                std::string ini = exe;
                ini = ini.substr(0, ini.find_last_of("\\/") + 1) + "Data\\F4SE\\Plugins\\CoSync.ini";
                g_cfgAuthoritativeHealth = GetPrivateProfileIntA("Puppet", "bAuthoritativeHealth", 1, ini.c_str()) != 0;
            }
        }
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        {
            LOG_ERROR("[EnemySync] MH_Initialize failed: %d", static_cast<int>(st));
            return;
        }

        if (Resolve(kRVA_HitCtor, kPro_HitCtor, "HitData ctor") && Resolve(kRVA_HitInit, kPro_HitInit, "HitData init") &&
            Resolve(kRVA_HitDtor, kPro_HitDtor, "HitData dtor") && Resolve(kRVA_HitEventSend, kPro_HitEvent, "TESHitEvent send"))
        {
            g_hitCtor  = reinterpret_cast<HitCtor_t>(g_base + kRVA_HitCtor);
            g_hitInit  = reinterpret_cast<HitInit_t>(g_base + kRVA_HitInit);
            g_hitDtor  = reinterpret_cast<HitDtor_t>(g_base + kRVA_HitDtor);
            g_hitEvent = reinterpret_cast<HitEvent_t>(g_base + kRVA_HitEventSend);
        }
        if (Resolve(kRVA_HasKeyword, kPro_HasKeyword, "Form.HasKeyword"))
            g_hasKeyword = reinterpret_cast<HasKeyword_t>(g_base + kRVA_HasKeyword);
        if (Resolve(kRVA_HasLocRefType, kPro_HasLocRef, "ObjectReference.HasLocRefType"))
            g_hasLocRef = reinterpret_cast<HasLocRef_t>(g_base + kRVA_HasLocRefType);
        if (Resolve(kRVA_CombatState, kPro_CombatState, "GetCombatState"))
            g_combatState = reinterpret_cast<CombatState_t>(g_base + kRVA_CombatState);

        if (Resolve(kRVA_KillCore, kPro_KillCore, "kill core"))
        {
            g_killCore = reinterpret_cast<KillCore_t>(g_base + kRVA_KillCore);
            if (HookOne(kRVA_KillCore, reinterpret_cast<void*>(&Detour_KillCore), reinterpret_cast<void**>(&g_origKillCore), "kill core"))
                g_killCore = g_origKillCore;   // our own replays bypass the gate
        }
        if (Resolve(kRVA_KillImpl, kPro_KillImpl, "KillImpl"))
            HookOne(kRVA_KillImpl, reinterpret_cast<void*>(&Detour_KillImpl), reinterpret_cast<void**>(&g_origKillImpl), "KillImpl");
        if (Resolve(kRVA_ModActorValue, kPro_ModAV, "ModActorValue"))
        {
            g_modAV = reinterpret_cast<ModAV_t>(g_base + kRVA_ModActorValue);
            if (HookOne(kRVA_ModActorValue, reinterpret_cast<void*>(&Detour_ModAV), reinterpret_cast<void**>(&g_origModAV), "ModActorValue"))
                g_modAV = g_origModAV;
        }
        if (Resolve(kRVA_SpellCast, kPro_SpellCast, "Spell.Cast"))
        {
            g_spellCast = reinterpret_cast<SpellCast_t>(g_base + kRVA_SpellCast);
            if (HookOne(kRVA_SpellCast, reinterpret_cast<void*>(&Detour_SpellCast), reinterpret_cast<void**>(&g_origCast), "Spell.Cast"))
                g_spellCast = g_origCast;
        }
        LOG_INFO("[EnemySync] ready: killGate=%d deathCapture=%d hitCapture=%d castCapture=%d hitEvent=%d classify=%d/%d combatState=%d authoritativeHealth=%d",
            g_origKillCore ? 1 : 0, g_origKillImpl ? 1 : 0, g_origModAV ? 1 : 0, g_origCast ? 1 : 0,
            g_hitEvent ? 1 : 0, g_hasKeyword ? 1 : 0, g_hasLocRef ? 1 : 0, g_combatState ? 1 : 0,
            IsAuthoritativeHealthActive() ? 1 : 0);
    }

    void Reset()
    {
        {
            std::lock_guard<std::mutex> lk(g_capMx);
            g_capKills.clear(); g_capHits.clear(); g_capCasts.clear();
        }
        {
            std::lock_guard<std::mutex> lk(g_rxMx);
            g_rxHits.clear(); g_rxStates.clear();
        }
        {
            std::lock_guard<std::mutex> lk(g_guardMx);
            g_guard.clear();
            g_playerProxies.clear();
        }
        g_owned.clear();
        g_remote.clear();
        g_deaths.clear();
        g_pendingKills.clear();
        g_killReplayedEids.clear();
        g_txHitSeq.clear();
        g_rxHitSeq.clear();
        g_hitBudget.clear();
    }

    bool IsGuardedPuppet(Actor* actor)
    {
        return IsGuarded(actor);
    }

    bool IsPlayerProxy(Actor* actor)
    {
        if (!actor)
            return false;
        {
            std::lock_guard<std::mutex> lk(g_guardMx);
            if (g_playerProxies.find(actor) == g_playerProxies.end())
                return false;
        }
        return IsRegisteredRemoteActor(actor);
    }

    ScopedHealthWrite::ScopedHealthWrite()  { ++tls_healthWriteDepth; }
    ScopedHealthWrite::~ScopedHealthWrite() { --tls_healthWriteDepth; }

    void OnOwnershipChanged(uint32_t entityID)
    {
        g_txHitSeq.erase(entityID);
        g_rxHitSeq.erase(entityID);
        g_owned.erase(entityID);
        g_remote.erase(entityID);
    }

    bool IsAuthoritativeHealthActive()
    {
        return g_cfgAuthoritativeHealth && g_origModAV != nullptr;
    }

    bool IsHitCaptureActive()
    {
        return g_origModAV != nullptr && g_healthAVI.load(std::memory_order_relaxed) != nullptr;
    }

    bool WasDeathAnnounced(uint32_t entityID)
    {
        return g_deaths.find(entityID) != g_deaths.end();
    }

    void NoteDeathAnnounced(uint32_t entityID)
    {
        g_deaths.emplace(entityID, DeathInfo{});   // keeps a killer the hook already recorded
    }

    std::string BuildKill(uint32_t entityID)
    {
        uint32_t killer = 0, flags = 0;
        auto it = g_deaths.find(entityID);
        if (it != g_deaths.end()) { killer = it->second.killer; flags = it->second.flags; }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "KILL|%u|%u|%u", entityID, killer, flags);
        return buf;
    }

    bool ReplayDeath(uint32_t entityID, Actor* puppet, uint32_t killerEntityID, uint32_t flags)
    {
        LOG_REQUIREMENT("CoSyncEnemySync::ReplayDeath", "Game thread (HealthSync flush). puppet is a live world-actor puppet this machine does not own.");
        if (!g_killCore || !puppet || !CoSyncGameAPI::IsFormStillLive(static_cast<TESForm*>(puppet)))
            return false;
        for (const PendingKill& p : g_pendingKills)
            if (p.eid == entityID && p.actor == puppet)
                return true;   // already queued in the engine
        Actor* killer = ActorOfEntity(killerEntityID);
        // 0.4.2: a corpse whose puppet is spawned again (rebind after an
        // unload, a late bind) already played its death here once -- it goes
        // straight to ragdoll instead of dying a second time on screen
        // (2026-09-30 client log 3476: every death replayed twice).
        const bool seenBefore = !g_killReplayedEids.insert(entityID).second;
        if (seenBefore)
            flags |= 1u;
        if (!EngineKill(puppet, killer, (flags & 1u) != 0))
            return false;
        LARGE_INTEGER f{}, c{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        g_pendingKills.push_back({ entityID, puppet, static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart) });
        ++g_dReplayKill;
        LOG_UPDATED("[EnemySync] replayed engine kill entity=%u killer=%u%s ragdoll=%d (death animation from this machine's graph)",
            entityID, killerEntityID, killerEntityID == CoSyncNet::GetMyEntityID() ? " (us: kill credit here)" : "",
            (flags & 1u) ? 1 : 0);
        return true;
    }

    uint32_t EntityIdOf(Actor* actor)
    {
        if (!actor)
            return 0;
        if (actor == reinterpret_cast<Actor*>(CoSyncGetPlayer()))
            return CoSyncNet::GetMyEntityID();
        if (const uint32_t eid = CoSyncNpcWorldSync::GetEntityIDForActor(actor))
            return eid;
        if (CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByActorFormID(actor->formID))
            return p->entityID;
        return 0;
    }

    bool IsHit(const std::string& msg)
    {
        return msg.rfind("EHIT|", 0) == 0;
    }

    bool IsEnemyMessage(const std::string& msg)
    {
        return IsHit(msg) || msg.rfind("ENS|", 0) == 0;
    }

    void OnReceive(const std::string& msg)
    {
        LOG_REQUIREMENT("CoSyncEnemySync::OnReceive", "Parses peer text defensively and only queues; the tick applies it.");
        if (IsHit(msg))
        {
            RxHit h{};
            if (std::sscanf(msg.c_str() + 5, "%u|%u|%u|%f|%u", &h.target, &h.attacker, &h.seq, &h.damage, &h.flags) < 4 ||
                h.target == 0 || h.attacker == 0 || !std::isfinite(h.damage))
                return;
            std::lock_guard<std::mutex> lk(g_rxMx);
            if (g_rxHits.size() < 2048)
                g_rxHits.push_back(h);
            return;
        }
        // ENS|eid|ver|combat|target|class|life|casts
        RxState s{};
        const char* p = msg.c_str() + 4;
        uint32_t* fields[] = { &s.eid, &s.ver, &s.combat, &s.target, &s.cls, &s.life };
        for (uint32_t* fld : fields)
        {
            char* e = nullptr;
            *fld = static_cast<uint32_t>(std::strtoul(p, &e, 10));
            if (!e || e == p || *e != '|')
                return;
            p = e + 1;
        }
        if (s.eid == 0)
            return;
        while (*p && *p != '-')
        {
            char* e = nullptr;
            const uint32_t v = static_cast<uint32_t>(std::strtoul(p, &e, 10));
            if (!e || e == p)
                break;
            if (v != 0 && s.casts.size() < 4)
                s.casts.push_back(v);
            p = (*e == ',') ? e + 1 : e;
            if (*e != ',')
                break;
        }
        std::lock_guard<std::mutex> lk(g_rxMx);
        if (g_rxStates.size() < 2048)
            g_rxStates.push_back(std::move(s));
    }

    void Tick(double now)
    {
        LOG_REQUIREMENT("CoSyncEnemySync::Tick", "Game thread, every CoSync tick.");
        EnsureForms();
        RebuildGuard();
        if (!CoSyncNet::IsConnected())
        {
            std::lock_guard<std::mutex> lk(g_capMx);
            g_capKills.clear(); g_capHits.clear(); g_capCasts.clear();
            return;
        }
        ProcessCapturedKills();
        ProcessCapturedHits(now);
        ProcessCapturedCasts(now);
        ApplyReceivedHits();
        OwnerStateTick(now);
        ApplyRemoteStates(now);
        KillWatchdog(now);
        Diagnostics(now);
    }
}
