#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncWorkshopAuthority.h"

#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncWorkshopEngine.h"
#include "CoSyncTeams.h"
#include "CoSyncWorldSync.h"
#include "CoSyncWorld.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncProfiles.h"
#include "CoSyncGameAPI.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"
#include "f4se/PluginAPI.h"
#include "f4se/GameWorkshop.h"
#include "f4se/GameExtraData.h"

#include "MinHook.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern PlayerCharacter* CoSyncGetPlayer();

// Wire format (text; floats %.4f; ids decimal):
//  client -> host (requests; the host trusts only the connection's identity)
//   WKQ|P|<clientRef>|<base>|x|y|z|rx|ry|rz|<ws>|<space>|<cell>
//   WKQ|M|<N|R>|<id>|<base>|x|y|z|rx|ry|rz|<ws>|<clientRef>
//   WKQ|S|<N|R>|<id>|<base>|x|y|z|<ws>|<clientRef>|<form:count,...>
//   WKQ|T|<N|R>|<id>|<base>|x|y|z|<ws>|<clientRef>
//   WKQ|R|<N|R>|<id>|<base>|x|y|z|<ws>|<clientRef>
//     N = host network id, R = form ID (an object from the host's save)
//  host -> clients (results)
//   WKA|<net>|<hostRef>|<base>|x|y|z|rx|ry|rz|<ws>|<space>|<cell>|<placerEid>|<placerRef>
//   WKV|<net>|<hostRef>|<base>|x|y|z|rx|ry|rz|<ws>
//   WKX|<net>|<hostRef>|<base>|x|y|z
//   WKR|<net>|<hostRef>
//   WKC|<ws>=<count>;...
//   WKN|<op>|<clientRef>|<reason>
//  0.3.6 wires (an endpoint id is N<net> or R<formID>, like the requests):
//   WKQ|W|<clientWire>|<splineBase>|<endA>|<endB>|<ws>
//   WKW|<net>|<hostRef>|<splineBase>|<endA>|<endB>|<ws>|<placerEid>|<placerRef>
namespace
{
    using namespace CoSyncWorkshopEngine;
    using CoSyncTeams::WorkshopOp;

    constexpr uint32_t kRecordType    = 'WSHP';
    constexpr uint32_t kRecordVersion = 1;
    constexpr float    kApplyRadius   = 12000.f;   // exterior: loaded-area distance for placing
    constexpr float    kMatchRadius   = 16.f;      // identity match tolerance (units)
    constexpr float    kMoveEpsilon   = 0.5f;
    constexpr float    kRotEpsilon    = 0.01f;
    constexpr size_t   kMaxRewards    = 16;
    constexpr uint32_t kMaxRewardEach = 200;

    // ── Hook plumbing ───────────────────────────────────────────────────────
    using Place_t     = bool  (*)(void* ctx);
    using Scrap_t     = void  (*)(void* ctx, void* refPtr, void* rewards);
    using Store_t     = void  (*)(void* ctx, void* refPtr, bool store, uint8_t a4, bool silent);
    using Repair_t    = void  (*)(void* self, char button);
    using Buildable_t = bool  (*)(TESObjectREFR* ws, NiPoint3* pos);
    using Budget_t    = float (*)();

    Place_t     g_origPlace     = nullptr;
    Scrap_t     g_origScrap     = nullptr;
    Store_t     g_origStore     = nullptr;
    Repair_t    g_origRepair    = nullptr;
    Buildable_t g_origBuildable = nullptr;
    Budget_t    g_origBudget    = nullptr;

    std::atomic<bool> s_hooksActive{ false };
    bool              s_hookAttempted = false;
    thread_local int  t_depth = 0;   // > 0: inside one of our own operations

    // Host rules, cached for the hot hooks (budget / buildable run per frame).
    std::atomic<bool>     s_freeBuild{ false };
    std::atomic<int>      s_limitMode{ 0 };
    std::atomic<uint32_t> s_customLimit{ 1000 };

    uintptr_t Base()
    {
        static const uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        return b;
    }

    // ── Captured operation ──────────────────────────────────────────────────
    struct Capture
    {
        TESObjectREFR* ref = nullptr;
        uint32_t formID = 0, base = 0;
        NiPoint3 pos{ 0.f, 0.f, 0.f }, rot{ 0.f, 0.f, 0.f };
        uint32_t ws = 0, linkedWs = 0, space = 0, cell = 0;
        bool     isMove = false;
        uint32_t nRewards = 0;
        uint32_t rewardForm[kMaxRewards] = {};
        uint32_t rewardCount[kMaxRewards] = {};
    };

    struct LocalEvent
    {
        char    op = 0;   // P M S T R
        Capture c;
    };

    // ── Replicated object ───────────────────────────────────────────────────
    struct Obj
    {
        uint32_t    net = 0, hostRef = 0, base = 0, ws = 0, space = 0, cell = 0;
        NiPoint3    pos{ 0.f, 0.f, 0.f }, rot{ 0.f, 0.f, 0.f };
        std::string placer;
        bool        built = false;   // placed through CoSync (counts toward the custom limit)
    };

    // Deferred application to THIS machine's world.
    struct Job
    {
        char     kind = 0;        // A add, V move, X remove, R repair
        Obj      o;
        bool     hostApply = false;   // host world: also fire the workshop events
        uint32_t nRewards = 0;
        uint32_t rewardForm[kMaxRewards] = {};
        uint32_t rewardCount[kMaxRewards] = {};
        double   first = 0.0;
        int      inRangeMisses = 0;
    };

    // s_mx guards the registry, maps and jobs, and IS held across engine
    // calls on the tick. The hooks run on the engine's own UI path, so they
    // must never wait on it: they only use s_evMx (event queue) and
    // s_countMx (build counts), which are never held across engine calls.
    std::recursive_mutex s_mx;
    std::mutex             s_evMx;
    std::mutex             s_countMx;
    std::deque<LocalEvent> s_events;       // from hooks, drained on Tick

    // Authority (host / solo)
    std::unordered_map<uint32_t, Obj>      s_reg;        // net -> object
    std::unordered_map<uint32_t, uint32_t> s_refToNet;   // host form -> net
    uint32_t s_nextNet = 1;
    bool     s_countsDirty = false;
    double   s_nextCounts = 0.0;

    // 0.3.6: wire endpoints per net id ("N<net>" / "R<formID>"), both roles.
    struct WireEnds { std::string a, b; };
    std::unordered_map<uint32_t, WireEnds> s_wireEnds;

    // Client
    std::unordered_map<uint32_t, uint32_t> s_netToLocal;
    std::unordered_map<uint32_t, uint32_t> s_localToNet;

    // Both
    std::unordered_map<uint32_t, uint32_t> s_counts;     // ws -> built count
    std::vector<Job> s_jobs;
    double s_nextJobs = 0.0;
    double s_nextDenyNote = 0.0;

    // Host: per-connection request budget.
    struct Bucket { double tokens = 40.0; double last = 0.0; };
    std::unordered_map<uint32_t, Bucket> s_buckets;

    double Now() { return static_cast<double>(GetTickCount64()) / 1000.0; }

    bool AuthorityNow()
    {
        return !CoSyncTransport::IsInitialized() || CoSyncTransport::IsHost();
    }

    bool SessionLive()
    {
        return CoSyncTransport::IsInitialized() && CoSyncNet::IsConnected();
    }

    bool Finite(const NiPoint3& p)
    {
        return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
               std::fabs(p.x) < 1.0e7f && std::fabs(p.y) < 1.0e7f && std::fabs(p.z) < 1.0e7f;
    }

    // ── SEH-guarded engine reads ────────────────────────────────────────────
    __declspec(noinline) bool ReadRefInfo(TESObjectREFR* ref, Capture& c)
    {
        __try
        {
            if (!ref || !ref->baseForm)
                return false;
            c.ref = ref;
            c.formID = ref->formID;
            c.base = ref->baseForm->formID;
            c.pos = ref->pos;
            c.rot = ref->rot;
            TESObjectCELL* cell = ref->parentCell;
            c.cell = cell ? cell->formID : 0u;
            c.space = 0;
            if (cell && !(cell->flags & TESObjectCELL::kFlag_IsInterior) && cell->worldSpace)
                c.space = reinterpret_cast<TESForm*>(cell->worldSpace)->formID;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    __declspec(noinline) uint32_t SafeFormID(TESObjectREFR* ref)
    {
        __try { return ref ? ref->formID : 0u; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
    }

    __declspec(noinline) TESObjectREFR* ReadNiPtr(void* p)
    {
        __try { return p ? *reinterpret_cast<TESObjectREFR**>(p) : nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    __declspec(noinline) uint32_t ReadU32(const void* base, size_t off)
    {
        __try { return base ? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(base) + off) : 0u; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
    }

    // BSTArray<BSTTuple<TESBoundObject*, u32>>: data at +0, count at +0x10,
    // 16-byte entries (ScrapReference's own loop).
    __declspec(noinline) void ReadRewards(void* arr, Capture& c)
    {
        c.nRewards = 0;
        __try
        {
            if (!arr) return;
            const uint8_t* data = *reinterpret_cast<uint8_t* const*>(arr);
            const uint32_t n = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(arr) + 0x10);
            if (!data) return;
            for (uint32_t i = 0; i < n && c.nRewards < kMaxRewards; ++i)
            {
                TESForm* f = *reinterpret_cast<TESForm* const*>(data + 16ull * i);
                const uint32_t cnt = *reinterpret_cast<const uint32_t*>(data + 16ull * i + 8);
                if (!f || cnt == 0) continue;
                c.rewardForm[c.nRewards] = f->formID;
                c.rewardCount[c.nRewards] = cnt;
                ++c.nRewards;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            c.nRewards = 0;
        }
    }

    // Wires are BendableSpline refs whose shape lives in extra data set up by
    // the workshop wiring code between two endpoints. A plain spawn of the
    // base elsewhere would be a broken object. 0.3.6: they are replicated as
    // "a wire of base S between endpoint A and endpoint B" and rebuilt on
    // each machine by the engine's own wiring calls (see CreateWireImpl).
    __declspec(noinline) bool IsSplineBase(uint32_t baseId)
    {
        __try
        {
            TESForm* f = LookupFormByID(baseId);
            return f && f->formType == kFormType_BNDS;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // ── 0.3.6: wires ────────────────────────────────────────────────────────
    // A wire's ExtraPowerLinks (type 0xD0) lists the two endpoint form IDs;
    // each endpoint's lists the wire (F4SE 0.7.9 GameExtraData.h).
    __declspec(noinline) bool ReadWireEnds(TESObjectREFR* wire, uint32_t& a, uint32_t& b)
    {
        a = b = 0;
        __try
        {
            ExtraDataList* x = wire ? wire->extraDataList : nullptr;
            auto* pl = x ? static_cast<ExtraPowerLinks*>(x->GetByType(kExtraData_PowerLinks)) : nullptr;
            if (!pl || pl->connections.count < 2)
                return false;
            UInt64 v0 = 0, v1 = 0;
            pl->connections.GetNthItem(0, v0);
            pl->connections.GetNthItem(1, v1);
            a = static_cast<uint32_t>(v0);
            b = static_cast<uint32_t>(v1);
            return a != 0 && b != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    __declspec(noinline) bool Has3D(TESObjectREFR* r)
    {
        __try { return r && r->GetObjectRootNode() != nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // The engine wiring sequence, as F4SE 0.7.9's ObjectReference.AttachWire
    // (PapyrusObjectReference.cpp AttachWireLatent) runs it on 1.11.240; every
    // callee verified as a function start in IDA (2026-09-29):
    //   Workshop::ContextData ctor sub_1403897F0, SplineUtils::UpdateSpline
    //   sub_14038DB50, SplineUtils::ConnectSpline sub_14038DFE0 (writes the
    //   ExtraPowerLinks on both endpoints and the wire), Workshop::ExtraData
    //   AddItem sub_140387090 / AddConnection sub_1403876D0 (power grid),
    //   PowerUtils::UpdateMovingWirelessItem sub_14038ECB0,
    //   TerminalUtils::EstablishTerminalLinks sub_14038FB40,
    //   hCurrentWorkshop 0x1430F7698.
    // The wire ref itself comes from CreateReferenceAtLocation (SpawnItemAtPosition)
    // instead of PlaceAtMe_Native, which needs a VM stack.
    __declspec(noinline) void WireEngineCalls(TESObjectREFR* wire, TESObjectREFR* a, TESObjectREFR* b,
                                              TESObjectREFR* workshop, Workshop::ExtraData* wx)
    {
        BSPointerHandle<TESObjectREFR> saved = *Workshop::hCurrentWorkshop;
        *Workshop::hCurrentWorkshop = workshop;
        Workshop::ContextData ctx(reinterpret_cast<Actor*>(CoSyncGetPlayer()));
        SplineUtils::UpdateSpline(&ctx, wire, b, 0, a, 0);
        SplineUtils::ConnectSpline(a, 0, b, 0, wire);
        if (wire->extraDataList)
            if (auto* sp = static_cast<ExtraBendableSplineParams*>(wire->extraDataList->GetByType(kExtraData_BendableSplineParams)))
                sp->thickness = 1.5f;
        wx->AddItem(wire);
        wx->AddConnection(a, b, wire);
        PowerUtils::UpdateMovingWirelessItem(a, wx);
        PowerUtils::UpdateMovingWirelessItem(b, wx);
        TerminalUtils::EstablishTerminalLinks(wire);
        *Workshop::hCurrentWorkshop = saved;
    }

    __declspec(noinline) bool WireEngineCallsSafe(TESObjectREFR* wire, TESObjectREFR* a, TESObjectREFR* b,
                                                  TESObjectREFR* workshop, Workshop::ExtraData* wx)
    {
        __try { WireEngineCalls(wire, a, b, workshop, wx); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    __declspec(noinline) Workshop::ExtraData* WorkshopExtra(TESObjectREFR* workshop)
    {
        __try
        {
            return workshop && workshop->extraDataList
                ? static_cast<Workshop::ExtraData*>(workshop->extraDataList->GetByType(kExtraData_WorkshopExtraData))
                : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    // Actor buildables (turrets, robots): permission-gated like everything
    // else, but not replicated as workshop objects -- a plain object spawn of
    // an NPC base would be a broken actor, and live actors are replicated by
    // CoSyncNpcWorldSync. Only the host may build them in a session, so the
    // host's saved world stays the authority for them.
    __declspec(noinline) bool IsActorBase(uint32_t baseId)
    {
        __try
        {
            TESForm* f = LookupFormByID(baseId);
            return f && f->formType == kFormType_NPC_;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    TESObjectREFR* RefByID(uint32_t id)
    {
        if (id == 0) return nullptr;
        TESForm* f = LookupFormByID(id);
        if (!f || f->formType < kFormType_REFR || f->formType > kFormType_PHZD)
            return nullptr;
        return static_cast<TESObjectREFR*>(f);
    }

    __declspec(noinline) TESObjectREFR* LiveRef(uint32_t id)
    {
        __try
        {
            TESObjectREFR* r = RefByID(id);
            if (!r || !r->baseForm) return nullptr;
            if (r->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)) return nullptr;
            return r;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    __declspec(noinline) TESObjectREFR* WorkshopByID(uint32_t id)
    {
        __try
        {
            TESObjectREFR* r = RefByID(id);
            return (r && IsWorkshopRef(r)) ? r : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    // The engine's REAL buildable-area test, even while "remove world build
    // restrictions" overrides it for the workshop mode.
    __declspec(noinline) bool OrigContains(uint32_t wsId, const NiPoint3& pos)
    {
        __try
        {
            TESObjectREFR* ws = WorkshopByID(wsId);
            if (!ws) return false;
            NiPoint3 p = pos;
            Buildable_t fn = g_origBuildable ? g_origBuildable
                           : reinterpret_cast<Buildable_t>(Base() + kRVA_IsLocationWithinBuildableArea);
            return fn(ws, &p);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void RefreshRules()
    {
        const CoSyncTeams::Rules s = CoSyncTeams::GetRules();
        s_freeBuild.store(s.freeBuild);
        s_limitMode.store(static_cast<int>(s.limitMode));
        s_customLimit.store(s.customLimit);
    }

    uint32_t CountFor(uint32_t ws)
    {
        std::lock_guard<std::mutex> lk(s_countMx);
        auto it = s_counts.find(ws);
        return it == s_counts.end() ? 0u : it->second;
    }

    void DenyNote(const std::string& why)
    {
        const double t = Now();
        if (t < s_nextDenyNote) return;
        s_nextDenyNote = t + 1.0;
        CoSyncGameAPI::QueueNotification(why.c_str());
    }

    // Every settlement whose rules apply to an operation: the one it is done
    // in, the one the object belongs to, and every ASSIGNED (team / personal)
    // settlement whose real build area contains the position -- so removing
    // world build restrictions never lets anyone build inside someone else's
    // settlement from outside it.
    std::vector<uint32_t> SettlementsFor(uint32_t ws, uint32_t extraWs, const NiPoint3& pos)
    {
        std::vector<uint32_t> list;
        if (ws) list.push_back(ws);
        if (extraWs && extraWs != ws) list.push_back(extraWs);
        for (uint32_t t : CoSyncTeams::GetAssignedSettlements())
            if (std::find(list.begin(), list.end(), t) == list.end() && OrigContains(t, pos))
                list.push_back(t);
        return list;
    }

    bool CheckAll(const std::string& key, uint32_t ws, uint32_t extraWs, const NiPoint3& pos, WorkshopOp op, std::string& why)
    {
        for (uint32_t t : SettlementsFor(ws, extraWs, pos))
            if (!CoSyncTeams::CheckWorkshop(key, t, op, &why))
                return false;
        return true;
    }

    // Local gate (hook thread). No SEH here: it builds strings.
    bool GateLocal(const Capture& c, WorkshopOp op)
    {
        std::string why;
        if (!CheckAll(CoSyncProfiles::GetMyProfileKey(), c.ws, c.linkedWs, c.pos, op, why))
        {
            LOG_INFO("[Workshop] local %s refused: %s", op == WorkshopOp::Build ? "build" : op == WorkshopOp::Move ? "move" :
                op == WorkshopOp::Scrap ? "scrap" : "repair", why.c_str());
            DenyNote(why);
            return false;
        }
        // Objects other machines cannot spawn would exist for one player
        // only: refuse them in a session, for the host too.
        if (op == WorkshopOp::Build && SessionLive() && CoSyncWorldSync::IsKnownUnsafePlaceAtMeForm(c.base))
        {
            char b[160];
            std::snprintf(b, sizeof(b), "That object (0x%08X) is on CoSync's crash blocklist for remote spawning and cannot be built in multiplayer yet.", c.base);
            DenyNote(b);
            return false;
        }
        if (op == WorkshopOp::Build && !AuthorityNow() && IsActorBase(c.base))
        {
            DenyNote("Turrets, robots and other actors can only be built by the host in multiplayer.");
            return false;
        }
        if (op == WorkshopOp::Build && s_limitMode.load() == static_cast<int>(CoSyncTeams::BuildLimitMode::Custom))
        {
            const uint32_t n = CountFor(c.ws), lim = s_customLimit.load();
            if (n >= lim)
            {
                char b[128];
                std::snprintf(b, sizeof(b), "Build limit reached for this settlement (%u / %u).", n, lim);
                DenyNote(b);
                return false;
            }
        }
        return true;
    }

    void QueueEvent(char op, const Capture& c)
    {
        std::lock_guard<std::mutex> lk(s_evMx);
        if (s_events.size() < 512)
            s_events.push_back({ op, c });
    }

    // ── Detours ─────────────────────────────────────────────────────────────
    bool DetourPlace(void* ctx)
    {
        if (t_depth > 0)
            return g_origPlace(ctx);

        Capture c;
        TESObjectREFR* item = GetPlacementItem();
        TESObjectREFR* ws = GetCurrentWorkshop();
        const bool have = item && ws && ReadRefInfo(item, c);
        if (have)
        {
            c.ws = SafeFormID(ws);
            // Same test PlaceCurrentReference uses (0x3823D0): an object whose
            // WorkshopItemKeyword link points at a workbench already belongs to
            // a settlement (a move); a fresh placement preview does not.
            TESObjectREFR* lw = GetLinkedWorkshop(item);
            c.isMove = lw != nullptr && IsWorkshopRef(lw);
            c.linkedWs = c.isMove ? SafeFormID(lw) : 0u;
            if (!GateLocal(c, c.isMove ? WorkshopOp::Move : WorkshopOp::Build))
                return false;   // the engine's own "cannot place": the item stays in hand
        }

        ++t_depth;
        const bool ok = g_origPlace(ctx);
        --t_depth;

        if (ok && have && ReadRefInfo(c.ref, c))
            QueueEvent(c.isMove ? 'M' : 'P', c);
        return ok;
    }

    // Shared by Scrap and Store.
    bool CaptureRemoval(void* refPtr, Capture& c)
    {
        TESObjectREFR* item = ReadNiPtr(refPtr);
        if (!item || !ReadRefInfo(item, c))
            return false;
        TESObjectREFR* lw = GetLinkedWorkshop(item);
        TESObjectREFR* cur = GetCurrentWorkshop();
        c.linkedWs = SafeFormID(lw);
        c.ws = lw ? c.linkedWs : SafeFormID(cur);
        return c.ws != 0;
    }

    void DetourScrap(void* ctx, void* refPtr, void* rewards)
    {
        if (t_depth > 0)
        {
            g_origScrap(ctx, refPtr, rewards);
            return;
        }
        Capture c;
        const bool have = CaptureRemoval(refPtr, c);
        if (have)
        {
            if (!GateLocal(c, WorkshopOp::Scrap))
                return;   // not scrapped; the object stays
            ReadRewards(rewards, c);
        }
        ++t_depth;
        g_origScrap(ctx, refPtr, rewards);
        --t_depth;
        if (have)
            QueueEvent('S', c);
    }

    void DetourStore(void* ctx, void* refPtr, bool store, uint8_t a4, bool silent)
    {
        if (t_depth > 0 || !store)
        {
            g_origStore(ctx, refPtr, store, a4, silent);
            return;
        }
        Capture c;
        const bool have = CaptureRemoval(refPtr, c);
        if (have && !GateLocal(c, WorkshopOp::Scrap))
            return;
        ++t_depth;
        g_origStore(ctx, refPtr, store, a4, silent);
        --t_depth;
        if (have)
            QueueEvent('T', c);
    }

    void DetourRepair(void* self, char button)
    {
        if (t_depth > 0 || button != 0)
        {
            g_origRepair(self, button);
            return;
        }
        Capture c;
        TESObjectREFR* item = ResolveHandle(ReadU32(self, 0x10));   // RequestRepairCallback+0x10: ref handle
        bool have = item && ReadRefInfo(item, c);
        if (have)
        {
            TESObjectREFR* lw = GetLinkedWorkshop(item);
            c.linkedWs = SafeFormID(lw);
            c.ws = lw ? c.linkedWs : SafeFormID(GetCurrentWorkshop());
            have = c.ws != 0;
            if (have && !GateLocal(c, WorkshopOp::Repair))
                return;
        }
        ++t_depth;
        g_origRepair(self, button);
        --t_depth;
        if (have)
            QueueEvent('R', c);
    }

    bool DetourBuildable(TESObjectREFR* ws, NiPoint3* pos)
    {
        if (s_freeBuild.load())
            return true;
        return g_origBuildable(ws, pos);
    }

    float DetourBudget()
    {
        // Custom and Unlimited replace the vanilla triangle/draw budget; the
        // custom object count is enforced by GateLocal and the host.
        if (s_limitMode.load() != static_cast<int>(CoSyncTeams::BuildLimitMode::Default))
            return 0.0f;
        return g_origBudget();
    }

    struct HookSpec
    {
        const char* name;
        uintptr_t   rva;
        void*       detour;
        void**      orig;
        uint8_t     prologue[8];   // 1.11.240 bytes (IDB + live exe, 2026-09-28)
    };

    bool InstallHooks()
    {
        static HookSpec specs[] = {
            { "PlaceCurrentReference", kRVA_PlaceCurrentReference, reinterpret_cast<void*>(&DetourPlace), reinterpret_cast<void**>(&g_origPlace),
              { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x55 } },
            { "ScrapReference", kRVA_ScrapReference, reinterpret_cast<void*>(&DetourScrap), reinterpret_cast<void**>(&g_origScrap),
              { 0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x40, 0x18, 0x48 } },
            { "StoreReference", kRVA_StoreReference, reinterpret_cast<void*>(&DetourStore), reinterpret_cast<void**>(&g_origStore),
              { 0x44, 0x88, 0x4C, 0x24, 0x20, 0x48, 0x89, 0x4C } },
            { "RepairCallback::Run", kRVA_RepairCallbackRun, reinterpret_cast<void*>(&DetourRepair), reinterpret_cast<void**>(&g_origRepair),
              { 0x84, 0xD2, 0x0F, 0x85, 0x68, 0x03, 0x00, 0x00 } },
            { "IsLocationWithinBuildableArea", kRVA_IsLocationWithinBuildableArea, reinterpret_cast<void*>(&DetourBuildable), reinterpret_cast<void**>(&g_origBuildable),
              { 0x40, 0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 } },
            { "BudgetRatio", kRVA_BudgetRatio, reinterpret_cast<void*>(&DetourBudget), reinterpret_cast<void**>(&g_origBudget),
              { 0x8B, 0x05, 0x5A, 0x98, 0xD7, 0x02, 0x0F, 0x57 } },
        };

        // All or nothing: a half-hooked workshop would gate some operations
        // and silently let others through.
        for (const HookSpec& h : specs)
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(Base() + h.rva);
            if (std::memcmp(p, h.prologue, sizeof(h.prologue)) != 0)
            {
                LOG_ERROR("[Workshop] %s prologue mismatch at +0x%zX (%02X %02X %02X %02X) -- not Fallout4.exe 1.11.240? Workshop authority DISABLED",
                    h.name, h.rva, p[0], p[1], p[2], p[3]);
                return false;
            }
        }

        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        {
            LOG_ERROR("[Workshop] MinHook init failed (%d)", static_cast<int>(st));
            return false;
        }

        size_t created = 0;
        for (const HookSpec& h : specs)
        {
            void* target = reinterpret_cast<void*>(Base() + h.rva);
            st = MH_CreateHook(target, h.detour, h.orig);
            if (st != MH_OK)
            {
                LOG_ERROR("[Workshop] MH_CreateHook(%s) failed (%d)", h.name, static_cast<int>(st));
                break;
            }
            ++created;
        }
        if (created == sizeof(specs) / sizeof(specs[0]))
        {
            size_t enabled = 0;
            for (const HookSpec& h : specs)
                if (MH_EnableHook(reinterpret_cast<void*>(Base() + h.rva)) == MH_OK)
                    ++enabled;
            if (enabled == created)
            {
                LOG_INFO("[Workshop] engine hooks installed: build/move/wire, scrap, store, repair, buildable area, budget");
                return true;
            }
            LOG_ERROR("[Workshop] only %zu of %zu workshop hooks enabled -- removing all", enabled, created);
        }
        for (size_t i = 0; i < created; ++i)
        {
            void* target = reinterpret_cast<void*>(Base() + specs[i].rva);
            MH_DisableHook(target);
            MH_RemoveHook(target);
        }
        return false;
    }

    // ── Encoding ────────────────────────────────────────────────────────────
    std::string Pct(const std::string& s)
    {
        static const char* hex = "0123456789ABCDEF";
        std::string o;
        for (unsigned char c : s)
        {
            if (c == '%' || c == '|' || c == '\n' || c == '\r' || c < 0x20)
            {
                o += '%'; o += hex[c >> 4]; o += hex[c & 15];
            }
            else
                o += static_cast<char>(c);
        }
        return o;
    }

    std::string Unpct(const std::string& s)
    {
        std::string o;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '%' && i + 2 < s.size())
            {
                o += static_cast<char>(std::strtoul(s.substr(i + 1, 2).c_str(), nullptr, 16));
                i += 2;
            }
            else
                o += s[i];
        }
        return o;
    }

    std::vector<std::string> Split(const std::string& s, char sep)
    {
        std::vector<std::string> out;
        size_t a = 0;
        while (true)
        {
            const size_t b = s.find(sep, a);
            out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        return out;
    }

    uint32_t U(const std::string& s) { return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10)); }
    float    F(const std::string& s) { return std::strtof(s.c_str(), nullptr); }

    std::string Fmt(const char* fmt, ...)
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        return buf;
    }

    // ── Broadcasts (host) ───────────────────────────────────────────────────
    void Send(const std::string& msg, uint32_t onlyConn)
    {
        if (!SessionLive()) return;
        if (onlyConn) CoSyncTransport::SendTo(onlyConn, msg);
        else          CoSyncTransport::Send(msg);
    }

    void BroadcastAdd(const Obj& o, uint32_t placerEid, uint32_t placerRef, uint32_t onlyConn = 0)
    {
        Send(Fmt("WKA|%u|%u|%u|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%u|%u|%u|%u|%u", o.net, o.hostRef, o.base,
            o.pos.x, o.pos.y, o.pos.z, o.rot.x, o.rot.y, o.rot.z, o.ws, o.space, o.cell, placerEid, placerRef), onlyConn);
    }

    void BroadcastMove(const Obj& o, uint32_t onlyConn = 0)
    {
        Send(Fmt("WKV|%u|%u|%u|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%u", o.net, o.hostRef, o.base,
            o.pos.x, o.pos.y, o.pos.z, o.rot.x, o.rot.y, o.rot.z, o.ws), onlyConn);
    }

    void BroadcastRemove(const Obj& o)
    {
        Send(Fmt("WKX|%u|%u|%u|%.4f|%.4f|%.4f", o.net, o.hostRef, o.base, o.pos.x, o.pos.y, o.pos.z), 0);
    }

    void BroadcastRepair(const Obj& o)
    {
        Send(Fmt("WKR|%u|%u", o.net, o.hostRef), 0);
    }

    std::string CountsBody()
    {
        std::lock_guard<std::mutex> lk(s_countMx);
        std::string b;
        for (const auto& kv : s_counts)
        {
            if (!b.empty()) b += ";";
            b += std::to_string(kv.first) + "=" + std::to_string(kv.second);
        }
        return b;
    }

    void RecountAuthority()
    {
        std::unordered_map<uint32_t, uint32_t> counts;
        for (const auto& kv : s_reg)
            if (kv.second.ws && kv.second.built) ++counts[kv.second.ws];
        {
            std::lock_guard<std::mutex> lk(s_countMx);
            s_counts.swap(counts);
        }
        s_countsDirty = true;
    }

    Obj ObjFromCapture(const Capture& c)
    {
        Obj o;
        o.hostRef = c.formID;
        o.base = c.base;
        o.pos = c.pos;
        o.rot = c.rot;
        o.ws = c.ws;
        o.space = c.space;
        o.cell = c.cell;
        return o;
    }

    Obj* RegByHostRef(uint32_t hostRef)
    {
        auto it = s_refToNet.find(hostRef);
        if (it == s_refToNet.end()) return nullptr;
        auto jt = s_reg.find(it->second);
        return jt == s_reg.end() ? nullptr : &jt->second;
    }

    Obj& RegAdd(Obj o)
    {
        o.net = s_nextNet++;
        if (o.hostRef) s_refToNet[o.hostRef] = o.net;
        Obj& r = s_reg[o.net];
        r = std::move(o);
        return r;
    }

    void RegErase(uint32_t net)
    {
        auto it = s_reg.find(net);
        if (it == s_reg.end()) return;
        if (it->second.hostRef) s_refToNet.erase(it->second.hostRef);
        s_reg.erase(it);
    }

    void CancelJobs(uint32_t net, uint32_t hostRef)
    {
        s_jobs.erase(std::remove_if(s_jobs.begin(), s_jobs.end(), [&](const Job& j) {
            return (net && j.o.net == net) || (hostRef && j.o.hostRef == hostRef && j.o.net == 0);
        }), s_jobs.end());
    }

    void AddJob(char kind, const Obj& o, bool hostApply, const Capture* rewards = nullptr)
    {
        Job j;
        j.kind = kind;
        j.o = o;
        j.hostApply = hostApply;
        j.first = Now();
        if (rewards)
        {
            j.nRewards = rewards->nRewards;
            std::memcpy(j.rewardForm, rewards->rewardForm, sizeof(j.rewardForm));
            std::memcpy(j.rewardCount, rewards->rewardCount, sizeof(j.rewardCount));
        }
        // A newer move replaces a pending move / add transform for the same object.
        if (kind == 'V')
            for (Job& e : s_jobs)
                if (e.o.net == o.net && o.net != 0 && (e.kind == 'A' || e.kind == 'V'))
                {
                    e.o.pos = o.pos; e.o.rot = o.rot;
                    return;
                }
        if (kind == 'X' || kind == 'R')
        {
            // Removing an object that was never placed here: just forget it.
            if (kind == 'X')
                for (auto it = s_jobs.begin(); it != s_jobs.end(); )
                {
                    if (o.net && it->o.net == o.net && it->kind == 'A')
                    {
                        it = s_jobs.erase(it);
                        continue;
                    }
                    ++it;
                }
        }
        if (s_jobs.size() < 8192)
            s_jobs.push_back(std::move(j));
    }

    // ── Applying to this machine's world (tick; SEH-wrapped by the caller) ──
    bool InRangeImpl(const Obj& o, TESObjectCELL*& myCell, TESWorldSpace*& myWS)
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc) return false;
        TESObjectREFR* pr = reinterpret_cast<TESObjectREFR*>(pc);
        myCell = pr->parentCell;
        myWS = CALL_MEMBER_FN(pr, GetWorldspace)();
        if (!myCell) return false;
        const uint32_t myWSID = myWS ? reinterpret_cast<TESForm*>(myWS)->formID : 0u;
        if (o.space != 0)
        {
            if (myWSID != o.space) return false;
            const float dx = pr->pos.x - o.pos.x, dy = pr->pos.y - o.pos.y;
            return dx * dx + dy * dy <= kApplyRadius * kApplyRadius;
        }
        return o.cell != 0 && myCell->formID == o.cell;
    }

    void DeleteRef(TESObjectREFR* ref)
    {
        static const BSFixedString kDelete("Delete");
        CoSyncGameAPI::DisableWorldActor(ref);
        VMArray<VMVariable> args;
        CallFunctionNoWait(ref, kDelete, args);
    }

    TESObjectREFR* SpawnImpl(const Obj& o, TESWorldSpace* myWS)
    {
        if ((o.base & 0xFF000000u) == 0xFF000000u || CoSyncWorldSync::IsKnownUnsafePlaceAtMeForm(o.base))
            return nullptr;
        TESForm* base = LookupFormByID(o.base);
        if (!base) return nullptr;
        TESObjectREFR* placed = CoSyncGameAPI::SpawnItemAtPosition(base, o.pos, o.rot);
        if (!placed) return nullptr;
        NiPoint3 pos = o.pos, rot = o.rot;
        UInt32 nh = 0;
        if (o.space != 0)   // re-home into the cell that owns this position
            MoveRefrToPosition(placed, &nh, nullptr, myWS, &pos, &rot);
        if (TESObjectREFR* ws = WorkshopByID(o.ws))
            SetLinkedWorkshop(placed, ws);
        return placed;
    }

    enum class Apply { Done, Wait, Drop };

    // Local form for a replicated object (client), or the host's own ref.
    TESObjectREFR* ResolveLocal(const Obj& o, bool bindIdentity)
    {
        if (AuthorityNow())
            return o.hostRef ? LiveRef(o.hostRef) : nullptr;
        if (o.net)
        {
            auto it = s_netToLocal.find(o.net);
            if (it != s_netToLocal.end())
                if (TESObjectREFR* r = LiveRef(it->second))
                    return r;
        }
        // An object from the host's save: same form ID here (we loaded it).
        if (o.hostRef)
        {
            TESObjectREFR* r = LiveRef(o.hostRef);
            if (r && r->baseForm && r->baseForm->formID == o.base)
            {
                const float dx = r->pos.x - o.pos.x, dy = r->pos.y - o.pos.y, dz = r->pos.z - o.pos.z;
                // A move names the object's NEW position; only require
                // position agreement when adding.
                (void)dz;
                if (!bindIdentity || dx * dx + dy * dy <= kMatchRadius * kMatchRadius)
                {
                    if (o.net && s_localToNet.find(r->formID) == s_localToNet.end())
                    {
                        s_netToLocal[o.net] = r->formID;
                        s_localToNet[r->formID] = o.net;
                    }
                    return r;
                }
            }
        }
        return nullptr;
    }

    void BindLocal(uint32_t net, uint32_t local)
    {
        if (!net || !local) return;
        auto old = s_netToLocal.find(net);
        if (old != s_netToLocal.end()) s_localToNet.erase(old->second);
        s_netToLocal[net] = local;
        s_localToNet[local] = net;
    }

    void UnbindLocal(uint32_t local)
    {
        auto it = s_localToNet.find(local);
        if (it == s_localToNet.end()) return;
        s_netToLocal.erase(it->second);
        s_localToNet.erase(it);
    }

    // Endpoint identity valid on every machine.
    std::string EndIdent(uint32_t localForm)
    {
        if (AuthorityNow())
        {
            auto it = s_refToNet.find(localForm);
            return it != s_refToNet.end() ? Fmt("N%u", it->second) : Fmt("R%u", localForm);
        }
        auto it = s_localToNet.find(localForm);
        return it != s_localToNet.end() ? Fmt("N%u", it->second) : Fmt("R%u", localForm);
    }

    TESObjectREFR* ResolveEnd(const std::string& id)
    {
        if (id.size() < 2) return nullptr;
        const uint32_t n = static_cast<uint32_t>(std::strtoul(id.c_str() + 1, nullptr, 10));
        if (id[0] == 'R') return LiveRef(n);
        if (id[0] != 'N') return nullptr;
        if (AuthorityNow())
        {
            auto it = s_reg.find(n);
            return (it != s_reg.end() && it->second.hostRef) ? LiveRef(it->second.hostRef) : nullptr;
        }
        auto it = s_netToLocal.find(n);
        return it != s_netToLocal.end() ? LiveRef(it->second) : nullptr;
    }

    // 'W' job: build wire j.o.net between its endpoints on THIS machine.
    Apply ApplyWire(Job& j)
    {
        if (ResolveLocal(j.o, false))
            return Apply::Done;   // already here
        auto we = s_wireEnds.find(j.o.net);
        if (we == s_wireEnds.end())
            return Apply::Drop;
        TESObjectREFR* a = ResolveEnd(we->second.a);
        TESObjectREFR* b = ResolveEnd(we->second.b);
        TESObjectREFR* ws = WorkshopByID(j.o.ws);
        if (!a || !b || !ws)
            return (++j.inRangeMisses > 600) ? Apply::Drop : Apply::Wait;   // endpoints not built / bound yet
        if (!Has3D(a) || !Has3D(b))
            return Apply::Wait;                                          // not loaded here: build when in range
        Workshop::ExtraData* wx = WorkshopExtra(ws);
        TESForm* spline = LookupFormByID(j.o.base);
        if (!wx || !spline || spline->formType != kFormType_BNDS)
        {
            LOG_WARN("[Workshop] Wire %u: workshop extra data or spline base 0x%08X missing -- dropped", j.o.net, j.o.base);
            return Apply::Drop;
        }
        TESObjectREFR* wire = CoSyncGameAPI::SpawnItemAtPosition(spline, a->pos, NiPoint3());
        if (!wire)
            return Apply::Wait;
        SetLinkedWorkshop(wire, ws);
        ++t_depth;   // our own wiring: never re-captured
        const bool ok = WireEngineCallsSafe(wire, a, b, ws, wx);
        --t_depth;
        if (!ok)
        {
            LOG_ERROR("[Workshop] Wire %u: engine wiring faulted -- wire removed", j.o.net);
            DeleteRef(wire);
            return Apply::Drop;
        }
        if (AuthorityNow())
        {
            if (Obj* r = (s_reg.count(j.o.net) ? &s_reg[j.o.net] : nullptr))
            {
                if (r->hostRef) s_refToNet.erase(r->hostRef);
                r->hostRef = wire->formID;
                s_refToNet[r->hostRef] = r->net;
            }
        }
        else
            BindLocal(j.o.net, wire->formID);
        uint32_t ea = 0, eb = 0;
        ReadWireEnds(wire, ea, eb);
        LOG_INFO("[Workshop] Wire Synchronized: wire %u = 0x%08X, Wire Endpoint A 0x%08X, Wire Endpoint B 0x%08X "
                 "(power links on the wire: 0x%08X / 0x%08X)", j.o.net, wire->formID, a->formID, b->formID, ea, eb);
        return Apply::Done;
    }

    void BroadcastWire(const Obj& o, uint32_t placerEid, uint32_t placerRef)
    {
        auto we = s_wireEnds.find(o.net);
        if (we == s_wireEnds.end()) return;
        Send(Fmt("WKW|%u|%u|%u|%s|%s|%u|%u|%u", o.net, o.hostRef, o.base,
            we->second.a.c_str(), we->second.b.c_str(), o.ws, placerEid, placerRef), 0);
    }

    __declspec(noinline) Apply ApplyJobImpl(Job& j)
    {
        TESObjectCELL* myCell = nullptr;
        TESWorldSpace* myWS = nullptr;
        const bool inRange = InRangeImpl(j.o, myCell, myWS);
        TESObjectREFR* ws = WorkshopByID(j.o.ws);
        const bool authority = AuthorityNow();

        switch (j.kind)
        {
        case 'W':
            return ApplyWire(j);
        case 'A':
        {
            if (TESObjectREFR* have = ResolveLocal(j.o, true))
            {
                // Already here (host save / our own copy): just keep it linked.
                if (ws && !GetLinkedWorkshop(have)) SetLinkedWorkshop(have, ws);
                if (!authority) BindLocal(j.o.net, have->formID);
                return Apply::Done;
            }
            if (!inRange) return Apply::Wait;
            TESObjectREFR* placed = SpawnImpl(j.o, myWS);
            if (!placed)
            {
                LOG_WARN("[Workshop] could not place base 0x%08X for object %u -- dropped", j.o.base, j.o.net);
                return Apply::Drop;
            }
            if (authority)
            {
                // The host's world is the saved one: register the new form
                // and let the settlement scripts count it.
                if (Obj* r = (j.o.net ? (s_reg.count(j.o.net) ? &s_reg[j.o.net] : nullptr) : nullptr))
                {
                    if (r->hostRef) s_refToNet.erase(r->hostRef);
                    r->hostRef = placed->formID;
                    s_refToNet[r->hostRef] = r->net;
                }
                if (j.hostApply && ws) DispatchItemPlaced(ws, placed);
            }
            else
                BindLocal(j.o.net, placed->formID);
            LOG_INFO("[Workshop] placed object %u base=0x%08X as 0x%08X at (%.0f,%.0f,%.0f)",
                j.o.net, j.o.base, placed->formID, j.o.pos.x, j.o.pos.y, j.o.pos.z);
            return Apply::Done;
        }
        case 'V':
        {
            TESObjectREFR* ref = ResolveLocal(j.o, false);
            if (!ref) return inRange ? (++j.inRangeMisses > 20 ? Apply::Drop : Apply::Wait) : Apply::Wait;
            const float dx = ref->pos.x - j.o.pos.x, dy = ref->pos.y - j.o.pos.y, dz = ref->pos.z - j.o.pos.z;
            const bool moved = dx * dx + dy * dy + dz * dz > kMoveEpsilon * kMoveEpsilon;
            const bool turned = std::fabs(ref->rot.x - j.o.rot.x) > kRotEpsilon ||
                                std::fabs(ref->rot.y - j.o.rot.y) > kRotEpsilon ||
                                std::fabs(ref->rot.z - j.o.rot.z) > kRotEpsilon;
            if (!moved && !turned) return Apply::Done;
            if (!inRange) return Apply::Wait;
            if (turned)
            {
                // A static ref's rotation only lands at construction on 1.11.240
                // (see CoSyncGameAPI::SpawnItemAtPosition) -> replace it.
                TESObjectREFR* fresh = SpawnImpl(j.o, myWS);
                if (fresh)
                {
                    const uint32_t oldId = ref->formID;
                    if (authority && j.hostApply && ws) DispatchItemDestroyed(ws, ref);
                    SetLinkedWorkshop(ref, nullptr);
                    DeleteRef(ref);
                    if (authority)
                    {
                        if (Obj* r = RegByHostRef(oldId))
                        {
                            s_refToNet.erase(oldId);
                            r->hostRef = fresh->formID;
                            s_refToNet[r->hostRef] = r->net;
                        }
                        if (j.hostApply && ws) DispatchItemPlaced(ws, fresh);
                    }
                    else
                    {
                        UnbindLocal(oldId);
                        BindLocal(j.o.net, fresh->formID);
                    }
                    return Apply::Done;
                }
            }
            NiPoint3 pos = j.o.pos, rot = j.o.rot;
            UInt32 nh = 0;
            const bool exterior = j.o.space != 0;
            MoveRefrToPosition(ref, &nh, exterior ? nullptr : myCell, exterior ? myWS : nullptr, &pos, &rot);
            if (authority && j.hostApply && ws) DispatchItemMoved(ws, ref);
            LOG_INFO("[Workshop] moved object %u (0x%08X) to (%.0f,%.0f,%.0f)", j.o.net, ref->formID, pos.x, pos.y, pos.z);
            return Apply::Done;
        }
        case 'X':
        {
            TESObjectREFR* ref = ResolveLocal(j.o, false);
            if (!ref)
            {
                // Already gone here (usually: we removed it ourselves).
                return inRange ? (++j.inRangeMisses > 10 ? Apply::Done : Apply::Wait) : Apply::Wait;
            }
            uint32_t rewarded = 0;
            if (authority && j.hostApply)
            {
                if (ws) DispatchItemDestroyed(ws, ref);
                // Scrap rewards go to this settlement's workbench, as they
                // would have if the host had scrapped it.
                if (TESObjectREFR* box = ws ? GetWorkbenchContainer(ws) : nullptr)
                    for (uint32_t i = 0; i < j.nRewards; ++i)
                        if (TESForm* f = LookupFormByID(j.rewardForm[i]))
                            if (f->formType == kFormType_MISC &&
                                CoSyncGameAPI::NativeAddItem(box, f, (std::min)(j.rewardCount[i], kMaxRewardEach)))
                                ++rewarded;
            }
            const uint32_t gone = ref->formID;
            SetLinkedWorkshop(ref, nullptr);
            if (!authority) UnbindLocal(gone);
            DeleteRef(ref);
            LOG_INFO("[Workshop] removed object %u (0x%08X base 0x%08X)%s", j.o.net, gone, j.o.base,
                rewarded ? Fmt(", %u reward stack(s) to the workbench", rewarded).c_str() : "");
            return Apply::Done;
        }
        case 'R':
        {
            TESObjectREFR* ref = ResolveLocal(j.o, false);
            if (!ref) return inRange ? Apply::Done : Apply::Wait;
            static const BSFixedString kClear("ClearDestruction");
            VMArray<VMVariable> args;
            CallFunctionNoWait(ref, kClear, args);
            return Apply::Done;
        }
        }
        return Apply::Drop;
    }

    __declspec(noinline) Apply ApplyJobSafe(Job& j)
    {
        __try { return ApplyJobImpl(j); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_WARN("[Workshop] applying %c for object %u faulted -- caught, dropped", j.kind, j.o.net);
            return Apply::Drop;
        }
    }

    void RunJobs()
    {
        if (s_jobs.empty() || !CoSyncWorld::IsWorldReady()) return;
        std::vector<Job> keep;
        keep.reserve(s_jobs.size());
        std::vector<Job> jobs;
        jobs.swap(s_jobs);
        size_t budget = 24;   // bounded engine work per pass
        for (Job& j : jobs)
        {
            if (budget == 0) { keep.push_back(std::move(j)); continue; }
            const Apply r = ApplyJobSafe(j);
            if (r != Apply::Wait) --budget;
            if (r == Apply::Wait) keep.push_back(std::move(j));
        }
        // Jobs queued while applying (none today) stay behind the kept ones.
        keep.insert(keep.end(), s_jobs.begin(), s_jobs.end());
        s_jobs.swap(keep);
    }

    // ── Local operations (from the hooks) ───────────────────────────────────
    void LocalAuthority(const LocalEvent& e)
    {
        const Capture& c = e.c;
        const uint32_t myEid = CoSyncNet::GetMyEntityID();
        switch (e.op)
        {
        case 'P':
        {
            Obj o = ObjFromCapture(c);
            o.placer = CoSyncProfiles::GetMyProfileKey();
            o.built = true;
            if (IsSplineBase(o.base))
            {
                uint32_t ea = 0, eb = 0;
                if (!ReadWireEnds(c.ref, ea, eb))
                {
                    LOG_WARN("[Workshop] Wire Created (host) 0x%08X but its endpoints are unreadable -- not replicated", c.formID);
                    return;
                }
                Obj& r = RegAdd(o);
                s_refToNet[c.formID] = r.net;
                r.hostRef = c.formID;
                s_wireEnds[r.net] = { EndIdent(ea), EndIdent(eb) };
                BroadcastWire(r, myEid, c.formID);
                LOG_INFO("[Workshop] Wire Created (host): wire %u = 0x%08X, Wire Endpoint A 0x%08X (%s), Wire Endpoint B 0x%08X (%s)",
                    r.net, c.formID, ea, s_wireEnds[r.net].a.c_str(), eb, s_wireEnds[r.net].b.c_str());
                return;
            }
            if ((o.base & 0xFF000000u) == 0xFF000000u || IsActorBase(o.base))
            {
                LOG_INFO("[Workshop] built base 0x%08X is a runtime form or an actor -- not replicated as a workshop object", o.base);
                return;
            }
            Obj& r = RegAdd(o);
            RecountAuthority();
            BroadcastAdd(r, myEid, c.formID);
            LOG_INFO("[Workshop] host built object %u (0x%08X base 0x%08X) in 0x%08X", r.net, c.formID, c.base, c.ws);
            return;
        }
        case 'M':
        {
            Obj* r = RegByHostRef(c.formID);
            if (!r) { Obj o = ObjFromCapture(c); o.ws = c.linkedWs ? c.linkedWs : c.ws; r = &RegAdd(o); RecountAuthority(); }
            r->pos = c.pos; r->rot = c.rot; r->space = c.space; r->cell = c.cell;
            BroadcastMove(*r);
            return;
        }
        case 'S':
        case 'T':
        {
            Obj o = ObjFromCapture(c);
            if (Obj* r = RegByHostRef(c.formID)) { o.net = r->net; RegErase(r->net); RecountAuthority(); }
            CancelJobs(o.net, c.formID);
            BroadcastRemove(o);
            return;
        }
        case 'R':
        {
            Obj o = ObjFromCapture(c);
            if (Obj* r = RegByHostRef(c.formID)) o.net = r->net;
            BroadcastRepair(o);
            return;
        }
        }
    }

    void LocalClient(const LocalEvent& e)
    {
        const Capture& c = e.c;
        if (!SessionLive()) return;
        auto idOf = [&](uint32_t local) {
            auto it = s_localToNet.find(local);
            return it != s_localToNet.end() ? Fmt("N|%u", it->second) : Fmt("R|%u", local);
        };
        switch (e.op)
        {
        case 'P':
            if (IsSplineBase(c.base))
            {
                uint32_t ea = 0, eb = 0;
                if (!ReadWireEnds(c.ref, ea, eb))
                {
                    LOG_WARN("[Workshop] Wire Created locally 0x%08X but its endpoints are unreadable -- not sent", c.formID);
                    return;
                }
                CoSyncTransport::Send(Fmt("WKQ|W|%u|%u|%s|%s|%u", c.formID, c.base,
                    EndIdent(ea).c_str(), EndIdent(eb).c_str(), c.ws));
                LOG_INFO("[Workshop] Wire Created locally 0x%08X (A 0x%08X, B 0x%08X) -- asking the host", c.formID, ea, eb);
                return;
            }
            CoSyncTransport::Send(Fmt("WKQ|P|%u|%u|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%u|%u|%u", c.formID, c.base,
                c.pos.x, c.pos.y, c.pos.z, c.rot.x, c.rot.y, c.rot.z, c.ws, c.space, c.cell));
            return;
        case 'M':
            CoSyncTransport::Send(Fmt("WKQ|M|%s|%u|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%u|%u", idOf(c.formID).c_str(), c.base,
                c.pos.x, c.pos.y, c.pos.z, c.rot.x, c.rot.y, c.rot.z, c.ws, c.formID));
            return;
        case 'S':
        case 'T':
        case 'R':
        {
            std::string msg = Fmt("WKQ|%c|%s|%u|%.4f|%.4f|%.4f|%u|%u", e.op, idOf(c.formID).c_str(), c.base,
                c.pos.x, c.pos.y, c.pos.z, c.ws, c.formID);
            if (e.op == 'S')
            {
                std::string rw;
                for (uint32_t i = 0; i < c.nRewards; ++i)
                {
                    if (i) rw += ",";
                    rw += std::to_string(c.rewardForm[i]) + ":" + std::to_string(c.rewardCount[i]);
                }
                msg += "|" + rw;
            }
            CoSyncTransport::Send(msg);
            if (e.op != 'R') UnbindLocal(c.formID);
            return;
        }
        }
    }

    // ── Host: one client request ────────────────────────────────────────────
    uint32_t EidForConn(uint32_t conn)
    {
        for (const auto& p : CoSyncNet::HostGetPeers())
            if (p.conn == conn) return p.entityID;
        return 0;
    }

    void Deny(uint32_t conn, char op, uint32_t clientRef, const std::string& why)
    {
        CoSyncTransport::SendTo(conn, Fmt("WKN|%c|%u|", op, clientRef) + Pct(why));
        LOG_INFO("[Workshop] DENIED %c from conn=%u (ref 0x%08X): %s", op, conn, clientRef, why.c_str());
    }

    // Resolve the object a request names. R-ids that the host has loaded
    // are checked against the real object; otherwise the reported data is
    // kept and re-checked when the host's world applies it.
    Obj* ResolveRequest(const std::string& kind, uint32_t id, uint32_t base, const NiPoint3& pos, uint32_t ws, bool& created)
    {
        created = false;
        if (kind == "N")
        {
            auto it = s_reg.find(id);
            return it == s_reg.end() ? nullptr : &it->second;
        }
        if (kind != "R" || id == 0)
            return nullptr;
        if (Obj* r = RegByHostRef(id))
            return r;
        Obj o;
        o.hostRef = id;
        o.base = base;
        o.pos = pos;
        o.ws = ws;
        if (TESObjectREFR* ref = LiveRef(id))
        {
            Capture c;
            if (!ReadRefInfo(ref, c) || c.base != base)
                return nullptr;
            o.pos = c.pos; o.rot = c.rot; o.space = c.space; o.cell = c.cell;
            TESObjectREFR* lw = GetLinkedWorkshop(ref);
            if (lw) o.ws = SafeFormID(lw);
        }
        created = true;
        return &RegAdd(o);
    }

    void HostRequest(const std::vector<std::string>& v, uint32_t conn)
    {
        if (v.empty()) return;
        const std::string key = CoSyncProfiles::HostGetConnKey(conn);
        const char op = v[0].empty() ? 0 : v[0][0];
        if (key.empty())
        {
            LOG_WARN("[Workshop] request from conn=%u with no identity yet -- ignored", conn);
            return;
        }
        std::string why;

        if (op == 'P')
        {
            if (v.size() < 12) return;
            const uint32_t cref = U(v[1]);
            Obj o;
            o.base = U(v[2]);
            o.pos = { F(v[3]), F(v[4]), F(v[5]) };
            o.rot = { F(v[6]), F(v[7]), F(v[8]) };
            o.ws = U(v[9]); o.space = U(v[10]); o.cell = U(v[11]);
            o.placer = key;
            o.built = true;
            if (!Finite(o.pos) || !Finite(o.rot)) return Deny(conn, 'P', cref, "Invalid placement.");
            if (!WorkshopByID(o.ws)) return Deny(conn, 'P', cref, "That is not a settlement.");
            if (IsActorBase(o.base))
                return Deny(conn, 'P', cref, "Turrets, robots and other actors can only be built by the host in multiplayer.");
            if ((o.base & 0xFF000000u) == 0xFF000000u || !LookupFormByID(o.base))
                return Deny(conn, 'P', cref, Fmt("That object (0x%08X) is not in the host's game data (a mod the host does not have?).", o.base));
            if (CoSyncWorldSync::IsKnownUnsafePlaceAtMeForm(o.base))
                return Deny(conn, 'P', cref, Fmt("That object (0x%08X) is on CoSync's crash blocklist for remote spawning and cannot be built in multiplayer yet.", o.base));
            if (IsSplineBase(o.base))
                return Deny(conn, 'P', cref, "Wires are not shared in multiplayer yet.");
            if (!CheckAll(key, o.ws, 0, o.pos, WorkshopOp::Build, why)) return Deny(conn, 'P', cref, why);
            if (!s_freeBuild.load() && !OrigContains(o.ws, o.pos))
                return Deny(conn, 'P', cref, "That spot is outside the settlement's build area.");
            if (s_limitMode.load() == static_cast<int>(CoSyncTeams::BuildLimitMode::Custom))
            {
                const uint32_t n = CountFor(o.ws);
                if (n >= s_customLimit.load())
                    return Deny(conn, 'P', cref, Fmt("Build limit reached for this settlement (%u / %u).", n, s_customLimit.load()));
            }
            Obj& r = RegAdd(o);
            RecountAuthority();
            AddJob('A', r, true);
            BroadcastAdd(r, EidForConn(conn), cref);
            LOG_INFO("[Workshop] accepted build %u base=0x%08X in 0x%08X from conn=%u", r.net, r.base, r.ws, conn);
            return;
        }

        if (op == 'W')
        {
            if (v.size() < 6) return;
            const uint32_t cref = U(v[1]);
            Obj o;
            o.base = U(v[2]);
            o.ws = U(v[5]);
            o.placer = key;
            o.built = true;
            if (!IsSplineBase(o.base)) return Deny(conn, 'P', cref, "That is not a wire.");
            if (!WorkshopByID(o.ws)) return Deny(conn, 'P', cref, "That is not a settlement.");
            TESObjectREFR* a = ResolveEnd(v[3]);
            TESObjectREFR* b = ResolveEnd(v[4]);
            if (!a || !b) return Deny(conn, 'P', cref, "The host does not know one of the wired objects yet.");
            o.pos = a->pos;
            if (!CheckAll(key, o.ws, 0, o.pos, WorkshopOp::Build, why)) return Deny(conn, 'P', cref, why);
            Obj& r = RegAdd(o);
            s_wireEnds[r.net] = { v[3], v[4] };
            AddJob('W', r, true);
            BroadcastWire(r, EidForConn(conn), cref);
            LOG_INFO("[Workshop] accepted wire %u (%s -> %s) in 0x%08X from conn=%u", r.net, v[3].c_str(), v[4].c_str(), r.ws, conn);
            return;
        }

        if (op == 'M')
        {
            if (v.size() < 13) return;
            NiPoint3 pos{ F(v[4]), F(v[5]), F(v[6]) }, rot{ F(v[7]), F(v[8]), F(v[9]) };
            const uint32_t ws = U(v[10]), cref = U(v[11]);
            if (!Finite(pos) || !Finite(rot)) return Deny(conn, 'M', cref, "Invalid move.");
            bool created = false;
            Obj* o = ResolveRequest(v[1], U(v[2]), U(v[3]), pos, ws, created);
            if (!o) return Deny(conn, 'M', cref, "Unknown object.");
            if (created) RecountAuthority();
            if (!CheckAll(key, o->ws, 0, o->pos, WorkshopOp::Move, why) ||
                !CheckAll(key, ws ? ws : o->ws, o->ws, pos, WorkshopOp::Move, why))
            {
                Deny(conn, 'M', cref, why);
                BroadcastMove(*o, conn);   // put it back where it was
                return;
            }
            if (!s_freeBuild.load() && !OrigContains(o->ws, pos))
            {
                Deny(conn, 'M', cref, "That spot is outside the settlement's build area.");
                BroadcastMove(*o, conn);
                return;
            }
            o->pos = pos;
            o->rot = rot;
            AddJob('V', *o, true);
            BroadcastMove(*o);
            LOG_INFO("[Workshop] accepted move %u (host 0x%08X base 0x%08X) to (%.0f,%.0f,%.0f) from conn=%u",
                o->net, o->hostRef, o->base, pos.x, pos.y, pos.z, conn);
            return;
        }

        if (op == 'S' || op == 'T' || op == 'R')
        {
            if (v.size() < 10) return;
            NiPoint3 pos{ F(v[4]), F(v[5]), F(v[6]) };
            const uint32_t ws = U(v[7]), cref = U(v[8]);
            if (!Finite(pos)) return;
            bool created = false;
            Obj* o = ResolveRequest(v[1], U(v[2]), U(v[3]), pos, ws, created);
            if (!o) return Deny(conn, op, cref, "Unknown object.");
            const WorkshopOp wop = op == 'R' ? WorkshopOp::Repair : WorkshopOp::Scrap;
            if (!CheckAll(key, o->ws, ws, o->pos, wop, why))
            {
                Deny(conn, op, cref, why);
                if (op != 'R')
                    BroadcastAdd(*o, 0, 0, conn);   // restore it for the sender
                if (created) { RegErase(o->net); RecountAuthority(); }
                return;
            }
            if (op == 'R')
            {
                AddJob('R', *o, true);
                BroadcastRepair(*o);
                LOG_INFO("[Workshop] accepted repair %u (host 0x%08X) from conn=%u", o->net, o->hostRef, conn);
                return;
            }
            Capture rewards;
            if (op == 'S' && v.size() >= 10)
                for (const std::string& pair : Split(v[9], ','))
                {
                    const size_t colon = pair.find(':');
                    if (colon == std::string::npos || rewards.nRewards >= kMaxRewards) continue;
                    rewards.rewardForm[rewards.nRewards] = U(pair.substr(0, colon));
                    rewards.rewardCount[rewards.nRewards] = (std::min)(U(pair.substr(colon + 1)), kMaxRewardEach);
                    ++rewards.nRewards;
                }
            const Obj copy = *o;
            RegErase(copy.net);
            RecountAuthority();
            CancelJobs(copy.net, 0);
            if (copy.hostRef)
                AddJob('X', copy, true, &rewards);
            BroadcastRemove(copy);
            LOG_INFO("[Workshop] accepted %s %u (host 0x%08X base 0x%08X, %u reward stack(s)) from conn=%u",
                op == 'S' ? "scrap" : "store", copy.net, copy.hostRef, copy.base, rewards.nRewards, conn);
            return;
        }
    }

    // ── Client: host results ────────────────────────────────────────────────
    Obj ParseObj(const std::vector<std::string>& v, bool full)
    {
        Obj o;
        o.net = U(v[0]); o.hostRef = U(v[1]); o.base = U(v[2]);
        o.pos = { F(v[3]), F(v[4]), F(v[5]) };
        if (full)
        {
            o.rot = { F(v[6]), F(v[7]), F(v[8]) };
            o.ws = U(v[9]);
        }
        return o;
    }

    void ClientResult(const std::string& type, const std::vector<std::string>& v)
    {
        const uint32_t myEid = CoSyncNet::GetMyEntityID();
        if (type == "WKA" && v.size() >= 14)
        {
            Obj o = ParseObj(v, true);
            o.space = U(v[10]); o.cell = U(v[11]);
            const uint32_t placerEid = U(v[12]), placerRef = U(v[13]);
            if (placerEid == myEid && placerRef != 0)
            {
                // Our own build, accepted: bind it.
                BindLocal(o.net, placerRef);
                return;
            }
            AddJob('A', o, false);
            return;
        }
        if (type == "WKW" && v.size() >= 8)
        {
            Obj o;
            o.net = U(v[0]); o.hostRef = U(v[1]); o.base = U(v[2]); o.ws = U(v[5]);
            s_wireEnds[o.net] = { v[3], v[4] };
            const uint32_t placerEid = U(v[6]), placerRef = U(v[7]);
            if (placerEid == myEid && placerRef != 0)
            {
                BindLocal(o.net, placerRef);   // our own wire, accepted
                return;
            }
            AddJob('W', o, false);
            return;
        }
        if (type == "WKV" && v.size() >= 10)
        {
            AddJob('V', ParseObj(v, true), false);
            return;
        }
        if (type == "WKX" && v.size() >= 6)
        {
            AddJob('X', ParseObj(v, false), false);
            return;
        }
        if (type == "WKR" && v.size() >= 2)
        {
            Obj o;
            o.net = U(v[0]); o.hostRef = U(v[1]);
            AddJob('R', o, false);
            return;
        }
        if (type == "WKC")
        {
            std::unordered_map<uint32_t, uint32_t> counts;
            if (!v.empty() && !v[0].empty())
                for (const std::string& kv : Split(v[0], ';'))
                {
                    const size_t eq = kv.find('=');
                    if (eq != std::string::npos) counts[U(kv.substr(0, eq))] = U(kv.substr(eq + 1));
                }
            std::lock_guard<std::mutex> lk(s_countMx);
            s_counts.swap(counts);
            return;
        }
        if (type == "WKN" && v.size() >= 3)
        {
            const char op = v[0].empty() ? 0 : v[0][0];
            const uint32_t cref = U(v[1]);
            const std::string why = Unpct(v[2]);
            CoSyncGameAPI::QueueNotification(why.c_str());
            if (op == 'P')
            {
                // Our predicted build was refused: remove our local copy.
                // (Moves are put back by the WKV that follows; a refused
                // scrap/store is restored by the WKA that follows.)
                UnbindLocal(cref);
                if (TESObjectREFR* r = LiveRef(cref))
                {
                    SetLinkedWorkshop(r, nullptr);
                    DeleteRef(r);
                }
            }
            return;
        }
    }
}

namespace CoSyncWorkshopAuthority
{
    void OnSessionStart(bool isHost)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        s_buckets.clear();
        s_netToLocal.clear();
        s_localToNet.clear();
        if (!isHost)
        {
            s_jobs.clear();
            std::lock_guard<std::mutex> lc(s_countMx);
            s_counts.clear();
        }
        else
            RecountAuthority();
        std::lock_guard<std::mutex> le(s_evMx);
        s_events.clear();
    }

    void OnSessionEnd()
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        const bool client = CoSyncTransport::IsInitialized() && !CoSyncTransport::IsHost();
        s_netToLocal.clear();
        s_localToNet.clear();
        s_buckets.clear();
        if (client)
        {
            s_jobs.clear();
            std::lock_guard<std::mutex> lc(s_countMx);
            s_counts.clear();
        }
    }

    bool HooksActive()
    {
        return s_hooksActive.load();
    }

    void Tick(double now)
    {
        (void)now;
        RefreshRules();

        if (!s_hookAttempted && CoSyncWorld::IsWorldReady())
        {
            s_hookAttempted = true;
            const bool ok = InstallHooks();
            s_hooksActive.store(ok);
            // The verified hooks replace the old after-the-fact cell scan.
            CoSyncWorldSync::SetLegacyWorkshopSync(!ok);
        }

        std::deque<LocalEvent> events;
        {
            std::lock_guard<std::mutex> le(s_evMx);
            events.swap(s_events);
        }

        std::lock_guard<std::recursive_mutex> lk(s_mx);

        // Local operations from the hooks.
        for (const LocalEvent& e : events)
        {
            if (AuthorityNow()) LocalAuthority(e);
            else                LocalClient(e);
        }

        const double t = Now();
        if (t >= s_nextJobs)
        {
            s_nextJobs = t + 0.5;
            RunJobs();
        }

        if (AuthorityNow() && s_countsDirty && t >= s_nextCounts)
        {
            s_countsDirty = false;
            s_nextCounts = t + 1.0;
            Send("WKC|" + CountsBody(), 0);
        }
    }

    bool IsWorkshopMessage(const std::string& msg)
    {
        return msg.size() >= 4 && msg[0] == 'W' && msg[1] == 'K' && msg[3] == '|';
    }

    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole)
    {
        LOG_REQUIREMENT("CoSyncWorkshopAuthority::OnReceive", "CoSync tick. Untrusted input: the host re-validates every request against the sender's connection identity and the real object.");
        if (msg.size() > 8192)
            return;
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        const std::string type = msg.substr(0, 3);
        const std::vector<std::string> v = Split(msg.substr(4), '|');

        if (type == "WKQ")
        {
            if (!isHostRole || conn == 0)
                return;
            Bucket& b = s_buckets[conn];
            const double t = Now();
            b.tokens = (std::min)(40.0, b.tokens + (t - b.last) * 20.0);
            b.last = t;
            if (b.tokens < 1.0)
                return;
            b.tokens -= 1.0;
            HostRequest(v, conn);
            return;
        }
        // Results only ever come from the host.
        if (isHostRole)
            return;
        ClientResult(type, v);
    }

    void HostOnPeerJoined(uint32_t conn)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        if (!SessionLive()) return;
        // Everything already in the host's world is in the save the newcomer
        // just loaded (same form IDs; bound on first use). Only objects the
        // host accepted but has not placed yet (it was far away) must be sent.
        size_t sent = 0;
        for (const auto& kv : s_reg)
            if (kv.second.hostRef == 0)
            {
                BroadcastAdd(kv.second, 0, 0, conn);
                ++sent;
            }
        Send("WKC|" + CountsBody(), conn);
        LOG_INFO("[Workshop] joining conn=%u: %zu registered object(s), %zu not yet in the host world sent",
            conn, s_reg.size(), sent);
    }

    uint32_t GetBuiltCount(uint32_t ws)
    {
        return CountFor(ws);
    }

    std::string BuildUiJson()
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        std::string j = "{\"hooks\":";
        j += s_hooksActive.load() ? "true" : "false";
        j += ",\"counts\":{";
        bool first = true;
        std::lock_guard<std::mutex> lc(s_countMx);
        for (const auto& kv : s_counts)
        {
            if (!first) j += ",";
            first = false;
            j += "\"" + std::to_string(kv.first) + "\":" + std::to_string(kv.second);
        }
        j += "}}";
        return j;
    }

    void SaveRecord(const F4SESerializationInterface* ser)
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        if (!AuthorityNow())
            return;
        std::string body = "N|" + std::to_string(s_nextNet) + "\n";
        for (const auto& kv : s_reg)
        {
            const Obj& o = kv.second;
            body += Fmt("O|%u|%u|%u|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%u|%u|%u|", o.net, o.hostRef, o.base,
                o.pos.x, o.pos.y, o.pos.z, o.rot.x, o.rot.y, o.rot.z, o.ws, o.space, o.cell) + Pct(o.placer) +
                (o.built ? "|1\n" : "|0\n");
        }
        const uint32_t len = static_cast<uint32_t>(body.size());
        ser->OpenRecord(kRecordType, kRecordVersion);
        ser->WriteRecordData(&len, 4);
        ser->WriteRecordData(body.data(), len);
        LOG_INFO("[Workshop] Co-save: wrote %zu settlement object(s)", s_reg.size());
    }

    bool LoadRecord(const F4SESerializationInterface* ser, uint32_t type, uint32_t version, uint32_t length)
    {
        if (type != kRecordType)
            return false;
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        if (version != kRecordVersion || length < 4)
            return true;
        std::string buf(length, '\0');
        if (ser->ReadRecordData(&buf[0], length) != length)
            return true;
        if (!AuthorityNow())
            return true;   // a client gets the live registry from the host
        uint32_t len = 0;
        std::memcpy(&len, buf.data(), 4);
        if (len > length - 4)
            return true;
        s_reg.clear();
        s_refToNet.clear();
        s_jobs.clear();
        s_nextNet = 1;
        for (const std::string& line : Split(buf.substr(4, len), '\n'))
        {
            const std::vector<std::string> v = Split(line, '|');
            if (v.size() >= 2 && v[0] == "N") s_nextNet = (std::max)(s_nextNet, U(v[1]));
            if (v.size() < 14 || v[0] != "O") continue;
            Obj o;
            o.net = U(v[1]); o.hostRef = U(v[2]); o.base = U(v[3]);
            o.pos = { F(v[4]), F(v[5]), F(v[6]) };
            o.rot = { F(v[7]), F(v[8]), F(v[9]) };
            o.ws = U(v[10]); o.space = U(v[11]); o.cell = U(v[12]);
            o.placer = Unpct(v[13]);
            o.built = v.size() >= 15 && v[14] == "1";
            if (!o.net) continue;
            s_nextNet = (std::max)(s_nextNet, o.net + 1);
            if (o.hostRef) s_refToNet[o.hostRef] = o.net;
            s_reg[o.net] = o;
            // Accepted while the host was far away and never placed yet.
            if (!o.hostRef)
                AddJob('A', o, true);
        }
        RecountAuthority();
        LOG_INFO("[Workshop] Co-save: loaded %zu settlement object(s)", s_reg.size());
        return true;
    }

    void Revert()
    {
        std::lock_guard<std::recursive_mutex> lk(s_mx);
        if (!AuthorityNow())
            return;
        s_reg.clear();
        s_refToNet.clear();
        s_jobs.clear();
        s_nextNet = 1;
        std::lock_guard<std::mutex> lc(s_countMx);
        s_counts.clear();
    }
}
