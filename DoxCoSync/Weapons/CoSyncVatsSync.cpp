#include "CoSyncVatsSync.h"
#include "ConsoleLogger.h"
#include "CoSyncTransport.h"
#include "CoSyncEntityRegistry.h"
#include "CoSyncAnimationUtils.h"

#include "GameCamera.h"
#include "Relocation.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameFormComponents.h"

#include "CoSyncActorValues.h"

#include <mutex>
#include <vector>
#include <unordered_set>
#include <string>
#include <cstdio>
#include <cstring>

enum class VatsEventType { Enter, Exit };

struct VatsOp
{
    uint32_t     entityID;
    VatsEventType type;
    float        apCurrent;
};

static std::mutex           s_mutex;
static std::vector<VatsOp>  s_pending;

static std::mutex                    s_vatsMutex;
static std::unordered_set<uint32_t>  s_inVats;

static bool    s_wasInVats     = false;
static float   s_lastSentAP    = -1.f;
static uint32_t s_enterCount   = 0;
static uint32_t s_exitCount    = 0;
static double  s_lastEnterTime = 0.0;

static ActorValueInfo* s_apAVI = nullptr;

static ActorValueInfo* GetApAVI()
{
    if (!s_apAVI)
    {
        TESForm* form = LookupFormByID(kAVI_ActionPoints);
        if (form)
        {
            s_apAVI = static_cast<ActorValueInfo*>(form);
            LOG_UPDATED("[VatsSync] Cached AP ActorValueInfo* = %p (formID=0x%08X)",
                        static_cast<void*>(s_apAVI), kAVI_ActionPoints);
        }
        else
        {
            LOG_UPDATED_WARN("[VatsSync] LookupFormByID(0x%08X) returned null — AP AVI not available",
                             kAVI_ActionPoints);
        }
    }
    return s_apAVI;
}


static float ReadLocalAP(Actor* player)
{
    ActorValueInfo* avi = GetApAVI();
    if (!avi || !player)
        return 0.f;
    return player->actorValueOwner.GetValue(avi);
}

bool CoSyncVatsSync::IsVatsEnter(const std::string& msg)
{

    return msg.size() > 10 &&
           msg[0]=='V' && msg[1]=='A' && msg[2]=='T' && msg[3]=='S' &&
           msg[4]=='_' && msg[5]=='E' && msg[6]=='N' && msg[7]=='T' &&
           msg[8]=='E' && msg[9]=='R' && msg[10]=='|';
}

bool CoSyncVatsSync::IsVatsExit(const std::string& msg)
{

    return msg.size() > 9 &&
           msg[0]=='V' && msg[1]=='A' && msg[2]=='T' && msg[3]=='S' &&
           msg[4]=='_' && msg[5]=='E' && msg[6]=='X' && msg[7]=='I' &&
           msg[8]=='T' && msg[9]=='|';
}

std::string CoSyncVatsSync::SerializeVatsEnter(uint32_t entityID, float apCurrent)
{
    char buf[80];
    std::snprintf(buf, sizeof(buf), "VATS_ENTER|%u|%.2f", entityID, apCurrent);
    return buf;
}

std::string CoSyncVatsSync::SerializeVatsExit(uint32_t entityID, float apCurrent)
{
    char buf[80];
    std::snprintf(buf, sizeof(buf), "VATS_EXIT|%u|%.2f", entityID, apCurrent);
    return buf;
}

void CoSyncVatsSync::OnReceiveVatsEnter(const std::string& msg)
{

    uint32_t eid = 0;
    float ap = 0.f;
    if (std::sscanf(msg.c_str() + 11, "%u|%f", &eid, &ap) < 1)
    {
        LOG_UPDATED_WARN("[VatsSync RX] Malformed VATS_ENTER packet: '%s'", msg.c_str());
        return;
    }

    LOG_UPDATED("[VatsSync RX] VATS_ENTER queued — entity=%u AP=%.1f  raw='%s'",
                eid, ap, msg.c_str());

    {
        std::lock_guard<std::mutex> lk(s_mutex);
        s_pending.push_back({ eid, VatsEventType::Enter, ap });
    }
}

void CoSyncVatsSync::OnReceiveVatsExit(const std::string& msg)
{

    uint32_t eid = 0;
    float ap = 0.f;
    if (std::sscanf(msg.c_str() + 10, "%u|%f", &eid, &ap) < 1)
    {
        LOG_UPDATED_WARN("[VatsSync RX] Malformed VATS_EXIT packet: '%s'", msg.c_str());
        return;
    }

    LOG_UPDATED("[VatsSync RX] VATS_EXIT  queued — entity=%u AP=%.1f  raw='%s'",
                eid, ap, msg.c_str());

    {
        std::lock_guard<std::mutex> lk(s_mutex);
        s_pending.push_back({ eid, VatsEventType::Exit, ap });
    }
}

void CoSyncVatsSync::FlushPendingVatsOps()
{
    std::vector<VatsOp> ops;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        ops.swap(s_pending);
    }
    if (ops.empty())
        return;

    LOG_UPDATED("[VatsSync FLUSH] Processing %u VATS op(s) this frame", (unsigned)ops.size());

    for (const VatsOp& op : ops)
    {
        const char* eventName = (op.type == VatsEventType::Enter) ? "ENTER" : "EXIT";


        {
            std::lock_guard<std::mutex> lk(s_vatsMutex);
            if (op.type == VatsEventType::Enter)
                s_inVats.insert(op.entityID);
            else
                s_inVats.erase(op.entityID);
        }

        Actor* actor = g_CoSyncEntities.GetActor(op.entityID);
        if (!actor)
        {
            LOG_UPDATED_WARN("[VatsSync FLUSH] VATS_%s — entity=%u actor not found in registry"
                             " (proxy not spawned yet or already despawned)",
                             eventName, op.entityID);
        }
        else
        {

            const char* bgEvent = (op.type == VatsEventType::Enter)
                ? CoSyncAnimUtils::Events::VATSStart
                : CoSyncAnimUtils::Events::VATSEnd;
            CoSyncAnimUtils::SendEvent(actor, bgEvent);
            LOG_UPDATED("[VatsSync FLUSH] VATS_%s APPLIED — entity=%u actor=%p AP=%.1f event='%s'",
                        eventName, op.entityID, static_cast<void*>(actor), op.apCurrent, bgEvent);
        }


        {
            std::lock_guard<std::mutex> lk(s_vatsMutex);
            LOG_UPDATED("[VatsSync FLUSH] Entities currently in VATS: %u total",
                        (unsigned)s_inVats.size());
        }
    }
}

bool CoSyncVatsSync::IsEntityInVats(uint32_t entityID)
{
    std::lock_guard<std::mutex> lk(s_vatsMutex);
    return s_inVats.count(entityID) > 0;
}

// UPDATE 2026-06-20 (round 46): the 2026-06-17 comment below (kept for
// history, now corrected) claimed s_playerCamera's RVA (0x030DBD58) was
// "re-checked via IDA" and correct. It was not -- this is the exact same
// IDA-vs-live-binary mismatch pattern that's bitten this investigation
// repeatedly (PlaceAtMe_Native x3, CallFunctionNoWait_Internal,
// s_IsMenuOpen, MoveRefrToPosition, both Equip/UnequipObject RVAs). Proof:
// f4se-0.7.8/f4se/GameCamera.cpp defines the REAL, actively-used global as
// `RelocPtr<PlayerCamera*> g_playerCamera(0x030DBDD8);` -- already used
// successfully elsewhere in the same compiled SDK (Hooks_Camera.cpp,
// PapyrusGame.cpp). s_playerCamera's 0x030DBD58 is 0x80 (128 bytes) below
// that -- a different static slot entirely, holding small handle-shaped
// garbage instead of a PlayerCamera*. The math closes exactly: every one of
// the 47 repeating crashes in the round-46 log read cam=0x80000215 (not a
// real heap pointer) and faulted at exactly cam+0xF0=0x80000305 (0xF0 is
// cameraStates[kCameraState_VATS]'s byte offset, 0xE0 + 2*8) -- the AV fault
// address recorded by the VEH (0x80000305) matches this computation
// bit-for-bit, not a coincidence. This was never a "camera not initialized
// yet" race (the 36-second-long, never-resolving recurrence in the round-46
// log already contradicted that theory) -- it was reading the wrong global
// on every single tick, forever. Fixed by deleting the duplicate local
// declaration and using GameCamera.h's existing, already-correct
// `g_playerCamera` extern directly instead of maintaining a second,
// error-prone copy of the same pointer (same precedent as the g_player /
// CoSyncGetPlayer() fix in CoSyncGameAPI.cpp).
//
// Original 2026-06-17 comment (now superseded, kept for history): re-checked
// s_playerCamera's RVA (0x030DBD58) via IDA against Fallout4.exe 1.11.221 --
// it's actually correct. The exact same global (qword_1430DBD58) is read the
// exact same way (one dereference, value IS the PlayerCamera*) by the real
// "Game.ForceFirstPerson"/"ForceThirdPerson" Papyrus natives
// (sub_1411299A0/B0). The `cameraStates` array offset (0xE0/224) is also
// independently confirmed -- it matches both the existing static_assert in
// GameCamera.h and a fresh IDA trace of those same native impls. So the
// address and the field layout were never actually wrong. The previously
// observed crash (AV on a small near-null "pointer") was most likely a
// one-time read race rather than a structural bug -- e.g. reading this
// before the camera system has fully initialized early in a session. Kept
// the SEH-guard + latch in place as a safety net regardless (same pattern as
// SafeReadVoidPtr). Kept as its own function because __try/__except can't
// share a frame with objects needing unwinding.
// UPDATE 2026-06-17: __declspec(noinline) is load-bearing, not a perf hint
// -- a non-noinline static __try/__except function can be silently
// inlined away with its SEH exception-dispatch data dropped under this
// project's /GL+/LTCG Release config (confirmed via direct .pdata/.xdata
// parsing of a compiled DLL for an unrelated logging crash this session,
// and again for the equivalent SafeReadU32/SafeReadPtr pattern in
// CoSyncDoorHook.cpp/CoSyncPickupHook.cpp/CoSyncLockHook.cpp).
__declspec(noinline) static bool SafeReadVatsCameraState(PlayerCamera* cam, TESCameraState*& vatsState, TESCameraState*& activeState)
{
    __try
    {
        vatsState   = cam->cameraStates[PlayerCamera::kCameraState_VATS];
        activeState = cam->cameraState;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void CoSyncVatsSync::TickLocalVats(uint32_t myEntityID)
{
    // UPDATE 2026-06-17: was permanently disabling VATS sync after the FIRST
    // SEH-caught failure, on the theory the RVA was stale garbage. It isn't
    // (see SafeReadVatsCameraState's comment above -- independently
    // reconfirmed twice now) -- the actual cause is PlayerCamera not being
    // fully initialized yet this early in a session, a one-time transient
    // race, not a permanent condition. Permanently latching off the first
    // failure meant VATS sync silently never recovered for the rest of the
    // session even once the camera became valid moments later. Now retries
    // every tick (so it recovers once the camera initializes) but only
    // logs the error once, since the global VEH still logs every
    // first-chance exception regardless of the SEH catch -- the log-spam
    // concern was about logging, not about retrying being unsafe.
    static bool s_loggedCameraFailureOnce = false;

    PlayerCamera* cam = *g_playerCamera;
    if (!cam)
    {
        LOG_TRACE("[VatsSync TICK] PlayerCamera ptr is null — skipping VATS check");
        return;
    }

    TESCameraState* vatsState   = nullptr;
    TESCameraState* activeState = nullptr;
    if (!SafeReadVatsCameraState(cam, vatsState, activeState))
    {
        if (!s_loggedCameraFailureOnce)
        {
            s_loggedCameraFailureOnce = true;
            LOG_ERROR("[VatsSync TICK] PlayerCamera read failed (likely camera "
                       "not initialized yet) — will keep retrying, not logging again");
        }
        return;
    }
    s_loggedCameraFailureOnce = false;

    bool inVats = (vatsState != nullptr) && (activeState == vatsState);


    if (inVats == s_wasInVats)
        return;

    s_wasInVats = inVats;

    float apNow = 0.f;
    {

        Actor* myActor = g_CoSyncEntities.GetActor(myEntityID);
        if (myActor)
            apNow = ReadLocalAP(myActor);
    }

    if (inVats)
    {
        ++s_enterCount;
        s_lastEnterTime = 0.0;

        LOG_SECTION("VATS ENTER");
        LOG_UPDATED("[VatsSync TX] *** VATS ACTIVATED ***");
        LOG_UPDATED("[VatsSync TX] entityID=%u  AP=%.1f  enterCount=%u",
                    myEntityID, apNow, s_enterCount);
        LOG_UPDATED("[VatsSync TX] cameraState=%p  vatsSlot=%p  match=%s",
                    static_cast<void*>(activeState),
                    static_cast<void*>(vatsState),
                    (activeState == vatsState) ? "YES" : "NO");

        std::string pkt = SerializeVatsEnter(myEntityID, apNow);
        LOG_UPDATED("[VatsSync TX] Sending packet: '%s'", pkt.c_str());
        CoSyncTransport::Send(pkt);
    }
    else
    {
        ++s_exitCount;

        LOG_SECTION("VATS EXIT");
        LOG_UPDATED("[VatsSync TX] *** VATS DEACTIVATED ***");
        LOG_UPDATED("[VatsSync TX] entityID=%u  AP=%.1f  exitCount=%u  (enterCount was %u)",
                    myEntityID, apNow, s_exitCount, s_enterCount);

        std::string pkt = SerializeVatsExit(myEntityID, apNow);
        LOG_UPDATED("[VatsSync TX] Sending packet: '%s'", pkt.c_str());
        CoSyncTransport::Send(pkt);
    }
}
