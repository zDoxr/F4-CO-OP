#include "CoSyncItemMods.h"
#include "CoSyncPowerArmorSync.h"
#include "CoSyncDoorHook.h"

#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncWorldSync.h"
#include "CoSyncGameAPI.h"
#include "CoSyncOutfitSync.h"
#include "CoSyncEquipHook.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncAnimationUtils.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncVMFormArg.h"
#include "GameForms.h"
#include "GameReferences.h"
#include "GameRTTI.h"
#include "GameData.h"
#include "GameObjects.h"
#include "GameFormComponents.h"
#include "GameExtraData.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"

#include "GameHandle.h"

#include <atomic>
#include <mutex>
#include <cmath>
#include <algorithm>
#include <vector>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

extern RelocAddr<_LookupFormByID>  LookupFormByID;
extern RelocAddr<_MoveRefrToPosition> MoveRefrToPosition;
extern CoSyncPlayerManager         g_CoSyncPlayerManager;
extern PlayerCharacter*            CoSyncGetPlayer();

// PA enter (IDA-verified 2026-07-15, SwitchToPowerArmor native sub_1410FBF90 →
// sub_140989D90 -- CORRECTED 2026-08-26, the 989A80 figure below and in older
// comments was a transcription error, mid-instruction in an unrelated 397-byte
// function with 0 real callers; the real, 3447-byte function with 8 direct
// callers is 989D90, re-decompiled and confirmed at CoSyncGameAPI.cpp's own
// g_EngineSwitchToPowerArmor RVA, which was ALREADY correct -- only this
// comment's address was stale): the enter path does the full model/biped/frame
// merge itself (it even calls ActorEquipManager::EquipObject at 0xCE5A90
// internally, with applyNow=0/deferred for PA pieces specifically -- see the
// force-immediate-reapply loop after SwitchToPowerArmorDirect below) and
// does NOT need actor AI — so it works on an AI-disabled proxy. Its ONE hard
// requirement is a valid PA *furniture* ref (baseform type FURN=42). The
// receiving client almost never has the host's chassis (dynamic 0xFF ref, or
// simply not near an identical frame), so SwitchToPowerArmor used to be called
// with a null/unresolved furniture and silently no-op — this is why PA never
// attached to proxies. Fix: spawn a fresh chassis at the proxy from the base
// form the host sends, enter it, and delete it again on exit.

// 2026-07-30 diagnostic: IDA-traced (canonical Fallout4_disk.i64) the real
// PlaceAtMe_Native implementation (sub_141159DB0) for the base=0x00034309
// WRITE-AV crash (Fallout4.exe+0x165910F). Before constructing the new
// object it reads a QWORD component-list pointer at [baseForm + 0x100] and,
// if non-null, unconditionally acquires a spinlock at [thatPointer + 0x20] --
// no null/sanity check gates the lock itself. Theory: this field is
// null/garbage on forms the engine never expects to be placed synthetically
// (matches every other form on kWorkshopInternalHelperForms), which turns
// the lock's target address into garbage and faults there. This log is
// read-only and SEH-isolated from the actual placement -- it exists purely
// to confirm/refine that theory with live data and catch OTHER forms that
// hit the same class before they crash for real. Safe even if the offset
// theory is wrong: worst case it logs a bogus-looking pointer, it never
// dereferences past the one QWORD.
__declspec(noinline) static void LogPlaceAtMeComponentDiag(TESForm* base)
{
    __try
    {
        const uint64_t componentListPtr = *reinterpret_cast<const uint64_t*>(
            reinterpret_cast<const uint8_t*>(base) + 0x100);
        LOG_UPDATED("[PASync] PlaceAtMe diag: base=0x%08X formType=%u [base+0x100]=0x%016llX%s",
            base->formID, base->formType, componentListPtr,
            componentListPtr == 0 ? "  <-- NULL, matches the crash theory" : "");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_UPDATED_WARN("[PASync] PlaceAtMe diag: faulted reading [base+0x100] for base=0x%08X",
            base ? base->formID : 0);
    }
}

// Superseded PlaceAtMe_Native entirely -- CreateReferenceAtLocation
// (CoSyncGameAPI::SpawnItemAtPosition) doesn't go through PlaceAtMe_Native's
// internal component-lookup-then-lock sequence at all, sidestepping the
// crash class above by construction rather than by blocklisting forms.
static TESObjectREFR* SpawnChassisAtProxy(TESForm* base, TESObjectREFR* atRef)
{
    if (!base || !atRef) return nullptr;
    NiPoint3 pos{ atRef->pos.x, atRef->pos.y, atRef->pos.z };
    // Facing set at spawn (dir field), not via a post-spawn MoveRefrToPosition
    // rotate -- IDA-confirmed dead for a non-actor ref (1.11.240.0). Yaw only,
    // matching the proxy's own facing; pitch/roll left at 0 like the rest of
    // this codebase's chassis placement always assumed.
    NiPoint3 rot{ 0.f, 0.f, atRef->rot.z };
    return CoSyncGameAPI::SpawnItemAtPosition(base, pos, rot);
}

__declspec(noinline) static float SafeEquippedItemHealth(Actor* actor, uint32_t fid);   // below
static bool IsBrokenPiece(float health) { return health >= 0.f && health <= 0.001f; }

static double NowSecondsPA()
{
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

// Defined in Papyrus_CoSync.cpp. Walks the actor's equipped items and fills
// the 6 PA slots (head/torso/lArm/rArm/lLeg/rLeg); returns how many it found.
extern int ScanActorEquippedPAPieces(Actor* actor, uint32_t outPieceFormIDs[6]);

// 2026-08-26: this WAS local to TickLocalPowerArmorPieces (function-static),
// which meant it was NEVER assigned anywhere -- the periodic re-broadcast
// below always sent chassisBaseFormID=0 and NotifyPowerArmor's own `if
// (chassisBaseFormID == 0) return;` guard silently dropped it before it ever
// reached the network. Live log confirmed it: "local PA pieces changed while
// worn -- re-broadcast [...]" fired with no matching "TX PA" line, every
// time. Moved to file scope and now actually SET by NotifyPowerArmor itself
// (see its body) whenever WE are the one entering, so this always reflects
// the real chassis this client is currently wearing. Root cause of "PA
// pieces don't show / don't update" for any piece swapped mid-session.
static uint32_t s_localChassisBaseFormID = 0;
static uint32_t s_localChassisRefFormID  = 0;
static NiPoint3 s_localChassisPos;

// ── Live PA piece re-sync (list item: helmet stopped syncing) ───────────────
// PA pieces were captured EXACTLY ONCE, inside the Papyrus enter event
// (CoSync_NotifyPowerArmor fires on OnSit/OnGetUp). So any piece changed while
// already wearing the frame -- swapping a helmet being the obvious case --
// never reached peers, and the proxy kept whatever it had at enter time.
//
// Re-scan the local player's equipped pieces on a timer while we know they are
// in PA, and re-broadcast only when the set actually changes. Cheap (an
// inventory walk every few seconds, and only while in power armor) and it
// needs no new Papyrus events.
static void TickLocalPowerArmorPieces()
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::TickLocalPowerArmorPieces", "Needs an active connection and a player who's actually in power armor (both checked first). Reading the equipped pieces is done safely.");
    if (!CoSyncNet::IsConnected())
        return;

    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc)
        return;

    Actor* me = reinterpret_cast<Actor*>(pc);
    if (!CoSyncPowerArmorSync::IsActorKnownInPowerArmor(me->formID))
        return;   // only meaningful while actually wearing a frame

    static double   s_lastCheck = 0.0;
    static uint32_t s_lastPieces[6] = {};

    const double now = NowSecondsPA();
    if (now - s_lastCheck < 2.0)
        return;
    s_lastCheck = now;

    // The shared chassis identifiers (set by NotifyPowerArmor on our own
    // enter) are only meaningful once an enter has actually happened this
    // session. IsActorKnownInPowerArmor above should already guarantee that,
    // but guard anyway rather than re-broadcast a stale/zero chassis.
    if (s_localChassisBaseFormID == 0)
        return;

    uint32_t pieces[6] = {};
    ScanActorEquippedPAPieces(me, pieces);

    // 0.3.3 (Stage 5): Power Armor is STATE, not just an event. PA| used to
    // go out only on enter, exit and piece changes, so a player who joined
    // (or whose proxy of us was freed and rebuilt) after we got in never
    // learned we were in Power Armor and saw a normal body. Re-send the
    // current state every 5 s while worn; receivers ignore an unchanged
    // repeat (FlushPendingPowerArmorOps).
    static double s_lastStateSend = 0.0;
    // A piece breaking or being repaired changes what others must see.
    static int s_lastBroken[6] = {};
    int broken[6] = {};
    for (int i = 0; i < 6; ++i)
        broken[i] = (pieces[i] && IsBrokenPiece(SafeEquippedItemHealth(me, pieces[i]))) ? 1 : 0;
    const bool changed   = memcmp(pieces, s_lastPieces, sizeof(pieces)) != 0 ||
                           memcmp(broken, s_lastBroken, sizeof(broken)) != 0;
    memcpy(s_lastBroken, broken, sizeof(broken));
    const bool keepAlive = (now - s_lastStateSend) >= 5.0;
    if (!changed && !keepAlive)
        return;
    s_lastStateSend = now;

    memcpy(s_lastPieces, pieces, sizeof(pieces));

    // Re-announce as an "enter" so the receiver re-applies the piece set to
    // the proxy; the chassis identifiers/position are the real ones this
    // client is actually wearing (see s_localChassisBaseFormID's comment).
    CoSyncPowerArmorSync::NotifyPowerArmor(
        CoSyncNet::GetMyEntityID(),
        s_localChassisBaseFormID,
        s_localChassisRefFormID,
        true,
        pieces,
        s_localChassisPos);

    if (changed)
        LOG_UPDATED("[PASync] local PA pieces changed while worn — re-broadcast "
                    "[%08X %08X %08X %08X %08X %08X]",
            pieces[0], pieces[1], pieces[2], pieces[3], pieces[4], pieces[5]);
}

// ── Engine SwitchFromPowerArmor (native) ────────────────────────────────────
// RVA 0x0098BE40 on 1.11.240 (re-checked 2026-09-28: the "SwitchFromPowerArmor"
// string 0x0252B6E8 is referenced at 0x98C371 inside it; 0x0098BB30 was the
// 1.11.221 figure). IDA-verified 2026-08-05. Derived by finding the
// "SwitchFromPowerArmor" string (0x02523678), which this very function hands to
// the engine logger together with its source line (0x5E5) at 0x0098C061 -- the
// function literally names itself. Entry rva confirmed 0x0098BB30, ONE argument
// (the actor), 6 callers, no VirtualMachine and no Papyrus stack ID, so unlike
// the SetCurrentStageID native it is safe to call directly from C++.
//
// Replaces CallFunctionNoWait("SwitchFromPowerArmor"), which is fire-and-forget
// and therefore hid the fact that the eject was not happening at all -- the
// cause of the model staying stretched in the power-armor pose after exit.
using SwitchFromPowerArmorFn = char(__fastcall*)(Actor*);
static RelocAddr<SwitchFromPowerArmorFn> g_switchFromPowerArmor(0x0098BE40);

__declspec(noinline) static bool SafeSwitchFromPowerArmorNative(Actor* actor)
{
    LOG_REQUIREMENT("(global)::SafeSwitchFromPowerArmorNative", "Needs a valid actor (checked by the caller). Ejects them from power armor safely, catching any crash along the way.");
    if (!actor) return false;
    __try
    {
        g_switchFromPowerArmor(actor);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[PASync] native SwitchFromPowerArmor faulted");
        return false;
    }
}

namespace
{
    struct PaOp
    {
        uint32_t senderEntityID    = 0;
        uint32_t chassisBaseFormID = 0;
        uint32_t chassisRefFormID  = 0;
        bool     entering          = false;
        bool     forceClone        = false;   // 0.3.6: animated entry was tried and did not take
        // PA piece formIDs for the 6 slots (head/torso/lArm/rArm/lLeg/rLeg).
        // 0 = slot empty. Embedded in the PA| enter packet so they arrive
        // atomically — no PAI| race with FlushPendingPowerArmorOps.
        uint32_t pieceFormIDs[6]   = {};
        // 0.3.3: condition of each piece (-1 = full); a broken piece (0) is
        // shown off the wearer, as the engine shows a broken piece.
        float    pieceHealth[6]    = { -1.f, -1.f, -1.f, -1.f, -1.f, -1.f };
        // The REAL chassis ref's world position at the moment of this event
        // (see NotifyPowerArmor's header comment) -- used to locate this
        // client's own local representation of that frame, not the fake
        // clone spawned for the proxy attach.
        NiPoint3 chassisPos;
    };

    std::mutex          s_mutex;
    std::vector<PaOp>   s_pending;
    // Latest reported worn piece set per sender (raw, broken pieces too),
    // under s_mutex. Entry removed on the sender's exit packet.
    std::unordered_map<uint32_t, std::vector<uint32_t>> s_rxPaPieces;
    // 0.3.6: animated entries in flight (proxy activated its real frame).
    struct PaAnimatedEnter { PaOp op; uint32_t frame = 0; double deadline = 0.0; };
    std::vector<PaAnimatedEnter> s_animatedEnters;   // tick thread only
    // Set by PADENY (network thread or host), consumed on the tick.
    std::atomic<bool> s_localEntryDenied{ false };
    std::atomic<bool> s_notifyEntryRefused{ false };

    // Sender-side PA state, keyed by real actor formID. Written from the
    // main/game thread only (Papyrus OnSit/OnGetUp -> CoSync_NotifyPowerArmor
    // -> MarkActorPowerArmorState), read from the same thread's per-tick
    // animation capture -- no cross-thread contention, no mutex needed,
    // matching this codebase's convention for same-thread-only statics
    // (e.g. CoSyncPlayerManager.cpp's s_companionRefToEntityID).
    std::unordered_set<uint32_t> s_locallyKnownInPA;
}

void CoSyncPowerArmorSync::MarkActorPowerArmorState(uint32_t actorFormID, bool inPowerArmor)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::MarkActorPowerArmorState", "Just updates a small internal list. The caller already made sure the actor is valid.");
    if (actorFormID == 0) return;
    if (inPowerArmor)
        s_locallyKnownInPA.insert(actorFormID);
    else
        s_locallyKnownInPA.erase(actorFormID);
}

bool CoSyncPowerArmorSync::IsActorKnownInPowerArmor(uint32_t actorFormID)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::IsActorKnownInPowerArmor", "Just checks a small internal list. No game state needed.");
    return actorFormID != 0 && s_locallyKnownInPA.count(actorFormID) != 0;
}

bool CoSyncPowerArmorSync::IsPowerArmor(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::IsPowerArmor", "Just checks the text of a message. No game state needed.");
    return msg.rfind("PA|", 0) == 0;
}

std::string CoSyncPowerArmorSync::SerializePowerArmor(
    uint32_t        senderEntityID,
    uint32_t        chassisBaseFormID,
    uint32_t        chassisRefFormID,
    bool            entering,
    const uint32_t  pieceFormIDs[6],
    const NiPoint3& chassisPos)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::SerializePowerArmor", "Just turns power armor info into text. Needs valid values for who's sending it and what armor/pieces are involved.");
    // Position sits right after chassisRefFormID and BEFORE the entering
    // flag, on BOTH branches -- so ParsePowerArmor can read it identically
    // before the two branches diverge on trailing piece data.
    if (entering)
    {
        // Enter packet carries 6 piece slots so they arrive atomically with
        // the PA state change — no separate PAI| packet race.
        const uint32_t* p = pieceFormIDs;
        char buf[400];
        // 0.3.3: + the condition of each piece (-1 = full), read off the
        // sender's own equipped stacks.
        float h[6] = { -1.f, -1.f, -1.f, -1.f, -1.f, -1.f };
        if (p)
            if (PlayerCharacter* pc = CoSyncGetPlayer())
                for (int i = 0; i < 6; ++i)
                    if (p[i]) h[i] = SafeEquippedItemHealth(reinterpret_cast<Actor*>(pc), p[i]);
        std::snprintf(buf, sizeof(buf), "PA|%u|%u|%u|%.2f|%.2f|%.2f|1|%u|%u|%u|%u|%u|%u|%.3f|%.3f|%.3f|%.3f|%.3f|%.3f",
            senderEntityID, chassisBaseFormID, chassisRefFormID,
            chassisPos.x, chassisPos.y, chassisPos.z,
            p ? p[0] : 0u, p ? p[1] : 0u, p ? p[2] : 0u,
            p ? p[3] : 0u, p ? p[4] : 0u, p ? p[5] : 0u,
            h[0], h[1], h[2], h[3], h[4], h[5]);
        return std::string(buf);
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "PA|%u|%u|%u|%.2f|%.2f|%.2f|0",
        senderEntityID, chassisBaseFormID, chassisRefFormID,
        chassisPos.x, chassisPos.y, chassisPos.z);
    return std::string(buf);
}

static bool ParsePowerArmor(const std::string& msg,
    uint32_t& outSender,
    uint32_t& outBaseFormID,
    uint32_t& outRefFormID,
    bool&     outEntering,
    uint32_t  outPieces[6],
    NiPoint3& outChassisPos,
    float     outHealth[6])
{
    for (int i = 0; i < 6; ++i) outHealth[i] = -1.f;
    LOG_REQUIREMENT("CoSyncPowerArmorSync::ParsePowerArmor", "Reads power armor info back out of text. Just needs a real message and somewhere to write the result.");
    memset(outPieces, 0, 6 * sizeof(uint32_t));
    outChassisPos = NiPoint3();

    if (msg.rfind("PA|", 0) != 0)
        return false;

    std::stringstream ss(msg.substr(3));
    std::string tok;

    if (!std::getline(ss, tok, '|') || tok.empty()) return false;
    outSender = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));

    if (!std::getline(ss, tok, '|') || tok.empty()) return false;
    outBaseFormID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));

    if (!std::getline(ss, tok, '|') || tok.empty()) return false;
    outRefFormID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));

    if (!std::getline(ss, tok, '|') || tok.empty()) return false;
    outChassisPos.x = std::strtof(tok.c_str(), nullptr);
    if (!std::getline(ss, tok, '|') || tok.empty()) return false;
    outChassisPos.y = std::strtof(tok.c_str(), nullptr);
    if (!std::getline(ss, tok, '|') || tok.empty()) return false;
    outChassisPos.z = std::strtof(tok.c_str(), nullptr);

    // Use '|' delimiter here so that enter packets (1|p0|p1|...) correctly
    // parse the flag field as just "1" and not the entire tail of the string.
    if (!std::getline(ss, tok, '|') || tok.empty()) return false;
    outEntering = (tok == "1");

    if (outEntering)
    {
        // Read up to 6 piece slots — absent fields default to 0 (backward
        // compatible with older PA| packets that carry no piece data).
        for (int i = 0; i < 6; ++i)
        {
            if (!std::getline(ss, tok, '|') || tok.empty()) break;
            outPieces[i] = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
        }
        // 0.3.3: optional piece conditions (absent from older senders).
        for (int i = 0; i < 6; ++i)
        {
            if (!std::getline(ss, tok, '|') || tok.empty()) break;
            const float h = std::strtof(tok.c_str(), nullptr);
            outHealth[i] = (std::isfinite(h) && h >= 0.f && h <= 1.f) ? h : -1.f;
        }
    }

    return true;
}

// ── 0.3.6: chassis ownership ────────────────────────────────────────────────
static bool SafeIsRefPowerArmorFrame(TESObjectREFR* ref);   // below
namespace
{
    struct PaOwnerRec
    {
        uint32_t ref   = 0;     // the owner's formID for the frame
        uint32_t base  = 0;
        NiPoint3 pos;           // where the frame stands (entry, then last exit)
        uint32_t owner = 0;
        double   ownerMissingSince = 0.0;   // host: owner not in the session
    };
    std::mutex              s_ownMx;
    std::vector<PaOwnerRec> s_owners;
    double                  s_ownLastBroadcast = 0.0;
    constexpr float         kOwnMatchDist   = 128.f;
    constexpr double        kOwnReleaseSec  = 30.0;
    constexpr double        kOwnResendSec   = 10.0;

    float Dist2(const NiPoint3& a, const NiPoint3& b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    }

    // Caller holds s_ownMx.
    int FindOwnerRecLocked(uint32_t ref, uint32_t base, const NiPoint3& pos)
    {
        for (size_t i = 0; i < s_owners.size(); ++i)
            if (s_owners[i].base == base && ref != 0 && s_owners[i].ref == ref)
                return static_cast<int>(i);
        for (size_t i = 0; i < s_owners.size(); ++i)
            if (s_owners[i].base == base && Dist2(s_owners[i].pos, pos) <= kOwnMatchDist * kOwnMatchDist)
                return static_cast<int>(i);
        return -1;
    }

    std::string OwnMsg(const PaOwnerRec& r)
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "PAOWN|%u|%u|%.2f|%.2f|%.2f|%u",
            r.ref, r.base, r.pos.x, r.pos.y, r.pos.z, r.owner);
        return buf;
    }

    // Host only: a PA enter/exit reported by `sender` (or by the host itself).
    void HostOnPaEvent(uint32_t sender, uint32_t ref, uint32_t base, const NiPoint3& pos, bool entering)
    {
        if (!CoSyncNet::IsHost() || !sender || !base)
            return;
        std::string send;
        std::string deny;
        {
            std::lock_guard<std::mutex> lk(s_ownMx);
            const int i = FindOwnerRecLocked(ref, base, pos);
            if (entering)
            {
                if (i < 0)
                {
                    PaOwnerRec r;
                    r.ref = ref; r.base = base; r.pos = pos; r.owner = sender;
                    s_owners.push_back(r);
                    send = OwnMsg(r);
                    LOG_UPDATED("[PASync] PA Owner Assigned: frame ref=0x%08X base=0x%08X at (%.0f,%.0f,%.0f) -> entity=%u",
                        ref, base, pos.x, pos.y, pos.z, sender);
                }
                else if (s_owners[i].owner == 0)
                {
                    s_owners[i].owner = sender;
                    s_owners[i].ref = ref;
                    send = OwnMsg(s_owners[i]);
                    LOG_UPDATED("[PASync] PA Owner Assigned: released frame ref=0x%08X -> entity=%u", ref, sender);
                }
                else if (s_owners[i].owner != sender)
                {
                    char b[64];
                    std::snprintf(b, sizeof(b), "PADENY|%u|%u", sender, ref);
                    deny = b;
                    LOG_UPDATED_WARN("[PASync] PA entry DENIED: entity=%u entered frame ref=0x%08X owned by entity=%u",
                        sender, ref, s_owners[i].owner);
                }
            }
            else if (i >= 0 && s_owners[i].owner == sender)
            {
                // The frame stays where its owner got out; ownership is kept.
                if (pos.x != 0.f || pos.y != 0.f || pos.z != 0.f)
                    s_owners[i].pos = pos;
                s_owners[i].ref = ref ? ref : s_owners[i].ref;
                send = OwnMsg(s_owners[i]);
                LOG_UPDATED("[PASync] PA Exit Completed: frame ref=0x%08X stays owned by entity=%u at (%.0f,%.0f,%.0f)",
                    s_owners[i].ref, sender, s_owners[i].pos.x, s_owners[i].pos.y, s_owners[i].pos.z);
            }
        }
        if (!send.empty())
            CoSyncTransport::Send(send);
        if (!deny.empty())
        {
            if (sender == CoSyncNet::GetMyEntityID())
                CoSyncPowerArmorSync::OnReceivePowerArmorOwnership(deny);   // the host itself lost the race
            else
                CoSyncTransport::Send(deny);
        }
    }

    // Host tick: release frames whose owner left, re-send the table.
    void HostTickOwnership(double now)
    {
        if (!CoSyncNet::IsHost() || !CoSyncNet::IsConnected())
            return;
        std::vector<std::string> out;
        {
            std::lock_guard<std::mutex> lk(s_ownMx);
            const uint32_t me = CoSyncNet::GetMyEntityID();
            for (PaOwnerRec& r : s_owners)
            {
                if (r.owner == 0 || r.owner == me)
                    continue;
                if (g_CoSyncPlayerManager.GetPlayerByEntityID(r.owner))
                {
                    r.ownerMissingSince = 0.0;
                    continue;
                }
                if (r.ownerMissingSince == 0.0)
                    r.ownerMissingSince = now;
                else if (now - r.ownerMissingSince >= kOwnReleaseSec)
                {
                    LOG_UPDATED("[PASync] PA Owner released: entity=%u left the session, frame ref=0x%08X is free",
                        r.owner, r.ref);
                    r.owner = 0;
                    out.push_back(OwnMsg(r));
                }
            }
            if (now - s_ownLastBroadcast >= kOwnResendSec)
            {
                s_ownLastBroadcast = now;
                for (const PaOwnerRec& r : s_owners)
                    if (r.owner != 0)
                        out.push_back(OwnMsg(r));
            }
            s_owners.erase(std::remove_if(s_owners.begin(), s_owners.end(),
                [](const PaOwnerRec& r) { return r.owner == 0; }), s_owners.end());
        }
        for (const std::string& m : out)
            CoSyncTransport::Send(m);
    }
}

bool CoSyncPowerArmorSync::IsPowerArmorOwnership(const std::string& msg)
{
    return msg.rfind("PAOWN|", 0) == 0 || msg.rfind("PADENY|", 0) == 0;
}

void CoSyncPowerArmorSync::OnReceivePowerArmorOwnership(const std::string& msg)
{
    if (msg.rfind("PADENY|", 0) == 0)
    {
        uint32_t eid = 0, ref = 0;
        if (std::sscanf(msg.c_str() + 7, "%u|%u", &eid, &ref) != 2 || eid != CoSyncNet::GetMyEntityID())
            return;
        LOG_UPDATED_WARN("[PASync] PA entry DENIED by host: frame ref=0x%08X is owned by another player -- ejecting", ref);
        s_localEntryDenied.store(true);
        return;
    }
    if (CoSyncNet::IsHost())
        return;   // the host's table is the source
    PaOwnerRec r;
    if (std::sscanf(msg.c_str() + 6, "%u|%u|%f|%f|%f|%u", &r.ref, &r.base, &r.pos.x, &r.pos.y, &r.pos.z, &r.owner) != 6)
        return;
    std::lock_guard<std::mutex> lk(s_ownMx);
    const int i = FindOwnerRecLocked(r.ref, r.base, r.pos);
    if (r.owner == 0)
    {
        if (i >= 0)
            s_owners.erase(s_owners.begin() + i);
        return;
    }
    if (i < 0)
    {
        s_owners.push_back(r);
        LOG_UPDATED("[PASync] PA Owner: frame ref=0x%08X base=0x%08X is owned by entity=%u", r.ref, r.base, r.owner);
    }
    else
        s_owners[i] = r;
}

bool CoSyncPowerArmorSync::ShouldBlockLocalEntry(TESObjectREFR* frame)
{
    if (!frame || !CoSyncNet::IsConnected() || !SafeIsRefPowerArmorFrame(frame))
        return false;
    TESForm* base = frame->baseForm;
    if (!base)
        return false;
    const uint32_t me = CoSyncNet::GetMyEntityID();
    uint32_t owner = 0;
    {
        std::lock_guard<std::mutex> lk(s_ownMx);
        const int i = FindOwnerRecLocked(frame->formID, base->formID, frame->pos);
        if (i >= 0)
            owner = s_owners[i].owner;
    }
    const bool block = owner != 0 && owner != me;
    if (block)
        s_notifyEntryRefused.store(true);   // shown from the tick (this runs on the activation thread)
    return block;
}

void CoSyncPowerArmorSync::NotifyPowerArmor(
    uint32_t        senderEntityID,
    uint32_t        chassisBaseFormID,
    uint32_t        chassisRefFormID,
    bool            entering,
    const uint32_t  pieceFormIDs[6],
    const NiPoint3& chassisPos)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::NotifyPowerArmor", "Needs an active connection and a real armor frame to talk about (both checked first) before sending.");
    if (!CoSyncNet::IsConnected())
        return;

    if (chassisBaseFormID == 0)
        return;

    // Both call sites (CoSync_NotifyPowerArmor, TickLocalPowerArmorPieces)
    // only ever report OUR OWN local PA state, never relay someone else's --
    // safe to cache here as the one source of truth TickLocalPowerArmorPieces
    // reads from for its periodic mid-session piece re-broadcast.
    if (entering)
    {
        s_localChassisBaseFormID = chassisBaseFormID;
        s_localChassisRefFormID  = chassisRefFormID;
        s_localChassisPos        = chassisPos;
    }

    const std::string msg = SerializePowerArmor(
        senderEntityID, chassisBaseFormID, chassisRefFormID, entering, pieceFormIDs, chassisPos);
    {
        static bool s_lastEntering = false;
        if (entering != s_lastEntering)
            LOG_UPDATED("[PASync] PA Detected (local): %s frame ref=0x%08X base=0x%08X at (%.0f,%.0f,%.0f)",
                entering ? "entered" : "exited", chassisRefFormID, chassisBaseFormID, chassisPos.x, chassisPos.y, chassisPos.z);
        s_lastEntering = entering;
    }
    CoSyncTransport::Send(msg);
    HostOnPaEvent(senderEntityID, chassisRefFormID, chassisBaseFormID, chassisPos, entering);

    LOG_UPDATED("[PASync] TX PA sender=%u base=0x%08X ref=0x%08X pos=(%.0f,%.0f,%.0f) entering=%d pieces=[%u,%u,%u,%u,%u,%u]",
        senderEntityID, chassisBaseFormID, chassisRefFormID,
        chassisPos.x, chassisPos.y, chassisPos.z, entering ? 1 : 0,
        pieceFormIDs ? pieceFormIDs[0] : 0u, pieceFormIDs ? pieceFormIDs[1] : 0u,
        pieceFormIDs ? pieceFormIDs[2] : 0u, pieceFormIDs ? pieceFormIDs[3] : 0u,
        pieceFormIDs ? pieceFormIDs[4] : 0u, pieceFormIDs ? pieceFormIDs[5] : 0u);
}

void CoSyncPowerArmorSync::OnReceivePowerArmor(const std::string& msg)
{
    uint32_t senderEntityID    = 0;
    uint32_t chassisBaseFormID = 0;
    uint32_t chassisRefFormID  = 0;
    bool     entering          = false;

    LOG_REQUIREMENT("CoSyncPowerArmorSync::OnReceivePowerArmor", "Needs to know our own player ID so it can tell a genuine message apart from our own message bouncing back.");
    uint32_t pieces[6] = {};
    float    pieceHealth[6];
    NiPoint3 chassisPos;
    if (!ParsePowerArmor(msg, senderEntityID, chassisBaseFormID, chassisRefFormID, entering, pieces, chassisPos, pieceHealth))
    {
        LOG_UPDATED_WARN("[PASync] Malformed PA packet: %s", msg.c_str());
        return;
    }


    if (senderEntityID != 0 && senderEntityID == CoSyncNet::GetMyEntityID())
        return;

    LOG_UPDATED("[PASync] RX PA sender=%u base=0x%08X ref=0x%08X pos=(%.0f,%.0f,%.0f) entering=%d pieces=[%u,%u,%u,%u,%u,%u]",
        senderEntityID, chassisBaseFormID, chassisRefFormID,
        chassisPos.x, chassisPos.y, chassisPos.z, entering ? 1 : 0,
        pieces[0], pieces[1], pieces[2], pieces[3], pieces[4], pieces[5]);

    HostOnPaEvent(senderEntityID, chassisRefFormID, chassisBaseFormID, chassisPos, entering);

    std::lock_guard<std::mutex> lk(s_mutex);
    if (entering)
        s_rxPaPieces[senderEntityID].assign(pieces, pieces + 6);
    else
        s_rxPaPieces.erase(senderEntityID);
    PaOp op{};
    op.senderEntityID    = senderEntityID;
    op.chassisBaseFormID = chassisBaseFormID;
    op.chassisRefFormID  = chassisRefFormID;
    op.entering          = entering;
    op.chassisPos        = chassisPos;
    memcpy(op.pieceFormIDs, pieces, sizeof(pieces));
    memcpy(op.pieceHealth, pieceHealth, sizeof(pieceHealth));
    // A broken piece (condition 0) is not worn visibly: the engine shows the
    // frame there. Treat it as an empty slot on the proxy.
    for (int i = 0; i < 6; ++i)
        if (op.pieceFormIDs[i] && IsBrokenPiece(op.pieceHealth[i]))
            op.pieceFormIDs[i] = 0;
    s_pending.push_back(op);
}

// UPDATE 2026-06-20 (round 41 audit): same unguarded peer-controlled FormID
// -> LookupFormByID(+DYNAMIC_CAST) crash pattern already found and fixed in
// CoSyncDoorHook/CoSyncLockHook/CoSyncQuestSync this session. SEH-wrapped,
// same noinline convention (load-bearing under /GL+/LTCG).
__declspec(noinline) static TESForm* SafeLookupForm(uint32_t formID)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::SafeLookupForm", "Looks up a game object by ID. If the other player sends a bad ID, it fails safely instead of crashing.");
    __try
    {
        return LookupFormByID(formID);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

__declspec(noinline) static TESObjectREFR* SafeLookupAndCastRefr(uint32_t formID)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::SafeLookupAndCastRefr", "Looks up a game object by ID and checks its type. If the other player sends a bad ID, it fails safely instead of crashing.");
    __try
    {
        TESForm* form = LookupFormByID(formID);
        // 2026-07-04: DYNAMIC_CAST removed — the identical call site in
        // CoSyncPlayerManager.cpp PumpDeferredSpawns_Impl was minidump-confirmed
        // to die inside broken RTTI (__std_type_info_name → strlen on the
        // formID), and an engine-side SEH frame terminates the process before
        // our __except can run. formType check by engine contract instead:
        // REFR (64) and ACHR (65) are the only TESObjectREFR types.
        if (form && (form->formType == kFormType_REFR || form->formType == kFormType_ACHR))
            return static_cast<TESObjectREFR*>(form);
        return nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// Scan the local player's cell for a FURN ref with the given base form ID
// AND near the given world position — used to find THIS client's own local
// representation of the real-world chassis the sender entered/exited (see
// NotifyPowerArmor's header comment for why formID alone isn't enough: a
// dynamic/workshop/console-spawned frame's formID is never directly
// resolvable cross-machine, and even when it is, base-form-only matching
// could pick the wrong frame out of several identical ones in the same
// room). Position match is deliberately generous (any placed PA frame is
// stationary, so a real match should be within a few units; 256 comfortably
// covers float/quantization slop without being loose enough to grab an
// unrelated frame elsewhere in a large cell). `excludeFormID` skips a
// specific ref (the fake clone this same enter cycle may have just spawned
// for the proxy attach, so this scan can never mistake it for the real
// frame). SEH-wrapped so a bad cell pointer doesn't crash; split into its
// own noinline function to satisfy C2712 (can't mix __try with VMArray/
// VMVariable non-trivial dtors).
__declspec(noinline) static TESObjectREFR* FindLocalChassisInCell(
    uint32_t chassisBaseFormID, const NiPoint3& pos, uint32_t excludeFormID)
{
    __try
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc) return nullptr;
        TESObjectREFR* pcRefr = reinterpret_cast<TESObjectREFR*>(pc);
        TESObjectCELL* cell = pcRefr->parentCell;
        if (!cell) return nullptr;
        constexpr float kMatchRadius = 256.0f;
        for (UInt32 i = 0; i < cell->objectList.count; ++i)
        {
            TESObjectREFR* ref = cell->objectList.entries[i];
            if (!ref || !ref->baseForm) continue;
            if (ref->formID == excludeFormID) continue;
            if (ref->flags & TESObjectREFR::kFlag_IsDeleted) continue;
            if (ref->baseForm->formID != chassisBaseFormID) continue;
            if (ref->baseForm->formType != 42) continue; // FURN only
            const float dx = ref->pos.x - pos.x;
            const float dy = ref->pos.y - pos.y;
            const float dz = ref->pos.z - pos.z;
            if ((dx * dx + dy * dy + dz * dz) > (kMatchRadius * kMatchRadius)) continue;
            return ref;
        }
        return nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// 0.3.5: which LOCAL frame is this machine's copy of a sender's frame.
// A frame the sender placed or spawned has a runtime (0xFF) formID that means
// nothing here, and the position search above only sees the local player's
// own cell -- the client log of 2026-09-29 shows every PAI/PAMODS for the
// host's frames failing to resolve (74x "PAI: no local frame", 13x PAMODS -1),
// so station work (paint, armor mods) never reached anyone. The copy is
// learned when the sender enters PA (the local frame found then) or, when this
// machine had none, is the chassis spawned for the proxy, which is kept after
// the exit instead of deleted. Keyed by (sender entity, sender's frame ref).
// Game thread only.
namespace
{
    std::unordered_map<uint64_t, uint32_t> s_frameCopyOf;

    uint64_t FrameKey(uint32_t senderEid, uint32_t senderRef)
    {
        return (static_cast<uint64_t>(senderEid) << 32) | senderRef;
    }
}

static bool SafeIsRefPowerArmorFrame(TESObjectREFR* ref);

static void RememberFrameCopy(uint32_t senderEid, uint32_t senderRef, uint32_t localRef)
{
    if (senderEid == 0 || senderRef == 0 || localRef == 0)
        return;
    uint32_t& slot = s_frameCopyOf[FrameKey(senderEid, senderRef)];
    if (slot != localRef)
        LOG_UPDATED("[PASync] frame 0x%08X of entity=%u is frame 0x%08X on this machine", senderRef, senderEid, localRef);
    slot = localRef;
}

static TESObjectREFR* LookupFrameCopy(uint32_t senderEid, uint32_t senderRef)
{
    auto it = s_frameCopyOf.find(FrameKey(senderEid, senderRef));
    if (it == s_frameCopyOf.end())
        return nullptr;
    TESObjectREFR* ref = SafeLookupAndCastRefr(it->second);
    if (!ref || (ref->flags & TESObjectREFR::kFlag_IsDeleted) || !SafeIsRefPowerArmorFrame(ref))
    {
        s_frameCopyOf.erase(it);   // gone (deleted / freed): forget it
        return nullptr;
    }
    return ref;
}

static double PaDiagNowSeconds()
{
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return static_cast<double>(cnt.QuadPart) / static_cast<double>(freq.QuadPart);
}

// Round <PA piece diagnostic, 2026-08-01>: read-only, SEH-safe dump of a
// ref's raw inventory contents. Exists to answer, with real data instead of
// another guess, the open question behind "PA pieces don't show on the
// receiving proxy": does AddItem via CallFunctionNoWait actually land on a
// freshly CreateReferenceAtLocation-spawned FURN chassis at all (round 145
// already found Papyrus AddItem silently fails on DLL-created ACTOR refs;
// unconfirmed whether the same applies to a bare FURN chassis, which may
// not even have inventoryList allocated yet), and separately, whether
// SwitchToPowerArmor's native internals actually pull pieces from the
// chassis onto the wearer's own equipped state the way this code has always
// assumed (from IDA reading alone, never confirmed live). Never changes any
// game state, purely observational.
__declspec(noinline) static void LogInventoryDiag(TESObjectREFR* refr, const char* label)
{
    if (!refr)
    {
        LOG_WARN("[PASync DIAG] %s: null ref", label);
        return;
    }
    __try
    {
        BGSInventoryList* inv = refr->inventoryList;
        if (!inv)
        {
            LOG_WARN("[PASync DIAG] %s: ref=0x%08X inventoryList=NULL (never allocated)",
                label, refr->formID);
            return;
        }
        LOG_WARN("[PASync DIAG] %s: ref=0x%08X inventoryList=%p count=%u capacity=%u",
            label, refr->formID, static_cast<void*>(inv), inv->items.count, inv->items.capacity);
        for (UInt32 i = 0; i < inv->items.count; ++i)
        {
            const BGSInventoryItem& item = inv->items[i];
            if (!item.form) continue;
            bool equipped = false;
            if (item.stack)
            {
                item.stack->Visit([&equipped](BGSInventoryItem::Stack* s)
                {
                    if (s->flags & BGSInventoryItem::Stack::kFlagEquipped) { equipped = true; return false; }
                    return true;
                });
            }
            LOG_WARN("[PASync DIAG]   [%u] form=0x%08X type=%u equipped=%d",
                i, item.form->formID, (unsigned)item.form->formType, equipped ? 1 : 0);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[PASync DIAG] %s: AV scanning ref=0x%08X inventory", label, refr->formID);
    }
}

namespace
{
    // Deferred re-check, scheduled ~1s after PA enter so any async
    // CallFunctionNoWait("AddItem") has had time to actually run through
    // the Papyrus VM queue before we look. Re-dumps both the chassis (did
    // AddItem land at all?) and the wearer actor (did SwitchToPowerArmor's
    // internals pull pieces onto it?).
    struct PaDiagCheck
    {
        uint32_t chassisRefFormID = 0;
        uint32_t actorFormID      = 0;
        double   checkAt          = 0.0;
    };
    std::vector<PaDiagCheck> s_pendingDiagChecks;

    // Round <PA actor-3D refresh, 2026-08-02>: 2026-08-01 testing proved the
    // piece-equip data path is NOT the remaining bug -- the proxy actor can
    // legitimately have equipped=1 on every PA armor form (confirmed via
    // LogInventoryDiag) and the pieces still never render on the chassis.
    // The chassis ref already gets a native Disable+Enable appearance
    // rebuild right after spawn (see EnableWorldActor call above); the
    // ACTOR never has, even though a real player's own 3D also gets
    // reprocessed by the engine when SwitchToPowerArmor runs on a live,
    // natively-driven character. Theory: our AI-disabled proxy actor needs
    // the same forced appearance-rebuild treatment SwitchToPowerArmor
    // normally gets "for free" on a real player, to re-resolve its
    // now-frame-relative biped attachment. Scheduled well after the enter
    // (not immediately) so SwitchToPowerArmor's own VM-queued call has
    // actually landed first -- refreshing the actor's 3D before that
    // finishes would just get overwritten by it.
    struct PaActorRefreshCheck
    {
        uint32_t actorFormID = 0;
        double   refreshAt   = 0.0;
    };
    std::vector<PaActorRefreshCheck> s_pendingActorRefresh;

    // 0.3.4: after a proxy's PA exit, confirm the ENGINE rebuilt its body
    // (see FinishPaExit) instead of forcing our own Disable+Enable.
    struct PaPostExitCheck
    {
        uint32_t entityID = 0;
        double   checkAt  = 0.0;
    };
    std::vector<PaPostExitCheck> s_pendingPostExit;

    // AI process level: Actor+0x300 = process, process+0xDF (0 high,
    // 1 middle-high, 2 middle-low, 3 low). Same byte sub_140CA6AA0 returns
    // and Actor::Update checks (1.11.240, IDA). -1 = no process/unreadable.
    __declspec(noinline) int SafeProcessLevel(Actor* actor)
    {
        if (!actor) return -1;
        __try
        {
            const uint8_t* process = *reinterpret_cast<uint8_t* const*>(
                reinterpret_cast<const uint8_t*>(actor) + 0x300);
            return process ? static_cast<int>(process[0xDF]) : -1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    // 2026-08-26: live-tested proof the native eject sometimes silently
    // refuses -- ExtraPowerArmor read back 1 (still in PA) immediately after
    // a "called=1" (no fault) SafeSwitchFromPowerArmorNative call, right
    // after a rapid sequence of piece-swap re-broadcasts. Theory: the
    // engine's own PA state machine still considers a piece transition in
    // flight and refuses to eject until it settles. The OLD code proceeded
    // to delete the spawned chassis and reshow the real frame regardless of
    // whether the eject actually took -- tearing down/repositioning objects
    // out from under an actor the engine still considers physically merged
    // into the rig, which is a far more direct explanation for "floating
    // chassis" and "a second chassis appears" (the reshow firing on top of
    // a still-attached, never-actually-ejected original) than anything
    // about the reshow/delete logic itself. Now: cleanup is gated on
    // CONFIRMING the eject worked, retried for a few seconds if not.
    struct PaExitRetry
    {
        uint32_t entityID     = 0;
        int      attemptsLeft = 10;
        double   nextAttemptAt = 0.0;
        // 0.3.4: when the native eject was last called. The engine's exit is
        // a sequence (animation, then the marker/race change), so the retry
        // loop now WAITS for it and only re-issues the eject after
        // kPaEjectReissueSec with no progress, instead of every second.
        double   lastEjectAt  = 0.0;
    };
    constexpr double kPaEjectReissueSec = 5.0;
    std::vector<PaExitRetry> s_pendingExitRetries;
}

// Runs the cleanup that used to fire unconditionally right after the eject
// call: delete the spawned proxy-attach chassis, reshow+reposition this
// client's local copy of the real-world frame, reset PA state, force a full
// 3D rebuild, and re-queue the weapon. ONLY call this once the eject is
// CONFIRMED to have actually taken (ExtraPowerArmor reads 0) -- see
// PaExitRetry's comment for why. Identical to the previously-inline logic,
// just made callable from both the immediate first-attempt path and the
// deferred-retry path below without duplicating it.
static void FinishPaExit(CoSyncPlayer* proxy, uint32_t senderEntityID)
{
    static const BSFixedString kDelete("Delete");

    // 0.3.5: no local copy of the sender's frame here -> the chassis the proxy
    // just left IS that frame from now on. The engine's exit already moved the
    // pieces back into it and placed it at the exit point (sub_14098D8C0), so
    // it stands where the sender's frame stands. Deleting it made the frame
    // vanish on this machine, and every later PAI/PAMODS (station work) for it
    // had nothing to land on.
    if (proxy->m_paSpawnedChassisRefFormID != 0 && proxy->m_paRealFrameRefFormID == 0 &&
        proxy->m_paChassisRefFormID != 0)
    {
        if (TESObjectREFR* spawned = SafeLookupAndCastRefr(proxy->m_paSpawnedChassisRefFormID))
        {
            RememberFrameCopy(senderEntityID, proxy->m_paChassisRefFormID, spawned->formID);
            LOG_UPDATED("[PASync] PA Exit Completed: kept chassis 0x%08X as this machine's copy of frame 0x%08X (entity=%u)",
                spawned->formID, proxy->m_paChassisRefFormID, senderEntityID);
            proxy->m_paSpawnedChassisRefFormID = 0;
            proxy->m_paChassisIsRealFrame = false;
        }
    }

    if (proxy->m_paSpawnedChassisRefFormID != 0)
    {
        TESObjectREFR* spawned = SafeLookupAndCastRefr(proxy->m_paSpawnedChassisRefFormID);
        if (spawned)
        {
            CoSyncGameAPI::DisableWorldActor(spawned);
            VMArray<VMVariable> delArgs;
            CallFunctionNoWait(spawned, kDelete, delArgs);
            LOG_UPDATED("[PASync] Exit PA: disabled + queued delete for spawned chassis=0x%08X entity=%u",
                proxy->m_paSpawnedChassisRefFormID, senderEntityID);
        }
        else
        {
            LOG_UPDATED_WARN("[PASync] Exit PA: spawned chassis=0x%08X not resolvable at exit (already freed?) entity=%u",
                proxy->m_paSpawnedChassisRefFormID, senderEntityID);
        }
        proxy->m_paSpawnedChassisRefFormID = 0;
    }

    if (proxy->m_paRealFrameRefFormID != 0)
    {
        TESObjectREFR* realFrame = SafeLookupAndCastRefr(proxy->m_paRealFrameRefFormID);
        if (realFrame && proxy->actorRef)
        {
            PlayerCharacter* pc = CoSyncGetPlayer();
            TESObjectREFR* pcRefr = pc ? reinterpret_cast<TESObjectREFR*>(pc) : nullptr;
            if (pcRefr)
            {
                TESWorldSpace* ws   = CALL_MEMBER_FN(pcRefr, GetWorldspace)();
                TESObjectCELL* cell = ws ? nullptr : pcRefr->parentCell;
                NiPoint3 exitPos{ proxy->actorRef->pos.x, proxy->actorRef->pos.y, proxy->actorRef->pos.z };
                NiPoint3 exitRot{ realFrame->rot.x, realFrame->rot.y, realFrame->rot.z };
                UInt32 nh = 0;
                MoveRefrToPosition(realFrame, &nh, cell, ws, &exitPos, &exitRot);
            }
            CoSyncGameAPI::EnableWorldActor(realFrame);
            LOG_UPDATED("[PASync] Exit PA: reshowed local real-frame ref=0x%08X at exit location entity=%u",
                proxy->m_paRealFrameRefFormID, senderEntityID);
        }
        else
        {
            LOG_UPDATED_WARN("[PASync] Exit PA: real-frame ref=0x%08X not resolvable at exit entity=%u",
                proxy->m_paRealFrameRefFormID, senderEntityID);
        }
        proxy->m_paRealFrameRefFormID = 0;
    }

    proxy->m_isInPowerArmor     = false;
    proxy->m_paChassisRefFormID = 0;
    proxy->m_paBehaviorEntered  = false;
    memset(proxy->m_paAppliedPieces, 0, sizeof(proxy->m_paAppliedPieces));

    // 0.3.4: NO Disable+Enable here any more. The engine rebuilds the body
    // itself (IDA 1.11.240): the native exit SwitchFromPowerArmor
    // (sub_14098BE40) sets the PA extra (type 0xBB) state to 3; the exit then
    // finishes in sub_14098D8C0 (state 5), which moves the pieces back to the
    // frame and -- for an actor in high or middle-high process
    // (sub_140CA6AA0 < 2) -- requests update3DModel 0x520 (0x20 = full 3D
    // reload), consumed by Actor::Update -> sub_140D02600 -> sub_140D27280.
    // Only after that does sub_14098B8C0 (state 6) remove the 0xBB extra,
    // which is exactly what TryPaExit waits for. The enter path
    // (sub_140989D90) makes the same request. So by the time we get here the
    // engine's rebuild is already queued; our RefreshActor3D was a second
    // rebuild that also dropped the weapon. A low-process actor finishes the
    // exit immediately and has no 3D to rebuild. The post-exit check below
    // confirms the result and falls back only if the engine visibly did not.
    const int level = SafeProcessLevel(proxy->actorRef);

    // The weapon and outfit are re-checked against the engine after the
    // engine's rebuild (both are idempotent: equip only what is missing).
    if (proxy->m_lastQueuedWeaponFormID != 0)
    {
        proxy->m_postRefreshWeaponFormID = proxy->m_lastQueuedWeaponFormID;
        proxy->m_postRefreshWeaponDelay  = 60;
        proxy->m_weaponConfirmedEquipped = false;
    }
    CoSyncOutfitSync::ClearAppliedOutfits(senderEntityID);

    s_pendingPostExit.push_back(PaPostExitCheck{ senderEntityID, PaDiagNowSeconds() + 3.0 });

    LOG_UPDATED("[PASync] Exit PA: engine exit finished entity=%u (process level %d; %s) -- "
                "weapon=0x%08X re-queued, body check in 3 s",
        senderEntityID, level,
        (level == 0 || level == 1) ? "engine full 3D reload queued" : "no 3D at this process level",
        proxy->m_postRefreshWeaponFormID);
}

// Attempts the native eject and checks whether it actually took. On success,
// runs FinishPaExit immediately. On failure, schedules a retry instead of
// proceeding with cleanup -- see PaExitRetry's comment for the live-tested
// reason this matters (tearing down a chassis the actor is still physically
// merged into is worse than leaving the old state alone a little longer).
static void TryPaExit(CoSyncPlayer* proxy, uint32_t senderEntityID)
{
    // 0.3.4: an exit for a proxy that is not in Power Armor is a no-op. The
    // sender can report one exit twice (a fallback and the real Papyrus
    // event), and each used to end in FinishPaExit's full 3D rebuild of a
    // proxy that was already out (0.3.3 host log 23:52:36.682:
    // "ExtraPowerArmor 0 -> 0" followed by a rebuild).
    if (!proxy->m_isInPowerArmor && proxy->m_paSpawnedChassisRefFormID == 0 &&
        CoSyncGameAPI::GetActorPowerArmorExtraState(proxy->actorRef) != 1)
    {
        LOG_UPDATED("[PASync] Exit PA entity=%u: proxy is already out of Power Armor -- nothing to do",
            senderEntityID);
        return;
    }

    LOG_UPDATED("[PASync] PA Exit Started: proxy entity=%u, engine exit (SwitchFromPowerArmor) on chassis 0x%08X",
        senderEntityID, proxy->m_paSpawnedChassisRefFormID);
    const int paBefore = CoSyncGameAPI::GetActorPowerArmorExtraState(proxy->actorRef);
    const bool ejected = SafeSwitchFromPowerArmorNative(proxy->actorRef);
    const int paAfter  = CoSyncGameAPI::GetActorPowerArmorExtraState(proxy->actorRef);

    LOG_UPDATED("[PASync] Exit PA entity=%u: native eject called=%d, "
                "ExtraPowerArmor %d -> %d (1=in PA, 0=out, -1=unreadable)",
        senderEntityID, ejected ? 1 : 0, paBefore, paAfter);

    if (paAfter != 1)
    {
        // 0 (confirmed out) or -1 (unreadable, e.g. actor gone) -- either way
        // there's nothing left to gate on, proceed with cleanup now.
        FinishPaExit(proxy, senderEntityID);
        return;
    }

    LOG_WARN("[PASync] Exit PA entity=%u: actor still in power armor after the native eject "
             "-- deferring cleanup instead of tearing down a still-merged chassis, will retry",
        senderEntityID);

    for (PaExitRetry& r : s_pendingExitRetries)
    {
        if (r.entityID == senderEntityID)
        {
            // A newer exit for the same entity superseded an in-flight retry
            // (e.g. re-entered and exited again before the old one settled) --
            // restart the attempt budget rather than stacking two chains.
            r.attemptsLeft  = 10;
            r.nextAttemptAt = PaDiagNowSeconds() + 1.0;
            r.lastEjectAt   = PaDiagNowSeconds();
            return;
        }
    }
    s_pendingExitRetries.push_back(PaExitRetry{ senderEntityID, 10, PaDiagNowSeconds() + 1.0, PaDiagNowSeconds() });
}

// ═══════════════════════════════════════════════════════════════════════════
// 2026-09-05: (1) C++ enter/exit FALLBACK detection and (2) unworn-frame
// inventory sync ("PAI|"). Ported from the feature branch.
//
// (1) Enter/exit normally arrives through Papyrus OnSit/OnGetUp ->
//     CoSync_NotifyPowerArmor. When that event does not fire (script latency,
//     a remote-event registration that never landed, OnSit swallowed by
//     another furniture use), the local player is in a frame but nothing was
//     broadcast. The engine swaps the wearer's race to PowerArmorRace while
//     in a frame, so Actor::race is a script-independent signal. This tick
//     compares it against the Papyrus-reported state (IsActorKnownInPowerArmor)
//     and, after a grace period, reports the transition itself.
//
// (2) Pieces swapped while a frame is unworn (PA station, transfer menu) were
//     never synced: the next wearer's enter packet carried whatever pieces
//     the SENDER's frame had, so the receiver's frame stayed stale until then
//     and could disagree. Both roles poll frames within 3000 units once a
//     second and broadcast the ARMO/AMMO contents when they change (host also
//     publishes the initial snapshot); receivers apply a diff to their local
//     copy of that frame, with a 3 s echo-suppression window per frame.
// ═══════════════════════════════════════════════════════════════════════════
namespace
{
    // Fallout4.esm, verified against the ESM EDID table 2026-09-05.
    constexpr uint32_t kRace_PowerArmor              = 0x0001D31Eu;   // PowerArmorRace
    constexpr uint32_t kKywd_FurnitureTypePowerArmor = 0x0003430Bu;   // FurnitureTypePowerArmor
    constexpr uint32_t kKywd_IsPowerArmorFrame       = 0x0015503Fu;   // isPowerArmorFrame

    constexpr double kFallbackPollInterval    = 0.25;
    constexpr double kFallbackGraceSec        = 1.5;   // let Papyrus OnSit/OnGetUp report first
    constexpr double kFallbackChassisGiveUp   = 6.0;
    constexpr float  kFallbackFrameRadius     = 300.f;

    constexpr double kFrameInvPollInterval    = 1.0;
    constexpr double kFrameInvEchoSuppressSec = 3.0;
    constexpr float  kFrameInvScanRadius      = 3000.f;

    struct PaInvItem
    {
        uint32_t formID = 0;
        int32_t  count  = 0;
        float    health = -1.f;   // 0.3.3: ExtraHealth of this stack, -1 = full / none
    };

    struct PaInvOp
    {
        uint32_t senderEntityID = 0;
        uint32_t refFormID      = 0;
        uint32_t baseFormID     = 0;
        NiPoint3 pos{ 0.f, 0.f, 0.f };
        std::vector<PaInvItem> items;
    };

    std::mutex            s_invMutex;
    std::vector<PaInvOp>  s_pendingInv;

    double   s_fbLastPoll        = 0.0;
    double   s_fbPendingSince    = 0.0;
    bool     s_fbLoggedNoChassis = false;
    // 0.3.4: the race has been seen as PowerArmorRace during the current
    // known-in-PA session. Until then the player is still ENTERING: the
    // enter notification fires when they sit in the frame, the race swaps
    // only when the enter animation completes.
    bool     s_fbRaceConfirmed   = false;

    double   s_lastFrameInvPoll  = 0.0;
    std::unordered_map<uint32_t, std::vector<PaInvItem>> s_frameLastSig;   // local ref -> last seen/applied

    std::unordered_map<uint32_t, std::string> s_frameLastMods;   // frame ref -> last piece-mod text (PAMODS)
    std::unordered_map<uint32_t, double>                 s_frameSuppressUntil;

    inline bool IsDynamicFormID(uint32_t fid) { return (fid & 0xFF000000u) == 0xFF000000u; }
}

// ── SEH leaf helpers (POD-only bodies, no C2712) ────────────────────────────

// 0.3.3: item condition. IDA 1.11.240: sub_1402800C0(ExtraDataList*) =
// GetByType(0x25 ExtraHealth) -> float at +0x18, or -1.0 when absent (= full);
// the vanilla GetItemHealthPercent native (sub_1411814A0 -> sub_140561720)
// ends in it, and the engine's own Power Armor code (SwitchToPowerArmor
// sub_140989D90, sub_14098C920, sub_14098DD40 ...) calls it. PA piece
// condition and fusion-core charge are both this value.
__declspec(noinline) static float SafeStackHealth(BGSInventoryItem::Stack* st)
{
    __try
    {
        if (!st || !st->extraData) return -1.f;
        ExtraHealth* h = static_cast<ExtraHealth*>(st->extraData->GetByType(kExtraData_Health));
        return h ? h->health : -1.f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1.f; }
}

// Health of the equipped stack of `fid` on an actor (-1 = full / none).
__declspec(noinline) static float SafeEquippedItemHealth(Actor* actor, uint32_t fid)
{
    __try
    {
        if (!actor || !actor->inventoryList || !fid) return -1.f;
        BGSInventoryList* inv = actor->inventoryList;
        for (UInt32 i = 0; i < inv->items.count; ++i)
        {
            const BGSInventoryItem& item = inv->items[i];
            if (!item.form || item.form->formID != fid) continue;
            for (BGSInventoryItem::Stack* st = item.stack; st; st = st->next)
                if (st->flags & BGSInventoryItem::Stack::kFlagEquipped)
                    return SafeStackHealth(st);
        }
        return -1.f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1.f; }
}

// Set (or add) ExtraHealth on one stack. health < 0 means "full": the value
// is set to 1.0 rather than removing the extra (removal would need the
// engine's extra-data free path).
__declspec(noinline) static bool SafeSetStackHealth(BGSInventoryItem::Stack* st, float health)
{
    __try
    {
        if (!st || !st->extraData) return false;
        const float v = (health < 0.f) ? 1.f : health;
        ExtraHealth* h = static_cast<ExtraHealth*>(st->extraData->GetByType(kExtraData_Health));
        if (h) { h->health = v; return true; }
        ExtraHealth* nh = ExtraHealth::Create(v);
        return nh && st->extraData->Add(kExtraData_Health, nh);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// All stacks of `form` in a ref's inventory (SEH leaf, fixed-size output).
__declspec(noinline) static int SafeCollectStacks(TESObjectREFR* ref, TESForm* form,
                                                  BGSInventoryItem::Stack** out, int cap)
{
    int n = 0;
    __try
    {
        BGSInventoryList* inv = ref ? ref->inventoryList : nullptr;
        for (UInt32 i = 0; inv && i < inv->items.count; ++i)
            if (inv->items[i].form == form)
                for (BGSInventoryItem::Stack* st = inv->items[i].stack; st && n < cap; st = st->next)
                    out[n++] = st;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    return n;
}


__declspec(noinline) static bool SafeKeywordFormHas(const BGSKeywordForm* kf, uint32_t kwFormID)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::SafeKeywordFormHas", "kf may be any pointer; walks kf->keywords[0..numKeywords) under SEH and compares formIDs, never calls the HasKeyword virtual");
    __try
    {
        if (!kf || !kf->keywords || kf->numKeywords == 0 || kf->numKeywords > 512)
            return false;
        for (UInt32 i = 0; i < kf->numKeywords; ++i)
        {
            const BGSKeyword* kw = kf->keywords[i];
            if (kw && kw->formID == kwFormID)
                return true;
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static bool SafeIsPowerArmorFrameBase(TESForm* base)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::SafeIsPowerArmorFrameBase", "base may be null; TESFurniture derives from TESObjectACTI whose keywordForm sits at 0xD0 (F4SE layout)");
    __try
    {
        if (!base || base->formType != kFormType_FURN)
            return false;
        const TESObjectACTI* acti = static_cast<const TESObjectACTI*>(base);
        return SafeKeywordFormHas(&acti->keywordForm, kKywd_FurnitureTypePowerArmor)
            || SafeKeywordFormHas(&acti->keywordForm, kKywd_IsPowerArmorFrame);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static bool SafeIsRefPowerArmorFrame(TESObjectREFR* ref)
{
    __try
    {
        if (!ref) return false;
        if (ref->flags & TESObjectREFR::kFlag_IsDeleted) return false;
        return SafeIsPowerArmorFrameBase(ref->baseForm);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static bool SafeActorInPowerArmorByRace(Actor* actor)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::SafeActorInPowerArmorByRace", "actor may be null; reads Actor::race (0x418) -- the engine swaps an actor's race to PowerArmorRace while it occupies a frame");
    __try
    {
        if (!actor) return false;
        const TESRace* race = actor->race;
        return race && race->formID == kRace_PowerArmor;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static uint32_t SafeGetFurnitureHandle(Actor* actor)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::SafeGetFurnitureHandle", "actor may be null; walks actor->middleProcess->unk08->furnitureHandle1 (F4SE Data08 offset 0x3A4) under SEH");
    __try
    {
        if (!actor || !actor->middleProcess || !actor->middleProcess->unk08)
            return 0;
        return actor->middleProcess->unk08->furnitureHandle1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

__declspec(noinline) static TESObjectREFR* ResolveHandle_Impl(uint32_t handle)
{
    NiPointer<TESObjectREFR> refPtr;
    if (LookupREFRByHandle(handle, refPtr) && refPtr.m_pObject)
        return refPtr.m_pObject;
    return nullptr;
}

__declspec(noinline) static TESObjectREFR* SafeResolveHandle(uint32_t handle)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::SafeResolveHandle", "handle may be any value; LookupREFRByHandle runs inside ResolveHandle_Impl (kept SEH-free because NiPointer has a destructor) and this wrapper catches faults");
    __try
    {
        if (handle == 0 || handle == 0xFFFFFFFFu) return nullptr;
        return ResolveHandle_Impl(handle);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

__declspec(noinline) static TESObjectREFR* FindNearestPowerArmorFrameByKeyword(
    const NiPoint3& pos, float radius, uint32_t excludeRefFormID)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::FindNearestPowerArmorFrameByKeyword", "requires CoSyncGetPlayer() with a valid parentCell; scans cell->objectList under SEH for enabled FURN refs whose base carries a power-armor frame keyword (any base form, unlike FindLocalChassisInCell)");
    __try
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc) return nullptr;
        TESObjectCELL* cell = reinterpret_cast<TESObjectREFR*>(pc)->parentCell;
        if (!cell) return nullptr;

        TESObjectREFR* best = nullptr;
        float bestSq = radius * radius;
        for (UInt32 i = 0; i < cell->objectList.count; ++i)
        {
            TESObjectREFR* ref = cell->objectList.entries[i];
            if (!ref || !ref->baseForm) continue;
            if (ref->formID == excludeRefFormID) continue;
            if (ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)) continue;
            if (!SafeIsPowerArmorFrameBase(ref->baseForm)) continue;
            const float dx = ref->pos.x - pos.x;
            const float dy = ref->pos.y - pos.y;
            const float dz = ref->pos.z - pos.z;
            const float dSq = dx * dx + dy * dy + dz * dz;
            if (dSq < bestSq) { bestSq = dSq; best = ref; }
        }
        return best;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

__declspec(noinline) static int GatherChassisArmorPieces(TESObjectREFR* chassis, uint32_t* out, int cap)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::GatherChassisArmorPieces", "chassis may be null; walks chassis->inventoryList under SEH collecting ARMO formIDs");
    int count = 0;
    __try
    {
        if (!chassis || !chassis->inventoryList) return 0;
        BGSInventoryList* inv = chassis->inventoryList;
        for (UInt32 i = 0; i < inv->items.count && count < cap; ++i)
        {
            const BGSInventoryItem& item = inv->items[i];
            if (!item.form || item.form->formType != kFormType_ARMO) continue;
            out[count++] = item.form->formID;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[PASync] GatherChassisArmorPieces: AV walking chassis inventory");
    }
    return count;
}

// Frame inventory signature: ARMO + AMMO items with total counts.
__declspec(noinline) static void GatherFrameSignature(TESObjectREFR* frame, std::vector<PaInvItem>& out)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::GatherFrameSignature", "frame may be null; walks frame->inventoryList and Stack chains under SEH; out is a caller-owned reference so this frame has no unwindable locals");
    __try
    {
        if (!frame || !frame->inventoryList) return;
        BGSInventoryList* inv = frame->inventoryList;
        for (UInt32 i = 0; i < inv->items.count; ++i)
        {
            const BGSInventoryItem& item = inv->items[i];
            if (!item.form) continue;
            const UInt8 ft = item.form->formType;
            if (ft != kFormType_ARMO && ft != kFormType_AMMO) continue;
            // 0.3.3: one entry per stack, with its condition (fusion-core
            // charge, piece damage/repair), instead of one total per form.
            BGSInventoryItem::Stack* s = item.stack;
            int guard = 0;
            while (s && guard++ < 64)
            {
                if (s->count > 0)
                {
                    PaInvItem it; it.formID = item.form->formID; it.count = s->count;
                    it.health = SafeStackHealth(s);
                    out.push_back(it);
                }
                s = s->next;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[PASync] GatherFrameSignature: AV walking frame inventory");
    }
}

// Health rounded to 1% for comparing and merging (float noise must not look
// like a change).
static int HealthKey(float h) { return h < 0.f ? -1 : static_cast<int>(h * 100.f + 0.5f); }

static void NormalizeSignature(std::vector<PaInvItem>& sig)
{
    std::sort(sig.begin(), sig.end(), [](const PaInvItem& a, const PaInvItem& b)
    {
        if (a.formID != b.formID) return a.formID < b.formID;
        return HealthKey(a.health) < HealthKey(b.health);
    });
    std::vector<PaInvItem> merged;
    for (const PaInvItem& it : sig)
    {
        if (!merged.empty() && merged.back().formID == it.formID &&
            HealthKey(merged.back().health) == HealthKey(it.health))
            merged.back().count += it.count;
        else
            merged.push_back(it);
    }
    sig.swap(merged);
}

static bool SignatureEquals(const std::vector<PaInvItem>& a, const std::vector<PaInvItem>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].formID != b[i].formID || a[i].count != b[i].count ||
            HealthKey(a[i].health) != HealthKey(b[i].health)) return false;
    return true;
}

// A frame currently occupied by us or by a proxy carries its pieces on the
// wearer (or is our spawned attach-clone) -- never publish/apply its inventory.
static bool IsFrameOccupiedLocally(uint32_t localRefFormID)
{
    PlayerCharacter* pc = CoSyncGetPlayer();
    if (pc && CoSyncPowerArmorSync::IsActorKnownInPowerArmor(reinterpret_cast<Actor*>(pc)->formID)
        && localRefFormID == s_localChassisRefFormID)
        return true;

    for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
    {
        const CoSyncPlayer* p = kv.second.get();
        if (!p) continue;
        if (p->m_paRealFrameRefFormID == localRefFormID) return true;
        if (p->m_paSpawnedChassisRefFormID == localRefFormID) return true;
    }
    return false;
}

bool CoSyncPowerArmorSync::IsCoSyncManagedFrame(uint32_t localRefFormID)
{
    return IsFrameOccupiedLocally(localRefFormID);
}

// ── (1) Enter/exit fallback ─────────────────────────────────────────────────
static void TickLocalPowerArmorFallback()
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::TickLocalPowerArmorFallback", "Needs an active connection and a valid player (both checked). Only acts when the race-based PA signal disagrees with the Papyrus-reported state for longer than the grace period.");
    if (!CoSyncNet::IsConnected())
        return;
    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc)
        return;
    Actor* me = reinterpret_cast<Actor*>(pc);

    const double now = NowSecondsPA();
    if (now - s_fbLastPoll < kFallbackPollInterval)
        return;
    s_fbLastPoll = now;

    const bool inPA  = SafeActorInPowerArmorByRace(me);
    const bool known = CoSyncPowerArmorSync::IsActorKnownInPowerArmor(me->formID);

    if (!known)
        s_fbRaceConfirmed = false;
    else if (inPA)
        s_fbRaceConfirmed = true;

    if (inPA == known)
    {
        s_fbPendingSince    = 0.0;
        s_fbLoggedNoChassis = false;
        return;
    }

    // 0.3.4 -- ROOT CAUSE of "Power Armor drops off for everyone ~2 s after
    // entering" (0.3.3 client log 23:52:22.048 enter -> 23:52:23.801 "FALLBACK
    // exit detected via PowerArmorRace", while the player was still in the
    // frame; the real exit came at 23:52:36 through Papyrus). The enter
    // notification marks us known-in-PA as soon as we sit in the frame, but
    // the race only becomes PowerArmorRace when the enter animation finishes,
    // so for the whole animation "known but not the race" looked like a missed
    // exit. A missed exit can only be claimed once the race HAS been seen in
    // this session and the engine's own ExtraPowerArmor marker on the player
    // is gone too.
    if (known && !inPA)
    {
        if (!s_fbRaceConfirmed || CoSyncGameAPI::GetActorPowerArmorExtraState(me) == 1)
        {
            s_fbPendingSince = 0.0;
            return;
        }
    }
    if (s_fbPendingSince == 0.0)
    {
        s_fbPendingSince = now;
        return;
    }
    if (now - s_fbPendingSince < kFallbackGraceSec)
        return;

    if (inPA)
    {
        TESObjectREFR* chassis = nullptr;
        {
            TESObjectREFR* furn = SafeResolveHandle(SafeGetFurnitureHandle(me));
            if (furn && SafeIsRefPowerArmorFrame(furn))
                chassis = furn;
        }
        if (!chassis)
        {
            NiPoint3 ppos{ me->pos.x, me->pos.y, me->pos.z };
            chassis = FindNearestPowerArmorFrameByKeyword(ppos, kFallbackFrameRadius, 0);
        }
        if (!chassis || !chassis->baseForm)
        {
            if (now - s_fbPendingSince >= kFallbackChassisGiveUp)
            {
                if (!s_fbLoggedNoChassis)
                {
                    s_fbLoggedNoChassis = true;
                    LOG_WARN("[PASync] FALLBACK: local player reads PowerArmorRace but no frame ref could be resolved within %.0fs -- enter not broadcast",
                        kFallbackChassisGiveUp);
                }
                // Stop re-trying this cycle; the exit side will still resync.
                CoSyncPowerArmorSync::MarkActorPowerArmorState(me->formID, true);
                s_fbPendingSince = 0.0;
            }
            return;
        }

        uint32_t pieces[6] = {};
        if (ScanActorEquippedPAPieces(me, pieces) == 0)
            GatherChassisArmorPieces(chassis, pieces, 6);

        CoSyncPowerArmorSync::MarkActorPowerArmorState(me->formID, true);
        CoSyncPowerArmorSync::NotifyPowerArmor(
            CoSyncNet::GetMyEntityID(),
            chassis->baseForm->formID,
            chassis->formID,
            true,
            pieces,
            chassis->pos);
        LOG_UPDATED("[PASync] FALLBACK enter detected via PowerArmorRace (Papyrus OnSit did not report): chassis ref=0x%08X base=0x%08X",
            chassis->formID, chassis->baseForm->formID);
    }
    else
    {
        CoSyncPowerArmorSync::MarkActorPowerArmorState(me->formID, false);
        if (s_localChassisBaseFormID != 0)
        {
            uint32_t none[6] = {};
            CoSyncPowerArmorSync::NotifyPowerArmor(
                CoSyncNet::GetMyEntityID(),
                s_localChassisBaseFormID,
                s_localChassisRefFormID,
                false,
                none,
                s_localChassisPos);
            s_frameLastSig.erase(s_localChassisRefFormID);
        }
        LOG_UPDATED("[PASync] FALLBACK exit detected via PowerArmorRace (Papyrus OnGetUp did not report): chassis ref=0x%08X",
            s_localChassisRefFormID);
    }
    s_fbPendingSince = 0.0;
}

// ── (2) Frame inventory sync ────────────────────────────────────────────────
static std::string SerializeFrameInventory(uint32_t sender, uint32_t refFormID, uint32_t baseFormID,
                                           const NiPoint3& pos, const std::vector<PaInvItem>& items)
{
    std::ostringstream ss;
    ss << "PAI|" << sender << "|" << refFormID << "|" << baseFormID << "|" << std::fixed;
    ss.precision(2);
    ss << pos.x << "|" << pos.y << "|" << pos.z << "|";
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i) ss << ",";
        ss << items[i].formID << ":" << items[i].count;
        if (items[i].health >= 0.f)
            ss << ":" << items[i].health;   // 0.3.3; absent = full (old peers ignore it)
    }
    return ss.str();
}

static bool ParseFrameInventory(const std::string& msg, PaInvOp& op)
{
    if (msg.rfind("PAI|", 0) != 0) return false;
    const char* s = msg.c_str() + 4;
    char* e = nullptr;
    op.senderEntityID = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return false; s = e + 1;
    op.refFormID      = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return false; s = e + 1;
    op.baseFormID     = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return false; s = e + 1;
    op.pos.x = std::strtof(s, &e); if (!e || *e != '|') return false; s = e + 1;
    op.pos.y = std::strtof(s, &e); if (!e || *e != '|') return false; s = e + 1;
    op.pos.z = std::strtof(s, &e); if (!e || *e != '|') return false; s = e + 1;
    while (*s)
    {
        PaInvItem it{};
        it.formID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
        if (!e || *e != ':') break;
        s = e + 1;
        it.count = static_cast<int32_t>(std::strtol(s, &e, 10));
        if (e && *e == ':')
        {
            s = e + 1;
            const float h = std::strtof(s, &e);
            if (std::isfinite(h) && h >= 0.f && h <= 1.f) it.health = h;
        }
        if (it.formID != 0 && it.count > 0)
            op.items.push_back(it);
        if (!e || *e != ',') break;
        s = e + 1;
    }
    NormalizeSignature(op.items);
    return op.baseFormID != 0;
}

bool CoSyncPowerArmorSync::IsPowerArmorInventory(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::IsPowerArmorInventory", "Just checks the text of a message. No game state needed.");
    return msg.rfind("PAI|", 0) == 0;
}

void CoSyncPowerArmorSync::OnReceivePowerArmorInventory(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::OnReceivePowerArmorInventory", "Network thread; pure parsing plus a self-echo check. The frame is touched later on the game thread.");
    PaInvOp op{};
    if (!ParseFrameInventory(msg, op))
    {
        LOG_UPDATED_WARN("[PASync] Malformed PAI packet: %s", msg.c_str());
        return;
    }
    if (op.senderEntityID != 0 && op.senderEntityID == CoSyncNet::GetMyEntityID())
        return;

    LOG_UPDATED("[PASync] RX PAI sender=%u ref=0x%08X base=0x%08X items=%u",
        op.senderEntityID, op.refFormID, op.baseFormID, (unsigned)op.items.size());

    std::lock_guard<std::mutex> lk(s_invMutex);
    s_pendingInv.push_back(std::move(op));
}

__declspec(noinline) static void TickFrameInventoryScan_Impl(double now)
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::TickFrameInventoryScan_Impl", "Requires a valid player with parentCell; iterates cell->objectList and frame inventories through SEH leaf helpers; caller wraps in SEH.");
    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc) return;
    TESObjectCELL* cell = reinterpret_cast<TESObjectREFR*>(pc)->parentCell;
    if (!cell) return;

    const NiPoint3 ppos{ pc->pos.x, pc->pos.y, pc->pos.z };
    const uint32_t myEid = CoSyncNet::GetMyEntityID();
    const bool isHost = CoSyncNet::IsHost();

    std::vector<PaInvItem> sig;
    for (UInt32 i = 0; i < cell->objectList.count; ++i)
    {
        TESObjectREFR* ref = cell->objectList.entries[i];
        if (!ref || !ref->baseForm) continue;
        if (ref->flags & (TESObjectREFR::kFlag_IsDeleted | TESObjectREFR::kFlag_IsDisabled)) continue;
        if (!SafeIsPowerArmorFrameBase(ref->baseForm)) continue;

        const float dx = ref->pos.x - ppos.x, dy = ref->pos.y - ppos.y, dz = ref->pos.z - ppos.z;
        if (dx * dx + dy * dy + dz * dz > kFrameInvScanRadius * kFrameInvScanRadius) continue;

        const uint32_t rid = ref->formID;
        if (IsFrameOccupiedLocally(rid)) continue;

        // Phase 8: the pieces' mods (paint, jetpack, linings...) -- separate
        // PAMODS packet with its own change detection (PAI only compares
        // item counts, so a repaint at a station never showed up).
        {
            const std::string mods = CoSyncItemMods::DescribeMods(ref, false);
            auto mit = s_frameLastMods.find(rid);
            const bool first   = (mit == s_frameLastMods.end());
            const bool changed = first ? isHost : (mit->second != mods);
            s_frameLastMods[rid] = mods;
            auto msup = s_frameSuppressUntil.find(rid);
            const bool suppressed = msup != s_frameSuppressUntil.end() && now < msup->second;
            if (changed && !suppressed && !mods.empty())
            {
                char hdr[160];
                std::snprintf(hdr, sizeof(hdr), "PAMODS|%u|%u|%u|%.2f|%.2f|%.2f|", myEid, rid,
                    ref->baseForm->formID, ref->pos.x, ref->pos.y, ref->pos.z);
                CoSyncTransport::Send(std::string(hdr) + mods);
                LOG_UPDATED("[PASync] TX PAMODS ref=0x%08X mods=%s", rid, mods.c_str());
            }
        }

        sig.clear();
        GatherFrameSignature(ref, sig);
        NormalizeSignature(sig);

        auto it = s_frameLastSig.find(rid);
        if (it == s_frameLastSig.end())
        {
            s_frameLastSig[rid] = sig;
            // Host publishes the authoritative initial snapshot; clients only
            // publish changes so both sides do not fight on first sight.
            if (!isHost) continue;
        }
        else
        {
            if (SignatureEquals(it->second, sig)) continue;
            it->second = sig;
        }

        auto sup = s_frameSuppressUntil.find(rid);
        if (sup != s_frameSuppressUntil.end() && now < sup->second)
            continue;

        CoSyncTransport::Send(SerializeFrameInventory(myEid, rid, ref->baseForm->formID,
            NiPoint3(ref->pos.x, ref->pos.y, ref->pos.z), sig));
        LOG_UPDATED("[PASync] TX PAI ref=0x%08X base=0x%08X items=%u",
            rid, ref->baseForm->formID, (unsigned)sig.size());
    }
}

__declspec(noinline) static void TickFrameInventoryScanSafe(double now)
{
    __try { TickFrameInventoryScan_Impl(now); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[PASync] TickFrameInventoryScan faulted -- caught");
    }
}

static void TickFrameInventoryScan()
{
    if (!CoSyncNet::IsConnected())
        return;
    const double now = NowSecondsPA();
    if (now - s_lastFrameInvPoll < kFrameInvPollInterval)
        return;
    s_lastFrameInvPoll = now;
    TickFrameInventoryScanSafe(now);
}

static void PaFrameAddItem(TESObjectREFR* target, TESForm* item, int32_t count)
{
    static const BSFixedString kAddItem("AddItem");
    if (!target || !item || count <= 0) return;
    // Engine-native add first (synchronous, real stacks); the queued Papyrus
    // call only as a fallback.
    if (CoSyncGameAPI::NativeAddItem(target, item, static_cast<UInt32>(count)))
        return;
    VMArray<VMVariable> args;
    VMVariable vForm; CoSyncVM::SetFormArg(vForm, item); args.Push(&vForm);
    SInt32 cnt = count; VMVariable vCount; vCount.Set(&cnt); args.Push(&vCount);
    bool silent = true; VMVariable vSilent; vSilent.Set(&silent); args.Push(&vSilent);
    CallFunctionNoWait(target, kAddItem, args);
}

static void PaFrameRemoveItem(TESObjectREFR* target, TESForm* item, int32_t count)
{
    static const BSFixedString kRemoveItem("RemoveItem");
    if (!target || !item || count <= 0) return;
    // Engine-native remove first (synchronous); Papyrus only as a fallback.
    if (CoSyncGameAPI::NativeRemoveItem(target, item, static_cast<UInt32>(count)))
        return;
    VMArray<VMVariable> args;
    VMVariable vForm; CoSyncVM::SetFormArg(vForm, item); args.Push(&vForm);
    SInt32 cnt = count; VMVariable vCount; vCount.Set(&cnt); args.Push(&vCount);
    bool silent = true; VMVariable vSilent; vSilent.Set(&silent); args.Push(&vSilent);
    VMVariable vDest; CoSyncVM::SetFormArg(vDest, nullptr); args.Push(&vDest);
    CallFunctionNoWait(target, kRemoveItem, args);
}

// This machine's copy of a frame another player reported: the same persistent
// ref, else the matching frame near that position.
static TESObjectREFR* ResolveReportedFrame(uint32_t senderEid, uint32_t refFormID, uint32_t baseFormID, const NiPoint3& pos)
{
    if (TESObjectREFR* known = LookupFrameCopy(senderEid, refFormID))
        return known;
    TESObjectREFR* frame = nullptr;
    if (!IsDynamicFormID(refFormID))
    {
        TESObjectREFR* real = SafeLookupAndCastRefr(refFormID);
        if (real && SafeIsRefPowerArmorFrame(real))
            frame = real;
    }
    if (!frame)
        frame = FindLocalChassisInCell(baseFormID, pos, 0);
    if (frame)
        RememberFrameCopy(senderEid, refFormID, frame->formID);
    return frame;
}

// -- Frame piece mods (PAMODS) ------------------------------------------------
namespace
{
    struct PaModsOp
    {
        uint32_t    senderEntityID = 0;
        uint32_t    refFormID = 0;
        uint32_t    baseFormID = 0;
        NiPoint3    pos;
        std::string mods;
        int         tries = 0;
        double      nextTry = 0.0;
    };
    std::mutex            s_modsMutex;
    std::vector<PaModsOp> s_pendingMods;   // from the network thread
    std::vector<PaModsOp> s_retryMods;     // game thread: pieces still arriving
}

bool CoSyncPowerArmorSync::IsPowerArmorMods(const std::string& msg)
{
    return msg.rfind("PAMODS|", 0) == 0;
}

void CoSyncPowerArmorSync::OnReceivePowerArmorMods(const std::string& msg)
{
    PaModsOp op{};
    const char* s = msg.c_str() + 7;
    char* e = nullptr;
    op.senderEntityID = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return; s = e + 1;
    op.refFormID      = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return; s = e + 1;
    op.baseFormID     = static_cast<uint32_t>(std::strtoul(s, &e, 10)); if (!e || *e != '|') return; s = e + 1;
    op.pos.x = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
    op.pos.y = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
    op.pos.z = std::strtof(s, &e); if (!e || *e != '|') return; s = e + 1;
    op.mods = s;
    if (op.baseFormID == 0 || op.mods.empty())
        return;
    if (op.senderEntityID != 0 && op.senderEntityID == CoSyncNet::GetMyEntityID())
        return;
    std::lock_guard<std::mutex> lk(s_modsMutex);
    s_pendingMods.push_back(std::move(op));
}

static void FlushPendingFrameModsOps()
{
    {
        std::lock_guard<std::mutex> lk(s_modsMutex);
        for (PaModsOp& op : s_pendingMods)
        {
            // A newer report for the same frame replaces an older retry.
            s_retryMods.erase(std::remove_if(s_retryMods.begin(), s_retryMods.end(),
                [&op](const PaModsOp& r) { return r.refFormID == op.refFormID && r.baseFormID == op.baseFormID; }),
                s_retryMods.end());
            s_retryMods.push_back(std::move(op));
        }
        s_pendingMods.clear();
    }
    if (s_retryMods.empty())
        return;
    const double now = NowSecondsPA();
    for (auto it = s_retryMods.begin(); it != s_retryMods.end();)
    {
        if (now < it->nextTry) { ++it; continue; }
        TESObjectREFR* frame = ResolveReportedFrame(it->senderEntityID, it->refFormID, it->baseFormID, it->pos);
        int missing = -1;
        if (frame && !IsFrameOccupiedLocally(frame->formID))
        {
            missing = CoSyncItemMods::ApplyMods(frame, it->mods, it->refFormID);
            s_frameLastMods[frame->formID] = it->mods;        // do not echo it back
            s_frameSuppressUntil[frame->formID] = now + kFrameInvEchoSuppressSec;
        }
        // Pieces may still be on their way (PAI) -- try again for about 10 s.
        if (missing != 0 && ++it->tries < 10)
        {
            it->nextTry = now + 1.0;
            ++it;
            continue;
        }
        if (missing != 0)
            LOG_UPDATED_WARN("[PASync] PAMODS for frame ref=0x%08X: %d mod(s) could not be applied", it->refFormID, missing);
        it = s_retryMods.erase(it);
    }
}

static void ApplyFrameInventoryOp(const PaInvOp& op, double now)
{
    TESObjectREFR* frame = ResolveReportedFrame(op.senderEntityID, op.refFormID, op.baseFormID, op.pos);

    if (!frame)
    {
        LOG_UPDATED_WARN("[PASync] PAI: no local frame for ref=0x%08X base=0x%08X near (%.0f,%.0f,%.0f)",
            op.refFormID, op.baseFormID, op.pos.x, op.pos.y, op.pos.z);
        return;
    }

    const uint32_t lid = frame->formID;
    if (IsFrameOccupiedLocally(lid))
        return;

    std::vector<PaInvItem> local;
    GatherFrameSignature(frame, local);
    NormalizeSignature(local);

    int adds = 0, removes = 0, healthFixes = 0;

    // Counts per form first (entries are per stack/condition since 0.3.3).
    auto total = [](const std::vector<PaInvItem>& v, uint32_t fid)
    {
        int32_t t = 0;
        for (const PaInvItem& x : v) if (x.formID == fid) t += x.count;
        return t;
    };
    std::vector<uint32_t> forms;
    for (const PaInvItem& r : op.items) if (std::find(forms.begin(), forms.end(), r.formID) == forms.end()) forms.push_back(r.formID);
    for (const PaInvItem& l : local)    if (std::find(forms.begin(), forms.end(), l.formID) == forms.end()) forms.push_back(l.formID);

    for (uint32_t fid : forms)
    {
        const int32_t want = total(op.items, fid);
        const int32_t have = total(local, fid);
        if (want == have) continue;
        TESForm* form = SafeLookupForm(fid);
        if (!form) continue;
        const UInt8 ft = form->formType;
        if (ft != kFormType_ARMO && ft != kFormType_AMMO) continue;
        if (want > have) { PaFrameAddItem(frame, form, want - have); ++adds; }
        else             { PaFrameRemoveItem(frame, form, have - want); ++removes; }
    }

    // 0.3.3: then condition, stack by stack. PA pieces and partly used fusion
    // cores are single-item stacks; assign the sender's conditions to ours in
    // sorted order. A merged multi-item stack with mixed conditions on the
    // sender cannot be split here and is left as it is.
    for (uint32_t fid : forms)
    {
        std::vector<float> want;
        for (const PaInvItem& r : op.items)
            if (r.formID == fid)
                for (int32_t k = 0; k < r.count && want.size() < 32; ++k) want.push_back(r.health);
        bool anyDamaged = false;
        for (float h : want) if (h >= 0.f) anyDamaged = true;
        if (!anyDamaged)
            continue;   // all full: nothing to set
        TESForm* form = SafeLookupForm(fid);
        if (!form) continue;
        CoSyncGameAPI::EnsureStackExtraLists(frame, form);
        BGSInventoryItem::Stack* stackBuf[32];
        const int nStacks = SafeCollectStacks(frame, form, stackBuf, 32);
        std::vector<BGSInventoryItem::Stack*> stacks(stackBuf, stackBuf + nStacks);
        bool allSingles = !stacks.empty() && stacks.size() == want.size();
        for (BGSInventoryItem::Stack* st : stacks) if (!st || st->count != 1) allSingles = false;
        if (!allSingles)
        {
            LOG_UPDATED("[PASync] PAI: condition of form 0x%08X on frame 0x%08X not applied (stacks merged)", fid, lid);
            continue;
        }
        std::sort(want.begin(), want.end());
        std::sort(stacks.begin(), stacks.end(), [](BGSInventoryItem::Stack* a, BGSInventoryItem::Stack* b)
        { return SafeStackHealth(a) < SafeStackHealth(b); });
        for (size_t k = 0; k < stacks.size(); ++k)
            if (HealthKey(SafeStackHealth(stacks[k])) != HealthKey(want[k]) && SafeSetStackHealth(stacks[k], want[k]))
                ++healthFixes;
    }

    s_frameLastSig[lid]       = op.items;
    s_frameSuppressUntil[lid] = now + kFrameInvEchoSuppressSec;

    if (adds || removes || healthFixes)
        LOG_UPDATED("[PASync] PAI applied to frame=0x%08X (remote ref=0x%08X): +%d/-%d ops, %d condition(s) set",
            lid, op.refFormID, adds, removes, healthFixes);
}

static void FlushPendingFrameInventoryOps()
{
    std::vector<PaInvOp> invOps;
    {
        std::lock_guard<std::mutex> lk(s_invMutex);
        if (s_pendingInv.empty())
            return;
        invOps.swap(s_pendingInv);
    }
    const double now = NowSecondsPA();
    for (const PaInvOp& op : invOps)
        ApplyFrameInventoryOp(op, now);
}

bool CoSyncPowerArmorSync::IsSenderPowerArmorPiece(uint32_t eid, uint32_t formID)
{
    if (!eid || !formID)
        return false;
    std::lock_guard<std::mutex> lk(s_mutex);
    auto it = s_rxPaPieces.find(eid);
    return it != s_rxPaPieces.end() &&
        std::find(it->second.begin(), it->second.end(), formID) != it->second.end();
}

void CoSyncPowerArmorSync::Reset()
{
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        s_rxPaPieces.clear();
    }
    {
        std::lock_guard<std::mutex> lk(s_ownMx);
        s_owners.clear();
        s_ownLastBroadcast = 0.0;
    }
    s_localEntryDenied.store(false);
    LOG_REQUIREMENT("CoSyncPowerArmorSync::Reset", "No FO4 engine state involved -- clears the frame-inventory / fallback session state for a new session.");
    {
        std::lock_guard<std::mutex> lk(s_invMutex);
        s_pendingInv.clear();
    }
    s_fbLastPoll        = 0.0;
    s_fbPendingSince    = 0.0;
    s_fbLoggedNoChassis = false;
    s_fbRaceConfirmed   = false;
    s_pendingPostExit.clear();
    s_lastFrameInvPoll  = 0.0;
    s_frameLastSig.clear();
    s_frameSuppressUntil.clear();
    s_frameLastMods.clear();
    {
        std::lock_guard<std::mutex> lk(s_modsMutex);
        s_pendingMods.clear();
    }
    s_retryMods.clear();
    LOG_INFO("[PASync] Reset (frame inventory + fallback state)");
}

// ── 0.3.6: animated entry into the REAL frame ────────────────────────────────
// Everyone should see the chassis open and the character climb in, as in
// single player. SwitchToPowerArmor (the clone path below) is documented by
// Bethesda as "Instantly pop an actor into aArmorFurniture without any
// animation". When this machine has a loaded copy of the sender's actual
// frame next to the proxy, the proxy activates THAT frame through the
// engine's activation (the same path an AI actor uses to get into power
// armor). The frame is painted from the sender's worn mods first. If the
// engine has not put the proxy in power armor within kAnimatedEnterSec
// (ExtraPowerArmor 0xBB present), the op is re-run through the clone path.
static constexpr double kAnimatedEnterSec = 6.0;
static constexpr float  kAnimatedEnterMaxDist = 400.f;

static bool TryAnimatedEnter(CoSyncPlayer* proxy, const PaOp& op)
{
    TESObjectREFR* frame = LookupFrameCopy(op.senderEntityID, op.chassisRefFormID);
    if (!frame)
        frame = FindLocalChassisInCell(op.chassisBaseFormID, op.chassisPos, 0);
    if (!frame || !SafeIsRefPowerArmorFrame(frame) || !frame->GetObjectRootNode() ||
        !proxy->actorRef->GetObjectRootNode() || (frame->flags & TESObjectREFR::kFlag_IsDisabled))
        return false;
    const float dx = frame->pos.x - proxy->actorRef->pos.x, dy = frame->pos.y - proxy->actorRef->pos.y;
    if (dx * dx + dy * dy > kAnimatedEnterMaxDist * kAnimatedEnterMaxDist)
        return false;

    const std::string worn = CoSyncItemMods::DescribeWanted(op.senderEntityID);
    if (!worn.empty())
    {
        const int missing = CoSyncItemMods::ApplyMods(frame, worn, frame->formID);
        LOG_UPDATED("[PASync] PA Paint/Mod Detected: sender entity=%u worn mods applied to real frame 0x%08X (%d not on a frame piece)",
            op.senderEntityID, frame->formID, missing);
    }

    LOG_UPDATED("[PASync] PA Enter Started (animated): proxy entity=%u activates real frame 0x%08X",
        op.senderEntityID, frame->formID);
    if (!CoSyncDoorHook::ActivateAs(frame, reinterpret_cast<TESObjectREFR*>(proxy->actorRef)))
    {
        LOG_UPDATED_WARN("[PASync] PA Enter (animated): activation refused for frame 0x%08X -- clone path", frame->formID);
        return false;
    }
    // The frame the proxy climbs into is this machine's copy of the sender's
    // frame: FinishPaExit keeps it on exit (spawned id set, real-frame id 0).
    proxy->m_isInPowerArmor            = true;
    proxy->m_paChassisRefFormID        = op.chassisRefFormID;
    proxy->m_paSpawnedChassisRefFormID = frame->formID;
    proxy->m_paChassisIsRealFrame      = true;
    proxy->m_paRealFrameRefFormID      = 0;
    proxy->m_paBehaviorEntered         = false;
    memcpy(proxy->m_paAppliedPieces, op.pieceFormIDs, sizeof(op.pieceFormIDs));
    RememberFrameCopy(op.senderEntityID, op.chassisRefFormID, frame->formID);
    s_animatedEnters.push_back({ op, frame->formID, PaDiagNowSeconds() + kAnimatedEnterSec });
    return true;
}

static void TickAnimatedEnters()
{
    if (s_animatedEnters.empty())
        return;
    const double now = PaDiagNowSeconds();
    for (size_t i = 0; i < s_animatedEnters.size(); )
    {
        PaAnimatedEnter& a = s_animatedEnters[i];
        CoSyncPlayer* proxy = g_CoSyncPlayerManager.GetPlayerByEntityID(a.op.senderEntityID);
        const int state = (proxy && proxy->actorRef) ? CoSyncGameAPI::GetActorPowerArmorExtraState(proxy->actorRef) : -1;
        if (state == 1)
        {
            LOG_UPDATED("[PASync] PA Enter Completed (animated): proxy entity=%u is in frame 0x%08X",
                a.op.senderEntityID, a.frame);
            if (proxy->m_lastEquippedWeaponFormID != 0)
                proxy->m_weaponDrawDelay = 30;
            s_animatedEnters[i] = s_animatedEnters.back();
            s_animatedEnters.pop_back();
            continue;
        }
        if (!proxy || now >= a.deadline)
        {
            if (proxy)
            {
                LOG_UPDATED_WARN("[PASync] PA Enter (animated) did not take in %.0f s for entity=%u -- using the clone entry",
                    kAnimatedEnterSec, a.op.senderEntityID);
                proxy->m_isInPowerArmor = false;
                proxy->m_paSpawnedChassisRefFormID = 0;
                proxy->m_paChassisIsRealFrame = false;
                PaOp retry = a.op;
                retry.forceClone = true;
                std::lock_guard<std::mutex> lk(s_mutex);
                s_pending.push_back(retry);
            }
            s_animatedEnters[i] = s_animatedEnters.back();
            s_animatedEnters.pop_back();
            continue;
        }
        ++i;
    }
}

void CoSyncPowerArmorSync::FlushPendingPowerArmorOps()
{
    LOG_REQUIREMENT("CoSyncPowerArmorSync::FlushPendingPowerArmorOps", "Needs the player manager to be ready, and checks each stand-in character is actually spawned before touching it. The armor frame's position is set when it's first created, not moved afterward.");

    HostTickOwnership(NowSecondsPA());
    TickAnimatedEnters();
    if (s_localEntryDenied.exchange(false) && CoSyncPowerArmorSync::IsLocalPlayerInPowerArmor())
    {
        CoSyncPowerArmorSync::ExitLocalPlayerPowerArmor();
        CoSyncGameAPI::ShowNotification("This Power Armor belongs to another player.");
    }
    if (s_notifyEntryRefused.exchange(false))
        CoSyncGameAPI::ShowNotification("This Power Armor belongs to another player.");

    // Sender-side: catch PA pieces swapped while already wearing the frame.
    TickLocalPowerArmorPieces();
    // 2026-09-05: race-based enter/exit fallback + unworn-frame inventory sync.
    TickLocalPowerArmorFallback();
    TickFrameInventoryScan();
    FlushPendingFrameInventoryOps();
    FlushPendingFrameModsOps();
    static const BSFixedString kSwitchToPowerArmor("SwitchToPowerArmor");

    // Diagnostic re-checks run every call (not gated on s_pending) so a
    // scheduled check fires even if no new PA packets arrive in the
    // meantime.
    if (!s_pendingDiagChecks.empty())
    {
        const double now = PaDiagNowSeconds();
        for (size_t i = 0; i < s_pendingDiagChecks.size(); )
        {
            if (now < s_pendingDiagChecks[i].checkAt) { ++i; continue; }

            const PaDiagCheck chk = s_pendingDiagChecks[i];
            TESObjectREFR* chassisRefr = SafeLookupAndCastRefr(chk.chassisRefFormID);
            LogInventoryDiag(chassisRefr, "chassis @T+1s (did AddItem land?)");

            TESForm* actorForm = SafeLookupForm(chk.actorFormID);
            Actor* actor = (actorForm && actorForm->formType == kFormType_ACHR)
                ? static_cast<Actor*>(actorForm) : nullptr;
            LogInventoryDiag(reinterpret_cast<TESObjectREFR*>(actor),
                "actor @T+1s (did SwitchToPowerArmor auto-equip from chassis?)");
            LOG_UPDATED("[PASync] PA state @T+1s: actor=0x%08X ExtraPowerArmor=%d (1=in PA, 0=not, -1=unreadable)",
                chk.actorFormID, CoSyncGameAPI::GetActorPowerArmorExtraState(actor));

            s_pendingDiagChecks[i] = s_pendingDiagChecks.back();
            s_pendingDiagChecks.pop_back();
        }
    }

    if (!s_pendingActorRefresh.empty())
    {
        const double now = PaDiagNowSeconds();
        for (size_t i = 0; i < s_pendingActorRefresh.size(); )
        {
            if (now < s_pendingActorRefresh[i].refreshAt) { ++i; continue; }

            const PaActorRefreshCheck chk = s_pendingActorRefresh[i];
            TESForm* actorForm = SafeLookupForm(chk.actorFormID);
            if (actorForm && actorForm->formType == kFormType_ACHR)
            {
                // 0.3.3: the engine's own worn-3D rebuild request instead of a
                // Disable+Enable (which drops the weapon and blinks the model).
                const bool ok = CoSyncGameAPI::RequestActorModelUpdate(static_cast<Actor*>(actorForm));
                LOG_UPDATED("[PASync] Post-enter model update requested on actor=0x%08X (%s)",
                    chk.actorFormID, ok ? "queued" : "no middle-high process yet");
            }
            else
            {
                LOG_UPDATED_WARN("[PASync] Post-enter actor 3D refresh: actor=0x%08X no longer resolvable, skipped",
                    chk.actorFormID);
            }

            s_pendingActorRefresh[i] = s_pendingActorRefresh.back();
            s_pendingActorRefresh.pop_back();
        }
    }

    // 0.3.4: did the engine restore the body after a PA exit? Expected: the
    // race is no longer PowerArmorRace and the 3D is present. Only if not,
    // fall back to the old full rebuild -- loudly, so a log shows it.
    if (!s_pendingPostExit.empty())
    {
        const double now = PaDiagNowSeconds();
        for (size_t i = 0; i < s_pendingPostExit.size(); )
        {
            if (now < s_pendingPostExit[i].checkAt) { ++i; continue; }
            const uint32_t eid = s_pendingPostExit[i].entityID;
            s_pendingPostExit[i] = s_pendingPostExit.back();
            s_pendingPostExit.pop_back();

            CoSyncPlayer* proxy = g_CoSyncPlayerManager.GetPlayerByEntityID(eid);
            if (!proxy || !proxy->actorRef || proxy->m_isInPowerArmor || proxy->m_proxyHiddenForCell ||
                !CoSyncGameAPI::IsFormStillLive(static_cast<TESForm*>(proxy->actorRef)))
                continue;   // gone, re-entered, or hidden (re-shown with a full rebuild anyway)

            const bool raceStillPA = SafeActorInPowerArmorByRace(proxy->actorRef);
            const bool has3D       = proxy->actorRef->GetObjectRootNode() != nullptr;
            const int  level       = SafeProcessLevel(proxy->actorRef);
            if (!raceStillPA && (has3D || level >= 2))
            {
                LOG_UPDATED("[PASync] Post-exit body check entity=%u: OK (race restored, 3D=%d, process level %d) -- engine rebuild was enough",
                    eid, has3D ? 1 : 0, level);
                continue;
            }
            LOG_WARN("[PASync] Post-exit body check entity=%u: FAILED (race still PowerArmorRace=%d, 3D=%d, process level %d) -- falling back to a full 3D rebuild",
                eid, raceStillPA ? 1 : 0, has3D ? 1 : 0, level);
            CoSyncGameAPI::RefreshActor3D(proxy->actorRef);
            if (proxy->m_lastQueuedWeaponFormID != 0)
            {
                proxy->m_postRefreshWeaponFormID = proxy->m_lastQueuedWeaponFormID;
                proxy->m_postRefreshWeaponDelay  = 60;
                proxy->m_weaponConfirmedEquipped = false;
            }
        }
    }

    // Also runs every call, unconditionally -- see PaExitRetry's comment.
    if (!s_pendingExitRetries.empty())
    {
        const double now = PaDiagNowSeconds();
        for (size_t i = 0; i < s_pendingExitRetries.size(); )
        {
            PaExitRetry& r = s_pendingExitRetries[i];
            if (now < r.nextAttemptAt) { ++i; continue; }

            CoSyncPlayer* proxy = g_CoSyncPlayerManager.GetPlayerByEntityID(r.entityID);
            if (!proxy || !proxy->actorRef || !proxy->m_isInPowerArmor)
            {
                // Proxy gone, or something else already resolved its PA
                // state (e.g. a fresh enter superseded this exit) -- nothing
                // left to retry.
                s_pendingExitRetries[i] = s_pendingExitRetries.back();
                s_pendingExitRetries.pop_back();
                continue;
            }

            const int paNow = CoSyncGameAPI::GetActorPowerArmorExtraState(proxy->actorRef);
            if (paNow != 1)
            {
                LOG_UPDATED("[PASync] Exit PA retry entity=%u: now confirmed out (ExtraPowerArmor=%d), finishing cleanup",
                    r.entityID, paNow);
                FinishPaExit(proxy, r.entityID);
                s_pendingExitRetries[i] = s_pendingExitRetries.back();
                s_pendingExitRetries.pop_back();
                continue;
            }

            if (--r.attemptsLeft <= 0)
            {
                LOG_WARN("[PASync] Exit PA retry entity=%u: gave up after repeated attempts, actor still reads "
                         "ExtraPowerArmor=1 -- leaving PA state as-is rather than tearing down a still-merged chassis",
                    r.entityID);
                s_pendingExitRetries[i] = s_pendingExitRetries.back();
                s_pendingExitRetries.pop_back();
                continue;
            }

            // Still inside the engine's own exit sequence: wait, do not
            // restart it. Re-issue the eject only after a stretch with no
            // progress.
            bool ejected = false;
            if (now - r.lastEjectAt >= kPaEjectReissueSec)
            {
                ejected = SafeSwitchFromPowerArmorNative(proxy->actorRef);
                r.lastEjectAt = now;
            }
            const int  paAfter = CoSyncGameAPI::GetActorPowerArmorExtraState(proxy->actorRef);
            LOG_UPDATED("[PASync] Exit PA retry entity=%u (attemptsLeft=%d): native eject %s, ExtraPowerArmor now=%d",
                r.entityID, r.attemptsLeft, ejected ? "re-issued" : "not re-issued (waiting for the engine's exit)", paAfter);
            if (paAfter != 1)
            {
                FinishPaExit(proxy, r.entityID);
                s_pendingExitRetries[i] = s_pendingExitRetries.back();
                s_pendingExitRetries.pop_back();
                continue;
            }

            r.nextAttemptAt = now + 1.0;
            ++i;
        }
    }

    std::vector<PaOp> ops;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_pending.empty())
            return;
        ops.swap(s_pending);
    }

    for (const PaOp& op : ops)
    {

        CoSyncPlayer* proxy = g_CoSyncPlayerManager.GetPlayerByEntityID(op.senderEntityID);
        if (!proxy || !proxy->actorRef || !proxy->hasSpawned)
        {
            // The sender repeats its PA state every 5 s, so this is picked up
            // once the proxy exists; no need to hold the op.
            LOG_EVERY_MS(10000, LOG_UPDATED_WARN("[PASync] No spawned proxy for sender=%u (PA state applies once it spawns)",
                op.senderEntityID));
            continue;
        }
        // A proxy hidden because its player is in another location gets its
        // PA from a later repeat, once it is shown here.
        if (op.entering && !proxy->m_isInPowerArmor && proxy->m_proxyHiddenForCell)
            continue;
        if (op.entering && !op.forceClone && std::any_of(s_animatedEnters.begin(), s_animatedEnters.end(),
                [&op](const PaAnimatedEnter& a) { return a.op.senderEntityID == op.senderEntityID; }))
            continue;   // animated entry in flight

        TESForm* chassisBaseForm = SafeLookupForm(op.chassisBaseFormID);
        if (!chassisBaseForm)
        {
            LOG_UPDATED_WARN("[PASync] Chassis base form 0x%08X not found", op.chassisBaseFormID);
            continue;
        }

        // Fired unconditionally (even for forms the blocklist below will skip
        // spawning) so a known-unsafe form like 0x00034309 still gets a live
        // data point confirming the [base+0x100] null-pointer theory, instead
        // of only ever seeing this diagnostic for NEW/unknown unsafe forms.
        if (op.entering)
            LogPlaceAtMeComponentDiag(chassisBaseForm);

        if (op.entering && proxy->m_isInPowerArmor && proxy->m_paSpawnedChassisRefFormID != 0)
        {
            // 2026-08-26: this proxy is ALREADY in PA -- this op is a
            // mid-session piece-set update (TickLocalPowerArmorPieces
            // re-announces as an "enter" whenever the wearer's pieces
            // change), not a fresh enter. Live log caught the bug this used
            // to fall into: treating EVERY one of these re-broadcasts as a
            // brand-new enter spawned a WHOLE NEW chassis and re-ran
            // SwitchToPowerArmorDirect every single time -- 4 distinct
            // "spawned chassis=..." lines for ONE continuous PA session in
            // one test, each overwriting m_paSpawnedChassisRefFormID so
            // every earlier clone became permanently orphaned (never
            // cleaned up, since only the LAST one is ever remembered for
            // exit). That's the most likely explanation for the reported
            // "chassis floating/duplicated" and "model stretching" on
            // exit -- repeated real merges into different frames in rapid
            // succession, not a single clean enter-then-exit. Apply the new
            // pieces to the EXISTING chassis instead.
            // 0.3.3 (Stage 5): the sender now repeats its PA state every 5 s.
            // An unchanged piece set must not touch the proxy -- this branch
            // unequips and re-equips every piece, which would rebuild the
            // armor every 5 s.
            if (memcmp(op.pieceFormIDs, proxy->m_paAppliedPieces, sizeof(op.pieceFormIDs)) == 0)
                continue;

            TESObjectREFR* existingChassis = SafeLookupAndCastRefr(proxy->m_paSpawnedChassisRefFormID);
            if (existingChassis)
            {
                uint32_t oldPieces[6];
                memcpy(oldPieces, proxy->m_paAppliedPieces, sizeof(oldPieces));
                memcpy(proxy->m_paAppliedPieces, op.pieceFormIDs, sizeof(op.pieceFormIDs));
                int piecesApplied = 0;
                for (int pi = 0; pi < 6; ++pi)
                {
                    if (op.pieceFormIDs[pi] == 0) continue;
                    TESForm* pieceForm = SafeLookupForm(op.pieceFormIDs[pi]);
                    if (!pieceForm) continue;
                    CoSyncGameAPI::DirectInjectInventoryItem(existingChassis, pieceForm);
                    // 0.3.3: equip only what the engine does not already
                    // have equipped (the old forced unequip->equip edge
                    // rebuilt every piece on every update).
                    bool present = false, equipped = false;
                    CoSyncGameAPI::GetInventoryEquipState(proxy->actorRef, op.pieceFormIDs[pi], present, equipped);
                    if (!equipped && CoSyncEquipHook::EquipItemDirect(proxy->actorRef, pieceForm))
                        ++piecesApplied;
                }
                for (int pi = 0; pi < 6; ++pi)
                {
                    const uint32_t oldFid = oldPieces[pi];
                    if (!oldFid) continue;
                    bool still = false;
                    for (int k = 0; k < 6; ++k) if (op.pieceFormIDs[k] == oldFid) still = true;
                    if (still) continue;
                    if (TESForm* oldForm = SafeLookupForm(oldFid))
                        CoSyncEquipHook::UnequipItemDirect(proxy->actorRef, oldForm);
                }
                CoSyncGameAPI::RequestActorModelUpdate(proxy->actorRef);
                LOG_UPDATED("[PASync] Piece update: %d piece(s) applied to existing chassis=0x%08X entity=%u",
                    piecesApplied, existingChassis->formID, op.senderEntityID);

                // 2026-08-26: this scheduled refresh existed ONLY on the
                // fresh-enter path below (s_pendingActorRefresh, 1.2s after
                // entering) -- a mid-session piece SWAP never got one at all,
                // regardless of whether the refresh is what actually makes
                // pieces render (still not fully confirmed -- see the ongoing
                // IDA investigation into what Disable+Enable really does).
                // Whatever it does, it was never even attempted here. Same
                // 1.2s delay, same reasoning: give the immediate EquipItemDirect
                // call above time to settle before forcing the rebuild.
                // 0.3.3: no Disable+Enable rebuild here any more; the engine
                // rebuilds the worn 3D itself (RequestActorModelUpdate above).
            }
            else
            {
                LOG_UPDATED_WARN("[PASync] Piece update: existing chassis=0x%08X not resolvable, skipped entity=%u",
                    proxy->m_paSpawnedChassisRefFormID, op.senderEntityID);
            }
        }
        else if (op.entering && !op.forceClone && TryAnimatedEnter(proxy, op))
        {
            // 0.3.6: the proxy is walking into the real frame with the engine's
            // own entry animation; verified (or replaced by the clone path)
            // in TickAnimatedEnters.
        }
        else if (op.entering)
        {
            // Guaranteed-valid chassis: spawn a FRESH frame at the proxy from the
            // base form the host sent. This replaces the old "resolve the host's
            // ref / cell-scan for a matching frame" logic, which failed on the
            // receiving client in the common case (the host's chassis ref is
            // dynamic 0xFF and unresolvable, and the client usually isn't near an
            // identical unoccupied frame) — leaving SwitchToPowerArmor with no
            // furniture to enter, so it silently no-op'd and PA never attached.
            TESObjectREFR* proxyRefr = reinterpret_cast<TESObjectREFR*>(proxy->actorRef);

            // No longer checked against IsKnownUnsafePlaceAtMeForm -- that
            // blocklist is specific to PlaceAtMe_Native's crash class, which
            // SpawnChassisAtProxy no longer goes through (see its own comment).
            TESObjectREFR* chassis = SpawnChassisAtProxy(chassisBaseForm, proxyRefr);

            if (!chassis)
            {
                LOG_WARN("[PASync] Enter PA: failed to spawn chassis base=0x%08X at proxy entity=%u — visual skipped",
                    op.chassisBaseFormID, op.senderEntityID);
            }
            else
            {
                // Position AND facing already landed correctly at spawn --
                // SpawnChassisAtProxy passes both into CreateReferenceAtLocation's
                // own dir field (see its comment). No separate MoveRefrToPosition
                // pin needed here: its rotation argument is IDA-confirmed dead for
                // a non-actor ref (1.11.240.0), and position already matches
                // (spawned at proxyRefr->pos in the first place).

                // 2026-07-31: chassis is spawned via CreateReferenceAtLocation
                // (see SpawnChassisAtProxy), which this codebase already knows
                // skips the template/facegen/appearance resolution a normal
                // PlaceAtMe runs -- the same reason RefreshActor3D exists for
                // actor proxies. Reported symptom without this: the frame
                // visually "glides" attached to the proxy instead of the actor
                // looking like it's inside it. Native, synchronous (not
                // Papyrus-queued) to avoid the same VM-queue race documented
                // on RefreshActor3D's own history.
                CoSyncGameAPI::DisableWorldActor(chassis);
                CoSyncGameAPI::EnableWorldActor(chassis);

                // Add PA pieces to the chassis BEFORE SwitchToPowerArmor so
                // the engine finds them in the frame's container during the
                // transition.
                // FIXED 2026-07-30: this used to ALSO call "EquipItem" on
                // chassis after AddItem, on the theory that PA frames wear
                // their pieces via ObjectReference.EquipItem like vanilla CK
                // frame scripts. That was never actually true -- checked
                // directly against the game's own vendored Papyrus source
                // (Data\Scripts\Source\Base\ObjectReference.psc vs Actor.psc):
                // EquipItem is declared ONLY on Actor, never on
                // ObjectReference, and no vanilla PA script calls it on a
                // frame ref either. CallFunctionNoWait(chassis, "EquipItem",
                // ...) was therefore silently failing to dispatch on every
                // call (chassis is a plain FURN TESObjectREFR, never an
                // Actor) -- pieces were added to the frame's inventory but
                // never actually equipped/attached, which is the reported
                // "PA piece equip not working" bug. Removed the dead call;
                // AddItem alone is kept, relying on SwitchToPowerArmor's own
                // native internals (sub_140989A80, which this codebase's
                // earlier IDA work already found calls
                // ActorEquipManager::EquipObject 0xCE5A90 internally) to pick
                // the pieces up from the frame's inventory on entry, the same
                // way a real player-installed frame behaves. Needs an
                // in-game retest; if pieces still don't attach, the frame's
                // real native piece-attach path needs IDA RE next.
                // Baseline, before any AddItem call has a chance to run
                // (CallFunctionNoWait is VM-queued, not synchronous) -- shows
                // whether the fresh chassis even has an inventoryList
                // allocated yet, and confirms it's empty before we act.
                LogInventoryDiag(chassis, "chassis BEFORE AddItem (baseline)");

                // 2026-08-26: switched from CallFunctionNoWait("AddItem") to
                // DirectInjectInventoryItem -- live log proved the Papyrus VM
                // AddItem call was the actual failure: 6 calls queued, only 1
                // (unrelated) item ever landed in the chassis's inventory
                // (LogInventoryDiag @T+1s confirmed it). Exact same silent-
                // failure class this codebase already fixed for actor refs
                // (round 145); DirectInjectInventoryItem was widened from
                // Actor* to TESObjectREFR* specifically so it could be reused
                // here for a plain FURN chassis. Also synchronous, unlike the
                // old VM-queued call -- pieces are guaranteed to be in the
                // chassis's inventory before SwitchToPowerArmorDirect runs a
                // few lines below, removing a queue-ordering race that used
                // to just be hoped for.
                // 0.3.6: the chassis copy must be the ONLY copy of each piece
                // the proxy ends up with. Copies already on the proxy (0.3.5:
                // OutfitSync injected the sender's worn PA pieces as outfit
                // armor, and re-injected one on every rebuild) made the engine
                // refuse every paint mod ("Can only mod singular items",
                // sub_141180DE0) and SwitchToPowerArmor equipped a bare copy.
                for (int pi = 0; pi < 6; ++pi)
                {
                    if (op.pieceFormIDs[pi] == 0) continue;
                    TESForm* pieceForm = SafeLookupForm(op.pieceFormIDs[pi]);
                    if (!pieceForm) continue;
                    TESObjectREFR* proxyRef = reinterpret_cast<TESObjectREFR*>(proxy->actorRef);
                    BGSInventoryItem::Stack* stacks[16];
                    const int ns = SafeCollectStacks(proxyRef, pieceForm, stacks, 16);
                    UInt32 have = 0;
                    for (int k = 0; k < ns; ++k)
                        have += stacks[k]->count;
                    if (have > 0 && CoSyncGameAPI::NativeRemoveItem(proxyRef, pieceForm, have))
                        LOG_UPDATED("[PASync] Enter PA: removed %u stray copy/copies of piece 0x%08X from proxy entity=%u",
                            have, op.pieceFormIDs[pi], op.senderEntityID);
                }

                int piecesAdded = 0;
                for (int pi = 0; pi < 6; ++pi)
                {
                    if (op.pieceFormIDs[pi] == 0) continue;
                    TESForm* pieceForm = SafeLookupForm(op.pieceFormIDs[pi]);
                    if (!pieceForm)
                    {
                        LOG_WARN("[PASync] PA piece formID=0x%08X not found on receiver, skipping",
                            op.pieceFormIDs[pi]);
                        continue;
                    }
                    if (CoSyncGameAPI::DirectInjectInventoryItem(chassis, pieceForm))
                        ++piecesAdded;
                    else
                        LOG_WARN("[PASync] PA piece formID=0x%08X DirectInject FAILED on chassis",
                            op.pieceFormIDs[pi]);
                }
                if (piecesAdded > 0)
                    LOG_UPDATED("[PASync] PA pieces: injected %d piece(s) directly into chassis entity=%u",
                        piecesAdded, op.senderEntityID);

                // 0.3.3: the pieces just injected are bare base forms. If this
                // machine has its copy of the sender's real frame (kept in sync
                // by PAMODS), copy its piece mods -- paint, armor upgrades,
                // station work -- onto the spawned chassis before entering, so
                // the proxy wears what the sender wears.
                // 0.3.6: first source is the sender's own report of the mods on
                // the pieces it WEARS (IMODS: paint, linings, jetpack -- sent on
                // every change, relayed by the host). The frame copy below was
                // the only source before and is usually empty by now: the
                // sender took the pieces out of the frame when it got in, and
                // PAI already mirrored that here.
                bool paintFromSender = false;
                {
                    const std::string worn = CoSyncItemMods::DescribeWanted(op.senderEntityID);
                    if (!worn.empty())
                    {
                        const int missing = CoSyncItemMods::ApplyMods(chassis, worn, chassis->formID);
                        paintFromSender = true;
                        LOG_UPDATED("[PASync] PA Paint/Mod Detected: sender entity=%u worn mods applied to chassis 0x%08X "
                            "before entry (%d listed mod(s) not on a chassis piece -- weapons etc.)",
                            op.senderEntityID, chassis->formID, missing);
                    }
                }
                TESObjectREFR* realCopy = paintFromSender ? nullptr : LookupFrameCopy(op.senderEntityID, op.chassisRefFormID);
                if (realCopy == chassis)
                    realCopy = nullptr;
                if (!realCopy && !paintFromSender)
                    realCopy = FindLocalChassisInCell(op.chassisBaseFormID, op.chassisPos, chassis->formID);
                if (realCopy)
                {
                    const std::string mods = CoSyncItemMods::DescribeMods(realCopy, false);
                    if (!mods.empty())
                    {
                        const int missing = CoSyncItemMods::ApplyMods(chassis, mods, chassis->formID);
                        LOG_UPDATED("[PASync] Enter PA: copied piece mods from frame 0x%08X to chassis 0x%08X (%d missing)",
                            realCopy->formID, chassis->formID, missing);
                    }
                }

                // Native enter first. The Papyrus wrapper this used to call has
                // three refusal paths that log to the PAPYRUS log only, and the
                // call itself is a form-argument CallFunctionNoWait on a proxy
                // actor -- the exact pattern already proven to silently fail on
                // these actors (the reason DirectInjectInventoryItem and
                // EquipItemDirect exist). Papyrus stays as a fallback for the
                // case where the native call itself faults.
                if (!CoSyncGameAPI::SwitchToPowerArmorDirect(proxy->actorRef, chassis))
                {
                    TESForm* refAsForm = chassis;
                    VMArray<VMVariable> swArgs;
                    VMVariable vRef; vRef.Set(&refAsForm); swArgs.Push(&vRef);
                    CallFunctionNoWait(proxy->actorRef, kSwitchToPowerArmor, swArgs);
                    LOG_UPDATED_WARN("[PASync] Enter PA: native switch failed, fell back to Papyrus entity=%u",
                        op.senderEntityID);
                }

                // 2026-08-26: IDA-confirmed (1.11.240.0, the real SwitchToPowerArmor
                // internal decompiled at its correct RVA 0x989D90 -- an earlier
                // session's 0x989A80 was a transcription error, mid-instruction in
                // an unrelated function) that its own internal per-piece
                // ActorEquipManager::EquipObject call installs each PA piece with
                // applyNow=0 (deferred) -- unlike CoSyncEquipHook::EquipItemDirect's
                // own call (the one already proven to make weapon/armor visuals
                // show), which uses applyNow=1 (immediate). A live, AI-driven
                // player gets a follow-up engine tick that applies a deferred
                // equip; our AI-disabled proxy never does, matching a PRIOR
                // session's finding that PA pieces can read equipped=1 in
                // inventory and still never visually render. Also confirmed: PA
                // piece installation goes through the exact same general equip-
                // visual pipeline as normal weapon/armor equips (0xCE5E20), not a
                // separate PA-specific biped path -- so EquipItemDirect (already
                // built for exactly this pipeline) is the correct tool to re-apply
                // with applyNow=1, not a new primitive. Loop AFTER
                // SwitchToPowerArmorDirect so the pieces are already logically
                // equipped (per the deferred call above) and this just forces the
                // immediate visual apply.
                // 2026-08-26 pt.2: EquipItemDirect alone still wasn't enough live
                // (confirmed: clean logs, zero failures, pieces still invisible).
                // Same class of bug as the weapon-vanish fix earlier this session
                // (DrawWeaponMagicHands being a no-op when called with the state
                // the actor is ALREADY in) -- ActorEquipManager::EquipObject very
                // plausibly has the same "already equipped, nothing to do"
                // early-out. The native SwitchToPowerArmorDirect call above
                // ALREADY ran its own internal EquipObject(applyNow=0) on each
                // piece, which likely marks it equipped even though the visual
                // never applied -- so our own follow-up EquipObject call sees
                // "already equipped" and may skip everything, INCLUDING the
                // applyNow=1 visual step we actually need. Force a real state
                // EDGE instead of a redundant same-state call: unequip first
                // (even if the engine already considers it equipped, this is a
                // real transition to a genuinely different state), then equip
                // with applyNow=1 -- mirrors the exact fix shape that worked for
                // the weapon (holster, then redraw) rather than trying the same
                // no-op call again.
                {
                    int piecesReapplied = 0;
                    for (int pi = 0; pi < 6; ++pi)
                    {
                        if (op.pieceFormIDs[pi] == 0) continue;
                        TESForm* pieceForm = SafeLookupForm(op.pieceFormIDs[pi]);
                        if (!pieceForm) continue;
                        // 0.3.3: the engine's deferred PA equip (applyNow=0)
                        // is applied by Actor::Update through update3DModel
                        // on this AI-enabled proxy; equip here only what it
                        // did not equip.
                        bool present = false, equipped = false;
                        CoSyncGameAPI::GetInventoryEquipState(proxy->actorRef, op.pieceFormIDs[pi], present, equipped);
                        if (!equipped && CoSyncEquipHook::EquipItemDirect(proxy->actorRef, pieceForm))
                            ++piecesReapplied;
                    }
                    if (piecesReapplied > 0)
                        LOG_UPDATED("[PASync] Enter PA: force-applied %d piece(s) via unequip+re-equip edge entity=%u",
                            piecesReapplied, op.senderEntityID);
                }

                proxy->m_paSpawnedChassisRefFormID = chassis->formID;
                LOG_UPDATED("[PASync] Enter PA: spawned chassis=0x%08X (base=0x%08X) at proxy entity=%u -> SwitchToPowerArmor",
                    chassis->formID, op.chassisBaseFormID, op.senderEntityID);

                // Schedule the deferred re-check (see LogInventoryDiag/
                // PaDiagCheck comments) -- 1s gives the VM queue plenty of
                // time to drain both AddItem and SwitchToPowerArmor.
                // Unconditional: the PA-state probe it now also carries is
                // worth having even on a zero-piece enter.
                s_pendingDiagChecks.push_back(PaDiagCheck{
                    chassis->formID, proxy->actorRef->formID, PaDiagNowSeconds() + 1.0 });

                // Schedule the actor-side 3D refresh (see PaActorRefreshCheck
                // comment) -- 1.2s so it runs after SwitchToPowerArmor's own
                // VM-queued call has landed, on every PA enter regardless of
                // whether any pieces were queued this time.
                s_pendingActorRefresh.push_back(PaActorRefreshCheck{
                    proxy->actorRef->formID, PaDiagNowSeconds() + 1.2 });
            }

            proxy->m_isInPowerArmor     = true;
            proxy->m_paChassisRefFormID = op.chassisRefFormID;
            proxy->m_paBehaviorEntered  = false;
            memcpy(proxy->m_paAppliedPieces, op.pieceFormIDs, sizeof(op.pieceFormIDs));

            // Weapon draw after PA enter: SwitchToPowerArmor is async (Papyrus
            // VM queue), so DrawWeaponMagicHands must fire after the PA
            // skeleton is live. 30 frames (~0.5s at 60fps) is enough for the
            // VM to drain the queued SwitchToPowerArmor and for the PA graph
            // to settle. Only arm the delay if the proxy already holds a
            // weapon — the equip system arms its own delay=8 for weapon
            // changes that happen while already in PA.
            if (proxy->m_lastEquippedWeaponFormID != 0)
            {
                proxy->m_weaponDrawDelay = 30;
                LOG_UPDATED("[PASync] Enter PA: armed weapon draw delay=30 for weapon=0x%08X entity=%u",
                    proxy->m_lastEquippedWeaponFormID, op.senderEntityID);
            }

            // 2026-08-26: hide THIS CLIENT'S own copy of the real-world frame
            // the sender actually entered, so it disappears for every observer
            // instead of just the wearer -- covers pre-placed, workshop-built,
            // and console-spawned frames alike (see FindLocalChassisInCell's
            // comment). Separate from the clone spawned above for the proxy
            // attach, which is unaffected either way.
            {
                TESObjectREFR* realFrame = LookupFrameCopy(op.senderEntityID, op.chassisRefFormID);
                if (!realFrame || realFrame->formID == proxy->m_paSpawnedChassisRefFormID)
                    realFrame = FindLocalChassisInCell(
                        op.chassisBaseFormID, op.chassisPos, proxy->m_paSpawnedChassisRefFormID);
                if (realFrame)
                {
                    CoSyncGameAPI::DisableWorldActor(realFrame);
                    proxy->m_paRealFrameRefFormID = realFrame->formID;
                    RememberFrameCopy(op.senderEntityID, op.chassisRefFormID, realFrame->formID);
                    LOG_UPDATED("[PASync] Enter PA: hid local real-frame ref=0x%08X (base=0x%08X) entity=%u",
                        realFrame->formID, op.chassisBaseFormID, op.senderEntityID);
                }
                else
                {
                    proxy->m_paRealFrameRefFormID = 0;
                    LOG_UPDATED("[PASync] Enter PA: no local real-frame found near (%.0f,%.0f,%.0f) base=0x%08X entity=%u -- not loaded/visible on this client",
                        op.chassisPos.x, op.chassisPos.y, op.chassisPos.z, op.chassisBaseFormID, op.senderEntityID);
                }
            }
        }
        else
        {
            // 2026-08-26: the eject-attempt + chassis-delete + real-frame-reshow
            // + 3D-rebuild sequence that used to be inline here now lives in
            // TryPaExit/FinishPaExit (above FlushPendingPowerArmorOps) so the
            // deferred-retry path (see PaExitRetry) can share the exact same
            // cleanup instead of duplicating it. Gated on the eject actually
            // taking -- see TryPaExit's comment for the live-tested reason.
            TryPaExit(proxy, op.senderEntityID);
        }
    }
}


// 0.3.3 (Stage 5): a proxy that is removed while in Power Armor (its player
// disconnected in PA, or the proxy was freed and is being rebuilt) used to
// leave its spawned chassis standing in the world for good, and the real frame
// it had hidden stayed hidden. Delete the one and re-show the other. The
// proxy's own actor is being torn down, so no eject / 3D rebuild here.
void CoSyncPowerArmorSync::OnProxyRemoved(CoSyncPlayer* proxy)
{
    if (!proxy)
        return;
    static const BSFixedString kDelete("Delete");
    if (proxy->m_paSpawnedChassisRefFormID != 0 && proxy->m_paChassisIsRealFrame)
    {
        // 0.3.6: the real frame (animated entry) stays in the world.
        LOG_UPDATED("[PASync] Proxy entity=%u removed while in PA: real frame 0x%08X kept",
            proxy->entityID, proxy->m_paSpawnedChassisRefFormID);
        proxy->m_paSpawnedChassisRefFormID = 0;
        proxy->m_paChassisIsRealFrame = false;
    }
    if (proxy->m_paSpawnedChassisRefFormID != 0)
    {
        if (TESObjectREFR* spawned = SafeLookupAndCastRefr(proxy->m_paSpawnedChassisRefFormID))
        {
            CoSyncGameAPI::DisableWorldActor(spawned);
            VMArray<VMVariable> delArgs;
            CallFunctionNoWait(spawned, kDelete, delArgs);
            LOG_UPDATED("[PASync] Proxy entity=%u removed while in PA: deleted its chassis=0x%08X",
                proxy->entityID, proxy->m_paSpawnedChassisRefFormID);
        }
        proxy->m_paSpawnedChassisRefFormID = 0;
    }
    if (proxy->m_paRealFrameRefFormID != 0)
    {
        if (TESObjectREFR* realFrame = SafeLookupAndCastRefr(proxy->m_paRealFrameRefFormID))
        {
            CoSyncGameAPI::EnableWorldActor(realFrame);
            LOG_UPDATED("[PASync] Proxy entity=%u removed while in PA: re-showed real frame 0x%08X",
                proxy->entityID, proxy->m_paRealFrameRefFormID);
        }
        proxy->m_paRealFrameRefFormID = 0;
    }
    proxy->m_isInPowerArmor = false;
    memset(proxy->m_paAppliedPieces, 0, sizeof(proxy->m_paAppliedPieces));
}

// -- Shared host save: the host's Power Armor on a joining client --------------
// A client plays in a copy of the HOST's save. If the host was wearing Power
// Armor when it saved, the client loads in wearing it (2026-09-26 test): its
// fresh character was built inside the host's frame. These let Profiles eject
// the client first and hide that frame (the host's proxy wears it).
bool CoSyncPowerArmorSync::IsLocalPlayerInPowerArmor()
{
    PlayerCharacter* pc = CoSyncGetPlayer();
    return pc && SafeActorInPowerArmorByRace(reinterpret_cast<Actor*>(pc));
}

uint32_t CoSyncPowerArmorSync::ExitLocalPlayerPowerArmor()
{
    PlayerCharacter* pc = CoSyncGetPlayer();
    if (!pc)
        return 0;
    Actor* me = reinterpret_cast<Actor*>(pc);
    TESObjectREFR* frame = SafeResolveHandle(SafeGetFurnitureHandle(me));
    if (frame && !SafeIsRefPowerArmorFrame(frame))
        frame = nullptr;
    const bool ok = SafeSwitchFromPowerArmorNative(me);
    LOG_INFO("[PASync] Leaving the Power Armor this world was saved in (frame ref=0x%08X, eject %s)",
        frame ? frame->formID : 0, ok ? "issued" : "FAILED");
    return frame ? frame->formID : 0;
}
