#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncEquipHook.h"

#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncEntityRegistry.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncVMFormArg.h"
#include "CoSyncPlayerAnimation.h"
#include "CoSyncGameAPI.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"

#include "MinHook.h"

#include <atomic>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "CoSyncThreadDiag.h"

extern RelocPtr<PlayerCharacter*> g_player;
extern RelocAddr<_LookupFormByID> LookupFormByID;
extern CoSyncEntityRegistry       g_CoSyncEntities;
extern CoSyncPlayerManager        g_CoSyncPlayerManager;

// RVA RE-VERIFIED 2026-07-14 against Fallout4_disk.i64 (fresh auto-analysis,
// 204,801 functions) via headless idat.exe + IDAPython. Round 93's values
// (0xCE5DA0 / 0xCE5900) were both wrong:
//
//   kEquipObject_RVA 0xCE5DA0 (round-93):
//     ida_funcs.get_func(0x140CE5DA0).start_ea == 0x140CE5A90 → mid-function,
//     784 bytes into sub_140CE5A90. CodeRefsTo = 0 (nothing calls it directly).
//     The REAL entry is 0xCE5A90: is_func_head=True, 54 code callers, 3 data
//     refs, decompiled as 11-param __fastcall matching EquipObjectFn exactly
//     (mgr, actor, instPair, stackID, count, slot, queueEquip, forceEquip,
//     playSounds, applyNow, locked). Body: forceEquip=1 → LABEL_36 →
//     sub_140CE7C20 (inner equip); fires sub_140CE8F00(a1+8, &v32) =
//     BSTEventSource<ActorEquipManagerEvent::Event> at ActorEquipManager+8.
//
//   kUnequipObject_RVA 0xCE5900 (round-93):
//     ida_funcs.get_func(0x140CE5900).start_ea == 0x140CE58E0 → mid-function;
//     the function at 0xCE58E0 is a 2-param BGSEquipSlot builder (RTTI casts,
//     not an unequip operation). CodeRefsTo(0xCE5900) = 0. The REAL
//     UnequipObject is 0xCE5F30: is_func_head=True, 35 code callers, 11-param
//     __fastcall matching UnequipObjectFn (mgr, actor, instPair, count, slot,
//     stackID, queueEquip, forceEquip, playSounds, applyNow, slotReplaced);
//     uses same lock (sub_14022D3A0), same instPair builder (sub_1404368E0),
//     calls sub_140CE8C50 for the actual unequip, fires sub_140CE9090(a1+8).
//     9 of EquipObject's 54 caller-functions also call this function.
//
// The round-93 reasoning ("found EquipObject by scanning UnequipObject's 48
// callers for a sibling 11-arg call") was likely done against the dump-based
// Fallout4.i64 (which has per-region address skew vs disk) and produced
// plausible-looking but wrong addresses. Typedefs and call-sites need no
// changes — the 11-param signatures confirmed above match exactly.
static constexpr uintptr_t kEquipObject_RVA   = 0x00CE5E20; // ActorEquipManager::EquipObject
static constexpr uintptr_t kUnequipObject_RVA = 0x00CE62C0; // ActorEquipManager::UnequipObject
static constexpr uint32_t  kPlayerFormID       = 0x14;

// TLS re-entry guard. Set to true before calling the engine equip on a proxy
// actor so the detour silently passes through instead of re-broadcasting.
thread_local bool tls_cs_equip_applying = false;

// ============================================================================
// Hook function types (x64 __fastcall = default MSVC calling conv for x64)
// ============================================================================

// ActorEquipManager::EquipObject(mgr, actor, form_pair, stackID, count, slot,
//                                 queueEquip, forceEquip, playSounds, applyNow, locked)
using EquipObjectFn = char(__fastcall*)(
    void* mgr, void* actor, void** object,
    uint32_t stackID, int32_t count, void* slot,
    char queueEquip, char forceEquip, char playSounds, char applyNow, char locked);

// ActorEquipManager::UnequipObject(mgr, actor, form_pair, count, slot, stackID,
//                                   queueEquip, forceEquip, playSounds, applyNow, slotReplaced)
using UnequipObjectFn = char(__fastcall*)(
    void* mgr, void* actor, void** object,
    int32_t count, void* slot, int32_t stackID,
    char queueEquip, char forceEquip, char playSounds, char applyNow, void* slotBeingReplaced);

static EquipObjectFn   g_origEquip   = nullptr;
static UnequipObjectFn g_origUnequip = nullptr;

static std::atomic<bool> g_hooksInstalled{ false };

// Cached ActorEquipManager singleton pointer — captured from the first
// Detour_EquipObject call so EquipItemDirect can call the engine directly
// without needing a separate singleton RVA lookup.
static void* s_equipMgr = nullptr;

// ActorEquipManager singleton global (1.11.240): CommonLibF4 ID 4798287 via the
// installed Address Library -> 0x31EE428, and IDA shows 39 engine call sites
// doing `mov rcx, [0x31EE428]` then `call EquipObject (0xCE5E20)`. Used when no
// equip has been seen yet this session (e.g. building a joining character).
static constexpr uintptr_t kRVA_ActorEquipManager = 0x031EE428;

static void* EquipManager()
{
    if (s_equipMgr)
        return s_equipMgr;
    static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return *reinterpret_cast<void**>(base + kRVA_ActorEquipManager);
}

// ============================================================================
// Pending receive queue — applied on game thread via FlushPendingEquipOps
// ============================================================================

namespace
{
    struct EquipOp
    {
        uint32_t entityID = 0;
        uint32_t formID   = 0;
        int      kind     = 0; // 1=equip, 0=unequip
    };

    std::mutex           s_mutex;
    std::vector<EquipOp> s_pending;
}

// ============================================================================
// Helpers
// ============================================================================

// __declspec(noinline) is load-bearing here: under this project's /GL+/LTCG
// Release config the compiler can otherwise inline these one-liners away,
// silently dropping the __try/__except protection.
__declspec(noinline) static uint32_t SafeReadFormID(void* form) noexcept
{
    LOG_REQUIREMENT("CoSyncEquipHook::SafeReadFormID", "Safe to call with a null form (returns 0). Guarded so even a bad, non-null pointer can't crash the game.");
    if (!form) return 0;
    __try {
        return *reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(form) + 0x14);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

__declspec(noinline) static void* SafeReadItemForm(void** object) noexcept
{
    LOG_REQUIREMENT("CoSyncEquipHook::SafeReadItemForm", "Safe to call with a null object (returns null). Guarded so even a bad, non-null pointer can't crash the game.");
    if (!object) return nullptr;
    __try { return *object; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// TESForm::formType is at offset 0x1A (F4SE GameForms.h line 476 verified).
__declspec(noinline) static uint8_t SafeReadFormType(void* form) noexcept
{
    LOG_REQUIREMENT("CoSyncEquipHook::SafeReadFormType", "Safe to call with a null form (returns 0). Guarded so an invalid pointer can't crash the game.");
    if (!form) return 0;
    __try {
        return *reinterpret_cast<uint8_t*>(
            reinterpret_cast<uint8_t*>(form) + 0x1A);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static bool IsLocalPlayer(void* actor) noexcept
{
    LOG_REQUIREMENT("CoSyncEquipHook::IsLocalPlayer", "No requirements — safely handles a null or invalid actor since it just calls SafeReadFormID underneath.");
    return SafeReadFormID(actor) == kPlayerFormID;
}

// round 94 diagnostic: unconditional fire-heartbeat for both detours, logged
// BEFORE any of the early-return filters below. This is the key signal for
// confirming the round-93 RVA fix (kEquipObject_RVA/kUnequipObject_RVA) --
// if equipping/unequipping ANY item in-game (player or NPC) never produces
// these lines, the hook isn't actually being reached and the RVA is still
// wrong despite compiling/installing cleanly.
namespace
{
    std::atomic<uint32_t> s_equipCalls{ 0 };
    std::atomic<uint32_t> s_unequipCalls{ 0 };

    void LogEquipHeartbeat(void* actor)
    {
        LOG_REQUIREMENT("CoSyncEquipHook::LogEquipHeartbeat", "Doesn't touch the game — just counts calls and prints the actor pointer as a label, never reads through it.");
        const uint32_t n = ++s_equipCalls;
        if (n <= 5 || n % 200 == 0)
            LOG_INFO("[EquipHook] Equip hook fired #%u actor=%p", n, actor);
    }
    void LogUnequipHeartbeat(void* actor)
    {
        LOG_REQUIREMENT("CoSyncEquipHook::LogUnequipHeartbeat", "Doesn't touch the game — just counts calls and prints the actor pointer as a label, never reads through it.");
        const uint32_t n = ++s_unequipCalls;
        if (n <= 5 || n % 200 == 0)
            LOG_INFO("[EquipHook] Unequip hook fired #%u actor=%p", n, actor);
    }
}

// ============================================================================
// Detours
// ============================================================================

static char __fastcall Detour_EquipObject(
    void* mgr, void* actor, void** object,
    uint32_t stackID, int32_t count, void* slot,
    char queueEquip, char forceEquip, char playSounds, char applyNow, char locked)
{
    LOG_REQUIREMENT("CoSyncEquipHook::Detour_EquipObject", "Needs the equip hook to be installed. Treats actor/object as untrusted and only reads them through the safe helpers. The re-entry flag must be accurate or this can echo its own equips back out.");
    CoSyncThreadDiag::Note(CoSyncThreadDiag::kEquip);
    LogEquipHeartbeat(actor);

    // Cache the singleton pointer on first call so EquipItemDirect can use it.
    if (!s_equipMgr && mgr)
        s_equipMgr = mgr;

    // Echo guard: skip when we're the ones applying a remote op.
    if (tls_cs_equip_applying)
        return g_origEquip ? g_origEquip(mgr, actor, object, stackID, count,
            slot, queueEquip, forceEquip, playSounds, applyNow, locked) : 0;

    // Only broadcast equips by the local player.
    if (!IsLocalPlayer(actor))
        return g_origEquip ? g_origEquip(mgr, actor, object, stackID, count,
            slot, queueEquip, forceEquip, playSounds, applyNow, locked) : 0;

    void*    itemForm    = SafeReadItemForm(object);
    uint8_t  itemFType   = SafeReadFormType(itemForm);
    uint32_t itemFormID  = SafeReadFormID(itemForm);

    const char rc = g_origEquip
        ? g_origEquip(mgr, actor, object, stackID, count, slot,
                      queueEquip, forceEquip, playSounds, applyNow, locked)
        : 0;

    // Only broadcast weapon equips — OutfitSync handles armor independently.
    if (itemFType == kFormType_WEAP && itemFormID != 0 && CoSyncNet::IsConnected())
    {
        const uint32_t eid = CoSyncNet::GetMyEntityID();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "EQUIP|%u|%u|1", eid, itemFormID);
        CoSyncTransport::Send(std::string(buf));

        static std::atomic<uint32_t> s_n{ 0 };
        const uint32_t n = ++s_n;
        if (n <= 20 || n % 200 == 0)
            LOG_INFO("[EquipHook] Sent equip #%u entity=%u formID=0x%08X", n, eid, itemFormID);
    }

    // 2026-08-26 TEMPORARY: live diagnostic for the "Stimpak doesn't visually
    // replicate in first person" bug report. See CoSyncPlayerAnimation.h's
    // StartAlchEquipDiag comment for the full context -- this just arms it.
    if (itemFType == kFormType_ALCH)
        CoSyncPlayerAnimation::StartAlchEquipDiag();

    return rc;
}

static char __fastcall Detour_UnequipObject(
    void* mgr, void* actor, void** object,
    int32_t count, void* slot, int32_t stackID,
    char queueEquip, char forceEquip, char playSounds, char applyNow, void* slotBeingReplaced)
{
    LOG_REQUIREMENT("CoSyncEquipHook::Detour_UnequipObject", "Needs the unequip hook to be installed. Treats actor/object as untrusted and only reads them through the safe helpers. The re-entry flag must be accurate or this can echo its own unequips back out.");
    CoSyncThreadDiag::Note(CoSyncThreadDiag::kEquip);
    LogUnequipHeartbeat(actor);

    if (tls_cs_equip_applying)
        return g_origUnequip ? g_origUnequip(mgr, actor, object, count, slot,
            stackID, queueEquip, forceEquip, playSounds, applyNow, slotBeingReplaced) : 0;

    if (!IsLocalPlayer(actor))
        return g_origUnequip ? g_origUnequip(mgr, actor, object, count, slot,
            stackID, queueEquip, forceEquip, playSounds, applyNow, slotBeingReplaced) : 0;

    void*    itemForm    = SafeReadItemForm(object);
    uint8_t  itemFType   = SafeReadFormType(itemForm);
    uint32_t itemFormID  = SafeReadFormID(itemForm);

    const char rc = g_origUnequip
        ? g_origUnequip(mgr, actor, object, count, slot, stackID,
                        queueEquip, forceEquip, playSounds, applyNow, slotBeingReplaced)
        : 0;

    // Only broadcast weapon unequips — OutfitSync handles armor independently.
    if (itemFType == kFormType_WEAP && itemFormID != 0 && CoSyncNet::IsConnected())
    {
        const uint32_t eid = CoSyncNet::GetMyEntityID();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "EQUIP|%u|%u|0", eid, itemFormID);
        CoSyncTransport::Send(std::string(buf));

        static std::atomic<uint32_t> s_n{ 0 };
        const uint32_t n = ++s_n;
        if (n <= 20 || n % 200 == 0)
            LOG_INFO("[EquipHook] Sent unequip #%u entity=%u formID=0x%08X", n, eid, itemFormID);
    }

    return rc;
}

// ============================================================================
// Public API
// ============================================================================

void InstallEquipHook()
{
    LOG_REQUIREMENT("InstallEquipHook", "Needs the verified equip/unequip function addresses in the running game. Must only be called once — that's enforced by a guard flag.");
    if (g_hooksInstalled.exchange(true))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[EquipHook] Couldn't set up hooking (code %d)", (int)st);
        g_hooksInstalled.store(false);
        return;
    }

    void* equip_target   = reinterpret_cast<void*>(base + kEquipObject_RVA);
    void* unequip_target = reinterpret_cast<void*>(base + kUnequipObject_RVA);

    st = MH_CreateHook(equip_target,
        reinterpret_cast<void*>(&Detour_EquipObject),
        reinterpret_cast<void**>(&g_origEquip));
    if (st != MH_OK)
    {
        LOG_ERROR("[EquipHook] Couldn't hook the equip function (code %d, target=%p)", (int)st, equip_target);
        g_hooksInstalled.store(false);
        return;
    }
    MH_EnableHook(equip_target);

    st = MH_CreateHook(unequip_target,
        reinterpret_cast<void*>(&Detour_UnequipObject),
        reinterpret_cast<void**>(&g_origUnequip));
    if (st != MH_OK)
    {
        LOG_ERROR("[EquipHook] Couldn't hook the unequip function (code %d, target=%p)", (int)st, unequip_target);
        MH_DisableHook(equip_target);
        MH_RemoveHook(equip_target);
        g_hooksInstalled.store(false);
        return;
    }
    MH_EnableHook(unequip_target);

    LOG_INFO("[EquipHook] Equip/unequip hooks installed: %p / %p",
        equip_target, unequip_target);
}

void UninstallEquipHook()
{
    LOG_REQUIREMENT("UninstallEquipHook", "Only does anything if the hooks were actually installed — checked by a guard flag before removing them.");
    if (!g_hooksInstalled.exchange(false))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* equip_target   = reinterpret_cast<void*>(base + kEquipObject_RVA);
    void* unequip_target = reinterpret_cast<void*>(base + kUnequipObject_RVA);

    MH_DisableHook(equip_target);
    MH_RemoveHook(equip_target);
    MH_DisableHook(unequip_target);
    MH_RemoveHook(unequip_target);

    LOG_INFO("[EquipHook] Equip/unequip hooks removed");
}

bool CoSyncEquipHook::IsEquip(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncEquipHook::IsEquip", "Doesn't touch the game — just checks the start of a text message.");
    return msg.rfind("EQUIP|", 0) == 0;
}

void CoSyncEquipHook::OnReceiveEquip(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncEquipHook::OnReceiveEquip", "Needs the message to actually start with the \"EQUIP|\" prefix and be long enough to hold it, plus a valid local entity ID.");
    const char* s = msg.c_str() + 6; // skip "EQUIP|"
    char* e = nullptr;

    uint32_t entityID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (!e || *e != '|' || entityID == 0) return;
    s = e + 1;

    uint32_t formID = static_cast<uint32_t>(std::strtoul(s, &e, 10));
    if (!e || *e != '|' || formID == 0) return;
    s = e + 1;

    int kind = static_cast<int>(std::strtol(s, &e, 10)); // 1=equip 0=unequip

    // Don't apply our own echoed packet.
    if (entityID == CoSyncNet::GetMyEntityID())
        return;

    std::lock_guard<std::mutex> lk(s_mutex);
    s_pending.push_back({ entityID, formID, kind });
}

// UPDATE 2026-06-20 (round 41 audit): same unguarded peer-controlled
// op.formID -> LookupFormByID crash pattern fixed elsewhere this session.
// SEH-wrapped, noinline (load-bearing under /GL+/LTCG).
__declspec(noinline) static TESForm* SafeLookupForm(uint32_t formID)
{
    LOG_REQUIREMENT("CoSyncEquipHook::SafeLookupForm", "Guarded so a bad or peer-supplied form ID can't crash the game.");
    __try
    {
        return LookupFormByID(formID);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

void CoSyncEquipHook::FlushPendingEquipOps()
{
    LOG_REQUIREMENT("CoSyncEquipHook::FlushPendingEquipOps", "Needs the player manager to be valid, and checks each proxy's actor before using it. Sets the re-entry flag around the actual equip/unequip call so the hook doesn't echo it back out.");
    static const BSFixedString kEquipItem("EquipItem");
    static const BSFixedString kUnequipItem("UnequipItem");
    std::vector<EquipOp> ops;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_pending.empty()) return;
        ops.swap(s_pending);
    }

    for (const EquipOp& op : ops)
    {
        CoSyncPlayer* pl = g_CoSyncPlayerManager.GetPlayerByEntityID(op.entityID);
        if (!pl || !pl->actorRef)
        {
            LOG_WARN("[EquipHook] Got entity=%u form=0x%08X kind=%d but its proxy isn't ready yet",
                op.entityID, op.formID, op.kind);
            continue;
        }

        TESForm* form = SafeLookupForm(op.formID);
        if (!form)
        {
            LOG_WARN("[EquipHook] Item form=0x%08X wasn't found", op.formID);
            continue;
        }

        // Weapons route through the two-phase AddItem->delay->EquipItem path in
        // CoSyncPlayerManager::Tick. Calling EquipObject without AddItem first is
        // always a silent no-op on fresh proxy actors that don't yet have the item
        // in their inventory.
        if (form->formType == kFormType_WEAP)
        {
            if (op.kind == 1)
            {
                pl->m_pendingWeaponFormID     = op.formID;
                pl->m_pendingWeaponChange     = true;
                pl->m_lastQueuedWeaponFormID  = op.formID;
                pl->m_weaponConfirmedEquipped = false;
                LOG_UPDATED("[EquipHook] Queued a weapon equip for entity=%u form=0x%08X",
                    op.entityID, op.formID);
            }
            else
            {
                // Holster: set pending to 0 so PlayerMgr fires UnequipItem.
                pl->m_pendingWeaponFormID    = 0;
                pl->m_pendingWeaponChange    = true;
                pl->m_lastQueuedWeaponFormID = 0;
                LOG_UPDATED("[EquipHook] Holstered a weapon for entity=%u form=0x%08X",
                    op.entityID, op.formID);
            }
            continue;
        }

        // Non-weapon (ARMO etc.) -- OutfitSync normally handles these, but process
        // directly if they arrive here, using the correct BGSObjectInstance layout.
        tls_cs_equip_applying = true;

        if (op.kind == 1)
        {
            if (s_equipMgr && g_origEquip)
            {
                CoSyncGameAPI::EnsureStackExtraLists(pl->actorRef, form);   // E1 precondition
                void* instPair[2] = { form, nullptr };
                g_origEquip(s_equipMgr, pl->actorRef, instPair,
                    0, 1, nullptr,
                    /*queueEquip*/0, /*forceEquip*/1,
                    /*playSounds*/0, /*applyNow*/1, /*locked*/0);
                LOG_UPDATED("[EquipHook] Equipped directly for entity=%u form=0x%08X proxy=%p",
                    op.entityID, op.formID, static_cast<void*>(pl->actorRef));
            }
            else
            {
                VMArray<VMVariable> args;
                VMVariable vF; CoSyncVM::SetFormArg(vF, form); args.Push(&vF);
                bool noRemove = false;
                VMVariable vN; vN.Set(&noRemove); args.Push(&vN);
                bool silent = true;
                VMVariable vS; vS.Set(&silent);   args.Push(&vS);
                CallFunctionNoWait(pl->actorRef, kEquipItem, args);
                LOG_UPDATED("[EquipHook] Equipped via script for entity=%u form=0x%08X proxy=%p",
                    op.entityID, op.formID, static_cast<void*>(pl->actorRef));
            }
        }
        else
        {
            if (s_equipMgr && g_origUnequip)
            {
                CoSyncGameAPI::EnsureStackExtraLists(pl->actorRef, form);   // E1 precondition
                void* instPair[2] = { form, nullptr };
                g_origUnequip(s_equipMgr, pl->actorRef, instPair,
                    1, nullptr, 0,
                    /*queueEquip*/0, /*forceEquip*/1,
                    /*playSounds*/0, /*applyNow*/1, /*slotBeingReplaced*/nullptr);
                LOG_UPDATED("[EquipHook] Unequipped directly for entity=%u form=0x%08X proxy=%p",
                    op.entityID, op.formID, static_cast<void*>(pl->actorRef));
            }
            else
            {
                VMArray<VMVariable> args;
                VMVariable vF; CoSyncVM::SetFormArg(vF, form); args.Push(&vF);
                bool noEquip = false;
                VMVariable vN; vN.Set(&noEquip);  args.Push(&vN);
                bool silent = true;
                VMVariable vS; vS.Set(&silent);   args.Push(&vS);
                CallFunctionNoWait(pl->actorRef, kUnequipItem, args);
                LOG_UPDATED("[EquipHook] Unequipped via script for entity=%u form=0x%08X proxy=%p",
                    op.entityID, op.formID, static_cast<void*>(pl->actorRef));
            }
        }

        tls_cs_equip_applying = false;
    }
}

// ============================================================================
// Direct equip/unequip — calls ActorEquipManager::EquipObject synchronously,
// which fires the full engine visual chain (HandleItemEquip → AttachWeapon →
// DoReparentWeapon → DrawWeaponMagicHands → Update3DPosition →
// RequestLoadAnimationsForWeaponChange). Works for weapons, armor, and clothing.
// Returns false only when the equip hook is not installed (or the arguments are
// null) -- caller should fall back to Papyrus in that case.
// MUST be called on the game thread.
// ============================================================================

bool CoSyncEquipHook::EquipItemDirect(Actor* actor, TESForm* form)
{
    LOG_REQUIREMENT("CoSyncEquipHook::EquipItemDirect",
        "Needs the equip manager to already be cached and the equip hook installed, "
        "plus a valid actor and form. Must be called on the game thread.");
    void* const mgr = EquipManager();
    if (!mgr || !g_origEquip || !actor || !form)
        return false;

    // BGSObjectInstance is 16 bytes: {TESForm* object, TBO_InstanceData* instanceData}.
    // For weapons, passing null instanceData causes GAMES to skip geometry assembly
    // on fully-modular weapons → weapon attaches to skeleton but has no mesh.
    // Call TESBoundObject::CloneInstanceData(nullptr) (vtable[0x56]) to get a
    // fresh base-config TBO_InstanceData* so GAMES assembles at least the default
    // weapon geometry. SEH-wrapped; null fall-through keeps old behaviour.
    TBO_InstanceData* idata = nullptr;
    if (form->formType == kFormType_WEAP)
    {
        using CloneFn = TBO_InstanceData*(__fastcall*)(TESForm*, TBO_InstanceData*);
        __try
        {
            CloneFn fn = reinterpret_cast<CloneFn>((*reinterpret_cast<void***>(form))[0x56]);
            if (fn) idata = fn(form, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { idata = nullptr; }

        if (idata)
            LOG_UPDATED("[EquipHook] Equipping weapon=0x%08X with a cloned instance %p",
                form->formID, static_cast<void*>(idata));
        else
            LOG_UPDATED_WARN("[EquipHook] Equipping weapon=0x%08X with no instance data — using the base form",
                form->formID);
    }

    // E1 (2026-09-26): the engine's EquipObject walks every stack of this
    // item and reads each stack's ExtraDataList without a null check, but the
    // engine creates lists lazily, so a plain stack can have none (client crash,
    // Fallout4+0x165940F reading 0x20). Satisfy that precondition first.
    CoSyncGameAPI::EnsureStackExtraLists(actor, form);

    // Ownership of the cloned instance (IDA 1.11.240, 2026-09-26): the WEAP
    // clone (vtable 0x56 = sub_140478100 -> ctor sub_140473990) is returned
    // with refcount 0 (+0x08). The engine's equip copies the pair (AddRef) and
    // releases its copy before returning (sub_140CDFCD0), so a floating object
    // could be destroyed while the call is still using it. Hold a reference
    // for the call, then release it the engine's way: whoever drops the last
    // reference destroys it -- the engine if it kept the weapon equipped, us
    // if it didn't. Freed exactly once, never early, never leaked.
    auto addRef = [](void* obj)
    {
        _InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<uint8_t*>(obj) + 0x08));
    };
    auto release = [](void* obj)
    {
        if (_InterlockedExchangeAdd(reinterpret_cast<volatile long*>(static_cast<uint8_t*>(obj) + 0x08), -1) == 1)
        {
            using DeletingDtor = void(__fastcall*)(void*, int);
            reinterpret_cast<DeletingDtor>((*reinterpret_cast<void***>(obj))[0])(obj, 1);
        }
    };
    if (idata)
        addRef(idata);

    void* instPair[2] = { form, idata };
    tls_cs_equip_applying = true;
    g_origEquip(mgr, actor, instPair,
        /*stackID*/0, /*count*/1, /*slot*/nullptr,
        /*queueEquip*/0, /*forceEquip*/1,
        /*playSounds*/0, /*applyNow*/1, /*locked*/0);
    tls_cs_equip_applying = false;

    if (idata)
        release(idata);
    return true;
}

bool CoSyncEquipHook::DrawWeaponEngine(Actor* actor, bool draw)
{
    LOG_REQUIREMENT("CoSyncEquipHook::DrawWeaponEngine",
        "Needs a valid, fully-spawned actor with a ready animation graph. Calls the engine's own "
        "draw/holster weapon function directly. Must be called on the game thread; guarded so an "
        "invalid actor can't crash the game.");
    if (!actor)
        return false;

    using DrawWeaponFn = char(__fastcall*)(Actor*, bool);
    __try
    {
        DrawWeaponFn fn = reinterpret_cast<DrawWeaponFn>(
            (*reinterpret_cast<void***>(actor))[201]);
        if (!fn)
            return false;
        fn(actor, draw);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[EquipHook] Drawing/holstering crashed for actor=%p draw=%d — caught it",
            static_cast<void*>(actor), draw ? 1 : 0);
        return false;
    }
}

bool CoSyncEquipHook::UnequipItemDirect(Actor* actor, TESForm* form)
{
    LOG_REQUIREMENT("CoSyncEquipHook::UnequipItemDirect",
        "Needs the equip manager to already be cached and the unequip hook installed, "
        "plus a valid actor and form. Must be called on the game thread.");
    void* const mgr = EquipManager();
    if (!mgr || !g_origUnequip || !actor || !form)
        return false;

    void* instPair[2] = { form, nullptr };
    tls_cs_equip_applying = true;
    g_origUnequip(mgr, actor, instPair,
        /*count*/1, /*slot*/nullptr, /*stackID*/0,
        /*queueEquip*/0, /*forceEquip*/1,
        /*playSounds*/0, /*applyNow*/1, /*slotBeingReplaced*/nullptr);
    tls_cs_equip_applying = false;
    return true;
}
