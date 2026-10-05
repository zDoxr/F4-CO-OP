#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncDoorHook.h"
#include "CoSyncSharedDialogue.h"

#include "CoSyncRequirementsLog.h"
#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncThrowableSync.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncReviveSync.h"
#include "CoSyncPowerArmorSync.h"
#include "CoSyncPickpocket.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameRTTI.h"

#include "MinHook.h"

#include <atomic>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include "CoSyncThreadDiag.h"

extern RelocAddr<_LookupFormByID> LookupFormByID;

// RVA from FO4_Wrld re/ dossier (B6 wedge 1, FO4 1.11.191 NG).
// sub_140514180 = "Activate worker" — called for ALL activations.
// Engine SET_OPEN_STATE = sub_140305760 — receiver-side apply (save-load applier,
// safe to call directly to force door state on the receiving client).
//
// RVA RE-VERIFIED 2026-07-21 against Fallout4_disk.i64: round 93's "revert to
// dossier" call above was itself wrong, and round 129 flagged both of these
// as suspect but they were never actually re-derived until now (unlike
// CoSyncPickupHook.cpp's identical bug, fixed in round 133/2026-07-15).
// Direct check: both 0x514180 and 0x305760 land in 0xCC (int3) padding, not
// a function head. Scanning forward past the padding lands on real function
// heads at +0x10 in both cases — is_func_head=YES, 59 and 26 callers
// respectively, and 0x514190's decompiled signature (7 args: refr,
// activator, a3..a7) matches this file's existing ActivateWorkerFn exactly.
// This means the door hook has installed at padding and NEVER FIRED for the
// entire project history — door sync has never actually worked.
static constexpr uintptr_t kActivateWorker_RVA  = 0x005144B0;
static constexpr uintptr_t kSetOpenState_RVA    = 0x00305A90;

// Door-like form types (from FO4_Wrld door_hook.cpp field notes).
static constexpr bool is_door_like(uint8_t ftype) noexcept
{
    return ftype == 0x1F || ftype == 0x20 || ftype == 0x24 || ftype == 0x29;
}

thread_local bool tls_cs_door_applying = false;
// >0 while a local activation runs (its door change is replicated as DOOR).
static thread_local int tls_cs_door_in_activate = 0;

using ActivateWorkerFn = char(__fastcall*)(
    void* refr, void* activator, void* a3, void* a4,
    void* a5, void* a6, void* a7);

using SetOpenStateFn = void(__fastcall*)(void* refr);

static ActivateWorkerFn g_origActivate   = nullptr;
static SetOpenStateFn   g_fnSetOpenState = nullptr;
static std::atomic<bool> g_hookInstalled{ false };

// ============================================================================
// Helpers
// ============================================================================

// UPDATE 2026-06-17: __declspec(noinline) is load-bearing, not a perf hint
// -- a non-noinline static __try/__except function can be fully inlined
// into its caller with its SEH exception-dispatch data silently dropped
// under this project's /GL+/LTCG Release config. Confirmed (via direct
// .pdata/.xdata parsing of a compiled DLL) for an unrelated logging crash
// earlier this session: the resolved exception handler for the caller was
// a plain /GS cookie check, not __C_specific_handler, meaning the __try
// never actually existed in the binary's exception-dispatch tables. Same
// risk applies here since these are also small `static` SEH helpers.
//
// UPDATE 2026-06-17 (second pass): noinline alone was NOT enough --
// disassembled the compiled binary and found these still had ZERO
// protected scope: just the raw load followed by a jump straight to
// `retn`, with the `return 0`/`return nullptr` fallback left behind as
// genuinely unreachable dead code. Removing `noexcept` fixed it -- a
// function marked `noexcept` is a direct contradiction with wrapping its
// only operation in `__try/__except (EXCEPTION_EXECUTE_HANDLER)` (the
// entire point of which is "this might fault"), and the optimizer was
// exploiting that contradiction to eliminate the guard entirely. Verified
// via the same .pdata/.xdata parse that these now resolve to
// `__C_specific_handler` with a real scope record after the fix.
__declspec(noinline) static uint32_t SafeReadU32(const void* base, size_t off)
{
    LOG_REQUIREMENT("(global)::SafeReadU32", "Safe even with a bad pointer or offset -- returns 0 instead of crashing.");
    __try {
        return *reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(base) + off);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

__declspec(noinline) static uint8_t SafeReadU8(const void* base, size_t off)
{
    LOG_REQUIREMENT("(global)::SafeReadU8", "Safe even with a bad pointer or offset -- returns 0xFF instead of crashing.");
    __try {
        return *reinterpret_cast<const uint8_t*>(
            reinterpret_cast<const uint8_t*>(base) + off);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFF; }
}

__declspec(noinline) static void* SafeReadPtr(const void* base, size_t off)
{
    LOG_REQUIREMENT("(global)::SafeReadPtr", "Safe even with a bad pointer or offset -- returns null instead of crashing.");
    __try {
        return *reinterpret_cast<void* const*>(
            reinterpret_cast<const uint8_t*>(base) + off);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Read refFormID (TESObjectREFR+0x14) and formType of its baseForm.
// Returns {refFormID, formType} or {0, 0xFF} on fault.
struct RefrInfo { uint32_t refFormID; uint8_t formType; };

static RefrInfo ReadRefrInfo(void* refr) noexcept
{
    LOG_REQUIREMENT("(global)::ReadRefrInfo", "Safe with a null reference (returns a blank result). Relies on the safe-read helpers above for anything else invalid.");
    RefrInfo out{ 0, 0xFF };
    if (!refr) return out;
    out.refFormID = SafeReadU32(refr, 0x14);
    void* base = SafeReadPtr(refr, 0xE0); // BASE_FORM_OFF
    if (base)
        out.formType = SafeReadU8(base, 0x1A); // FORMTYPE_OFF
    return out;
}

// ============================================================================
// Pending receive queue
// ============================================================================

namespace
{
    std::mutex            s_mutex;
    std::vector<uint32_t> s_pending; // refFormIDs of doors to activate
}

// ============================================================================
// Detour
// ============================================================================

extern PlayerCharacter* CoSyncGetPlayer();

static char __fastcall Detour_ActivateWorker(
    void* refr, void* activator, void* a3, void* a4,
    void* a5, void* a6, void* a7)
{
    LOG_REQUIREMENT("(global)::Detour_ActivateWorker", "Needs the hook to already be installed so it can call the real game function. Broadcasting the door also needs an active connection.");
    CoSyncThreadDiag::Note(CoSyncThreadDiag::kDoorActivate);
    // round 94 diagnostic: unconditional fire-heartbeat, confirms the
    // round-93 RVA fix (kActivateWorker_RVA) is reached. This fires for
    // EVERY activation in the game (not just doors), so it should appear
    // constantly during normal play -- a strong, easy signal.
    {
        static std::atomic<uint32_t> s_n{ 0 };
        const uint32_t n = ++s_n;
        if (n <= 5 || n % 500 == 0)
            LOG_INFO("[DoorHook] Detour_ActivateWorker fired #%u refr=%p", n, refr);
    }

    if (tls_cs_door_applying)
        return g_origActivate ? g_origActivate(refr, activator, a3, a4, a5, a6, a7) : 0;

    const RefrInfo ri = ReadRefrInfo(refr);

    // ── Throwable stand-ins are scenery, not loot ───────────────────────────
    // The in-flight grenade/missile a peer sees is a real WEAP/AMMO item ref
    // (that is what gives it the correct model), so the engine would happily
    // let someone pick it out of the air, or loot one that outlived its
    // delete. Refuse activation on anything CoSyncThrowableSync spawned.
    if (ri.refFormID != 0 && CoSyncThrowableSync::IsStandInRef(ri.refFormID))
    {
        static std::atomic<uint32_t> s_n{ 0 };
        const uint32_t n = ++s_n;
        if (n <= 5 || n % 50 == 0)
            LOG_INFO("[ThrowSync] blocked activation #%u of in-flight stand-in ref=0x%08X",
                n, ri.refFormID);
        return 0;
    }

    // ── 0.3.6: Power Armor ownership ────────────────────────────────────────
    // The first player to get into a frame owns it (host-authoritative, see
    // CoSyncPowerArmorSync.h). Anyone else can see it but not enter it.
    if (ri.refFormID != 0 && activator &&
        activator == static_cast<void*>(CoSyncGetPlayer()) &&
        CoSyncPowerArmorSync::ShouldBlockLocalEntry(static_cast<TESObjectREFR*>(refr)))
    {
        LOG_UPDATED("[PASync] PA Enter refused: frame ref=0x%08X is owned by another player", ri.refFormID);
        return 0;
    }

    // ── Revive: activating a downed player's proxy ──────────────────────────
    // Checked BEFORE the client-dialogue gate below, which otherwise blocks a
    // CLIENT from activating any NPC-formType ref at all -- a downed teammate
    // must be revivable by clients too, not just the host. formType 45 also
    // covers dead/inanimate actor refs (see that block's own comment), so
    // this only ever matches something CoSyncReviveSync itself marked
    // isDowned -- an ordinary corpse or live NPC is untouched.
    if (ri.refFormID != 0 && ri.formType == 45 && CoSyncNet::IsConnected())
    {
        CoSyncPlayer* target = g_CoSyncPlayerManager.GetPlayerByActorFormID(ri.refFormID);
        if (target && target->isDowned)
        {
            CoSyncReviveSync::BeginReviveAttempt(target->entityID);
            return 0; // suppress default activation -- no dialogue/loot on a downed player
        }

        // ── 0.5.0: another player is never "talked to" ──────────────────────
        // Sneaking: the pickpocket exchange with that player's machine
        // (CoSyncPickpocket), which opens the engine's own pickpocket menu
        // once the victim's machine has said what can be taken. Otherwise
        // the activation does nothing (no CoSync "Talk").
        if (target && !target->isLocalPlayer && target->createType == CoSyncEntityType::Player &&
            activator == static_cast<void*>(CoSyncGetPlayer()))
        {
            if (CoSyncPickpocket::IsPlayerSneaking())
                CoSyncPickpocket::BeginAttempt(target->entityID);
            return 0;
        }
    }

    // ── CLIENT: no control over dialogue ────────────────────────────────────
    // The host owns the story. Quest stages and objectives are already
    // suppressed on clients (CoSyncQuestHook / CoSyncQuestObjectiveHook), but
    // that only stops the CONSEQUENCE -- a client could still open dialogue
    // and drive scenes the host never sees. Blocking here stops it at the
    // source: no activation, so the DialogueMenu never opens and there is
    // nothing to select.
    //
    // Done at this hook deliberately. The alternative -- letting the menu open
    // and force-closing it -- needs UIMessageManager::SendUIMessage, whose
    // f4se 0.7.8 address (0x01A89170) targets runtime 1.10.163 while this
    // project runs 1.11.221; unverified RVAs are exactly what has burned this
    // codebase repeatedly. This hook (kActivateWorker_RVA) is already
    // installed and proven for door sync.
    //
    // KNOWN COST: base formType 45 is TESNPC, which covers DEAD actors too, so
    // this also blocks a client looting corpses and interacting with their own
    // companion. Narrowing it to live actors needs a life-state read we do not
    // have a verified offset for yet -- deliberately not guessed here.
    if (ri.refFormID != 0
        && ri.formType == 45              // kFormType_NPC_ (an actor's base)
        && CoSyncNet::IsConnected()
        && !CoSyncNet::IsHost())
    {
        static std::atomic<uint32_t> s_blocked{ 0 };
        const uint32_t n = ++s_blocked;
        if (n <= 10 || n % 50 == 0)
            LOG_INFO("[DialogGate] [CLIENT] blocked activation #%u of actor ref=0x%08X "
                     "(host owns dialogue)", n, ri.refFormID);
        // 0.3.5: tell the host who wants to talk to whom; the host's
        // conversation is then shared with this client (CoSyncSharedDialogue).
        CoSyncSharedDialogue::OnClientTalkAttempt(ri.refFormID);
        return 0;
    }
    // ── Run the real activation FIRST, then broadcast only if it worked ─────
    // 2026-08-04 (list item #6): this used to send DOOR| BEFORE calling the
    // original, with no check of whether the door was locked, jammed, owned by
    // a quest, or otherwise refusing to open. Bumping into a locked door
    // therefore told every peer to toggle it, so their copy swung open while
    // the local one stayed shut -- and the two sides disagreed from then on.
    //
    // The engine's own return value already encodes "did this activation
    // actually happen", covering locked AND every other refusal reason without
    // needing to special-case them. Gate on that instead of trying to
    // enumerate the failure modes.
    ++tls_cs_door_in_activate;
    const char actResult = g_origActivate
        ? g_origActivate(refr, activator, a3, a4, a5, a6, a7)
        : 0;
    --tls_cs_door_in_activate;

    if (ri.refFormID != 0 && is_door_like(ri.formType) && CoSyncNet::IsConnected())
    {
        // Belt and braces: also consult the lock flag directly. A door can be
        // reported "activated" while a script re-locks it, and a locked door
        // must never be replayed as an open on a peer.
        const bool locked = CoSyncDoorHook::IsRefLocked(refr);

        if (actResult != 0 && !locked)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "DOOR|%u", ri.refFormID);
            CoSyncTransport::Send(std::string(buf));

            static std::atomic<uint32_t> s_n{ 0 };
            const uint32_t n = ++s_n;
            if (n <= 20 || n % 200 == 0)
                LOG_INFO("[DoorHook] TX DOOR #%u refFormID=0x%08X ftype=0x%02X",
                    n, ri.refFormID, ri.formType);
        }
        else
        {
            static std::atomic<uint32_t> s_skipped{ 0 };
            const uint32_t n = ++s_skipped;
            if (n <= 10 || n % 50 == 0)
                LOG_UPDATED("[DoorHook] NOT broadcasting door 0x%08X — activation "
                            "result=%d locked=%d (refused locally, must not open on peers)",
                    ri.refFormID, (int)actResult, locked ? 1 : 0);
        }
    }

    return actResult;
}

// ============================================================================
// Public API
// ============================================================================

void InstallDoorHook()
{
    LOG_REQUIREMENT("(global)::InstallDoorHook", "Needs the game module to resolve and its known internal addresses to be correct. Meant to run once before door syncing can work.");
    if (g_hookInstalled.exchange(true))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* activateTarget = reinterpret_cast<void*>(base + kActivateWorker_RVA);

    g_fnSetOpenState = reinterpret_cast<SetOpenStateFn>(base + kSetOpenState_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[DoorHook] MH_Initialize failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_CreateHook(activateTarget,
        reinterpret_cast<void*>(&Detour_ActivateWorker),
        reinterpret_cast<void**>(&g_origActivate));
    if (st != MH_OK)
    {
        LOG_ERROR("[DoorHook] MH_CreateHook(ActivateWorker) failed: %d target=%p", (int)st, activateTarget);
        g_hookInstalled.store(false);
        return;
    }

    MH_EnableHook(activateTarget);
    LOG_INFO("[DoorHook] Installed at %p (RVA 0x%zX)", activateTarget, kActivateWorker_RVA);
}

void UninstallDoorHook()
{
    LOG_REQUIREMENT("(global)::UninstallDoorHook", "Needs the hook to already be installed. Recomputes the same address to remove it cleanly.");
    if (!g_hookInstalled.exchange(false))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kActivateWorker_RVA);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    LOG_INFO("[DoorHook] Uninstalled");
}

// Lock-data accessor, same entry point CoSyncLockHook uses. Resolved lazily so
// this file does not depend on that hook having been installed first.
namespace
{
    using LockDataGetFn = void* (__fastcall*)(void* refr);
    // TESObjectREFR -> REFR_LOCK* (sub_1405634A0, IDA 1.11.240 2026-09-26);
    // the old 0x563490 was int3 padding on this build. Same value as
    // CoSyncLockHook's kLockDataGet_RVA.
    constexpr uintptr_t kLockDataGet_RVA_Local = 0x005634A0;
    constexpr size_t    kLockData_FlagsOff     = 0x10;
    constexpr uint8_t   kLockFlag_Locked       = 0x01;
}

__declspec(noinline) static char ActivateAsImpl(TESObjectREFR* target, TESObjectREFR* activator)
{
    __try
    {
        tls_cs_door_applying = true;
        const char r = g_origActivate(target, activator, nullptr, reinterpret_cast<void*>(1), nullptr, nullptr, nullptr);
        tls_cs_door_applying = false;
        return r;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        tls_cs_door_applying = false;
        return 0;
    }
}

bool CoSyncDoorHook::ActivateAs(TESObjectREFR* target, TESObjectREFR* activator)
{
    if (!g_origActivate || !target || !activator)
        return false;
    return ActivateAsImpl(target, activator) != 0;
}

bool CoSyncDoorHook::InLocalActivation()
{
    return tls_cs_door_in_activate > 0 || tls_cs_door_applying;
}

bool CoSyncDoorHook::IsRefLocked(void* refr)
{
    LOG_REQUIREMENT("CoSyncDoorHook::IsRefLocked", "Safe with any reference including null. Reports 'not locked' on any failure so a crash can never block a legitimate door broadcast.");
    if (!refr) return false;

    static LockDataGetFn s_fn = nullptr;
    if (!s_fn)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
        if (!base) return false;
        s_fn = reinterpret_cast<LockDataGetFn>(base + kLockDataGet_RVA_Local);
    }

    __try
    {
        void* ld = s_fn(refr);
        if (!ld) return false;   // no lock data at all == not lockable
        const uint8_t flags = *reinterpret_cast<const uint8_t*>(
            reinterpret_cast<const uint8_t*>(ld) + kLockData_FlagsOff);
        return (flags & kLockFlag_Locked) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool CoSyncDoorHook::IsDoork(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncDoorHook::IsDoork", "Just checks the start of a text message, no other requirement.");
    return msg.rfind("DOOR|", 0) == 0;
}

void CoSyncDoorHook::OnReceiveDoor(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncDoorHook::OnReceiveDoor", "Needs the message to be at least as long as its tag prefix. Bad or blank ID values are rejected.");
    const char* s = msg.c_str() + 5; // skip "DOOR|"
    char* e = nullptr;
    uint32_t refFormID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (e == s || refFormID == 0) return;

    LOG_UPDATED("[DoorHook] RX DOOR refFormID=0x%08X — queued", refFormID);
    std::lock_guard<std::mutex> lk(s_mutex);
    s_pending.push_back(refFormID);
}

// UPDATE 2026-06-19: confirmed live via TTD trace -- LookupFormByID(refFormID)
// below was called completely unguarded with refFormID sourced directly from
// the network ("DOOR|<id>" sent by the peer), and a bad/stale ID drove the
// engine into the same 0x1A7FC54 generic-dispatch crash chased across
// project_cosync_crash_fixes.md rounds 9-38 (that routine is reachable from
// many unrelated callers, not just UI/menu teardown -- this is the first
// confirmed alternate path into it). Matches the existing
// SafeLookupAndCastPlayer pattern in GameGlobals.cpp: noinline is load-bearing
// (an SEH scope can be inlined away under /GL+/LTCG) and noexcept must NOT be
// added (see this file's own UPDATE comments above -- noexcept lets the
// optimizer prove the __try can never fire and strips it).
__declspec(noinline) static TESObjectREFR* SafeLookupAndCastDoorRefr(uint32_t refFormID)
{
    LOG_REQUIREMENT("(global)::SafeLookupAndCastDoorRefr", "Safe with any form ID, including garbage values from a peer's packet. Returns null on failure, not found, or the wrong kind of object.");
    __try
    {
        TESForm* form = LookupFormByID(refFormID);
        // 2026-07-04: DYNAMIC_CAST removed — minidump-confirmed broken RTTI
        // (see CoSyncPlayerManager.cpp PumpDeferredSpawns_Impl); an engine-side
        // SEH frame terminates the process before our __except runs. formType
        // check by engine contract: REFR (64) / ACHR (65) are TESObjectREFR.
        if (form && (form->formType == kFormType_REFR || form->formType == kFormType_ACHR))
            return static_cast<TESObjectREFR*>(form);
        return nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// SEH isolated: no C++ objects in scope — avoids C2712.
static void DoActivateRefr(void* refr) noexcept
{
    LOG_REQUIREMENT("(global)::DoActivateRefr", "Needs the original game function and a valid reference. The call itself is wrapped in a safety net in case the reference turns out bad.");
    if (!g_origActivate || !refr) return;
    tls_cs_door_applying = true;
    __try {
        g_origActivate(refr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    tls_cs_door_applying = false;
}

void CoSyncDoorHook::FlushPendingDoorOps()
{
    LOG_REQUIREMENT("CoSyncDoorHook::FlushPendingDoorOps", "Safe to call with nothing queued -- just does nothing. Real requirements live in the lookup/activate helpers it calls.");
    std::vector<uint32_t> ops;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_pending.empty()) return;
        ops.swap(s_pending);
    }

    for (uint32_t refFormID : ops)
    {
        TESObjectREFR* refr = SafeLookupAndCastDoorRefr(refFormID);
        if (!refr)
        {
            LOG_WARN("[DoorHook] FLUSH refFormID=0x%08X not found or not a REFR", refFormID);
            continue;
        }

        DoActivateRefr(static_cast<void*>(refr));
        LOG_UPDATED("[DoorHook] FLUSH applied refFormID=0x%08X", refFormID);
    }
}
