#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncCombatAuthority.h"
#include "CoSyncThrowableSync.h"
#include "CoSyncAnimationUtils.h"
#include "CoSyncAnimEventHook.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncEntityState.h"
#include "CoSyncNpcWorldSync.h"
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
#include "MinHook.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

extern PlayerCharacter*    CoSyncGetPlayer();
extern CoSyncPlayerManager g_CoSyncPlayerManager;

namespace
{
    // ── Engine addresses (1.11.240, IDA-verified 2026-09-29) ────────────────
    // TESObjectWEAP::Fire (CommonLibF4 ID 2198960 -> 0x4799B0 via the installed
    // Address Library; callers WeaponFireHandler::Handle sub_140DFFBE0, task
    // executor sub_140C4D320, sub_140988680).
    constexpr uintptr_t kRVA_WeaponFire = 0x004799B0;
    constexpr uint8_t   kWeaponFirePrologue[15] =
        { 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08 };
    // Actor::DoHitMe(HitData&) (CommonLibF4 ID 2231148 -> 0xCD2CA0). Reads the
    // aggressor handle from HitData+0x40 itself.
    constexpr uintptr_t kRVA_DoHitMe = 0x00CD2CA0;
    constexpr uint8_t   kDoHitMePrologue[14] =
        { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57 };
    // Actor::GetEquippedWeaponInstance(actor, out, equipIndex): the pair
    // {TESObjectWEAP*, TBO_InstanceData*} with a reference on the instance
    // (WeaponFireHandler uses it the same way right before Fire).
    constexpr uintptr_t kRVA_GetEquippedWeapon = 0x00CD0D10;
    constexpr uint8_t   kGetEquippedWeaponPrologue[15] =
        { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x50 };

    constexpr uintptr_t kActor_Process      = 0x300;   // Actor::currentProcess (AIProcess*)
    constexpr uintptr_t kProcess_AimTarget  = 0xBC;    // handle Fire aims at (sub_140CF1270)
    constexpr uintptr_t kHitData_Aggressor  = 0x40;    // HitData::aggressor

    using Fire_t     = void(__fastcall*)(void* weaponInstance, TESObjectREFR* source, uint32_t equipIndex, void* ammo, void* poison);
    using DoHitMe_t  = void(__fastcall*)(Actor* target, void* hitData);
    using GetWeap_t  = void*(__fastcall*)(Actor* actor, void* outPair, uint32_t equipIndex);

    Fire_t    g_origFire    = nullptr;
    DoHitMe_t g_origDoHitMe = nullptr;
    GetWeap_t g_getWeapon   = nullptr;
    uintptr_t g_base        = 0;
    bool      g_installed   = false;

    thread_local int t_replaying = 0;   // >0 while CoSync replays a remote shot

    // Shots by actors simulated here, recorded by the Fire hook (any thread),
    // sent from the game thread.
    struct LocalShot
    {
        uint32_t shooterFormID;
        uint32_t equipIndex;
        uint32_t weaponFormID;
        uint32_t targetHandle;
    };
    std::mutex             g_localMx;
    std::vector<LocalShot> g_localShots;

    struct RemoteShot
    {
        uint32_t shooterEid;
        uint32_t seq;
        uint32_t weaponFormID;
        uint32_t equipIndex;
        uint32_t targetEid;
        uint32_t flags;
        double   rxTime;
    };
    std::mutex             g_remoteMx;
    std::deque<RemoteShot> g_remoteShots;

    std::unordered_map<uint32_t, uint32_t> g_txSeq;      // shooterEid -> last seq sent   (game thread)
    std::unordered_map<uint32_t, uint32_t> g_rxSeq;      // shooterEid -> last seq played (game thread)

    // Diagnostics
    std::atomic<uint32_t> g_refusedNative{ 0 };
    std::atomic<uint32_t> g_lastRefusedFormID{ 0 };
    std::atomic<uint32_t> g_gatedHits{ 0 };
    std::atomic<uint32_t> g_lastGatedAggressor{ 0 };
    uint32_t g_txCount = 0, g_rxCount = 0, g_replayed = 0, g_animOnly = 0, g_skippedWeapon = 0,
             g_skippedStale = 0, g_skippedDead = 0, g_animAccepted = 0, g_animRejected = 0;

    double NowSec()
    {
        static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
    }

    __declspec(noinline) uint8_t SafeFormType(const TESForm* f)
    {
        __try { return f ? f->formType : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    __declspec(noinline) uint32_t SafeReadU32(const void* p, uintptr_t off)
    {
        __try { return p ? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p) + off) : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    __declspec(noinline) void* SafeReadPtr(const void* p, uintptr_t off)
    {
        __try { return p ? *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(p) + off) : nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    // Handle -> ref (engine LookupREFRByHandle, which validates index and age).
    TESObjectREFR* RefFromHandle(uint32_t handle)
    {
        if (handle == 0 || handle == 0xFFFFFFFFu)
            return nullptr;
        NiPointer<TESObjectREFR> ref;
        if (!LookupREFRByHandle(handle, ref))
            return nullptr;
        return ref.m_pObject;   // the ref stays alive (loaded, in the world); only used this frame
    }

    // ── Hooks ────────────────────────────────────────────────────────────────
    void __fastcall Detour_Fire(void* weaponInstance, TESObjectREFR* source, uint32_t equipIndex, void* ammo, void* poison)
    {
        if (t_replaying == 0 && source && SafeFormType(source) == kFormType_ACHR)
        {
            Actor* actor = static_cast<Actor*>(source);
            if (IsRegisteredRemoteActor(actor))
            {
                // Simulated elsewhere: its shots arrive as SHOT and are replayed
                // (t_replaying). Its own AI/graph firing here would be a second,
                // divergent gun.
                g_refusedNative.fetch_add(1, std::memory_order_relaxed);
                g_lastRefusedFormID.store(SafeReadU32(actor, 0x14), std::memory_order_relaxed);
                return;
            }
            LocalShot s{};
            s.shooterFormID = SafeReadU32(actor, 0x14);
            s.equipIndex    = equipIndex;
            TESForm* weap   = weaponInstance ? static_cast<TESForm*>(SafeReadPtr(weaponInstance, 0)) : nullptr;
            s.weaponFormID  = weap ? SafeReadU32(weap, 0x14) : 0;
            void* process   = SafeReadPtr(actor, kActor_Process);
            s.targetHandle  = process ? SafeReadU32(process, kProcess_AimTarget) : 0;
            if (s.shooterFormID != 0)
            {
                std::lock_guard<std::mutex> lk(g_localMx);
                if (g_localShots.size() < 256)
                    g_localShots.push_back(s);
            }
        }
        g_origFire(weaponInstance, source, equipIndex, ammo, poison);
    }

    void __fastcall Detour_DoHitMe(Actor* target, void* hitData)
    {
        if (hitData)
        {
            const uint32_t h = SafeReadU32(hitData, kHitData_Aggressor);
            if (h != 0)
            {
                TESObjectREFR* agg = RefFromHandle(h);
                if (agg && SafeFormType(agg) == kFormType_ACHR && IsRegisteredRemoteActor(static_cast<Actor*>(agg)))
                {
                    // The aggressor is simulated on another machine: that machine
                    // applies this hit; here it is only seen.
                    g_gatedHits.fetch_add(1, std::memory_order_relaxed);
                    g_lastGatedAggressor.store(SafeReadU32(agg, 0x14), std::memory_order_relaxed);
                    return;
                }
            }
        }
        g_origDoHitMe(target, hitData);
    }

    bool HookOne(uintptr_t rva, const uint8_t* prologue, size_t n, void* detour, void** orig, const char* name)
    {
        void* target = reinterpret_cast<void*>(g_base + rva);
        if (std::memcmp(target, prologue, n) != 0)
        {
            LOG_ERROR("[CombatAuthority] %s prologue mismatch at base+0x%llX (not 1.11.240?) -- hook skipped",
                name, (unsigned long long)rva);
            return false;
        }
        MH_STATUS st = MH_CreateHook(target, detour, orig);
        if (st != MH_OK)
        {
            LOG_ERROR("[CombatAuthority] MH_CreateHook(%s) failed: %d", name, (int)st);
            return false;
        }
        st = MH_EnableHook(target);
        if (st != MH_OK)
        {
            LOG_ERROR("[CombatAuthority] MH_EnableHook(%s) failed: %d", name, (int)st);
            MH_RemoveHook(target);
            *orig = nullptr;
            return false;
        }
        LOG_INFO("[CombatAuthority] %s hooked at base+0x%llX", name, (unsigned long long)rva);
        return true;
    }

    // ── Game-thread helpers ─────────────────────────────────────────────────
    bool LocallySimulatedEntity(uint32_t eid)
    {
        const auto& states = g_CoSyncPlayerManager.GetStates();
        auto it = states.find(eid);
        if (it == states.end() || !it->second.hasCreate)
            return false;
        return CoSyncNpcWorldSync::IsWorldActorEntityID(eid)
            ? it->second.isLocallyOwned
            : (CoSyncNet::IsHost() && it->second.hostAuthoritative);
    }

    // Entity ID the network knows `actor` by, from this machine's view.
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

    // Local actor for a network entity ID (the local player for our own ID).
    Actor* ActorOfEntity(uint32_t eid)
    {
        if (eid == 0)
            return nullptr;
        if (eid == CoSyncNet::GetMyEntityID())
            return reinterpret_cast<Actor*>(CoSyncGetPlayer());
        CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(eid);
        return (p && p->hasSpawned) ? p->actorRef : nullptr;
    }

    bool ProjectileExplodes(uint32_t weaponFormID)
    {
        TESForm* f = LookupFormByID(weaponFormID);
        if (!f || f->formType != kFormType_WEAP)
            return false;
        BGSProjectile* proj = CoSyncThrowableSync::GetWeaponProjectile(static_cast<TESObjectWEAP*>(f));
        return proj && proj->projectileData.explosionType != nullptr;
    }

    __declspec(noinline) bool SafeProjectileExplodes(uint32_t weaponFormID)
    {
        __try { return ProjectileExplodes(weaponFormID); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void SendLocalShots()
    {
        std::vector<LocalShot> shots;
        {
            std::lock_guard<std::mutex> lk(g_localMx);
            if (g_localShots.empty())
                return;
            shots.swap(g_localShots);
        }
        if (!CoSyncNet::IsConnected())
            return;

        // At most 30 shots per shooter per second on the wire (a minigun's
        // true rate still reads as continuous fire).
        static std::unordered_map<uint32_t, std::pair<double, int>> s_budget;
        const double now = NowSec();

        for (const LocalShot& s : shots)
        {
            TESForm* f = LookupFormByID(s.shooterFormID);
            if (!f || f->formType != kFormType_ACHR)
                continue;
            Actor* shooter = static_cast<Actor*>(f);
            const uint32_t eid = EntityIdOf(shooter);
            if (eid == 0)
                continue;
            const bool isMe = (shooter == reinterpret_cast<Actor*>(CoSyncGetPlayer()));
            if (!isMe && !LocallySimulatedEntity(eid))
                continue;   // a real actor here that the network knows under someone else's authority

            auto& b = s_budget[eid];
            if (now - b.first >= 1.0) { b.first = now; b.second = 0; }
            if (++b.second > 30)
                continue;

            Actor* target = nullptr;
            if (TESObjectREFR* t = RefFromHandle(s.targetHandle))
                if (SafeFormType(t) == kFormType_ACHR)
                    target = static_cast<Actor*>(t);
            const uint32_t targetEid = EntityIdOf(target);
            const uint32_t flags = SafeProjectileExplodes(s.weaponFormID) ? 1u : 0u;
            const uint32_t seq = ++g_txSeq[eid];

            char buf[128];
            std::snprintf(buf, sizeof(buf), "SHOT|%u|%u|%u|%u|%u|%u",
                eid, seq, s.weaponFormID, s.equipIndex, targetEid, flags);
            CoSyncTransport::Send(std::string(buf));
            ++g_txCount;
            if (g_txCount <= 20 || g_txCount % 200 == 0)
                LOG_UPDATED("[CombatAuthority] TX SHOT shooter=%u seq=%u weapon=0x%08X idx=%u target=%u%s (tx#%u)",
                    eid, seq, s.weaponFormID, s.equipIndex, targetEid, flags ? " explosive" : "", g_txCount);
        }
    }

    // Replays one shot through the engine on the proxy. SEH-isolated (no
    // unwinding objects here): the handle write is restored on every path.
    __declspec(noinline) bool EngineFire_Impl(Actor* shooter, uint32_t equipIndex, uint32_t targetHandle, uint32_t& firedWeapon)
    {
        firedWeapon = 0;
        void* pair[2] = { nullptr, nullptr };
        bool fired = false;
        uint8_t* process = nullptr;
        uint32_t oldTarget = 0;
        bool targetWritten = false;
        __try
        {
            g_getWeapon(shooter, pair, equipIndex);
            TESForm* weap = static_cast<TESForm*>(pair[0]);
            if (weap && weap->formType == kFormType_WEAP)
            {
                firedWeapon = weap->formID;
                process = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(shooter) + kActor_Process);
                if (process)
                {
                    oldTarget = *reinterpret_cast<uint32_t*>(process + kProcess_AimTarget);
                    *reinterpret_cast<uint32_t*>(process + kProcess_AimTarget) = targetHandle;
                    targetWritten = true;
                }
                ++t_replaying;
                g_origFire(pair, shooter, equipIndex, nullptr, nullptr);
                --t_replaying;
                fired = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            t_replaying = 0;
            fired = false;
        }
        __try
        {
            if (targetWritten)
                *reinterpret_cast<uint32_t*>(process + kProcess_AimTarget) = oldTarget;
            // Release the reference GetEquippedWeaponInstance gave us.
            void* inst = pair[1];
            if (inst && _InterlockedExchangeAdd(reinterpret_cast<volatile long*>(static_cast<uint8_t*>(inst) + 0x08), -1) == 1)
                reinterpret_cast<void(__fastcall*)(void*, int)>((*reinterpret_cast<void***>(inst))[0])(inst, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return fired;
    }

    void ReplayRemoteShots()
    {
        std::deque<RemoteShot> shots;
        {
            std::lock_guard<std::mutex> lk(g_remoteMx);
            if (g_remoteShots.empty())
                return;
            shots.swap(g_remoteShots);
        }
        const double now = NowSec();
        int budget = 24;   // engine fires per tick at most

        for (const RemoteShot& s : shots)
        {
            if (now - s.rxTime > 1.0)
            {
                ++g_skippedStale;   // a burst that arrived late is history, not a shot
                continue;
            }
            uint32_t& last = g_rxSeq[s.shooterEid];
            if (s.seq != 0 && s.seq <= last && last - s.seq < 0x10000000u)
                continue;           // duplicate / out of order
            last = s.seq;

            CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(s.shooterEid);
            if (!p || p->isLocalPlayer || !p->hasSpawned || !p->actorRef)
                continue;
            Actor* shooter = p->actorRef;
            if (!IsRegisteredRemoteActor(shooter))
                continue;           // we simulate this one: our own shot relayed back
            if (CoSyncGameAPI::IsDeadOrDying(shooter))
            {
                ++g_skippedDead;
                continue;
            }

            // The animation. Players keep 0.3.4's replay (attack state, shot,
            // muzzle event). NPC graphs: the attack state only -- the engine
            // fire below is the shot. Any fire the graph triggers itself on
            // the proxy is refused by the Fire hook, so nothing doubles.
            CoSyncAnimEventHook::SetApplyingRemote(true);
            const bool acc = CoSyncAnimUtils::SendEvent(shooter, CoSyncAnimUtils::Events::AttackStart);
            if (p->createType != CoSyncEntityType::NPC)
            {
                CoSyncAnimUtils::SendEvent(shooter, CoSyncAnimUtils::Events::WeaponFire);
                CoSyncAnimUtils::SendEvent(shooter, CoSyncAnimUtils::Events::WeaponFireEffect);
            }
            CoSyncAnimEventHook::SetApplyingRemote(false);
            acc ? ++g_animAccepted : ++g_animRejected;

            if (s.flags & 1u)
            {
                ++g_animOnly;       // explosive: flight + detonation come from Throwable/ExplosionSync
                continue;
            }
            if (budget <= 0)
                continue;
            --budget;

            uint32_t targetHandle = 0;
            if (Actor* t = ActorOfEntity(s.targetEid))
            {
                UInt32 h = 0;
                CreateHandleByREFR(h, t);
                targetHandle = h;
            }
            uint32_t firedWeapon = 0;
            const bool fired = EngineFire_Impl(shooter, s.equipIndex, targetHandle, firedWeapon);
            if (fired)
                ++g_replayed;
            if (!fired || firedWeapon != s.weaponFormID)
                ++g_skippedWeapon;   // not fired, or fired with what the proxy holds (a weapon swap in flight)
            if (g_replayed <= 20 || g_replayed % 200 == 0)
                LOG_UPDATED("[CombatAuthority] replayed SHOT shooter=%u seq=%u weapon=0x%08X (proxy holds 0x%08X) target=%u%s anim=%s",
                    s.shooterEid, s.seq, s.weaponFormID, firedWeapon, s.targetEid,
                    targetHandle ? "" : " (no local target: proxy's own aim)", acc ? "accepted" : "rejected");
        }
    }
}

namespace CoSyncCombatAuthority
{
    void Install()
    {
        LOG_REQUIREMENT("CoSyncCombatAuthority::Install", "Plugin load. Needs the verified 1.11.240 exe: every target's prologue is compared before hooking.");
        if (g_installed)
            return;
        g_installed = true;
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        {
            LOG_ERROR("[CombatAuthority] MH_Initialize failed: %d", (int)st);
            return;
        }
        if (std::memcmp(reinterpret_cast<void*>(g_base + kRVA_GetEquippedWeapon), kGetEquippedWeaponPrologue,
                        sizeof(kGetEquippedWeaponPrologue)) == 0)
            g_getWeapon = reinterpret_cast<GetWeap_t>(g_base + kRVA_GetEquippedWeapon);
        else
            LOG_ERROR("[CombatAuthority] GetEquippedWeaponInstance prologue mismatch -- shots will not be replayed");

        HookOne(kRVA_WeaponFire, kWeaponFirePrologue, sizeof(kWeaponFirePrologue),
                reinterpret_cast<void*>(&Detour_Fire), reinterpret_cast<void**>(&g_origFire), "TESObjectWEAP::Fire");
        HookOne(kRVA_DoHitMe, kDoHitMePrologue, sizeof(kDoHitMePrologue),
                reinterpret_cast<void*>(&Detour_DoHitMe), reinterpret_cast<void**>(&g_origDoHitMe), "Actor::DoHitMe");
    }

    bool IsShot(const std::string& msg)
    {
        return msg.rfind("SHOT|", 0) == 0;
    }

    void OnReceiveShot(const std::string& msg)
    {
        LOG_REQUIREMENT("CoSyncCombatAuthority::OnReceiveShot", "Parses peer-supplied text defensively; only queues. The replay runs in Tick on the game thread.");
        RemoteShot s{};
        unsigned long v[6] = {};
        const char* p = msg.c_str() + 5;
        for (int i = 0; i < 6; ++i)
        {
            char* e = nullptr;
            v[i] = std::strtoul(p, &e, 10);
            if (!e || e == p)
                return;
            p = (*e == '|') ? e + 1 : e;
        }
        s.shooterEid   = v[0];
        s.seq          = v[1];
        s.weaponFormID = v[2];
        s.equipIndex   = v[3];
        s.targetEid    = v[4];
        s.flags        = v[5];
        s.rxTime       = NowSec();
        if (s.shooterEid == 0 || s.shooterEid == CoSyncNet::GetMyEntityID())
            return;
        ++g_rxCount;
        std::lock_guard<std::mutex> lk(g_remoteMx);
        if (g_remoteShots.size() < 512)
            g_remoteShots.push_back(s);
    }

    void Tick(double now)
    {
        SendLocalShots();
        if (g_getWeapon && g_origFire)
            ReplayRemoteShots();

        static double s_lastDiag = 0.0;
        if (now - s_lastDiag >= 10.0)
        {
            s_lastDiag = now;
            const uint32_t refused = g_refusedNative.exchange(0);
            const uint32_t gated   = g_gatedHits.exchange(0);
            if (g_txCount || g_rxCount || refused || gated)
                LOG_UPDATED("[CombatAuthority] last 10 s: tx=%u rx=%u replayed=%u animOnly(explosive)=%u weaponMismatch=%u stale=%u deadShooter=%u "
                            "| attack anim accepted=%u rejected=%u | proxy native fires refused=%u (last 0x%08X) | remote-aggressor hits refused=%u (last 0x%08X)",
                    g_txCount, g_rxCount, g_replayed, g_animOnly, g_skippedWeapon, g_skippedStale, g_skippedDead,
                    g_animAccepted, g_animRejected, refused, g_lastRefusedFormID.load(), gated, g_lastGatedAggressor.load());
            g_txCount = g_rxCount = g_replayed = g_animOnly = g_skippedWeapon = g_skippedStale = g_skippedDead = 0;
            g_animAccepted = g_animRejected = 0;
        }
    }

    void Reset()
    {
        {
            std::lock_guard<std::mutex> lk(g_localMx);
            g_localShots.clear();
        }
        {
            std::lock_guard<std::mutex> lk(g_remoteMx);
            g_remoteShots.clear();
        }
        g_txSeq.clear();
        g_rxSeq.clear();
    }
}
