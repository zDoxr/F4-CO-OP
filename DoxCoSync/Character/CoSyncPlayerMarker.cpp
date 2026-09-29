#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncPlayerMarker.h"
#include "CoSyncNet.h"
#include "CoSyncPlayer.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncOverlay.h"

#include "CoSyncRequirementsLog.h"
#include "ConsoleLogger.h"
#include "CoSyncGameAPI.h"
#include "CoSyncVMFormArg.h"
#include "GameData.h"
#include "GameForms.h"
#include "GameReferences.h"
#include "PapyrusVM.h"
#include "PapyrusEvents.h"
#include "PapyrusArgs.h"

extern RelocPtr<DataHandler*> g_dataHandler;
extern RelocAddr<_LookupFormByID> LookupFormByID;
extern RelocAddr<_MoveRefrToPosition> MoveRefrToPosition;
extern PlayerCharacter* CoSyncGetPlayer();

namespace
{
    // Local formID in CoSync.esp for the pre-placed map marker REFR.
    // CORRECTED 2026-07-21: the constant below was never right. The esp DOES
    // have a REFR placed in the Commonwealth worldspace with the correct base
    // form (vanilla generic MapMarker STAT, 0x00000010) and the correct
    // Initially Disabled flag (0x800) — someone got partway through this in
    // the CK on 2026-07-15 — but its real local formID is 0x00B6CA, not
    // 0x0000BE64, and it never actually had map-marker data (no XMRK/FNAM/
    // TNAM subrecords existed on it at all, so AddToMap() had nothing to
    // reveal regardless of formID). Both fixed by hand-patching the esp
    // directly (structurally self-verified round-trip before deploy — see
    // scratch script history) rather than redoing this in the CK: appended
    // XMRK (has-marker flag) + FNAM (0x03 = VISIBLE|TRAVEL_LOC) + TNAM
    // (0 = generic icon) to the existing record, propagated the +21 byte
    // size delta through its 3 enclosing GRUPs. Position is irrelevant — the
    // plugin MoveTo's it onto the proxy every tick.
    // 2026-08-04: ONE marker only ever supported ONE remote player. Map
    // markers are strictly per-REFR -- the engine has no way to merge several
    // players into a single icon, and the custom-map-marker path
    // (SetCustomMapMarkerCommand -> 0x00D63150) is a SINGLE player-placed
    // marker, so it cannot carry N players either. The only mechanism that
    // scales is N marker REFRs.
    //
    // CoSync.esp therefore now carries FOUR identical map-marker records.
    // 0x00B6CA is the original; 0x00B6CC-0x00B6CE were appended by
    // hand-patching the plugin (each a byte-for-byte copy of 0x00B6CA with
    // only formID + EDID changed, so record sizes are unchanged; the 3
    // enclosing GRUP sizes and the TES4 HEDR record count / nextObjectID were
    // propagated, and the result was re-parsed end-to-end to confirm no new
    // structural damage vs the pre-edit backup).
    //
    // 2026-09-26 (Phase 7d, 8-player sessions): three more copies appended the
    // same way, 0x00B6D5-0x00B6D7 (EDIDs CoSyncRemotePlayerMarke5..7; 4
    // enclosing GRUP sizes + HEDR propagated, re-parsed end-to-end). Seven
    // markers = one per other player in an 8-player session.
    constexpr UInt32 kMarkerLocalFormIDs[] = {
        0x0000B6CA, 0x0000B6CC, 0x0000B6CD, 0x0000B6CE,
        0x0000B6D5, 0x0000B6D6, 0x0000B6D7
    };
    constexpr size_t kMaxMarkers = sizeof(kMarkerLocalFormIDs) / sizeof(kMarkerLocalFormIDs[0]);

    // How often (seconds) to teleport the marker to the proxy's position.
    // 1 second keeps the map reasonably accurate without flooding the Papyrus
    // VM queue.
    // 2026-08-05: 1.0s -> 0.25s. The move is now the native
    // MoveRefrToPosition rather than a queued Papyrus MoveTo, so it is cheap
    // enough to run 4x as often, and a once-a-second marker visibly lags a
    // sprinting player across the map.
    static constexpr double kUpdateInterval = 0.25;

    // One slot per marker REFR. A slot is claimed by the first proxy that asks
    // for a marker and released when that proxy goes away.
    struct MarkerSlot
    {
        TESObjectREFR* markerRef      = nullptr;
        Actor*         proxyActor     = nullptr;
        uint32_t       trackedEntityID = 0;
        double         lastUpdate     = 0.0;
        bool           shown          = false;
    };
    static MarkerSlot s_slots[kMaxMarkers];
    static bool       s_initialized = false;

    // Slot currently assigned to `entityID`, or nullptr.
    static MarkerSlot* FindSlotForEntity(uint32_t entityID)
    {
        if (entityID == 0) return nullptr;
        for (MarkerSlot& s : s_slots)
            if (s.trackedEntityID == entityID) return &s;
        return nullptr;
    }

    // First slot with a resolved marker and no owner.
    static MarkerSlot* FindFreeSlot()
    {
        for (MarkerSlot& s : s_slots)
            if (s.trackedEntityID == 0 && s.markerRef) return &s;
        return nullptr;
    }
    // Entity the marker/dot is tracking -- needed so a map click can teleport.
}

// ── Native map-marker access (derived 2026-08-04, see memory) ───────────────
// Papyrus AddToMap (native 0x011530E0) begins with
//     if (!GetExtraMapMarker(refr)) return error("is not a map marker");
// so a ref without ExtraMapMarker is silently rejected -- and because every
// marker call goes through CallFunctionNoWait (fire-and-forget, no return
// value), that rejection is COMPLETELY INVISIBLE. That is why the marker could
// resolve, enable, MoveTo and "AddToMap queued" ~86 times and still never show.
//
// These let us (a) see whether the engine actually considers our ref a map
// marker, and (b) set the visible / can-travel bits ourselves rather than
// trusting a Papyrus call we get no answer from.
//   GetExtraMapMarker : 0x00562ED0  -> ExtraMapMarker* (null if not a marker)
//   SetVisible        : 0x002C0F90(marker, bool)   flags bit0 at marker+0x10
//   SetCanTravelTo    : 0x002C0FD0(marker, bool)   flags bit1 at marker+0x10
using GetExtraMapMarkerFn = void* (__fastcall*)(TESObjectREFR*);
using SetMarkerFlagFn     = void  (__fastcall*)(void*, bool);
static RelocAddr<GetExtraMapMarkerFn> g_getExtraMapMarker(0x005631F0);
static RelocAddr<SetMarkerFlagFn>     g_setMarkerVisible (0x002C12B0);
static RelocAddr<SetMarkerFlagFn>     g_setMarkerTravel  (0x002C12F0);

static constexpr uintptr_t kMarkerFlagsOffset = 0x10;

// Reports whether the engine sees `ref` as a map marker, and forces the
// visible / can-travel bits on when it does. Returns false when the ref has no
// ExtraMapMarker -- i.e. exactly the case AddToMap would reject.
__declspec(noinline) static bool SafeForceMarkerVisible(TESObjectREFR* ref, const char* context)
{
    LOG_REQUIREMENT("(global)::SafeForceMarkerVisible", "Requires a non-null REFR (checked); calls the derived native ExtraMapMarker accessor and flag setters under SEH, reporting false rather than faulting if the ref has no marker data.");
    if (!ref) return false;
    __try
    {
        void* marker = g_getExtraMapMarker(ref);
        if (!marker)
        {
            LOG_WARN("[PlayerMarker] %s: ref 0x%08X has NO ExtraMapMarker — the engine does "
                     "not consider it a map marker, so AddToMap silently rejects it",
                context, ref->formID);
            return false;
        }

        const uint8_t before = *reinterpret_cast<const uint8_t*>(
            reinterpret_cast<const uint8_t*>(marker) + kMarkerFlagsOffset);

        g_setMarkerVisible(marker, true);
        g_setMarkerTravel(marker, true);

        const uint8_t after = *reinterpret_cast<const uint8_t*>(
            reinterpret_cast<const uint8_t*>(marker) + kMarkerFlagsOffset);

        LOG_INFO("[PlayerMarker] %s: ref 0x%08X marker=%p flags 0x%02X -> 0x%02X "
                 "(bit0 visible, bit1 canTravel)",
            context, ref->formID, marker, before, after);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[PlayerMarker] %s: faulted reading/setting ExtraMapMarker on 0x%08X",
            context, ref ? ref->formID : 0);
        return false;
    }
}

__declspec(noinline) static TESObjectREFR* SafeResolveMarkerRef(UInt32 localFormID)
{
    LOG_REQUIREMENT("CoSyncPlayerMarker::SafeResolveMarkerRef",
        "Requires g_dataHandler to be resolved and CoSync.esp loaded; "
        "the supplied local formID must point at a TESObjectREFR "
        "(placed map marker) in CoSync.esp — see header comment for xEdit steps.");
    __try
    {
        DataHandler* dh = *g_dataHandler;
        if (!dh) return nullptr;

        UInt8 modIndex = dh->GetLoadedModIndex("CoSync.esp");
        if (modIndex == 0xFF)
        {
            LOG_ERROR("[PlayerMarker] CoSync.esp not in mod list");
            return nullptr;
        }

        const UInt32 formID = (static_cast<UInt32>(modIndex) << 24) | localFormID;
        TESForm* form = LookupFormByID(formID);
        if (!form)
        {
            LOG_ERROR("[PlayerMarker] Marker REFR 0x%08X not found — "
                "add CoSyncRemotePlayerMarker REFR to CoSync.esp (see header)", formID);
            return nullptr;
        }
        if (form->formType != kFormType_REFR)
        {
            LOG_ERROR("[PlayerMarker] formID 0x%08X is type %u, expected kFormType_REFR",
                formID, (unsigned)form->formType);
            return nullptr;
        }

        LOG_INFO("[PlayerMarker] Marker REFR resolved: 0x%08X", formID);
        return static_cast<TESObjectREFR*>(form);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[PlayerMarker] SafeResolveMarkerRef faulted");
        return nullptr;
    }
}

// Queues ObjectReference.MoveTo(target, 0,0,0, matchRotation=false) on the
// marker. Shared by ShowForProxy (initial placement, before AddToMap) and Tick
// (ongoing tracking) so both paths cannot drift apart.
static void QueueMarkerMoveTo(TESObjectREFR* markerRef, Actor* proxyActor)
{
    LOG_REQUIREMENT("(global)::QueueMarkerMoveTo", "Requires s_markerRef and proxyActor to be non-null (checked by callers); queues MoveTo on the marker's Papyrus VM queue -- must be called from the game thread.");
    if (!markerRef || !proxyActor)
        return;

    // 2026-08-04 HOST CRASH (real unwind, resolved via DoxCoSync.map):
    //   CoSyncPlayerMarker::Tick -> QueueMarkerMoveTo -> PackHandle
    //     -> Fallout4.exe+0x109A200  READ AV fault=0xFFFFFFFFFFFFFFFF
    //   live bytes at RIP: FF 90 90 01 00 00 = call qword ptr [rax+0x190]
    // i.e. the engine's handle-policy virtual call while packing this actor
    // into a VMValue. s_proxyActor is a cached raw Actor* ticked on a timer,
    // so it can outlive the actor -- cell transitions free proxies, not just
    // reloads. Packing a freed/unresolvable actor is what faulted.
    //
    // This is the standing rule in this codebase: ANY cached Actor* must be
    // re-validated with IsFormStillLive before it is handed to the engine.
    if (!CoSyncGameAPI::IsFormStillLive(static_cast<TESForm*>(proxyActor)))
    {
        LOG_UPDATED_WARN("[PlayerMarker] QueueMarkerMoveTo: proxy actor no longer live — skipped");
        return;
    }

    // ── NATIVE move, not Papyrus MoveTo ─────────────────────────────────────
    // 2026-08-05: the Init probe proved every marker ref DOES carry
    // ExtraMapMarker with flags 0x03, so AddToMap was never being rejected --
    // yet no marker appeared. That leaves placement: the records' authored
    // DATA is all zeros, so if MoveTo never lands every marker just sits at
    // Commonwealth (0,0,0), on the map but nowhere near the player.
    //
    // Papyrus MoveTo goes through CallFunctionNoWait, which is fire-and-forget
    // -- a failure is invisible, exactly like AddToMap's was. MoveRefrToPosition
    // is the native this codebase already relies on for debris/object sync and
    // it is synchronous, so a failure is at least observable.
    //
    // Cell/worldspace rule (same as CoSyncWorldSync and CoSyncGameAPI): exactly
    // ONE may be non-null. Exterior -> worldspace set, cell null (the engine
    // re-derives the grid cell from pos+worldspace); interior -> cell set.
    //
    // 2026-08-07: derive the context from the PROXY, not the local player. The
    // marker is meant to sit exactly where the remote player's proxy is, so its
    // placement must use the proxy's OWN worldspace/cell. Using the local
    // player's context misplaced the marker whenever the two briefly diverged
    // (cell-transition frames, interior/exterior straddle), which read as "the
    // marker isn't aligned to the proxy". Falls back to the local player's
    // context only if the proxy's own cell isn't resolvable yet.
    TESObjectREFR* proxyRefr = static_cast<TESObjectREFR*>(proxyActor);
    TESWorldSpace* ws   = CALL_MEMBER_FN(proxyRefr, GetWorldspace)();
    TESObjectCELL* cell = ws ? nullptr : proxyRefr->parentCell;

    if (!ws && !cell)
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc)
            return;
        TESObjectREFR* pcRefr = reinterpret_cast<TESObjectREFR*>(pc);
        ws   = CALL_MEMBER_FN(pcRefr, GetWorldspace)();
        cell = ws ? nullptr : pcRefr->parentCell;
    }

    NiPoint3 pos{ proxyActor->pos.x, proxyActor->pos.y, proxyActor->pos.z };
    NiPoint3 rot{ 0.f, 0.f, 0.f };
    UInt32   nullHandle = 0;
    MoveRefrToPosition(markerRef, &nullHandle, cell, ws, &pos, &rot);
}

void CoSyncPlayerMarker::Init()
{
    LOG_REQUIREMENT("CoSyncPlayerMarker::Init",
        "Idempotent — safe to call multiple times; actual work requires "
        "DataHandler ready and CoSync.esp present.");
    if (s_initialized)
        return;
    s_initialized = true;

    size_t resolved = 0;
    for (size_t i = 0; i < kMaxMarkers; ++i)
    {
        s_slots[i].markerRef = SafeResolveMarkerRef(kMarkerLocalFormIDs[i]);
        if (s_slots[i].markerRef) ++resolved;
    }
    LOG_INFO("[PlayerMarker] Init: %u/%u marker REFRs resolved — up to %u peers can be shown",
        (unsigned)resolved, (unsigned)kMaxMarkers, (unsigned)resolved);

    // Probe every resolved ref once at startup. This distinguishes "the whole
    // plugin record shape is wrong" from "only the 3 records I appended are
    // wrong" -- 0x00B6CA is the original, 0x00B6CC-CE are the copies.
    for (size_t i = 0; i < kMaxMarkers; ++i)
    {
        if (!s_slots[i].markerRef) continue;
        SafeForceMarkerVisible(s_slots[i].markerRef, "Init probe");
    }
}

void CoSyncPlayerMarker::ShowForProxy(Actor* proxyActor, uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncPlayerMarker::ShowForProxy",
        "Requires proxyActor to be a valid, spawned Actor*; s_markerRef must "
        "be non-null (resolved by Init). Queues Enable+AddToMap on the marker "
        "REFR in that order via the REFR's Papyrus VM queue, then begins "
        "periodic MoveTo tracking in Tick.");
    if (!proxyActor)
        return;

    if (!s_initialized)
        Init();

    // One marker per peer. Reuse this entity's existing slot if it already
    // has one (re-show after a cell transition), otherwise claim a free one.
    MarkerSlot* slot = FindSlotForEntity(entityID);
    if (!slot) slot = FindFreeSlot();
    if (!slot)
    {
        LOG_WARN("[PlayerMarker] ShowForProxy: no free marker REFR for entity=%u "
                 "(all %u in use) — add more marker records to CoSync.esp",
            entityID, (unsigned)kMaxMarkers);
        return;
    }
    if (!slot->markerRef)
    {
        LOG_WARN("[PlayerMarker] ShowForProxy: marker REFR not resolved — "
            "add CoSyncRemotePlayerMarker to CoSync.esp to enable map tracking");
        return;
    }

    // RE-ENABLED 2026-08-03 — the esp defect this was disabled over is fixed.
    //
    // The 2026-07-21 disable was correct at the time: dumping the shipping esp
    // confirmed REFR 0x0100B6CA carried ONLY `NAME` + `DATA`. It had no XMRK
    // (so the engine never treated it as a map marker), no FNAM (no visible /
    // can-travel flags), no TNAM (no icon), and — the part that made MoveTo
    // genuinely dangerous — it was missing the Persistent flag 0x400 despite
    // sitting in a cell-persistent-children GRUP. Moving a non-persistent,
    // effectively unparented ref once a second is exactly the sort of thing
    // that bakes a broken changed-form into the save, which is why it was a
    // plausible suspect for the infinite-loading-on-Load-Game bug.
    //
    // The esp has now been patched to the exact vanilla map-marker layout
    // (copied field-for-field off Fallout4.esm's own markers, e.g. 0x00191F00):
    //     EDID NAME XMRK FNAM(0x03) FULL TNAM(0) DATA   — in that order, with
    //     XMRK/FNAM/FULL/TNAM BEFORE DATA as vanilla has them
    // and record flags are now 0x0C00 (Persistent | Initially Disabled).
    // Structure re-verified: subrecords sum exactly to dataSize and all three
    // enclosing GRUP sizes were propagated (+72).
    //
    // FNAM 0x03 = Visible | CanTravelTo. The CanTravelTo bit is deliberate:
    // it makes the other player's marker a legitimate fast-travel destination,
    // which gives us map-driven teleport natively, with no Scaleform hooking.
    slot->trackedEntityID = entityID;
    slot->proxyActor      = proxyActor;
    slot->shown           = true;

    // ORDER IS LOAD-BEARING: Enable -> MoveTo -> AddToMap.
    //
    // 2026-08-03 second attempt. The first attempt did Enable then AddToMap
    // immediately and left the MoveTo to Tick a second later. The log showed
    // both calls queueing fine and the marker still never appeared, because
    // the marker was still sitting in its authored home cell at (0,0,0) at the
    // moment AddToMap ran -- since the esp fix that home is CoSyncMarkerCell,
    // an INTERIOR cell of our own. A map marker is registered against whatever
    // worldspace it currently occupies, so revealing it there publishes it to a
    // map nobody will ever open, not the Commonwealth map.
    //
    // Moving it onto the proxy FIRST puts it in the right worldspace before it
    // is revealed. These are all CallFunctionNoWait, which queue in order on
    // the ref's VM queue, so the sequence is preserved.
    {
        // Native enable, not Papyrus. The record carries the Initially
        // Disabled flag (0x800), so if this never lands the marker stays
        // disabled and nothing else matters -- and via CallFunctionNoWait we
        // would never find out. EnableWorldActor is synchronous and also works
        // on refs with an enable-state parent, which Papyrus Enable refuses.
        CoSyncGameAPI::EnableWorldActor(slot->markerRef);
    }

    QueueMarkerMoveTo(slot->markerRef, proxyActor);

    {
        // AddToMap takes an argument. From the vanilla script definition
        // (f4se-0.7.8/scripts/vanilla/ObjectReference.psc:212):
        //     Function AddToMap(bool abAllowFastTravel = false) native
        // The 2026-08-03 attempt pushed ZERO args. A Papyrus default value is
        // filled in by the COMPILER at the call site, not by the VM, so a
        // native invoke with the wrong arity just fails — and CallFunctionNoWait
        // is fire-and-forget, so it failed silently with nothing in the log.
        // That is why the marker resolved, enabled and tracked correctly (the
        // MoveTo lines prove all of that) yet never appeared on the Pip-Boy.
        //
        // Passing TRUE also gives us fast-travel-to-teammate for free, which is
        // the map-driven teleport half of item #3 — it matches the FNAM 0x03
        // (Visible | CanTravelTo) flags baked into the marker record in the esp.
        VMArray<VMVariable> args;
        bool allowFastTravel = true;
        VMVariable vFT; vFT.Set(&allowFastTravel); args.Push(&vFT);
        static const BSFixedString kAddToMap("AddToMap");
        CallFunctionNoWait(slot->markerRef, kAddToMap, args);
    }

    // Belt-and-braces, and the diagnostic that finally answers "why is there
    // no marker": ask the engine directly whether this ref carries
    // ExtraMapMarker, and force the visible / can-travel bits ourselves. If
    // this logs "NO ExtraMapMarker" then AddToMap was always being rejected
    // and the plugin record is the problem, not the code around it.
    {
        SafeForceMarkerVisible(slot->markerRef, "ShowForProxy");
    }

    LOG_INFO("[PlayerMarker] ShowForProxy: Enable -> MoveTo -> AddToMap queued "
             "marker=0x%08X entity=%u proxy=%p — tracking every %.1fs",
        slot->markerRef->formID, entityID, static_cast<void*>(proxyActor), kUpdateInterval);
}

void CoSyncPlayerMarker::HideForEntity(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncPlayerMarker::HideForEntity",
        "Safe no-op when the entity holds no marker slot; queues "
        "ObjectReference.Disable on that entity's marker and frees the slot.");
    MarkerSlot* slot = FindSlotForEntity(entityID);
    if (!slot)
        return;

    // Stop tracking FIRST so a concurrent Tick cannot queue a MoveTo onto a
    // marker we are about to disable.
    slot->trackedEntityID = 0;
    slot->proxyActor      = nullptr;
    slot->shown           = false;

    if (!slot->markerRef)
        return;

    VMArray<VMVariable> args;
    bool fadeOut = false;
    VMVariable vFade; vFade.Set(&fadeOut); args.Push(&vFade);
    static const BSFixedString kDisable("Disable");
    CallFunctionNoWait(slot->markerRef, kDisable, args);

    LOG_INFO("[PlayerMarker] HideForEntity: Disable queued entity=%u marker=0x%08X",
        entityID, slot->markerRef->formID);
}

void CoSyncPlayerMarker::HideMarker()
{
    LOG_REQUIREMENT("CoSyncPlayerMarker::HideMarker",
        "Safe no-op when nothing is tracked; hides EVERY assigned marker. Used "
        "on disconnect/teardown -- per-player removal is HideForEntity.");
    for (MarkerSlot& s : s_slots)
        if (s.trackedEntityID != 0)
            CoSyncPlayerMarker::HideForEntity(s.trackedEntityID);
}

void CoSyncPlayerMarker::Tick(double now)
{
    LOG_REQUIREMENT("CoSyncPlayerMarker::Tick",
        "No hard precondition — safe no-op when nothing is tracked; queues "
        "ObjectReference.MoveTo(proxyActor, 0,0,0, false) on each assigned "
        "marker REFR. Throttled per slot to kUpdateInterval seconds.");

    // ── Freeze marker movement during loading / fast travel ─────────────────
    // 2026-08-05: the markers carry CanTravelTo (FNAM 0x03), so they are
    // legitimate fast-travel destinations -- and they are now relocated 4x a
    // second. Moving a destination while the engine is travelling to it is an
    // obvious way to break the trip, which fits the reported client teleport
    // trouble. Hold position while a loading screen is up; the marker resumes
    // tracking the moment the world is back.
    if (CoSyncOverlay_IsLoadingScreenActive())
        return;

    for (MarkerSlot& slot : s_slots)
    {
        if (!slot.markerRef || slot.trackedEntityID == 0)
            continue;

        if (now - slot.lastUpdate < kUpdateInterval)
            continue;

        slot.lastUpdate = now;

        // ── Re-resolve the actor from the ENTITY every cycle ─────────────────
        // 2026-08-05: the marker was reported jumping to random NPCs. Cause:
        // slot.proxyActor is a cached raw Actor*, captured once at
        // ShowForProxy and then used for the rest of the session. When the
        // engine frees a proxy (cell transitions free them) and later reuses
        // that memory/formID for something else -- very often an NPC, since
        // those churn constantly -- the marker happily followed whatever now
        // lives there. IsFormStillLive cannot catch this: the form IS live, it
        // is simply a DIFFERENT actor.
        //
        // The entityID is the stable identity, so resolve through it each time
        // and let the player manager say where that peer's actor is now.
        CoSyncPlayer* peer = g_CoSyncPlayerManager.GetPlayerByEntityID(slot.trackedEntityID);
        Actor* liveActor = (peer && peer->hasSpawned) ? peer->actorRef : nullptr;
        if (!liveActor)
            continue;

        if (liveActor != slot.proxyActor)
        {
            LOG_UPDATED("[PlayerMarker] entity=%u proxy actor changed (%p -> %p) — "
                        "re-targeting marker 0x%08X",
                slot.trackedEntityID, static_cast<void*>(slot.proxyActor),
                static_cast<void*>(liveActor), slot.markerRef->formID);
            slot.proxyActor = liveActor;
        }

        QueueMarkerMoveTo(slot.markerRef, liveActor);
    }

    // Throttled liveness proof (LOG_TRACE compiles out in Release, which is
    // why an earlier run's "0 MoveTo lines" proved nothing).
    static uint64_t s_nextLivenessMs = 0;
    if (GetTickCount64() >= s_nextLivenessMs && (s_nextLivenessMs = GetTickCount64() + 15000) != 0)
    {
        for (const MarkerSlot& sl : s_slots)
        {
            if (sl.trackedEntityID == 0 || !sl.markerRef || !sl.proxyActor) continue;
            // The decisive number: where the marker ACTUALLY is versus where
            // the proxy is. If the marker stays at (0,0,0) -- its authored
            // DATA -- then the move is not landing and every marker is sitting
            // at the Commonwealth origin instead of on the player.
            LOG_INFO("[PlayerMarker] entity=%u marker=0x%08X at (%.0f,%.0f,%.0f) "
                     "proxy at (%.0f,%.0f,%.0f) disabled=%d",
                sl.trackedEntityID, sl.markerRef->formID,
                sl.markerRef->pos.x, sl.markerRef->pos.y, sl.markerRef->pos.z,
                sl.proxyActor->pos.x, sl.proxyActor->pos.y, sl.proxyActor->pos.z,
                (sl.markerRef->flags & TESObjectREFR::kFlag_IsDisabled) ? 1 : 0);
        }
    }
}
