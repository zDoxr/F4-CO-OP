#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncExplosionSync.h"
#include <algorithm>

#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncGameAPI.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameRTTI.h"
#include "GameObjects.h"
#include "PapyrusVM.h"

#include <mutex>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <unordered_map>

extern PlayerCharacter* CoSyncGetPlayer();
extern RelocPtr<GameVM*> g_gameVM;
extern RelocAddr<_LookupFormByID>      LookupFormByID;
extern RelocAddr<_MoveRefrToPosition>  MoveRefrToPosition;
extern RelocAddr<_PlaceAtMe_Native>    PlaceAtMe_Native;

// Local FormToREFR (CoSyncWorldSync.cpp keeps its own file-static copy too).
static TESObjectREFR* FormToREFR(TESForm* form)
{
    if (!form) return nullptr;
    if (form->formType != kFormType_REFR && form->formType != kFormType_ACHR)
        return nullptr;
    return static_cast<TESObjectREFR*>(form);
}

namespace
{
    // f4se GameForms.h form-type table: "EXPL 90 BGSExplosion".
    constexpr UInt8 kFormType_EXPL = 90;

    // Explosions are transient, so poll faster than the debris baseline or we
    // miss them entirely. Still far cheaper than a hook on a combat path.
    constexpr double kExpl_PollInterval = 0.1;
    double s_lastPoll = 0.0;

    // Every explosion ref we have already accounted for -- either we saw it
    // locally and broadcast it, or WE spawned it replaying a peer. The second
    // case is what stops the obvious feedback loop: peer A's explosion spawns a
    // ref on peer B, whose own scan would otherwise broadcast it straight back.
    std::unordered_set<uint32_t> s_knownExplosionRefs;
    // Bases that faulted PlaceAtMe_Native -- never retried (mirrors the WBF
    // s_workshopFaultedBaseForms policy in CoSyncWorldSync.cpp).
    std::unordered_set<uint32_t> s_faultedBases;

    struct PendingExplosion
    {
        uint32_t baseFormID = 0;
        float    x = 0.f, y = 0.f, z = 0.f;
    };
    std::mutex                   s_mutex;
    std::vector<PendingExplosion> s_pending;

    // 0.3.5: explosions that already happened HERE (detected by the local
    // scan, not our own replays). A replicated destruction (CoSyncWorldState-
    // Sync) or a replayed shot makes this machine's engine produce the same
    // explosion itself -- the car blowing up at its last stage -- so the
    // sender's EXP for it must not add a second one. Game thread only.
    struct RecentExplosion { uint32_t base; float x, y, z; double t; };
    std::vector<RecentExplosion> s_recentLocal;

    bool MatchesRecentLocal(uint32_t base, float x, float y, float z, double now)
    {
        for (const RecentExplosion& r : s_recentLocal)
        {
            if (r.base != base || now - r.t > 3.0) continue;
            const float dx = r.x - x, dy = r.y - y, dz = r.z - z;
            if (dx * dx + dy * dy + dz * dz <= 400.0f * 400.0f)
                return true;
        }
        return false;
    }

    struct ExplDiag
    {
        uint32_t sent      = 0;
        uint32_t applied   = 0;
        uint32_t noBase    = 0;
        uint32_t placeFail = 0;
        uint32_t skipped   = 0;  // already known (our own replay)
    };
    ExplDiag s_diag{};
    double   s_lastDiagLog = 0.0;
    constexpr double kDiagInterval = 5.0;

    double NowSeconds()
    {
        LARGE_INTEGER f{}, c{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
    }

    // Same pre-clear + SEH shape as CoSyncWorldSync::PlaceAtMeOnPlayerSafe --
    // see the long round-55/58/61/67 history there. vm must be the REAL vm
    // (nullptr under-retains PlaceAtMe's refcount tracking -> heap corruption).
    __declspec(noinline) TESObjectREFR* PlaceAtMeSafe(VirtualMachine* vm,
                                                      TESObjectREFR* pcRefr,
                                                      void** formPair)
    {
        __try
        {
            auto* p = reinterpret_cast<uint8_t*>(pcRefr);
            uint32_t* field7C = reinterpret_cast<uint32_t*>(p + 0x7C);
            if (*field7C != 0)
                *field7C = 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        __try
        {
            // f4se 0.7.9 added two trailing bool params to PlaceAtMe_Native (was 6 args, now 8) --
            // kept the same physical args in the same positions, false/false for the new ones.
            return PlaceAtMe_Native(vm, 0, reinterpret_cast<TESObjectREFR**>(formPair), pcRefr, 1, false, false, false);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    // A base is only blacklisted after REPEATED failures. The first live test
    // showed why one strike is wrong: a single bad spawn path permanently
    // disabled a perfectly valid Molotov base for the whole session.
    std::unordered_map<uint32_t, int> s_placeFailures;
    constexpr int kMaxPlaceFailures = 3;

    void LogDiag(double now)
    {
        if ((now - s_lastDiagLog) < kDiagInterval) return;
        s_lastDiagLog = now;
        const ExplDiag& d = s_diag;
        if (d.sent || d.applied || d.noBase || d.placeFail || d.skipped)
        {
            LOG_INFO("[ExplDiag] last %.0fs: sent=%u applied=%u noBase=%u "
                     "placeFail=%u skippedKnown=%u | tracked=%u",
                kDiagInterval, d.sent, d.applied, d.noBase, d.placeFail,
                d.skipped, (unsigned)s_knownExplosionRefs.size());
        }
        s_diag = ExplDiag{};
    }

    __declspec(noinline) void Tick_Impl(double now)
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc) return;
        TESObjectREFR* pcRefr = reinterpret_cast<TESObjectREFR*>(pc);
        TESObjectCELL* cell = pcRefr->parentCell;
        if (!cell) return;

        for (UInt32 i = 0; i < cell->objectList.count; ++i)
        {
            TESObjectREFR* ref = cell->objectList.entries[i];
            if (!ref || !ref->baseForm) continue;
            if (ref->baseForm->formType != kFormType_EXPL) continue;

            const uint32_t fid = ref->formID;
            if (fid == 0) continue;

            // Seen already (ours, or a replay we spawned) -> never rebroadcast.
            if (!s_knownExplosionRefs.insert(fid).second)
            {
                s_diag.skipped++;
                continue;
            }

            {
                const double tnow = NowSeconds();
                s_recentLocal.erase(std::remove_if(s_recentLocal.begin(), s_recentLocal.end(),
                    [tnow](const RecentExplosion& r) { return tnow - r.t > 5.0; }), s_recentLocal.end());
                s_recentLocal.push_back({ ref->baseForm->formID, ref->pos.x, ref->pos.y, ref->pos.z, tnow });
            }
            char buf[128];
            std::snprintf(buf, sizeof(buf), "EXP|%u|%.1f|%.1f|%.1f",
                ref->baseForm->formID, ref->pos.x, ref->pos.y, ref->pos.z);
            CoSyncTransport::Send(std::string(buf));
            s_diag.sent++;

            LOG_INFO("[ExplSync TX] base=0x%08X ref=0x%08X pos=(%.0f,%.0f,%.0f)",
                ref->baseForm->formID, fid, ref->pos.x, ref->pos.y, ref->pos.z);
        }

        // The tracked set would otherwise grow for the whole session. Explosion
        // refs are transient, so once it gets large just drop it -- the worst
        // case from forgetting one is a single duplicate replay, and a stale
        // formID can be reused by the engine for something else entirely.
        if (s_knownExplosionRefs.size() > 4096)
        {
            LOG_INFO("[ExplSync] clearing tracked ref set (%u entries)",
                (unsigned)s_knownExplosionRefs.size());
            s_knownExplosionRefs.clear();
        }
    }

    __declspec(noinline) void Flush_Impl(const std::vector<PendingExplosion>& pending)
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc) return;
        TESObjectREFR* pcRefr = reinterpret_cast<TESObjectREFR*>(pc);

        GameVM* vm = *g_gameVM;
        if (!vm || !vm->m_virtualMachine) return;

        for (const auto& e : pending)
        {
            if (s_faultedBases.count(e.baseFormID)) continue;

            TESForm* base = LookupFormByID(e.baseFormID);
            if (!base)
            {
                s_diag.noBase++;
                continue;
            }
            // Peer-supplied formID: refuse anything that is not actually an
            // explosion, so a malformed/hostile packet can't PlaceAtMe an
            // arbitrary form at the player.
            if (base->formType != kFormType_EXPL)
            {
                s_diag.noBase++;
                LOG_WARN("[ExplSync] RX base=0x%08X is formType %u, not EXPL — ignored",
                    e.baseFormID, (unsigned)base->formType);
                continue;
            }

            // 2026-08-03: spawn DIRECTLY at the reported position via
            // CreateReferenceAtLocation. This replaces the earlier XMarker-anchor
            // workaround entirely and fixes two things at once:
            //   * an explosion detonates the instant it is created, so it MUST
            //     be born at the target -- "PlaceAtMe(player) then move" would
            //     have gone off at the receiving player's own feet;
            //   * PlaceAtMe_Native faults inside its component-lookup-then-lock
            //     sequence (RVA 0x16590EF, the 0x165910F family) even for
            //     ordinary bases -- see the long note in CoSyncWorldSync.cpp's
            //     WBF apply. SpawnItemAtPosition does not go through it.
            if (MatchesRecentLocal(e.baseFormID, e.x, e.y, e.z, NowSeconds()))
            {
                s_diag.skipped++;   // it already went off here (replicated destruction / replayed shot)
                continue;
            }
            NiPoint3 pos{ e.x, e.y, e.z };
            TESObjectREFR* placed = CoSyncGameAPI::SpawnItemAtPosition(base, pos);
            if (!placed)
            {
                // Do NOT permanently blacklist on the first failure any more:
                // the earlier run blacklisted a perfectly good Molotov base
                // because the anchor bug made every attempt fail. Count strikes
                // instead, so a transient failure cannot kill a base forever.
                if (++s_placeFailures[e.baseFormID] >= kMaxPlaceFailures)
                {
                    s_faultedBases.insert(e.baseFormID);
                    LOG_WARN("[ExplSync] base=0x%08X failed to spawn %d times — "
                             "now skipped for this session",
                        e.baseFormID, kMaxPlaceFailures);
                }
                s_diag.placeFail++;
                LOG_WARN("[ExplSync] SpawnItemAtPosition failed for base=0x%08X", e.baseFormID);
                continue;
            }

            // Mark so the local scan never re-broadcasts what we just spawned
            // on a peer's behalf (this is the feedback-loop guard).
            s_knownExplosionRefs.insert(placed->formID);

            s_diag.applied++;
            LOG_INFO("[ExplSync] applied base=0x%08X at (%.0f,%.0f,%.0f)",
                e.baseFormID, e.x, e.y, e.z);
        }
    }
}

bool CoSyncExplosionSync::IsExplosion(const std::string& msg)
{
    return msg.rfind("EXP|", 0) == 0;
}

void CoSyncExplosionSync::Tick(double now)
{
    if (!CoSyncNet::IsConnected()) return;
    if ((now - s_lastPoll) < kExpl_PollInterval) return;
    s_lastPoll = now;

    __try
    {
        Tick_Impl(now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[ExplSync] Tick crashed, recovered (skipping this scan)");
    }

    LogDiag(now);
}

void CoSyncExplosionSync::OnReceiveExplosion(const std::string& msg)
{
    const char* s = msg.c_str() + 4;
    char* e = nullptr;

    uint32_t baseFormID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (!e || *e != '|' || baseFormID == 0) return;
    s = e + 1;

    PendingExplosion p{};
    p.baseFormID = baseFormID;
    p.x = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
    p.y = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
    p.z = std::strtof(s, &e);

    LOG_INFO("[ExplSync] RX base=0x%08X pos=(%.0f,%.0f,%.0f)",
        p.baseFormID, p.x, p.y, p.z);

    std::lock_guard<std::mutex> lk(s_mutex);
    s_pending.push_back(p);
}

// C2712: a function holding a std::vector (non-trivial unwind) cannot also own
// a __try, so the SEH scope lives in its own frame taking only a reference --
// the same _Impl/Safe split used throughout CoSyncWorldSync.cpp.
static void FlushSafe(const std::vector<PendingExplosion>& pending)
{
    __try
    {
        Flush_Impl(pending);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[ExplSync] FlushPendingExplosions crashed, recovered "
                 "(dropping this batch)");
    }
}

void CoSyncExplosionSync::FlushPendingExplosions()
{
    std::vector<PendingExplosion> pending;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_pending.empty()) return;
        pending.swap(s_pending);
    }

    FlushSafe(pending);
}
