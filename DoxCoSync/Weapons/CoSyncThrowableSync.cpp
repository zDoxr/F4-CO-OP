#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncThrowableSync.h"

#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncGameAPI.h"
#include "CoSyncPlayer.h"
#include "CoSyncPlayerManager.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameRTTI.h"
#include "GameObjects.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"

#include <mutex>
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

extern PlayerCharacter* CoSyncGetPlayer();
extern RelocAddr<_LookupFormByID> LookupFormByID;

namespace
{
    // ── VISUALS DISABLED 2026-08-04 (user request) ──────────────────────────
    // The cosmetic in-flight stand-in kept appearing on the peer as a second,
    // droppable grenade-shaped object rather than a live projectile. Rather
    // than keep iterating, the whole visual is switched off: no THRP/THRM/THRX
    // traffic leaves this machine and no stand-in is ever spawned.
    //
    // Explosions still sync via CoSyncExplosionSync, so a thrown grenade still
    // detonates correctly for everyone -- the only thing lost is watching the
    // object travel through the air.
    //
    // Everything below is intact and re-enables by flipping this one flag.
    constexpr bool kThrowableVisualsEnabled = false;

    // 2026-08-04: this was hardcoded to 62 on the first pass, from reading
    // line positions in the GameForms.h enum rather than its values. PROJ is
    // 51. The type check therefore matched nothing, the poll rejected every
    // ref in the cell, and the whole module logged not one line through a
    // session with four grenades thrown. Use the SYMBOL -- there is no reason
    // to restate the number, and a wrong one fails completely silently.
    constexpr UInt8 kFormType_PROJ_Local = kFormType_PROJ;

    // ── Filter tuning ───────────────────────────────────────────────────────
    // Vanilla reference points (Fallout4.esm PROJ records):
    //   GrenadeFragProjectile   speed ~1500   gravity 1.0   explosion yes
    //   MolotovProjectile       speed ~1400   gravity 1.0   explosion yes
    //   MineFragProjectile      speed ~ 700   gravity 1.0   explosion yes
    //   40mmGrenadeProjectile   speed ~3000   gravity 0.5   explosion yes
    //   MissileProjectile       speed ~4400   gravity 0     explosion yes
    //   MiniNukeProjectile      speed ~2000   gravity 0.3   explosion yes
    //   (any bullet)            speed 30000+  gravity 0     explosion no
    // The cap sits above every launcher and far below any bullet, so an
    // explosive legendary round -- a bullet-speed projectile that DOES carry
    // an explosion -- is excluded. That is deliberate, not an oversight: it
    // covers its whole flight inside one frame, so streaming it would cost a
    // packet per round of an automatic weapon and change nothing on screen.
    // The EXPL replay in CoSyncExplosionSync already handles its impact.
    constexpr float kMaxSyncableSpeed = 6000.0f;

    // Detection + send rate. Faster than the explosion poll (0.1s): a grenade
    // is only airborne for ~1-2s, and at 0.1s we would get ~10 samples for the
    // whole arc.
    constexpr double kPollInterval = 0.05;

    // Hard ceiling on concurrently streamed projectiles. A cluster of mines or
    // a Fat Man volley must never turn into an unbounded per-frame broadcast.
    constexpr size_t kMaxTracked = 24;

    // Receiver-side safety nets. kStaleTTL fires when THRX is lost (the sender
    // crashed, the packet dropped, the projectile left the sender's cell
    // before detonating); kMaxLifetime is the absolute backstop so no code
    // path can strand a grenade model in someone's world forever.
    // 2026-08-04 (list item #1): both tightened. A stand-in that stops being
    // updated is, by definition, no longer following a live projectile -- it
    // is just an item about to drop to the floor. There is no reason to leave
    // it there for seconds. A real grenade's whole flight is 1-2s, so a 1s
    // silence already means the stream is over, and 8s is a generous ceiling
    // for the slowest arcing throw.
    constexpr double kStaleTTL     = 1.0;
    constexpr double kMaxLifetime  = 8.0;

    // ── Launch anchoring ────────────────────────────────────────────────────
    // 2026-08-04 round 3: the grenade appeared off to the side of the throwing
    // proxy. The wire data was provably exact (client TX pos == host stand-in
    // pos to the unit), so the position was never the problem -- the ANCHOR
    // was. Measured at launch, host thrower origin (-21054,-39336,2204) vs its
    // own grenade (-21038,-39384,2319): +115 in Z is camera height, and ~50
    // units horizontally is far beyond any hand offset (~20). A first-person
    // thrower's grenade is born at their CAMERA, and replaying that absolute
    // point against a third-person proxy -- whose facing is smoothed and whose
    // position is interpolated with a delay -- puts it wherever the camera
    // happened to be, not where the proxy's hand appears to be.
    //
    // Fix: the sender also transmits the launch offset expressed in ITS OWN
    // local frame, and the receiver re-applies that offset to the PROXY's
    // current position and heading. The difference between that and the raw
    // absolute point becomes a correction that decays to zero over
    // kAnchorDecay, so the throw LEAVES the proxy's hand and then converges
    // onto the true trajectory well before the explosion (which is authored
    // by CoSyncExplosionSync at the real world position and must still line up).
    constexpr double kAnchorDecay = 0.5;

    // A proxy standing somewhere stale must not be able to fling the stand-in
    // across the map; beyond this the correction is dropped entirely and the
    // raw absolute path is used.
    constexpr float kMaxAnchorCorrection = 300.0f;

    // Radians/sec of tumble applied to hand throwables in flight. Cosmetic --
    // the real projectile's spin is havok-driven and not on the wire, and a
    // grenade that flies perfectly rigid reads as obviously fake.
    constexpr float kTumbleRate = 9.0f;

    double NowSeconds()
    {
        LARGE_INTEGER f{}, c{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
    }

    // ── Sender state ────────────────────────────────────────────────────────

    struct TrackedProjectile
    {
        uint32_t baseFormID   = 0;
        NiPoint3 lastPos{ 0.f, 0.f, 0.f };
        NiPoint3 vel{ 0.f, 0.f, 0.f };
        double   lastSample   = 0.0;
        double   lastSent     = 0.0;
        bool     seenThisPoll = false;
    };
    std::unordered_map<uint32_t, TrackedProjectile> s_tracked;   // local refID -> state
    double s_lastPoll = 0.0;

    // ── Receiver state ──────────────────────────────────────────────────────

    struct PendingOp
    {
        enum Type { Spawn, Move, Kill };
        Type     type = Spawn;
        uint64_t key  = 0;          // (ownerEID << 32) | srcRefID
        uint32_t ownerEID  = 0;     // needed to find the thrower's proxy
        uint32_t srcRefID  = 0;     // kept for logging only
        uint32_t baseFormID = 0;
        uint32_t visualBaseFormID = 0;   // inert item to draw, chosen by the sender
        NiPoint3 pos{ 0.f, 0.f, 0.f };
        NiPoint3 vel{ 0.f, 0.f, 0.f };
        NiPoint3 localOffset{ 0.f, 0.f, 0.f };   // launch offset in thrower frame
    };

    // Two peers can independently mint the same 0xFF-space ref formID, so the
    // sender's entityID has to be part of the key or their projectiles would
    // collide in s_remote.
    uint64_t RemoteKey(uint32_t ownerEID, uint32_t srcRefID)
    {
        return (static_cast<uint64_t>(ownerEID) << 32) | srcRefID;
    }
    std::mutex             s_mutex;
    std::vector<PendingOp> s_pendingOps;

    struct RemoteProjectile
    {
        // formID ONLY, never a cached TESObjectREFR*. Re-resolved through
        // LookupFormByID every single frame: a cell transition frees proxies
        // and spawned refs alike, and a stale pointer written to by
        // PositionRefRaw is a freed-memory write on a hot path (the exact
        // shape of the 2026-08-02 FlushPendingOutfitOps crash).
        uint32_t standInRefID = 0;
        NiPoint3 pos{ 0.f, 0.f, 0.f };
        NiPoint3 vel{ 0.f, 0.f, 0.f };
        double   lastUpdate   = 0.0;
        double   spawnTime    = 0.0;
        bool     tumbles      = false;
        // Launch-anchor correction, decayed to zero over kAnchorDecay so the
        // throw starts at the proxy's hand and ends on the true trajectory.
        NiPoint3 anchorFix{ 0.f, 0.f, 0.f };
    };
    std::unordered_map<uint64_t, RemoteProjectile> s_remote;     // RemoteKey -> stand-in

    // ── THE FEEDBACK-LOOP GUARD ─────────────────────────────────────────────
    // Every ref formID this machine created as a stand-in. Tick_Impl MUST skip
    // these.
    //
    // 2026-08-04, round 2 live test: one thrown grenade produced 20+ objects
    // and nearly crashed all three sessions. The header used to claim this
    // module had "no feedback loop by construction" because the replay only
    // creates stand-ins, never live projectiles. That reasoning was wrong the
    // moment SpawnStandIn was written to prefer the PROJ form itself as the
    // stand-in base (it has the correct in-flight mesh) -- a stand-in is
    // therefore a PROJ-BASED REF, which is precisely what the detector scans
    // for. So each peer re-detected its own stand-ins and re-broadcast them,
    // and with three peers that compounds every poll. Each new stand-in also
    // gets a fresh refID, so the (owner, refID) key never deduplicated it.
    //
    // This is the same guard s_knownExplosionRefs provides in
    // CoSyncExplosionSync. Do not delete it on the grounds that the replay
    // "can't" be re-detected -- that is the exact assumption that failed.
    //
    // Round 4 live test: the guard leaked and the loop came back, one hop
    // every ~0.5-2s instead of every poll. Host log, one throw:
    //   01.325  stand-in 0xFF0024FF created
    //   01.801  "[ThrowSync TX] launch ref=0xFF0024FF"  <- its OWN stand-in
    // Cause: DeleteStandIn dropped the exemption immediately, but the actual
    // removal is CallFunctionNoWait(Disable/Delete) -- QUEUED Papyrus, which
    // lands frames later. Between the erase and the delete the ref is still
    // sitting in the cell with a PROJ base and no exemption, so the next 50ms
    // poll re-broadcast it. The tell in the logs is the launch offset:
    // "left 534" / "left 887" are not throws, they are refs found far from
    // the player.
    //
    // So the exemption now OUTLIVES the delete request. Value is the time the
    // exemption may be forgotten (0.0 = still alive, never expire). The grace
    // exists only so the engine can eventually reuse the formID for a real
    // projectile without us blinding ourselves to it.
    std::unordered_map<uint32_t, double> s_ourStandIns;
    constexpr double kStandInExemptGrace = 15.0;

    // Exempt while alive.
    void ExemptStandIn(uint32_t refID)
    {
        s_ourStandIns[refID] = 0.0;
    }

    // Stop exempting, but only after the queued delete has had ample time to
    // land. Never erase outright -- that is the bug above.
    void ReleaseStandIn(uint32_t refID, double now)
    {
        auto it = s_ourStandIns.find(refID);
        if (it != s_ourStandIns.end() && it->second == 0.0)
            it->second = now + kStandInExemptGrace;
    }

    bool IsOurStandIn(uint32_t refID, double now)
    {
        auto it = s_ourStandIns.find(refID);
        if (it == s_ourStandIns.end()) return false;
        return it->second == 0.0 || now < it->second;
    }

    // Safety valve, added after the same test. Even with the loop above
    // closed, no future bug should be able to melt a session: this caps how
    // many NEW projectile broadcasts can leave this machine per second.
    // Legitimate play cannot approach it (a Fat Man is one projectile, a
    // shotgun-style cluster a handful).
    constexpr uint32_t kMaxSpawnBroadcastsPerSecond = 12;
    double   s_broadcastWindowStart = 0.0;
    uint32_t s_broadcastsThisWindow = 0;

    // Stand-in bases whose placement faulted, so we stop retrying them -- same
    // policy as CoSyncExplosionSync::s_faultedBases and CoSyncWorldSync's
    // s_workshopFaultedBaseForms.
    std::unordered_set<uint32_t> s_faultedWeapBases;

    // ── Diagnostics ─────────────────────────────────────────────────────────

    struct ThrowDiag
    {
        uint32_t spawnSent   = 0;
        uint32_t moveSent    = 0;
        uint32_t killSent    = 0;
        uint32_t spawnApplied= 0;
        uint32_t spawnFailed = 0;
        uint32_t rejected    = 0;   // peer sent a base we refused
        uint32_t expired     = 0;   // TTL cleanup, i.e. a lost THRX
    };
    ThrowDiag s_diag{};
    double    s_lastDiagLog = 0.0;
    constexpr double kDiagInterval = 5.0;

    // Every distinct PROJ base we have seen locally, whether or not it passed
    // the filter, logged exactly once with the fields the filter reads. This
    // is how kMaxSyncableSpeed gets retuned against real data (including DLC
    // and modded explosives) instead of by guessing at record values.
    std::unordered_set<uint32_t> s_loggedBases;

    void LogDiag(double now)
    {
        if ((now - s_lastDiagLog) < kDiagInterval) return;
        s_lastDiagLog = now;
        const ThrowDiag& d = s_diag;
        if (d.spawnSent || d.moveSent || d.killSent || d.spawnApplied ||
            d.spawnFailed || d.rejected || d.expired)
        {
            LOG_INFO("[ThrowDiag] last %.0fs: sent spawn=%u move=%u kill=%u | "
                     "received applied=%u failed=%u rejected=%u expired=%u | "
                     "tracked=%u remote=%u",
                kDiagInterval, d.spawnSent, d.moveSent, d.killSent,
                d.spawnApplied, d.spawnFailed, d.rejected, d.expired,
                (unsigned)s_tracked.size(), (unsigned)s_remote.size());
        }
        s_diag = ThrowDiag{};
    }

    // ── Engine reads (all SEH-guarded at the call site) ─────────────────────

    TESObjectREFR* FormToREFR(TESForm* form)
    {
        if (!form) return nullptr;
        if (form->formType < kFormType_REFR || form->formType > kFormType_PHZD)
            return nullptr;
        return static_cast<TESObjectREFR*>(form);
    }

    BGSProjectile* AsProjectileBase(TESForm* form)
    {
        if (!form || form->formType != kFormType_PROJ_Local)
            return nullptr;
        return static_cast<BGSProjectile*>(form);
    }

    void LogNewBaseOnce(BGSProjectile* proj, bool accepted)
    {
        if (!proj) return;
        if (!s_loggedBases.insert(proj->formID).second) return;
        const auto& d = proj->projectileData;
        LOG_INFO("[ThrowSync] new projectile base=0x%08X: speed=%.0f gravity=%.2f "
                 "explodes=%s proximity=%.1f timer=%.2f weaponSource=0x%08X -> %s",
            proj->formID, d.speed, d.gravity,
            d.explosionType ? "yes" : "no", d.explosionProximity, d.explosionTimer,
            d.defaultWeaponSource ? d.defaultWeaponSource->formID : 0u,
            accepted ? "will be synced" : "filtered out");
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Shared classification helpers
// ────────────────────────────────────────────────────────────────────────────

bool CoSyncThrowableSync::IsSyncableProjectile(BGSProjectile* proj)
{
    if (!proj) return false;
    const auto& d = proj->projectileData;
    if (!d.explosionType) return false;
    if (!(d.speed > 0.0f) || d.speed > kMaxSyncableSpeed) return false;
    return true;
}

BGSProjectile* CoSyncThrowableSync::GetWeaponProjectile(TESObjectWEAP* weap)
{
    if (!weap) return nullptr;
    if (!weap->weapData.firingData) return nullptr;
    return weap->weapData.firingData->projectileOverride;
}

bool CoSyncThrowableSync::IsHandThrowable(TESObjectWEAP* weap)
{
    BGSProjectile* proj = GetWeaponProjectile(weap);
    if (!IsSyncableProjectile(proj)) return false;
    // Gravity is what separates a thrown object from a launched one: a
    // grenade/Molotov/mine arcs (gravity 1.0), a missile flies flat
    // (gravity 0). Only the arcing ones are throw-ANIMATED -- a missile
    // launcher fires with the normal weapon attack animation, which the
    // existing attackStart path already drives.
    return proj->projectileData.gravity > 0.0f;
}

__declspec(noinline) static uint32_t GetEquippedThrowableFormID_Impl(Actor* actor)
{
    if (!actor || !actor->middleProcess || !actor->middleProcess->unk08)
        return 0;

    Actor::AIProcess::Data08* data = actor->middleProcess->unk08;
    for (UInt32 i = 0; i < data->equipData.count; ++i)
    {
        const Actor::AIProcess::Data08::EquipData& ed = data->equipData[i];
        if (!ed.item || ed.item->formType != kFormType_WEAP)
            continue;

        // A throw temporarily puts the grenade into its own equip slot
        // ALONGSIDE the primary weapon, so unlike GetEquippedWeaponFormID
        // (which stops at the first WEAP) we must keep walking past the gun
        // to find it.
        TESObjectWEAP* weap = static_cast<TESObjectWEAP*>(ed.item);
        if (CoSyncThrowableSync::IsHandThrowable(weap))
            return weap->formID;
    }
    return 0;
}

uint32_t CoSyncThrowableSync::GetEquippedThrowableFormID(Actor* actor)
{
    __try
    {
        return GetEquippedThrowableFormID_Impl(actor);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

__declspec(noinline) static bool IsMineThrowable_Impl(uint32_t weapFormID)
{
    if (weapFormID == 0) return false;

    TESForm* form = LookupFormByID(weapFormID);
    if (!form || form->formType != kFormType_WEAP) return false;

    BGSProjectile* proj =
        CoSyncThrowableSync::GetWeaponProjectile(static_cast<TESObjectWEAP*>(form));
    if (!proj) return false;

    // Mines arm and detonate on proximity; grenades and Molotovs run purely
    // on explosionTimer and leave proximity at 0. This is a data-driven test
    // against the PROJ record, so DLC and modded mines classify correctly
    // without being listed anywhere.
    return proj->projectileData.explosionProximity > 0.0f;
}

bool CoSyncThrowableSync::IsMineThrowable(uint32_t weapFormID)
{
    __try
    {
        return IsMineThrowable_Impl(weapFormID);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Sender: detect + stream local projectiles
// ────────────────────────────────────────────────────────────────────────────

namespace
{
    // Inert item form types acceptable as a stand-in model. Deliberately
    // exhaustive and deliberately EXCLUDING every "behavioural" form (PROJ,
    // EXPL, ACTI, HAZD, ...) -- placing one of those makes the engine ACT on
    // it rather than merely draw it, which is the round-5 live-projectile bug.
    bool IsInertItemBase(UInt8 formType)
    {
        return formType == kFormType_MISC
            || formType == kFormType_WEAP
            || formType == kFormType_AMMO
            || formType == kFormType_KEYM
            || formType == kFormType_ALCH;
    }

    // Works out which inert item to draw for a projectile the LOCAL player
    // just launched.
    //
    // Why the sender decides: 2026-08-04 round 6 log --
    //   PROJ base=0x000E6B2F speed=1900 gravity=0.60 explosion=yes
    //                        weaponSrc=0x00000000 -> STREAMED
    // The launcher's projectile record has NO defaultWeaponSource, so the
    // receiver-side derivation had nothing to build a model from and silently
    // produced no visual ("I can't see the projectile when I shoot"). The
    // SENDER always knows what fired it, so it resolves the model and puts
    // the formID on the wire; the receiver only has to sanity-check it.
    __declspec(noinline) uint32_t ResolveVisualBase(Actor* thrower, BGSProjectile* proj)
    {
        const bool isHandThrown = proj && proj->projectileData.gravity > 0.0f;

        // Prefer what the thrower actually has equipped.
        if (thrower && thrower->middleProcess && thrower->middleProcess->unk08)
        {
            Actor::AIProcess::Data08* data = thrower->middleProcess->unk08;
            TESObjectWEAP* gun       = nullptr;
            TESObjectWEAP* throwable = nullptr;

            for (UInt32 i = 0; i < data->equipData.count; ++i)
            {
                const Actor::AIProcess::Data08::EquipData& ed = data->equipData[i];
                if (!ed.item || ed.item->formType != kFormType_WEAP) continue;
                TESObjectWEAP* w = static_cast<TESObjectWEAP*>(ed.item);
                // A throw equips the grenade ALONGSIDE the gun, so both slots
                // are live and we must classify rather than take the first.
                if (CoSyncThrowableSync::IsHandThrowable(w)) { if (!throwable) throwable = w; }
                else                                         { if (!gun)       gun       = w; }
            }

            if (isHandThrown && throwable)
                return throwable->formID;              // the grenade item itself
            if (!isHandThrown && gun && gun->weapData.ammo)
                return gun->weapData.ammo->formID;     // Missile / MiniNuke / 40mm / cannonball
        }

        // Fall back to the PROJ record (this is the path NPC-thrown grenades
        // take, since the equipped-weapon lookup above only knows about the
        // local player).
        if (proj)
        {
            if (TESObjectWEAP* w = proj->projectileData.defaultWeaponSource)
            {
                if (isHandThrown) return w->formID;
                if (w->weapData.ammo) return w->weapData.ammo->formID;
            }
        }
        return 0;   // no visual available -- explosion only
    }

    __declspec(noinline) void Tick_Impl(double now)
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc) return;
        TESObjectREFR* pcRefr = reinterpret_cast<TESObjectREFR*>(pc);
        TESObjectCELL* cell = pcRefr->parentCell;
        if (!cell) return;

        for (auto& kv : s_tracked)
            kv.second.seenThisPoll = false;

        const uint32_t myEID = CoSyncNet::GetMyEntityID();

        // Thrower frame, captured once per poll for the launch-offset maths.
        // Same Z-up yaw convention as CoSyncGameAPI::PositionRemoteActor
        // (round 128, ground-truth-confirmed): local->world is
        // [c,-s,0; s,c,0; 0,0,1], so world->local is its transpose.
        const NiPoint3 myPos = pcRefr->pos;
        const float    myYaw = pcRefr->rot.z;
        const float    myCos = cosf(myYaw);
        const float    mySin = sinf(myYaw);

        char buf[256];

        for (UInt32 i = 0; i < cell->objectList.count; ++i)
        {
            TESObjectREFR* ref = cell->objectList.entries[i];
            if (!ref || !ref->baseForm) continue;
            if (ref->baseForm->formType != kFormType_PROJ_Local) continue;
            if (ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled))
                continue;

            const uint32_t fid = ref->formID;
            if (fid == 0) continue;

            // Never re-broadcast a stand-in we created replaying a peer --
            // see the s_ourStandIns comment. This check must stay ABOVE the
            // filter and the tracking insert, or the loop reopens.
            if (IsOurStandIn(fid, now))
                continue;

            BGSProjectile* proj = static_cast<BGSProjectile*>(ref->baseForm);
            const bool syncable = CoSyncThrowableSync::IsSyncableProjectile(proj);
            LogNewBaseOnce(proj, syncable);
            if (!syncable) continue;

            auto it = s_tracked.find(fid);
            if (it == s_tracked.end())
            {
                if (s_tracked.size() >= kMaxTracked)
                    continue;

                if ((now - s_broadcastWindowStart) >= 1.0)
                {
                    s_broadcastWindowStart = now;
                    s_broadcastsThisWindow = 0;
                }
                if (++s_broadcastsThisWindow > kMaxSpawnBroadcastsPerSecond)
                {
                    // Do NOT track it either -- tracking it here would start
                    // streaming THRM for something peers never got a THRP for.
                    LOG_UPDATED_WARN("[ThrowSync] hit the broadcast rate cap (%u/s) — "
                                     "dropping projectile ref=0x%08X; check for a "
                                     "runaway loop if this keeps happening",
                        kMaxSpawnBroadcastsPerSecond, fid);
                    continue;
                }

                TrackedProjectile t{};
                t.baseFormID   = proj->formID;
                t.lastPos      = ref->pos;
                t.lastSample   = now;
                t.lastSent     = now;
                t.seenThisPoll = true;
                s_tracked.emplace(fid, t);

                // Launch offset in the thrower's own frame (world->local is
                // the transpose of the yaw matrix).
                const float wx = ref->pos.x - myPos.x;
                const float wy = ref->pos.y - myPos.y;
                const float lx =  myCos * wx + mySin * wy;
                const float ly = -mySin * wx + myCos * wy;
                const float lz = ref->pos.z - myPos.z;

                // Only trust the equipped-weapon lookup when the launch is
                // close enough to have come from the local player; a distant
                // one is an NPC's and must use the PROJ record instead.
                const float distSq = wx * wx + wy * wy + lz * lz;
                Actor* thrower = (distSq < 300.0f * 300.0f)
                    ? reinterpret_cast<Actor*>(pc) : nullptr;
                const uint32_t visualBase = ResolveVisualBase(thrower, proj);

                std::snprintf(buf, sizeof(buf),
                    "THRP|%u|%u|%u|%.1f|%.1f|%.1f|%.1f|%.1f|%.1f|%u",
                    myEID, fid, proj->formID,
                    ref->pos.x, ref->pos.y, ref->pos.z, lx, ly, lz, visualBase);
                CoSyncTransport::Send(std::string(buf));
                s_diag.spawnSent++;

                LOG_INFO("[ThrowSync] sent launch ref=0x%08X base=0x%08X position=(%.0f,%.0f,%.0f) "
                         "localOffset=(fwd %.0f, left %.0f, up %.0f) visualBase=0x%08X",
                    fid, proj->formID, ref->pos.x, ref->pos.y, ref->pos.z, lx, ly, lz,
                    visualBase);
                continue;
            }

            TrackedProjectile& t = it->second;
            t.seenThisPoll = true;

            const double dt = now - t.lastSample;
            if (dt > 0.001)
            {
                t.vel.x = static_cast<float>((ref->pos.x - t.lastPos.x) / dt);
                t.vel.y = static_cast<float>((ref->pos.y - t.lastPos.y) / dt);
                t.vel.z = static_cast<float>((ref->pos.z - t.lastPos.z) / dt);
                t.lastPos    = ref->pos;
                t.lastSample = now;
            }

            std::snprintf(buf, sizeof(buf), "THRM|%u|%u|%.1f|%.1f|%.1f|%.1f|%.1f|%.1f",
                myEID, fid, ref->pos.x, ref->pos.y, ref->pos.z, t.vel.x, t.vel.y, t.vel.z);
            CoSyncTransport::Send(std::string(buf));
            t.lastSent = now;
            s_diag.moveSent++;
        }

        // Anything tracked but absent from the cell this poll is gone: it
        // detonated, its lifetime ran out, or it crossed into a neighbouring
        // exterior cell (objectList is the player's OWN cell only). All three
        // end the flight as far as peers are concerned -- the detonation case
        // is then covered by CoSyncExplosionSync's own EXPL broadcast.
        for (auto it = s_tracked.begin(); it != s_tracked.end(); )
        {
            if (it->second.seenThisPoll)
            {
                ++it;
                continue;
            }

            std::snprintf(buf, sizeof(buf), "THRX|%u|%u", myEID, it->first);
            CoSyncTransport::Send(std::string(buf));
            s_diag.killSent++;

            LOG_INFO("[ThrowSync] sent gone for ref=0x%08X", it->first);
            it = s_tracked.erase(it);
        }

        // Forget stand-in exemptions whose grace has fully elapsed, so the map
        // stays bounded over a long session and the engine is free to reuse
        // those formIDs for real projectiles again.
        for (auto it = s_ourStandIns.begin(); it != s_ourStandIns.end(); )
        {
            if (it->second != 0.0 && now >= it->second)
                it = s_ourStandIns.erase(it);
            else
                ++it;
        }
    }
}

void CoSyncThrowableSync::Tick(double now)
{
    if (!kThrowableVisualsEnabled) return;   // in-flight visuals switched off
    if (!CoSyncNet::IsConnected()) return;
    if ((now - s_lastPoll) < kPollInterval) return;
    s_lastPoll = now;

    __try
    {
        Tick_Impl(now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[ThrowSync] the tick crashed, recovered (skipping this scan)");
    }

    LogDiag(now);
}

// ────────────────────────────────────────────────────────────────────────────
// Receiver: parse, spawn/move/delete stand-ins
// ────────────────────────────────────────────────────────────────────────────

bool CoSyncThrowableSync::IsStandInRef(uint32_t refFormID)
{
    if (refFormID == 0) return false;
    // Deliberately ignores the grace expiry used by the detector: an expired
    // entry means "the engine may reuse this formID", but for as long as the
    // entry exists at all we should not let anyone loot it.
    return s_ourStandIns.find(refFormID) != s_ourStandIns.end();
}

bool CoSyncThrowableSync::IsThrowableMsg(const std::string& msg)
{
    return msg.rfind("THRP|", 0) == 0
        || msg.rfind("THRM|", 0) == 0
        || msg.rfind("THRX|", 0) == 0;
}

void CoSyncThrowableSync::OnReceiveThrowableMsg(const std::string& msg)
{
    if (!kThrowableVisualsEnabled) return;   // in-flight visuals switched off
    PendingOp op{};

    const char* s = msg.c_str() + 5;   // past "THRx|"
    char* e = nullptr;

    const uint32_t ownerEID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (!e || *e != '|' || ownerEID == 0) return;
    s = e + 1;

    // The host relays every THR* it receives, and GNS_Session::SendText
    // broadcasts to all client connections including the originating one, so
    // a client's own throw arrives back here. Applying it would spawn a
    // stand-in on top of the real grenade this machine is already simulating.
    if (ownerEID == CoSyncNet::GetMyEntityID())
        return;

    op.srcRefID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (!e || op.srcRefID == 0) return;
    op.ownerEID = ownerEID;
    op.key = RemoteKey(ownerEID, op.srcRefID);

    if (msg[3] == 'X')
    {
        op.type = PendingOp::Kill;
    }
    else if (msg[3] == 'P')
    {
        op.type = PendingOp::Spawn;
        if (*e != '|') return; s = e + 1;
        op.baseFormID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
        if (!e || *e != '|' || op.baseFormID == 0) return; s = e + 1;
        op.pos.x = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
        op.pos.y = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
        op.pos.z = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
        op.localOffset.x = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
        op.localOffset.y = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
        op.localOffset.z = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
        op.visualBaseFormID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    }
    else if (msg[3] == 'M')
    {
        op.type = PendingOp::Move;
        if (*e != '|') return; s = e + 1;
        op.pos.x = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
        op.pos.y = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
        op.pos.z = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
        op.vel.x = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
        op.vel.y = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
        op.vel.z = std::strtof(s, &e);
    }
    else
    {
        return;
    }

    std::lock_guard<std::mutex> lk(s_mutex);
    s_pendingOps.push_back(op);
}

namespace
{
    // Queues Disable+Delete on a stand-in, exactly the pattern
    // CoSyncItemDropSync::ProcessDespawnOps_Impl uses for its clones.
    __declspec(noinline) void DeleteStandIn(uint32_t standInRefID)
    {
        if (standInRefID == 0) return;

        // Start the exemption's grace timer rather than dropping it now: the
        // Disable+Delete below is QUEUED, so this ref is still in the cell
        // for some frames yet and must stay invisible to our own detector.
        ReleaseStandIn(standInRefID, NowSeconds());

        TESForm* form = LookupFormByID(standInRefID);
        TESObjectREFR* refr = FormToREFR(form);
        if (!refr) return;

        // NATIVE disable FIRST, and synchronously.
        //
        // 2026-08-04 (list item #1): removal used to be Papyrus-only
        // (CallFunctionNoWait Disable + Delete). Two problems with that:
        //   * CallFunctionNoWait was pointed at the WRONG FUNCTION until today
        //     (see PapyrusNoWaitReloc.cpp), so the disable/delete simply never
        //     happened -- every stand-in ever spawned stayed in the world;
        //   * even when it works it is QUEUED, so between "stop streaming
        //     positions" and "the VM gets round to it" the ref is left to
        //     havok and drops to the floor as a lootable item. That is exactly
        //     the reported symptom: a second grenade-shaped object falling to
        //     the ground that can be picked up.
        // The native path is immediate and needs no VM, so the object cannot
        // be seen falling and cannot be looted even if Papyrus is unhealthy.
        CoSyncGameAPI::DisableWorldActor(refr);

        // Papyrus Delete afterwards, purely to reclaim the reference. If it
        // never lands the ref is already disabled and harmless.
        static const BSFixedString kDelete("Delete");
        VMArray<VMVariable> delArgs;
        CallFunctionNoWait(refr, kDelete, delArgs);
    }

    // Picks what to actually put in the world for a given projectile.
    //
    // NEVER USE THE PROJ FORM ITSELF. That was the original choice, because
    // BGSProjectile is a TESBoundObject carrying the exact in-flight mesh, and
    // the assumption was that CreateReferenceAtLocation on it yields an inert
    // prop. It does not. 2026-08-04 round 5 log, host side:
    //   11.906  SpawnItemAtPosition spawned refID=0xFF0024C9   (our stand-in)
    //   12.068  [ThrowSync TX] launch ref=0xFF0024E4           (never spawned by us)
    // The engine instantiated a REAL, LIVE projectile from the placed PROJ ref
    // -- a separate ref we never created. It then flew under its own physics
    // with no launch velocity (this is the "thrown completely backwards"
    // report), was picked up by our own detector as a genuine projectile, and
    // would have detonated on its own. s_ourStandIns can never suppress it,
    // because its formID is one we never see created.
    //
    // So the stand-in must be an INERT item form that merely looks right:
    //   * hand throwable (gravity > 0): projectileData.defaultWeaponSource IS
    //     the grenade/Molotov/mine item, so its world model is exactly right.
    //   * launcher (gravity == 0): that same field is the LAUNCHER, which
    //     would fly a missile launcher through the air -- so go one step
    //     further to weapData.ammo, the round itself (Missile, MiniNuke,
    //     cannonball, 40mm grenade). Correct model, and AMMO is an ordinary
    //     inert item.
    // Both are plain items, so neither can ever be mistaken for a projectile
    // by our detector or by the engine.
    __declspec(noinline) TESObjectREFR* SpawnStandIn(BGSProjectile* proj,
                                                     uint32_t visualBaseFormID,
                                                     const NiPoint3& pos)
    {
        if (!proj) return nullptr;

        TESForm* standInBase = nullptr;

        // The sender's choice wins -- it knows which weapon actually fired,
        // which is the only way to get a model for a PROJ record whose
        // defaultWeaponSource is null (every launcher tested so far).
        if (visualBaseFormID != 0)
        {
            TESForm* f = LookupFormByID(visualBaseFormID);
            if (f && IsInertItemBase(f->formType))
                standInBase = f;
            else if (f)
                LOG_UPDATED_WARN("[ThrowSync] received visualBase=0x%08X is form type %u, not a "
                                 "harmless item — refusing to place it",
                    visualBaseFormID, (unsigned)f->formType);
        }

        // Local derivation as a fallback (older peer, or sender had nothing).
        if (!standInBase)
        {
            TESObjectWEAP* weap = proj->projectileData.defaultWeaponSource;
            if (!weap) return nullptr;
            standInBase = (proj->projectileData.gravity > 0.0f)
                ? static_cast<TESForm*>(weap)
                : static_cast<TESForm*>(weap->weapData.ammo);
            if (!standInBase) return nullptr;
        }

        if (s_faultedWeapBases.count(standInBase->formID))
            return nullptr;

        TESObjectREFR* placed = CoSyncGameAPI::SpawnItemAtPosition(standInBase, pos);
        if (!placed)
        {
            s_faultedWeapBases.insert(standInBase->formID);
            LOG_WARN("[ThrowSync] stand-in base=0x%08X (for projectile 0x%08X) failed to place — "
                     "skipping it for this session",
                standInBase->formID, proj->formID);
        }
        return placed;
    }

    __declspec(noinline) void ApplyOps_Impl(const std::vector<PendingOp>& ops, double now)
    {
        for (const PendingOp& op : ops)
        {
            if (op.type == PendingOp::Kill)
            {
                auto it = s_remote.find(op.key);
                if (it == s_remote.end()) continue;
                DeleteStandIn(it->second.standInRefID);
                s_remote.erase(it);
                continue;
            }

            if (op.type == PendingOp::Move)
            {
                auto it = s_remote.find(op.key);
                if (it == s_remote.end()) continue;
                it->second.pos        = op.pos;
                it->second.vel        = op.vel;
                it->second.lastUpdate = now;
                continue;
            }

            // Spawn. A duplicate THRP for a ref we already track means the
            // sender's formID was reused after our THRX went missing -- drop
            // the old stand-in rather than leaking it.
            auto existing = s_remote.find(op.key);
            if (existing != s_remote.end())
            {
                DeleteStandIn(existing->second.standInRefID);
                s_remote.erase(existing);
            }

            if (s_remote.size() >= kMaxTracked)
                continue;

            TESForm* baseForm = LookupFormByID(op.baseFormID);
            BGSProjectile* proj = AsProjectileBase(baseForm);
            if (!proj || !CoSyncThrowableSync::IsSyncableProjectile(proj))
            {
                // A malformed or hostile packet must not be able to place an
                // arbitrary form at the player -- same refusal the explosion
                // sync applies to non-EXPL bases.
                s_diag.rejected++;
                LOG_WARN("[ThrowSync] received base=0x%08X is not a syncable projectile — ignored",
                    op.baseFormID);
                continue;
            }

            // Re-anchor the launch point onto the thrower's PROXY. See the
            // kAnchorDecay comment: the raw absolute point is where the
            // sender's first-person CAMERA was, which does not line up with
            // the third-person proxy the receiver is looking at.
            NiPoint3 anchorFix{ 0.f, 0.f, 0.f };
            if (CoSyncPlayer* thrower = g_CoSyncPlayerManager.GetPlayerByEntityID(op.ownerEID))
            {
                Actor* pa = thrower->actorRef;
                if (pa && thrower->hasSpawned && CoSyncGameAPI::IsFormStillLive(pa))
                {
                    const float pc = cosf(pa->rot.z);
                    const float ps = sinf(pa->rot.z);

                    // local->world for the proxy's own heading.
                    const float ex = pa->pos.x + pc * op.localOffset.x - ps * op.localOffset.y;
                    const float ey = pa->pos.y + ps * op.localOffset.x + pc * op.localOffset.y;
                    const float ez = pa->pos.z + op.localOffset.z;

                    const float dx = ex - op.pos.x;
                    const float dy = ey - op.pos.y;
                    const float dz = ez - op.pos.z;

                    // A stale/mispositioned proxy must not fling the stand-in
                    // across the map -- past this, trust the absolute path.
                    const float distSq = dx * dx + dy * dy + dz * dz;
                    if (distSq <= kMaxAnchorCorrection * kMaxAnchorCorrection)
                    {
                        anchorFix.x = dx;
                        anchorFix.y = dy;
                        anchorFix.z = dz;
                    }
                    else
                    {
                        LOG_UPDATED_WARN("[ThrowSync] position correction of %.0f units for entity=%u "
                                         "is too large — using the raw launch point instead",
                            sqrtf(distSq), op.ownerEID);
                    }
                }
            }

            NiPoint3 spawnPos{ op.pos.x + anchorFix.x,
                               op.pos.y + anchorFix.y,
                               op.pos.z + anchorFix.z };

            TESObjectREFR* placed = SpawnStandIn(proj, op.visualBaseFormID, spawnPos);
            if (!placed)
            {
                s_diag.spawnFailed++;
                continue;
            }

            // Exempt from our own detector BEFORE anything else can poll --
            // the tick order already puts Tick() ahead of the flush, but a
            // stand-in must never be visible to the detector even for one
            // scan (that is the 20-grenades bug).
            ExemptStandIn(placed->formID);

            RemoteProjectile r{};
            r.standInRefID = placed->formID;
            r.pos          = op.pos;
            r.lastUpdate   = now;
            r.spawnTime    = now;
            r.tumbles      = proj->projectileData.gravity > 0.0f;
            r.anchorFix    = anchorFix;
            s_remote.emplace(op.key, r);

            s_diag.spawnApplied++;
            LOG_INFO("[ThrowSync] placed stand-in ref=0x%08X for source=0x%08X base=0x%08X at (%.0f,%.0f,%.0f) "
                     "correction=(%.0f,%.0f,%.0f)",
                placed->formID, op.srcRefID, op.baseFormID,
                spawnPos.x, spawnPos.y, spawnPos.z,
                anchorFix.x, anchorFix.y, anchorFix.z);
        }
    }

    // Runs every frame, not at the 20Hz packet rate: between THRM updates the
    // stand-in dead-reckons along its last known velocity, which is what
    // turns a 20Hz sample stream into a smooth arc instead of a visible
    // 3-inches-at-a-time stutter.
    __declspec(noinline) void AdvanceStandIns_Impl(double now)
    {
        for (auto it = s_remote.begin(); it != s_remote.end(); )
        {
            RemoteProjectile& r = it->second;

            const double sinceUpdate = now - r.lastUpdate;
            const double age         = now - r.spawnTime;

            if (sinceUpdate > kStaleTTL || age > kMaxLifetime)
            {
                DeleteStandIn(r.standInRefID);
                s_diag.expired++;
                LOG_UPDATED("[ThrowSync] stand-in 0x%08X expired (quiet for %.1fs, age %.1fs)",
                    r.standInRefID, sinceUpdate, age);
                it = s_remote.erase(it);
                continue;
            }

            TESForm* form = LookupFormByID(r.standInRefID);
            TESObjectREFR* refr = FormToREFR(form);
            if (!refr)
            {
                // The engine took it (cell unload, our own Delete landing).
                // DeleteStandIn is not called on this path, so start the
                // exemption's grace timer here. Still not an outright erase:
                // LookupFormByID missing it does not guarantee every engine
                // list has dropped it yet.
                ReleaseStandIn(r.standInRefID, now);
                it = s_remote.erase(it);
                continue;
            }

            const float dt = static_cast<float>(sinceUpdate);

            // Launch-anchor correction, faded out linearly over kAnchorDecay:
            // full strength at the instant of the throw (so it leaves the
            // proxy's hand) and zero shortly after (so the flight ends on the
            // authoritative path, where CoSyncExplosionSync will put the
            // detonation).
            const float fade = (age >= kAnchorDecay)
                ? 0.0f
                : static_cast<float>(1.0 - age / kAnchorDecay);

            NiPoint3 p;
            p.x = r.pos.x + r.vel.x * dt + r.anchorFix.x * fade;
            p.y = r.pos.y + r.vel.y * dt + r.anchorFix.y * fade;
            p.z = r.pos.z + r.vel.z * dt + r.anchorFix.z * fade;

            const float yaw = atan2f(r.vel.y, r.vel.x);
            const float tumble = r.tumbles ? static_cast<float>(age) * kTumbleRate : 0.0f;

            CoSyncGameAPI::PositionRefRaw(refr, p, yaw, tumble);
            ++it;
        }
    }

    __declspec(noinline) void ApplyOpsSafe(const std::vector<PendingOp>& ops, double now)
    {
        __try
        {
            ApplyOps_Impl(ops, now);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_WARN("[ThrowSync] applying ops crashed, recovered (dropping this batch)");
        }
    }

    __declspec(noinline) void AdvanceStandInsSafe(double now)
    {
        __try
        {
            AdvanceStandIns_Impl(now);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_WARN("[ThrowSync] advancing stand-ins crashed, recovered (skipping this frame)");
        }
    }
}

void CoSyncThrowableSync::FlushPendingThrowables(double now)
{
    if (!kThrowableVisualsEnabled) return;   // in-flight visuals switched off
    std::vector<PendingOp> ops;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        ops.swap(s_pendingOps);
    }

    if (!ops.empty())
        ApplyOpsSafe(ops, now);

    if (!s_remote.empty())
        AdvanceStandInsSafe(now);
}

namespace
{
    __declspec(noinline) void Reset_Impl()
    {
        for (auto& kv : s_remote)
            DeleteStandIn(kv.second.standInRefID);
    }

    // C2712 split: Reset() itself holds a std::lock_guard (non-trivial unwind)
    // and so cannot also own a __try -- the same _Impl/Safe shape used
    // throughout CoSyncWorldSync.cpp.
    __declspec(noinline) void ResetSafe()
    {
        __try
        {
            Reset_Impl();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_WARN("[ThrowSync] reset crashed, recovered (some stand-ins may remain)");
        }
    }
}

void CoSyncThrowableSync::Reset()
{
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        s_pendingOps.clear();
    }

    ResetSafe();

    s_remote.clear();
    s_tracked.clear();
    s_ourStandIns.clear();
    s_broadcastsThisWindow = 0;
    s_diag = ThrowDiag{};
}
