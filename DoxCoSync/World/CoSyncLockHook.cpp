#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncLockHook.h"

#include "CoSyncRequirementsLog.h"
#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"

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

// RVAs from FO4_Wrld offsets.h (B6.3 lock state sync, FO4 1.11.191 NG).
//
// RVA CORRECTED round 93 (IDA cross-check, ida_funcs.get_func + Hex-Rays):
// round 40's "+0x10" fix below was itself wrong -- it's the same
// disassembly-desync defect found project-wide in round 90/93 (a raw
// Capstone re-disasm at the +0x10 offset "looks like" a clean prologue --
// push rbx; sub rsp,0x20 -- but that's not sufficient evidence it's the
// real entry; IDA's control-flow-validated function boundaries show the
// ORIGINAL un-offset FO4_Wrld dossier values were the true entries all
// along). Reverting to the dossier's un-offset addresses.
// Round 129: LIVE-CONFIRMED (in-process [LiveBytes] dump 21:41 + fresh
// disk-exe IDB Fallout4_disk.i64). The old dump-based IDB is skewed by +0x10
// in this region: the real 0x31-byte twin functions start at 0x563330 and
// 0x563370 (live bytes: `40 53 48 83 EC 20 48 8B D9 E8 ...` at both). The
// round-128 values (0x563320/0x563360) were the dump-IDB's skewed addresses —
// live bytes there are a function tail and lone-ret padding respectively.
static constexpr uintptr_t kForceUnlock_RVA      = 0x00563650;
static constexpr uintptr_t kForceLock_RVA        = 0x00563690;
// TESObjectREFR -> REFR_LOCK* (own ExtraLock, else the linked door's).
// IDA 1.11.240, 2026-09-26: sub_1405634A0, 52 callers; returns
// sub_14027FC20(extraList) = ExtraLock(type 0x2A)+0x18. The previous
// 0x563490 was int3 padding on this build (Phase 6 RVA sweep), so every
// lock-state read raised a breakpoint exception and reported "not locked".
static constexpr uintptr_t kLockDataGet_RVA      = 0x005634A0;
// Applying a remote lock flip: the old kLockPapyrusApply_RVA (0x01158640,
// verified on 1.11.221) lands 0x30 bytes inside sub_141158610 on 1.11.240 --
// a Papyrus native that reports "is not a radio receiver". Remote flips now
// replay the engine's own ForceLock/ForceUnlock (the functions hooked above,
// called through their trampolines), exactly what the sender's engine ran.

// LockData layout (RE'd in FO4_Wrld re/ dossier).
static constexpr size_t  kLockData_FlagsOff = 0x10;
static constexpr uint8_t kLockFlag_Locked   = 0x01;

thread_local bool tls_cs_lock_applying = false;

using LockFlipFn        = void(__fastcall*)(void* refr);
using LockDataGetFn     = void*(__fastcall*)(void* refr);

static LockFlipFn        g_origForceUnlock   = nullptr;
static LockFlipFn        g_origForceLock     = nullptr;
static LockDataGetFn     g_fnLockDataGet     = nullptr;

static std::atomic<bool> g_hooksInstalled{ false };

// ============================================================================
// Helpers
// ============================================================================

// UPDATE 2026-06-17: __declspec(noinline) is load-bearing (can be silently
// inlined away with SEH dropped under /GL+/LTCG) and `noexcept` was
// removed -- combined with __try/__except, `noexcept` let the optimizer
// eliminate the protected scope entirely (confirmed via disassembly on
// the sibling copies of this exact helper in CoSyncDoorHook.cpp/
// CoSyncPickupHook.cpp: just the raw load + jmp straight to retn, no
// protected scope at all, with the fallback `return` left as dead code).
// See those files for the fuller writeup.
__declspec(noinline) static uint32_t SafeReadU32(const void* base, size_t off)
{
    LOG_REQUIREMENT("(global)::SafeReadU32", "Safe to call with a bad pointer -- wrapped in a crash guard, returns 0 if the read fails.");
    __try {
        return *reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(base) + off);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

__declspec(noinline) static uint8_t SafeReadU8(const void* base, size_t off)
{
    LOG_REQUIREMENT("(global)::SafeReadU8", "Safe to call with a bad pointer -- wrapped in a crash guard, returns 0 if the read fails.");
    __try {
        return *reinterpret_cast<const uint8_t*>(
            reinterpret_cast<const uint8_t*>(base) + off);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

__declspec(noinline) static bool VerifyLockedState(void* refr, bool expected)
{
    LOG_REQUIREMENT("(global)::VerifyLockedState", "Needs a valid ref and a resolved game function pointer. The actual read is wrapped in a crash guard.");
    if (!refr || !g_fnLockDataGet) return false;
    __try {
        void* ld = g_fnLockDataGet(refr);
        if (!ld) return false;
        const bool actual = (SafeReadU8(ld, kLockData_FlagsOff) & kLockFlag_Locked) != 0;
        return actual == expected;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ============================================================================
// Pending receive queue
// ============================================================================

namespace
{
    struct LockOp
    {
        uint32_t refFormID = 0;
        int      locked    = 0; // 1=lock, 0=unlock
    };

    std::mutex           s_mutex;
    std::vector<LockOp>  s_pending;
}

// ============================================================================
// Common detour body
// ============================================================================

static void OnLockFlip(void* refr, bool newLockedState, LockFlipFn orig,
                       std::atomic<uint64_t>& counter, const char* tag)
{
    LOG_REQUIREMENT("(global)::OnLockFlip", "Needs the original game function to actually lock/unlock the door. Sending the update to other players needs a live connection.");
    const uint64_t fire = counter.fetch_add(1, std::memory_order_relaxed) + 1;

    // round 94 diagnostic: unconditional fire-heartbeat regardless of
    // connection state, confirms the round-93 RVA fix (kForceUnlock_RVA/
    // kForceLock_RVA) is actually being reached even in singleplayer/
    // pre-connection testing (the existing "TX %s" log below only fires
    // once connected, so it can't prove the hook itself works on its own).
    if (fire <= 5 || fire % 200 == 0)
        LOG_INFO("[LockHook] %s detour fired #%llu refr=%p", tag, (unsigned long long)fire, refr);

    if (tls_cs_lock_applying)
    {
        if (orig) orig(refr);
        return;
    }

    // Run engine flip first — state is correct before we broadcast.
    if (orig)
        orig(refr);
    else
    {
        LOG_ERROR("[LockHook] %s orig NULL — engine call dropped", tag);
        return;
    }

    if (!CoSyncNet::IsConnected()) return;

    const uint32_t refFormID = SafeReadU32(refr, 0x14);
    if (refFormID == 0) return;

    // Sanity: confirm the engine actually flipped to the expected state.
    if (!VerifyLockedState(refr, newLockedState))
    {
        LOG_WARN("[LockHook] %s #%llu state mismatch for refFormID=0x%08X — skip broadcast",
            tag, (unsigned long long)fire, refFormID);
        return;
    }

    char buf[48];
    std::snprintf(buf, sizeof(buf), "LOCK|%u|%d", refFormID, newLockedState ? 1 : 0);
    CoSyncTransport::Send(std::string(buf));

    if (fire <= 20 || fire % 200 == 0)
        LOG_INFO("[LockHook] TX %s #%llu refFormID=0x%08X", tag, (unsigned long long)fire, refFormID);
}

namespace { std::atomic<uint64_t> g_unlockFires{ 0 }, g_lockFires{ 0 }; }

static void __fastcall Detour_ForceUnlock(void* refr)
{
    LOG_REQUIREMENT("(global)::Detour_ForceUnlock", "Needs the hook to already be installed so we can still reach the original game function afterward.");
    CoSyncThreadDiag::Note(CoSyncThreadDiag::kLockFlip);
    OnLockFlip(refr, false, g_origForceUnlock, g_unlockFires, "UNLOCK");
}

static void __fastcall Detour_ForceLock(void* refr)
{
    LOG_REQUIREMENT("(global)::Detour_ForceLock", "Needs the hook to already be installed so we can still reach the original game function afterward.");
    CoSyncThreadDiag::Note(CoSyncThreadDiag::kLockFlip);
    OnLockFlip(refr, true, g_origForceLock, g_lockFires, "LOCK");
}

// ============================================================================
// Public API
// ============================================================================

void InstallLockHook()
{
    LOG_REQUIREMENT("(global)::InstallLockHook", "Needs the game's .exe to be loaded and this game version's lock functions to be at the addresses this build expects. Only meant to run once, before lock syncing can work.");
    if (g_hooksInstalled.exchange(true))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));

    g_fnLockDataGet = reinterpret_cast<LockDataGetFn>(base + kLockDataGet_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[LockHook] MH_Initialize failed: %d", (int)st);
        g_hooksInstalled.store(false);
        return;
    }

    void* unlockTarget = reinterpret_cast<void*>(base + kForceUnlock_RVA);
    void* lockTarget   = reinterpret_cast<void*>(base + kForceLock_RVA);

    // Round 128: on failure, dump the live first bytes of the target — MinHook
    // status 8 (UNSUPPORTED_FUNCTION) on a clean-looking function usually
    // means the RUNTIME bytes differ from the static IDA view (another
    // plugin's hook already there, or IDB drift — round 32 confirmed this
    // exact class of mismatch in other regions).
    auto logBytes = [](const char* name, void* target)
    {
        const unsigned char* b = reinterpret_cast<const unsigned char*>(target);
        LOG_ERROR("[LockHook] %s target=%p live bytes: "
            "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            name, target,
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
            b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    };

    st = MH_CreateHook(unlockTarget,
        reinterpret_cast<void*>(&Detour_ForceUnlock),
        reinterpret_cast<void**>(&g_origForceUnlock));
    if (st != MH_OK)
    {
        LOG_ERROR("[LockHook] MH_CreateHook(ForceUnlock) failed: %d", (int)st);
        logBytes("ForceUnlock", unlockTarget);
        g_hooksInstalled.store(false);
        return;
    }
    MH_EnableHook(unlockTarget);

    st = MH_CreateHook(lockTarget,
        reinterpret_cast<void*>(&Detour_ForceLock),
        reinterpret_cast<void**>(&g_origForceLock));
    if (st != MH_OK)
    {
        LOG_ERROR("[LockHook] MH_CreateHook(ForceLock) failed: %d", (int)st);
        logBytes("ForceLock", lockTarget);
        MH_DisableHook(unlockTarget);
        MH_RemoveHook(unlockTarget);
        g_hooksInstalled.store(false);
        return;
    }
    MH_EnableHook(lockTarget);

    LOG_INFO("[LockHook] Installed: ForceUnlock@%p ForceLock@%p", unlockTarget, lockTarget);
}

void UninstallLockHook()
{
    LOG_REQUIREMENT("(global)::UninstallLockHook", "Needs InstallLockHook() to have already succeeded, so there's actually something to remove.");
    if (!g_hooksInstalled.exchange(false))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* unlockTarget = reinterpret_cast<void*>(base + kForceUnlock_RVA);
    void* lockTarget   = reinterpret_cast<void*>(base + kForceLock_RVA);

    MH_DisableHook(unlockTarget); MH_RemoveHook(unlockTarget);
    MH_DisableHook(lockTarget);   MH_RemoveHook(lockTarget);
    LOG_INFO("[LockHook] Uninstalled");
}

bool CoSyncLockHook::IsLock(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncLockHook::IsLock", "Doesn't touch the game. Just checks if a message starts with the lock prefix.");
    return msg.rfind("LOCK|", 0) == 0;
}

void CoSyncLockHook::OnReceiveLock(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncLockHook::OnReceiveLock", "Needs a properly formatted LOCK message. Anything malformed is safely rejected.");
    const char* s = msg.c_str() + 5; // skip "LOCK|"
    char* e = nullptr;

    uint32_t refFormID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (!e || *e != '|' || refFormID == 0) return;
    s = e + 1;

    int locked = static_cast<int>(std::strtol(s, &e, 10));

    LOG_UPDATED("[LockHook] RX LOCK refFormID=0x%08X locked=%d — queued", refFormID, locked);
    std::lock_guard<std::mutex> lk(s_mutex);
    s_pending.push_back({ refFormID, locked });
}

// UPDATE 2026-06-20 (round 40): same unguarded LookupFormByID/DYNAMIC_CAST
// pattern found and fixed in CoSyncDoorHook.cpp's FlushPendingDoorOps this
// session (op.refFormID is peer-controlled network input, parsed straight
// out of "LOCK|<id>|<locked>") -- a second, independent trigger into the
// same generic engine-dispatch crash family. Same fix: noinline (load-
// bearing under /GL+/LTCG) SEH wrapper, no noexcept (see SafeReadU32's
// comment above for why).
__declspec(noinline) static TESObjectREFR* SafeLookupAndCastLockRefr(uint32_t refFormID)
{
    LOG_REQUIREMENT("(global)::SafeLookupAndCastLockRefr", "Safe to call with any ref ID, even a bad one from another player -- wrapped in a crash guard, returns null if it's missing or the wrong type.");
    __try
    {
        TESForm* form = LookupFormByID(refFormID);
        return form ? DYNAMIC_CAST(form, TESForm, TESObjectREFR) : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// SEH isolated: no C++ objects in scope — avoids C2712.
static void DoLockApply(void* refr, uint8_t locked) noexcept
{
    LOG_REQUIREMENT("(global)::DoLockApply", "Needs the resolved game function and a valid ref. The actual game call is wrapped in a crash guard.");
    const LockFlipFn fn = locked ? g_origForceLock : g_origForceUnlock;
    if (!fn || !refr) return;
    tls_cs_lock_applying = true;
    __try {
        fn(refr);   // original ForceLock/ForceUnlock via the MinHook trampoline
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    tls_cs_lock_applying = false;
}

void CoSyncLockHook::FlushPendingLockOps()
{
    LOG_REQUIREMENT("CoSyncLockHook::FlushPendingLockOps", "Safe to call any time -- does nothing if there are no pending lock changes.");
    std::vector<LockOp> ops;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_pending.empty()) return;
        ops.swap(s_pending);
    }

    for (const LockOp& op : ops)
    {
        TESObjectREFR* refr = SafeLookupAndCastLockRefr(op.refFormID);
        if (!refr)
        {
            LOG_WARN("[LockHook] FLUSH refFormID=0x%08X not found or not a REFR", op.refFormID);
            continue;
        }

        DoLockApply(static_cast<void*>(refr), static_cast<uint8_t>(op.locked ? 1 : 0));
        LOG_UPDATED("[LockHook] FLUSH applied refFormID=0x%08X locked=%d", op.refFormID, op.locked);
    }
}
