#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncMineSync.h"
#include "CoSyncEnemySync.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncGameAPI.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "ConsoleLogger.h"
#include "GameReferences.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"
#include "MinHook.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern PlayerCharacter*    CoSyncGetPlayer();
extern CoSyncPlayerManager g_CoSyncPlayerManager;
extern RelocAddr<_LookupFormByID> LookupFormByID;

namespace
{
    // ── Engine (Fallout4.exe 1.11.240, IDA 2026-10-01; see the header) ──────
    constexpr uintptr_t kRVA_MineCanTrigger = 0x00E48440;   // (projectile, actor, shooter, x) -> bool
    constexpr uint8_t   kPro_MineCanTrigger[19] = { 0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x30 };
    constexpr uintptr_t kRVA_GrenadeProjVtbl = 0x0257DB78;  // ??_7GrenadeProjectile@@6B@ (mines are lobbed projectiles)
    constexpr uintptr_t kProj_ShooterHandle  = 0x1A8;       // read by sub_140E51300 / sub_140E48440

    using CanTrigger_t = uint64_t(__fastcall*)(void*, Actor*, Actor*, void*);

    uintptr_t    g_base = 0;
    CanTrigger_t g_origCanTrigger = nullptr;
    std::atomic<uint32_t> g_dRefusedTriggers{ 0 };

    // A mine on this machine is triggered only by an actor this machine
    // simulates: never by another player's proxy or another machine's NPC.
    uint64_t __fastcall Detour_MineCanTrigger(void* projectile, Actor* actor, Actor* shooter, void* x)
    {
        if (actor && (CoSyncEnemySync::IsPlayerProxy(actor) || CoSyncEnemySync::IsGuardedPuppet(actor)))
        {
            g_dRefusedTriggers.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        return g_origCanTrigger(projectile, actor, shooter, x);
    }

    double NowSeconds()
    {
        LARGE_INTEGER f{}, c{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
    }

    uint64_t KeyOf(uint32_t owner, uint32_t ref) { return (static_cast<uint64_t>(owner) << 32) | ref; }

    enum class Kind : uint8_t { Origin, Static, Replica, Linked };

    struct LocalMine
    {
        Kind     kind = Kind::Origin;
        uint32_t base = 0;
        uint32_t liveRef = 0;       // the live projectile here (0 while a replica is pending)
        uint32_t placedRef = 0;     // the ref we placed to make the engine build a replica
        uint32_t shooterEid = 0;
        NiPoint3 pos{ 0.f, 0.f, 0.f };
        float    rotZ = 0.f;
        void*    cell = nullptr;
        double   firstSeen = 0.0, stableSince = 0.0, lastAnnounce = -100.0, spawnedAt = 0.0;
        bool     announced = false;
        std::vector<uint64_t> aliases;   // other machines' keys for this same mine
    };
    std::unordered_map<uint64_t, LocalMine> g_mines;           // key -> mine here
    std::unordered_map<uint32_t, uint64_t>  g_keyOfLiveRef;    // live ref formID -> key
    std::unordered_set<uint32_t>            g_ourRefs;         // refs we created (never announced as ours)
    std::unordered_set<uint32_t>            g_removedByUs;     // refs we removed (their disappearance is not news)
    std::unordered_map<uint64_t, double>    g_ended;           // keys that ended (MINEX) -> when

    struct Scanned { TESObjectREFR* ref; uint32_t base; NiPoint3 pos; float rotZ; void* cell; };
    std::unordered_map<uint32_t, Scanned> g_lastScan;          // fid -> scan entry
    std::unordered_set<void*>             g_lastScannedCells;

    struct RxMine { uint32_t sender, owner, ref, proj, shooter; NiPoint3 pos; float rotZ; uint32_t ws, cell; };
    std::vector<RxMine> g_rxMines;
    struct RxEnd { uint32_t sender, owner, ref; };
    std::vector<RxEnd> g_rxEnds;

    uint32_t g_dTxMine = 0, g_dTxEnd = 0, g_dRxMine = 0, g_dRxEnd = 0, g_dSpawned = 0, g_dLinked = 0, g_dRemoved = 0;

    // ── Helpers ─────────────────────────────────────────────────────────────
    __declspec(noinline) bool SafeIsMineBase(TESForm* base)
    {
        __try
        {
            if (!base || base->formType != kFormType_PROJ) return false;
            BGSProjectile* p = static_cast<BGSProjectile*>(base);
            return p->projectileData.explosionProximity > 0.0f && p->projectileData.explosionType != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    __declspec(noinline) bool SafeIsGrenadeProjectile(TESObjectREFR* ref)
    {
        __try { return ref && *reinterpret_cast<uintptr_t*>(ref) == g_base + kRVA_GrenadeProjVtbl; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    __declspec(noinline) uint32_t SafeReadShooterHandle(TESObjectREFR* proj)
    {
        __try { return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(proj) + kProj_ShooterHandle); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    __declspec(noinline) void SafeWriteShooterHandle(TESObjectREFR* proj, uint32_t h)
    {
        __try { *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(proj) + kProj_ShooterHandle) = h; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    Actor* ActorOfEntity(uint32_t eid)
    {
        if (eid == 0) return nullptr;
        if (eid == CoSyncNet::GetMyEntityID()) return reinterpret_cast<Actor*>(CoSyncGetPlayer());
        CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(eid);
        if (!p || !p->hasSpawned || !p->actorRef) return nullptr;
        return CoSyncGameAPI::IsFormStillLive(static_cast<TESForm*>(p->actorRef)) ? p->actorRef : nullptr;
    }

    uint32_t ShooterEidOf(TESObjectREFR* proj)
    {
        const uint32_t h = SafeReadShooterHandle(proj);
        if (h == 0 || h == 0xFFFFFFFFu) return 0;
        NiPointer<TESObjectREFR> ref;
        if (!LookupREFRByHandle(h, ref) || !ref.m_pObject || ref.m_pObject->formType != kFormType_ACHR)
            return 0;
        return CoSyncEnemySync::EntityIdOf(static_cast<Actor*>(ref.m_pObject));
    }

    void GiveShooter(TESObjectREFR* proj, uint32_t shooterEid)
    {
        if (!proj || shooterEid == 0 || !SafeIsGrenadeProjectile(proj))
            return;
        Actor* a = ActorOfEntity(shooterEid);
        if (!a) return;
        UInt32 h = 0;
        CreateHandleByREFR(h, a);
        if (h) SafeWriteShooterHandle(proj, h);
    }

    // Removes this machine's copy without detonating it (native disable first,
    // as CoSyncThrowableSync does for its refs; Papyrus Delete reclaims it).
    void RemoveRef(uint32_t fid, bool deleteIt)
    {
        if (fid == 0) return;
        TESForm* f = LookupFormByID(fid);
        if (!f) return;
        TESObjectREFR* r = static_cast<TESObjectREFR*>(f);
        g_removedByUs.insert(fid);
        CoSyncGameAPI::DisableWorldActor(r);
        if (deleteIt)
        {
            static const BSFixedString kDelete("Delete");
            VMArray<VMVariable> args;
            CallFunctionNoWait(r, kDelete, args);
        }
    }

    void SendEnd(uint64_t key)
    {
        const uint32_t me = CoSyncNet::GetMyEntityID();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "MINEX|%u|%u|%u", me, static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key));
        CoSyncTransport::Send(buf);
        ++g_dTxEnd;
    }

    void Announce(uint64_t key, LocalMine& m, double now)
    {
        uint32_t ws = 0xFFFFFFFFu, cell = 0;
        g_CoSyncPlayerManager.GetEntityLocation(CoSyncNet::GetMyEntityID(), ws, cell);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "MINE|%u|%u|%u|%u|%u|%.1f|%.1f|%.1f|%.3f|%u|%u",
            CoSyncNet::GetMyEntityID(), static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key),
            m.base, m.shooterEid, m.pos.x, m.pos.y, m.pos.z, m.rotZ, ws, cell);
        CoSyncTransport::Send(buf);
        if (!m.announced)
            LOG_UPDATED("[MineSync] TX mine 0x%08X base=0x%08X at (%.0f,%.0f,%.0f) shooter=%u",
                static_cast<uint32_t>(key), m.base, m.pos.x, m.pos.y, m.pos.z, m.shooterEid);
        m.announced = true;
        m.lastAnnounce = now;
        ++g_dTxMine;
    }

    // ── Scan ────────────────────────────────────────────────────────────────
    __declspec(noinline) void ScanCell(TESObjectCELL* cell)
    {
        __try
        {
            for (UInt32 i = 0; i < cell->objectList.count; ++i)
            {
                TESObjectREFR* ref = cell->objectList.entries[i];
                if (!ref || !ref->baseForm || ref->baseForm->formType != kFormType_PROJ)
                    continue;
                if (ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled))
                    continue;
                if (!SafeIsMineBase(ref->baseForm))
                    continue;
                g_lastScan[ref->formID] = Scanned{ ref, ref->baseForm->formID, ref->pos, ref->rot.z, cell };
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void Scan()
    {
        g_lastScan.clear();
        g_lastScannedCells.clear();
        std::unordered_set<TESObjectCELL*> cells;
        if (PlayerCharacter* pc = CoSyncGetPlayer())
            if (TESObjectCELL* c = reinterpret_cast<TESObjectREFR*>(pc)->parentCell)
                cells.insert(c);
        for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
        {
            const CoSyncPlayer* p = kv.second.get();
            if (p && p->createType == CoSyncEntityType::Player && p->hasSpawned && p->actorRef &&
                CoSyncGameAPI::IsFormStillLive(static_cast<TESForm*>(p->actorRef)) && p->actorRef->parentCell)
                cells.insert(p->actorRef->parentCell);
        }
        for (const auto& kv : g_mines)
            if (kv.second.cell) cells.insert(static_cast<TESObjectCELL*>(kv.second.cell));
        for (TESObjectCELL* c : cells)
        {
            g_lastScannedCells.insert(c);
            ScanCell(c);
        }
    }

    float Dist2(const NiPoint3& a, const NiPoint3& b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    }

    void ResolvePendingReplicas(double now)
    {
        for (auto& kv : g_mines)
        {
            LocalMine& m = kv.second;
            if (m.kind != Kind::Replica || m.liveRef != 0)
                continue;
            // The live projectile the engine built for our placed ref: a new
            // mine of the same base where we placed it. Prefer one that is not
            // the placed ref itself; the placed ref only after a grace period.
            uint32_t best = 0;
            for (const auto& s : g_lastScan)
            {
                if (s.second.base != m.base || g_keyOfLiveRef.count(s.first) || Dist2(s.second.pos, m.pos) > 200.f * 200.f)
                    continue;
                if (s.first == m.placedRef)
                {
                    if (now - m.spawnedAt >= 1.0 && best == 0) best = s.first;
                    continue;
                }
                if ((s.first & 0xFF000000u) == 0xFF000000u) { best = s.first; break; }
            }
            if (best == 0)
            {
                if (now - m.spawnedAt > 5.0)
                    LOG_UPDATED_WARN("[MineSync] replica of 0x%08X: no live projectile appeared for placed ref 0x%08X yet",
                        static_cast<uint32_t>(kv.first), m.placedRef);
                continue;
            }
            m.liveRef = best;
            g_keyOfLiveRef[best] = kv.first;
            g_ourRefs.insert(best);
            const Scanned& s = g_lastScan[best];
            m.cell = s.cell;
            GiveShooter(s.ref, m.shooterEid);
            LOG_UPDATED("[MineSync] replica of mine 0x%08X is live here as 0x%08X (placed 0x%08X, shooter=%u)",
                static_cast<uint32_t>(kv.first), best, m.placedRef, m.shooterEid);
        }
    }

    void TrackLocal(double now)
    {
        const uint32_t me = CoSyncNet::GetMyEntityID();
        for (const auto& s : g_lastScan)
        {
            const uint32_t fid = s.first;
            auto k = g_keyOfLiveRef.find(fid);
            if (k != g_keyOfLiveRef.end())
            {
                LocalMine& m = g_mines[k->second];
                if (Dist2(m.pos, s.second.pos) > 4.f) { m.pos = s.second.pos; m.stableSince = now; }
                m.rotZ = s.second.rotZ;
                m.cell = s.second.cell;
                continue;
            }
            if (g_ourRefs.count(fid))
                continue;   // a placed ref still waiting to be matched
            const bool runtime = (fid & 0xFF000000u) == 0xFF000000u;
            // A replica the engine is still building (it can roll a little
            // before it rests): never mistake it for a mine of our own.
            if (runtime)
            {
                bool pendingNear = false;
                for (const auto& pm : g_mines)
                    if (pm.second.kind == Kind::Replica && pm.second.liveRef == 0 && now - pm.second.spawnedAt < 5.0 &&
                        pm.second.base == s.second.base && Dist2(pm.second.pos, s.second.pos) < 400.f * 400.f)
                        pendingNear = true;
                if (pendingNear)
                    continue;
            }
            const uint64_t key = runtime ? KeyOf(me, fid) : KeyOf(0, fid);
            LocalMine m;
            m.kind = runtime ? Kind::Origin : Kind::Static;
            m.base = s.second.base;
            m.liveRef = fid;
            m.pos = s.second.pos;
            m.rotZ = s.second.rotZ;
            m.cell = s.second.cell;
            m.firstSeen = m.stableSince = now;
            m.shooterEid = runtime ? ShooterEidOf(s.second.ref) : 0;
            g_mines[key] = m;
            g_keyOfLiveRef[fid] = key;
        }

        // Announce origin mines once they rest (a thrown mine flies first).
        for (auto& kv : g_mines)
        {
            LocalMine& m = kv.second;
            if (m.kind != Kind::Origin || m.liveRef == 0 || !g_lastScan.count(m.liveRef))
                continue;
            if (now - m.stableSince < 0.6)
                continue;
            if (!m.announced || now - m.lastAnnounce >= 15.0)
                Announce(kv.first, m, now);
        }

        // Gone: detonated, disarmed or picked up here -> everyone else removes
        // theirs. A mine whose cell is not scanned any more only unloaded.
        std::vector<uint64_t> gone;
        for (auto& kv : g_mines)
        {
            LocalMine& m = kv.second;
            if (m.liveRef == 0)
            {
                if (m.kind == Kind::Replica && now - m.spawnedAt > 20.0)
                    gone.push_back(kv.first);   // the engine never built it: give up quietly
                continue;
            }
            if (g_lastScan.count(m.liveRef))
                continue;
            if (!g_lastScannedCells.count(m.cell))
                continue;   // unloaded, not ended
            const bool ours = g_removedByUs.erase(m.liveRef) != 0;
            if (!ours)
            {
                SendEnd(kv.first);
                for (uint64_t a : m.aliases) SendEnd(a);
                LOG_UPDATED("[MineSync] mine 0x%08X (base 0x%08X) ended here (detonated/disarmed) -- peers remove theirs",
                    m.liveRef, m.base);
            }
            gone.push_back(kv.first);
        }
        for (uint64_t key : gone)
        {
            auto it = g_mines.find(key);
            if (it == g_mines.end()) continue;
            if (it->second.placedRef) { RemoveRef(it->second.placedRef, true); g_ourRefs.erase(it->second.placedRef); }
            g_keyOfLiveRef.erase(it->second.liveRef);
            g_ourRefs.erase(it->second.liveRef);
            g_ended[key] = now;
            for (uint64_t a : it->second.aliases) g_ended[a] = now;
            g_mines.erase(it);
        }
    }

    void ApplyReceived(double now)
    {
        std::vector<RxEnd> ends;
        ends.swap(g_rxEnds);
        for (const RxEnd& e : ends)
        {
            const uint64_t key = KeyOf(e.owner, e.ref);
            g_ended[key] = now;
            for (auto it = g_mines.begin(); it != g_mines.end(); ++it)
            {
                LocalMine& m = it->second;
                bool match = (it->first == key);
                for (uint64_t a : m.aliases) match = match || a == key;
                if (!match) continue;
                RemoveRef(m.liveRef, m.kind != Kind::Static);
                if (m.placedRef) RemoveRef(m.placedRef, true);
                ++g_dRemoved;
                LOG_UPDATED("[MineSync] RX end of mine %u:0x%08X from %u -- removed our copy 0x%08X (no detonation here; its explosion arrives as EXP)",
                    e.owner, e.ref, e.sender, m.liveRef);
                g_ended[it->first] = now;
                g_keyOfLiveRef.erase(m.liveRef);
                g_ourRefs.erase(m.placedRef);
                g_mines.erase(it);
                break;
            }
        }

        std::vector<RxMine> mines;
        mines.swap(g_rxMines);
        const uint32_t me = CoSyncNet::GetMyEntityID();
        PlayerCharacter* pc = CoSyncGetPlayer();
        for (const RxMine& r : mines)
        {
            if (r.owner == me) continue;
            const uint64_t key = KeyOf(r.owner, r.ref);
            auto ended = g_ended.find(key);
            if (ended != g_ended.end() && now - ended->second < 120.0) continue;
            if (g_mines.count(key)) continue;
            bool aliased = false;
            for (auto& kv : g_mines)
                for (uint64_t a : kv.second.aliases) aliased = aliased || a == key;
            if (aliased) continue;
            if (!pc || !CoSyncPlayerManager::IsLocalLocation(r.ws, r.cell)) continue;   // re-announced every 15 s
            const NiPoint3 ppos = reinterpret_cast<TESObjectREFR*>(pc)->pos;
            if (Dist2(ppos, r.pos) > 16384.f * 16384.f) continue;

            // The same mine may already exist here (a CK mine, or both machines
            // saw one appear): link to it instead of placing a second.
            uint64_t linkTo = 0;
            for (auto& kv : g_mines)
            {
                if (kv.second.liveRef && kv.second.base == r.proj && Dist2(kv.second.pos, r.pos) < 64.f * 64.f)
                { linkTo = kv.first; break; }
            }
            if (linkTo)
            {
                g_mines[linkTo].aliases.push_back(key);
                ++g_dLinked;
                LOG_UPDATED("[MineSync] mine %u:0x%08X is our 0x%08X (same mine) -- linked", r.owner, r.ref, g_mines[linkTo].liveRef);
                continue;
            }

            TESForm* base = LookupFormByID(r.proj);
            if (!SafeIsMineBase(base)) continue;
            NiPoint3 rot(0.f, 0.f, r.rotZ);
            TESObjectREFR* placed = CoSyncGameAPI::SpawnItemAtPosition(base, r.pos, rot);
            if (!placed) continue;
            LocalMine m;
            m.kind = Kind::Replica;
            m.base = r.proj;
            m.placedRef = placed->formID;
            m.shooterEid = r.shooter;
            m.pos = r.pos;
            m.rotZ = r.rotZ;
            m.spawnedAt = m.firstSeen = m.stableSince = now;
            g_ourRefs.insert(placed->formID);
            g_mines[key] = m;
            ++g_dSpawned;
            LOG_UPDATED("[MineSync] RX mine %u:0x%08X base=0x%08X -- placing a real mine here (placed 0x%08X)",
                r.owner, r.ref, r.proj, placed->formID);
        }

        for (auto it = g_ended.begin(); it != g_ended.end(); )
            it = (now - it->second > 300.0) ? g_ended.erase(it) : std::next(it);
    }
}

namespace CoSyncMineSync
{
    void Install()
    {
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        {
            LOG_ERROR("[MineSync] MH_Initialize failed: %d", static_cast<int>(st));
            return;
        }
        void* target = reinterpret_cast<void*>(g_base + kRVA_MineCanTrigger);
        if (std::memcmp(target, kPro_MineCanTrigger, sizeof(kPro_MineCanTrigger)) != 0)
        {
            LOG_ERROR("[MineSync] mine trigger test prologue mismatch at base+0x%llX (not 1.11.240?) -- proxies can still trigger mines",
                static_cast<unsigned long long>(kRVA_MineCanTrigger));
        }
        else if (MH_CreateHook(target, reinterpret_cast<void*>(&Detour_MineCanTrigger), reinterpret_cast<void**>(&g_origCanTrigger)) != MH_OK ||
                 MH_EnableHook(target) != MH_OK)
        {
            LOG_ERROR("[MineSync] could not hook the mine trigger test");
            g_origCanTrigger = nullptr;
        }
        else
            LOG_INFO("[MineSync] mine trigger test hooked at base+0x%llX (proxies and puppets never trigger mines here)",
                static_cast<unsigned long long>(kRVA_MineCanTrigger));
    }

    void Reset()
    {
        // Replicas of other machines' mines go with the session; our own and CK mines stay.
        for (auto& kv : g_mines)
        {
            if (kv.second.kind == Kind::Replica)
            {
                RemoveRef(kv.second.liveRef, true);
                if (kv.second.placedRef) RemoveRef(kv.second.placedRef, true);
            }
        }
        g_mines.clear(); g_keyOfLiveRef.clear(); g_ourRefs.clear(); g_removedByUs.clear(); g_ended.clear();
        g_lastScan.clear(); g_lastScannedCells.clear(); g_rxMines.clear(); g_rxEnds.clear();
    }

    void Tick(double now)
    {
        if (!CoSyncNet::IsConnected())
            return;
        static double s_next = 0.0;
        if (now < s_next)
            return;
        s_next = now + 0.25;

        ApplyReceived(now);
        Scan();
        ResolvePendingReplicas(now);
        TrackLocal(now);

        static double s_nextDiag = 0.0;
        if (now >= s_nextDiag)
        {
            s_nextDiag = now + 30.0;
            const uint32_t refused = g_dRefusedTriggers.exchange(0);
            if (g_dTxMine || g_dTxEnd || g_dRxMine || g_dRxEnd || g_dSpawned || g_dLinked || g_dRemoved || refused)
                LOG_UPDATED("[MineSync] last 30 s: TX MINE=%u MINEX=%u | RX MINE=%u MINEX=%u | replicas placed=%u linked=%u removed=%u | "
                            "proxy/puppet mine triggers refused=%u | mines tracked=%zu",
                    g_dTxMine, g_dTxEnd, g_dRxMine, g_dRxEnd, g_dSpawned, g_dLinked, g_dRemoved, refused, g_mines.size());
            g_dTxMine = g_dTxEnd = g_dRxMine = g_dRxEnd = g_dSpawned = g_dLinked = g_dRemoved = 0;
        }
    }

    bool IsMineMsg(const std::string& msg)
    {
        return msg.rfind("MINE|", 0) == 0 || msg.rfind("MINEX|", 0) == 0;
    }

    void OnReceive(const std::string& msg)
    {
        if (msg.rfind("MINEX|", 0) == 0)
        {
            RxEnd e{};
            if (std::sscanf(msg.c_str() + 6, "%u|%u|%u", &e.sender, &e.owner, &e.ref) != 3 || e.ref == 0)
                return;
            if (e.sender == CoSyncNet::GetMyEntityID())
                return;
            ++g_dRxEnd;
            if (g_rxEnds.size() < 512) g_rxEnds.push_back(e);
            return;
        }
        RxMine r{};
        if (std::sscanf(msg.c_str() + 5, "%u|%u|%u|%u|%u|%f|%f|%f|%f|%u|%u", &r.sender, &r.owner, &r.ref, &r.proj, &r.shooter,
                        &r.pos.x, &r.pos.y, &r.pos.z, &r.rotZ, &r.ws, &r.cell) != 11 || r.ref == 0 || r.proj == 0)
            return;
        if (!std::isfinite(r.pos.x) || !std::isfinite(r.pos.y) || !std::isfinite(r.pos.z) || !std::isfinite(r.rotZ))
            return;
        ++g_dRxMine;
        if (g_rxMines.size() < 512) g_rxMines.push_back(r);
    }
}
