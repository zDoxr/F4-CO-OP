#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncPickupHook.h"
#include "CoSyncContainerSync.h"
#include "CoSyncItemDropSync.h"

#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncRequirementsLog.h"

#include "MinHook.h"

#include <atomic>
#include <cstdio>

// sub_140500430 = "INV_ADD_FROM_WORLD" helper.
// Called by PlayerCharacter::PickUp when the player grabs a world item.
// Signature (RE'd in FO4_Wrld re/ dossier):
//   void __fastcall(
//       TESObjectREFR* dstActor,   // rcx — destination (player)
//       TESObjectREFR* srcREFR,    // rdx — world REFR being absorbed
//       unsigned int   count);     // r8d — item count (usually 1)
// RVA CORRECTED round 93 (IDA cross-check, ida_funcs.get_func + Hex-Rays):
// round 40's "+0x10" fix below was itself wrong -- same disassembly-desync
// defect class found project-wide in round 90/93, and the same exact
// pattern independently confirmed wrong for CoSyncDoorHook.cpp's two RVAs
// and CoSyncLockHook.cpp's three RVAs in round 93. IDA's control-flow-
// validated function boundaries confirm the ORIGINAL dossier address
// (0x500430) IS the true function entry; round 40's "int3 padding before
// the real prologue" read was a misread of the standalone Capstone
// re-disassembly. Reverting to the dossier value.
// RVA RE-VERIFIED 2026-07-15 against Fallout4_disk.i64: the dossier/dump value
// 0x500430 is the +0x10 dump-IDB skew again (same defect as LookupFormByID
// 0x311850→0x311860). Byte-pattern search of the dump function's first 32
// prologue bytes (44 89 44 24 18 53 57 41 57 48 81 EC 80 ...) lands at exactly
// ONE address in the disk exe: 0x140500440 — is_func_head=YES, 11 callers,
// identical Hex-Rays body (CNT-signature container check, dstActor+0xE0
// inventory, srcREFR+0x100 extra-data walk, inner add at sub_140502940).
// This is why round-133 found "0x500430 = padding, inert" and disabled the
// hook: it was patching the padding byte before the real prologue.
static constexpr uintptr_t kInvAddFromWorld_RVA = 0x00500760;
static constexpr uint32_t  kPlayerFormID         = 0x14;

using InvAddFromWorldFn = void(__fastcall*)(void* dstActor, void* srcREFR, unsigned int count);

static InvAddFromWorldFn g_origInvAdd   = nullptr;
static std::atomic<bool> g_hookInstalled{ false };

// ============================================================================
// Helpers
// ============================================================================

// UPDATE 2026-06-17: __declspec(noinline) is load-bearing -- see the fuller
// comment in CoSyncDoorHook.cpp's identical helpers. A non-noinline static
// __try/__except function risks being inlined away with its SEH
// exception-dispatch data silently dropped under this project's
// /GL+/LTCG Release config. Also dropped `noexcept` (second pass) -- a
// noexcept function combined with __try/__except let the optimizer
// eliminate the protected scope entirely (confirmed via disassembly: just
// the raw load + jmp straight to retn, with the fallback `return` left as
// dead code). `noexcept` is a direct contradiction with "this might
// fault," which is the entire reason this guard exists.
__declspec(noinline) static uint32_t SafeReadU32(const void* base, size_t off)
{
    LOG_REQUIREMENT("CoSyncPickupHook::SafeReadU32", "Reads a 4-byte value from memory safely. Returns 0 instead of crashing if the address turns out to be bad.");
    __try {
        return *reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(base) + off);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

__declspec(noinline) static void* SafeReadPtr(const void* base, size_t off)
{
    LOG_REQUIREMENT("CoSyncPickupHook::SafeReadPtr", "Reads a pointer from memory safely. Returns null instead of crashing if the address turns out to be bad.");
    __try {
        return *reinterpret_cast<void* const*>(
            reinterpret_cast<const uint8_t*>(base) + off);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ============================================================================
// Detour
// ============================================================================

static void __fastcall Detour_InvAddFromWorld(void* dstActor, void* srcREFR, unsigned int count)
{
    LOG_REQUIREMENT("CoSyncPickupHook::Detour_InvAddFromWorld", "Runs whenever any actor picks up a world item. Needs the hook installed and treats the actor/item pointers as untrusted, only reading them through the safe helpers. Only sends a pickup message once connected to another player.");
    // round 94 diagnostic: unconditional fire-heartbeat, confirms the
    // round-93 RVA fix (kInvAddFromWorld_RVA) is reached -- fires for every
    // actor picking up any world item, not just the player.
    {
        static std::atomic<uint32_t> s_n{ 0 };
        const uint32_t n = ++s_n;
        if (n <= 5 || n % 200 == 0)
            LOG_INFO("[PickupHook] Detour_InvAddFromWorld fired #%u dstActor=%p", n, dstActor);
    }

    // Let the engine run first so the pickup completes regardless of what we do.
    if (g_origInvAdd)
        g_origInvAdd(dstActor, srcREFR, count);

    if (!CoSyncNet::IsConnected()) return;

    // Filter: dstActor must be local player.
    const uint32_t dstFormID = SafeReadU32(dstActor, 0x14);
    if (dstFormID != kPlayerFormID) return;

    // Filter: srcREFR must be a world-placed item (has a parentCell).
    // Container withdrawals arrive with a wrapper ref whose parentCell may
    // differ; world items always have a valid cell pointer at REFR+0xB8.
    if (!srcREFR) return;
    void* parentCell = SafeReadPtr(srcREFR, 0xB8); // PARENT_CELL_OFF
    if (!parentCell)
    {
        // Container withdrawal: item taken from a container, not the world.
        // baseForm is at TESObjectREFR+0xE0 (STATIC_ASSERT-verified).
        void* baseForm = SafeReadPtr(srcREFR, 0xE0);
        if (baseForm)
        {
            const uint32_t baseFormID = SafeReadU32(baseForm, 0x14);
            if (baseFormID != 0)
                CoSyncContainerSync::OnContainerWithdrawal(baseFormID, count);
        }
        return;
    }

    const uint32_t refFormID = SafeReadU32(srcREFR, 0x14);
    if (refFormID == 0) return;

    // If this pickup is a tracked drop-sync ref (either this machine's own
    // reported original, or a clone spawned for someone else's drop),
    // notify so DROPGONE can propagate — harmless no-op otherwise.
    CoSyncItemDropSync::NotifyLocalPickup(refFormID);

    // "PU|refFormID" — matches CoSyncWorldSync::OnReceivePickup format.
    // UPDATE 2026-06-17: was std::snprintf -- a raw CRT printf-family call
    // outside the logging pipeline entirely, with no guard at all. Given
    // this session's whole investigation established that the CRT's
    // printf-formatting machinery can become durably corrupted (every
    // subsequent call faults once triggered, regardless of arguments),
    // any unguarded printf-family call anywhere in the codebase is a
    // latent crash site, not just the logging macros. std::to_string for
    // an integer uses the STL's own digit-extraction routine (basic
    // division/modulo), not <cstdio>'s vsnprintf/__stdio_common_vsnprintf
    // path, so it can't hit the same fault.
    CoSyncTransport::Send(std::string("PU|") + std::to_string(refFormID));

    static std::atomic<uint32_t> s_n{ 0 };
    const uint32_t n = ++s_n;
    if (n <= 20 || n % 200 == 0)
        LOG_INFO("[PickupHook] TX PU #%u refFormID=0x%08X count=%u", n, refFormID, count);
}

// ============================================================================
// Public API
// ============================================================================

void InstallPickupHook()
{
    LOG_REQUIREMENT("InstallPickupHook", "Only runs once. Hooks the engine's pickup function so item pickups can be shared with other players.");
    if (g_hookInstalled.exchange(true))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kInvAddFromWorld_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[PickupHook] MH_Initialize failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Detour_InvAddFromWorld),
        reinterpret_cast<void**>(&g_origInvAdd));
    if (st != MH_OK)
    {
        LOG_ERROR("[PickupHook] MH_CreateHook failed: %d target=%p", (int)st, target);
        g_hookInstalled.store(false);
        return;
    }

    MH_EnableHook(target);
    LOG_INFO("[PickupHook] Installed at %p (RVA 0x%zX)", target, kInvAddFromWorld_RVA);
}

void UninstallPickupHook()
{
    LOG_REQUIREMENT("UninstallPickupHook", "Only runs if the hook is currently installed. Removes the pickup hook cleanly.");
    if (!g_hookInstalled.exchange(false))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kInvAddFromWorld_RVA);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    LOG_INFO("[PickupHook] Uninstalled");
}
