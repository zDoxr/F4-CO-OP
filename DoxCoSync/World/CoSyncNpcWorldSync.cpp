#define WIN32_LEAN_AND_MEAN
#include "CoSyncProxyLooks.h"
#include <Windows.h>

#include "CoSyncNpcWorldSync.h"

#include "CoSyncRequirementsLog.h"
#include "ConsoleLogger.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncProfiles.h"
#include "CoSyncEntityState.h"
#include "CoSyncEntityRegistry.h"
#include "CoSyncTransport.h"
#include "CoSyncGameAPI.h"
#include "CoSyncPapyrusHelper.h"
#include "CoSyncSpawnTasks.h"
#include "CoSyncHealthSync.h"
#include "CoSyncActorValueSafety.h"
#include "EntitySerialization.h"
#include "Packets_EntityCreate.h"
#include "Packets_EntityDestroy.h"
#include "CoSyncEntityTypes.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "GameRTTI.h"
#include "GameHandle.h"

#include "CoSyncNet.h"
#include "TickHook.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"

#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <cstring>

// Was extern RelocPtr<PlayerCharacter*> g_player -- switched to CoSyncGetPlayer()
// (see GameGlobals.cpp), the established, confirmed-working replacement for
// raw g_player dereferences used throughout the rest of the codebase.
extern PlayerCharacter* CoSyncGetPlayer();

// 2026-08-16 pt.6: RE-ENABLED. Was disabled after network overload (140
// tracked actors -> 1000+/sec combined) and 9x CoSync-VEH crashes in 19s
// (TickScanWorldActors_Impl -> IsActorDead -> SafeGetValue on dynamic-baseform
// actors). The dynamic-baseform crash was already fixed before the disable
// (both IsActorDead call sites in this file skip the AV touch for that actor
// class, treating "not disabled" as sufficient liveness evidence instead --
// see considerRefr and the departure sweep below). The overload was root-
// caused this pass: HostSendNpcUpdates_Impl (CoSyncPlayerManager.cpp) sent a
// full-payload update for every tracked world actor on every 20Hz tick with
// no batching at all -- fixed there with a 24-per-tick rotation cap, the same
// class of fix a separate FO4 co-op project (FO4_Wrld) used for the identical
// shape of bug. kMaxConcurrentWorldActors raised from 15 to 64 accordingly
// (see its own comment below). This is the first test of the reworked
// system -- watch for `[SendDiag] rotationSkipped=` confirming the cap
// engages under a real fight, and for the absence of any further
// `[CoSync-VEH] CRASH` with lastTickStep=TickScanWorldActors.
static constexpr bool kEnemyCreatureSyncEnabled = true;

static void LogEnemyCreatureSyncDisabledOnce()
{
    static bool s_logged = false;
    if (!s_logged)
    {
        s_logged = true;
        LOG_WARN("[NpcWorldSync] Enemy and creature syncing is turned off right now (it can overload the network and crash clients). Skipping world-actor discovery this session.");
    }
}

// ── Per-race sync visibility (item #6) ──────────────────────────────────────
// There is NO race filter anywhere in this file -- discovery and spawning key
// off actor->baseForm generically, and since the build-220 AI-alive puppet
// model the engine animates creatures natively instead of us driving 299 anim
// vars. So creatures should sync structurally exactly like humans, and the
// open question was never "is it wired up" but "WHICH races actually make it
// through in practice".
//
// Fallout4.esm has 45 RACE records / 3015 NPCs, but 1998 of those NPCs share
// Character\CharacterAssets\skeleton.nif (human) -- the other ~1017 span 31
// distinct skeletons, each with its own behaviour graph. See
// Animations/CREATURE_SYNC_INVENTORY.md for the full table and tiering.
//
// Race is read through Actor vtable slot 0x91 GetActorRace(), which is
// declared on TESObjectREFR (f4se GameReferences.h:135) and therefore sits
// safely inside REFR's 198-slot vtable -- unlike SetPosition(0xCA), which is
// Actor-only and would run off the end for a plain ref.
namespace
{
    constexpr size_t kVtbl_GetActorRace = 0x91;

    __declspec(noinline) uint32_t SafeGetRaceFormID(Actor* actor)
    {
        LOG_REQUIREMENT("(anonymous)::SafeGetRaceFormID", "Needs a valid actor. Reads its race and returns the race's ID; if the actor is gone, safely returns 0 instead of crashing.");
        if (!actor) return 0;
        using Fn = TESForm* (*)(Actor*);
        __try
        {
            void*** vt = reinterpret_cast<void***>(actor);
            if (!vt || !*vt) return 0;
            Fn fn = reinterpret_cast<Fn>((*vt)[kVtbl_GetActorRace]);
            if (!fn) return 0;
            TESForm* race = fn(actor);
            return race ? race->formID : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // 2026-08-16 pt.7: dynamic (0xFF-prefixed) base forms are PROCESS-LOCAL --
    // the same runtime leveled-actor instance gets a DIFFERENT dynamic form
    // number on every machine's independent engine, so sending one across the
    // wire and resolving it locally (LookupFormByID, CoSyncSpawnTasks::
    // ResolveNPC) finds whatever unrelated form happens to occupy that slot
    // number on the RECEIVING machine. Confirmed live cause of "wrong ghoul
    // model": host log showed race=0x0001A009 (FeralGhoul) actors registered
    // with base=0xFF000DCE-style dynamic forms; the client resolved that
    // number against its OWN independently-allocated dynamic-form table and
    // spawned whatever NPC happened to live there -- not a ghoul.
    //
    // TESNPC's templateNPC field is the standard FO4 mechanism a leveled/
    // runtime-spawned NPC inherits its look from (a LeveledCharacter entry
    // resolves to a template NPC and the runtime instance inherits Traits --
    // race, sex, head data -- from it wholesale). That template chain bottoms
    // out at a real ESM/ESP-authored TESNPC record, whose formID IS identical
    // across every machine's load order (same assumption already made
    // everywhere else in this codebase for static form resolution). Walk the
    // chain to find that static ancestor and use IT instead of the raw
    // dynamic formID -- for both the wire baseFormID (so the client resolves
    // the correct race/appearance) and the spatial dedup key (so two
    // machines' independent copies of the same encounter creature hash to the
    // same key despite having different raw dynamic form numbers, which they
    // do not today -- a second, related bug this same fix closes).
    //
    // Offset 0x270 for templateNPC is already used and verified elsewhere in
    // this codebase (CoSyncGameAPI.cpp's SpawnRemoteActor player-proxy
    // special case clears the same field at the same offset).
    struct TESNPCWithTemplate { uint8_t pad[0x270]; TESNPC* templateNPC; };

    __declspec(noinline) uint32_t ResolveCrossMachineBaseFormID(TESNPC* npc)
    {
        LOG_REQUIREMENT("(anonymous)::ResolveCrossMachineBaseFormID", "Safe to call with a null NPC (returns 0). Follows the NPC's template chain up to 8 steps looking for a stable, shared ID. Returns 0 if none is found, so the caller skips syncing instead of sending an ID that only makes sense on this machine.");
        if (!npc) return 0;

        __try
        {
            TESNPC* cur = npc;
            for (int hop = 0; cur && hop < 8; ++hop)
            {
                if ((cur->formID & 0xFF000000u) != 0xFF000000u)
                    return cur->formID;   // static ancestor -- shared across machines

                TESNPC* next = reinterpret_cast<TESNPCWithTemplate*>(cur)->templateNPC;
                if (!next || next == cur)
                    return 0;             // chain doesn't bottom out at a static form
                cur = next;
            }
            return 0;                     // hop limit hit without finding a static ancestor
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    std::unordered_map<uint32_t, uint32_t> s_raceRegistrations;  // raceFormID -> count
    double   s_lastRaceDiag = 0.0;
    constexpr double kRaceDiagInterval = 15.0;

    void LogRaceDiag(double now)
    {
        if ((now - s_lastRaceDiag) < kRaceDiagInterval) return;
        s_lastRaceDiag = now;
        if (s_raceRegistrations.empty()) return;

        std::string line;
        for (const auto& kv : s_raceRegistrations)
        {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "0x%08X:%u ", kv.first, kv.second);
            line += buf;
        }
        LOG_INFO("[RaceDiag] synced creatures by race (raceID:count) -- %s", line.c_str());
    }
}

namespace
{
    static constexpr uint32_t kHealthAVI_FormID    = 0x0002D4u;
    static constexpr uint32_t kLegendaryRankAVI_FormID = 0x001DFDFBu;  // [AVIF:001DFDFB] LegendaryRank in Fallout4.esm

    // Declared here (used by IsActorDead below) rather than down with the
    // rest of the scan state, since anonymous-namespace declaration order
    // still matters within a single TU. Shared across every IsActorDead
    // call site (discovery filter, departure check, dynamic-spawn suppress)
    // so a broken actor is blacklisted permanently after its first fault,
    // not per-call-site.
    std::unordered_set<Actor*> s_badHealthActors;

    // Actors whose parentCell was null at PreSaveGame and was temporarily patched
    // to the player's current cell so BGSSaveLoadGame::SaveGame() can build a
    // valid location descriptor (Fallout4.exe+0xBF712D). Cleared and restored to
    // nullptr in RestoreActorsAfterSave (kMessage_PostSaveGame).
    std::unordered_set<Actor*> s_saveProtectedActors;

    static ActorValueInfo* GetHealthAVICached()
    {
        LOG_REQUIREMENT("(anonymous)::GetHealthAVICached", "Just needs form lookup to work. Caches the Health stat type and returns null if it can't be found.");
        static ActorValueInfo* s_cached = nullptr;
        if (!s_cached)
        {
            TESForm* form = LookupFormByID(kHealthAVI_FormID);
            if (form && form->formType == kFormType_AVIF)
                s_cached = static_cast<ActorValueInfo*>(form);
        }
        return s_cached;
    }

    static ActorValueInfo* GetLegendaryAVICached()
    {
        static ActorValueInfo* s_cached = nullptr;
        if (!s_cached)
        {
            TESForm* form = LookupFormByID(kLegendaryRankAVI_FormID);
            if (form && form->formType == kFormType_AVIF)
                s_cached = static_cast<ActorValueInfo*>(form);
        }
        return s_cached;
    }

    // ACHR record flag bit 9 = "Starts Dead". Pre-placed corpses keep a
    // positive Health value, so the AV check alone reports them ALIVE -- the
    // host then registered them and the client replaced its corpse with a
    // live, idle clone ("dead enemies stand back up where they died"). This
    // reads TESForm::flags only (no actorValueOwner touch), so it is safe on
    // every actor class including the dynamic-baseform ones.
    __declspec(noinline) static bool SafeHasStartsDeadFlag(Actor* actor)
    {
        LOG_REQUIREMENT("(anonymous)::SafeHasStartsDeadFlag", "actor may be null; reads TESForm::flags under SEH and tests the ACHR 'Starts Dead' record bit (0x200).");
        __try
        {
            return actor && (actor->flags & 0x200u) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline bool IsActorDead(Actor* actor)
    {
        LOG_REQUIREMENT("(anonymous)::IsActorDead", "Needs a valid actor. Uses the safe health-reading helper since this actor might be one of our proxies with AI turned off. If reading its health ever fails, that actor is remembered so we don't keep trying and failing every tick. Also treats the ACHR 'Starts Dead' record flag as dead since such corpses keep positive Health.");
        if (SafeHasStartsDeadFlag(actor))
            return true;
        if (s_badHealthActors.count(actor))
            return false;
        ActorValueInfo* healthAVI = GetHealthAVICached();
        if (!healthAVI)
            return false;
        float value = 0.f;
        if (!CoSyncAVSafety::SafeGetValue(actor, healthAVI, value))
        {
            s_badHealthActors.insert(actor);
            return false;
        }
        return value <= 0.f;
    }
}

namespace
{

    double s_lastScanTime = -999.0;

    // Host: world actors for which a KILL has been broadcast. They stay
    // registered as corpses until they unload / leave range, so the client
    // keeps its corpse instead of getting an immediate DESTROY that used to
    // despawn the corpse and re-enable the (alive-in-its-save) original.
    std::unordered_set<uint32_t>           s_deadEntities;

    // Client: world-actor proxies a KILL has been applied to.
    std::unordered_set<uint32_t>           s_killedEntities;

    // Host: per-ref throttle for NPCDEAD replies to NPCSEEN reports.
    std::unordered_map<uint32_t, double>   s_npcDeadSentAt;

    // Client: placed refs the host reported dead (suppresses NPCSEEN spam
    // while the local Kill() is still pending).
    std::unordered_set<uint32_t>           s_knownDeadRefs;

    std::mutex                             s_npcDeadMutex;
    std::vector<uint32_t>                  s_pendingNpcDead;


    static constexpr double kScanInterval = 1.0;

    // ProcessLists singleton (AI process manager), IDA-verified 2026-07-20 for
    // 1.11.221 via the console ToggleAI handler (0x5EA650 -> qword_1430DC140)
    // and the AI-resume walker (0x140DAF350): high-process actor handle array
    // data at +0x40, count at +0x50, uint32 handles resolved through the
    // standard handle table (LookupREFRByHandle). This is the engine's own
    // list of every actively-simulated actor in the loaded area — the widest
    // set the engine physically has (whole uGrids grid + attached interiors),
    // vs. the old parentCell-only scan.
    RelocPtr<uintptr_t> g_processListsPtr(0x30E71B8);
    static constexpr uintptr_t kPL_HighHandleData  = 0x40;
    static constexpr uintptr_t kPL_HighHandleCount = 0x50;
    // 2026-08-02: CoSync only ever walked the HIGH actor list, so NPCs that sit
    // in the MIDDLE-HIGH process list (very common in busy areas — settlers,
    // creatures, NPCs not directly engaged with the player) were NEVER
    // discovered/synced. Live proof: host + client co-located in a populated
    // fight registered only 2 world actors (both stale/far), while dozens of
    // NPCs around the players stayed local-only (desynced). commonlibf4's
    // ProcessLists layout: highActorHandles@0x40, middleHighActorHandles@0x70;
    // CoSync's count sits at array+0x10 (High uses 0x40/0x50), so MiddleHigh is
    // 0x70/0x80. Now walk BOTH lists (partner/FalloutTogether do the same).
    static constexpr uintptr_t kPL_MiddleHighHandleData  = 0x70;
    static constexpr uintptr_t kPL_MiddleHighHandleCount = 0x80;

    uint32_t s_nextEntityID = CoSyncNpcWorldSync::kWorldActorBase;

    std::unordered_map<Actor*, uint32_t>  s_actorToEID;
    std::unordered_map<uint32_t, Actor*>  s_eidToActor;
    std::unordered_map<uint32_t, float>   s_lastNpcHP;
    std::unordered_map<uint32_t, int>     s_lastNpcLegendary;

    std::mutex                            s_legendaryMutex;
    std::unordered_map<uint32_t, int>     s_pendingLegendary;

    // Client spawn-fallback bookkeeping: world NPCs whose bind kept failing
    // (the client's own save doesn't have that NPC in the cell — live case
    // 2026-07-05: P2 had no Codsworth at Sanctuary) get a real clone spawned
    // after kBindFailSpawnDelay of retries.
    std::unordered_map<uint32_t, double>  s_firstBindFail;
    std::unordered_set<uint32_t>          s_spawnFallback;
    constexpr double kBindFailSpawnDelay = 24.0;

    std::unordered_map<uint32_t, uint32_t> s_claimedRefToEID;
    std::unordered_map<uint32_t, double>   s_npcSeenResendAt;

    // ── Tilted-style per-creature ownership (Stage 1) ────────────────────────
    // Dynamic (0xFF) creatures have NO shared formID across machines, so the
    // host cannot dedupe two peers' reports of the SAME encounter by ref. It
    // dedupes SPATIALLY instead: a coarse (baseForm, quantized worldspace pos)
    // key. First reporter of a key owns the creature; later reports of the same
    // key are told to puppet the existing entity rather than spawning a rival.
    // (Host-only state.) Proximity-based ownership HANDOFF is Stage 2 — for now
    // the owner is simply whoever discovered the creature first.
    std::unordered_map<uint64_t, uint32_t> s_spatialClaims;      // spatialKey -> entityID
    std::unordered_map<uint32_t, uint64_t> s_entityToSpatialKey; // entityID   -> spatialKey

    // Client-side: local dynamic creatures already reported to the host, so a
    // client doesn't re-broadcast the same CSEEN every scan. Keyed by the
    // client's own (session-relative) refFormID.
    // Dynamic ref -> when it was last reported (re-reported every 10 s so a
    // released NPC can be claimed again; the host de-duplicates).
    std::unordered_map<uint32_t, double>   s_reportedLocalRefs;

    // Quantize a world position + base form into a coarse spatial key. 512 units
    // (~1/8 of an exterior cell) tolerates the drift between two engines'
    // independent copies of the same encounter creature while still separating
    // genuinely different spawns.
    inline uint64_t MakeSpatialKey(uint32_t baseFormID, const NiPoint3& pos)
    {
        const int64_t qx = static_cast<int64_t>(std::floor(pos.x / 512.0f));
        const int64_t qy = static_cast<int64_t>(std::floor(pos.y / 512.0f));
        // Fold base form + quantized X/Y into 64 bits. Z is ignored on purpose:
        // ground creatures at the same X/Y are the same spawn regardless of a
        // small Z difference (ragdoll, terrain).
        uint64_t k = static_cast<uint64_t>(baseFormID) * 0x9E3779B1ull;
        k ^= (static_cast<uint64_t>(qx & 0xFFFFFF) << 24);
        k ^= (static_cast<uint64_t>(qy & 0xFFFFFF));
        return k;
    }

    // Refs this peer Disable()d as originals when becoming a puppet-holder.
    // The engine's disabled flag is set asynchronously (or not at all) after
    // the Papyrus Disable, so the scan needs its own record to skip them.
    std::unordered_set<uint32_t>           s_disabledOriginalRefs;

    std::unordered_map<Actor*, int> s_dynamicSpawnTicksSeen;
    double s_lastSuppressScanTime = -999.0;

    double NpcNowSeconds()
    {
        LOG_REQUIREMENT("(anonymous)::NpcNowSeconds", "Doesn't touch the game at all -- just reads the system's high-precision clock.");
        LARGE_INTEGER f{}, c{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
    }

    // 2026-08-16: hard ceiling on concurrently-tracked world actors. Removing
    // considerRefr's dynamic-baseform exclusion (same date) correctly let
    // real enemies get discovered, but exposed that departure/cleanup
    // (the kKeepRadius sweep in TickScanWorldActors_Impl) essentially never
    // catches most of them -- live log proof: a single ~4min session
    // discovered 140 world actors and only released 18 of them, ending with
    // both host and client streaming 1000+ position/anim updates/sec
    // combined (~148MB total traffic in 4 minutes, GNS lock-held spikes up
    // to 68.6ms) for a two-player session. kWorldActorMax's ~131k-ID window
    // was never a meaningful ceiling for this (would take ~62 hours to
    // exhaust at that rate) -- it bounds ID-space exhaustion, not live
    // concurrent count. This bounds concurrent count directly instead, as
    // an immediate safety net while the departure-timing bug itself is
    // still being tracked down. s_eidToActor is exactly "currently allocated
    // and not yet destroyed", so its size is the live count for free.
    //
    // 2026-08-16 pt.6 (FO4_Wrld-informed rework): raised from 15. The real
    // overload mechanism turned out to be CoSyncPlayerManager.cpp's
    // HostSendNpcUpdates_Impl sending a full-payload EntityUpdatePacket
    // (position + weapon + throwable + combat target + complete anim-var
    // snapshot) for EVERY tracked world actor on EVERY 20Hz call, completely
    // unbatched -- 140 tracked actors x 20Hz is exactly the measured
    // 1000+/sec. That function now round-robins a bounded 24 world-actor
    // sends per tick instead, so per-tick bandwidth no longer scales with how
    // many actors are TRACKED. This cap's job changes accordingly: from "the
    // only thing standing between us and 148MB/4min" to a real ceiling for
    // memory/CPU safety. 64 is well above anything a two-player session has
    // ever measured (140 discovered was the pathological case that exposed
    // the bug) while still being a hard stop, not another open-ended leak.
    static constexpr size_t kMaxConcurrentWorldActors = 64;

    uint32_t AllocateEntityID()
    {
        LOG_REQUIREMENT("(anonymous)::AllocateEntityID", "Doesn't touch the game -- just hands out a new ID from our internal tracking. Returns 0 if we're out of IDs or already tracking as many actors as we allow.");

        if (s_eidToActor.size() >= kMaxConcurrentWorldActors)
        {
            static double s_lastCapWarn = 0.0;
            const double now = NpcNowSeconds();
            if (now - s_lastCapWarn >= 5.0)
            {
                s_lastCapWarn = now;
                LOG_UPDATED_WARN("[NpcWorldSync] Hit the limit of %zu actors tracked at once -- won't track more until some leave",
                    kMaxConcurrentWorldActors);
            }
            return 0;
        }

        const uint32_t startID = s_nextEntityID;
        do
        {
            uint32_t candidate = s_nextEntityID;


            ++s_nextEntityID;
            if (s_nextEntityID > CoSyncNpcWorldSync::kWorldActorMax)
                s_nextEntityID = CoSyncNpcWorldSync::kWorldActorBase;


            if (s_eidToActor.find(candidate) == s_eidToActor.end()
                && g_CoSyncPlayerManager.GetStates().find(candidate) == g_CoSyncPlayerManager.GetStates().end())
                return candidate;


        } while (s_nextEntityID != startID);

        LOG_UPDATED_WARN("[NpcWorldSync] Ran out of IDs to hand out to world actors!");
        return 0;
    }

    void ReleaseClaimForEntity(uint32_t eid)
    {
        for (auto it = s_claimedRefToEID.begin(); it != s_claimedRefToEID.end(); )
        {
            if (it->second == eid)
            {
                s_npcSeenResendAt.erase(it->first);
                it = s_claimedRefToEID.erase(it);
            }
            else
                ++it;
        }

        // Release the spatial ownership claim too, so the encounter creature can
        // be re-owned (by any peer) the next time it is discovered.
        auto skIt = s_entityToSpatialKey.find(eid);
        if (skIt != s_entityToSpatialKey.end())
        {
            auto scIt = s_spatialClaims.find(skIt->second);
            if (scIt != s_spatialClaims.end() && scIt->second == eid)
                s_spatialClaims.erase(scIt);
            s_entityToSpatialKey.erase(skIt);
        }
    }

}

namespace CoSyncNpcWorldSync
{
static bool IsHandoffHeld(uint32_t refFormID, uint32_t baseFormID, const NiPoint3& pos);   // dynamic ownership, below

// How far apart the host's reported position and this client's own copy of an
// NPC may be before a bind is refused.
//
// A CK-placed NPC has the SAME formID on both machines, and generic actors
// share base forms, so resolving a peer's NPC locally is NOT proof it is the
// same instance-in-context. Binding without this check disabled the client's
// real NPC and then steered it to the host's position -- every NPC loaded in
// the client's cell teleported to the host as soon as proxy transforms began
// applying (reported 2026-08-04).
//
// 2026-08-04 round 2: 6000 was FAR too generous. Live log still showed 4 NPCs
// bound and disabled on the client (disabledRef=4) with only 2 refusals -- and
// the user still saw NPCs following the proxy around. 6000 units is roughly
// three exterior cells; a genuinely-same NPC does not need anywhere near that.
//
// Sanity check on the number: the host re-broadcasts CREATE every ~2-5s and an
// NPC moves at a few hundred units/sec, so worst-case legitimate drift between
// the host's reported position and the client's copy is order ~1000 units.
// 1500 keeps that headroom while refusing anything that is plainly a different
// instance somewhere else in the world.
static constexpr float kMaxBindDist   = 1500.0f;

// entityID -> the client's own real NPC ref we disabled and replaced with a
// proxy. Used to keep that hidden original co-located with its proxy so quest
// aliases/markers, which still reference it, resolve to the right place.
static std::unordered_map<uint32_t, uint32_t> s_entityToDisabledOriginal;
static constexpr float kMaxBindDistSq = kMaxBindDist * kMaxBindDist;

// UPDATE 2026-06-18 (round 23 audit): split into a noinline _Impl plus a thin
// SEH wrapper, same treatment as CoSyncWorldSync.cpp's HostTickObjects fix
// (round 16) -- this function has the identical unguarded pattern that was
// the confirmed root cause there: a raw loop over cell->objectList.entries[i]
// dereferencing ->baseForm/->formType on every entry, with zero SEH
// protection, called every single tick while connected.
__declspec(noinline) static void TickScanWorldActors_Impl(double now)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::TickScanWorldActors_Impl", "Needs a valid local player. Looks through the game's list of currently active actors to find new ones to sync, and falls back to scanning the player's cell if that list isn't available. The caller wraps this in a crash guard.");

    if (now - s_lastScanTime < kScanInterval)
        return;
    s_lastScanTime = now;


    PlayerCharacter* localPlayer = CoSyncGetPlayer();
    if (!localPlayer)
        return;

    static constexpr float kDiscoverRadius   = 16384.0f;   // 4 exterior cells
    static constexpr float kDiscoverRadiusSq = kDiscoverRadius * kDiscoverRadius;
    const NiPoint3 gatePos{ localPlayer->pos.x, localPlayer->pos.y, localPlayer->pos.z };

    std::unordered_set<Actor*> cellActors;

    auto considerRefr = [&](TESObjectREFR* refr)
    {
        if (!refr)
            return;

        if (!refr->baseForm || refr->baseForm->formType != kFormType_NPC_)
            return;

        // DYNAMIC_CAST (__RTDynamicCast) is confirmed broken in this codebase
        // even for provably-correct types (rounds 106-109) -- crashes inside
        // ucrtbase.dll internals. baseForm->formType == kFormType_NPC_ just
        // above already guarantees refr is an Actor by engine contract (an
        // ACHR reference to an NPC base form is always an Actor), so
        // static_cast is safe and avoids the broken RTTI path entirely.
        Actor* actor = static_cast<Actor*>(refr);
        if (actor == static_cast<Actor*>(localPlayer))
            return;

        // 2026-08-16: REMOVED the blanket "actor->baseForm->formID has a
        // dynamic (0xFF-prefixed) formID -> skip" exclusion that used to sit
        // here. Live log proof (Vault 114 + a Gunner/Brahmin camp session):
        // a pack of 3-5 hostile actors sat in the client's own cell the
        // whole fight (0xFF000E1A/E1B/E1C/E1D/E16, visible via
        // ClientBindWorldActor_Impl's cell-scan diag "NPC[i] base=0xFF...")
        // and were NEVER discovered/synced -- this exact check silently
        // threw them all out at considerRefr's first filter, session after
        // session, no matter what else got fixed upstream (bidirectional
        // discovery, cell-scan supplement, tier coverage). No comment/commit
        // ever documented why this existed; it does NOT protect against our
        // own world-NPC puppet clones (those PlaceAtMe-spawn with the real
        // actor's STATIC baseForm, only the REF id is dynamic -- an entirely
        // separate, already-handled case via the isDynamic/reserved0 logic
        // elsewhere in this file). The remote-player-proxy check right below
        // already covers the one clone case that legitimately needs
        // excluding here.
        //
        // Skip CoSync-spawned proxy actors (remote player body form) — these
        // are PlaceAtMe references we own, not real world NPCs. Round 134:
        // match both the esp proxy base and the legacy vanilla raider (old
        // save-persisted proxies still carry 0x20749). Round 137: also skip
        // the previous esp proxy record 0x003599 (save-persisted proxies from
        // before the switch to RMPlayer 0x00A797).
        if (CoSyncProxyLooks::IsProxyBaseFormID(actor->baseForm->formID)
            || actor->baseForm->formID == 0x00020749u
            || (actor->baseForm->formID & 0x00FFFFFFu) == 0x00003599u)
            return;

        // Companions (2026-09-26): the HOST's teammates are its real companions
        // and sync like any host-owned world actor, so clients see them
        // following the host. A client's teammates are either the host's
        // companions inherited through the shared save (CoSyncCompanions
        // hands those back to the host) or the client's own companion copies
        // (registered explicitly with COMPOWN) -- never discovered here.
        if (actor->IsPlayerTeammate() && !CoSyncNet::IsHost())
            return;

        if (refr->flags & TESObjectREFR::kFlag_IsDisabled)
            return;

        // Already-tracked actors (including our own AI-disabled clone-
        // fallback proxies, which use the REAL NPC's base form — the
        // dynamic-base-form check above only excludes the player-proxy
        // base, not these) must be skipped BEFORE IsActorDead, not after.
        // IsActorDead touches actorValueOwner (CoSyncAVSafety::SafeGetValue),
        // which is fatal on any AI-disabled actor per this file's own
        // established CoSyncActorValueSafety.h rule — calling it on an
        // already-registered clone every single scan tick, forever, was
        // producing a real, repeatable, SEH-recovered-but-still-costly
        // crash storm (confirmed 2026-07-21 test logs: same actor pointer,
        // same Health AVI, faulting every ~1s scan indefinitely).
        if (s_actorToEID.count(actor) || g_CoSyncEntities.GetEntityID(actor) != 0)
            return;

        // 2026-08-16: skip the AV-touching dead-check for actors with a
        // dynamic base form. Until today, EVERY such actor was excluded
        // before reaching here (see the now-removed blanket dynamic-baseform
        // filter above -- the comment a few lines up about "the dynamic-
        // base-form check above" refers to it), so this class of actor was
        // NEVER exercised against IsActorDead()/SafeGetValue() before. Live
        // proof it's unsafe: removing that filter produced 9 real
        // [CoSync-VEH] CRASH events in ~19s (READ AV, actor->
        // actorValueOwner's vtable, fault=0xFFFFFFFFFFFFFFFF) on the very
        // next test, ending the client session -- SafeGetValue's own __try/
        // __except recovers each individual fault (per-actor blacklist via
        // s_badHealthActors), but repeating that 9 times in 19s is exactly
        // the "SEH-recovered-but-still-costly crash storm" this file's own
        // established rule warns about, and this time it was costly enough
        // to end the session. Runtime-templated leveled-actor base forms are
        // plausibly shorter-lived / less stable than persistent ESM/ESP
        // actors, so treating "not disabled" (kFlag_IsDisabled, already
        // checked above) as sufficient liveness evidence for this class is
        // a far smaller risk than touching actorValueOwner on them.
        const bool hasDynamicBaseForm = (actor->baseForm->formID & 0xFF000000u) == 0xFF000000u;
        if (!hasDynamicBaseForm && IsActorDead(actor))
            return;

        const float dx = refr->pos.x - gatePos.x;
        const float dy = refr->pos.y - gatePos.y;
        const float dz = refr->pos.z - gatePos.z;
        if (dx * dx + dy * dy + dz * dz > kDiscoverRadiusSq)
            return;

        cellActors.insert(actor);
    };

    bool scannedProcessLists = false;
    size_t foundByProcessLists = 0;
    {
        const uintptr_t pl = *g_processListsPtr;
        if (pl)
        {
            // Walk one ProcessLists actor-handle list (data ptr + count). Each
            // handle is resolved via LookupREFRByHandle, which rejects stale/
            // invalid handles, so a slightly-off offset degrades to "found
            // nothing" rather than crashing (and the whole scan is SEH-wrapped).
            auto walkHandleList = [&](uintptr_t dataOff, uintptr_t countOff)
            {
                uint32_t* handles = *reinterpret_cast<uint32_t* const*>(pl + dataOff);
                const uint32_t n  = *reinterpret_cast<const uint32_t*>(pl + countOff);
                if (!handles || n == 0 || n >= 4096)
                    return;
                scannedProcessLists = true;
                for (uint32_t i = 0; i < n; ++i)
                {
                    NiPointer<TESObjectREFR> refPtr;
                    if (LookupREFRByHandle(handles[i], refPtr) && refPtr.m_pObject)
                        considerRefr(refPtr.m_pObject);
                }
            };
            walkHandleList(kPL_HighHandleData,       kPL_HighHandleCount);
            walkHandleList(kPL_MiddleHighHandleData, kPL_MiddleHighHandleCount);  // NEW: settlers/creatures/idle NPCs live here
        }
    }
    foundByProcessLists = cellActors.size();

    // 2026-08-15: this cell scan used to run ONLY when the ProcessLists walk
    // found nothing at all (pl==0 or both handle lists empty/rejected) --
    // i.e. it was a total-failure fallback, not a supplement. ProcessLists
    // only covers the High + MiddleHigh AI tiers (kPL_HighHandleData/
    // MiddleHighHandleData above); actors the engine is running at lower
    // priority -- MiddleLow/Low, which is exactly where an idle/not-yet-
    // aggroed enemy (a ghoul lying dormant, a raider on guard duty) commonly
    // sits until it notices the player -- were never scanned even though
    // ProcessLists itself was perfectly valid. User-reported symptom: standing
    // around visible ghouls/enemy NPCs, none discovered/synced at all. Now
    // ALWAYS also scans the player's own cell as a supplement (not an
    // either/or) -- considerRefr already skips anything ProcessLists already
    // registered (s_actorToEID/g_CoSyncEntities checks), so this only adds
    // actors the tier walk missed. Only covers the player's current cell
    // (unlike ProcessLists' full kDiscoverRadius reach), so it closes the
    // "right next to me" case rather than the "4 cells away" case.
    TESObjectCELL* cell = localPlayer->parentCell;
    if (cell)
    {
        for (uint32_t i = 0; i < (uint32_t)cell->objectList.count; ++i)
            considerRefr(cell->objectList[i]);
    }

    const size_t foundByCellScan = cellActors.size() - foundByProcessLists;
    if (foundByCellScan > 0)
    {
        LOG_UPDATED("[NpcWorldSync] Nearby scan found %zu actor(s) the main scan missed (mainScanOk=%d)",
            foundByCellScan, scannedProcessLists ? 1 : 0);
    }


    const bool isHost = CoSyncNet::IsHost();

    for (Actor* actor : cellActors)
    {
        if (s_actorToEID.count(actor))
            continue;

        if (g_CoSyncEntities.GetEntityID(actor) != 0)
            continue;

        if (!isHost)
        {
            // Tilted-style bidirectional discovery: a client reports the
            // creatures IT discovers (including dynamic 0xFF encounter spawns)
            // to the host, which arbitrates ownership. Previously 0xFF refs were
            // skipped entirely, so the host never saw the client's creatures --
            // the "only client could see them" gap. The host dedupes 0xFF
            // creatures SPATIALLY (they have no shared formID), so a co-located
            // same-encounter creature the host already owns is matched instead
            // of duplicated. The client's own refFormID (session-relative) is
            // still sent so the OWNER path can bind its real local actor.
            const bool isDynamic = (actor->formID & 0xFF000000u) == 0xFF000000u;

            // Dedup FF reports client-side so we don't re-broadcast the same
            // encounter creature every 1s scan (non-FF refs are cheap for the
            // host to dedup by ref, so those keep the old re-report behaviour).
            if (isDynamic)
            {
                auto rep = s_reportedLocalRefs.find(actor->formID);
                if (rep != s_reportedLocalRefs.end() && NpcNowSeconds() - rep->second < 10.0)
                    continue;
            }

            // Host already told us this placed actor is dead in its world;
            // our local Kill() is queued -- don't keep reporting it alive.
            if (!isDynamic && s_knownDeadRefs.count(actor->formID))
                continue;

            if (s_disabledOriginalRefs.count(actor->formID))
                continue;

            const uint32_t myEid = g_CoSyncPlayerManager.GetLocalEntityID();
            if (myEid == 0)
                continue;

            // 2026-08-16 pt.7: resolve through the static template ancestor
            // instead of sending actor->baseForm->formID raw -- see
            // ResolveCrossMachineBaseFormID's comment. actor->baseForm is
            // guaranteed a TESNPC by considerRefr's formType filter above.
            uint32_t baseFormID = actor->baseForm
                ? ResolveCrossMachineBaseFormID(static_cast<TESNPC*>(actor->baseForm))
                : 0;
            if (baseFormID == 0)
                continue;

            NiPoint3 pos{}, rot{};
            CoSyncGameAPI::GetActorWorldTransform(actor, pos, rot);

            uint32_t locWS = 0xFFFFFFFFu, locCell = 0;
            g_CoSyncPlayerManager.GetEntityLocation(myEid, locWS, locCell);
            char buf[256];
            snprintf(buf, sizeof(buf), "NPCSEEN|%u|%u|%.2f,%.2f,%.2f|%u|%u|%u",
                baseFormID, actor->formID, pos.x, pos.y, pos.z, myEid, locWS, locCell);
            CoSyncTransport::Send(std::string(buf));

            if (isDynamic)
                s_reportedLocalRefs[actor->formID] = NpcNowSeconds();
            continue;
        }

        if ((actor->formID & 0xFF000000u) != 0xFF000000u
            && s_claimedRefToEID.count(actor->formID))
            continue;

        // Position/baseForm must be known BEFORE allocating, so the dynamic-
        // actor spatial dedup below (added 2026-08-12) can run first. Moved up
        // from after AllocateEntityID().
        NiPoint3 spawnPos{};
        NiPoint3 spawnRot{};
        CoSyncGameAPI::GetActorWorldTransform(actor, spawnPos, spawnRot);

        // 2026-08-16 pt.7: resolve through the static template ancestor
        // instead of sending actor->baseForm->formID raw -- see
        // ResolveCrossMachineBaseFormID's comment. actor->baseForm is
        // guaranteed a TESNPC by considerRefr's formType filter above.
        uint32_t baseFormID = actor->baseForm
            ? ResolveCrossMachineBaseFormID(static_cast<TESNPC*>(actor->baseForm))
            : 0;
        if (baseFormID == 0)
        {
            LOG_UPDATED_WARN("[NpcWorldSync] Actor %p has no usable base form to sync from — skipping", static_cast<void*>(actor));
            continue;
        }

        // 2026-08-12: this is the host's OWN direct-scan discovery of a world
        // actor -- unlike OnReceiveNpcSeen_Impl (STAGE2, a client's report of
        // its own local discovery), this path never checked s_spatialClaims
        // before registering. For a dynamic (0xFF ref) creature that's a real
        // duplicate: if a client already reported this same physical encounter
        // creature (spatially co-located, same base form) and the host claimed
        // it via STAGE2, the host's own scan discovering the identical
        // creature in ITS OWN world a moment later had nothing stopping it
        // from registering a SECOND, rival, host-authoritative entity for it
        // -- fully AI-driven, unsuppressed, un-puppeted, running its own local
        // combat/movement logic. That's the observed "spawned a double NPC and
        // it just walked off, but there was the one that was mirrored" -- the
        // mirrored one is the STAGE2-owned entity; the one that walked off is
        // this path's redundant registration of the same creature. Only
        // dynamic refs need this: static (CK-placed) refs already dedupe by
        // ref via s_claimedRefToEID above, which is exact and cannot false-
        // positive the way a spatial quantization could for two genuinely
        // distinct static NPCs.
        const bool isDynamicActor = (actor->formID & 0xFF000000u) == 0xFF000000u;
        if (isDynamicActor)
        {
            const uint64_t skey = MakeSpatialKey(baseFormID, spawnPos);
            if (s_spatialClaims.count(skey))
            {
                LOG_UPDATED("[NpcWorldSync] Skipping actor %p (base=0x%08X) — another player already claimed this spot, avoiding a duplicate",
                    static_cast<void*>(actor), baseFormID);
                continue;
            }
        }

        // Just handed to a player who is near it (HostTickOwnershipHandoff):
        // the host's wide scan must not grab it straight back.
        if (IsHandoffHeld(actor->formID, baseFormID, spawnPos))
            continue;

        uint32_t eid = AllocateEntityID();
        if (eid == 0)
        {
            LOG_UPDATED_WARN("[NpcWorldSync] Couldn't get an ID for actor %p — skipping it", static_cast<void*>(actor));
            continue;
        }

        s_actorToEID[actor] = eid;
        s_eidToActor[eid]   = actor;

        CoSyncPlayer& proxy = g_CoSyncPlayerManager.GetOrCreateByEntityID(eid);
        proxy.entityID    = eid;
        proxy.actorRef    = actor;
        proxy.hasSpawned  = true;
        proxy.isLocalPlayer = false;
        proxy.createType  = CoSyncEntityType::NPC;
        proxy.username    = "";

        CoSyncEntityState& state = g_CoSyncPlayerManager.GetOrCreateState(eid);
        state.entityID          = eid;
        state.hasCreate         = true;
        state.isNPC             = true;
        state.hostAuthoritative = true;
        state.isLocallyOwned    = true;

        EntityCreatePacket cp{};
        cp.entityID      = eid;
        cp.type          = CoSyncEntityType::NPC;
        cp.baseFormID    = baseFormID;
        cp.ownerEntityID = 0;
        cp.spawnFlags    = EntityCreatePacket::RemoteControlled;
        cp.spawnPos      = spawnPos;
        cp.spawnRot      = spawnRot;
        cp.reserved0     = ((actor->formID & 0xFF000000u) == 0xFF000000u) ? 0u : actor->formID;
        g_CoSyncPlayerManager.GetEntityLocation(g_CoSyncPlayerManager.GetLocalEntityID(), cp.locWS, cp.locCell);

        state.lastCreate = cp;

        if (cp.reserved0 != 0)
            s_claimedRefToEID[cp.reserved0] = eid;

        // Register the spatial claim so a client's later CSEEN for the SAME
        // co-located encounter creature dedupes against this host-owned entity
        // instead of allocating a rival one (Tilted-style spatial ownership).
        {
            const uint64_t skey = MakeSpatialKey(baseFormID, spawnPos);
            s_spatialClaims[skey]       = eid;
            s_entityToSpatialKey[eid]   = skey;
        }


        g_CoSyncEntities.Register(eid, actor);


        CoSyncTransport::Send(SerializeEntityCreate(cp));

        const uint32_t raceFID = SafeGetRaceFormID(actor);
        s_raceRegistrations[raceFID]++;

        LOG_INFO("[NpcWorldSync] Now syncing actor %p as entity=%u base=0x%08X race=0x%08X pos=(%.1f,%.1f,%.1f)",
            static_cast<void*>(actor), eid, baseFormID, raceFID,
            spawnPos.x, spawnPos.y, spawnPos.z);
    }


    // HP + legendary rank — broadcast every scan interval for all registered NPCs
    ActorValueInfo* healthAVI = GetHealthAVICached();
    ActorValueInfo* legAVI    = GetLegendaryAVICached();
    if (healthAVI)
    {
        for (auto& kv : s_eidToActor)
        {
            uint32_t eid   = kv.first;
            Actor*   actor = kv.second;
            if (!actor)
                continue;
            if (s_badHealthActors.count(actor))
                continue;

            // round 30: routed through CoSyncAVSafety -- see
            // CoSyncActorValueSafety.h (direct actorValueOwner touches on
            // these AI-disabled proxy NPCs can crash at Fallout4.exe+0xCC934D).
            // Permanent blacklist on first fault -- this loop was the one
            // place in the codebase still missing it (confirmed live via a
            // 2026-07-21 crash-storm test: the SAME actor faulted on every
            // single scan tick, indefinitely, without one).
            float curHP = 0.f, maxHP = 0.f;
            const bool gotValue = CoSyncAVSafety::SafeGetValue(actor, healthAVI, curHP);
            const bool gotMax   = CoSyncAVSafety::SafeGetMaximum(actor, healthAVI, maxHP);
            if (!gotValue || !gotMax)
            {
                s_badHealthActors.insert(actor);
                LOG_UPDATED_WARN("[NpcWorldSync] entity=%u actor=%p couldn't read health — won't check its HP again this session",
                    eid, static_cast<void*>(actor));
                continue;
            }

            auto hpIt = s_lastNpcHP.find(eid);

            // Death: an actor can be dead/dying while its Health AV still reads
            // slightly above 0, and it can leave the process-list scan before HP
            // reaches 0 -- so poll IsActorDead explicitly instead of relying only
            // on HP<=0 (FalloutTogether polls IsDead every 250ms). Send KILL once
            // on the alive->dead transition so the puppet dies on every client.
            const bool wasAlive = (hpIt == s_lastNpcHP.end() || hpIt->second > 0.f);
            const bool nowDead  = (curHP <= 0.f) || IsActorDead(actor);
            if (nowDead && wasAlive)
            {
                CoSyncTransport::Send(CoSyncHealthSync::SerializeKill(eid));
                s_deadEntities.insert(eid);
                LOG_UPDATED("[NpcWorldSync] entity=%u died (hp=%.0f) — sent death update", eid, curHP);
                s_lastNpcHP[eid] = 0.f;
            }
            else if (!nowDead && (hpIt == s_lastNpcHP.end() || fabsf(curHP - hpIt->second) >= 1.0f))
            {
                CoSyncTransport::Send(CoSyncHealthSync::SerializeHP(eid, curHP, maxHP));

                s_lastNpcHP[eid] = curHP;
            }

            // Legendary rank: broadcast whenever it changes so the client
            // applies the same rank the host's engine assigned at spawn.
            if (legAVI)
            {
                float legVal = 0.f;
                CoSyncAVSafety::SafeGetValue(actor, legAVI, legVal);
                const int legRank = static_cast<int>(legVal);
                auto legIt = s_lastNpcLegendary.find(eid);
                if (legIt == s_lastNpcLegendary.end() || legIt->second != legRank)
                {
                    s_lastNpcLegendary[eid] = legRank;
                    if (legRank > 0)
                    {
                        std::string pkt = "LGD|";
                        pkt += std::to_string(eid);
                        pkt += "|";
                        pkt += std::to_string(legRank);
                        CoSyncTransport::Send(pkt);
                        LOG_UPDATED("[NpcWorldSync] Sending legendary rank: entity=%u rank=%d actor=%p",
                            eid, legRank, static_cast<void*>(actor));
                    }
                }
            }
        }
    }

    // Periodic CREATE re-broadcast. A client that hadn't finished loading the
    // cell when the one-shot CREATE arrived could never bind that NPC (its
    // ClientBindWorldActor found no match, and nothing ever retried) — the #1
    // way "NPC world sync" silently did nothing. Re-sending is cheap and the
    // client ignores CREATEs for entities it already bound. spawnPos is
    // refreshed so late binders match by proximity to where the NPC is NOW.
    {
        static double s_lastCreateRebroadcast = -999.0;
        static constexpr double kCreateRebroadcastInterval = 5.0;
        if (now - s_lastCreateRebroadcast >= kCreateRebroadcastInterval)
        {
            s_lastCreateRebroadcast = now;
            uint32_t sentCount = 0;
            for (auto& kv : s_eidToActor)
            {
                Actor* actor = kv.second;
                if (!actor)
                    continue;

                CoSyncEntityState& st = g_CoSyncPlayerManager.GetOrCreateState(kv.first);
                if (!st.hasCreate)
                    continue;

                NiPoint3 curPos{}, curRot{};
                if (CoSyncGameAPI::GetActorWorldTransform(actor, curPos, curRot))
                {
                    st.lastCreate.spawnPos = curPos;
                    st.lastCreate.spawnRot = curRot;
                }

                CoSyncTransport::Send(SerializeEntityCreate(st.lastCreate));
                ++sentCount;

                // Late joiners must see corpses as corpses, not idle clones.
                if (s_deadEntities.count(kv.first))
                    CoSyncTransport::Send(CoSyncHealthSync::SerializeKill(kv.first));
            }
            if (sentCount > 0)
                LOG_INFO("[NpcWorldSync] Resent spawn info for %u world actors", sentCount);
        }
    }

    // Departure: the old rule ("not in the player's current cell this scan")
    // dropped every synced NPC the moment the host crossed an exterior cell
    // boundary, even though those NPCs were still loaded and visible one cell
    // over. Keep an out-of-cell NPC registered while it is still loaded
    // (parentCell non-null), alive, and within the loaded-grid keep radius.
    // Must stay ABOVE kDiscoverRadius or entities churn (register/destroy
    // loops) at the discovery boundary.
    static constexpr float kKeepRadius   = 20480.0f;   // 5 exterior cells
    static constexpr float kKeepRadiusSq = kKeepRadius * kKeepRadius;

    NiPoint3 playerPos{ localPlayer->pos.x, localPlayer->pos.y, localPlayer->pos.z };

    // 2026-07-21: user reported NPCs never unloading across a cell-area
    // change (entity counts observed ballooning to 100+ over a session),
    // plus the remote player proxy/HP-AP overlay vanishing on an ext->int
    // transition. Root cause: this distance check compared raw positions
    // with no worldspace/cell-identity check at all. Interior cells use
    // their own small, arbitrary local coordinates unrelated to the
    // exterior worldspace grid -- crossing ext<->int (or between two
    // different interiors) makes playerPos and an old actor's cached
    // aPos numerically incomparable. Depending on pure coincidence, the
    // squared-distance could land under kKeepRadiusSq (actor never
    // recognized as departed -> never cleaned up -> re-discovered fresh
    // next time that area is re-entered -> duplicate spawn) or wildly over
    // it. Now requires matching worldspace (or, for two interiors -- both
    // report a null worldspace -- the exact same parentCell) before trusting
    // the distance number at all; any actor whose context doesn't match the
    // player's current one is treated as departed immediately, regardless
    // of what the raw coordinates say.
    TESObjectCELL* playerCell = localPlayer->parentCell;
    TESWorldSpace* playerWS   = CALL_MEMBER_FN(reinterpret_cast<TESObjectREFR*>(localPlayer), GetWorldspace)();

    std::vector<Actor*> departed;
    for (auto& kv : s_actorToEID)
    {
        Actor* regActor = kv.first;
        if (cellActors.find(regActor) != cellActors.end())
            continue;

        bool keep = false;
        // 2026-08-16: same dynamic-baseform IsActorDead skip as considerRefr
        // above -- this departure sweep touches every currently-registered
        // actor once per scan tick, so it hit the exact same crash class.
        // 2026-09-05: dead actors are no longer treated as departed. A KILL
        // followed by an immediate DESTROY made the client despawn the proxy
        // corpse and re-enable its own (alive) original. Corpses now stay
        // registered until they actually unload / leave the keep radius, and
        // the DESTROY then carries kDestroyReason_Dead. This also removes the
        // actorValueOwner touch from the departure sweep entirely.
        if (regActor && regActor->parentCell)
        {
            TESWorldSpace* actorWS = CALL_MEMBER_FN(static_cast<TESObjectREFR*>(regActor), GetWorldspace)();
            const bool sameContext = (playerWS && actorWS && playerWS == actorWS) ||
                                      (!playerWS && !actorWS && regActor->parentCell == playerCell);

            if (sameContext)
            {
                NiPoint3 aPos{}, aRot{};
                if (CoSyncGameAPI::GetActorWorldTransform(regActor, aPos, aRot))
                {
                    const float dx = aPos.x - playerPos.x;
                    const float dy = aPos.y - playerPos.y;
                    const float dz = aPos.z - playerPos.z;
                    keep = (dx * dx + dy * dy + dz * dz) < kKeepRadiusSq;
                }
            }
        }

        if (!keep)
            departed.push_back(regActor);
    }

    for (Actor* actor : departed)
    {
        auto it = s_actorToEID.find(actor);
        if (it == s_actorToEID.end())
            continue;

        uint32_t eid = it->second;


        const bool wasDead = s_deadEntities.count(eid) != 0;

        EntityDestroyPacket dp{};
        dp.entityID    = eid;
        dp.reasonFlags = wasDead ? kDestroyReason_Dead : 0u;
        CoSyncTransport::Send(SerializeEntityDestroy(dp));


        g_CoSyncEntities.Remove(eid);

        s_actorToEID.erase(it);
        s_eidToActor.erase(eid);
        s_lastNpcHP.erase(eid);
        s_lastNpcLegendary.erase(eid);
        s_deadEntities.erase(eid);
        ReleaseClaimForEntity(eid);
        g_CoSyncPlayerManager.UnregisterEntityIDOnly(eid);

        LOG_UPDATED("[NpcWorldSync] Actor %p (entity=%u) left the area — removed",
            static_cast<void*>(actor), eid);
    }
}

void TickScanWorldActors(double now)
{
    if (!kEnemyCreatureSyncEnabled)
    {
        LogEnemyCreatureSyncDisabledOnce();
        return;
    }

    LOG_REQUIREMENT("CoSyncNpcWorldSync::TickScanWorldActors", "No requirements beyond being called each tick. Wraps the scan in a crash guard so a problem here doesn't crash the game.");
    // The character editor stages the player in Vault 111's cryo room -- not
    // part of the session. Scanning there claimed Vault 111's NPCs (0.3.0 host
    // log 16:50:18; also the old W1 "host NPCs disabled" bug).
    if (CoSyncProfiles::IsEditorBusy())
        return;
    __try
    {
        TickScanWorldActors_Impl(now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[NpcWorldSync] World-actor scan hit a problem — caught it and skipping this pass");
    }

    // Outside the __try: pure bookkeeping, and it must still report even on a
    // scan that faulted (that is exactly when knowing which races were in play
    // is most useful).
    LogRaceDiag(now);
}

__declspec(noinline) static void TickSuppressLocalDynamicSpawns_Impl(double now)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::TickSuppressLocalDynamicSpawns_Impl", "Needs a valid local player who's in a valid cell. Goes through everything in that cell, so the cell and its contents need to stay valid for the whole scan.");

    if (now - s_lastSuppressScanTime < kScanInterval)
        return;
    s_lastSuppressScanTime = now;

    PlayerCharacter* localPlayer = CoSyncGetPlayer();
    if (!localPlayer)
        return;

    TESObjectCELL* cell = localPlayer->parentCell;
    if (!cell)
        return;

    uint32_t hostCellFormID = 0;
    uint32_t hostWSID       = 0xFFFFFFFFu;
    Actor*   hostActorRef   = nullptr;
    bool     haveHost       = false;
    {
        const uint32_t localEID = g_CoSyncPlayerManager.GetLocalEntityID();
        for (auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
        {
            CoSyncPlayer& pl = *kv.second;
            if (pl.entityID == 0 || pl.entityID == localEID)
                continue;

            auto it = g_CoSyncPlayerManager.GetStates().find(pl.entityID);
            if (it == g_CoSyncPlayerManager.GetStates().end() || !it->second.hasCreate)
                continue;

            if (it->second.lastCreate.type != CoSyncEntityType::Player)
                continue;

            hostCellFormID = pl.lastRemoteCellFormID;
            hostWSID       = pl.lastRemoteWorldspaceID;
            hostActorRef   = pl.actorRef;
            haveHost        = true;
            break;
        }
    }

    if (!haveHost)
        return;

    // 2026-08-15: was an exact-cell-match gate (cell->formID != hostCellFormID
    // -> return), so this suppress pass only ever ran while standing in the
    // literal same cell as the host. In a large EXTERIOR worldspace the host
    // and client routinely stream/keep creatures across several exterior
    // cells apart (see kDiscoverRadius/kKeepRadius below) without ever
    // sharing a cell formID, so a client's own duplicate dynamic spawn there
    // was never suppressed -- the flagged-but-unclosed "exterior worldspace
    // double-spawn" gap. Interiors still require an exact cell match (two
    // different interior cells both report worldspace==0, so a formID/WS
    // comparison alone can't tell them apart -- same convention as
    // TickScanWorldActors_Impl's departure check above).
    TESWorldSpace* myWS = CALL_MEMBER_FN(reinterpret_cast<TESObjectREFR*>(localPlayer), GetWorldspace)();
    const uint32_t myWSID = myWS ? reinterpret_cast<TESForm*>(myWS)->formID : 0u;

    const bool bothInterior = (hostWSID == 0u) && (myWSID == 0u);
    if (bothInterior)
    {
        if (hostCellFormID == 0 || cell->formID != hostCellFormID)
            return;
    }
    else
    {
        if (hostWSID == 0xFFFFFFFFu || hostWSID != myWSID)
            return;

        // Same worldspace isn't enough on its own -- only suppress when the
        // host is actually close enough that its own puppet of this creature
        // is plausible (matches the discovery radius TickScanWorldActors_Impl
        // reports new dynamic creatures within, so "host could see this too"
        // and "client reported this to host" cover the same area).
        static constexpr float kSuppressRadius   = 16384.0f;
        static constexpr float kSuppressRadiusSq = kSuppressRadius * kSuppressRadius;
        if (!hostActorRef)
            return;
        const float hdx = hostActorRef->pos.x - localPlayer->pos.x;
        const float hdy = hostActorRef->pos.y - localPlayer->pos.y;
        const float hdz = hostActorRef->pos.z - localPlayer->pos.z;
        if (hdx * hdx + hdy * hdy + hdz * hdz > kSuppressRadiusSq)
            return;
    }

    std::unordered_set<Actor*> currentDynamicActors;

    for (uint32_t i = 0; i < (uint32_t)cell->objectList.count; ++i)
    {
        TESObjectREFR* refr = cell->objectList[i];
        if (!refr)
            continue;

        if (!refr->baseForm || refr->baseForm->formType != kFormType_NPC_)
            continue;

        Actor* actor = static_cast<Actor*>(refr);
        if (actor == static_cast<Actor*>(localPlayer))
            continue;

        if ((actor->formID & 0xFF000000u) != 0xFF000000u)
            continue;

        // Already hidden by a previous suppress pass. DespawnProxyActor's
        // Disable()+Delete() is queued on the Papyrus VM and the engine does
        // NOT always Delete a native encounter creature -- it stays in the
        // cell objectList as a disabled ref. Without this skip the same actor
        // was re-"suppressed" every 2s for the whole session (live log
        // 2026-08-07: one actor spammed ~90 times). A disabled ref is already
        // invisible/inert, so leave it alone.
        if (refr->flags & TESObjectREFR::kFlag_IsDisabled)
            continue;

        if (g_CoSyncEntities.GetEntityID(actor) != 0)
            continue;

        if (actor->IsPlayerTeammate())
            continue;

        if (IsActorDead(actor))
            continue;

        TESNPC* npc = static_cast<TESNPC*>(actor->baseForm);
        constexpr uint32_t kUnsafeFlags = TESActorBaseData::kFlagEssential
            | TESActorBaseData::kFlagProtected
            | TESActorBaseData::kFlagUnique;
        if (npc->actorData.flags & kUnsafeFlags)
            continue;

        // One despawn attempt per ref, ever -- the disabled-flag skip above
        // handles the common case, but Delete can also silently no-op leaving
        // the ref enabled; this stops any residual re-hit loop regardless.
        static std::unordered_set<uint32_t> s_suppressAttempted;
        if (s_suppressAttempted.count(refr->formID))
            continue;

        currentDynamicActors.insert(actor);

        int& ticksSeen = s_dynamicSpawnTicksSeen[actor];
        ++ticksSeen;
        if (ticksSeen < 2)
            continue;

        s_suppressAttempted.insert(refr->formID);
        CoSyncGameAPI::DespawnProxyActor(actor);
        // bothInterior tag distinguishes the original exact-cell path from
        // the 2026-08-15 worldspace+proximity path -- next live test in an
        // exterior worldspace should show suppressed=1 entries with ws=1
        // where previously (exact-cell gate) nothing fired at all.
        LOG_UPDATED("[NpcWorldSync] Blocked a duplicate spawn: actor=%p base=0x%08X ws=%d hostCell=0x%08X myCell=0x%08X",
            static_cast<void*>(actor), actor->baseForm->formID, bothInterior ? 0 : 1, hostCellFormID, cell->formID);
        s_dynamicSpawnTicksSeen.erase(actor);
    }

    for (auto it = s_dynamicSpawnTicksSeen.begin(); it != s_dynamicSpawnTicksSeen.end(); )
    {
        if (currentDynamicActors.find(it->first) == currentDynamicActors.end())
            it = s_dynamicSpawnTicksSeen.erase(it);
        else
            ++it;
    }
}

void TickSuppressLocalDynamicSpawns(double now)
{
    if (!kEnemyCreatureSyncEnabled)
    {
        LogEnemyCreatureSyncDisabledOnce();
        return;
    }

    LOG_REQUIREMENT("CoSyncNpcWorldSync::TickSuppressLocalDynamicSpawns", "No requirements beyond being called each tick. Wraps the scan in a crash guard so a problem here doesn't crash the game.");
    __try
    {
        TickSuppressLocalDynamicSpawns_Impl(now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[NpcWorldSync] Duplicate-spawn check hit a problem — caught it and skipping this pass");
    }
}

bool IsNpcSeen(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::IsNpcSeen", "Doesn't touch the game — just checks the start of a text message.");
    return msg.rfind("NPCSEEN|", 0) == 0;
}

__declspec(noinline) static void OnReceiveNpcSeen_Impl(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::OnReceiveNpcSeen_Impl", "Host-only. Needs a properly formatted NPC-sighting message from a client. Creates a new synced entity for a newly-reported actor, or resends the existing spawn info if it's already known.");
    if (!CoSyncNet::IsHost())
        return;

    uint32_t baseFormID = 0, refFormID = 0, reporterEid = 0;
    NiPoint3 pos{};

    const char* s = msg.c_str() + 8; // skip "NPCSEEN|"
    char* e = nullptr;
    baseFormID = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return; s = e + 1;
    refFormID  = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return; s = e + 1;
    if (std::sscanf(s, "%f,%f,%f", &pos.x, &pos.y, &pos.z) != 3) return;
    const char* bar = std::strchr(s, '|');
    if (!bar) return;
    char* after = nullptr;
    reporterEid = static_cast<uint32_t>(std::strtoul(bar + 1, &after, 10));
    // Location of the sighting (0.3.1). Required: a sighting without one is
    // exactly what let Vault 111's cryopod NPCs, seen from the character
    // editor room, be created at interior coordinates in the host's world.
    uint32_t locWS = 0xFFFFFFFFu, locCell = 0;
    if (!after || *after != '|') return;
    locWS = static_cast<uint32_t>(std::strtoul(after + 1, &after, 10));
    if (!after || *after != '|') return;
    locCell = static_cast<uint32_t>(std::strtoul(after + 1, nullptr, 10));

    if (baseFormID == 0 || reporterEid == 0 || refFormID == 0 || locWS == 0xFFFFFFFFu)
        return;
    // NOTE: 0xFF (dynamic) refs are now ACCEPTED. They are session-relative to
    // the reporting client, so they can't be deduped by ref across machines --
    // the spatial claim below handles that instead. (Tilted-style bidirectional
    // creature ownership: the client reports its own encounter creatures too.)
    const bool isDynamic = (refFormID & 0xFF000000u) == 0xFF000000u;

    const double now = NpcNowSeconds();

    // 2026-09-05: the reporter sees this PLACED actor alive. If it is dead in
    // the host's world (killed earlier, or a pre-placed corpse), the client
    // must kill its copy -- the old flow allocated a client-owned live entity
    // for it and the host disabled its own corpse and spawned an idle clone in
    // its place ("pre-dead enemies come back to life and just stand there").
    if (!isDynamic)
    {
        TESForm* localForm = LookupFormByID(refFormID);
        Actor* localActor = (localForm && localForm->formType == kFormType_ACHR)
            ? static_cast<Actor*>(localForm) : nullptr;
        const bool localHasDynamicBase = localActor && localActor->baseForm
            && (localActor->baseForm->formID & 0xFF000000u) == 0xFF000000u;
        if (localActor && !localHasDynamicBase && IsActorDead(localActor))
        {
            double& deadResendAt = s_npcDeadSentAt[refFormID];
            if (now >= deadResendAt)
            {
                deadResendAt = now + 5.0;
                char buf[64];
                snprintf(buf, sizeof(buf), "NPCDEAD|%u", refFormID);
                CoSyncTransport::Send(std::string(buf));
                LOG_UPDATED("[NpcWorldSync] NPCSEEN ref=0x%08X base=0x%08X from entity=%u is DEAD here -> sent NPCDEAD",
                    refFormID, baseFormID, reporterEid);
            }
            return;
        }
    }

    // ── Dedup path 1: exact ref already claimed (shared static formID) ───────
    // 2026-08-12, TTD-verified against a live client trace: this used to
    // resend stIt->second.lastCreate completely unmodified -- that struct's
    // spawnPos is only ever set once, at initial allocation below, and was
    // never refreshed. Because a client's own local cell scan re-reports an
    // already-known creature every cycle, this path fired repeatedly for the
    // whole session (observed ~every 5-6s for a single creature), each time
    // re-sending the SAME ancient discovery-time position. On the receiving
    // side that meant ClientBindWorldActor kept re-running against a frozen
    // spawn point instead of the creature's real, current position -- the
    // exact mechanism behind the "combat desync / enemies failing to sync"
    // reports. `pos` here is this report's current position, so refresh the
    // stored CREATE with it before resending.
    auto claimed = s_claimedRefToEID.find(refFormID);
    if (!isDynamic && claimed != s_claimedRefToEID.end())
    {
        const uint32_t eid = claimed->second;
        double& resendAt = s_npcSeenResendAt[refFormID];
        if (now < resendAt)
            return;
        resendAt = now + 2.0;
        auto stIt = g_CoSyncPlayerManager.GetStates().find(eid);
        if (stIt != g_CoSyncPlayerManager.GetStates().end() && stIt->second.hasCreate)
        {
            // GetStates() is const -- GetOrCreateState() gets a writable
            // reference to the SAME already-existing entry (eid is known
            // present from the lookup above, so this can't allocate a new
            // one) so spawnPos can actually be refreshed before resending.
            CoSyncEntityState& mutState = g_CoSyncPlayerManager.GetOrCreateState(eid);
            mutState.lastCreate.spawnPos = pos;
            CoSyncTransport::Send(SerializeEntityCreate(mutState.lastCreate));
        }
        return;
    }

    // ── Dedup path 2: spatial claim (co-located same-encounter creature) ─────
    // Works for both dynamic and static: if this creature's (base, quantized
    // position) is already owned by an entity, the reporter is a NON-owner --
    // resend that entity's CREATE (throttled) so the reporter puppets it and
    // suppresses its own local copy, instead of spawning a rival. Same stale-
    // position fix as path 1 above -- refresh spawnPos from this report before
    // resending instead of replaying the entity's original discovery-time
    // position forever.
    const uint64_t skey = MakeSpatialKey(baseFormID, pos);
    auto sclaim = s_spatialClaims.find(skey);
    if (sclaim != s_spatialClaims.end())
    {
        const uint32_t eid = sclaim->second;
        double& resendAt = s_npcSeenResendAt[refFormID];
        if (now < resendAt)
            return;
        resendAt = now + 2.0;
        auto stIt = g_CoSyncPlayerManager.GetStates().find(eid);
        if (stIt != g_CoSyncPlayerManager.GetStates().end() && stIt->second.hasCreate)
        {
            CoSyncEntityState& mutState = g_CoSyncPlayerManager.GetOrCreateState(eid);
            mutState.lastCreate.spawnPos = pos;
            CoSyncTransport::Send(SerializeEntityCreate(mutState.lastCreate));
        }
        return;
    }

    // ── New creature: the reporter becomes its owner ─────────────────────────
    uint32_t eid = AllocateEntityID();
    if (eid == 0)
        return;

    EntityCreatePacket cp{};
    cp.entityID      = eid;
    cp.type          = CoSyncEntityType::NPC;
    cp.baseFormID    = baseFormID;
    cp.ownerEntityID = reporterEid;
    cp.spawnFlags    = EntityCreatePacket::RemoteControlled;
    cp.spawnPos      = pos;
    cp.spawnRot      = NiPoint3(0.f, 0.f, 0.f);
    cp.reserved0     = refFormID;
    cp.locWS         = locWS;
    cp.locCell       = locCell;

    CoSyncEntityState& state = g_CoSyncPlayerManager.GetOrCreateState(eid);
    state.entityID          = eid;
    state.hasCreate         = true;
    state.isNPC             = true;
    state.hostAuthoritative = false;
    state.isLocallyOwned    = false;
    state.lastCreate        = cp;

    // Register both claim indexes. The ref index is only meaningful for shared
    // static formIDs (a dynamic ref would collide between two clients that each
    // happen to hold the same 0xFF slot), so only static refs go in it; the
    // spatial claim is the authority for dynamic creatures.
    if (!isDynamic)
        s_claimedRefToEID[refFormID] = eid;
    s_spatialClaims[skey]      = eid;
    s_entityToSpatialKey[eid]  = skey;

    CoSyncTransport::Send(SerializeEntityCreate(cp));

    LOG_UPDATED("[NpcWorldSync] Created entity=%u owner=%u base=0x%08X ref=0x%08X%s pos=(%.1f,%.1f,%.1f)",
        eid, reporterEid, baseFormID, refFormID, isDynamic ? " (dynamic)" : "", pos.x, pos.y, pos.z);

    // Host puppets/binds it locally too. For a client-owned dynamic creature the
    // host can't resolve the client's session-relative ref, so ClientBindWorldActor
    // routes it to the non-owner puppet-clone path (owner != host).
    ClientBindWorldActor(cp);
}

// Host: an actor a client OWNS outright -- its personal companion copy. Unlike
// NPCSEEN there is no ref/spatial de-duplication: the host (and other players)
// may have the same companion standing right there, and that is allowed.
uint32_t HostRegisterClientOwnedActor(uint32_t baseFormID, uint32_t refFormID, const NiPoint3& pos, uint32_t ownerEid)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::HostRegisterClientOwnedActor", "Host only, main thread (transport inbox).");
    if (!CoSyncNet::IsHost() || !baseFormID || !refFormID || !ownerEid)
        return 0;
    const uint32_t eid = AllocateEntityID();
    if (eid == 0)
        return 0;

    EntityCreatePacket cp{};
    cp.entityID      = eid;
    cp.type          = CoSyncEntityType::NPC;
    cp.baseFormID    = baseFormID;
    cp.ownerEntityID = ownerEid;
    cp.spawnFlags    = EntityCreatePacket::RemoteControlled;
    cp.spawnPos      = pos;
    cp.spawnRot      = NiPoint3(0.f, 0.f, 0.f);
    cp.reserved0     = refFormID;

    CoSyncEntityState& state = g_CoSyncPlayerManager.GetOrCreateState(eid);
    state.entityID          = eid;
    state.hasCreate         = true;
    state.isNPC             = true;
    state.hostAuthoritative = false;
    state.isLocallyOwned    = false;
    state.lastCreate        = cp;

    CoSyncTransport::Send(SerializeEntityCreate(cp));
    LOG_UPDATED("[NpcWorldSync] Companion entity=%u owned by %u base=0x%08X ref=0x%08X",
        eid, ownerEid, baseFormID, refFormID);

    // The owner links its real actor; everyone else (the host included) gets a
    // puppet that follows the owner's stream.
    ClientBindWorldActor(cp);
    return eid;
}

void OnReceiveNpcSeen(const std::string& msg)
{
    if (!kEnemyCreatureSyncEnabled)
    {
        LogEnemyCreatureSyncDisabledOnce();
        return;
    }

    LOG_REQUIREMENT("CoSyncNpcWorldSync::OnReceiveNpcSeen", "No requirements beyond being called when a message arrives. Wraps the handling in a crash guard.");
    __try
    {
        OnReceiveNpcSeen_Impl(msg);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[NpcWorldSync] Handling an NPC-sighting report hit a problem — caught it and skipping this one");
    }
}

bool IsNpcDead(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::IsNpcDead", "No FO4 engine state involved -- pure string prefix check.");
    return msg.rfind("NPCDEAD|", 0) == 0;
}

void OnReceiveNpcDead(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::OnReceiveNpcDead", "Pure parsing on the network thread; the actor is touched later in FlushPendingNpcDeadOps on the game thread. Host ignores it (host is the death authority).");
    if (CoSyncNet::IsHost())
        return;
    const uint32_t refFormID = static_cast<uint32_t>(std::strtoul(msg.c_str() + 8, nullptr, 10));
    if (refFormID == 0 || (refFormID & 0xFF000000u) == 0xFF000000u)
        return;
    std::lock_guard<std::mutex> lk(s_npcDeadMutex);
    s_pendingNpcDead.push_back(refFormID);
}

__declspec(noinline) static void ApplyNpcDead_Impl(uint32_t refFormID)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::ApplyNpcDead_Impl", "Game thread only; resolves the placed ref via LookupFormByID and queues Actor.Kill() through the Papyrus VM when it is a live non-player ACHR. Caller wraps in SEH.");
    s_knownDeadRefs.insert(refFormID);

    TESForm* form = LookupFormByID(refFormID);
    Actor* actor = (form && form->formType == kFormType_ACHR) ? static_cast<Actor*>(form) : nullptr;
    if (!actor)
        return;

    PlayerCharacter* lp = CoSyncGetPlayer();
    if (lp && actor == static_cast<Actor*>(lp))
        return;

    const bool dynamicBase = actor->baseForm && (actor->baseForm->formID & 0xFF000000u) == 0xFF000000u;
    if (!dynamicBase && IsActorDead(actor))
        return;

    static const BSFixedString kKill("Kill");
    VMArray<VMVariable> emptyArgs;
    CallFunctionNoWait(actor, kKill, emptyArgs);
    LOG_UPDATED("[NpcWorldSync] NPCDEAD ref=0x%08X base=0x%08X -> Kill() queued on the local copy",
        refFormID, actor->baseForm ? actor->baseForm->formID : 0u);
}

__declspec(noinline) static void ApplyNpcDeadSafe(uint32_t refFormID)
{
    __try { ApplyNpcDead_Impl(refFormID); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[NpcWorldSync] ApplyNpcDead faulted for ref=0x%08X -- caught", refFormID);
    }
}

void FlushPendingNpcDeadOps()
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::FlushPendingNpcDeadOps", "Game thread only; drains the NPCDEAD queue and applies each via the SEH-wrapped ApplyNpcDeadSafe.");
    std::vector<uint32_t> ops;
    {
        std::lock_guard<std::mutex> lk(s_npcDeadMutex);
        if (s_pendingNpcDead.empty())
            return;
        ops.swap(s_pendingNpcDead);
    }
    for (uint32_t ref : ops)
        ApplyNpcDeadSafe(ref);
}

void MarkEntityKilled(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::MarkEntityKilled", "No FO4 engine state involved -- bookkeeping only.");
    s_killedEntities.insert(entityID);
}

bool IsEntityDead(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::IsEntityDead", "No FO4 engine state involved -- set lookups only.");
    return s_deadEntities.count(entityID) != 0 || s_killedEntities.count(entityID) != 0;
}

uint32_t GetEntityIDForActor(Actor* actor)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::GetEntityIDForActor", "Safe to call with any actor, even null or unknown — just does a lookup and returns 0 if not found.");
    auto it = s_actorToEID.find(actor);
    return (it != s_actorToEID.end()) ? it->second : 0u;
}

Actor* FindClosestRegisteredActor(Actor* fromActor, float maxDistSq)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::FindClosestRegisteredActor", "Safe to call with a null starting actor (returns null). Needs to be able to read positions for the starting actor and every candidate it compares against.");
    if (!fromActor)
        return nullptr;

    NiPoint3 fromPos{};
    NiPoint3 fromRot{};
    if (!CoSyncGameAPI::GetActorWorldTransform(fromActor, fromPos, fromRot))
        return nullptr;

    Actor*  bestActor  = nullptr;
    float   bestDistSq = maxDistSq;

    for (auto& kv : s_eidToActor)
    {
        Actor* candidate = kv.second;
        if (!candidate || candidate == fromActor)
            continue;

        NiPoint3 cPos{};
        NiPoint3 cRot{};
        if (!CoSyncGameAPI::GetActorWorldTransform(candidate, cPos, cRot))
            continue;

        float dx = cPos.x - fromPos.x;
        float dy = cPos.y - fromPos.y;
        float dz = cPos.z - fromPos.z;
        float distSq = dx * dx + dy * dy + dz * dz;

        if (distSq < bestDistSq)
        {
            bestDistSq = distSq;
            bestActor  = candidate;
        }
    }

    return bestActor;
}

void Reset()
{
    s_actorToEID.clear();
    s_eidToActor.clear();
    s_lastNpcHP.clear();
    s_lastNpcLegendary.clear();
    s_firstBindFail.clear();
    s_spawnFallback.clear();
    s_claimedRefToEID.clear();
    s_npcSeenResendAt.clear();
    s_disabledOriginalRefs.clear();
    s_badHealthActors.clear();
    s_spatialClaims.clear();
    s_entityToSpatialKey.clear();
    s_reportedLocalRefs.clear();
    s_deadEntities.clear();
    s_killedEntities.clear();
    s_npcDeadSentAt.clear();
    s_knownDeadRefs.clear();
    {
        std::lock_guard<std::mutex> lk(s_npcDeadMutex);
        s_pendingNpcDead.clear();
    }
    s_nextEntityID = kWorldActorBase;
    s_lastScanTime = -999.0;
    {
        std::lock_guard<std::mutex> lk(s_legendaryMutex);
        s_pendingLegendary.clear();
    }
    LOG_INFO("[NpcWorldSync] Cleared everything for a new session");
}

// Disables the real world NPC (if found) and spawns a controlled proxy clone.
// Mirrors exactly how CompanionSync handles Codsworth: the real actor's AI
// would fight PositionRemoteActor writes every frame, so we hide the original
// and let the spawn task create a fresh AI-disabled proxy instead.
// realActor may be null when the NPC is absent from the client's save —
// in that case we just queue the spawn without a disable step.
// 2026-08-02: a proxy may only be spawned into the cell the LOCAL player is
// standing in. CreateReferenceAtLocation places into the local player's current
// cell/worldspace regardless of the XYZ it is handed, so spawning an entity
// whose authoritative position belongs to a different cell produces a reference
// at nonsense coordinates inside an unloaded area, which the engine frees
// almost immediately. Live proof (05:14:56 client log): proxies were placed at
// exterior (-57174.6,70643.2,6169.2) while the local player stood at interior
// (-1522.5,-1.7,13.9), and all four were "freed (unloaded) — clearing" within
// 150-200ms; only the one entity whose local actor was already loaded and
// directly bindable survived. That churn is also why the client reported
// noActor=40 in [HpDiag] and why enemy health could never be applied.
// Returning false here defers the spawn -- state.spawnQueued is left clear so
// the existing SpawnWatchdog retries once the player is actually in range.
static bool IsSpawnPositionUsable(const NiPoint3& spawnPos)
{
    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc)
        return false;

    // Generous: only rejects positions that are certainly in another cell.
    // kKeepRadius-scale so it never fights the discovery/keep hysteresis.
    static constexpr float kMaxSpawnDist   = 20480.0f;
    static constexpr float kMaxSpawnDistSq = kMaxSpawnDist * kMaxSpawnDist;

    const float dx = spawnPos.x - pc->pos.x;
    const float dy = spawnPos.y - pc->pos.y;
    const float dz = spawnPos.z - pc->pos.z;
    return (dx * dx + dy * dy + dz * dz) <= kMaxSpawnDistSq;
}

// Finds this client's OWN local creature that corresponds to a creature another
// peer owns, so the non-owner puppet path can DISABLE it and show the puppet
// instead of both (the visual double). Matches by base form + proximity to the
// owner's reported position, in the local player's current cell. Returns null
// when there's no local match (the creature genuinely doesn't exist here).
// 2026-08-12: was a single-cell `lp->parentCell->objectList` scan -- the exact
// narrow method TickScanWorldActors_Impl's own discovery moved away from on
// 2026-08-02 (see g_processListsPtr comment above) because a nearby actor is
// very often NOT in the player's own exact parentCell (a neighboring exterior
// cell, or the engine's middle-high simulation tier). For a dynamic encounter
// creature specifically, this function existing to find-and-disable the local
// duplicate before SpawnProxyForWorldActor clones a puppet -- if it can't find
// a real match that DOES exist nearby, SpawnProxyForWorldActor is called with
// realActor=nullptr, silently skips the disable step, and clones anyway. That
// is the observed "spawned a double NPC ... did not remove the original":
// this scan simply never saw the original because it was a cell over. Now
// walks the same ProcessLists High+MiddleHigh actor-handle lists discovery
// uses, so "is there a local copy nearby" gets the same answer either way.
static Actor* FindLocalDuplicateCreature(uint32_t baseFormID, const NiPoint3& atPos)
{
    PlayerCharacter* lp = CoSyncGetPlayer();
    if (!lp || baseFormID == 0)
        return nullptr;

    // Tight tolerance: a genuine co-located same-encounter creature is within a
    // couple hundred units; anything further is a different creature we must NOT
    // disable. 1500 matches kMaxBindDist used elsewhere for "same instance".
    static constexpr float kMaxDupDistSq = 1500.0f * 1500.0f;

    Actor* best   = nullptr;
    float  bestSq = kMaxDupDistSq;

    auto consider = [&](TESObjectREFR* r)
    {
        if (!r || !r->baseForm) return;
        if (r->baseForm->formType != kFormType_NPC_) return;
        // 2026-08-16 pt.7: baseFormID (the param) is now the STATIC template-
        // ancestor formID (see ResolveCrossMachineBaseFormID), but a locally,
        // independently-AI-spawned duplicate's own baseForm may still be a
        // raw dynamic (0xFF) instance formID -- comparing those directly
        // would never match, silently breaking local-duplicate detection for
        // every dynamic creature (both the puppet AND the local original
        // would render). Resolve this candidate through the same chain
        // before comparing so both sides are in the same terms.
        if (ResolveCrossMachineBaseFormID(static_cast<TESNPC*>(r->baseForm)) != baseFormID) return;
        if (r->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)) return;
        Actor* a = static_cast<Actor*>(r);
        if (a == static_cast<Actor*>(lp)) return;
        if (g_CoSyncEntities.GetEntityID(a) != 0) return;   // already a proxy/bound
        if (s_actorToEID.count(a)) return;
        const float dx = r->pos.x - atPos.x;
        const float dy = r->pos.y - atPos.y;
        const float dz = r->pos.z - atPos.z;
        const float dSq = dx*dx + dy*dy + dz*dz;
        if (dSq < bestSq) { bestSq = dSq; best = a; }
    };

    bool scannedProcessLists = false;
    {
        const uintptr_t pl = *g_processListsPtr;
        if (pl)
        {
            auto walkHandleList = [&](uintptr_t dataOff, uintptr_t countOff)
            {
                uint32_t* handles = *reinterpret_cast<uint32_t* const*>(pl + dataOff);
                const uint32_t n  = *reinterpret_cast<const uint32_t*>(pl + countOff);
                if (!handles || n == 0 || n >= 4096)
                    return;
                scannedProcessLists = true;
                for (uint32_t i = 0; i < n; ++i)
                {
                    NiPointer<TESObjectREFR> refPtr;
                    if (LookupREFRByHandle(handles[i], refPtr) && refPtr.m_pObject)
                        consider(refPtr.m_pObject);
                }
            };
            walkHandleList(kPL_HighHandleData,       kPL_HighHandleCount);
            walkHandleList(kPL_MiddleHighHandleData, kPL_MiddleHighHandleCount);
        }
    }

    if (!scannedProcessLists)
    {
        TESObjectCELL* c = lp->parentCell;
        if (!c)
            return nullptr;
        for (uint32_t i = 0; i < static_cast<uint32_t>(c->objectList.count); ++i)
            consider(c->objectList[i]);
    }

    return best;
}

// Returns true when it actually hid realActor and queued the replacement
// spawn; false when the spawn was only deferred (target not in the local area)
// and nothing was disabled. The callers' logs used to claim "Hid the local ref"
// either way -- 642 such lines in the 26 Sep host log, of which only one NPC
// (0x4B8F9) was really hidden.
static bool SpawnProxyForWorldActor(Actor* realActor, const EntityCreatePacket& p)
{
    if (!IsSpawnPositionUsable(p.spawnPos))
    {
        static double s_lastFarLog = 0.0;
        const double nowFar = NpcNowSeconds();
        if (nowFar - s_lastFarLog > 3.0)
        {
            s_lastFarLog = nowFar;
            LOG_UPDATED("[NpcWorldSync] Holding off on spawning entity=%u — target position (%.0f,%.0f,%.0f) isn't in the local player's area yet",
                p.entityID, p.spawnPos.x, p.spawnPos.y, p.spawnPos.z);
        }
        // Record the create so nothing is lost, but leave spawnQueued clear so
        // the watchdog retries when the player reaches the same cell.
        CoSyncEntityState& deferred = g_CoSyncPlayerManager.GetOrCreateState(p.entityID);
        deferred.entityID          = p.entityID;
        deferred.hasCreate         = true;
        deferred.isNPC             = true;
        deferred.hostAuthoritative = (p.ownerEntityID == 0);
        deferred.isLocallyOwned    = false;
        deferred.lastCreate        = p;
        deferred.spawnQueued       = false;
        return false;
    }

    // Skip Disable() if the engine already has this ref disabled. Repeated
    // Disable() calls degrade the Papyrus VM script state until the next
    // CallFunctionNoWait crashes (RAX=0xFEEEFEEE UAF). kFlag_IsDisabled is set
    // by the engine on the first Disable() and persists across our Reset(), so
    // this check survives the Reset() that clears s_spawnFallback between
    // pre-role and post-role bind attempts.
    if (realActor && !(realActor->flags & TESObjectREFR::kFlag_IsDisabled))
        CoSyncGameAPI::DisableWorldActor(realActor);

    if (realActor)
    {
        s_disabledOriginalRefs.insert(realActor->formID);
        // Remember WHICH entity replaced this original. Quest aliases, quest
        // target markers and anything else that resolves "the NPC" still point
        // at the ORIGINAL ref, not at our proxy -- so if the original stays
        // frozen where it was disabled, the quest marker sits there while the
        // visible NPC walks away. Reported symptom: "the marker did not follow
        // the quest npc for client".
        s_entityToDisabledOriginal[p.entityID] = realActor->formID;
    }

    s_spawnFallback.insert(p.entityID);
    s_firstBindFail.erase(p.entityID);

    CoSyncEntityState& state = g_CoSyncPlayerManager.GetOrCreateState(p.entityID);
    state.entityID          = p.entityID;
    state.hasCreate         = true;
    state.isNPC             = true;
    state.hostAuthoritative = (p.ownerEntityID == 0);
    state.isLocallyOwned    = false;
    state.lastCreate        = p;
    state.spawnQueued          = true;
    state.lastSpawnAttemptTime = NpcNowSeconds();

    CoSyncSpawnTasks::EnqueueSpawn(
        p.entityID, p.baseFormID, CoSyncEntityType::NPC,
        p.spawnFlags, p.spawnPos, p.spawnRot);
    return true;
}

// UPDATE 2026-06-18 (round 23 audit): same fix as TickScanWorldActors above --
// identical unguarded cell->objectList raw-loop pattern, called every time a
// client receives an EntityCreate packet for an NPC.
__declspec(noinline) static void ClientBindWorldActor_Impl(const EntityCreatePacket& p)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::ClientBindWorldActor_Impl", "Needs a valid local player. Mainly looks up the actor directly by the ID the host sent, which works no matter which cell it's in. Only falls back to scanning the cell if that ID is missing. The caller wraps this in a crash guard.");

    // Host re-broadcasts CREATE every few seconds as a bind-retry mechanism.
    // Guard 1: entity already fully bound.
    if (s_eidToActor.find(p.entityID) != s_eidToActor.end())
        return;
    // Guard 2: spawn already queued via SpawnProxyForWorldActor — do NOT call
    // DisableWorldActor a second time. SpawnProxyForWorldActor inserts into
    // s_spawnFallback but not s_eidToActor (that only happens once the spawned
    // proxy is registered after the async task runs). Without this guard, every
    // 5s re-broadcast calls DisableWorldActor again on the already-disabled actor;
    // after ~2 calls the Papyrus VM tears down that actor's internal script object,
    // and the next CallFunctionNoWait lands on freed memory (RAX=0xFEEEFEEEFEEEFEEE
    // at f4se_1_11_221.dll+0x161F4 CALL [RAX], confirmed crash 2026-07-16 client log).
    if (s_spawnFallback.count(p.entityID))
        return;

    PlayerCharacter* localPlayer = CoSyncGetPlayer();
    if (!localPlayer)
        return;

    // 0.3.1: an NPC in another interior/worldspace is not bound here -- no
    // disabling of our own copy, no clone at its coordinates in our world.
    // The owner re-sends CREATE every few seconds, so it binds as soon as this
    // machine is in the same place.
    if (p.ownerEntityID != g_CoSyncPlayerManager.GetLocalEntityID() &&
        !CoSyncPlayerManager::IsLocalLocation(p.locWS, p.locCell))
        return;

    // Set when a bind is refused because the host and this client disagree
    // about WHERE the NPC is. That means the NPC exists here but somewhere
    // else -- so it must NOT be cloned either (see the clone fallback).
    bool refusedForPosition = false;

    // ── Owner path: this CREATE confirms an NPC this machine reported ───────
    // The actor is already this peer's own real, natively-simulated NPC — bind
    // it directly (no disable, no clone) and become its update source.
    if (p.ownerEntityID != 0 && p.ownerEntityID == g_CoSyncPlayerManager.GetLocalEntityID())
    {
        if (p.reserved0 == 0)
            return;

        TESForm* ownForm = LookupFormByID(p.reserved0);
        Actor* ownActor = (ownForm && ownForm->formType == kFormType_ACHR)
            ? static_cast<Actor*>(ownForm) : nullptr;
        if (!ownActor || ownActor == static_cast<Actor*>(localPlayer))
        {
            LOG_UPDATED_WARN("[NpcWorldSync] Couldn't find the actor for entity=%u ref=0x%08X yet — will try again next update",
                p.entityID, p.reserved0);
            return;
        }
        if (g_CoSyncEntities.GetEntityID(ownActor) != 0 || s_actorToEID.count(ownActor))
            return;

        s_actorToEID[ownActor]   = p.entityID;
        s_eidToActor[p.entityID] = ownActor;

        CoSyncPlayer& ownProxy = g_CoSyncPlayerManager.GetOrCreateByEntityID(p.entityID);
        ownProxy.entityID      = p.entityID;
        ownProxy.actorRef      = ownActor;
        ownProxy.hasSpawned    = true;
        ownProxy.isLocalPlayer = false;
        ownProxy.createType    = CoSyncEntityType::NPC;
        ownProxy.ownerEntityID = p.ownerEntityID;

        CoSyncEntityState& ownState = g_CoSyncPlayerManager.GetOrCreateState(p.entityID);
        ownState.entityID          = p.entityID;
        ownState.hasCreate         = true;
        ownState.isNPC             = true;
        ownState.hostAuthoritative = false;
        ownState.isLocallyOwned    = true;
        ownState.lastCreate        = p;

        g_CoSyncEntities.Register(p.entityID, ownActor);

        LOG_UPDATED("[NpcWorldSync] Linked entity=%u ref=0x%08X to actor=%p — now sending updates for it",
            p.entityID, p.reserved0, static_cast<void*>(ownActor));
        return;
    }

    // ── Non-owner dynamic creature -> puppet clone (Tilted-style) ────────────
    // We are NOT the owner (owner path above returned). A DYNAMIC creature --
    // reserved0 is 0 (host-discovered) or an 0xFF ref (a client's own encounter
    // spawn) -- has no shared instance on this machine to bind against, so the
    // old cell-scan-bind path mistook our OWN local creatures for it and refused
    // 67/78 on position (the "no creatures" bug). Spawn a fresh puppet clone
    // driven by the owner's updates instead. Applies on BOTH host and clients:
    // whichever peer doesn't own a given creature puppets it, so everyone sees
    // every creature. TickSuppressLocalDynamicSpawns removes our own duplicate.
    {
        const bool isDynamicCreature =
            (p.reserved0 == 0) || ((p.reserved0 & 0xFF000000u) == 0xFF000000u);
        if (isDynamicCreature)
        {
            // A host-owned creature (ownerEntityID==0) seen by the host itself
            // is our own real actor (bound in the scan) -- never puppet it.
            if (p.ownerEntityID == 0 && CoSyncNet::IsHost())
                return;

            // If we have our OWN local copy of this same co-located creature
            // (both engines spawned the encounter independently), disable it so
            // the puppet REPLACES it instead of doubling. SpawnProxyForWorldActor
            // disables the ref it is handed; null when there's no local match.
            Actor* localDup = FindLocalDuplicateCreature(p.baseFormID, p.spawnPos);
            if (!SpawnProxyForWorldActor(localDup, p))
                return;   // deferred: target not in the local area yet
            static std::atomic<uint32_t> s_dynClones{ 0 };
            const uint32_t n = ++s_dynClones;
            if (n <= 20 || n % 25 == 0)
                LOG_UPDATED("[NpcWorldSync] entity=%u base=0x%08X owner=%u -> spawned a controlled copy "
                            "(not ours to own%s)",
                    p.entityID, p.baseFormID, p.ownerEntityID,
                    localDup ? ", replaced a local duplicate" : "");
            return;
        }
    }

    // ── Primary path: direct bind via placed-reference formID ────────────────
    // The host sends actor->formID in reserved0 (world-placed actors only,
    // never FF-refs). LookupFormByID is cell-independent, instant, and
    // immune to parentCell timing and baseFormID ambiguity. This is the same
    // mechanism companions use and is why quest NPCs now bind correctly on
    // the first CREATE regardless of where the NPC currently is in the world.
    if (p.reserved0 != 0 && (p.reserved0 & 0xFF000000u) != 0xFF000000u)
    {
        TESForm* form = LookupFormByID(p.reserved0);
        // kFormType_ACHR == 65 — placed NPC reference; same static_cast
        // pattern used throughout this codebase instead of broken DYNAMIC_CAST.
        Actor* actor = (form && form->formType == kFormType_ACHR)
            ? static_cast<Actor*>(form) : nullptr;

        // actor->parentCell != nullptr is the guard: LookupFormByID is
        // cell-independent (resolves any placed NPC regardless of load
        // state, per the comment above), but EntityCreatePacket carries no
        // worldspace/cell field at all -- if the host is near this NPC
        // somewhere this client has never loaded (different worldspace or
        // interior the client isn't in), the form still resolves here but
        // parentCell is null. Proceeding anyway would call engine Disable()
        // on a real, persistent placed NPC this client's own game never
        // legitimately attached, baking a changed-form disable flag into the
        // client's own save for an NPC it was never near -- a real, repeated
        // occurrence with a host standing in a heavily populated interior
        // while a client was elsewhere in a different worldspace entirely.
        // Skip the direct bind and fall through to the cell-scan/clone-
        // fallback path below, which already safely no-ops the disable step
        // when the NPC isn't in the local player's own cell.
        // ── Position-agreement guard ─────────────────────────────────────────
        // 2026-08-04: binding required only that the NPC be LOADED on this
        // client (parentCell != null) -- never that both machines agreed on
        // WHERE it is. A CK-placed NPC has the SAME formID on both machines,
        // so a client sitting in its own cell resolves the host's refFormID to
        // its OWN copy, binds it, and from then on steers that real NPC to the
        // host's reported position. Reported symptom: on the client, every NPC
        // loaded in its cell teleported to the host's location as soon as the
        // proxy transform started applying.
        //
        // A bind is only meaningful when both sides are looking at the same
        // NPC in the same place. If the host says it is somewhere far from
        // where this client's copy actually stands, these are two different
        // world states -- do not bind, do not disable the client's real NPC,
        // and do not drag it across the map. Falling through leaves it to the
        // clone-fallback path, which is safe.
        bool positionsAgree = true;
        if (actor && actor->parentCell)
        {
            const float dx = actor->pos.x - p.spawnPos.x;
            const float dy = actor->pos.y - p.spawnPos.y;
            const float dz = actor->pos.z - p.spawnPos.z;
            const float distSq = dx * dx + dy * dy + dz * dz;

            if (distSq > kMaxBindDistSq)
            {
                positionsAgree = false;
                refusedForPosition = true;
                LOG_UPDATED_WARN("[NpcWorldSync] Not binding entity=%u ref=0x%08X: host says it's at "
                                 "(%.0f,%.0f,%.0f) but our local copy is at (%.0f,%.0f,%.0f), %.0f units apart "
                                 "— this would have teleported our own NPC",
                    p.entityID, p.reserved0,
                    p.spawnPos.x, p.spawnPos.y, p.spawnPos.z,
                    actor->pos.x, actor->pos.y, actor->pos.z,
                    sqrtf(distSq));
            }
        }

        if (actor && actor != static_cast<Actor*>(localPlayer) && actor->parentCell && positionsAgree)
        {
            const uint32_t existingEID = g_CoSyncEntities.GetEntityID(actor);
            if (existingEID != 0)
            {
                // Already registered — either a stale re-bind attempt or the
                // entity was bound to a different ID. Log and bail.
                LOG_UPDATED_WARN("[NpcWorldSync] ref=0x%08X is already registered as entity=%u (requested entity=%u) — skipping",
                    p.reserved0, existingEID, p.entityID);
                return;
            }
            if (s_actorToEID.count(actor))
            {
                LOG_UPDATED_WARN("[NpcWorldSync] ref=0x%08X is already tracked — skipping",
                    p.reserved0);
                return;
            }

            if (SpawnProxyForWorldActor(actor, p))
                LOG_UPDATED("[NpcWorldSync] Hid the local ref=0x%08X actor=%p, spawning a synced copy as entity=%u base=0x%08X",
                    p.reserved0, static_cast<void*>(actor), p.entityID, p.baseFormID);
            return;
        }

        // Actor not in this save (form absent/wrong type), or resolved but
        // not locally loaded (parentCell null -- host is near it somewhere
        // this client isn't). Log and fall through to cell scan so old saves
        // without the NPC, and cross-worldspace cases, still degrade safely.
        LOG_UPDATED_WARN("[NpcWorldSync] ref=0x%08X wasn't found in the save or isn't loaded here (form=%p type=%u parentCell=%p) — falling back to a nearby scan for entity=%u",
            p.reserved0,
            static_cast<void*>(form),
            form ? (unsigned)form->formType : 0u,
            actor ? static_cast<void*>(actor->parentCell) : nullptr,
            p.entityID);
    }

    // ── Fallback path: cell scan by baseFormID ────────────────────────────────
    // Used when reserved0 is absent (old packet format) or when LookupFormByID
    // returned null (NPC genuinely absent from this client's save/load-order).
    TESObjectCELL* cell = localPlayer->parentCell;
    if (!cell)
        return;

    Actor*   bestActor  = nullptr;
    float    bestDistSq = FLT_MAX;

    uint32_t diagNpcFormIDs[16] = {};
    uint32_t diagNpcCount       = 0;
    uint32_t diagSkippedReg     = 0;

    for (uint32_t i = 0; i < static_cast<uint32_t>(cell->objectList.count); ++i)
    {
        TESObjectREFR* refr = cell->objectList[i];
        if (!refr || !refr->baseForm)
            continue;

        if (refr->baseForm->formType == kFormType_NPC_ && diagNpcCount < 16)
            diagNpcFormIDs[diagNpcCount++] = refr->baseForm->formID;

        if (refr->baseForm->formID  != p.baseFormID)
            continue;
        if (refr->baseForm->formType != kFormType_NPC_)
            continue;

        Actor* actor = static_cast<Actor*>(refr);
        if (!actor || actor == static_cast<Actor*>(localPlayer))
            continue;

        if (g_CoSyncEntities.GetEntityID(actor) != 0)
            { ++diagSkippedReg; continue; }
        if (s_actorToEID.count(actor))
            { ++diagSkippedReg; continue; }

        NiPoint3 pos{}, rot{};
        if (!CoSyncGameAPI::GetActorWorldTransform(actor, pos, rot))
            continue;

        const float dx = pos.x - p.spawnPos.x;
        const float dy = pos.y - p.spawnPos.y;
        const float dz = pos.z - p.spawnPos.z;
        const float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq < bestDistSq) { bestDistSq = distSq; bestActor = actor; }
    }

    // 2026-08-04: bestDistSq was COMPUTED here but never actually tested --
    // the scan simply took the nearest base-form match in the cell at ANY
    // distance. That is the same "drag the client's own NPC to the host" bug
    // the direct-bind path had, and it was the bigger leak of the two: the
    // live log showed 71 CREATEs falling through to this path versus a single
    // refusal on the direct path. A generic Raider standing across the map
    // matched a Raider next to the host and got disabled and teleported.
    if (bestActor && bestDistSq > kMaxBindDistSq)
    {
        refusedForPosition = true;
        LOG_UPDATED_WARN("[NpcWorldSync] Not binding entity=%u base=0x%08X from the nearby scan: "
                         "the closest local match is %.0f units from where the host says it is "
                         "— this would have teleported our own NPC",
            p.entityID, p.baseFormID, sqrtf(bestDistSq));
        bestActor = nullptr;
    }

    if (bestActor)
    {
        if (SpawnProxyForWorldActor(bestActor, p))
            LOG_UPDATED("[NpcWorldSync] Hid local actor=%p (nearby scan, dist=%.0f), spawning a synced copy as entity=%u base=0x%08X",
                static_cast<void*>(bestActor), sqrtf(bestDistSq), p.entityID, p.baseFormID);
        return;
    }

    // ── Clone fallback: NPC truly absent from this client's save ─────────────
    // Only reached when BOTH direct lookup and cell scan failed. For world-
    // placed NPCs (non-FF refFormID) this means the NPC doesn't exist in the
    // client's load order at all, not just a timing miss (timing misses are
    // handled by the host re-broadcasting CREATE every 5s and the direct-lookup
    // retrying on each re-broadcast — usually succeeds within one retry).
    LOG_WARN("[NpcWorldSync] Looked in cell=0x%08X (objects=%u NPCs=%u alreadyTracked=%u) for base=0x%08X ref=0x%08X, no match",
        cell->formID, (uint32_t)cell->objectList.count,
        diagNpcCount, diagSkippedReg, p.baseFormID, p.reserved0);
    for (uint32_t di = 0; di < diagNpcCount; ++di)
        LOG_WARN("[NpcWorldSync]   NPC[%u] base=0x%08X", di, diagNpcFormIDs[di]);

    // 2026-08-05: do NOT clone an NPC we deliberately refused to bind.
    //
    // The clone fallback exists for one case only: the NPC genuinely does not
    // exist in this client's load order. A POSITION refusal is the opposite --
    // the NPC is right here, it is simply somewhere other than where the host
    // says its copy is. Cloning it then puts a second copy of a real NPC in
    // the world, which is exactly the "NPCs still spawning" report: the live
    // log showed 12 refusals and 24 clones on one client. The distance guard
    // did not remove the problem, it converted dragging into duplication.
    if (refusedForPosition)
    {
        static std::atomic<uint32_t> s_n{ 0 };
        const uint32_t n = ++s_n;
        if (n <= 10 || n % 25 == 0)
            LOG_UPDATED("[NpcWorldSync] entity=%u base=0x%08X not bound due to distance — "
                        "not cloning it either (the NPC exists here, just elsewhere)",
                p.entityID, p.baseFormID);
        return;
    }

    if (s_spawnFallback.count(p.entityID))
        return;

    const double now = NpcNowSeconds();
    double& firstFail = s_firstBindFail[p.entityID];
    if (firstFail == 0.0)
    {
        firstFail = now;
        if (diagNpcCount == 0)
        {
            firstFail -= (kBindFailSpawnDelay - 3.0);
            LOG_UPDATED_WARN("[NpcWorldSync] No NPC with base=0x%08X in this cell — cloning it in 3s (entity=%u)",
                p.baseFormID, p.entityID);
        }
        else
        {
            LOG_UPDATED_WARN("[NpcWorldSync] No bind match yet for entity=%u base=0x%08X ref=0x%08X — will clone it in %.0fs if this keeps failing",
                p.entityID, p.baseFormID, p.reserved0, kBindFailSpawnDelay);
        }
    }
    else if (now - firstFail >= kBindFailSpawnDelay)
    {
        s_spawnFallback.insert(p.entityID);

        // Suppress the client's original NPC if one exists in the cell —
        // use Papyrus Disable() instead of raw flag so the engine properly
        // tears down its AI, collision, and rendering.
        {
            PlayerCharacter* lp = CoSyncGetPlayer();
            TESObjectCELL*   c  = lp ? lp->parentCell : nullptr;
            if (c)
            {
                TESObjectREFR* bestOrig  = nullptr;
                float          bestOrigSq = FLT_MAX;
                for (uint32_t k = 0; k < static_cast<uint32_t>(c->objectList.count); ++k)
                {
                    TESObjectREFR* r = c->objectList[k];
                    if (!r || !r->baseForm) continue;
                    if (r->baseForm->formType != kFormType_NPC_) continue;
                    if (r->baseForm->formID != p.baseFormID) continue;
                    if (r->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)) continue;
                    Actor* a = static_cast<Actor*>(r);
                    if (a == static_cast<Actor*>(lp)) continue;
                    if (g_CoSyncEntities.GetEntityID(a) != 0) continue;
                    if (s_actorToEID.count(a)) continue;
                    const float dx = r->pos.x - p.spawnPos.x;
                    const float dy = r->pos.y - p.spawnPos.y;
                    const float dz = r->pos.z - p.spawnPos.z;
                    const float dSq = dx*dx + dy*dy + dz*dz;
                    if (dSq < bestOrigSq) { bestOrigSq = dSq; bestOrig = r; }
                }
                if (bestOrig)
                {
                    CoSyncGameAPI::DisableWorldActor(bestOrig);
                    s_disabledOriginalRefs.insert(bestOrig->formID);
                    LOG_UPDATED("[NpcWorldSync] Queued hiding the original ref=0x%08X (base=0x%08X, dist=%.0f) before cloning",
                        bestOrig->formID, p.baseFormID, sqrtf(bestOrigSq));
                }
            }
        }

        CoSyncEntityState& state = g_CoSyncPlayerManager.GetOrCreateState(p.entityID);
        state.entityID          = p.entityID;
        state.hasCreate         = true;
        state.isNPC             = true;
        state.hostAuthoritative = (p.ownerEntityID == 0);
        state.isLocallyOwned    = false;
        state.lastCreate        = p;
        state.spawnQueued          = true;
        state.lastSpawnAttemptTime = NpcNowSeconds();

        CoSyncSpawnTasks::EnqueueSpawn(
            p.entityID, p.baseFormID, CoSyncEntityType::NPC,
            p.spawnFlags, p.spawnPos, p.spawnRot);

        LOG_UPDATED("[NpcWorldSync] Cloning entity=%u base=0x%08X at (%.0f,%.0f,%.0f) since no local match was ever found",
            p.entityID, p.baseFormID, p.spawnPos.x, p.spawnPos.y, p.spawnPos.z);
    }
}

void SyncDisabledOriginalToProxy(uint32_t entityID, const NiPoint3& pos)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::SyncDisabledOriginalToProxy", "Must run on the game thread. Does nothing unless this entity replaced a local original NPC. The original is looked up fresh every call and only moved once a second, which is plenty for a quest marker.");
    auto it = s_entityToDisabledOriginal.find(entityID);
    if (it == s_entityToDisabledOriginal.end())
        return;

    static std::unordered_map<uint32_t, double> s_lastMove;
    const double now = NpcNowSeconds();
    double& last = s_lastMove[entityID];
    if (now - last < 1.0)
        return;
    last = now;

    TESForm* form = LookupFormByID(it->second);
    if (!form || form->formType != kFormType_ACHR)
        return;
    TESObjectREFR* orig = static_cast<TESObjectREFR*>(form);

    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc) return;
    TESObjectREFR* pcRefr = reinterpret_cast<TESObjectREFR*>(pc);

    TESWorldSpace* ws   = CALL_MEMBER_FN(pcRefr, GetWorldspace)();
    TESObjectCELL* cell = ws ? nullptr : pcRefr->parentCell;

    NiPoint3 p2{ pos.x, pos.y, pos.z };
    NiPoint3 r2{ 0.f, 0.f, 0.f };
    UInt32   nh = 0;
    MoveRefrToPosition(orig, &nh, cell, ws, &p2, &r2);
}

void ClientBindWorldActor(const EntityCreatePacket& p)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::ClientBindWorldActor", "Just needs a valid spawn packet from the host. Wraps the binding logic in a crash guard so a problem here doesn't crash the game.");
    __try
    {
        ClientBindWorldActor_Impl(p);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[NpcWorldSync] Binding entity=%u base=0x%08X hit a problem — caught it",
            p.entityID, p.baseFormID);
    }
}

// Round 138: stale proxy-body cleanup. Live 2026-07-05 P2 cell scan showed a
// persisted FF-ref actor with base 0x00000007 standing around — a proxy from
// the old fallback era baked into the save ("still 00000007 npc" report).
// Runs on BOTH roles: despawns runtime-created (FF-ref) actors that use any
// known proxy base but aren't registered to a live CoSync entity.
__declspec(noinline) static void CleanupStaleProxyBodies_Impl(double now)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::CleanupStaleProxyBodies_Impl", "Needs a valid local player in a valid cell. Goes through everything in that cell; the caller wraps this in a crash guard.");
    static double s_lastCleanup = -999.0;
    static constexpr double kCleanupInterval = 10.0;
    if (now - s_lastCleanup < kCleanupInterval)
        return;
    s_lastCleanup = now;

    PlayerCharacter* localPlayer = CoSyncGetPlayer();
    if (!localPlayer)
        return;

    TESObjectCELL* cell = localPlayer->parentCell;
    if (!cell)
        return;

    const uint32_t rmBase = CoSyncPapyrusHelper::GetRemotePlayerBaseFormID();

    for (uint32_t i = 0; i < (uint32_t)cell->objectList.count; ++i)
    {
        TESObjectREFR* refr = cell->objectList[i];
        if (!refr || !refr->baseForm)
            continue;
        if (refr->baseForm->formType != kFormType_NPC_)
            continue;

        // Only runtime-created refs (0xFF...) — never touch save-baked or
        // plugin-placed world actors, and never the player (refID 0x14).
        if ((refr->formID & 0xFF000000u) != 0xFF000000u)
            continue;

        const uint32_t base = refr->baseForm->formID;
        const bool isProxyBase =
            base == 0x00000007u ||
            base == 0x00020749u ||
            (base & 0x00FFFFFFu) == 0x00003599u ||
            (rmBase != 0 && base == rmBase) ||
            CoSyncProxyLooks::IsProxyBaseFormID(base);
        if (!isProxyBase)
            continue;

        Actor* actor = static_cast<Actor*>(refr);
        if (actor == static_cast<Actor*>(localPlayer))
            continue;

        // Live proxies are registered with the entity registry — skip them.
        if (g_CoSyncEntities.GetEntityID(actor) != 0)
            continue;

        // Round 140: ALSO consult the PlayerManager — CompanionSync links
        // companion proxies without putting them in g_CoSyncEntities, and
        // the round-139 build repeatedly disabled a LIVE companion here
        // (live log 14:39:27: GHOST despawn of the companion spawned at
        // 14:39:21). Any actor a CoSyncPlayer proxy currently owns is alive.
        {
            bool ownedByProxy = false;
            for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
            {
                const CoSyncPlayer* p = kv.second.get();
                if (p && p->actorRef == actor)
                {
                    ownedByProxy = true;
                    break;
                }
            }
            if (ownedByProxy)
                continue;
        }

        // Disabled refs stay in the cell objectList, so without this the
        // same ghost gets "despawned" every pass forever.
        static std::unordered_set<uint32_t> s_despawnAttempted;
        if (s_despawnAttempted.count(refr->formID))
            continue;
        s_despawnAttempted.insert(refr->formID);

        CoSyncGameAPI::DespawnProxyActor(actor);
        LOG_UPDATED("[NpcWorldSync] Removed a leftover proxy refID=0x%08X base=0x%08X from an old session",
            refr->formID, base);
    }
}

void CleanupStaleProxyBodies(double now)
{
    if (!kEnemyCreatureSyncEnabled)
    {
        LogEnemyCreatureSyncDisabledOnce();
        return;
    }

    LOG_REQUIREMENT("CoSyncNpcWorldSync::CleanupStaleProxyBodies", "No requirements beyond being called each tick. Wraps the cell scan in a crash guard.");
    __try
    {
        CleanupStaleProxyBodies_Impl(now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[NpcWorldSync] Leftover-proxy cleanup hit a problem — caught it");
    }
}

bool IsLegendary(const std::string& msg)
{
    return msg.rfind("LGD|", 0) == 0;
}

void OnReceiveLegendary(const std::string& msg)
{
    // "LGD|entityID|rank"
    const char* s = msg.c_str() + 4;
    char* e = nullptr;
    const uint32_t eid  = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (!e || *e != '|' || eid == 0) { LOG_WARN("[NpcWorldSync] Got a badly-formed legendary-rank message"); return; }
    s = e + 1;
    const int rank = static_cast<int>(std::strtol(s, &e, 10));
    if (e == s) { LOG_WARN("[NpcWorldSync] Got a legendary-rank message with a bad rank value"); return; }

    LOG_UPDATED("[NpcWorldSync] Received legendary rank: entity=%u rank=%d (queued)", eid, rank);
    std::lock_guard<std::mutex> lk(s_legendaryMutex);
    s_pendingLegendary[eid] = rank;
}

__declspec(noinline) static void FlushPendingLegendaryOps_Impl(
    const std::unordered_map<uint32_t, int>& pending)
{
    ActorValueInfo* legAVI = GetLegendaryAVICached();
    if (!legAVI)
    {
        LOG_WARN("[NpcWorldSync] Can't apply legendary ranks — the LegendaryRank stat (form 0x%08X) wasn't found", kLegendaryRankAVI_FormID);
        return;
    }

    for (const auto& kv : pending)
    {
        const uint32_t entityID = kv.first;
        const int      rank     = kv.second;

        Actor* actor = nullptr;
        auto it = s_eidToActor.find(entityID);
        if (it != s_eidToActor.end())
            actor = it->second;
        if (!actor)
        {
            CoSyncPlayer* proxy = g_CoSyncPlayerManager.GetPlayerByEntityID(entityID);
            if (proxy) actor = proxy->actorRef;
        }
        if (!actor)
        {
            LOG_WARN("[NpcWorldSync] No actor found for entity=%u — can't apply its legendary rank", entityID);
            continue;
        }

        CoSyncAVSafety::SafeSetBase(actor, legAVI, static_cast<float>(rank));
        LOG_UPDATED("[NpcWorldSync] Applied legendary rank: entity=%u actor=%p rank=%d",
            entityID, static_cast<void*>(actor), rank);
    }
}

__declspec(noinline) static void FlushPendingLegendaryOpsSafe(
    const std::unordered_map<uint32_t, int>& pending)
{
    __try { FlushPendingLegendaryOps_Impl(pending); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[NpcWorldSync] Applying legendary ranks hit a problem — caught it");
    }
}

void FlushPendingLegendaryOps()
{
    std::unordered_map<uint32_t, int> pending;
    {
        std::lock_guard<std::mutex> lk(s_legendaryMutex);
        s_pendingLegendary.swap(pending);
    }
    if (pending.empty())
        return;
    FlushPendingLegendaryOpsSafe(pending);
}

bool IsWorldActorWanted(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::IsWorldActorWanted", "Doesn't touch the game — just checks our own internal tracking maps. Both are only ever changed on the game thread, so no locking is needed here.");
    return s_spawnFallback.count(entityID) != 0 || s_eidToActor.count(entityID) != 0;
}

// ── Dynamic ownership (0.3.1) ───────────────────────────────────────────────
namespace
{
    constexpr double kHandoffInterval  = 2.0;
    constexpr double kHandoffCooldown  = 15.0;    // per NPC, against ping-pong
    constexpr double kHandoffHoldSecs  = 30.0;    // host scan keeps its hands off
    constexpr float  kOwnerKeepDist    = 12000.f; // owner still simulating it
    constexpr float  kClaimantDist     = 8000.f;  // someone near enough to take it

    std::unordered_map<uint32_t, double> s_handoffHoldRef;      // static ref -> until
    std::unordered_map<uint64_t, double> s_handoffHoldSpatial;  // spatial key -> until (dynamic refs)
    std::unordered_map<uint32_t, double> s_ownershipSince;      // entity -> last create/transfer
}

static bool IsHandoffHeld(uint32_t refFormID, uint32_t baseFormID, const NiPoint3& pos)
{
    const double now = NpcNowSeconds();
    if ((refFormID & 0xFF000000u) != 0xFF000000u)
    {
        auto it = s_handoffHoldRef.find(refFormID);
        return it != s_handoffHoldRef.end() && now < it->second;
    }
    auto it = s_handoffHoldSpatial.find(MakeSpatialKey(baseFormID, pos));
    return it != s_handoffHoldSpatial.end() && now < it->second;
}

void HostTickOwnershipHandoff(double now)
{
    if (!CoSyncNet::IsHost() || !CoSyncNet::IsConnected())
        return;
    static double s_next = 0.0;
    if (now < s_next)
        return;
    s_next = now + kHandoffInterval;

    struct Who { uint32_t eid; uint32_t ws; uint32_t cell; NiPoint3 pos; };
    std::vector<Who> players;
    const uint32_t hostEid = g_CoSyncPlayerManager.GetLocalEntityID();
    if (PlayerCharacter* lp = CoSyncGetPlayer())
    {
        Who w{ hostEid, 0xFFFFFFFFu, 0, reinterpret_cast<TESObjectREFR*>(lp)->pos };
        if (g_CoSyncPlayerManager.GetEntityLocation(hostEid, w.ws, w.cell))
            players.push_back(w);
    }
    for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
    {
        const CoSyncPlayer* p = kv.second.get();
        if (!p || p->createType != CoSyncEntityType::Player || p->entityID == hostEid)
            continue;
        Who w{ p->entityID, 0xFFFFFFFFu, 0, p->authoritativePos };
        if (g_CoSyncPlayerManager.GetEntityLocation(p->entityID, w.ws, w.cell))
            players.push_back(w);
    }
    auto find = [&](uint32_t eid) -> const Who* {
        for (const Who& w : players) if (w.eid == eid) return &w;
        return nullptr;
    };
    auto dist2 = [](const NiPoint3& a, const NiPoint3& b) {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    };

    struct Release { uint32_t eid; uint32_t ref; uint32_t base; NiPoint3 pos; uint32_t to; uint32_t from; };
    std::vector<Release> releases;

    for (const auto& kv : g_CoSyncPlayerManager.GetStates())
    {
        const uint32_t eid = kv.first;
        const CoSyncEntityState& st = kv.second;
        if (!IsWorldActorEntityID(eid) || !st.hasCreate || IsEntityDead(eid))
            continue;
        const EntityCreatePacket& c = st.lastCreate;
        if (c.locWS == 0xFFFFFFFFu)
            continue;   // old-style entity without a location: leave it alone

        double& since = s_ownershipSince[eid];
        if (since == 0.0) since = now;
        if (now - since < kHandoffCooldown)
            continue;

        // Where the NPC is now: its streamed position if we have one.
        NiPoint3 npcPos = c.spawnPos;
        if (const CoSyncPlayer* np = g_CoSyncPlayerManager.GetPlayerByEntityID(eid))
            if (np->authoritativePos.x != 0.f || np->authoritativePos.y != 0.f || np->authoritativePos.z != 0.f)
                npcPos = np->authoritativePos;

        const uint32_t owner = c.ownerEntityID ? c.ownerEntityID : hostEid;
        const Who* o = find(owner);
        const auto sameLoc = [&](const Who& w) {
            return c.locWS != 0 ? w.ws == c.locWS : (w.ws == 0 && w.cell == c.locCell);
        };
        const bool ownerHere = o && sameLoc(*o) && dist2(o->pos, npcPos) <= kOwnerKeepDist * kOwnerKeepDist;
        if (ownerHere)
            continue;

        const Who* best = nullptr;
        float bestD = kClaimantDist * kClaimantDist;
        for (const Who& w : players)
        {
            if (w.eid == owner || !sameLoc(w)) continue;
            const float d = dist2(w.pos, npcPos);
            if (d < bestD) { bestD = d; best = &w; }
        }
        if (!best)
            continue;   // nobody near it: nothing gained by moving it
        releases.push_back({ eid, c.reserved0, c.baseFormID, npcPos, best->eid, owner });
    }

    for (const Release& r : releases)
    {
        // Hold it from the host's own scan unless the host IS the new owner.
        if (r.to != hostEid)
        {
            if (r.ref != 0 && (r.ref & 0xFF000000u) != 0xFF000000u)
                s_handoffHoldRef[r.ref] = now + kHandoffHoldSecs;
            else
                s_handoffHoldSpatial[MakeSpatialKey(r.base, r.pos)] = now + kHandoffHoldSecs;
        }
        EntityDestroyPacket d{};
        d.entityID    = r.eid;
        d.reasonFlags = 0;
        CoSyncTransport::Send(SerializeEntityDestroy(d));
        ClientUnbindWorldActor(r.eid, 0);
        s_ownershipSince.erase(r.eid);
        LOG_UPDATED("[NpcWorldSync] Handoff: entity=%u ref=0x%08X released by %u (away) -- %u is near it and takes it over",
            r.eid, r.ref, r.from, r.to);
    }

    // Forget holds and timers that expired / belong to gone entities.
    for (auto it = s_handoffHoldRef.begin(); it != s_handoffHoldRef.end(); )
        it = (now >= it->second) ? s_handoffHoldRef.erase(it) : std::next(it);
    for (auto it = s_handoffHoldSpatial.begin(); it != s_handoffHoldSpatial.end(); )
        it = (now >= it->second) ? s_handoffHoldSpatial.erase(it) : std::next(it);
    for (auto it = s_ownershipSince.begin(); it != s_ownershipSince.end(); )
        it = g_CoSyncPlayerManager.GetStates().count(it->first) ? std::next(it) : s_ownershipSince.erase(it);
}

void HostReleaseEntitiesOwnedBy(uint32_t ownerEntityID)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::HostReleaseEntitiesOwnedBy", "Host, game thread (PEERLEFT). Safe when the owner had nothing.");
    if (!CoSyncNet::IsHost() || ownerEntityID == 0)
        return;

    std::vector<std::pair<uint32_t, uint32_t>> owned;   // entityID, destroy reason flags
    for (const auto& kv : g_CoSyncPlayerManager.GetStates())
    {
        const CoSyncEntityState& st = kv.second;
        if (IsWorldActorEntityID(kv.first) && st.hasCreate && st.lastCreate.ownerEntityID == ownerEntityID)
            owned.push_back({ kv.first, IsEntityDead(kv.first) ? kDestroyReason_Dead : 0u });
    }

    for (const auto& e : owned)
    {
        EntityDestroyPacket d{};
        d.entityID    = e.first;
        d.reasonFlags = e.second;
        CoSyncTransport::Send(SerializeEntityDestroy(d));
        ClientUnbindWorldActor(e.first, e.second);   // erases the state entry: collected above first
    }

    if (!owned.empty())
        LOG_UPDATED("[NpcWorldSync] Released %zu world NPC(s) owned by departed player %u -- the host takes them back",
            owned.size(), ownerEntityID);
}

void ForgetBinding(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::ForgetBinding", "Game thread. Safe for an untracked entity. Only erases binding-map entries; never dereferences the (possibly freed) actor and never touches the player manager.");
    auto it = s_eidToActor.find(entityID);
    if (it != s_eidToActor.end())
    {
        s_actorToEID.erase(it->second);   // key only -- the actor may already be freed
        s_eidToActor.erase(it);
    }
    s_spawnFallback.erase(entityID);
    s_firstBindFail.erase(entityID);
    LOG_UPDATED("[NpcWorldSync] entity=%u binding forgotten (actor freed) -- the owner's next CREATE re-binds it",
        entityID);
}

void ClientUnbindWorldActor(uint32_t entityID, uint32_t reasonFlags)
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::ClientUnbindWorldActor", "Safe to call even if this entity isn't currently tracked. Despawning the proxy happens asynchronously, so it's safe to call from here. When the entity is known dead (KILL applied locally, or kDestroyReason_Dead in the packet) the proxy corpse is left in the world and the disabled original is NOT re-enabled.");
    // Round 140: BOUND world actors are the client's OWN real NPCs — on a
    // DESTROY they must only be unlinked, never disabled (the old code
    // disabled the client's real Codsworth when he left the host's cell).
    // Only spawn-fallback CLONES (FF refs CoSync created) get despawned.
    Actor* boundActor = nullptr;
    auto it = s_eidToActor.find(entityID);
    if (it != s_eidToActor.end())
    {
        boundActor = it->second;
        s_actorToEID.erase(boundActor);
        s_eidToActor.erase(it);
    }

    const bool hadFallback = s_spawnFallback.count(entityID) != 0;
    const bool isDead      = (reasonFlags & kDestroyReason_Dead) != 0 || s_killedEntities.count(entityID) != 0;

    Actor* cloneToDespawn = nullptr;
    if (!boundActor && hadFallback)
    {
        CoSyncPlayer* proxy = g_CoSyncPlayerManager.GetPlayerByEntityID(entityID);
        if (proxy && proxy->actorRef)
            cloneToDespawn = proxy->actorRef;
    }

    // Re-enable the real original this peer disabled when it became a
    // puppet-holder for this entity — otherwise the NPC stays disabled in
    // this world forever after the owner departs.
    // 2026-09-05: NOT for dead entities -- the original is alive in this
    // client's save, so re-enabling it resurrects the enemy the host killed.
    uint32_t originalRefToEnable = 0;
    if (hadFallback && !isDead)
    {
        auto stIt = g_CoSyncPlayerManager.GetStates().find(entityID);
        if (stIt != g_CoSyncPlayerManager.GetStates().end() && stIt->second.hasCreate)
        {
            const uint32_t r0 = stIt->second.lastCreate.reserved0;
            if (r0 != 0 && (r0 & 0xFF000000u) != 0xFF000000u)
                originalRefToEnable = r0;
        }
    }

    // W1 (2026-09-26): s_entityToDisabledOriginal is the authoritative record
    // of which local NPC this entity hid. The fallback flag above can be gone
    // (ForgetBinding after an engine free, or a re-bind), and the map entry
    // was never erased at all -- so restore from it and always drop it here.
    {
        auto mo = s_entityToDisabledOriginal.find(entityID);
        if (mo != s_entityToDisabledOriginal.end())
        {
            if (originalRefToEnable == 0 && !isDead && (mo->second & 0xFF000000u) != 0xFF000000u)
                originalRefToEnable = mo->second;
            s_entityToDisabledOriginal.erase(mo);
        }
    }
    s_spawnFallback.erase(entityID);
    s_firstBindFail.erase(entityID);
    s_killedEntities.erase(entityID);
    ReleaseClaimForEntity(entityID);

    // The TickHook keeps a raw CoSyncPlayer* per proxy actor; drop it before
    // UnregisterEntityIDOnly destroys the CoSyncPlayer.
    if (cloneToDespawn)
        UnregisterRemoteActor(cloneToDespawn);

    g_CoSyncEntities.Remove(entityID);
    g_CoSyncPlayerManager.UnregisterEntityIDOnly(entityID);

    if (cloneToDespawn)
    {
        if (isDead)
        {
            // Keep the corpse where it fell; the engine cleans it up with the
            // cell like any other dead actor.
            LOG_UPDATED("[NpcWorldSync] entity=%u unbound DEAD -- keeping proxy corpse %p, original stays disabled",
                entityID, static_cast<void*>(cloneToDespawn));
        }
        else
        {
            CoSyncGameAPI::DespawnProxyActor(cloneToDespawn);
        }
    }

    if (originalRefToEnable != 0)
    {
        s_disabledOriginalRefs.erase(originalRefToEnable);
        TESForm* form = LookupFormByID(originalRefToEnable);
        if (form && form->formType == kFormType_ACHR)
        {
            Actor* original = static_cast<Actor*>(form);
            if (original->flags & TESObjectREFR::kFlag_IsDisabled)
            {
                CoSyncGameAPI::EnableProxyActor(original);
                LOG_UPDATED("[NpcWorldSync] Showed the original ref=0x%08X again after unbinding entity=%u",
                    originalRefToEnable, entityID);
            }
        }
    }

    LOG_UPDATED("[NpcWorldSync] Unbound entity=%u bound=%p clone=%p", entityID,
        static_cast<void*>(boundActor), static_cast<void*>(cloneToDespawn));
}

// IDA-confirmed 2026-07-21: BGSSaveLoadGame::SaveGame() (Fallout4.exe+0xBEDD20)
// walks the engine's changed-forms list and, for each one, builds a location
// descriptor (Fallout4.exe+0xBF712D) that reads a field off the reference's
// TESObjectREFR::parentCell (offset 0xB8, per this codebase's own
// GameReferences.h static_assert) -- or a global fallback if that's null --
// with no guard against BOTH being null. Disabled-original NPC refs (native
// Disable() called on a real placed actor) and live tracked proxy/clone
// actors (mid spawn/despawn lifecycle) are exactly the kind of references
// this codebase creates that could plausibly end up "changed" (dirty since
// load) with a stale/null parentCell.
//
// This function now does two things: (1) logs every tracked actor with its
// parentCell status, and (2) for any actor whose parentCell is null, patches
// it to the local player's current cell so BGSSaveLoadGame::SaveGame() can
// build a valid location descriptor without crashing. Patched actors are
// recorded in s_saveProtectedActors so RestoreActorsAfterSave() can zero
// them back out in kMessage_PostSaveGame. SEH-wrapped.
__declspec(noinline) static void DiagnoseDisabledOriginalsForSave_Impl()
{
    if (s_disabledOriginalRefs.empty())
    {
        LOG_UPDATED("[SaveDiag] No hidden original NPCs to check before saving");
    }

    for (uint32_t formID : s_disabledOriginalRefs)
    {
        TESForm* form = LookupFormByID(formID);
        if (!form || form->formType != kFormType_ACHR)
        {
            LOG_WARN("[SaveDiag] Hidden original formID=0x%08X isn't a valid NPC reference (form=%p type=%u)",
                formID, static_cast<void*>(form), form ? (unsigned)form->formType : 0u);
            continue;
        }

        TESObjectREFR* refr = static_cast<TESObjectREFR*>(form);
        LOG_UPDATED("[SaveDiag] Hidden original formID=0x%08X is in cell=%p%s",
            formID, static_cast<void*>(refr->parentCell),
            refr->parentCell ? "" : "  <-- none");
    }

    // Second category: live tracked proxy/clone actors (spawned via
    // CreateReferenceAtLocation, mid-lifecycle Disable/Enable/Delete).
    // Logs each one and patches null parentCells before the save walk.
    PlayerCharacter* lp = CoSyncGetPlayer();
    TESObjectCELL*   playerCell = lp ? lp->parentCell : nullptr;

    const auto& players = g_CoSyncPlayerManager.GetAllPlayers();
    if (players.empty())
    {
        LOG_UPDATED("[SaveDiag] No synced actors to check before saving");
    }
    for (const auto& kv : players)
    {
        const uint32_t entityID = kv.first;
        Actor* actor = kv.second ? kv.second->actorRef : nullptr;
        if (!actor)
        {
            LOG_UPDATED("[SaveDiag] entity=%u has no actor yet (not spawned) — skipping", entityID);
            continue;
        }

        TESObjectREFR* refr = static_cast<TESObjectREFR*>(actor);
        if (!refr->parentCell)
        {
            LOG_UPDATED("[SaveDiag] entity=%u actor=%p formID=0x%08X has no cell — temporarily using the player's cell %p so saving works",
                entityID, static_cast<void*>(actor), refr->formID, static_cast<void*>(playerCell));
            if (playerCell)
            {
                refr->parentCell = playerCell;
                s_saveProtectedActors.insert(actor);
            }
            else
            {
                LOG_WARN("[SaveDiag] entity=%u has no cell and neither does the player — can't work around it, saving may crash",
                    entityID);
            }
        }
        else
        {
            LOG_UPDATED("[SaveDiag] entity=%u actor=%p formID=0x%08X cell=%p (fine)",
                entityID, static_cast<void*>(actor), refr->formID, static_cast<void*>(refr->parentCell));
        }
    }
}

void DiagnoseDisabledOriginalsForSave()
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::DiagnoseDisabledOriginalsForSave", "Checks for actors with no cell and works around it before the game saves, so saving doesn't crash. Wrapped in a crash guard. Remembers which actors it touched so RestoreActorsAfterSave() can undo it.");
    __try
    {
        DiagnoseDisabledOriginalsForSave_Impl();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[SaveDiag] Pre-save check hit a problem — caught it");
    }
}

__declspec(noinline) static void RestoreActorsAfterSave_Impl()
{
    if (s_saveProtectedActors.empty())
    {
        LOG_UPDATED("[SaveDiag] No actors need restoring after the save");
        return;
    }
    uint32_t count = 0;
    for (Actor* actor : s_saveProtectedActors)
    {
        if (!actor) continue;
        TESObjectREFR* refr = static_cast<TESObjectREFR*>(actor);
        refr->parentCell = nullptr;
        ++count;
    }
    s_saveProtectedActors.clear();
    LOG_UPDATED("[SaveDiag] Restored %u actors back to having no cell after saving", count);
}

void RestoreActorsAfterSave()
{
    LOG_REQUIREMENT("CoSyncNpcWorldSync::RestoreActorsAfterSave", "Called right after the game finishes saving. Undoes the temporary cell fix from DiagnoseDisabledOriginalsForSave. Wrapped in a crash guard.");
    __try
    {
        RestoreActorsAfterSave_Impl();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[SaveDiag] Post-save restore hit a problem — caught it and cleared the tracking list");
        s_saveProtectedActors.clear();
    }
}

}
