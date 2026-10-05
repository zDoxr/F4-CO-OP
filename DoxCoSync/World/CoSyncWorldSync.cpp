#include "CoSyncWorldSync.h"
#include "CoSyncCombatAuthority.h"
#include "CoSyncWorldStateSync.h"
#include "CoSyncWorldSpawnSync.h"
#include "CoSyncNpcIdleSync.h"
#include "CoSyncSharedDialogue.h"
#include "CoSyncContainerSync.h"

#include <Windows.h>

#include "CoSyncRequirementsLog.h"
#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "TickHook.h"
#include "CoSyncGameAPI.h"
#include "CoSyncExplosionSync.h"
#include "CoSyncThrowableSync.h"
#include "CoSyncOutfitSync.h"

#include "GameReferences.h"
#include "GameEvents.h"
#include "GameForms.h"
#include "GameUtilities.h"
#include "GameRTTI.h"
#include "GameObjects.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"

#include <atomic>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>

// Was extern RelocPtr<PlayerCharacter*> g_player -- switched to CoSyncGetPlayer()
// (see GameGlobals.cpp), the established, confirmed-working replacement for
// raw g_player dereferences used throughout the rest of the codebase.
extern PlayerCharacter* CoSyncGetPlayer();
extern RelocPtr<GameVM*> g_gameVM;

// Replaces DYNAMIC_CAST(form, TESForm, TESObjectREFR) throughout this file.
// __RTDynamicCast has proven unreliable in this codebase (rounds 106-109).
// All placed-reference form types (REFR through PHZD) are TESObjectREFR
// subclasses, so a formType range-check + static_cast is safe and correct.
static TESObjectREFR* FormToREFR(TESForm* form)
{
    if (!form) return nullptr;
    const auto ft = form->formType;
    return (ft >= kFormType_REFR && ft <= kFormType_PHZD)
        ? static_cast<TESObjectREFR*>(form)
        : nullptr;
}

std::string CoSyncWorldSync::SerializeWorldSync(const WorldSyncPacket& p)
{
    LOG_REQUIREMENT("CoSyncWorldSync::SerializeWorldSync", "Just turns a packet struct into text. Doesn't touch the game at all.");
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "WS|" << p.entityID
       << "|" << p.posX << "|" << p.posY << "|" << p.posZ
       << "|" << p.rotZ
       << "|" << p.worldspaceFormID
       << "|" << p.cellFormID;
    return ss.str();
}

bool CoSyncWorldSync::DeserializeWorldSync(const std::string& msg, WorldSyncPacket& out)
{
    LOG_REQUIREMENT("CoSyncWorldSync::DeserializeWorldSync", "Reads a packet back out of text. Just needs a real string to read; bad or misspelled messages are safely rejected.");
    if (!IsWorldSync(msg)) return false;
    const char* s = msg.c_str() + 3;
    char* e = nullptr;
    out.entityID         = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return false; s = e + 1;
    out.posX             = std::strtof(s, &e);                              if (!e || *e != '|') return false; s = e + 1;
    out.posY             = std::strtof(s, &e);                              if (!e || *e != '|') return false; s = e + 1;
    out.posZ             = std::strtof(s, &e);                              if (!e || *e != '|') return false; s = e + 1;
    out.rotZ             = std::strtof(s, &e);                              if (!e || *e != '|') return false; s = e + 1;
    out.worldspaceFormID = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return false; s = e + 1;
    out.cellFormID       = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    return true;
}

std::string CoSyncWorldSync::SerializeWorkshopBuildFull(const WorkshopBuildFullPacket& p)
{
    LOG_REQUIREMENT("CoSyncWorldSync::SerializeWorkshopBuildFull", "Just turns a packet struct into text. Doesn't touch the game at all.");
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4);
    ss << "WBF|" << p.entityID
       << "|" << p.baseFormID
       << "|" << p.posX << "|" << p.posY << "|" << p.posZ
       << "|" << p.rotX << "|" << p.rotY << "|" << p.rotZ
       << "|" << p.worldspaceFormID << "|" << p.cellFormID
       << "|" << p.refFormID << "|" << p.workshopFormID;
    return ss.str();
}

bool CoSyncWorldSync::DeserializeWorkshopBuildFull(const std::string& msg, WorkshopBuildFullPacket& out)
{
    LOG_REQUIREMENT("CoSyncWorldSync::DeserializeWorkshopBuildFull", "Reads a build packet back out of text. Just needs a real string; anything that doesn't start with \"WBF|\" or is missing a piece is safely rejected.");
    if (!IsWorkshopBuildFull(msg)) return false;
    const char* s = msg.c_str() + 4;
    char* e = nullptr;
    out.entityID   = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return false; s = e + 1;
    out.baseFormID = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return false; s = e + 1;
    out.posX       = std::strtof(s, &e);                              if (!e || *e != '|') return false; s = e + 1;
    out.posY       = std::strtof(s, &e);                              if (!e || *e != '|') return false; s = e + 1;
    out.posZ       = std::strtof(s, &e);                              if (!e || *e != '|') return false; s = e + 1;
    out.rotX       = std::strtof(s, &e);                              if (!e || *e != '|') return false; s = e + 1;
    out.rotY       = std::strtof(s, &e);                              if (!e || *e != '|') return false; s = e + 1;
    out.rotZ       = std::strtof(s, &e);

    // Optional trailing fields (older peers omit them).
    out.worldspaceFormID = 0; out.cellFormID = 0; out.refFormID = 0; out.workshopFormID = 0;
    if (e && *e == '|') { s = e + 1; out.worldspaceFormID = static_cast<uint32_t>(std::strtoul(s, &e, 10)); }
    if (e && *e == '|') { s = e + 1; out.cellFormID       = static_cast<uint32_t>(std::strtoul(s, &e, 10)); }
    if (e && *e == '|') { s = e + 1; out.refFormID        = static_cast<uint32_t>(std::strtoul(s, &e, 10)); }
    if (e && *e == '|') { s = e + 1; out.workshopFormID   = static_cast<uint32_t>(std::strtoul(s, &e, 10)); }
    return true;
}

std::string CoSyncWorldSync::SerializeWorkshopScrapFull(const WorkshopScrapFullPacket& p)
{
    LOG_REQUIREMENT("CoSyncWorldSync::SerializeWorkshopScrapFull", "Just turns a scrapped-item packet into text. Doesn't touch the game at all.");
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "WDS|" << p.entityID
       << "|" << p.refFormID
       << "|" << p.baseFormID
       << "|" << p.posX << "|" << p.posY << "|" << p.posZ;
    return ss.str();
}

bool CoSyncWorldSync::DeserializeWorkshopScrapFull(const std::string& msg, WorkshopScrapFullPacket& out)
{
    LOG_REQUIREMENT("CoSyncWorldSync::DeserializeWorkshopScrapFull", "Reads a scrapped-item packet back out of text. Just needs a real string; anything that doesn't start with \"WDS|\" or is missing a piece is safely rejected.");
    if (!IsWorkshopScrapFull(msg)) return false;
    const char* s = msg.c_str() + 4;
    char* e = nullptr;
    out.entityID   = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return false; s = e + 1;
    out.refFormID  = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return false; s = e + 1;
    out.baseFormID = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return false; s = e + 1;
    out.posX       = std::strtof(s, &e);                              if (!e || *e != '|') return false; s = e + 1;
    out.posY       = std::strtof(s, &e);                              if (!e || *e != '|') return false; s = e + 1;
    out.posZ       = std::strtof(s, &e);
    return true;
}

static std::string SerializeFurniture(uint32_t refFormID, uint32_t actorEntityID, bool isSitting)
{
    LOG_REQUIREMENT("(global)::SerializeFurniture", "Just turns a few numbers into text. Doesn't touch the game at all.");
    std::ostringstream ss;
    ss << "FU|" << refFormID << "|" << actorEntityID << "|" << (isSitting ? 1 : 0);
    return ss.str();
}

static bool DeserializeFurniture(const std::string& msg,
                                  uint32_t& outRefFormID,
                                  uint32_t& outActorEntityID,
                                  bool&     outIsSitting)
{
    LOG_REQUIREMENT("(global)::DeserializeFurniture", "Reads a furniture packet back out of text. Just needs a real string; anything that doesn't start with \"FU|\" or is missing a piece is safely rejected.");
    if (msg.rfind("FU|", 0) != 0)
        return false;

    std::stringstream ss(msg.substr(3));
    std::string tok;

    if (!std::getline(ss, tok, '|') || tok.empty()) return false;
    outRefFormID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (outRefFormID == 0) return false;

    if (!std::getline(ss, tok, '|')) return false;
    outActorEntityID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));

    if (!std::getline(ss, tok, '|')) return false;
    outIsSitting = (tok == "1");

    return true;
}

static bool IsLootableBaseType(UInt8 ft)
{
    LOG_REQUIREMENT("(global)::IsLootableBaseType", "Just compares a number. Doesn't touch the game at all.");
    return ft == 35
        || ft == 43
        || ft == 44
        || ft == 47
        || ft == 48;
}


static bool IsDebrisBaseType(UInt8 ft)
{
    LOG_REQUIREMENT("(global)::IsDebrisBaseType", "Just compares a number. Doesn't touch the game at all.");
    return ft == 38
        || ft == 28
        || ft == 35;
}

namespace
{

    struct PendingFurnitureApply
    {
        uint32_t refFormID      = 0;
        uint32_t actorEntityID  = 0;
        bool     isSitting      = false;
    };

    std::mutex                         s_furnitureMutex;
    std::vector<PendingFurnitureApply> s_pendingFurniture;

    bool s_initialized = false;

    constexpr double kWS_BroadcastInterval = 5.0;
    constexpr float  kWS_PositionThreshold = 100.f;
    double   s_lastWSBroadcast             = 0.0;
    float    s_lastWSPosX                  = 0.f;
    float    s_lastWSPosY                  = 0.f;
    float    s_lastWSPosZ                  = 0.f;
    uint32_t s_lastWSWorldspace            = 0;

    std::mutex                        s_wsMutex;
    bool                              s_hasPendingWS        = false;
    CoSyncWorldSync::WorldSyncPacket  s_pendingWS{};
    bool                              s_hasAppliedFirstSync = false;

    // round 25: P2 crash -- ClientTickWorkshopBuilds_Impl's PlaceAtMe_Native
    // call faulted 3x in a row (identical garbage RAX=0x4189480205395305 each
    // time) immediately after a same-tick ClientTickWorldSync_Impl teleport
    // (firstSync=1, MoveRefrToPosition crossing cells/worldspaces). Each
    // individual fault WAS caught cleanly by PlaceAtMeOnPlayerSafe's __except
    // (see the "PlaceAtMe_Native faulted -- caught" log lines), but
    // afterward CoSync-VEH's "Known bad-function-pointer crash" recovery
    // kicked in repeatedly -- RIP itself had gone to garbage stack addresses
    // (0xD6DED6F438, then 0x1, 0x9C0, ...), meaning something deeper than
    // the data pointer was corrupted by then. The process died silently
    // right after, with no further log output. MoveRefrToPosition doesn't
    // synchronously finish settling the engine's per-cell/worldspace attach
    // state (parentCell pointers, 3D load) within the same tick it's called
    // -- hammering PlaceAtMe_Native 3x for queued WBF entries in that exact
    // same tick walked that still-transitioning state. Tracked here so
    // CoSyncWorldSync::Tick can defer the WBF/PlaceAtMe replay for a few
    // ticks after any teleport, not just the very first one.
    //
    // round 26: the next test crashed anyway -- NOT in WorkshopBuilds (the
    // round-25 gate held), but in ClientTickObjectMoves, which calls the
    // same risky native (MoveRefrToPosition, not PlaceAtMe_Native, but
    // sharing the same cell-attach engine internals) once per queued
    // debris-object move, completely unguarded by this counter. The P2 log
    // showed 8 backlogged DebrisSync RX OBJ moves applied in the same tick
    // as a fresh teleport; the crash (Fallout4.exe+0x22BCBD1, a plain `ret`
    // faulting on its own stack-read) resolved to call-stack frames ~0x200
    // bytes from PlaceAtMe_Native's real entry (0x1159DB0) -- confirmed via
    // live .pdata function boundaries, NOT the IDA database (which turned
    // out to be from a different Fallout4.exe build than the one installed
    // -- MD5 mismatch confirmed, so IDA's name/function recognition for
    // this region was actively misleading and is not trustworthy here).
    //
    // round 27: round 26's "fix" (gate by 5 *ticks*, not wall-clock time)
    // crashed again -- same PlaceAtMeOnPlayerSafe call, only 39ms after the
    // teleport log line. 5 ticks elapsing within 39ms requires CoSyncWorldSync
    // ::Tick to be running at >=150Hz, which is entirely plausible: it's
    // dispatched once per F4SE main-thread message-queue pump (see
    // CoSyncRuntime.cpp), effectively once per rendered frame, and an
    // uncapped frame rate in a simple just-loaded scene can easily exceed
    // that. A tick-count gate silently assumes a roughly-constant tick
    // duration; the engine's actual cell/worldspace attach settling is bound
    // by real wall-clock work (3D load, navmesh, etc.), not by how many
    // engine ticks happened to elapse. Switched to a wall-clock gate instead
    // -- see NowSeconds() below, same self-contained QueryPerformanceCounter
    // idiom CoSyncPlayer.cpp already uses for its own post-spawn settle delay
    // (kAnimGraphSettleDelay). This counter is shared by both call sites --
    // see Tick() below.
    double                           s_lastTeleportTime     = -1000.0;
    constexpr double kTeleportSettleSeconds = 1.0;

    // round 31: P2 still crashed in PlaceAtMe_Native 1.02-1.24s after a
    // teleport -- just PAST the 1.0s gate above, meaning it opened while the
    // engine was still mid-settle. The teleport in question was firstSync=1
    // (the player's initial join placement, crossing into a brand new
    // worldspace/cell -- full streaming/3D load/navmesh, not just a same-cell
    // position correction). A flat 1.0s budget tuned against ordinary
    // mid-session corrections was never going to cover that heavier case.
    // Track whether the most recent teleport was a first-sync (cross-
    // worldspace) one and require a longer settle window for it specifically,
    // instead of just inflating the constant for every case (which would
    // needlessly delay routine corrections that were already settling fine
    // in ~1s per the lack of any prior recurrence on those).
    bool             s_lastTeleportWasFirstSync = false;
    constexpr double kFirstSyncSettleSeconds    = 3.0;

    double NowSeconds()
    {
        LOG_REQUIREMENT("CoSyncWorldSync::NowSeconds", "Just reads the system clock. Works on any normal Windows PC.");
        LARGE_INTEGER f{}, c{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
    }

    // 2026-07-15: was 0.01 (effectively every frame) — 2,092 "[Pickup SCAN]"
    // log lines in a ~15-minute session and a full cell->objectList walk at
    // frame rate. Pickup disappearance is a backstop for the (now re-enabled)
    // event-driven PickupHook, so 1s latency is fine.
    constexpr double kPickup_PollInterval = 1.0;
    double                   s_lastPickupPoll          = 0.0;
    std::unordered_set<uint32_t> s_trackedLootableFormIDs;


    std::unordered_set<uint32_t> s_broadcastPickups;

    std::unordered_set<uint32_t> s_knownDynamicBuildings;

    // Recently applied workshop builds, for the duplicate-build guard in
    // ClientTickWorkshopBuilds_Impl. A host build is announced by BOTH the
    // instant Papyrus event and the periodic scan, and those two paths do not
    // reliably agree on the placed ref's formID -- so dedupe on (base form,
    // position, recency) instead, which does not depend on ref identity.
    struct RecentBuild
    {
        uint32_t baseFormID;
        float    x, y, z;
        uint32_t placedRefFormID;
        double   time;
    };
    std::vector<RecentBuild> s_recentBuilds;

    // Generous enough to absorb the pre-settle vs settled position difference
    // between the two announcement paths, tight enough that two pieces the
    // player deliberately stacked are still two pieces.
    constexpr float  kRecentBuildRadius = 60.0f;
    constexpr double kRecentBuildWindow = 10.0;

    // ── Build DELETION detection (2026-08-04) ───────────────────────────────
    // "Workshop buildings are not getting deleted when deleted by the other
    // person." The WDS| apply path has always worked; what was missing is a
    // PRODUCER. That used to be CoSyncWorkshopScrapHook, which is disabled
    // because its RVA points into a get-current-workshop helper rather than
    // ScrapReference (see CoSyncNet.cpp). Rather than hook an unverified
    // function on a hot path, detect deletions by polling -- the same choice
    // made for explosion detection, and for the same reason.
    //
    // Deliberately NOT "absent from the cell walk": a ref simply unloads when
    // the player walks away, which would fire false deletions constantly.
    // Instead each tracked ref is re-resolved by formID, which keeps working
    // for unloaded refs, and only a form that has genuinely vanished or been
    // flagged Deleted/Disabled counts as scrapped.
    // 2026-09-05: extended with rotation, cell/worldspace and the last
    // announce time so the scan can also detect MOVES (workshop move mode,
    // or the final drop after the settle window caught the placement
    // preview) and re-announce them with the same sender ref.
    struct BuildTrack
    {
        uint32_t baseFormID = 0;
        float    x = 0.f, y = 0.f, z = 0.f;
        float    rx = 0.f, ry = 0.f, rz = 0.f;
        uint32_t cellFormID = 0;
        uint32_t wsFormID   = 0;
        double   lastSent   = 0.0;
    };
    std::unordered_map<uint32_t, BuildTrack> s_buildTrack;

    // Receiver: (senderEntityID << 32 | senderRefFormID) -> local refFormID.
    // Lets a later WBF for the same sender ref MOVE our copy instead of
    // spawning a twin, and lets WDS scrap exactly the right object.
    std::unordered_map<uint64_t, uint32_t> s_remoteRefToLocalRef;

    inline uint64_t RemoteRefKey(uint32_t entityID, uint32_t refFormID)
    {
        return (static_cast<uint64_t>(entityID) << 32) | refFormID;
    }

    constexpr float  kBuildMoveThreshold     = 1.0f;
    constexpr float  kBuildRotThreshold      = 0.01f;
    constexpr double kBuildMoveResend        = 0.5;
    constexpr float  kBuildDedupeRadius      = 4.0f;
    constexpr float  kExteriorApplyRadius    = 12000.f;
    constexpr double kDeferredBuildTTL       = 600.0;
    constexpr double kDeferredRetryInterval  = 0.5;
    double           s_lastDeferredRetry     = 0.0;
    constexpr double kBuildSweepInterval = 0.5;
    double s_lastBuildSweep = 0.0;
    uint32_t s_wdsSent = 0;

    // Client build-scan funnel counters. The client broadcast builds in the
    // 23:48 session and ZERO in the 00:25 one, with no code path between them
    // that obviously explains it -- so instead of guessing, count exactly which
    // filter is eating the client's own placements.
    struct CbsDiag
    {
        uint32_t examined=0, notDynamicRef=0, badType=0, dynBase=0,
                 helper=0, alreadyKnown=0, notSettled=0, broadcast=0;
    };
    CbsDiag s_cbsDiag{};
    double  s_lastCbsLog = 0.0;
    constexpr double kCbsLogInterval = 5.0;

    // 2026-07-21: previously duplicated by hand into a SEPARATE local copy
    // inside ClientTickBuildScan_Impl ("kInternalHelpers", commented "Same
    // crash-blocked forms as HostTickObjects") -- confirmed via a live
    // session that the duplicate had gone stale: 0x001439D8 (added to THIS
    // set in build 168 after a host-side crash) was never copied over, so
    // the client kept broadcasting it and the HOST crashed receiving it,
    // same Fallout4.exe+0x165910F signature, just the opposite direction.
    // One shared set now -- both HostTickObjects_Impl and
    // ClientTickBuildScan_Impl reference this directly, so a future addition
    // only ever needs to happen once.
    const std::unordered_set<uint32_t> kWorkshopInternalHelperForms = {
        0x0023D900, // WorkshopFloraDamageHelperCorn
        0x0023D7DB, // WorkshopFloraDamageHelperCarrot
        0x0023D7DA, // WorkshopFloraDamageHelperTato
        0x0023D7D9, // WorkshopFloraDamageHelperVine
        0x0023D7D8, // WorkshopFloraDamageHelperMutfruit
        0x000A7692, // WorkshopFloraDamageHelperGeneric
        // 2026-07-10 runtime crash log: PlaceAtMe_Native faults on the
        // client for these two base forms (RVA 0x2F2355/0x2F2377).
        // 0x00034309 is a Power Armor FURN frame — placeable in workshop
        // but crashes when independently spawned on a remote client.
        // 0x0013FF17 is an unidentified FURN/STAT that faults the same way.
        0x0013FF17,
        0x00034309,
        // 2026-07-17: base=0x00054BD1 PlaceAtMe_Native faults with a WRITE
        // AV to Fallout4.exe+0x248F658 (LOCK CMPXCHG on a read-only page).
        // Confirmed deterministic across HOST and CLIENT in the same session.
        0x00054BD1,
        // 2026-07-20: combat-FX dynamic refs at an NPC camp; PlaceAtMe_Native
        // WRITE-AVs at Fallout4.exe+0x165910F on both machines.
        0x000CA064,
        0x000CA066,
        0x000EB2B4,
        // 2026-07-21: host in a heavily-built/populated city, client crashed
        // within a second of joining. Same signature, confirmed via
        // disassembly at the fault RIP: "F0 0F B1 4F 04" = LOCK CMPXCHG
        // [RDI+4], ECX -- an engine refcount release hitting a stale/bad
        // pointer, WRITE AV to a read-only image page. Unidentified base
        // form, same as 0x0013FF17 above.
        0x001439D8,
    };

    // A dynamic ref must survive this long before it can be broadcast as a
    // build — transient combat FX (muzzle-flash lights, explosion helpers)
    // share buildable base form types but never live this long.
    std::unordered_map<uint32_t, double> s_wbfFirstSeen;

    bool WbfSettleElapsed(uint32_t refFormID, double now)
    {
        // 2026-08-04: 2.0 -> 0.6. This gate is the ENTIRE cause of "workshop
        // building is slow to update" -- the network is not the problem, the
        // logs measured TX->RX at 11ms and spawn in the same millisecond. A
        // newly placed piece simply was not looked at for 2s.
        //
        // The gate exists to reject transient combat-FX dynamic refs that
        // briefly appear with buildable-looking form types (the round-27
        // 0x165910F storm at NPC camps). Those live for a frame or two, so
        // 0.6s still rejects them comfortably while cutting perceived build
        // latency by more than 3x. The static base block lists and form-type
        // filters remain the primary defence; this is only the second line.
        constexpr double kWbfSettleSeconds = 0.6;
        if (s_wbfFirstSeen.size() > 512)
        {
            for (auto it = s_wbfFirstSeen.begin(); it != s_wbfFirstSeen.end(); )
            {
                if (now - it->second > 30.0)
                    it = s_wbfFirstSeen.erase(it);
                else
                    ++it;
            }
        }
        auto it = s_wbfFirstSeen.find(refFormID);
        if (it == s_wbfFirstSeen.end())
        {
            s_wbfFirstSeen[refFormID] = now;
            return false;
        }
        return (now - it->second) >= kWbfSettleSeconds;
    }

    std::mutex               s_pickupMutex;
    std::vector<uint32_t>    s_pendingPickups;

    struct PendingBuildApply
    {
        CoSyncWorldSync::WorkshopBuildFullPacket p{};
        double firstSeen = 0.0;
    };
    std::mutex                     s_buildMutex;
    std::vector<PendingBuildApply> s_pendingBuilds;

    // Game-thread only: builds waiting for this machine to be in the right
    // worldspace/cell before they can be placed (see ApplyBuild_Impl).
    std::vector<PendingBuildApply> s_deferredBuilds;

    struct PendingScrapApply
    {
        uint32_t entityID   = 0;
        uint32_t refFormID  = 0;
        uint32_t baseFormID = 0;
        float    posX=0.f, posY=0.f, posZ=0.f;
    };
    std::mutex                      s_scrapMutex;
    std::vector<PendingScrapApply>  s_pendingBuildScraps;

    struct ObjState
    {
        NiPoint3 pos{ 0.f, 0.f, 0.f };
        NiPoint3 rot{ 0.f, 0.f, 0.f };
    };

    std::unordered_map<uint32_t, ObjState> s_objLastSent;

    // 0.3.3 (Stage 7): objects another machine is driving right now (formID ->
    // time we last applied its OBJ). The machine whose player holds/threw an
    // object is its only transform source; everyone else only applies.
    std::unordered_map<uint32_t, double> s_objRemoteDrivenAt;
    constexpr double kObj_RemoteDrivenHold = 1.5;   // s after its last OBJ
    bool   s_objBaselineBuilt = false;

    // Baseline cadence for ordinary, untouched debris: a full cell->objectList
    // walk, so it stays deliberately slow (see kPickup_PollInterval's note above
    // -- a frame-rate cell walk was already a measured problem once).
    constexpr double kObj_PollInterval  = 0.5f;
    constexpr float  kObj_MoveThreshold = 5.0f;
    double s_lastObjPoll = 0.0;

    // ── 2026-08-03: player-driven ("hot") object streaming ───────────────────
    // The complaint was that a carried/thrown object lags and moves slower than
    // the player. Three separate causes, all addressed here:
    //   (1) the host sampled at 2Hz (kObj_PollInterval) -> two discrete jumps/sec;
    //   (2) a 5-unit deadzone swallowed the small motion of a carried object;
    //   (3) ONLY the host ever broadcast OBJ (HostTickObjects lives inside the
    //       `if (IsHost())` branch of Tick) -- so when the CLIENT grabbed
    //       something, its local physics moved it while the host kept
    //       broadcasting the stale position, yanking it back every 0.5s. That
    //       rubber-band is why it felt like it "moves slower than you".
    // Fix: whichever machine's player is holding an object owns it, streams it
    // at kObj_FastPollInterval with a much smaller deadzone, and ignores
    // incoming OBJ for it. The hot set is tiny (usually 0-1 objects), so the
    // fast path resolves by formID instead of walking the cell list again.
    constexpr double kObj_FastPollInterval  = 0.05;  // 20Hz while player-driven
    constexpr float  kObj_FastMoveThreshold = 0.5f;  // vs 5.0 baseline
    // A thrown object keeps its owner (and its fast cadence) briefly after
    // release so the flight path syncs, not just the moment of the throw.
    constexpr double kObj_ThrowTailSeconds  = 5.0;
    // Only rescan the cell for a newly-grabbed ref this often (grabs are rare;
    // this scan only runs at all while a grab is unresolved).
    constexpr double kObj_GrabResolveInterval = 0.1;

    double s_lastObjFastPoll     = 0.0;
    double s_lastGrabResolve     = 0.0;
    // formID -> time the local player last held it (0 tail = still held).
    std::unordered_map<uint32_t, double> s_hotObjects;
    uint32_t s_grabbedFormID     = 0;   // resolved formID of what we're holding
    uint32_t s_lastGrabbedIndex  = 0;   // last seen BSPointerHandle index

    // [DebrisDiag] counters, flushed on an interval so a bad session is
    // diagnosable from the log alone without a rebuild.
    struct DebrisDiag
    {
        uint32_t fastSent      = 0;  // OBJ sent on the hot path
        uint32_t slowSent      = 0;  // OBJ sent on the baseline cell walk
        uint32_t applied       = 0;  // incoming OBJ applied
        uint32_t suppressedOwn = 0;  // incoming OBJ ignored (we own that object)
        uint32_t grabResolved  = 0;  // grabbed handle -> formID resolutions
        uint32_t grabUnresolved= 0;  // grab seen but no matching ref in cell
        uint32_t applyNoForm   = 0;  // incoming OBJ whose formID didn't resolve
        uint32_t hotExpired    = 0;  // objects that aged out of the hot set
    };
    DebrisDiag s_debrisDiag{};
    double     s_lastDebrisDiagLog = 0.0;
    constexpr double kDebrisDiagInterval = 5.0;

    struct PendingObjMove
    {
        uint32_t refFormID = 0;
        NiPoint3 pos{ 0.f, 0.f, 0.f };
        NiPoint3 rot{ 0.f, 0.f, 0.f };
    };
    std::mutex                   s_objMutex;
    std::vector<PendingObjMove>  s_pendingObjMoves;


    bool s_hasRequestedState = false;

    std::atomic<bool> s_legacyWorkshop{ true };
}


// ── Grabbed-object detection (IDA-verified 2026-08-03, 1.11.221) ────────────
// Derived from the engine's own `GetPlayerGrabbedRef` console command rather
// than guessed. Method: the format string "GetPlayerGrabbedRef >> (%08x)" lives
// at rva 0x024AF430 and has exactly ONE `lea` xref in .text, at 0x5D5AA0. Its
// handler (0x5D59B0) does:
//     mov  rax, [rip+0x2CFC908]        ; singleton
//     mov  r8d, [rax + 0xB5C]          ; <- the grabbed BSPointerHandle
//     ...
//     and  r10d, 0x1FFFFF              ; handle index (low 21 bits)
//     mov  eax, [rbx + 0x28]           ; candidate refr
//     shr  eax, 0xB                    ; refr's own handle index
//     cmp  eax, r10d                   ; engine's own validation
//     mov  eax, [rbx + 0x14]           ; -> formID it prints
// That last comparison is the useful part: a refr carries its own handle index
// at (*(uint32_t*)(refr + 0x28)) >> 11, so we can match "is this ref the one
// being held?" by comparing INDICES, with no handle-table walk and therefore
// no new global RVA to go stale (this codebase has been bitten by stale RVAs
// repeatedly -- see GameGlobals.cpp). We deliberately read the handle off
// CoSyncGetPlayer() (formID 0x14, proven reliable) instead of adopting the
// singleton at 0x032D22E0, which GameGlobals.cpp documents as the *stale*
// g_player address; +0xB5C sits comfortably inside PlayerCharacter either way.
namespace
{
    constexpr size_t   kPlayerGrabbedHandleOff = 0xB5C;
    constexpr size_t   kRefrHandleIndexOff     = 0x28;
    constexpr uint32_t kHandleIndexMask        = 0x1FFFFF;
    constexpr uint32_t kRefrHandleIndexShift   = 11;
}

// noinline + no noexcept is load-bearing under this project's /GL+/LTCG Release
// config -- see the long note in CoSyncPickupHook.cpp's identical helpers.
__declspec(noinline) static uint32_t ObjSafeReadU32(const void* base, size_t off)
{
    LOG_REQUIREMENT("(global)::ObjSafeReadU32", "Reads a number from memory. If the address turns out to be bad, it safely returns 0 instead of crashing the game.");
    __try
    {
        return *reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(base) + off);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Handle index of whatever the LOCAL player is currently holding, or 0.
static uint32_t GetLocalGrabbedHandleIndex()
{
    LOG_REQUIREMENT("(global)::GetLocalGrabbedHandleIndex", "Checks the player exists first, then safely reads what object they're holding. If that read ever goes wrong, it just reports 'nothing grabbed' instead of crashing.");
    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc) return 0;
    const uint32_t handle = ObjSafeReadU32(pc, kPlayerGrabbedHandleOff);
    if (handle == 0) return 0;
    return handle & kHandleIndexMask;
}

// A refr's own handle index, per the engine's validation above.
static uint32_t GetRefrHandleIndex(TESObjectREFR* ref)
{
    LOG_REQUIREMENT("(global)::GetRefrHandleIndex", "Checks the object reference exists first, then safely reads its handle index. Returns 0 if anything goes wrong.");
    if (!ref) return 0;
    return ObjSafeReadU32(ref, kRefrHandleIndexOff) >> kRefrHandleIndexShift;
}

// True while this machine's player owns the object (holding it, or inside the
// post-release throw tail). The owner streams it and ignores incoming OBJ.
static bool IsLocallyOwnedObject(uint32_t formID, double now)
{
    LOG_REQUIREMENT("(global)::IsLocallyOwnedObject", "Just checks a small internal list. An object you're still holding never expires, only one you've thrown or dropped can.");
    if (formID == 0) return false;
    auto it = s_hotObjects.find(formID);
    if (it == s_hotObjects.end()) return false;
    if (it->second == 0.0) return true;                       // still held
    return (now - it->second) < kObj_ThrowTailSeconds;        // throw tail
}

// 0.3.3: ObjectReference.SetMotionType (vanilla Papyrus native; Motion_Fixed=0,
// Motion_Dynamic=1, Motion_Keyframed=2). An object another machine is driving
// is keyframed here: local Havok stops shoving and dropping it between that
// machine's updates. It goes back to dynamic once the stream stops.
static void SetRefMotionType(TESObjectREFR* ref, SInt32 motionType)
{
    if (!ref) return;
    static const BSFixedString kSetMotionType("SetMotionType");
    VMArray<VMVariable> args;
    VMVariable vType; vType.Set(&motionType); args.Push(&vType);
    bool allowActivate = true;
    VMVariable vAct; vAct.Set(&allowActivate); args.Push(&vAct);
    CallFunctionNoWait(ref, kSetMotionType, args);
}

// Remote-driven objects whose stream has ended: back to dynamic. Both roles.
static void TickRemoteDrivenObjects(double now)
{
    for (auto it = s_objRemoteDrivenAt.begin(); it != s_objRemoteDrivenAt.end(); )
    {
        if (now - it->second < kObj_RemoteDrivenHold)
        {
            ++it;
            continue;
        }
        if (TESObjectREFR* ref = FormToREFR(LookupFormByID(it->first)))
            if (!(ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)))
                SetRefMotionType(ref, 1);   // Motion_Dynamic
        it = s_objRemoteDrivenAt.erase(it);
    }
}

// Emit one OBJ packet for a ref, updating the last-sent baseline.
static void SendObjMove(TESObjectREFR* ref, uint32_t fid)
{
    LOG_REQUIREMENT("(global)::SendObjMove", "Needs a valid object whose position can be read. Sends it in the same format the receiving side expects.");
    ObjState st;
    st.pos = ref->pos;
    st.rot = ref->rot;
    s_objLastSent[fid] = st;

    char buf[128];
    std::snprintf(buf, sizeof(buf),
        "OBJ|%u|%.2f|%.2f|%.2f|%.4f|%.4f|%.4f",
        fid,
        ref->pos.x, ref->pos.y, ref->pos.z,
        ref->rot.x, ref->rot.y, ref->rot.z);
    CoSyncTransport::Send(std::string(buf));
}

// Keeps s_hotObjects in step with what the local player is holding. Runs on
// BOTH host and client -- ownership is "whoever is holding it", not "the host".
__declspec(noinline) static void TickGrabOwnership_Impl(double now)
{
    LOG_REQUIREMENT("(global)::TickGrabOwnership_Impl", "Needs the player and their current location to exist. Only scans the room's object list while it's still figuring out what got grabbed.");
    const uint32_t idx = GetLocalGrabbedHandleIndex();

    // Released: stamp the throw tail so the flight keeps streaming from us.
    if (idx == 0)
    {
        if (s_grabbedFormID != 0)
        {
            s_hotObjects[s_grabbedFormID] = now;   // start tail
            LOG_INFO("[DebrisSync] released ref=0x%08X — throw tail %.1fs",
                s_grabbedFormID, kObj_ThrowTailSeconds);
            s_grabbedFormID = 0;
        }
        s_lastGrabbedIndex = 0;
        return;
    }

    // Same object we already resolved: just refresh "still held".
    if (idx == s_lastGrabbedIndex && s_grabbedFormID != 0)
    {
        s_hotObjects[s_grabbedFormID] = 0.0;
        return;
    }

    // New/unresolved grab. Find which ref carries this handle index. Only
    // reached while actually grabbing something, and throttled, so the cell
    // walk here is rare -- unlike the baseline scan it is not a per-tick cost.
    if ((now - s_lastGrabResolve) < kObj_GrabResolveInterval) return;
    s_lastGrabResolve = now;

    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc) return;
    TESObjectCELL* cell = reinterpret_cast<TESObjectREFR*>(pc)->parentCell;
    if (!cell) return;

    for (UInt32 i = 0; i < cell->objectList.count; ++i)
    {
        TESObjectREFR* ref = cell->objectList.entries[i];
        if (!ref || !ref->baseForm) continue;
        if (GetRefrHandleIndex(ref) != idx) continue;

        s_grabbedFormID    = ref->formID;
        s_lastGrabbedIndex = idx;
        if (s_objRemoteDrivenAt.erase(ref->formID))
            SetRefMotionType(ref, 1);   // it is ours now: back to normal physics
        s_hotObjects[s_grabbedFormID] = 0.0;   // held
        s_debrisDiag.grabResolved++;
        LOG_INFO("[DebrisSync] grabbed ref=0x%08X base=0x%08X (handleIdx=%u) — "
                 "now locally owned, streaming at %.0fHz",
            s_grabbedFormID, ref->baseForm->formID, idx,
            1.0 / kObj_FastPollInterval);
        return;
    }

    // Grabbed something we can't find in the cell list (an actor, a ref from a
    // neighbouring cell, ...). Not fatal -- it just stays on the baseline path.
    s_debrisDiag.grabUnresolved++;
}

// Streams every locally-owned object at the fast cadence, and retires ones
// whose throw tail has expired. Resolves by formID -- the hot set is tiny, so
// this deliberately does NOT walk the cell list.
__declspec(noinline) static void TickHotObjects_Impl(double now)
{
    LOG_REQUIREMENT("(global)::TickHotObjects_Impl", "Needs an active connection (the caller checks this first). Looks up each tracked object safely, even if its ID is stale.");
    if ((now - s_lastObjFastPoll) < kObj_FastPollInterval) return;
    s_lastObjFastPoll = now;

    for (auto it = s_hotObjects.begin(); it != s_hotObjects.end(); )
    {
        const uint32_t fid  = it->first;
        const double   tail = it->second;

        // Expire finished throws.
        if (tail != 0.0 && (now - tail) >= kObj_ThrowTailSeconds)
        {
            s_debrisDiag.hotExpired++;
            it = s_hotObjects.erase(it);
            continue;
        }
        ++it;

        TESObjectREFR* ref = FormToREFR(LookupFormByID(fid));
        if (!ref || !ref->baseForm) continue;
        if (ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled))
            continue;

        auto last = s_objLastSent.find(fid);
        if (last != s_objLastSent.end())
        {
            const NiPoint3& lp = last->second.pos;
            const float dx = ref->pos.x - lp.x;
            const float dy = ref->pos.y - lp.y;
            const float dz = ref->pos.z - lp.z;
            if ((dx*dx + dy*dy + dz*dz) <
                (kObj_FastMoveThreshold * kObj_FastMoveThreshold))
                continue;
        }

        SendObjMove(ref, fid);
        s_debrisDiag.fastSent++;
        LOG_UPDATED("[DebrisSync TX] hot OBJ ref=0x%08X pos=(%.0f,%.0f,%.0f)",
            fid, ref->pos.x, ref->pos.y, ref->pos.z);
    }
}

// Re-resolves every locally-placed build we are tracking and broadcasts WDS|
// for any that has been scrapped. Runs on BOTH roles: scrapping is done by
// whoever is holding the hammer, not by the host.
// 2026-09-05: shared by the host and client building scans. Records the ref
// in s_buildTrack (with rotation + placement context) and broadcasts a WBF
// that carries the sender ref + worldspace/cell so receivers can defer until
// they are in the right place and can later MOVE the same object.
static void AnnounceBuilding(TESObjectREFR* ref, TESObjectCELL* cell, double now, bool isMove)
{
    LOG_REQUIREMENT("(global)::AnnounceBuilding", "Requires a live ref with baseForm and the attached cell it sits in (callers already filtered both); reads TESObjectCELL::worldSpace (0xC8) for exterior cells.");
    const bool     interior = (cell->flags & TESObjectCELL::kFlag_IsInterior) != 0;
    const uint32_t wsFID    = (!interior && cell->worldSpace)
        ? reinterpret_cast<TESForm*>(cell->worldSpace)->formID : 0u;

    BuildTrack& t = s_buildTrack[ref->formID];
    t.baseFormID = ref->baseForm->formID;
    t.x = ref->pos.x; t.y = ref->pos.y; t.z = ref->pos.z;
    t.rx = ref->rot.x; t.ry = ref->rot.y; t.rz = ref->rot.z;
    t.cellFormID = cell->formID;
    t.wsFormID   = wsFID;
    t.lastSent   = now;

    CoSyncWorldSync::WorkshopBuildFullPacket p{};
    p.entityID         = CoSyncNet::GetMyEntityID();
    p.baseFormID       = ref->baseForm->formID;
    p.posX = ref->pos.x; p.posY = ref->pos.y; p.posZ = ref->pos.z;
    p.rotX = ref->rot.x; p.rotY = ref->rot.y; p.rotZ = ref->rot.z;
    p.worldspaceFormID = wsFID;
    p.cellFormID       = cell->formID;
    p.refFormID        = ref->formID;
    p.workshopFormID   = 0;

    CoSyncTransport::Send(CoSyncWorldSync::SerializeWorkshopBuildFull(p));
    LOG_UPDATED("[WorldSync] TX WBF %s: ref=0x%08X base=0x%08X pos=(%.0f,%.0f,%.0f) rot=(%.2f,%.2f,%.2f) ws=0x%08X cell=0x%08X",
        isMove ? "MOVE" : "NEW", ref->formID, p.baseFormID, p.posX, p.posY, p.posZ,
        p.rotX, p.rotY, p.rotZ, wsFID, cell->formID);
}

// Re-announces tracked buildings in the current cell whose transform changed
// since the last announce (workshop move mode, or a piece that was still
// being positioned when the settle window announced it).
static void TickBuildMoves(TESObjectCELL* cell, double now)
{
    LOG_REQUIREMENT("(global)::TickBuildMoves", "Requires the attached cell the player stands in; resolves each tracked ref by formID (safe for unloaded refs) and only touches refs that resolve to live, enabled REFRs.");
    for (auto& kv : s_buildTrack)
    {
        BuildTrack& t = kv.second;
        if (t.cellFormID != cell->formID) continue;
        if (now - t.lastSent < kBuildMoveResend) continue;

        TESObjectREFR* ref = FormToREFR(LookupFormByID(kv.first));
        if (!ref || !ref->baseForm) continue;
        if (ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)) continue;

        const float dx = ref->pos.x - t.x, dy = ref->pos.y - t.y, dz = ref->pos.z - t.z;
        const bool moved  = (dx * dx + dy * dy + dz * dz) >= (kBuildMoveThreshold * kBuildMoveThreshold);
        const bool turned = fabsf(ref->rot.x - t.rx) > kBuildRotThreshold ||
                            fabsf(ref->rot.y - t.ry) > kBuildRotThreshold ||
                            fabsf(ref->rot.z - t.rz) > kBuildRotThreshold;
        if (!moved && !turned) continue;

        AnnounceBuilding(ref, cell, now, true);
    }
}

__declspec(noinline) static void TickBuildDeletions_Impl(double now)
{
    LOG_REQUIREMENT("(global)::TickBuildDeletions_Impl", "Needs an active connection (the caller checks this first). Looking up each tracked build is safe even if its ID is stale.");
    if ((now - s_lastBuildSweep) < kBuildSweepInterval) return;
    s_lastBuildSweep = now;
    if (s_buildTrack.empty()) return;

    for (auto it = s_buildTrack.begin(); it != s_buildTrack.end(); )
    {
        const uint32_t fid = it->first;
        TESObjectREFR* ref = FormToREFR(LookupFormByID(fid));

        // Gone entirely, or still resolvable but flagged deleted/disabled ==
        // scrapped. An UNLOADED ref still resolves and is unflagged, so simply
        // walking out of the settlement does not trip this.
        const bool scrapped =
            (ref == nullptr) ||
            (ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)) != 0;

        if (!scrapped) { ++it; continue; }

        CoSyncWorldSync::WorkshopScrapFullPacket p{};
        p.entityID   = CoSyncNet::GetMyEntityID();
        p.refFormID  = fid;
        p.baseFormID = it->second.baseFormID;
        p.posX       = it->second.x;
        p.posY       = it->second.y;
        p.posZ       = it->second.z;

        CoSyncTransport::Send(CoSyncWorldSync::SerializeWorkshopScrapFull(p));
        s_wdsSent++;
        LOG_INFO("[WorkshopScrap TX] WDS ref=0x%08X base=0x%08X pos=(%.0f,%.0f,%.0f) "
                 "(total sent=%u)",
            fid, p.baseFormID, p.posX, p.posY, p.posZ, s_wdsSent);

        // Stop tracking it, and drop it from the known set so a later rebuild
        // at the same spot is treated as a genuinely new build.
        s_knownDynamicBuildings.erase(fid);
        for (auto m = s_remoteRefToLocalRef.begin(); m != s_remoteRefToLocalRef.end(); )
        {
            if (m->second == fid) m = s_remoteRefToLocalRef.erase(m);
            else ++m;
        }
        it = s_buildTrack.erase(it);
    }
}

static void TickBuildDeletions(double now)
{
    LOG_REQUIREMENT("(global)::TickBuildDeletions", "No special setup needed. If something goes wrong while it runs, the game keeps going instead of crashing.");
    __try
    {
        TickBuildDeletions_Impl(now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[WorkshopScrap] TickBuildDeletions crashed, recovered");
    }
}

static void TickPlayerDrivenObjects(double now)
{
    LOG_REQUIREMENT("(global)::TickPlayerDrivenObjects", "Just needs to be connected. If either of its two update steps fails, it's caught safely instead of crashing the game.");
    __try
    {
        TickGrabOwnership_Impl(now);
        TickHotObjects_Impl(now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[DebrisSync] TickPlayerDrivenObjects crashed, recovered "
                 "(skipping this tick's grab/hot pass)");
    }
}

static void LogDebrisDiag(double now)
{
    LOG_REQUIREMENT("(global)::LogDebrisDiag", "Just prints out the running debris-sync counters every few seconds, then resets them.");
    if ((now - s_lastDebrisDiagLog) < kDebrisDiagInterval) return;
    s_lastDebrisDiagLog = now;

    const DebrisDiag& d = s_debrisDiag;
    // Only speak up when something actually happened, so an idle session does
    // not spam the log (the round-133 42K-line OutfitSync lesson).
    if (d.fastSent || d.slowSent || d.applied || d.suppressedOwn ||
        d.grabResolved || d.grabUnresolved || d.applyNoForm || d.hotExpired)
    {
        LOG_INFO("[DebrisDiag] last %.0fs: fastSent=%u slowSent=%u applied=%u "
                 "suppressedOwn=%u grabResolved=%u grabUnresolved=%u "
                 "applyNoForm=%u hotExpired=%u | hot=%u grabbedForm=0x%08X",
            kDebrisDiagInterval,
            d.fastSent, d.slowSent, d.applied, d.suppressedOwn,
            d.grabResolved, d.grabUnresolved, d.applyNoForm, d.hotExpired,
            (unsigned)s_hotObjects.size(), s_grabbedFormID);
    }
    s_debrisDiag = DebrisDiag{};
}


static bool IsCraftingStation(TESObjectREFR* furniture);

// Round 106 audit: refFormID is peer-controlled (straight off an "FU|"
// furniture-sync packet) and was fed into LookupFormByID+DYNAMIC_CAST with
// zero SEH -- unlike this file's own HostTickObjects/HostTickObjects_Impl
// split. Called directly from CoSyncWorldSync::Tick (also unwrapped), so
// this had no backstop anywhere in the chain.
static void ApplyFurnitureToProxy_Impl(uint32_t refFormID, uint32_t actorEntityID, bool isSitting)
{
    LOG_REQUIREMENT("(global)::ApplyFurnitureToProxy_Impl", "Handles a missing furniture ID gracefully. Needs the other player's stand-in character to actually exist before it can sit them down.");

    if (refFormID != 0)
    {
        TESForm* form = LookupFormByID(refFormID);
        TESObjectREFR* refr = FormToREFR(form);
        if (refr && IsCraftingStation(refr))
        {
            LOG_TRACE("[WorldSync] FU receive-side blocked — crafting station ref=0x%08X entity=%u",
                refFormID, actorEntityID);
            return;
        }
    }

    CoSyncPlayer* player = g_CoSyncPlayerManager.GetPlayerByEntityID(actorEntityID);
    if (!player)
    {
        LOG_WARN("[WorldSync] FU: no proxy for entityID=%u", actorEntityID);
        return;
    }

    player->m_isSitting = isSitting;

    // 0.3.6: an NPC puppet does what its owner's NPC does with furniture
    // (chairs, benches, wall leans, counters): the engine seats it on the
    // SAME furniture and stands it up with its own animation.
    if (player->createType == CoSyncEntityType::NPC && player->actorRef && player->hasSpawned)
    {
        TESObjectREFR* furn = refFormID ? FormToREFR(LookupFormByID(refFormID)) : nullptr;
        if (isSitting)
        {
            player->m_npcFurnitureRef = refFormID;
            const bool ok = furn && CoSyncGameAPI::SnapActorIntoFurniture(player->actorRef, furn);
            LOG_UPDATED("[NpcSync] entity=%u Animation/Action State: uses furniture 0x%08X -> %s",
                actorEntityID, refFormID, ok ? "seated natively" : (furn ? "snap refused (no 3D yet?), retried on tick" : "furniture not in this world"));
            if (!furn)
                player->m_npcFurnitureRef = 0;
        }
        else
        {
            player->m_npcFurnitureRef = 0;
            const bool ok = CoSyncGameAPI::RequestActorStand(player->actorRef);
            LOG_UPDATED("[NpcSync] entity=%u Animation/Action State: leaves furniture 0x%08X -> %s",
                actorEntityID, refFormID, ok ? "engine stand-up requested" : "no process");
        }
    }

    LOG_INFO("[WorldSync] FU applied: entity=%u isSitting=%d", actorEntityID, isSitting ? 1 : 0);
}

__declspec(noinline) static void ApplyFurnitureToProxy(uint32_t refFormID, uint32_t actorEntityID, bool isSitting)
{
    LOG_REQUIREMENT("(global)::ApplyFurnitureToProxy", "Just needs normal arguments. If the other player sends a bad furniture ID, it's caught safely instead of crashing.");
    __try
    {
        ApplyFurnitureToProxy_Impl(refFormID, actorEntityID, isSitting);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[WorldSync] ApplyFurnitureToProxy crashed, recovered (refFormID=0x%08X entity=%u)",
            refFormID, actorEntityID);
    }
}

static bool IsCraftingStation(TESObjectREFR* furniture)
{
    LOG_REQUIREMENT("(global)::IsCraftingStation", "Safely returns false for a missing furniture object instead of crashing; otherwise just checks its type.");
    if (!furniture || !furniture->baseForm)
        return false;

    static const uint32_t kCraftingFormIDs[] = {

        0x00055B05u,
        0x000EE5EEu,
        0x001C4CC8u,


        0x00055B04u,


        0x000AF127u,
        0x000AF129u,
        0x001D8690u,


        0x000AF128u,
        0x00129A64u,


        0x00055B03u,
        0x001B9DB0u,


        0x00057FE2u,
        0x001DFC00u,


        0x000B5A46u,
        0x001D89C5u,
    };

    const uint32_t baseID = furniture->baseForm->formID;
    for (uint32_t fid : kCraftingFormIDs)
    {
        if (baseID == fid)
            return true;
    }
    return false;
}

namespace
{
    class FurnitureSink : public BSTEventSink<TESFurnitureEvent>
    {
    public:
        EventResult ReceiveEvent(TESFurnitureEvent* evn, void* ) override
        {
            LOG_REQUIREMENT("CoSyncWorldSync::FurnitureSink::ReceiveEvent", "Safely skips an incomplete event instead of crashing. Needs the player to exist so it can tell if this event is about them.");
            if (!evn || !evn->actor || !evn->furniture)
                return kEvent_Continue;

            PlayerCharacter* pPlayer = CoSyncGetPlayer();
            if (!pPlayer)
                return kEvent_Continue;
            if (reinterpret_cast<Actor*>(pPlayer) != evn->actor)
            {
                // 0.3.6: NPCs. Queued to the tick (this sink can run off the
                // main thread); see TickNpcFurnitureEvents.
                CoSyncWorldSync::QueueNpcFurnitureEvent(evn->actor->formID, evn->furniture->formID, !evn->isGettingUp);
                return kEvent_Continue;
            }

            if (IsCraftingStation(evn->furniture))
            {
                LOG_TRACE("[WorldSync] FU skipped — crafting station refFormID=0x%08X", evn->furniture->formID);
                return kEvent_Continue;
            }

            const uint32_t refFormID     = evn->furniture->formID;
            const uint32_t actorEntityID = CoSyncNet::GetMyEntityID();
            const bool     isSitting     = !evn->isGettingUp;

            CoSyncWorldSync::NotifyFurnitureUse(refFormID, actorEntityID, isSitting);
            return kEvent_Continue;
        }
    };

    FurnitureSink s_furnitureSink;
}

void CoSyncWorldSync::Init()
{
    LOG_REQUIREMENT("CoSyncWorldSync::Init", "Safe to call any time. If the furniture event system isn't ready yet, it just logs a warning instead of failing. Safe to call more than once too.");
    if (s_initialized)
        return;

    BSTEventDispatcher<TESFurnitureEvent>* pDisp = GetEventDispatcher<TESFurnitureEvent>();
    if (pDisp)
    {
        // REVERTED 2026-06-18 (round 26): round 23's CoSyncEventSinkSafety
        // replacement (calling the engine's real array-grow primitive
        // directly via a hand-built BSTArrayAllocatorFunctor) turned out to
        // be broken -- it crashed with a near-null READ fault inside
        // Fallout4.exe+0x1659539 on every single game load, confirmed via
        // CoSync_VEH log + resolve_sym (CoSyncWorldSync::Init+0xB2, this
        // exact line). The theoretical heap-corruption risk in plain
        // AddEventSink that motivated that "fix" was never actually
        // confirmed to fire in practice -- a known-working pre-session
        // backup uses this exact plain call and has never crashed on load.
        // Reverting to it: a real, 100%-reproducible crash beats a
        // theoretical, never-observed one.
        pDisp->AddEventSink(&s_furnitureSink);
        LOG_INFO("[WorldSync] TESFurnitureEvent sink registered");
    }
    else
    {
        LOG_WARN("[WorldSync] TESFurnitureEvent dispatcher not found");
    }

    s_lastWSBroadcast    = 0.0;
    s_lastWSPosX         = 0.f;
    s_lastWSPosY         = 0.f;
    s_lastWSPosZ         = 0.f;
    s_lastWSWorldspace   = 0;
    s_hasPendingWS       = false;
    s_hasAppliedFirstSync = false;
    s_lastPickupPoll     = 0.0;
    s_trackedLootableFormIDs.clear();
    s_broadcastPickups.clear();
    s_knownDynamicBuildings.clear();
    s_buildTrack.clear();
    s_remoteRefToLocalRef.clear();
    s_deferredBuilds.clear();
    s_lastDeferredRetry = 0.0;

    s_initialized = true;
    LOG_INFO("[WorldSync] Init OK");
}

void CoSyncWorldSync::Shutdown()
{
    LOG_REQUIREMENT("CoSyncWorldSync::Shutdown", "Safe to call any time, even if Init() was never called or already shut down.");
    BSTEventDispatcher<TESFurnitureEvent>* pDisp = GetEventDispatcher<TESFurnitureEvent>();
    if (pDisp)
        pDisp->RemoveEventSink(&s_furnitureSink);

    {
        std::lock_guard<std::mutex> lk(s_furnitureMutex);
        s_pendingFurniture.clear();
    }
    {
        std::lock_guard<std::mutex> lk(s_pickupMutex);
        s_pendingPickups.clear();
    }
    {
        std::lock_guard<std::mutex> lk(s_buildMutex);
        s_pendingBuilds.clear();
    }
    {
        std::lock_guard<std::mutex> lk(s_scrapMutex);
        s_pendingBuildScraps.clear();
    }

    s_trackedLootableFormIDs.clear();
    s_broadcastPickups.clear();
    s_knownDynamicBuildings.clear();
    s_initialized = false;
}

// Every player (host AND clients, 0.3.1) reports where it is: immediately on
// a worldspace or interior-cell change, otherwise every kWS_BroadcastInterval
// or after moving kWS_PositionThreshold. Before 0.3.1 only the host sent it,
// so nobody knew where a client was.
static void TickMyLocation(double now)
{
    LOG_REQUIREMENT("(global)::TickMyLocation", "Needs an active connection and a valid player (both checked first) before it reads the player's location and sends it out.");
    if (!CoSyncNet::IsConnected())
        return;

    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc)
        return;

    TESObjectREFR* pcRefr = reinterpret_cast<TESObjectREFR*>(pc);

    const float posX = pcRefr->pos.x;
    const float posY = pcRefr->pos.y;
    const float posZ = pcRefr->pos.z;
    const float rotZ = pcRefr->rot.z;


    TESWorldSpace* ws   = CALL_MEMBER_FN(pcRefr, GetWorldspace)();
    TESObjectCELL* cell = pcRefr->parentCell;
    const uint32_t wsID   = ws   ? reinterpret_cast<TESForm*>(ws)->formID : 0;
    const uint32_t cellID = cell ? cell->formID : 0;

    const float dx = posX - s_lastWSPosX;
    const float dy = posY - s_lastWSPosY;
    const float dz = posZ - s_lastWSPosZ;
    const float distSq = dx*dx + dy*dy + dz*dz;

    const bool intervalUp   = (now - s_lastWSBroadcast) >= kWS_BroadcastInterval;
    const bool movedFar     = distSq > (kWS_PositionThreshold * kWS_PositionThreshold);
    static uint32_t s_lastWSCell = 0;
    const bool worldChanged = (wsID != s_lastWSWorldspace) || (wsID == 0 && cellID != s_lastWSCell);

    if (!intervalUp && !movedFar && !worldChanged)
        return;

    s_lastWSBroadcast  = now;
    s_lastWSPosX       = posX;
    s_lastWSPosY       = posY;
    s_lastWSPosZ       = posZ;
    s_lastWSWorldspace = wsID;
    s_lastWSCell       = cellID;

    CoSyncWorldSync::WorldSyncPacket p{};
    p.entityID         = CoSyncNet::GetMyEntityID();
    p.posX             = posX;
    p.posY             = posY;
    p.posZ             = posZ;
    p.rotZ             = rotZ;
    p.worldspaceFormID = wsID;
    p.cellFormID       = cellID;

    CoSyncTransport::Send(CoSyncWorldSync::SerializeWorldSync(p));

    LOG_UPDATED("[WorldSync TX] WS sent: entity=%u pos=(%.0f,%.0f,%.0f) ws=0x%08X cell=0x%08X  reason=%s%s%s",
        p.entityID, posX, posY, posZ, wsID, cellID,
        worldChanged ? "WORLD_CHANGED " : "",
        movedFar     ? "MOVED_FAR "     : "",
        intervalUp   ? "INTERVAL"       : "");
}

// UPDATE 2026-06-18 (round 16): this function's 3 loops iterate
// cell->objectList.entries[] directly (up to several hundred raw engine
// pointers per call, dereferencing ->baseForm/->flags/->pos/->rot/->formID on
// each) with NO SEH protection at all -- unlike every other engine-memory
// touch in this codebase (every SafeRead*-style helper elsewhere has
// noinline+__try/__except). Confirmed reproducible (2/2 live tests): a crash
// inside the engine's own reference-cleanup code (a vtable dispatch on what
// looks like a dangling pointer, Fallout4.exe+0x1A7FC54) happens within ~16ms
// of this function running, immediately after a [Pickup SCAN] log line
// reporting up to 327 cellObjects just iterated by this exact loop, during
// active world load / a second player just connecting (this mod's established
// highest-risk window all session for reference/graph data not being fully
// constructed yet). Wrapping in noinline+SEH like everywhere else -- this is
// containment, not a proven fix for whatever upstream engine state issue
// triggers it, but it is the one unguarded engine-memory loop in this whole
// file and the timing match is exact.
__declspec(noinline) static void HostTickObjects_Impl(double now)
{
    LOG_REQUIREMENT("(global)::HostTickObjects_Impl", "Needs an active connection, a valid player, and a valid current location (all checked first) before it scans the objects around them.");
    if (!CoSyncNet::IsConnected())
        return;

    // FIX (2026-06-24): every connected test tonight died right here,
    // silently (no SEH catch despite the caller's own __try/__except, no
    // VEH catch) -- always on the 2nd of this function's 3 raw
    // cell->objectList walks, microseconds after the 1st walk completed
    // cleanly over the exact same 327 entries. Same memory, two passes,
    // one clean, one fatal -- points at a race against something else
    // mutating this cell in the brief gap between loops, not a bad
    // pointer per se, and it happens on literally the first tick after
    // connecting every single time (kScanInterval's very first window).
    // Matches this codebase's established "engine state not settled yet
    // right after a second player connects" bug class (e.g. round 125's
    // NpcAnimSync settle-delay). Gating the entire risky 3-loop cell scan
    // behind a settle delay from first-connected, same idea, to test
    // whether avoiding this exact timing window avoids the race.
    static double s_connectedSince = -1.0;
    if (s_connectedSince < 0.0)
        s_connectedSince = now;
    constexpr double kPostConnectSettleDelay = 3.0;
    if ((now - s_connectedSince) < kPostConnectSettleDelay)
        return;

    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc)
        return;

    TESObjectCELL* cell = reinterpret_cast<TESObjectREFR*>(pc)->parentCell;
    if (!cell)
        return;


    if ((now - s_lastPickupPoll) >= kPickup_PollInterval)
    {
        s_lastPickupPoll = now;

        std::unordered_set<uint32_t> current;
        for (UInt32 i = 0; i < cell->objectList.count; i++)
        {
            TESObjectREFR* ref = cell->objectList.entries[i];
            if (!ref || !ref->baseForm) continue;
            if (ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)) continue;
            if ((ref->formID & 0xFF000000) == 0xFF000000) continue;
            if (!IsLootableBaseType(ref->baseForm->formType)) continue;
            current.insert(ref->formID);
        }

        // Log only when the picture changes — at a fixed poll this line was
        // the single largest source of log noise in live sessions.
        static uint32_t s_lastLoggedTracked = ~0u, s_lastLoggedCurrent = ~0u;
        if ((uint32_t)s_trackedLootableFormIDs.size() != s_lastLoggedTracked ||
            (uint32_t)current.size() != s_lastLoggedCurrent)
        {
            s_lastLoggedTracked = (uint32_t)s_trackedLootableFormIDs.size();
            s_lastLoggedCurrent = (uint32_t)current.size();
            LOG_UPDATED("[Pickup SCAN] cell=0x%08X tracked=%u current=%u cellObjects=%u",
                cell->formID,
                (unsigned)s_trackedLootableFormIDs.size(),
                (unsigned)current.size(),
                (unsigned)cell->objectList.count);
        }


        if (!s_trackedLootableFormIDs.empty())
        {
            for (uint32_t fid : s_trackedLootableFormIDs)
            {
                if (!current.count(fid) && !s_broadcastPickups.count(fid))
                {


                    s_broadcastPickups.insert(fid);
                    std::string pkt = "PU|";
                    pkt += std::to_string(fid);
                    CoSyncTransport::Send(pkt);
                    LOG_UPDATED("[Pickup TX] Item removed: formID=0x%08X — broadcasting PU packet", fid);
                }
            }
        }

        s_trackedLootableFormIDs = std::move(current);
    }


    // BISECTION (2026-06-24): re-enabled. Never actually proven guilty --
    // see HostTickObjects's call site comment above. The real crash all
    // night was CoSyncGameAPI::SpawnRemoteActor's broken DYNAMIC_CAST,
    // fixed at its source.
    if (s_legacyWorkshop.load())
    {
    for (UInt32 i = 0; i < cell->objectList.count; i++)
    {
        TESObjectREFR* ref = cell->objectList.entries[i];
        if (!ref || !ref->baseForm) continue;
        if (ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)) continue;
        if ((ref->formID & 0xFF000000) != 0xFF000000) continue;
        // Dynamic BASE form (0xFF prefix) = runtime-only, not present on client.
        // e.g. 0xFF000B05 (workshop beacon) — always "not found" on client.
        if ((ref->baseForm->formID & 0xFF000000) == 0xFF000000) continue;
        {
            // Workshop-buildable base form types (STAT, ACTI, CONT, LIGH, SCOL, MSTT, FURN, TERM)
            const UInt8 ft = ref->baseForm->formType;
            if (ft != 27 && ft != 31 && ft != 34 && ft != 36 &&
                ft != 37 && ft != 38 && ft != 42 && ft != 55)
                continue;
        }

        // round 31 (real root cause, not the firstSync-settle-window guess):
        // base=0x0023D7D9 is MSTT editorID "WorkshopFloraDamageHelperVine"
        // (confirmed via direct Fallout4.esm record lookup) -- an
        // engine-internal helper the workshop flora-clearing system spawns
        // itself to track local weak-spot damage on plants, NOT something a
        // player ever places. It passed the formType filter above (MSTT is
        // a legitimately buildable type) and got broadcast as a WBF "real
        // building", but PlaceAtMe_Native faults trying to independently
        // respawn it on a remote client -- a category error, not a timing
        // one, which is why no settle-delay budget ever fixed it.
        //
        // round 32: the first attempt at this filter used
        // ref->baseForm->GetEditorID() + a "DamageHelper" substring check --
        // rebuilt, redeployed, freshly relaunched (confirmed via log
        // timestamps vs DLL write time), and base=0x0023D7D9 still got sent
        // and still crashed, completely unaffected. F4SE's own GameForms.h
        // comment on GetEditorID is the explanation: "Only returns string
        // for things that actually store the EDID ingame" -- MSTT is pure
        // scenery, exactly the kind of form type the shipped game strips
        // EditorID strings from to save memory, so the check was silently
        // comparing against an empty/null string every time. Re-scanned all
        // of Fallout4.esm directly (same from-scratch parser) for every
        // record with "DamageHelper" anywhere in its EDID -- this is the
        // complete, fixed, enumerable set of 6 vanilla engine-internal
        // helpers (all MSTT, all from the base game, not something that
        // grows over time): Corn/Carrot/Tato/Vine/Mutfruit/Generic. A static
        // formID set is strictly safer here than a runtime string accessor
        // that's proven unreliable for this exact form type, and exactly as
        // surgical since the set really is this small and fixed. See
        // kWorkshopInternalHelperForms (file scope, shared with
        // ClientTickBuildScan_Impl) for the full list and history.
        if (kWorkshopInternalHelperForms.count(ref->baseForm->formID))
            continue;

        if (s_knownDynamicBuildings.count(ref->formID)) continue;

        if (!WbfSettleElapsed(ref->formID, now)) continue;

        s_knownDynamicBuildings.insert(ref->formID);
        // Tracks it (deletion sweep + move detection) and broadcasts the WBF.
        AnnounceBuilding(ref, cell, now, false);
    }

    TickBuildMoves(cell, now);
    }


    if ((now - s_lastObjPoll) < kObj_PollInterval)
        return;
    s_lastObjPoll = now;

    for (UInt32 i = 0; i < cell->objectList.count; ++i)
    {
        TESObjectREFR* ref = cell->objectList.entries[i];
        if (!ref || !ref->baseForm) continue;
        if (ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)) continue;
        if ((ref->formID & 0xFF000000) == 0xFF000000) continue;
        if (!IsDebrisBaseType(ref->baseForm->formType)) continue;

        const uint32_t fid = ref->formID;

        // Objects this machine's player is holding/just threw are streamed by
        // TickHotObjects_Impl at 20Hz with a 0.5-unit deadzone. Skipping them
        // here keeps the slow 5-unit path from clobbering that baseline and
        // re-sending a stale position between fast ticks.
        if (IsLocallyOwnedObject(fid, now)) continue;

        // 0.3.3 (Stage 7): never re-broadcast an object someone else is
        // driving. The host applied the carrier's OBJ and this 2 Hz walk then
        // saw it "moved" and sent the (by then stale) position back out, so a
        // third machine received two streams for one carried object and it
        // jumped between them.
        {
            if (s_objRemoteDrivenAt.count(fid))
                continue;   // TickRemoteDrivenObjects releases it
        }

        auto it = s_objLastSent.find(fid);
        if (it == s_objLastSent.end())
        {

            ObjState st;
            st.pos = ref->pos;
            st.rot = ref->rot;
            s_objLastSent[fid] = st;
            continue;
        }


        const NiPoint3& lp = it->second.pos;
        const float dx = ref->pos.x - lp.x;
        const float dy = ref->pos.y - lp.y;
        const float dz = ref->pos.z - lp.z;
        if ((dx*dx + dy*dy + dz*dz) < (kObj_MoveThreshold * kObj_MoveThreshold))
            continue;

        SendObjMove(ref, fid);
        s_debrisDiag.slowSent++;

        LOG_UPDATED("[DebrisSync TX] OBJ moved: ref=0x%08X base=0x%08X pos=(%.0f,%.0f,%.0f)",
            fid, ref->baseForm->formID, ref->pos.x, ref->pos.y, ref->pos.z);
    }

    s_objBaselineBuilt = true;
}

static void HostTickObjects(double now)
{
    LOG_REQUIREMENT("(global)::HostTickObjects", "No special setup needed. If the object scan hits a problem, it's caught safely instead of crashing the game.");
    __try
    {
        HostTickObjects_Impl(now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[WorldSync] HostTickObjects crashed, recovered (skipping this tick's cell scan)");
    }
}

// Round 106 audit: pending.worldspaceFormID/cellFormID are peer/host-supplied
// (off a WorldSyncPacket) and were fed into LookupFormByID with zero SEH --
// same gap as ApplyFurnitureToProxy above, in the sibling Client-side Tick
// function. _Impl/Safe split (WorldSyncPacket is POD here, but the split
// keeps this consistent with every other fix in this round).
__declspec(noinline) static void ClientTickWorldSync_Impl(const CoSyncWorldSync::WorldSyncPacket& pending)
{
    LOG_REQUIREMENT("(global)::ClientTickWorldSync_Impl", "Needs a valid player. Looks up the location the host sent and moves the player there.");
    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc)
        return;

    // 2026-08-02 (build 224): the "client follows host" teleport is DISABLED.
    // It called MoveRefrToPosition on the LOCAL player (pcRefr), dragging the
    // client onto the host's position — wrong for co-op, where each player moves
    // independently. It was a silent no-op for months (MoveRefrToPosition RVA was
    // the wrong function, GameGlobals.cpp 0x01180A90); build 223 corrected the RVA
    // to 0x01180CD0 and ACTIVATED this latent bug, which (a) teleported the client
    // to the host every worldspace change / >3000u and (b) hung the game thread
    // when it ran MoveRefrToPosition on the player during an ext->int cell load
    // (freeze on transition). The cross-cell fix that IS wanted — relocating the
    // remote PROXY so it follows through doors — lives in
    // CoSyncGameAPI::ResyncActorCellMembership and is unaffected by this.
    //
    // 2026-08-03 — narrowed from "always off" to ONE-SHOT ON JOIN (item #5,
    // "on connection the client is forced into the host's world"). Build 224
    // was right that the CONTINUOUS follow is wrong for co-op: it re-teleported
    // the client on every worldspace change / >3000u and hung the game thread
    // during ext->int loads. But the very FIRST sync is a different thing
    // entirely — it is the join handshake that puts both players in the same
    // place to begin with, and without it the client spawns wherever its own
    // save left it and the two never meet.
    //
    // So: allow exactly the first packet through, then permanently stop. Every
    // later packet returns here, which preserves build 224's fix in full —
    // independent movement, no transition freezes — because the freeze case
    // was mid-session transitions, not the initial placement. The heavier
    // kFirstSyncSettleSeconds (3s vs 1s) budget already exists specifically to
    // cover this cross-worldspace first teleport; see s_lastTeleportWasFirstSync.
    //
    // This is the WORLD half of the "dedicated server" model: the host's world
    // is authoritative for WHERE you join. Character progress stays per-player.
    if (s_hasAppliedFirstSync)
        return;

    TESObjectREFR* pcRefr = reinterpret_cast<TESObjectREFR*>(pc);

    TESWorldSpace* myWS = CALL_MEMBER_FN(pcRefr, GetWorldspace)();
    const uint32_t myWSID = myWS ? reinterpret_cast<TESForm*>(myWS)->formID : 0;

    // (The old "already synced, but drifted >3000u or changed worldspace ->
    // re-teleport" block lived here. It is unreachable now that the one-shot
    // gate above returns on every packet after the first, and it WAS the
    // continuous-follow behaviour build 224 correctly removed. Deleted rather
    // than left dead so nobody re-enables it by deleting the gate.)
    (void)myWSID;

    TESObjectCELL* targetCell = nullptr;
    TESWorldSpace* targetWS   = nullptr;

    if (pending.worldspaceFormID != 0)
    {
        TESForm* wsForm = LookupFormByID(pending.worldspaceFormID);
        targetWS = wsForm ? reinterpret_cast<TESWorldSpace*>(wsForm) : nullptr;
    }

    if (pending.cellFormID != 0)
    {
        TESForm* cellForm = LookupFormByID(pending.cellFormID);
        targetCell = cellForm ? reinterpret_cast<TESObjectCELL*>(cellForm) : nullptr;
    }

    NiPoint3 pos{ pending.posX, pending.posY, pending.posZ };
    NiPoint3 rot{ 0.f, 0.f, pending.rotZ };
    UInt32   nullHandle = 0;

    const bool wasFirstSync = !s_hasAppliedFirstSync;

    LOG_UPDATED("[WorldSync] Client teleport: (%.0f,%.0f,%.0f) ws=0x%08X cell=0x%08X firstSync=%d",
        pos.x, pos.y, pos.z, pending.worldspaceFormID, pending.cellFormID,
        wasFirstSync ? 1 : 0);

    s_hasAppliedFirstSync = true;
    s_lastTeleportTime = NowSeconds();
    s_lastTeleportWasFirstSync = wasFirstSync;

    MoveRefrToPosition(pcRefr, &nullHandle, targetCell, targetWS, &pos, &rot);
}

__declspec(noinline) static void ClientTickWorldSyncSafe(const CoSyncWorldSync::WorldSyncPacket& pending)
{
    __try
    {
        ClientTickWorldSync_Impl(pending);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[WorldSync] ClientTickWorldSync crashed, recovered (skipping this teleport)");
    }
}

static void ClientTickWorldSync()
{
    CoSyncWorldSync::WorldSyncPacket pending{};
    bool has = false;
    {
        std::lock_guard<std::mutex> lk(s_wsMutex);
        if (s_hasPendingWS)
        {
            pending = s_pendingWS;
            has = true;
            s_hasPendingWS = false;
        }
    }
    if (!has)
        return;

    ClientTickWorldSyncSafe(pending);
}

// Round 106 audit: fid is peer-controlled (off a "PU|" pickup packet), fed
// into LookupFormByID+DYNAMIC_CAST with zero SEH. _Impl takes the vector by
// const-ref so the thin Safe wrapper's __try never shares scope with a
// non-trivial-dtor local (std::vector always has one, even for trivial T).
__declspec(noinline) static void ClientTickPickup_Impl(const std::vector<uint32_t>& pending)
{
    for (uint32_t fid : pending)
    {
        TESForm* form = LookupFormByID(fid);
        if (!form)
        {
            LOG_WARN("[WorldSync] PU client: formID=0x%08X not found", fid);
            continue;
        }
        TESObjectREFR* ref = FormToREFR(form);
        if (!ref)
        {
            LOG_WARN("[WorldSync] PU client: formID=0x%08X not a REFR", fid);
            continue;
        }

        // Direct flag-write (kFlag_IsDisabled) does not trigger the engine's
        // 3D unload / collision removal on an already-visible ref. Call
        // ObjectReference.Disable() through the VM so the engine removes the
        // mesh from the scene, matching what happens on the host's side.
        static const BSFixedString kDisable("Disable");
        VMArray<VMVariable> emptyArgs;
        CallFunctionNoWait(ref, kDisable, emptyArgs);
        LOG_UPDATED("[WorldSync] PU applied: formID=0x%08X Disable() queued", fid);
    }
}

__declspec(noinline) static void ClientTickPickupSafe(const std::vector<uint32_t>& pending)
{
    __try
    {
        ClientTickPickup_Impl(pending);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[WorldSync] ClientTickPickup crashed, recovered (skipping this batch)");
    }
}

static void ClientTickPickup()
{
    std::vector<uint32_t> pending;
    {
        std::lock_guard<std::mutex> lk(s_pickupMutex);
        pending.swap(s_pendingPickups);
    }
    if (pending.empty())
        return;

    ClientTickPickupSafe(pending);
}

// UPDATE 2026-06-20 (round 55): split out of ClientTickWorkshopBuilds --
// that function has a std::vector<PendingBuildApply> local (non-trivial
// destructor), and mixing that with __try/__except in the same function
// triggers C2712 (same restriction documented in CoSyncGameAPI.cpp's
// ResyncActorCellMembership split). pcRefr's +0x7E extra-data-cache flag
// can be stale/inconsistent (claims populated, backing pointer is garbage),
// crashing PlaceAtMe's internal extra-data check -- clear the flag so
// PlaceAtMe takes its own already-correct empty-cache path, and SEH-wrap
// the call itself as a safety net. See CoSyncGameAPI.cpp::SpawnRemoteActor
// for the full disassembly-backed proof.
// UPDATE 2026-06-20 (round 58): removed the flag-clear write (see
// CoSyncGameAPI.cpp::SpawnRemoteActor for the full reasoning) -- a
// controlled experiment after round 57's relaunch produced a NEW,
// more severe STATUS_HEAP_CORRUPTION fault at this exact call site.
// SEH wrap on the call itself stays as the only safety net.
// UPDATE 2026-06-20 (round 60): pass the real vm instead of nullptr -- see
// CoSyncGameAPI.cpp::SpawnRemoteActor for the full reasoning (testing
// whether vm=nullptr was leaving PlaceAtMe's ownership/refcount tracking
// under-retained, causing the deterministic STATUS_HEAP_CORRUPTION crash
// in its release logic).
// UPDATE 2026-06-20 (round 61): vm=real CONFIRMED to fix the heap
// corruption -- re-adding round 55's flag-clear write (wrongly blamed for
// the heap corruption in round 58; that was actually vm=nullptr). Both
// fixes are needed together -- see CoSyncGameAPI.cpp::SpawnRemoteActor.
__declspec(noinline) static TESObjectREFR* PlaceAtMeOnPlayerSafe(VirtualMachine* vm, TESObjectREFR* pcRefr, void** formPair)
{
    // UPDATE 2026-06-20 (round 67): was clearing only the upper 16 bits of
    // TESObjectREFR::unk7C (a single UInt32 field per f4se's GameReferences.h
    // -- +0x7E is just its high half). See CoSyncGameAPI.cpp::SpawnRemoteActor
    // round 67 notes for the full story. Clearing the whole field now.
    __try
    {
        auto* p = reinterpret_cast<uint8_t*>(pcRefr);
        uint32_t* field7C = reinterpret_cast<uint32_t*>(p + 0x7C);
        if (*field7C != 0)
            *field7C = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    EXCEPTION_RECORD excRecord{};
    __try
    {
        // f4se 0.7.9 added two trailing bool params to PlaceAtMe_Native (was 6 args, now 8) --
        // kept the same physical args in the same positions, false/false for the new ones.
        return PlaceAtMe_Native(vm, 0, reinterpret_cast<TESObjectREFR**>(formPair), pcRefr, 1, false, false, false);
    }
    // UPDATE 2026-06-20 (round 56): same diagnostic upgrade as
    // CoSyncGameAPI.cpp::SpawnRemoteActor -- capture the actual fault
    // address/code instead of swallowing it silently.
    __except ((excRecord = *GetExceptionInformation()->ExceptionRecord, EXCEPTION_EXECUTE_HANDLER))
    {
        uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        uintptr_t faultAddr = reinterpret_cast<uintptr_t>(excRecord.ExceptionAddress);
        LOG_WARN("[WorldSync] WBF client: PlaceAtMe_Native faulted -- code=0x%08X addr=0x%p (RVA=0x%llX) -- caught",
            excRecord.ExceptionCode, excRecord.ExceptionAddress,
            (unsigned long long)(faultAddr - moduleBase));
        return nullptr;
    }
}

// Round 106 audit: only the PlaceAtMe_Native call inside this loop had SEH
// (via PlaceAtMeOnPlayerSafe) -- the LookupFormByID(b.baseFormID) on the very
// same peer-controlled baseFormID, plus the pcRefr/cell/worldspace engine
// pointer work around it, were all unguarded. _Impl/Safe split, same as the
// other 4 fixes this round; PlaceAtMeOnPlayerSafe's own nested __try is kept
// as-is (double-guarding the riskiest single call is harmless).
//
// round 31 (P2 crash): the exact same baseFormID faulted in PlaceAtMe_Native
// 3 times in 9ms (each individually caught cleanly by PlaceAtMeOnPlayerSafe's
// own __except), but right after the 3rd one CoSync-VEH's "Known
// bad-function-pointer crash" recovery started firing repeatedly on garbage
// RIPs -- the same "process limps along on corrupted state after repeated
// PlaceAtMe_Native faults" failure mode already root-caused in rounds 25-27
// (MoveRefrToPosition/PlaceAtMe_Native don't synchronously finish settling
// engine state, and hammering them again on the still-bad input compounds
// whatever got corrupted instead of just re-failing cleanly). A formID that
// faulted once isn't going to succeed on retry #2/#3 moments later -- track
// it and skip permanently instead of calling the same known-bad native again.
namespace
{
    std::mutex            s_workshopFaultedMutex;
    std::vector<uint32_t> s_workshopFaultedBaseForms;

    bool HasWorkshopBaseFaulted(uint32_t baseFormID)
    {
        LOG_REQUIREMENT("(anonymous)::HasWorkshopBaseFaulted", "Just checks a small internal list. No game state needed.");
        std::lock_guard<std::mutex> lk(s_workshopFaultedMutex);
        for (uint32_t f : s_workshopFaultedBaseForms)
            if (f == baseFormID)
                return true;
        return false;
    }

    void RecordWorkshopBaseFault(uint32_t baseFormID)
    {
        LOG_REQUIREMENT("(anonymous)::RecordWorkshopBaseFault", "Just checks a small internal list. No game state needed.");
        std::lock_guard<std::mutex> lk(s_workshopFaultedMutex);
        for (uint32_t f : s_workshopFaultedBaseForms)
            if (f == baseFormID)
                return;
        s_workshopFaultedBaseForms.push_back(baseFormID);
        LOG_WARN("[WorldSync] WBF client: base=0x%08X now permanently skipped after a PlaceAtMe_Native fault",
            baseFormID);
    }
}
enum class BuildApplyResult { Applied, Deferred, Dropped };

// 2026-09-05: same base form already sitting at (almost) the same spot --
// shared saves, or an object both sides scanned. Search cell must be attached.
static TESObjectREFR* FindExistingBuildingNear(TESObjectCELL* searchCell, uint32_t baseFormID,
                                               const NiPoint3& pos, float radius)
{
    LOG_REQUIREMENT("(global)::FindExistingBuildingNear", "searchCell may be null (returns null); iterates searchCell->objectList raw entries so the caller must be SEH-wrapped and the cell attached.");
    if (!searchCell) return nullptr;
    TESObjectREFR* best = nullptr;
    float bestSq = radius * radius;
    for (UInt32 i = 0; i < searchCell->objectList.count; ++i)
    {
        TESObjectREFR* ref = searchCell->objectList.entries[i];
        if (!ref || !ref->baseForm) continue;
        if (ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)) continue;
        if (ref->baseForm->formID != baseFormID) continue;
        const float dx = ref->pos.x - pos.x;
        const float dy = ref->pos.y - pos.y;
        const float dz = ref->pos.z - pos.z;
        const float dSq = dx * dx + dy * dy + dz * dz;
        if (dSq < bestSq) { bestSq = dSq; best = ref; }
    }
    return best;
}

// Records a local ref we placed/matched for a remote build. useLocalTransform
// makes the tracker remember the ref's ACTUAL transform (matched/unmoved
// objects) so sub-unit placement differences never bounce back as MOVEs.
static void TrackLocalBuilding(TESObjectREFR* local, const CoSyncWorldSync::WorkshopBuildFullPacket& p,
                               double now, bool useLocalTransform)
{
    BuildTrack& t = s_buildTrack[local->formID];
    t.baseFormID = p.baseFormID;
    if (useLocalTransform)
    {
        t.x = local->pos.x; t.y = local->pos.y; t.z = local->pos.z;
        t.rx = local->rot.x; t.ry = local->rot.y; t.rz = local->rot.z;
    }
    else
    {
        t.x = p.posX; t.y = p.posY; t.z = p.posZ;
        t.rx = p.rotX; t.ry = p.rotY; t.rz = p.rotZ;
    }
    t.cellFormID = local->parentCell ? local->parentCell->formID : p.cellFormID;
    t.wsFormID   = p.worldspaceFormID;
    t.lastSent   = now;
    s_knownDynamicBuildings.insert(local->formID);
}

static void DisableAndDeleteLocalBuilding(TESObjectREFR* ref)
{
    static const BSFixedString kDelete("Delete");
    if (!ref) return;
    CoSyncGameAPI::DisableWorldActor(ref);
    VMArray<VMVariable> delArgs;
    CallFunctionNoWait(ref, kDelete, delArgs);
}

// One remote build. Deferred = the receiver is not in the object's
// worldspace/cell yet (retried by ProcessDeferredBuilds until TTL).
__declspec(noinline) static BuildApplyResult ApplyBuild_Impl(const PendingBuildApply& b, double now)
{
    LOG_REQUIREMENT("(global)::ApplyBuild_Impl", "Game thread only. Requires CoSyncGetPlayer() non-null with a parentCell (else Deferred); feeds peer-supplied formIDs into LookupFormByID; spawning goes through CoSyncGameAPI::SpawnItemAtPosition (CreateReferenceAtLocation, position+rotation at construction). Caller wraps in SEH.");
    const CoSyncWorldSync::WorkshopBuildFullPacket& p = b.p;

    if (HasWorkshopBaseFaulted(p.baseFormID))
        return BuildApplyResult::Dropped;

    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc) return BuildApplyResult::Deferred;
    TESObjectREFR* pcRefr = reinterpret_cast<TESObjectREFR*>(pc);
    TESObjectCELL* myCell = pcRefr->parentCell;
    TESWorldSpace* myWS   = CALL_MEMBER_FN(pcRefr, GetWorldspace)();
    const uint32_t myWSID = myWS ? reinterpret_cast<TESForm*>(myWS)->formID : 0u;
    if (!myCell) return BuildApplyResult::Deferred;

    // CreateReferenceAtLocation drops the object into the LOCAL player's
    // current cell regardless of the XYZ (see IsSpawnPositionUsable history
    // in NpcWorldSync). So a build must wait until this machine is in the
    // same interior cell, or the same worldspace within the loaded grid; for
    // exteriors the engine is then asked to re-home the ref by position.
    const bool exterior = (p.worldspaceFormID != 0);
    TESObjectCELL* searchCell = myCell;

    if (exterior)
    {
        if (myWSID != p.worldspaceFormID)
            return BuildApplyResult::Deferred;
        const float dx = pcRefr->pos.x - p.posX;
        const float dy = pcRefr->pos.y - p.posY;
        if (dx * dx + dy * dy > kExteriorApplyRadius * kExteriorApplyRadius)
            return BuildApplyResult::Deferred;

        if (p.cellFormID != 0)
        {
            TESForm* cf = LookupFormByID(p.cellFormID);
            if (cf && cf->formType == kFormType_CELL)
            {
                TESObjectCELL* pcell = static_cast<TESObjectCELL*>(cf);
                if (pcell->worldSpace == myWS && pcell->objectList.count > 0)
                    searchCell = pcell;
            }
        }
    }
    else if (p.cellFormID != 0)
    {
        if (myCell->formID != p.cellFormID)
            return BuildApplyResult::Deferred;
    }
    // Legacy packet (no context): apply in the current cell as before.

    NiPoint3 pos{ p.posX, p.posY, p.posZ };
    NiPoint3 rot{ p.rotX, p.rotY, p.rotZ };
    UInt32   nh = 0;
    const uint64_t key = (p.refFormID != 0) ? RemoteRefKey(p.entityID, p.refFormID) : 0ull;

    // 1) Known sender ref -> MOVE our existing copy. Position moves via
    //    MoveRefrToPosition (works for statics); a rotation change needs a
    //    respawn because MoveRefrToPosition/SetAngle rotation is dead for
    //    non-actor refs on 1.11.240 (IDA-confirmed by the spawn path below).
    if (key != 0)
    {
        auto mit = s_remoteRefToLocalRef.find(key);
        if (mit != s_remoteRefToLocalRef.end())
        {
            TESObjectREFR* local = FormToREFR(LookupFormByID(mit->second));
            if (local && !(local->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)))
            {
                const float dx = local->pos.x - pos.x, dy = local->pos.y - pos.y, dz = local->pos.z - pos.z;
                const bool moved  = (dx * dx + dy * dy + dz * dz) >= (kBuildMoveThreshold * kBuildMoveThreshold);
                const bool turned = fabsf(local->rot.x - rot.x) > kBuildRotThreshold ||
                                    fabsf(local->rot.y - rot.y) > kBuildRotThreshold ||
                                    fabsf(local->rot.z - rot.z) > kBuildRotThreshold;

                if (turned)
                {
                    TESForm* base = LookupFormByID(p.baseFormID);
                    TESObjectREFR* fresh = base ? CoSyncGameAPI::SpawnItemAtPosition(base, pos, rot) : nullptr;
                    if (fresh)
                    {
                        if (exterior)
                            MoveRefrToPosition(fresh, &nh, nullptr, myWS, &pos, &rot);
                        const uint32_t oldID = local->formID;
                        DisableAndDeleteLocalBuilding(local);
                        s_knownDynamicBuildings.erase(oldID);
                        s_buildTrack.erase(oldID);
                        s_remoteRefToLocalRef[key] = fresh->formID;
                        TrackLocalBuilding(fresh, p, now, false);
                        LOG_UPDATED("[WorldSync] WBF move+rotate applied: remote=0x%08X local 0x%08X -> respawned 0x%08X rot=(%.2f,%.2f,%.2f)",
                            p.refFormID, oldID, fresh->formID, rot.x, rot.y, rot.z);
                        return BuildApplyResult::Applied;
                    }
                    // Fall through to a position-only move if the respawn failed.
                }
                if (moved || turned)
                {
                    MoveRefrToPosition(local, &nh, exterior ? nullptr : myCell, exterior ? myWS : nullptr, &pos, &rot);
                    LOG_UPDATED("[WorldSync] WBF move applied: remote=0x%08X local=0x%08X -> (%.0f,%.0f,%.0f)",
                        p.refFormID, local->formID, pos.x, pos.y, pos.z);
                }
                TrackLocalBuilding(local, p, now, !(moved || turned));
                return BuildApplyResult::Applied;
            }
            s_remoteRefToLocalRef.erase(mit);
        }
    }

    // 2) Same object already exists here (shared save, or both sides scanned it).
    if (TESObjectREFR* existing = FindExistingBuildingNear(searchCell, p.baseFormID, pos, kBuildDedupeRadius))
    {
        if (key != 0)
            s_remoteRefToLocalRef[key] = existing->formID;
        TrackLocalBuilding(existing, p, now, true);
        LOG_UPDATED("[WorldSync] WBF matched existing ref=0x%08X base=0x%08X (no duplicate placed)",
            existing->formID, p.baseFormID);
        return BuildApplyResult::Applied;
    }

    // 3) Recent-build guard (2026-08-04): a second announcement of a build
    //    we placed moments ago, from a peer without a stable ref, repositions
    //    that piece instead of spawning a twin.
    {
        for (auto it = s_recentBuilds.begin(); it != s_recentBuilds.end(); )
        {
            if ((now - it->time) > kRecentBuildWindow) { it = s_recentBuilds.erase(it); continue; }
            if (it->baseFormID == p.baseFormID)
            {
                const float dx = it->x - p.posX, dy = it->y - p.posY, dz = it->z - p.posZ;
                if ((dx * dx + dy * dy + dz * dz) <= (kRecentBuildRadius * kRecentBuildRadius))
                {
                    TESObjectREFR* prev = FormToREFR(LookupFormByID(it->placedRefFormID));
                    if (prev && !(prev->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)))
                    {
                        MoveRefrToPosition(prev, &nh, exterior ? nullptr : myCell, exterior ? myWS : nullptr, &pos, &rot);
                        it->x = p.posX; it->y = p.posY; it->z = p.posZ;
                        it->time = now;
                        if (key != 0)
                            s_remoteRefToLocalRef[key] = prev->formID;
                        TrackLocalBuilding(prev, p, now, false);
                        LOG_UPDATED("[WorldSync] WBF duplicate suppressed: base=0x%08X reuses placed=0x%08X (repositioned)",
                            p.baseFormID, it->placedRefFormID);
                    }
                    return BuildApplyResult::Applied;
                }
            }
            ++it;
        }
    }

    // 4) Place a new one. Rotation goes into the spawn call (the only verified
    //    way to land a static ref's orientation on 1.11.240).
    TESForm* base = LookupFormByID(p.baseFormID);
    if (!base)
    {
        LOG_WARN("[WorldSync] WBF client: base 0x%08X not found", p.baseFormID);
        return BuildApplyResult::Dropped;
    }

    TESObjectREFR* placed = CoSyncGameAPI::SpawnItemAtPosition(base, pos, rot);
    if (!placed)
    {
        RecordWorkshopBaseFault(p.baseFormID);
        LOG_WARN("[WorldSync] WBF client: SpawnItemAtPosition failed for 0x%08X", p.baseFormID);
        return BuildApplyResult::Dropped;
    }

    // Exterior: the spawn landed in the player's current cell; let the engine
    // re-home the ref into the cell that owns this position.
    if (exterior)
        MoveRefrToPosition(placed, &nh, nullptr, myWS, &pos, &rot);

    if (key != 0)
        s_remoteRefToLocalRef[key] = placed->formID;
    TrackLocalBuilding(placed, p, now, false);
    s_recentBuilds.push_back({ p.baseFormID, p.posX, p.posY, p.posZ, placed->formID, now });

    LOG_UPDATED("[WorldSync] WBF applied: base=0x%08X placed=0x%08X pos=(%.0f,%.0f,%.0f) rot=(%.2f,%.2f,%.2f) ws=0x%08X cell=0x%08X remote=0x%08X",
        p.baseFormID, placed->formID, p.posX, p.posY, p.posZ, p.rotX, p.rotY, p.rotZ,
        p.worldspaceFormID, p.cellFormID, p.refFormID);
    return BuildApplyResult::Applied;
}

__declspec(noinline) static void ProcessDeferredBuilds_Impl(double now)
{
    LOG_REQUIREMENT("(global)::ProcessDeferredBuilds_Impl", "Game thread only; walks s_deferredBuilds applying each via ApplyBuild_Impl and keeps the ones that must wait for this machine to reach the right worldspace/cell (TTL kDeferredBuildTTL). Caller wraps in SEH.");
    size_t kept = 0;
    for (size_t i = 0; i < s_deferredBuilds.size(); ++i)
    {
        const PendingBuildApply& b = s_deferredBuilds[i];
        const BuildApplyResult r = ApplyBuild_Impl(b, now);
        if (r == BuildApplyResult::Deferred)
        {
            if (now - b.firstSeen < kDeferredBuildTTL)
            {
                if (kept != i) s_deferredBuilds[kept] = b;
                ++kept;
            }
            else
            {
                LOG_UPDATED_WARN("[WorldSync] WBF dropped after %.0fs: base=0x%08X ws=0x%08X cell=0x%08X never became reachable",
                    kDeferredBuildTTL, b.p.baseFormID, b.p.worldspaceFormID, b.p.cellFormID);
            }
        }
    }
    s_deferredBuilds.resize(kept);
}

__declspec(noinline) static void ProcessDeferredBuildsSafe(double now)
{
    __try
    {
        ProcessDeferredBuilds_Impl(now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[WorldSync] ClientTickWorkshopBuilds crashed, recovered (clearing this batch)");
        s_deferredBuilds.clear();
    }
}

static void ClientTickWorkshopBuilds()
{
    {
        std::lock_guard<std::mutex> lk(s_buildMutex);
        if (!s_pendingBuilds.empty())
        {
            s_deferredBuilds.insert(s_deferredBuilds.end(), s_pendingBuilds.begin(), s_pendingBuilds.end());
            s_pendingBuilds.clear();
        }
    }
    if (s_deferredBuilds.empty())
        return;

    const double now = NowSeconds();
    if (now - s_lastDeferredRetry < kDeferredRetryInterval)
        return;
    s_lastDeferredRetry = now;

    ProcessDeferredBuildsSafe(now);
}
// Round 106 audit: s.refFormID is peer-controlled (off a workshop-scrap
// packet), fed into LookupFormByID+DYNAMIC_CAST with zero SEH, plus this
// function also walks the raw engine cell->objectList array unguarded.
// _Impl/Safe split, same convention as the other fixes this round.
__declspec(noinline) static void ClientTickWorkshopScraps_Impl(const std::vector<PendingScrapApply>& pending)
{
    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc) return;
    TESObjectREFR* pcRefr = reinterpret_cast<TESObjectREFR*>(pc);
    TESObjectCELL* cell = pcRefr->parentCell;
    if (!cell) return;

    static const BSFixedString kDisable("Disable");

    for (const auto& s : pending)
    {
        // 2026-09-05: object we placed (or matched) for this sender's ref ->
        // exact match through the remote->local mapping.
        if (s.refFormID != 0)
        {
            auto mit = s_remoteRefToLocalRef.find(RemoteRefKey(s.entityID, s.refFormID));
            if (mit != s_remoteRefToLocalRef.end())
            {
                const uint32_t localRef = mit->second;
                TESObjectREFR* ref = FormToREFR(LookupFormByID(localRef));
                if (ref && !(ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)))
                {
                    VMArray<VMVariable> emptyArgs;
                    CallFunctionNoWait(ref, kDisable, emptyArgs);
                    LOG_UPDATED("[WorkshopScrap] Disabled mapped ref=0x%08X (remote 0x%08X) base=0x%08X",
                        localRef, s.refFormID, s.baseFormID);
                }
                CoSyncWorldSync::NotifyLocalBuildingRemoved(localRef);
                continue;
            }
        }

        // Static ref: same formID on all clients — disable directly
        if (s.refFormID != 0 && (s.refFormID & 0xFF000000) != 0xFF000000)
        {
            TESForm* form = LookupFormByID(s.refFormID);
            TESObjectREFR* ref = FormToREFR(form);
            if (ref && !(ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)))
            {
                VMArray<VMVariable> emptyArgs;
                CallFunctionNoWait(ref, kDisable, emptyArgs);
                LOG_UPDATED("[WorkshopScrap] Disabled static ref=0x%08X base=0x%08X",
                    s.refFormID, s.baseFormID);
            }
            CoSyncWorldSync::NotifyLocalBuildingRemoved(s.refFormID);
            continue;
        }

        // Dynamic ref (0xFF...): find by baseFormID + proximity in cell
        static constexpr float kScrapMatchRadiusSq = 128.f * 128.f;
        TESObjectREFR* best = nullptr;
        float bestDistSq = kScrapMatchRadiusSq;

        for (UInt32 i = 0; i < cell->objectList.count; ++i)
        {
            TESObjectREFR* ref = cell->objectList.entries[i];
            if (!ref || !ref->baseForm) continue;
            if ((ref->formID & 0xFF000000) != 0xFF000000) continue;
            if (ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)) continue;
            if (ref->baseForm->formID != s.baseFormID) continue;

            const float dx = ref->pos.x - s.posX;
            const float dy = ref->pos.y - s.posY;
            const float dz = ref->pos.z - s.posZ;
            const float dSq = dx*dx + dy*dy + dz*dz;
            if (dSq < bestDistSq) { bestDistSq = dSq; best = ref; }
        }

        if (best)
        {
            VMArray<VMVariable> emptyArgs;
            CallFunctionNoWait(best, kDisable, emptyArgs);
            CoSyncWorldSync::NotifyLocalBuildingRemoved(best->formID);
            LOG_UPDATED("[WorkshopScrap] Disabled dynamic ref=0x%08X base=0x%08X dist=%.1f",
                best->formID, s.baseFormID, sqrtf(bestDistSq));
        }
        else
        {
            LOG_UPDATED_WARN("[WorkshopScrap] No match for base=0x%08X pos=(%.0f,%.0f,%.0f)",
                s.baseFormID, s.posX, s.posY, s.posZ);
        }
    }
}

__declspec(noinline) static void ClientTickWorkshopScrapsSafe(const std::vector<PendingScrapApply>& pending)
{
    __try
    {
        ClientTickWorkshopScraps_Impl(pending);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[WorldSync] ClientTickWorkshopScraps crashed, recovered (skipping this batch)");
    }
}

static void ClientTickWorkshopScraps()
{
    std::vector<PendingScrapApply> pending;
    {
        std::lock_guard<std::mutex> lk(s_scrapMutex);
        pending.swap(s_pendingBuildScraps);
    }
    if (pending.empty())
        return;

    ClientTickWorkshopScrapsSafe(pending);
}

// Round 106 audit: m.refFormID is peer-controlled (off an "OBJ|" debris-move
// packet), fed into LookupFormByID+DYNAMIC_CAST with zero SEH -- last of the
// 5 unguarded Client-side Tick functions found in this file this round.
__declspec(noinline) static void ClientTickObjectMoves_Impl(const std::vector<PendingObjMove>& pending)
{
    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc) return;
    TESObjectREFR* pcRefr = reinterpret_cast<TESObjectREFR*>(pc);
    TESObjectCELL* myCell = pcRefr->parentCell;
    TESWorldSpace* myWS   = CALL_MEMBER_FN(pcRefr, GetWorldspace)();

    const double nowApply = NowSeconds();

    for (const auto& m : pending)
    {
        // Ownership: while THIS machine's player is holding (or has just
        // thrown) an object, we are authoritative for it. Applying the peer's
        // older position here is exactly what produced the rubber-band --
        // local physics moved it, the peer echoed a stale position back, and
        // the object visibly dragged behind the player.
        if (IsLocallyOwnedObject(m.refFormID, nowApply))
        {
            s_debrisDiag.suppressedOwn++;
            continue;
        }

        TESForm* form = LookupFormByID(m.refFormID);
        if (!form)
        {
            s_debrisDiag.applyNoForm++;
            LOG_UPDATED_WARN("[DebrisSync] OBJ: ref=0x%08X not found", m.refFormID);
            continue;
        }

        TESObjectREFR* ref = FormToREFR(form);
        if (!ref)
        {
            LOG_UPDATED_WARN("[DebrisSync] OBJ: ref=0x%08X not a REFR", m.refFormID);
            continue;
        }


        if (ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled))
            continue;

        NiPoint3 pos = m.pos;
        NiPoint3 rot = m.rot;
        UInt32 nh = 0;

        MoveRefrToPosition(ref, &nh, myCell, myWS, &pos, &rot);
        s_debrisDiag.applied++;

        // The applied position is now the baseline, and the object is
        // remote-driven: our own change detection must not echo it.
        if (s_objRemoteDrivenAt.find(m.refFormID) == s_objRemoteDrivenAt.end())
            SetRefMotionType(ref, 2);   // Motion_Keyframed while someone else drives it
        s_objRemoteDrivenAt[m.refFormID] = nowApply;
        s_objLastSent[m.refFormID] = ObjState{ m.pos, m.rot };

        LOG_UPDATED("[DebrisSync] OBJ applied: ref=0x%08X pos=(%.0f,%.0f,%.0f)",
            m.refFormID, pos.x, pos.y, pos.z);
    }
}

__declspec(noinline) static void ClientTickObjectMovesSafe(const std::vector<PendingObjMove>& pending)
{
    __try
    {
        ClientTickObjectMoves_Impl(pending);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[WorldSync] ClientTickObjectMoves crashed, recovered (skipping this batch)");
    }
}

static void ClientTickObjectMoves()
{
    std::vector<PendingObjMove> pending;
    {
        std::lock_guard<std::mutex> lk(s_objMutex);
        pending.swap(s_pendingObjMoves);
    }
    if (pending.empty())
        return;

    ClientTickObjectMovesSafe(pending);
}

// Mirrors the WBF detection loop in HostTickObjects_Impl but runs for the
// client role. Finds new 0xFF dynamic objects the local player built in
// workshop mode and broadcasts them so peers can PlaceAtMe a copy.
// Remotely-received builds are pre-marked in s_knownDynamicBuildings (by
// ClientTickWorkshopBuilds_Impl above) so they are never re-broadcast.
static void LogCbsDiag(double now)
{
    if ((now - s_lastCbsLog) < kCbsLogInterval) return;
    s_lastCbsLog = now;
    const CbsDiag& d = s_cbsDiag;
    if (d.examined || d.broadcast)
    {
        LOG_INFO("[CbsDiag] client build scan last %.0fs: examined=%u notDynRef=%u "
                 "badType=%u dynBase=%u helper=%u alreadyKnown=%u notSettled=%u "
                 "BROADCAST=%u",
            kCbsLogInterval, d.examined, d.notDynamicRef, d.badType, d.dynBase,
            d.helper, d.alreadyKnown, d.notSettled, d.broadcast);
    }
    s_cbsDiag = CbsDiag{};
}

__declspec(noinline) static void ClientTickBuildScan_Impl(double now)
{
    static double s_connectedSince = -1.0;
    if (s_connectedSince < 0.0) s_connectedSince = now;
    if ((now - s_connectedSince) < 3.0) return;

    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc) return;
    TESObjectCELL* cell = reinterpret_cast<TESObjectREFR*>(pc)->parentCell;
    if (!cell) return;

    for (UInt32 i = 0; i < cell->objectList.count; ++i)
    {
        TESObjectREFR* ref = cell->objectList.entries[i];
        if (!ref || !ref->baseForm) continue;
        if (ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)) continue;
        s_cbsDiag.examined++;
        if ((ref->formID & 0xFF000000u) != 0xFF000000u) { s_cbsDiag.notDynamicRef++; continue; }
        const UInt8 ft = ref->baseForm->formType;
        if (ft != 27 && ft != 31 && ft != 34 && ft != 36 &&
            ft != 37 && ft != 38 && ft != 42 && ft != 55)
            { s_cbsDiag.badType++; continue; }
        // Dynamic BASE form (0xFF prefix) = runtime-only, never resolvable on
        // the peer — HostTickObjects has this same filter; its absence here is
        // what produced the live "RX WBF: base=0xFF000B05 not found" spam
        // (2026-07-15 log): the client kept broadcasting a ref whose base
        // exists only in its own session.
        if ((ref->baseForm->formID & 0xFF000000u) == 0xFF000000u) { s_cbsDiag.dynBase++; continue; }
        if (kWorkshopInternalHelperForms.count(ref->baseForm->formID)) { s_cbsDiag.helper++; continue; }
        if (s_knownDynamicBuildings.count(ref->formID)) { s_cbsDiag.alreadyKnown++; continue; }

        if (!WbfSettleElapsed(ref->formID, now)) { s_cbsDiag.notSettled++; continue; }

        s_knownDynamicBuildings.insert(ref->formID);
        s_cbsDiag.broadcast++;
        // Tracks it (deletion sweep + move detection) and broadcasts the WBF.
        AnnounceBuilding(ref, cell, now, false);
    }

    TickBuildMoves(cell, now);
}

static void ClientTickBuildScan(double now)
{
    __try { ClientTickBuildScan_Impl(now); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[WorldSync] ClientTickBuildScan faulted -- caught");
    }
    // Outside the __try so the funnel still reports on a faulting scan -- if
    // the scan is dying early that is precisely what we need to see.
    LogCbsDiag(now);
}

// Mirrors the lootable-item disappearance detection in HostTickObjects_Impl,
// but runs on the client/proxy. When the local proxy picks up a MISC/WEAP/
// AMMO/KEYM/ALCH item, it vanishes from the client's cell; this scan catches
// the disappearance and broadcasts PU|formID so the host (and other peers via
// host relay) can disable the same ref in their worlds.
__declspec(noinline) static void ClientTickPickupScan_Impl(double now)
{
    static double s_connectedSince = -1.0;
    if (s_connectedSince < 0.0) s_connectedSince = now;
    if ((now - s_connectedSince) < 3.0) return;

    if ((now - s_lastPickupPoll) < kPickup_PollInterval)
        return;
    s_lastPickupPoll = now;

    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc) return;
    TESObjectCELL* cell = reinterpret_cast<TESObjectREFR*>(pc)->parentCell;
    if (!cell) return;

    std::unordered_set<uint32_t> current;
    for (UInt32 i = 0; i < cell->objectList.count; ++i)
    {
        TESObjectREFR* ref = cell->objectList.entries[i];
        if (!ref || !ref->baseForm) continue;
        if (ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)) continue;
        if ((ref->formID & 0xFF000000) == 0xFF000000) continue;
        if (!IsLootableBaseType(ref->baseForm->formType)) continue;
        current.insert(ref->formID);
    }

    if (!s_trackedLootableFormIDs.empty())
    {
        for (uint32_t fid : s_trackedLootableFormIDs)
        {
            if (!current.count(fid) && !s_broadcastPickups.count(fid))
            {
                s_broadcastPickups.insert(fid);
                std::string pkt = "PU|";
                pkt += std::to_string(fid);
                CoSyncTransport::Send(pkt);
                LOG_UPDATED("[Pickup TX CLIENT] Item removed: formID=0x%08X — broadcasting PU packet", fid);
            }
        }
    }

    s_trackedLootableFormIDs = std::move(current);
}

static void ClientTickPickupScan(double now)
{
    __try { ClientTickPickupScan_Impl(now); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[WorldSync] ClientTickPickupScan faulted -- caught");
    }
}

__declspec(noinline) static void TickProxyCellVisibility_Impl()
{
    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc) return;

    TESObjectREFR* pcRefr = reinterpret_cast<TESObjectREFR*>(pc);
    TESWorldSpace* myWS   = CALL_MEMBER_FN(pcRefr, GetWorldspace)();
    const uint32_t myWSID = myWS ? reinterpret_cast<TESForm*>(myWS)->formID : 0u;
    // Every true interior cell reports worldspaceFormID==0 -- comparing only
    // that field means two players in two DIFFERENT interiors (both ws=0)
    // read as "together" and get shown to each other. Fall back to comparing
    // the actual cellFormID (already on the wire via WorldSyncPacket, already
    // captured into lastRemoteCellFormID, previously unused) whenever either
    // side's worldspace is 0.
    const uint32_t myCellID = pcRefr->parentCell
        ? reinterpret_cast<TESForm*>(pcRefr->parentCell)->formID : 0u;

    // ── Local transition detection (list item #5) ───────────────────────────
    // A proxy is positioned by raw scene-graph writes, so when the LOCAL
    // player changes cell or worldspace (teleport, interior<->exterior) the
    // proxy's 3D is still attached to the world we just left -- it goes
    // missing even though nothing about the REMOTE player changed, so the
    // mismatch test below never fires and never repairs it.
    //
    // Detect our own transition and force every currently-visible proxy
    // through the same full rebuild the re-show path uses.
    static uint32_t s_lastLocalWSID   = 0xFFFFFFFFu;
    static uint32_t s_lastLocalCellID = 0xFFFFFFFFu;
    // Only a real world-scene teardown strands the proxy. Within one EXTERIOR
    // worldspace, walking across cell boundaries changes parentCell constantly
    // (and oscillates between two cells right at a boundary) but the worldspace
    // scene graph stays loaded around the player, so the proxy is NOT stranded
    // -- rebuilding on every exterior cell change stormed RefreshActor3D +
    // ClearAppliedOutfits (115 rebuilds of a single cell in one live session),
    // flickering proxies in and out and making them go invisible. Rebuild only
    // when the WORLDSPACE itself changes (interior<->exterior, cross-worldspace
    // fast travel) or, while in an interior (ws==0), when the cell changes
    // (interior<->interior). A cell-only change inside an exterior worldspace
    // no longer triggers a rebuild; the mismatch-based hide/show path below
    // still repairs any genuine worldspace divergence.
    const bool wsChanged           = (myWSID != s_lastLocalWSID);
    const bool interiorCellChanged = (myWSID == 0u) && (myCellID != s_lastLocalCellID);
    const bool localTransitioned =
        (s_lastLocalWSID != 0xFFFFFFFFu) &&
        (wsChanged || interiorCellChanged);
    if (localTransitioned)
        LOG_UPDATED("[WorldSync] %s: local ws 0x%08X cell 0x%08X (was ws 0x%08X cell 0x%08X)",
            wsChanged ? "Worldspace Change" : "Cell Change", myWSID, myCellID, s_lastLocalWSID, s_lastLocalCellID);
    s_lastLocalWSID   = myWSID;
    s_lastLocalCellID = myCellID;

    // 0.3.4: re-file misplaced proxies. A proxy is created in the cell the
    // LOCAL player is in at that moment, and puppets/players are then moved
    // with SetPosition, which never changes cell; the cell resync only runs
    // when a transform update arrives. So an NPC proxy spawned while this
    // player was elsewhere (0.3.3 client log: entities 65536/65538 spawned at
    // 23:45:21 while the client was in the Vault 111 editor cell) stayed filed
    // in that interior at exterior coordinates, never got 3D, and read STUCK
    // for the whole session. Every visible proxy is checked on a local
    // transition and every 2 s (cheap: a cell/grid compare; the engine move
    // only runs when the cell is wrong).
    static ULONGLONG s_nextRefileMs = 0;
    const ULONGLONG nowMs = GetTickCount64();
    const bool refileDue = localTransitioned || nowMs >= s_nextRefileMs;
    if (refileDue)
        s_nextRefileMs = nowMs + 2000;

    const auto& players = g_CoSyncPlayerManager.GetAllPlayers();
    for (const auto& kv : players)
    {
        CoSyncPlayer* proxy = kv.second.get();
        if (!proxy || proxy->isLocalPlayer)           continue;
        if (!proxy->actorRef || !proxy->hasSpawned)   continue;

        if (refileDue && !proxy->m_proxyHiddenForCell && !proxy->m_awaitingFirstTransform &&
            IsRegisteredRemoteActor(proxy->actorRef))
            CoSyncGameAPI::RefileMisplacedActor(proxy->actorRef);

        // 0.3.6: decide visibility BEFORE the local-transition rebuild. The
        // rebuild used to run first and the mismatch test then disabled the
        // proxy in the same frame (Disable+Enable+Disable on one actor: every
        // load in the 0.3.5 logs). A proxy that is about to be hidden is
        // rebuilt by the SHOW path when it comes back, not here.
        const bool haveRemoteLoc = proxy->lastRemoteWorldspaceID != 0xFFFFFFFFu;
        const bool bothInterior = haveRemoteLoc && (proxy->lastRemoteWorldspaceID == 0u) && (myWSID == 0u);
        const bool mismatch = haveRemoteLoc && (bothInterior
            ? (proxy->lastRemoteCellFormID != myCellID)
            : (proxy->lastRemoteWorldspaceID != myWSID));

        if (localTransitioned && !proxy->m_proxyHiddenForCell && !mismatch)
        {
            CoSyncGameAPI::RefreshActor3D(proxy->actorRef);
            if (proxy->m_lastQueuedWeaponFormID != 0)
            {
                proxy->m_postRefreshWeaponFormID = proxy->m_lastQueuedWeaponFormID;
                proxy->m_postRefreshWeaponDelay  = 60;
                proxy->m_weaponConfirmedEquipped = false;
            }
            CoSyncOutfitSync::ClearAppliedOutfits(proxy->entityID);
            LOG_UPDATED("[WorldSync] Local transition (ws 0x%08X cell 0x%08X) — rebuilding proxy "
                        "entity=%u so it does not stay attached to the old cell",
                myWSID, myCellID, proxy->entityID);
        }

        if (!haveRemoteLoc) continue; // no WS packet yet


        if (mismatch && !proxy->m_proxyHiddenForCell)
        {
            // 2026-07-21: was CallFunctionNoWait (async Papyrus) -- switched
            // to the native, synchronous DisableWorldActor for the same
            // reason as RefreshActor3D's fix: no VM-queue gap to land a save
            // (or anything else) in.
            CoSyncGameAPI::DisableWorldActor(proxy->actorRef);
            proxy->m_proxyHiddenForCell = true;
            LOG_UPDATED("[WorldSync] Proxy entity=%u HIDDEN: remote ws=0x%08X vs local ws=0x%08X",
                proxy->entityID, proxy->lastRemoteWorldspaceID, myWSID);
        }
        else if (!mismatch && proxy->m_proxyHiddenForCell)
        {
            // 2026-07-21: user reported the remote proxy (and its HP/AP
            // overlay) coming back broken/gone-for-good after returning
            // from the mismatched cell, not just delayed. A bare Enable()
            // after a Disable() is exactly the "invisible proxy skeleton"
            // bug this codebase already root-caused once before (see
            // RefreshActor3D's own history) -- Enable() alone does not
            // rerun the engine's full appearance/anim-graph rebuild pass;
            // only a fresh Disable()+Enable() cycle does. Re-showing now
            // runs that same full cycle instead of a bare Enable(), so the
            // proxy comes back in the same working state a first-time spawn
            // would produce, not whatever partially-torn-down state a plain
            // Enable() leaves it in.
            CoSyncGameAPI::RefreshActor3D(proxy->actorRef);
            proxy->m_proxyHiddenForCell = false;

            // ── Re-assert gear after the rebuild (list item #5) ──────────────
            // Reported: after a teleport or an interior<->exterior transition
            // the proxy comes back with no outfit and/or no weapon.
            //
            // RefreshActor3D's Enable() restores ARMOR through the biped
            // system but NOT individually-equipped weapons (round 143) -- which
            // is why ProcessWeaponVerifyQueue arms m_postRefreshWeaponDelay
            // whenever IT calls RefreshActor3D. This path called RefreshActor3D
            // without arming anything, so the weapon simply never came back.
            //
            // Delay 60 matches the verify-queue path: the Disable+Enable that
            // RefreshActor3D queues must actually land before the equip is
            // re-fired, or it equips into an actor that is still torn down.
            if (proxy->m_lastQueuedWeaponFormID != 0)
            {
                proxy->m_postRefreshWeaponFormID = proxy->m_lastQueuedWeaponFormID;
                proxy->m_postRefreshWeaponDelay  = 60;
                proxy->m_weaponConfirmedEquipped = false;
            }

            // Outfit: drop the "already applied" record so the next outfit
            // packet is treated as new and re-applied, instead of being
            // skipped as redundant against state the rebuild just discarded.
            CoSyncOutfitSync::ClearAppliedOutfits(proxy->entityID);

            LOG_UPDATED("[WorldSync] Proxy entity=%u SHOWN (full refresh, weapon=0x%08X re-queued, "
                        "outfit re-armed): remote ws=0x%08X matches local ws=0x%08X",
                proxy->entityID, proxy->m_postRefreshWeaponFormID,
                proxy->lastRemoteWorldspaceID, myWSID);
        }
    }
}

static void TickProxyCellVisibility()
{
    __try { TickProxyCellVisibility_Impl(); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[WorldSync] TickProxyCellVisibility faulted -- caught");
    }
}

static void TickNpcFurnitureEvents();   // below (0.3.6)
static void TickNpcSeatRetry(double now);
static void TickNpcStateLog(double now);

void CoSyncWorldSync::Tick(double now)
{
    TickNpcFurnitureEvents();
    TickNpcSeatRetry(now);
    TickNpcStateLog(now);

    {
        std::vector<PendingFurnitureApply> pending;
        {
            std::lock_guard<std::mutex> lk(s_furnitureMutex);
            pending.swap(s_pendingFurniture);
        }
        for (const auto& p : pending)
            ApplyFurnitureToProxy(p.refFormID, p.actorEntityID, p.isSitting);
    }

    if (!s_initialized)
        return;

    // Track the nearest static container to the player every tick so that the
    // InvAddFromWorld hook knows which container was looted (both host + client).
    if (CoSyncNet::IsConnected())
        CoSyncContainerSync::UpdateNearestContainer(now);
        CoSyncContainerSync::TickDeposits(now);

    // Player-driven object streaming runs on BOTH roles, deliberately outside
    // the IsHost() split below. Ownership of a carried/thrown object belongs to
    // whoever is holding it, not to the host -- before this, HostTickObjects
    // was the only OBJ producer, so a client's grab was never broadcast at all
    // and the host's stale 2Hz echo dragged the object back. Runs before the
    // role branch so the hot set is current when ClientTickObjectMoves applies.
    if (CoSyncNet::IsConnected())
    {
        TickPlayerDrivenObjects(now);
        TickRemoteDrivenObjects(now);
        LogDebrisDiag(now);

        // Explosions are caused by whoever pulled the trigger, so like debris
        // this is role-independent -- both sides detect and both sides replay.
        // World-object state first: a replicated destruction produces its own
        // explosion here, which the explosion scan must see before a duplicate
        // EXP from the sender is considered (0.3.5).
        CoSyncWorldStateSync::Tick(now);
        CoSyncWorldSpawnSync::Tick(now);
        CoSyncNpcIdleSync::Tick(now);
        CoSyncSharedDialogue::Tick(now);

        CoSyncExplosionSync::Tick(now);
        CoSyncExplosionSync::FlushPendingExplosions();

        // The projectile that CAUSES that explosion — grenades, Molotovs,
        // mines, missiles, mini nukes. Same role-independent reasoning.
        // FlushPendingThrowables is deliberately called every tick and not
        // throttled: it dead-reckons the in-flight stand-ins between the
        // 20Hz THRM updates, and throttling it would put the stutter back.
        CoSyncThrowableSync::Tick(now);
        CoSyncThrowableSync::FlushPendingThrowables(now);

        // Shots: sent by the machine that simulates the shooter, replayed
        // through the engine's own Fire everywhere else (0.3.5).
        CoSyncCombatAuthority::Tick(now);

        // Scrap/delete detection — role-independent for the same reason as
        // builds and explosions: the person swinging the hammer owns the event.
        if (s_legacyWorkshop.load())
            TickBuildDeletions(now);
    }

    // Location reports: both roles.
    if (CoSyncNet::IsConnected())
        TickMyLocation(now);

    if (CoSyncNet::IsHost())
    {
        // Proxies of players who are somewhere else are hidden on the host too.
        TickProxyCellVisibility();

        // BISECTION (2026-06-24): re-enabled. HostTickObjects (and its WBF
        // loop, disabled in earlier rounds tonight) was never actually
        // proven guilty -- every test died in SpawnRemotePlayerTask::Run's
        // CoSyncGameAPI::SpawnRemoteActor (a separate, F4SE-task-queue code
        // path untracked by g_lastTickStep, confirmed via lastTickStep
        // reading "RTGT:end" -- i.e. the whole main tick chain, this
        // function included, had already finished cleanly before that
        // crash). That bug (broken DYNAMIC_CAST on the freshly-spawned
        // actor) is now fixed at its source. Re-enabling this to actually
        // test it for the first time once spawning gets far enough to reach
        // a tick where it matters.
        HostTickObjects(now);

        // Apply workshop objects/scraps/pickups received FROM clients
        // (bidirectional sync). The host relays these to other peers in
        // OnReceive* above; applying them here makes the host's own world
        // reflect client builds, scraps, and pickups too. PlaceAtMe_Native
        // risk is the same as the client path, so reuse the same
        // battle-hardened helpers.
        ClientTickWorkshopBuilds();
        ClientTickWorkshopScraps();
        ClientTickPickup();

        // Drain OBJ moves received FROM clients. Before player-driven
        // streaming existed the host was the only OBJ producer, so nothing
        // ever queued here and this drain was unnecessary; now a client's
        // carried/thrown object arrives as OBJ and the host must both apply it
        // and, without this, would grow s_pendingObjMoves without bound.
        ClientTickObjectMoves();
    }
    else
    {
        ClientTickWorldSync();
        TickProxyCellVisibility();

        // round 25/26/27: see s_lastTeleportTime's declaration comment above
        // -- defer any call that invokes the engine's MoveRefrToPosition/
        // PlaceAtMe_Native for queued peer data until real wall-clock time
        // has passed since the last teleport, since the engine's per-cell/
        // worldspace attach state doesn't finish settling synchronously
        // within the same tick a teleport lands. Round 25 caught this for
        // ClientTickWorkshopBuilds (PlaceAtMe_Native, 3x uncaught-past-the-
        // 3rd-retry crash) but gated by tick count; round 26 found the same
        // gap in ClientTickObjectMoves (MoveRefrToPosition) and extended the
        // tick-count gate to cover it too -- but round 27 crashed again, in
        // the SAME PlaceAtMeOnPlayerSafe call, only 39ms after the teleport
        // log line. 5 ticks elapsing in 39ms implies >=150Hz, plausible for
        // an uncapped frame rate -- a tick-count gate doesn't bound real
        // time. Now gated on actual elapsed seconds instead.
        //
        // round 31: still crashed, 1.02-1.24s after a firstSync=1 (cross-
        // worldspace join) teleport specifically -- the flat 1.0s budget was
        // tuned against ordinary same-worldspace corrections and doesn't
        // cover the heavier full-cell/worldspace load a first-sync teleport
        // triggers. Use the longer kFirstSyncSettleSeconds budget when the
        // last teleport was that case.
        const double settleBudget = s_lastTeleportWasFirstSync ? kFirstSyncSettleSeconds : kTeleportSettleSeconds;
        const bool teleportSettled = (NowSeconds() - s_lastTeleportTime) >= settleBudget;

        if (teleportSettled)
            ClientTickWorkshopBuilds();

        ClientTickPickup();
        ClientTickWorkshopScraps();

        if (teleportSettled)
            ClientTickObjectMoves();

        // Scan for objects the client built in workshop mode and broadcast them.
        // Also scan for lootable items the client picked up and broadcast PU|
        // so the host (and other peers via relay) can disable them too.
        if (CoSyncNet::IsConnected())
        {
            if (s_legacyWorkshop.load())
                ClientTickBuildScan(now);
            ClientTickPickupScan(now);
        }


        if (!s_hasRequestedState && CoSyncNet::IsConnected())
        {
            s_hasRequestedState = true;
            CoSyncTransport::Send(std::string("REQSTATE"));
            LOG_INFO("[DebrisSync] Sent REQSTATE to host");
        }
    }
}

void CoSyncWorldSync::OnReceiveWorldSync(const WorldSyncPacket& p)
{
    // Host relays every player's location to everyone (senders ignore their
    // own echo below). The sender is checked by CoSyncNet (P5/N7 "WS").
    if (CoSyncNet::IsHost())
        CoSyncTransport::Send(SerializeWorldSync(p));

    if (p.entityID == CoSyncNet::GetMyEntityID())
        return;

    // Record where this player is: proxy visibility, placement, NPC binding,
    // voice range and the spawn watchdog all depend on it.
    CoSyncPlayer* proxy = g_CoSyncPlayerManager.GetPlayerByEntityID(p.entityID);
    if (proxy)
    {
        proxy->lastRemoteWorldspaceID = p.worldspaceFormID;
        proxy->lastRemoteCellFormID   = p.cellFormID;
    }

    // Only the HOST's location is the join placement (one-shot teleport).
    if (CoSyncNet::IsHost() || p.entityID != CoSyncNet::GetHostEntityID())
        return;

    std::lock_guard<std::mutex> lk(s_wsMutex);
    s_pendingWS    = p;
    s_hasPendingWS = true;

    LOG_UPDATED("[WorldSync] RX WS: entity=%u pos=(%.0f,%.0f,%.0f) ws=0x%08X",
        p.entityID, p.posX, p.posY, p.posZ, p.worldspaceFormID);
}

void CoSyncWorldSync::OnReceivePickup(const std::string& msg)
{
    // Host relays to all other clients, then falls through to apply the pickup
    // to its own world too. Before this fix, the early return meant the host
    // never disabled the ref when a client picked it up, so the item stayed
    // visible in the host's world indefinitely.
    if (CoSyncNet::IsHost())
        CoSyncTransport::Send(msg);

    const char* s = msg.c_str() + 3;
    char* e = nullptr;
    uint32_t formID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (e == s || formID == 0)
    {
        LOG_WARN("[WorldSync] Malformed PU packet");
        return;
    }

    LOG_UPDATED("[WorldSync] RX PU: formID=0x%08X (queued)", formID);

    std::lock_guard<std::mutex> lk(s_pickupMutex);
    s_pendingPickups.push_back(formID);
    // Suppress re-broadcasting by whichever cell scan runs locally (host or
    // client). Without this the scan would see the disabled ref as "gone" and
    // broadcast a duplicate PU| on the very next poll tick.
    s_broadcastPickups.insert(formID);
}

void CoSyncWorldSync::SetLegacyWorkshopSync(bool enabled)
{
    if (s_legacyWorkshop.exchange(enabled) != enabled)
        LOG_INFO("[WorldSync] Legacy cell-scan workshop sync %s", enabled ? "ENABLED" : "DISABLED (CoSyncWorkshopAuthority owns workshop objects)");
}

bool CoSyncWorldSync::IsLegacyWorkshopSync()
{
    return s_legacyWorkshop.load();
}

void CoSyncWorldSync::OnReceiveWorkshopBuildFull(const WorkshopBuildFullPacket& p)
{
    if (!s_legacyWorkshop.load())
        return;
    if (p.entityID == CoSyncNet::GetMyEntityID())
        return;

    LOG_UPDATED("[WorldSync] RX WBF: base=0x%08X pos=(%.0f,%.0f,%.0f) from entity=%u",
        p.baseFormID, p.posX, p.posY, p.posZ, p.entityID);

    if (CoSyncNet::IsHost())
        CoSyncTransport::Send(SerializeWorkshopBuildFull(p));

    // Both host and client apply received builds — host needs to place a copy
    // of client-built objects in its own world (bidirectional workshop sync).
    PendingBuildApply b{};
    b.p         = p;
    b.firstSeen = NowSeconds();

    std::lock_guard<std::mutex> lk(s_buildMutex);
    s_pendingBuilds.push_back(b);
}

bool CoSyncWorldSync::IsKnownUnsafePlaceAtMeForm(uint32_t baseFormID)
{
    return kWorkshopInternalHelperForms.count(baseFormID) != 0;
}

void CoSyncWorldSync::NotifyLocalBuildingRemoved(uint32_t refFormID)
{
    LOG_REQUIREMENT("CoSyncWorldSync::NotifyLocalBuildingRemoved", "No FO4 engine state involved -- bookkeeping only; safe for unknown refs.");
    if (refFormID == 0) return;
    s_knownDynamicBuildings.erase(refFormID);
    s_buildTrack.erase(refFormID);
    for (auto it = s_remoteRefToLocalRef.begin(); it != s_remoteRefToLocalRef.end(); )
    {
        if (it->second == refFormID) it = s_remoteRefToLocalRef.erase(it);
        else ++it;
    }
}

void CoSyncWorldSync::FillPlacementContext(TESObjectREFR* ref, WorkshopBuildFullPacket& p)
{
    LOG_REQUIREMENT("CoSyncWorldSync::FillPlacementContext", "ref may be null; reads ref->parentCell and TESObjectCELL::worldSpace (0xC8) -- caller must be on the game thread with a live ref.");
    p.worldspaceFormID = 0;
    p.cellFormID       = 0;
    if (!ref) return;
    TESObjectCELL* cell = ref->parentCell;
    if (!cell) return;
    p.cellFormID = cell->formID;
    if (!(cell->flags & TESObjectCELL::kFlag_IsInterior) && cell->worldSpace)
        p.worldspaceFormID = reinterpret_cast<TESForm*>(cell->worldSpace)->formID;
}

void CoSyncWorldSync::NotifyWorkshopBuildImmediate(const WorkshopBuildFullPacket& p, uint32_t placedRefFormID)
{
    if (!CoSyncNet::IsConnected())
        return;

    // Pre-seed the dedupe set with our OWN local ref formID so
    // HostTickObjects_Impl/ClientTickBuildScan_Impl's cell scan doesn't
    // rediscover this same object a couple seconds from now and re-broadcast
    // a redundant WBF for it.
    if (placedRefFormID != 0)
        s_knownDynamicBuildings.insert(placedRefFormID);

    CoSyncTransport::Send(SerializeWorkshopBuildFull(p));
    LOG_UPDATED("[WorldSync] TX WBF (instant): base=0x%08X pos=(%.0f,%.0f,%.0f) rot=(%.2f,%.2f,%.2f)",
        p.baseFormID, p.posX, p.posY, p.posZ, p.rotX, p.rotY, p.rotZ);
}

// ── 0.3.6: NPC furniture ────────────────────────────────────────────────────
namespace
{
    struct NpcFurnEvt { uint32_t actor, furn; bool sitting; };
    std::mutex              s_npcFurnMx;
    std::vector<NpcFurnEvt> s_npcFurnEvts;
}

void CoSyncWorldSync::QueueNpcFurnitureEvent(uint32_t actorFormID, uint32_t furnFormID, bool sitting)
{
    std::lock_guard<std::mutex> lk(s_npcFurnMx);
    if (s_npcFurnEvts.size() < 256)
        s_npcFurnEvts.push_back({ actorFormID, furnFormID, sitting });
}

// Main thread. For an NPC this machine SIMULATES (no network transforms
// arrive for it): tell everyone which furniture it uses. For a puppet (its
// transforms come from the owner): its own AI must not pick furniture the
// owner's NPC is not using -- stand it back up.
static void TickNpcFurnitureEvents()
{
    std::vector<NpcFurnEvt> evts;
    {
        std::lock_guard<std::mutex> lk(s_npcFurnMx);
        evts.swap(s_npcFurnEvts);
    }
    if (evts.empty() || !CoSyncNet::IsConnected())
        return;
    for (const NpcFurnEvt& e : evts)
    {
        CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByActorFormID(e.actor);
        if (!p || p->createType != CoSyncEntityType::NPC || p->isLocalPlayer)
            continue;
        if (!p->m_hasRealTransform)
        {
            if (IsCraftingStation(FormToREFR(LookupFormByID(e.furn))))
                continue;
            CoSyncWorldSync::NotifyFurnitureUse(e.furn, p->entityID, e.sitting);
            LOG_UPDATED("[NpcSync] entity=%u (simulated here) %s furniture 0x%08X -- sent",
                p->entityID, e.sitting ? "sits in / uses" : "gets up from", e.furn);
        }
        else if (e.sitting && e.furn != p->m_npcFurnitureRef && p->actorRef)
        {
            CoSyncGameAPI::RequestActorStand(p->actorRef);
            LOG_UPDATED("[NpcSync] entity=%u puppet's own AI took furniture 0x%08X the owner's NPC is not using -- standing it up",
                p->entityID, e.furn);
        }
    }
}

// 0.3.6 diagnostics: NPC Entity / AI / Movement / Animation / Weapon state,
// logged on transitions only (1 Hz scan).
static void TickNpcStateLog(double now)
{
    static double s_next = 0.0;
    if (now < s_next) return;
    s_next = now + 1.0;
    struct Last { int moving = -1; int weapon = -1; uint32_t furn = 0xFFFFFFFFu; int role = -1; };
    static std::unordered_map<uint32_t, Last> s_last;
    for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
    {
        CoSyncPlayer* p = kv.second.get();
        if (!p || p->createType != CoSyncEntityType::NPC || !p->actorRef || !p->hasSpawned)
            continue;
        const int role = p->m_hasRealTransform ? 1 : 0;   // 1 = puppet (owner elsewhere), 0 = simulated here
        const float v2 = p->m_lastVel.x * p->m_lastVel.x + p->m_lastVel.y * p->m_lastVel.y;
        const int moving = role ? (v2 > 100.f ? 1 : 0) : -2;
        const int weapon = p->m_prevNpcWeaponDrawn ? 1 : 0;
        Last& l = s_last[p->entityID];
        if (l.role == role && l.moving == moving && l.weapon == weapon && l.furn == p->m_npcFurnitureRef)
            continue;
        l.role = role; l.moving = moving; l.weapon = weapon; l.furn = p->m_npcFurnitureRef;
        LOG_UPDATED("[NpcSync] NPC Entity=%u actor=0x%08X AI State=%s Movement State=%s Animation/Action State=%s Weapon State=%s",
            p->entityID, reinterpret_cast<TESForm*>(p->actorRef)->formID,
            role ? "puppet (owner's AI decides)" : "simulated here (local AI)",
            moving == -2 ? "local" : (moving ? "moving" : "still"),
            p->m_npcFurnitureRef ? "in furniture" : "free",
            weapon ? "drawn" : "holstered");
    }
}

// Retry seats that were refused because 3D was still loading.
static void TickNpcSeatRetry(double now)
{
    static double s_next = 0.0;
    if (now < s_next) return;
    s_next = now + 1.0;
    for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
    {
        CoSyncPlayer* p = kv.second.get();
        if (!p || p->createType != CoSyncEntityType::NPC || !p->m_npcFurnitureRef || !p->actorRef || !p->hasSpawned)
            continue;
        if (TESObjectREFR* f = FormToREFR(LookupFormByID(p->m_npcFurnitureRef)))
            CoSyncGameAPI::SnapActorIntoFurniture(p->actorRef, f);   // no-op once seated there
    }
}

void CoSyncWorldSync::NotifyFurnitureUse(uint32_t refFormID, uint32_t actorEntityID, bool isSitting)
{
    if (!CoSyncNet::IsInitialized() || !CoSyncNet::IsConnected())
        return;

    const std::string msg = SerializeFurniture(refFormID, actorEntityID, isSitting);

    LOG_INFO("[WorldSync] TX FU: ref=0x%08X entity=%u sitting=%d",
        refFormID, actorEntityID, isSitting ? 1 : 0);

    CoSyncTransport::Send(msg);
}

void CoSyncWorldSync::OnReceiveFurniture(const std::string& msg)
{
    uint32_t refFormID     = 0;
    uint32_t actorEntityID = 0;
    bool     isSitting     = false;

    if (!DeserializeFurniture(msg, refFormID, actorEntityID, isSitting))
    {
        LOG_WARN("[WorldSync] Malformed FU packet");
        return;
    }

    if (CoSyncNet::IsHost())
        CoSyncTransport::Send(msg);

    if (actorEntityID == CoSyncNet::GetMyEntityID())
        return;

    {
        std::lock_guard<std::mutex> lk(s_furnitureMutex);
        s_pendingFurniture.push_back({ refFormID, actorEntityID, isSitting });
    }

    LOG_INFO("[WorldSync] RX FU: ref=0x%08X entity=%u sitting=%d",
        refFormID, actorEntityID, isSitting ? 1 : 0);
}

void CoSyncWorldSync::OnReceiveObjectMove(const std::string& msg)
{

    const char* s = msg.c_str() + 4;
    char* e = nullptr;

    uint32_t refFormID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (!e || *e != '|' || refFormID == 0) return;
    s = e + 1;

    float posX = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
    float posY = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
    float posZ = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
    float rotX = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
    float rotY = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
    float rotZ = std::strtof(s, &e);

    LOG_UPDATED("[DebrisSync] RX OBJ: ref=0x%08X pos=(%.0f,%.0f,%.0f)", refFormID, posX, posY, posZ);

    std::lock_guard<std::mutex> lk(s_objMutex);
    PendingObjMove m;
    m.refFormID = refFormID;
    m.pos = { posX, posY, posZ };
    m.rot = { rotX, rotY, rotZ };
    s_pendingObjMoves.push_back(m);
}

void CoSyncWorldSync::SendStateRequest()
{
    if (s_hasRequestedState) return;

}

void CoSyncWorldSync::OnReceiveStateRequest()
{
    if (!CoSyncNet::IsHost()) return;

    LOG_INFO("[DebrisSync] REQSTATE received — replaying %u displaced objects",
        (unsigned)s_objLastSent.size());

    for (const auto& kv : s_objLastSent)
    {
        const uint32_t fid   = kv.first;
        const ObjState& st   = kv.second;

        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "OBJ|%u|%.2f|%.2f|%.2f|%.4f|%.4f|%.4f",
            fid,
            st.pos.x, st.pos.y, st.pos.z,
            st.rot.x, st.rot.y, st.rot.z);

        CoSyncTransport::Send(std::string(buf));
    }
}

void CoSyncWorldSync::OnReceiveWorkshopScrapFull(const WorkshopScrapFullPacket& p)
{
    if (!s_legacyWorkshop.load())
        return;
    if (p.entityID == CoSyncNet::GetMyEntityID())
        return;

    LOG_UPDATED("[WorkshopScrap] RX WDS: ref=0x%08X base=0x%08X pos=(%.0f,%.0f,%.0f) from entity=%u",
        p.refFormID, p.baseFormID, p.posX, p.posY, p.posZ, p.entityID);

    if (CoSyncNet::IsHost())
        CoSyncTransport::Send(SerializeWorkshopScrapFull(p));

    // Both host and client apply received scraps — host needs to disable the
    // object in its own world when a client scraps it.
    std::lock_guard<std::mutex> lk(s_scrapMutex);
    s_pendingBuildScraps.push_back({ p.entityID, p.refFormID, p.baseFormID, p.posX, p.posY, p.posZ });
}
