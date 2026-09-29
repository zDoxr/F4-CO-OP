#include "CoSyncGameAPI.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncEntityRegistry.h"
#include "GameReferences.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "GameData.h"
#include "GameExtraData.h"
#include "GameUtilities.h"
#include "GameRTTI.h"
#include "GameHandle.h"
#include "NiNodes.h"
#include "PapyrusVM.h"
#include "PapyrusEvents.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncVMFormArg.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <unordered_map>
#include <cmath>
#include <mutex>
#include <vector>
#include <string>

extern RelocPtr<PlayerCharacter*> g_player;
extern RelocPtr<GameVM*> g_gameVM;
extern PlayerCharacter* CoSyncGetPlayer();
extern RelocAddr<_MoveRefrToPosition> MoveRefrToPosition;
extern RelocAddr<_LookupFormByID> LookupFormByID;

// NG 1.11.221 — re-verified 2026-07-04 by decompiling sub_1402E4DF0 AND all 20 of its
// in-exe callers (PlaceAtMe family) in IDA. The previous interpretation had the RDX/R8
// argument roles SWAPPED and the struct layout shifted by 8 bytes, which made the engine
// take its clean "no object" failure path and return a pointer into OUR OWN STACK that
// the old Path-A heuristic then treated as a live Actor* (root cause of the EXEC-AV
// crashes that looked animation-related).
//
// True contract (identical at every engine call site):
//   uint32_t* CreateReferenceAtLocation(TESDataHandler* dh,       // RCX = *(RVA 0x30DC000)
//                                       uint32_t*       outHandle, // RDX = &local DWORD
//                                       NEW_REFR_DATA*  data);     // R8
//   - returns outHandle; *outHandle = 32-bit ObjectRefHandle, 0/invalid on failure
//   - resolve handle -> TESObjectREFR* via LookupREFRByHandle
//   - data MUST carry the engine's own NEW_REFR_DATA vftable at +0x00: on the success
//     path the engine makes an unconditional virtual call (**(void***)data)(data, refr);
//     slot 0 of the real vftable (RVA 0x246C448) is a nullsub, so it is a safe no-op.
// LookupREFRByHandle: RVA 0x002A0540 — confirmed unchanged; body reads handle bits
//   (AND mask 0x001FFFFF / 0x03E00000) and accesses global handle table via RIP-rel ADD.
// Root-cause of the proxy-spawn crash (DoxCoSync.dll+0x1E7C15, READ AV at
// 0xFFFFFFFFFFFFFFFF, 4 occurrences per session, 2026-07-13):
// The engine's CreateReferenceAtLocation unconditionally calls vtable slot 0
// on the NEW_REFR_DATA struct on its success path: (**(void***)data)(data, refr).
// We were setting vftable = exe_base + 0x246C448 (a Fallout4.exe .rdata RVA),
// expecting that slot 0 there would be a nullsub.  At runtime, however,
// *(void**)(exe_base + 0x246C448) resolved to DoxCoSync.dll+0x1E7C15 (a large
// interior function in our DLL) — not a nullsub.  This caused the engine to
// jump into the middle of our DLL code, which immediately faulted trying to
// read from 0xFFFFFFFFFFFFFFFF (corrupted XMM-save target).
//
// The raw RSP stack dump confirmed the call chain: Fallout4.exe+0x16590EF
// (return address at [RSP+0x000]) called our DLL via that vtable slot.  The
// unwind-table walk reported only DoxCoSync frames because the unwind info at
// +0x1E7C15 was wrong (the function was entered via an indirect call, not the
// normal prologue path, so the unwind tables misled the walker).
//
// Fix: replace the unreliable Fallout4.exe vtable RVA with a self-contained
// nullsub in our own DLL.  Slot 0 = CoSyncNewRefrData_VtSlot0 (does nothing,
// returns immediately).  No Fallout4.exe RVA dependency, no version fragility.
static void CoSyncNewRefrData_VtSlot0(void*, void*) { /* nullsub */ }
static void* s_CoSyncNewRefrDataVtbl[] = {
    reinterpret_cast<void*>(CoSyncNewRefrData_VtSlot0)
};

struct CoSync_NEW_REFR_DATA
{
    void*    vftable;            // 00 — must point to a vtable whose slot 0 is callable;
                                 //      set to s_CoSyncNewRefrDataVtbl (our own nullsub)
    float    locX, locY, locZ;   // 08 location
    float    dirX, dirY, dirZ;   // 14 direction/angle
    void*    object;             // 20 TESBoundObject* (our TESNPC*)
    void*    interior;           // 28 TESObjectCELL* (null for exterior)
    void*    worldSpace;         // 30 TESWorldSpace* (null for interior)
    void*    reference;          // 38 (null)
    void*    primitive;          // 40 (null)
    void*    unk48;              // 48 (null; PlaceAtMe forwards an opaque arg here)
    void*    extraPtr;           // 50 refcounted ExtraDataList-style ptr (null)
    void*    unk58;              // 58 (null)
    void*    unk60;              // 60 (null)
    uint8_t  unk68;              // 68 = 0   } every engine caller writes the DWORD at
    uint8_t  unk69;              // 69 = 0   } +0x68 as 0x01000000, i.e. only byte +0x6B
    uint8_t  initiallyDisabled;  // 6A = 0   } is set
    uint8_t  unk6B;              // 6B = 1 (engine default in all 20 call sites)
    uint8_t  unk6C;              // 6C = 1 on the PlaceAtMe path (post-create init)
    uint8_t  unk6D;              // 6D = 0
    uint8_t  pad6E[2];           // 6E
};                               // 70 bytes — matches engine stack frames exactly

typedef uint32_t* (*_CreateReferenceAtLocation)(void* dh, uint32_t* outHandle, CoSync_NEW_REFR_DATA* data);
static RelocAddr<_CreateReferenceAtLocation> g_CreateReferenceAtLocation(0x2E5110);

// Engine enable/disable internals, IDA-verified 2026-07-20 against
// Fallout4_disk.i64 via the console Enable(0x5C45F0)/Disable(0x5C47D0)
// command handlers: both handlers refuse refs that have an enable-state
// parent ("...will be ignored") — the same guard that makes Papyrus
// Disable() a silent no-op on CK-placed camp NPCs — then call these for
// the real work. Calling them directly bypasses only the refusal, not the
// teardown; all three are internally critical-section guarded.
typedef void (*_EngineDisableRefr)(TESObjectREFR* refr, char fadeOut);
typedef void (*_EngineEnableRefr)(TESObjectREFR* refr);
static RelocAddr<_EngineDisableRefr> g_EngineDisableRefr(0x5B4210);
static RelocAddr<_EngineEnableRefr>  g_EngineEnableRefrPre(0x5B4760);
static RelocAddr<_EngineEnableRefr>  g_EngineEnableRefrMain(0x5B4470);

__declspec(noinline) static bool EngineDisableRefrSafe(TESObjectREFR* refr)
{
    __try
    {
        g_EngineDisableRefr(refr, 0);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

__declspec(noinline) static bool EngineEnableRefrSafe(TESObjectREFR* refr)
{
    __try
    {
        g_EngineEnableRefrPre(refr);
        if (refr->flags & TESObjectREFR::kFlag_IsDisabled)
            g_EngineEnableRefrMain(refr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Power-armor enter internals, IDA-verified 2026-08-02 against Fallout4_disk.i64.
// The Papyrus native SwitchToPowerArmor (sub_1410FBF90) is only a validating
// wrapper with three refusal paths that report to the PAPYRUS log alone --
// invisible to us, same trap class as the Papyrus Disable() enable-parent
// refusal. The real work is sub_140989A80(actor, furnitureRef, 0): head of a
// 3447-byte function with 8 in-exe callers (several of them AI/furniture-use
// paths, i.e. the route vanilla NPCs such as BoS knights take into PA). Every
// player-only block inside it is gated behind `actor == g_player` (camera,
// Pip-Boy, UI), the AI-process block is already null-guarded, and passing 0
// for the third argument skips a further camera/graph block -- so it is safe
// on an AI-disabled proxy actor. Third arg intentionally 0, matching what the
// Papyrus wrapper itself passes.
typedef __int64 (*_EngineSwitchToPowerArmor)(void* actor, void* furnitureRef, char flag);
static RelocAddr<_EngineSwitchToPowerArmor> g_EngineSwitchToPowerArmor(0x989D90);

__declspec(noinline) static bool EngineSwitchToPowerArmorSafe(Actor* actor, TESObjectREFR* chassis)
{
    __try
    {
        g_EngineSwitchToPowerArmor(actor, chassis, 0);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// SEH-isolated read of a ref's inventoryList pointer. IsFormStillLive closes
// the "object was freed" window, but the registry pointer can still be
// stale-but-plausible for a tick, so the first dereference is guarded too.
// noinline is load-bearing under /GL+/LTCG (inlining drops the __try scope).
// Widened from Actor* to TESObjectREFR* alongside DirectInjectInventoryItem --
// inventoryList is a TESObjectREFR-level field, not Actor-specific.
__declspec(noinline) static BGSInventoryList* SafeReadInventoryList(TESObjectREFR* refr)
{
    __try
    {
        return refr->inventoryList;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// Read-only presence test for an extra-data type on a ref, replicating the
// engine's own sub_1402726F0 in C++ so no engine call, no lock, and no hang is
// possible. Extra-data type IDs live at object offset 0x12 (not a virtual);
// ExtraPowerArmor's constructor (sub_1402B1DF0) stamps 0xBB. The presence
// bitfield hangs at ExtraDataList+0x18 and is 0x1B bytes -- both the offset and
// the bound match F4SE's own ExtraDataList::PresenceBitfield declaration.
// Returns 1 = present, 0 = absent, -1 = unreadable.
__declspec(noinline) static int ReadExtraPresenceBit(TESObjectREFR* refr, uint8_t type)
{
    __try
    {
        if (!refr)
            return -1;
        ExtraDataList* list = refr->extraDataList;
        if (!list)
            return 0;
        const uint8_t* bits = *reinterpret_cast<const uint8_t* const*>(
            reinterpret_cast<const uint8_t*>(list) + 0x18);
        if (!bits)
            return 0;
        const unsigned idx = static_cast<unsigned>(type) >> 3;
        if (idx >= 0x1B)
            return -1;
        return (bits[idx] >> (type & 7)) & 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}
// The singleton every engine caller passes in RCX (qword_1430DC000). NOT the same global
// the old code used (0x30DC080) — the dh value logged from that one (0x7FF7DE36F000)
// pointed inside the exe image, i.e. it was never a real heap TESDataHandler*.
static RelocPtr<void*> g_engineDataHandler(0x30E7080);

// C2712: __try and objects-with-destructors (NiPointer) can't live in the same function.
// This helper owns the NiPointer lifetime; SpawnRemoteActor only deals with raw pointers.
// forcePersist=true on the created ref means the engine holds its own reference, so the
// object stays live after our NiPointer destructs here.
static TESObjectREFR* ResolveHandleToREFR(UInt32 handle)
{
    NiPointer<TESObjectREFR> refPtr;
    if (!LookupREFRByHandle(handle, refPtr))
        return nullptr;
    return refPtr.m_pObject;
}

namespace CoSyncGameAPI
{

    // Round 136: separate function (not inline in SpawnRemoteActor) because
    // the VM arg objects below need C++ unwinding, which cannot share a
    // function with SpawnRemoteActor's __try (C2712).
    //
    // - Essential on the shared proxy BASE so the proxy body can never die
    //   locally (death belongs to the real owner; owner HP broadcasts keep
    //   the proxy's bar in sync). kFlagEssential=0x02 is SDK-defined on
    //   TESActorBaseData and mirrors the CK ACBS bit. NOT set: Invulnerable —
    //   an invulnerable proxy takes zero damage, which would blind the
    //   HP-delta damage relay entirely (a TESHitEvent hook would be required
    //   instead).
    // - PlayerFaction membership replaces the old kFlag_Teammate so hostile
    //   NPCs still aggro the proxy without teammate friendly-fire immunity.
    static void SetupPlayerProxyProtections(Actor* actor, TESNPC* npcBase)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::SetupPlayerProxyProtections", "Needs a freshly spawned, non-null proxy actor and its NPC base record. Skips marking the base essential for the real player's own record (form 0x7) so the actual local player never becomes unkillable by accident.");

        // GUARD: never flag base 0x7 — that's the REAL local player's own
        // NPC record (fallback path when CoSync.esp is missing); marking it
        // essential would make the local player unkillable.
        if (npcBase->formID != 0x00000007u)
        {
            npcBase->actorData.flags |= TESActorBaseData::kFlagEssential;
            LOG_INFO("[CoSyncGameAPI] marked proxy base 0x%08X as essential (flags now 0x%llX)",
                npcBase->formID, (unsigned long long)npcBase->actorData.flags);
        }

        // SetFactionRank is NOT queued here — the VM script object for a
        // freshly-created actor is not ready yet; see QueueProxyFactionRank
        // (fired from the deferred tick in CoSyncPlayerManager).
    }

    void QueueProxyFactionRank(Actor* actor)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::QueueProxyFactionRank", "Needs a non-null actor that's still alive and whose script object is ready. Only call this from the delayed post-spawn step, not during SpawnTask::Run.");
        if (!actor || !IsFormStillLive(actor))
        {
            LOG_WARN("[CoSyncGameAPI::QueueProxyFactionRank] actor=%p is gone, skipping", static_cast<void*>(actor));
            return;
        }

        static constexpr uint32_t kPlayerFactionFormID = 0x0001C21Cu;
        TESForm* factionForm = LookupFormByID(kPlayerFactionFormID);
        if (factionForm && factionForm->formType == kFormType_FACT)
        {
            // TESFaction is only forward-declared in the vendored SDK; this
            // local subtype exists purely to give PackValue the FACT kTypeID
            // so the arg reaches the VM as class "Faction" (exact match for
            // SetFactionRank's parameter) instead of base "Form".
            class CoSyncFactionArg : public TESForm
            {
            public:
                enum { kTypeID = kFormType_FACT };
            };

            CoSyncFactionArg* fac = static_cast<CoSyncFactionArg*>(factionForm);
            VMArray<VMVariable> args;
            VMVariable vFac; vFac.Set(&fac);     args.Push(&vFac);
            SInt32 rank = 0;
            VMVariable vRank; vRank.Set(&rank);  args.Push(&vRank);
            static const BSFixedString kSetFactionRank("SetFactionRank");
            CallFunctionNoWait(actor, kSetFactionRank, args);
            LOG_INFO("[CoSyncGameAPI] queued adding the proxy to the player faction");
        }
        else
        {
            LOG_WARN("[CoSyncGameAPI] could not find the player faction (0x%08X) — proxy has no faction", kPlayerFactionFormID);
        }
    }

    // SEH helpers: __try/__except in the parent SpawnRemoteActor forces MSVC to emit
    // MOVDQA saves for XMM6-XMM15 at the __try boundary. With 8 callee-save pushes
    // leaving RSP 8-byte misaligned, those MOVDQA stores crash (#GP -> AV fault=0xFFFFFFFF).
    // Extracting SEH into separate noinline functions keeps SpawnRemoteActor SEH-free
    // so MSVC never generates the misaligned MOVDQA in that frame.

    struct CreateRefResult { uint32_t handle; bool faulted; uint32_t exCode; };

    __declspec(noinline)
    static CreateRefResult CreateReferenceSafe(void* dh, CoSync_NEW_REFR_DATA* spawnData)
    {
        CreateRefResult r{};
        __try
        {
            uint32_t* ret = g_CreateReferenceAtLocation(dh, &r.handle, spawnData);
            LOG_INFO("[CoSyncGameAPI] got back handle=0x%08X (return=%p, address=%p)",
                r.handle, (void*)ret, (void*)&r.handle);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            r.faulted = true;
            r.exCode  = GetExceptionCode();
        }
        return r;
    }

    __declspec(noinline)
    static void SpawnDiagSafe(Actor* actor)
    {
        __try
        {
            auto* b = reinterpret_cast<uint8_t*>(actor);
            UInt32 flags = *reinterpret_cast<UInt32*>(b + 0x10);
            UInt32 refID = *reinterpret_cast<UInt32*>(b + 0x14);
            void* pCell  = *reinterpret_cast<void**>(b + 0xB8);
            void* ldData = *reinterpret_cast<void**>(b + 0xF0);
            void* root   = ldData
                ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ldData) + 0x08)
                : nullptr;
            LOG_INFO("[SpawnDiag] ref=0x%08X flags=0x%08X cell=%p loadedData=%p rootNode=%p",
                refID, flags, pCell, ldData, root);
            if (flags & 0x400) LOG_WARN("[SpawnDiag] the initially-disabled flag is still set, which wasn't expected here");
            if (flags & 0x20)  LOG_WARN("[SpawnDiag] the turned-off flag is set");
            if (!pCell)        LOG_WARN("[SpawnDiag] no parent cell found");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_WARN("[SpawnDiag] this diagnostic check crashed and was caught");
        }
    }

    Actor* SpawnRemoteActor(TESNPC* npcBase, bool isPlayerProxy)
    {
        LOG_INFO("========================================");
        LOG_INFO("[CoSyncGameAPI::SpawnRemoteActor] starting (isPlayerProxy=%d)", isPlayerProxy ? 1 : 0);
        LOG_INFO("========================================");

        if (!npcBase)
        {
            LOG_ERROR("[CoSyncGameAPI] no NPC base was given");
            return nullptr;
        }
        LOG_INFO("[CoSyncGameAPI] NPC base=0x%p formID=0x%08X", npcBase, npcBase->formID);

        Actor* player = CoSyncGetPlayer();
        if (!player)
        {
            LOG_ERROR("[CoSyncGameAPI] could not find the player");
            return nullptr;
        }

        void* dh = *g_engineDataHandler;
        if (!dh)
        {
            LOG_ERROR("[CoSyncGameAPI] the game's data handler pointer is missing");
            return nullptr;
        }

        // Determine which cell/worldspace to spawn into.
        // Spawning at the player's current position guarantees we land in a loaded cell.
        // ALWAYS pass parentCell explicitly: if interior=null for exterior spawns, the engine
        // navigates the worldspace's cell-list to find the current cell, but that list uses a
        // sentinel (0x7FFFFFFFFFFFFFFC) for empty nodes that the function doesn't guard against,
        // causing an AV at [sentinel+0x28]. Passing the real cell directly skips that path.
        TESObjectREFR* pcRefr = static_cast<TESObjectREFR*>(player);
        TESWorldSpace* ws     = CALL_MEMBER_FN(pcRefr, GetWorldspace)();
        TESObjectCELL* cell   = pcRefr->parentCell;  // valid for both interior and exterior

        CoSync_NEW_REFR_DATA spawnData{};
        spawnData.vftable = s_CoSyncNewRefrDataVtbl;
        spawnData.locX = player->pos.x;
        spawnData.locY = player->pos.y;
        spawnData.locZ = player->pos.z;
        spawnData.object      = npcBase;
        spawnData.interior    = cell;
        spawnData.worldSpace  = ws;
        spawnData.initiallyDisabled = 0;   // spawn enabled; no Enable() call needed
        spawnData.unk6B = 1;               // engine default at every call site
        spawnData.unk6C = 1;               // PlaceAtMe path: run post-create init

        LOG_INFO("[CoSyncGameAPI] asking the game to create a new reference...");
        LOG_INFO("[CoSyncGameAPI]   dataHandler=%p npcBase=%p worldspace=%p cell=%p",
            dh, npcBase, ws, cell);
        LOG_INFO("[CoSyncGameAPI]   spawn position=(%.1f,%.1f,%.1f) vtable=%p (does nothing, just a placeholder)",
            spawnData.locX, spawnData.locY, spawnData.locZ, spawnData.vftable);

        CreateRefResult cr = CreateReferenceSafe(dh, &spawnData);
        if (cr.faulted)
        {
            LOG_ERROR("[CoSyncGameAPI] creating the reference crashed — code=0x%08X", cr.exCode);
            return nullptr;
        }
        uint32_t refHandle = cr.handle;

        if (!refHandle)
        {
            LOG_ERROR("[CoSyncGameAPI] creating the reference failed (got an empty handle) for formID=0x%08X",
                npcBase->formID);
            return nullptr;
        }

        TESObjectREFR* refr = ResolveHandleToREFR(refHandle);
        if (!refr)
        {
            LOG_ERROR("[CoSyncGameAPI] could not resolve handle=0x%08X back to a reference (formID=0x%08X)",
                refHandle, npcBase->formID);
            return nullptr;
        }

        // Type is guaranteed Actor by engine contract: we passed a TESNPC as
        // object, so the engine created an ACHR ref — no RTTI needed.
        Actor* actor = static_cast<Actor*>(refr);
        LOG_INFO("[CoSyncGameAPI] spawned actor=0x%p", actor);

        // Player-base proxy: clear templateNPC so first-person animation templates
        // don't override the proxy's own animation graph settings.
        if (isPlayerProxy && npcBase->formID == 0x00000007)
        {
			LOG_ERROR("[CoSyncGameAPI] checking for a template NPC to clear");

            struct TESNPCWithTemplate { uint8_t pad[0x270]; TESNPC* templateNPC; };
            auto* npcT = reinterpret_cast<TESNPCWithTemplate*>(npcBase);
            if (npcT->templateNPC)
            {
                LOG_WARN("[CoSyncGameAPI] found a template NPC, clearing it for this player proxy");
                npcT->templateNPC = nullptr;
            }
        }

        // Mark persistent + initial-state flags
        actor->flags &= 0xFFDFFFFF;
        actor->flags |= 0x00000400;

        // Round 136: player proxies NO LONGER get kFlag_Teammate. Live test
        // (2026-07-05 host log) showed zero [HealthSync] TX DMG while the host
        // shot P2's proxy — teammate status makes the proxy immune to the
        // local player's friendly fire, so its HP never drops and the
        // HP-delta damage relay can never fire. Hostile-NPC aggro toward the
        // proxy is preserved via PlayerFaction membership instead of teammate
        // status (SetupPlayerProxyProtections).
        // Round 138: teammate is now COMPANION-ONLY. World-NPC clones (client
        // spawn-fallback for missing world NPCs) must be neither teammates
        // (friendly-fire immunity would break the damage relay for them, and
        // a hostile raider clone is not an ally) nor player-proxy protected.
        if (isPlayerProxy)
            SetupPlayerProxyProtections(actor, npcBase);

        // Round 124: removed the two raw flag pokes that used to live here
        // (actor+0x1AC bit28 "locomotion-walker skip" and actor+0x2D0 bit1
        // "EnableAI=false, synchronous"). Both were reverse-engineered against
        // the fake-actor era (before the CreateReferenceAtLocation arg-swap
        // fix, when no real actor ever existed) and their true semantics on a
        // real, live Actor are unverified. Writing unknown engine flags into a
        // freshly created actor is the prime suspect for the current
        // "present, activatable ('Talk to Raider'), animatable, but never
        // renders a body" state. AI suppression is handled by the async
        // Papyrus DisableActorAI (EnableAI(false,false)) call the spawn task
        // already makes; the brief race window that motivated the synchronous
        // poke is preferable to corrupted actor state.

        SpawnDiagSafe(actor);

        LOG_INFO("========================================");
        LOG_INFO("[CoSyncGameAPI] spawn finished, actor=0x%p", actor);
        LOG_INFO("========================================");

        return actor;
    }

    TESObjectREFR* SpawnItemAtPosition(TESForm* itemBase, const NiPoint3& pos, const NiPoint3& rot)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::SpawnItemAtPosition", "Needs a non-null item form and a valid player (used to figure out which cell/worldspace to spawn into) — both are checked. Uses the same reference-creation method as SpawnRemoteActor, just without the actor-only setup steps.");
        if (!itemBase)
        {
            LOG_ERROR("[CoSyncGameAPI] SpawnItemAtPosition: no item form was given");
            return nullptr;
        }

        Actor* player = CoSyncGetPlayer();
        if (!player)
        {
            LOG_ERROR("[CoSyncGameAPI] SpawnItemAtPosition: could not find the player");
            return nullptr;
        }

        void* dh = *g_engineDataHandler;
        if (!dh)
        {
            LOG_ERROR("[CoSyncGameAPI] SpawnItemAtPosition: the game's data handler pointer is missing");
            return nullptr;
        }

        TESObjectREFR* pcRefr = static_cast<TESObjectREFR*>(player);
        TESWorldSpace* ws     = CALL_MEMBER_FN(pcRefr, GetWorldspace)();
        TESObjectCELL* cell   = pcRefr->parentCell;

        CoSync_NEW_REFR_DATA spawnData{};
        spawnData.vftable = s_CoSyncNewRefrDataVtbl;
        spawnData.locX = pos.x;
        spawnData.locY = pos.y;
        spawnData.locZ = pos.z;
        spawnData.dirX = rot.x;
        spawnData.dirY = rot.y;
        spawnData.dirZ = rot.z;
        spawnData.object      = itemBase;
        spawnData.interior    = cell;
        spawnData.worldSpace  = ws;
        spawnData.initiallyDisabled = 0;
        spawnData.unk6B = 1;
        spawnData.unk6C = 1;

        CreateRefResult cr = CreateReferenceSafe(dh, &spawnData);
        if (cr.faulted)
        {
            LOG_ERROR("[CoSyncGameAPI] SpawnItemAtPosition: creating the reference crashed — code=0x%08X", cr.exCode);
            return nullptr;
        }
        if (!cr.handle)
        {
            LOG_ERROR("[CoSyncGameAPI] SpawnItemAtPosition: got an empty handle for formID=0x%08X", itemBase->formID);
            return nullptr;
        }

        TESObjectREFR* refr = ResolveHandleToREFR(cr.handle);
        if (!refr)
        {
            LOG_ERROR("[CoSyncGameAPI] SpawnItemAtPosition: could not resolve handle=0x%08X", cr.handle);
            return nullptr;
        }

        refr->flags &= 0xFFDFFFFF;
        refr->flags |= 0x00000400;

        LOG_INFO("[CoSyncGameAPI] SpawnItemAtPosition: spawned ref=0x%08X base=0x%08X position=(%.1f,%.1f,%.1f) rotation=(%.3f,%.3f,%.3f)",
            refr->formID, itemBase->formID, pos.x, pos.y, pos.z, rot.x, rot.y, rot.z);
        return refr;
    }

    namespace
    {
        double GameAPINowSeconds()
        {
            LOG_REQUIREMENT("CoSyncGameAPI::GameAPINowSeconds", "Doesn't touch any game state — just needs the system's performance-counter frequency to be non-zero (assumed, not checked).");
            static double s_freq = [] {
                LARGE_INTEGER f{};
                QueryPerformanceFrequency(&f);
                return double(f.QuadPart);
                }();

            LARGE_INTEGER c{};
            QueryPerformanceCounter(&c);
            return double(c.QuadPart) / s_freq;
        }

        // The raw NiNode write in PositionRemoteActor below only updates the
        // rendered transform -- it never touches the engine's own cell/worldspace
        // membership bookkeeping (TESObjectCELL::objectList et al), which is what
        // the real engine MoveRefrToPosition call does as a separate, earlier step
        // before its own final scene-graph update (IDA-confirmed against
        // sub_141180B30). Left unattended indefinitely, a remote actor's real cell
        // membership drifts out of sync with where it visually appears. This
        // periodically calls the real engine function to resync that bookkeeping,
        // anchored on the local player's current cell/worldspace (same anchor
        // already used safely elsewhere in this codebase, e.g.
        // CoSyncWorldSync.cpp's ClientTickObjectMoves). See
        // [[project_cosync_crash_fixes]] round 24.
        constexpr double kCellResyncInterval = 1.0;

        // 0.3.3 (Stage 1): is `cell` the cell the engine would file `pos` under?
        //
        // TESObjectCELL layout, verified in IDA 1.11.240 against the engine's
        // own cell-name formatter sub_1404C5690 and the accessors
        // sub_1404C5BE0 / sub_1404C5C00:
        //   +0x40 byte bit0  interior flag
        //   +0x50            exterior: EXTERIOR_DATA* ([+0] grid X, [+4] grid Y)
        // 4096 units per exterior cell (Creation Engine grid size).
        __declspec(noinline) bool CellHoldsPosition(TESObjectCELL* cell, TESObjectCELL* localCell,
                                                    const NiPoint3& pos)
        {
            if (!cell)
                return false;
            __try
            {
                const uint8_t* c = reinterpret_cast<const uint8_t*>(cell);
                if (c[0x40] & 1)                       // interior: must be the shared cell
                    return cell == localCell;
                const int32_t* ext = *reinterpret_cast<int32_t* const*>(c + 0x50);
                if (!ext)
                    return false;
                const int32_t gx = static_cast<int32_t>(std::floor(pos.x / 4096.0f));
                const int32_t gy = static_cast<int32_t>(std::floor(pos.y / 4096.0f));
                return ext[0] == gx && ext[1] == gy;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        __declspec(noinline) void ResyncActorCellMembership_Impl(Actor* actor, const NiPoint3& pos, const NiPoint3& rot)
        {
            LOG_REQUIREMENT("CoSyncGameAPI::ResyncActorCellMembership_Impl", "Needs a valid player (checked, returns early if missing). Moves the actor using the game's own move function, which the caller wraps in a crash guard.");
            PlayerCharacter* pc = CoSyncGetPlayer();
            if (!pc)
                return;

            TESObjectREFR* pcRefr = reinterpret_cast<TESObjectREFR*>(pc);

            // Only relocate when the proxy is filed under the WRONG cell for
            // where it now stands (null, another interior, or a different
            // exterior grid square).
            //
            // 0.3.3 (Stage 1): this used to compare against the LOCAL player's
            // cell. A remote player standing one exterior cell away from us is
            // correctly in a different cell, so the test never passed and the
            // proxy got a full engine MoveRefrToPosition (havok controller
            // reset, 3D update) once a second for as long as they stood there:
            // a periodic hitch in the model. Compare against the cell the
            // target position belongs to instead.
            if (CellHoldsPosition(actor->parentCell, pcRefr->parentCell, pos))
                return;

            // 0.3.1: the proxy is only ever moved inside the LOCAL world when
            // its entity is actually here. Someone in another interior or
            // worldspace is hidden by WorldSync's visibility pass instead;
            // moving them to their coordinates in OUR world put them at a
            // meaningless spot whose cell unloaded (the engine then freed the
            // proxy and it respawned at a stale position -- the "tether").
            {
                uint32_t ws = 0xFFFFFFFFu, cell = 0;
                const uint32_t eid = g_CoSyncEntities.GetEntityID(actor);
                if (eid && g_CoSyncPlayerManager.GetEntityLocation(eid, ws, cell) &&
                    !CoSyncPlayerManager::IsLocalLocation(ws, cell))
                    return;
            }

            // FalloutTogether/TiltedEvolution's exact cross-cell mechanism: the
            // engine's MoveTo / MoveRefrToPosition (source, handle, cell, worldspace,
            // pos, rot). Interior target -> cell set, worldspace null; exterior ->
            // worldspace set, cell null (the engine re-derives the grid cell from
            // pos+worldspace). This is what SetPosition/the raw NiNode write could
            // NOT do (no worldspace arg -> couldn't cross an ext<->int boundary).
            // RVA fixed to the IDA-verified 0x01180CD0 (see GameGlobals.cpp) — the
            // old 0x01180A90 was an unrelated inventory functor, so this silently
            // no-op'd for months and proxies never followed through doors.
            TESWorldSpace* ws   = CALL_MEMBER_FN(pcRefr, GetWorldspace)();
            TESObjectCELL* cell = ws ? nullptr : pcRefr->parentCell;

            UInt32   dummyHandle = 0;
            NiPoint3 p = pos;
            NiPoint3 r = rot;
            MoveRefrToPosition(actor, &dummyHandle, cell, ws, &p, &r);
        }

        // C2712: a function-local `static` with a non-trivial constructor (the
        // map below) uses C++11 thread-safe lazy-init machinery that itself needs
        // object unwinding, so it can't share a function with __try/__except.
        // Kept separate from ResyncActorCellMembership() (which owns the map) so
        // this one only ever takes plain pointer/value parameters.
        //
        // Round 137: RE-ENABLED. This was stubbed to a no-op during the round-122
        // bisection; that crash was later pinned on NpcAnimSync_Hook, not this.
        // The stub had a severe side effect: PositionRemoteActor's raw NiNode
        // write moves ONLY the rendered body — the actor's real engine position
        // (and its havok/charController collision capsule) stays wherever the
        // engine last placed it. Attacks aimed at the visible proxy therefore
        // pass straight through and its HP never changes — live-confirmed
        // 2026-07-05 round 2: [HealthSync] proxy-HP diag pinned at 80.0 for the
        // whole session, zero TX DMG. MoveRefrToPosition moves the REAL actor,
        // making the proxy hittable where it visually stands, which is what the
        // whole HP-delta damage relay depends on.
        void ResyncActorCellMembershipSafe(Actor* actor, const NiPoint3& pos, const NiPoint3& rot)
        {
            __try
            {
                ResyncActorCellMembership_Impl(actor, pos, rot);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                LOG_ERROR("[Resync] moving the actor crashed but was caught (actor=%p)", static_cast<void*>(actor));
            }
        }

        void ResyncActorCellMembership(Actor* actor, const NiPoint3& pos, const NiPoint3& rot)
        {
            LOG_REQUIREMENT("CoSyncGameAPI::ResyncActorCellMembership", "Uses the actor pointer as a lookup key without checking for null — relies on the caller having already confirmed the actor isn't null.");
            static std::unordered_map<Actor*, double> s_lastResync;

            const double now = GameAPINowSeconds();
            double& last = s_lastResync[actor];
            if (now - last < kCellResyncInterval)
                return;
            last = now;

            ResyncActorCellMembershipSafe(actor, pos, rot);
        }
    }

    __declspec(noinline) static void SnapActor_Impl(Actor* actor, const NiPoint3& pos, const NiPoint3& rot)
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc)
            return;
        TESObjectREFR* pcRefr = reinterpret_cast<TESObjectREFR*>(pc);
        // Same rule as ResyncActorCellMembership: never into a world the
        // entity is not in.
        {
            uint32_t lws = 0xFFFFFFFFu, lcell = 0;
            const uint32_t eid = g_CoSyncEntities.GetEntityID(actor);
            if (eid && g_CoSyncPlayerManager.GetEntityLocation(eid, lws, lcell) &&
                !CoSyncPlayerManager::IsLocalLocation(lws, lcell))
                return;
        }
        TESWorldSpace* ws   = CALL_MEMBER_FN(pcRefr, GetWorldspace)();
        TESObjectCELL* cell = ws ? nullptr : pcRefr->parentCell;
        UInt32   handle = 0;
        NiPoint3 p = pos;
        NiPoint3 r = rot;
        MoveRefrToPosition(actor, &handle, cell, ws, &p, &r);
    }

    void SnapActor(Actor* actor, const NiPoint3& pos, const NiPoint3& rot)
    {
        if (!actor || !IsFormStillLive(static_cast<TESForm*>(actor)))
            return;
        __try
        {
            SnapActor_Impl(actor, pos, rot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[Puppet] relocating the actor crashed but was caught (actor=%p)", static_cast<void*>(actor));
        }
    }

    // 0.3.3 (Stage 1): Actor::SetPosition(pos, updateCharController), Actor
    // vtable slot 0xCA = sub_140C60B50 (1.11.240, verified in IDA; the same
    // slot SteerPuppetPosition already uses). With the flag set it writes the
    // reference position (sub_140513DB0), moves the havok character
    // controller to it (sub_141894C50), zeroes the controller velocity and
    // updates the 3D root's translation.
    __declspec(noinline) static void EngineSetActorPosition(Actor* actor, const NiPoint3& pos)
    {
        using SetPos_t = void(*)(Actor*, const NiPoint3*, bool);
        __try
        {
            void** vtbl = *reinterpret_cast<void***>(actor);
            if (vtbl && vtbl[0xCA])
                reinterpret_cast<SetPos_t>(vtbl[0xCA])(actor, &pos, true);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[CoSyncGameAPI] SetPosition on a proxy crashed but was caught (actor=%p)",
                static_cast<void*>(actor));
        }
    }

    void PositionRemoteActor(
        Actor* actor,
        const NiPoint3& pos,
        const NiPoint3& rot)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::PositionRemoteActor", "Needs a non-null actor (checked), and for the visual update, a valid root node from the actor (also checked).");
        if (!actor)
            return;

        // 0.3.3 (Stage 1) -- ROOT CAUSE of the proxy model jitter/flicker.
        //
        // This used to write actor->pos directly and leave the havok character
        // controller wherever the engine last put it. Every frame the engine
        // job "ApplyActorFootIKResults" (job table entry sub_140BD5C40 ->
        // sub_140DB3ED0 over the high process list -> sub_140E1BE80, IDA
        // 1.11.240) reads the controller position and writes it back into the
        // actor with SetPosition (blended toward it at min(15*dt,1) per frame).
        // So each frame the engine pulled the proxy toward the stale controller
        // spot and the CoSync tick pushed it back to the network position: two
        // writers on one actor, and the model visibly alternated between them.
        //
        // Moving the controller with the engine's own SetPosition makes both
        // writers agree: the engine's per-frame sync now reads back the network
        // position. It also puts the collision capsule where the model stands,
        // which is what hits and the HP relay need (previously only fixed up by
        // the 1 Hz cell resync).
        EngineSetActorPosition(actor, pos);

        // Keep the fields in agreement for anything that reads them directly.
        actor->pos.x = pos.x;
        actor->pos.y = pos.y;
        actor->pos.z = pos.z;
        actor->rot.x = rot.x;
        actor->rot.y = rot.y;
        actor->rot.z = rot.z;

        // Keep the engine's own cell/worldspace membership bookkeeping from
        // drifting indefinitely out of sync with the raw transform write below.
        ResyncActorCellMembership(actor, pos, rot);

        // Write directly to the NiNode scene graph for the common case — bypasses
        // MoveRefrToPosition's BSTaskPool dispatch and NavMesh queries on every tick.
        NiNode* root = actor->GetObjectRootNode();
        if (!root)
            return;

        // Round 125: yaw ONLY for the body transform — rot.x is the remote
        // player's camera/aim pitch and must never rotate the skeleton root.
        // Round 126: Pap's NiMatrix43::SetEulerAngles is the Y-UP
        // euclideanspace.com formula: its "heading" rotates about the Y axis,
        // which is HORIZONTAL in Fallout's Z-up world — so the yaw we passed
        // pitched the whole body into the ground whenever the remote player
        // turned ("axis attached to the mouse"). Build the Z-up yaw matrix
        // directly (Bethesda SetEulerAnglesXYZ convention, pure-yaw case:
        // [c s 0; -s c 0; 0 0 1]) and zero the pad column explicitly —
        // NiMatrix43 is a stack local, so its 4th column is garbage otherwise.
        // Round 128: convention now GROUND-TRUTH-CONFIRMED via [RotDiag] (local
        // player rotZ vs engine-authored root m_worldTransform.rot). Example:
        // rotZ=4.9284 → cos=0.214, sin=−0.977; engine row0=[0.214, +0.977, 0]
        // = [cos, −sin, 0]; row1=[sin, cos, 0]. The round-126 matrix was the
        // transpose (rotation by −θ) → mirrored facing ("actor faces left").
        NiMatrix43 rotMat;
        const float cz = cosf(rot.z);
        const float sz = sinf(rot.z);
        rotMat.data[0][0] = cz;   rotMat.data[0][1] = -sz;  rotMat.data[0][2] = 0.0f; rotMat.data[0][3] = 0.0f;
        rotMat.data[1][0] = sz;   rotMat.data[1][1] = cz;   rotMat.data[1][2] = 0.0f; rotMat.data[1][3] = 0.0f;
        rotMat.data[2][0] = 0.0f; rotMat.data[2][1] = 0.0f; rotMat.data[2][2] = 1.0f; rotMat.data[2][3] = 0.0f;

        root->m_localTransform.rot = rotMat;
        root->m_localTransform.pos = pos;
        root->m_worldTransform.rot = rotMat;
        root->m_worldTransform.pos = pos;

        // Propagate updated root transform down through all child bones
        NiAVObject::NiUpdateData ctx;
        root->UpdateWorldData(&ctx);
    }

    __declspec(noinline) static void PositionRefRaw_Impl(
        TESObjectREFR* refr,
        const NiPoint3& pos,
        float yaw,
        float tumble)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::PositionRefRaw_Impl", "Needs a non-null reference with a valid root node — both checked. The caller wraps this in a crash guard because the game can free the reference between our check and this write.");
        if (!refr)
            return;

        refr->pos.x = pos.x;
        refr->pos.y = pos.y;
        refr->pos.z = pos.z;
        refr->rot.z = yaw;

        NiNode* root = refr->GetObjectRootNode();
        if (!root)
            return;

        // Rz(yaw) * Ry(tumble). Rz uses the SAME Z-up convention proven in
        // PositionRemoteActor (round 128, ground-truth-confirmed against the
        // engine-authored root transform) -- [cos, -sin, 0; sin, cos, 0] --
        // NOT Pap's NiMatrix43::SetEulerAngles, whose "heading" rotates about
        // the Y axis and would pitch the object into the ground.
        const float cz = cosf(yaw),    sz = sinf(yaw);
        const float ct = cosf(tumble), st = sinf(tumble);

        NiMatrix43 rotMat;
        rotMat.data[0][0] = cz * ct;  rotMat.data[0][1] = -sz;  rotMat.data[0][2] = cz * st;  rotMat.data[0][3] = 0.0f;
        rotMat.data[1][0] = sz * ct;  rotMat.data[1][1] =  cz;  rotMat.data[1][2] = sz * st;  rotMat.data[1][3] = 0.0f;
        rotMat.data[2][0] = -st;      rotMat.data[2][1] = 0.0f; rotMat.data[2][2] = ct;       rotMat.data[2][3] = 0.0f;

        root->m_localTransform.rot = rotMat;
        root->m_localTransform.pos = pos;
        root->m_worldTransform.rot = rotMat;
        root->m_worldTransform.pos = pos;

        NiAVObject::NiUpdateData ctx;
        root->UpdateWorldData(&ctx);
    }

    void PositionRefRaw(
        TESObjectREFR* refr,
        const NiPoint3& pos,
        float yaw,
        float tumble)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::PositionRefRaw", "Wraps the real work in a crash guard — a stand-in reference can get freed by the game (cell unload, deletion) between the liveness check and the write, and a crash here must not bring down the whole game.");
        __try
        {
            PositionRefRaw_Impl(refr, pos, yaw, tumble);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_UPDATED_WARN("[CoSyncGameAPI] positioning crashed but was recovered from (reference skipped)");
        }
    }

    bool GetActorWorldTransform(
        Actor* actor,
        NiPoint3& outPos,
        NiPoint3& outRot)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::GetActorWorldTransform", "Needs a non-null actor (checked) — returns false without writing anything if the actor is null.");
        if (!actor)
            return false;

        outPos = NiPoint3(actor->pos.x, actor->pos.y, actor->pos.z);
        outRot = NiPoint3(actor->rot.x, actor->rot.y, actor->rot.z);
        return true;
    }

    // UPDATE 2026-06-17: re-verified via IDA on 1.11.221 -- Actor.EnableAI is
    // registered as NativeFunction2<Actor,void,bool,bool> (confirmed via its
    // "EnableAI"/"Actor" registration string, sub_1410FC980), i.e. it takes
    // TWO bool arguments (abEnable, abClearScripts), matching Bethesda's
    // documented signature. Both call sites below were packing the wrong
    // argument count -- DisableActorAI pushed only one bool, EnableActorAI
    // pushed zero -- so the VM was unpacking a missing/garbage second
    // parameter on every call, independent of whether CallFunctionNoWait's
    // RVA was correct. This is a more concrete, directly-confirmed candidate
    // for the "EnableActorAI/DisableActorAI never actually ran" theory in
    // CoSyncPlayer.cpp's player-proxy animation crash investigation than the
    // RVA alone. Fixed to push both required arguments.
    void DisableActorAI(Actor* actor)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::DisableActorAI", "Needs a non-null actor (checked) and a reachable script VM. Passes both arguments EnableAI expects.");
        LOG_INFO("[CoSyncGameAPI::DisableActorAI] starting for actor=0x%p", actor);

        if (!actor)
        {
            LOG_WARN("[CoSyncGameAPI::DisableActorAI] no actor was given");
            return;
        }
        if (!IsFormStillLive(static_cast<TESForm*>(actor)))
        {
            LOG_WARN("[CoSyncGameAPI::DisableActorAI] actor=%p is gone, skipping", static_cast<void*>(actor));
            return;
        }

        VMArray<VMVariable> args;
        bool enabled = false;
        VMVariable vEnabled; vEnabled.Set(&enabled); args.Push(&vEnabled);
        bool clearScripts = false;
        VMVariable vClear; vClear.Set(&clearScripts); args.Push(&vClear);
        static const BSFixedString kEnableAI("EnableAI");
        CallFunctionNoWait(actor, kEnableAI, args);
        LOG_INFO("[CoSyncGameAPI::DisableActorAI] done — AI turned off");
    }

    // ── Partner-style puppet steering (Actor vtable, SEH-guarded) ────────────
    // Slots cross-checked against BOTH CoSync's verified DrawWeaponMagicHands =
    // slot 0xC9 (byte 0x648) memory AND commonlibf4 RE/A/Actor.h:
    //   0xC8  SetAvoidanceDisabled(bool)
    //   0xCA  SetPosition(const NiPoint3&, bool updateCharController)
    //   0xCB  KillDying()
    //   0x100 StopCombat()
    //   0x105 SetLifeState(ACTOR_LIFE_STATE)      [kDying = 1]
    // vtable dispatch is stable across builds (no RVA-skew hazard), so these are
    // safe where hand-derived RVAs are not.
    //
    // 2026-08-12: kVtbl_SetAvoidanceDisabled (0xC8) is a VERIFIED NO-OP in this
    // build (1.11.221.0) -- confirmed against Fallout4_disk.i64 (canonical disk
    // IDB, not the skewed dump-IDB): Hex-Rays gives `void nullsub_6203() { ; }`
    // for that exact slot, and every neighboring slot (0xC0-0xD5) resolves to a
    // real, uniquely-sized function, so this isn't a wrong-slot guess -- it's
    // Bethesda's own compiled Actor base class doing nothing here. This matches
    // commonlibf4's own RE/A/Actor.h, which declares the same slot as a literal
    // inline no-op (`virtual void SetAvoidanceDisabled(bool) { return; } // 0C8`)
    // rather than a REL::Relocation call-through like every other real vtable
    // method in that header -- the commonlibf4 authors already knew.
    //
    // The reference this file's puppet design mirrors (commonwealth-online's
    // F4TNpcSync::NeutralizePuppet) never relied on that call alone -- it always
    // paired it with `StopCombat()`, a genuinely virtual, non-trivial function
    // (895 bytes, checks a real combat-state bitfield). CoSync had ported the
    // decorative no-op half of that pattern but not the load-bearing half: a
    // puppet enemy whose local AI enters combat (e.g. against the local client's
    // own player) keeps running its own local combat/movement logic every tick,
    // directly fighting SteerPuppetPosition's network-authoritative placement --
    // this is a real, engine-verified mechanism by which a synced enemy visibly
    // drifts from the host's actual position. kVtbl_StopCombat below, called
    // from CoSyncSpawnTasks.cpp alongside the (harmless but inert) avoidance
    // call, closes that gap.
    namespace
    {
        constexpr size_t kVtbl_SetAvoidanceDisabled = 0xC8;
        constexpr size_t kVtbl_SetPosition          = 0xCA;
        constexpr size_t kVtbl_KillDying            = 0xCB;
        constexpr size_t kVtbl_StopCombat           = 0x100;
        constexpr size_t kVtbl_SetLifeState         = 0x105;
        constexpr int    kLifeState_Dying           = 1;

        // Actor::uiFlags (offset 0x43C, per f4se's GameReferences.h STATIC_ASSERT)
        // is a runtime cache separate from the base-record actorData.flags that
        // Papyrus SetEssential/SetProtected write. IDA-verified 2026-08-18 against
        // 1.11.240's SetLifeState (vtable slot 0x105): it reads these two bits and,
        // if either is set, silently redirects a requested Dying (1) transition to
        // life-state 7 (essential-down) instead -- KillDying (slot 0xCB) then no-ops
        // because it requires the life-state field to read exactly Dying. Any puppet
        // whose base NPC template is Essential/Protected/unique-named keeps this
        // runtime snapshot from actor creation; the existing Papyrus SetEssential/
        // SetProtected calls in InitNpcProxy only ever touch the base form, never
        // this instance field, so it was never actually being cleared.
        constexpr uint32_t kUiFlag_RuntimeEssential = 0x40000;
        constexpr uint32_t kUiFlag_RuntimeProtected = 0x80000;

        template <class Fn>
        __forceinline Fn ActorVFunc(Actor* actor, size_t slot)
        {
            void*** vtbl = reinterpret_cast<void***>(actor);
            if (!vtbl || !*vtbl)
                return nullptr;
            return reinterpret_cast<Fn>((*vtbl)[slot]);
        }
    }

    void SteerPuppetPosition(Actor* actor, const NiPoint3& pos, float headingZ)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::SteerPuppetPosition", "Needs a non-null, still-alive actor. Sets its facing direction, then moves it to the given position through the game's own move function, wrapped in a crash guard. The actor's AI and animation keep running normally — we're only pinning its position.");
        if (!actor || !IsFormStillLive(static_cast<TESForm*>(actor)))
            return;
        using SetPos_t = void(*)(Actor*, const NiPoint3*, bool);
        __try
        {
            actor->rot.z = headingZ;
            SetPos_t fn = ActorVFunc<SetPos_t>(actor, kVtbl_SetPosition);
            if (fn)
                fn(actor, &pos, true);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[Puppet] moving the puppet crashed — actor=%p", static_cast<void*>(actor));
        }
    }

    void SetActorAvoidanceDisabled(Actor* actor, bool disabled)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::SetActorAvoidanceDisabled", "Needs a non-null, still-alive actor. Turns collision-avoidance on or off through the game's own function, wrapped in a crash guard.");
        if (!actor || !IsFormStillLive(static_cast<TESForm*>(actor)))
            return;
        using Fn = void(*)(Actor*, bool);
        __try
        {
            Fn fn = ActorVFunc<Fn>(actor, kVtbl_SetAvoidanceDisabled);
            if (fn)
                fn(actor, disabled);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[Puppet] changing avoidance crashed — actor=%p", static_cast<void*>(actor));
        }
    }

    void StopActorCombat(Actor* actor)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::StopActorCombat", "Needs a non-null, still-alive actor. Only calls the game's stop-combat function if the actor is actually in combat. Wrapped in a crash guard.");
        if (!actor || !IsFormStillLive(static_cast<TESForm*>(actor)))
            return;
        __try
        {
            if (!actor->IsInCombat())
                return;
            using Fn = void(*)(Actor*);
            Fn fn = ActorVFunc<Fn>(actor, kVtbl_StopCombat);
            if (fn)
                fn(actor);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[Puppet] stopping combat crashed — actor=%p", static_cast<void*>(actor));
        }
    }

    // See the header. Offsets: Actor+0x300 = process (the same field
    // Actor::SetPosition / GetCharController read), process+0x08 = middle-high
    // data, +0x496 = update3DModel flags.
    __declspec(noinline) bool HasMiddleHighProcess(Actor* actor)
    {
        if (!actor)
            return false;
        __try
        {
            const uint8_t* process = *reinterpret_cast<uint8_t* const*>(
                reinterpret_cast<const uint8_t*>(actor) + 0x300);
            return process && *reinterpret_cast<void* const*>(process + 0x08) != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    __declspec(noinline) bool GetInventoryEquipState(Actor* actor, uint32_t formID, bool& present, bool& equipped)
    {
        present = false;
        equipped = false;
        if (!actor || !IsFormStillLive(static_cast<TESForm*>(actor)))
            return false;
        __try
        {
            BGSInventoryList* inv = actor->inventoryList;
            if (!inv)
                return true;
            for (UInt32 k = 0; k < inv->items.count; ++k)
            {
                const BGSInventoryItem& item = inv->items[k];
                if (!item.form || item.form->formID != formID)
                    continue;
                present = true;
                for (BGSInventoryItem::Stack* st = item.stack; st; st = st->next)
                    if (st->flags & BGSInventoryItem::Stack::kFlagEquipped) { equipped = true; break; }
                break;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            present = false;
            equipped = false;
            return false;
        }
    }

    __declspec(noinline) bool RequestActorModelUpdate(Actor* actor)
    {
        if (!actor || !IsFormStillLive(static_cast<TESForm*>(actor)))
            return false;
        if (!HasMiddleHighProcess(actor) || !actor->GetObjectRootNode())
            return false;
        using SetUpdate3DFlags_t = void(__fastcall*)(void* process, uint16_t flags);
        static const uintptr_t kRVA_SetUpdate3DFlags = 0x00D363C0;   // sub_140D363C0
        __try
        {
            void* process = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(actor) + 0x300);
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
            reinterpret_cast<SetUpdate3DFlags_t>(base + kRVA_SetUpdate3DFlags)(process, 0x11);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[CoSyncGameAPI] model update request crashed but was caught (actor=%p)",
                static_cast<void*>(actor));
            return false;
        }
    }

    __declspec(noinline) int GetLifeState(Actor* actor)
    {
        if (!actor)
            return -1;
        __try
        {
            const uint32_t st = *reinterpret_cast<const uint32_t*>(
                reinterpret_cast<const uint8_t*>(actor) + 0x130);
            return static_cast<int>((st >> 17) & 0xFu);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    bool IsDeadOrDying(Actor* actor)
    {
        const int ls = GetLifeState(actor);
        return ls == 1 || ls == 2;
    }

    void KillPuppet(Actor* actor)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::KillPuppet", "Needs a non-null, still-alive actor. Sets its life state to dying, then triggers a real death (ragdoll, loot) through the game's own functions. Wrapped in a crash guard.");
        if (!actor || !IsFormStillLive(static_cast<TESForm*>(actor)))
            return;
        using SetLife_t   = void(*)(Actor*, int);
        using KillDying_t = void(*)(Actor*);
        __try
        {
            actor->uiFlags &= ~(kUiFlag_RuntimeEssential | kUiFlag_RuntimeProtected);

            SetLife_t setLife = ActorVFunc<SetLife_t>(actor, kVtbl_SetLifeState);
            if (setLife)
                setLife(actor, kLifeState_Dying);
            KillDying_t kd = ActorVFunc<KillDying_t>(actor, kVtbl_KillDying);
            if (kd)
                kd(actor);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[Puppet] killing the puppet crashed — actor=%p", static_cast<void*>(actor));
        }
    }

    // Reads the actor's REAL current combat target (who its AI is actually
    // fighting), for the host to broadcast so every peer's puppet attacks the
    // same actor rather than a nearest-player guess.
    //
    // Offset IDA-verified 2026-08-07 against Fallout4.exe 1.11.221.0 via the
    // console `GetCombatTarget` command handler (dumper fn_GetCombatTarget_08x
    // at RVA 0x005CD840): after resolving the subject actor it does
    //     ecx = [actor+0x380];          // 32-bit BSPointerHandle
    //     if (ecx == 0) -> no target
    //     <decode index/generation, index<<4 + [0x30DA410], validate gen>
    //     print [refr+0x14] (formID)
    // i.e. Actor+0x380 is the combat-target handle, resolved through the exact
    // same handle table this file already uses via LookupREFRByHandle. We only
    // ever call this on the host's REAL, AI-alive NPCs (a plain uint32 read, no
    // vtable dispatch, no actorValueOwner), so it is safe on any live actor.
    static constexpr uintptr_t kActor_CombatTargetHandle = 0x380;

    // Raw uint32 read of the combat-target handle, isolated in its own function
    // because it uses __try and therefore cannot coexist with an unwinding
    // object (the NiPointer below) in the same scope.
    __declspec(noinline) static UInt32 SafeReadCombatHandle(Actor* actor)
    {
        __try
        {
            return *reinterpret_cast<const UInt32*>(
                reinterpret_cast<const uint8_t*>(actor) + kActor_CombatTargetHandle);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    Actor* GetCombatTargetActor(Actor* actor)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::GetCombatTargetActor", "Needs a non-null actor. Reads its current combat-target handle and resolves it to the actual actor. Wrapped in a crash guard; returns null if there's no target. Meant for real host-side NPCs, though the read itself is safe on any live actor.");
        if (!actor)
            return nullptr;

        const UInt32 handle = SafeReadCombatHandle(actor);
        if (handle == 0 || handle == 0xFFFFFFFFu)
            return nullptr;

        // LookupREFRByHandle validates the handle (index + generation) and
        // returns false for stale/invalid ones, so this is safe on its own.
        NiPointer<TESObjectREFR> refPtr;
        if (!LookupREFRByHandle(handle, refPtr) || !refPtr.m_pObject)
            return nullptr;

        TESObjectREFR* refr = refPtr.m_pObject;
        // Same static_cast-by-formType contract used throughout this codebase
        // (broken RTTI DYNAMIC_CAST avoided): an ACHR/actor ref always has an
        // NPC_ base form.
        if (!refr->baseForm || refr->baseForm->formType != kFormType_NPC_)
            return nullptr;
        return static_cast<Actor*>(refr);
    }

    void EnableActorAI(Actor* actor)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::EnableActorAI", "Needs a non-null actor (checked) and a reachable script VM. Passes both arguments EnableAI expects.");
        if (!actor)
            return;
        if (!IsFormStillLive(static_cast<TESForm*>(actor)))
        {
            LOG_WARN("[CoSyncGameAPI::EnableActorAI] actor=%p is gone, skipping", static_cast<void*>(actor));
            return;
        }

        VMArray<VMVariable> args;
        bool enabled = true;
        VMVariable vEnabled; vEnabled.Set(&enabled); args.Push(&vEnabled);
        bool clearScripts = false;
        VMVariable vClear; vClear.Set(&clearScripts); args.Push(&vClear);
        static const BSFixedString kEnableAI("EnableAI");
        CallFunctionNoWait(actor, kEnableAI, args);
    }

    // Counterpart to DespawnProxyActor's Disable() — re-enables a temporarily
    // despawned proxy via ObjectReference.Enable(false).
    void EnableProxyActor(Actor* actor)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::EnableProxyActor", "Needs a non-null, still-alive actor (checked). Calls the game's own internal enable function directly on the game thread — this works even on references that have an enable-state parent, which the normal script Enable() call silently refuses to touch.");
        if (!actor)
        {
            LOG_WARN("[CoSyncGameAPI::EnableProxyActor] no actor was given");
            return;
        }
        if (!IsFormStillLive(static_cast<TESForm*>(actor)))
        {
            LOG_WARN("[CoSyncGameAPI::EnableProxyActor] actor=%p is gone, skipping", static_cast<void*>(actor));
            return;
        }

        const bool ok = EngineEnableRefrSafe(static_cast<TESObjectREFR*>(actor));
        LOG_INFO("[CoSyncGameAPI::EnableProxyActor] enable call %s, ref=0x%08X",
            ok ? "OK" : "FAULTED (caught)", actor->formID);
    }

    // Round 101: until now, despawning a remote-player proxy (disconnect/
    // EntityDestroy) only erased CoSync's own bookkeeping (entity registry,
    // CoSyncPlayer wrapper, cached state) -- it never told the game engine to
    // do anything with the actual spawned NPC actor. Confirmed via IDA: the
    // real TESObjectREFR::Disable() is vtable slot 176 (0xB0, matches
    // CommonLibF4's documented slot exactly -- decompile shows it checking
    // formFlags bit 11 (0x800, the same bit IsDisabled() reads) before doing
    // real engine-side teardown: AI/process-list removal, equipment/combat
    // state cleanup. Without ever calling this, every proxy left over from a
    // disconnect remains standing in the world as a permanent, frozen,
    // fully-solid ghost actor for the other player -- a guaranteed,
    // 100%-reproducible bug on every single disconnect, not an edge case.
    // Routed through the same CallFunctionNoWait/Papyrus-VM mechanism already
    // proven correct in this file for EnableAI/DisableActorAI (ObjectReference
    // .Disable(bool abFadeOut) is one of the most stable, unchanged-since-
    // Skyrim native Papyrus functions) rather than a new raw vtable-dispatch
    // path, to reuse already-trusted argument-marshaling plumbing.
    void DespawnProxyActor(Actor* actor)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::DespawnProxyActor", "Needs a non-null actor (checked) and a reachable script VM. Relies on the built-in Disable() script function, which is a stable, well-tested part of the engine.");
        if (!actor)
            return;
        if (!IsFormStillLive(static_cast<TESForm*>(actor)))
        {
            LOG_WARN("[CoSyncGameAPI::DespawnProxyActor] actor=%p is gone, skipping", static_cast<void*>(actor));
            return;
        }

        VMArray<VMVariable> args;
        bool fadeOut = false;
        VMVariable vFade; vFade.Set(&fadeOut); args.Push(&vFade);
        static const BSFixedString kDisable("Disable");
        CallFunctionNoWait(actor, kDisable, args);

        // Round 140: Delete() after Disable so despawned CoSync-created refs
        // (proxies, clones, ghosts) actually leave the save instead of
        // persisting as disabled refs forever (both calls queue in order on
        // the ref's VM queue).
        static const BSFixedString kDelete("Delete");
        VMArray<VMVariable> delArgs;
        CallFunctionNoWait(actor, kDelete, delArgs);

        LOG_INFO("[CoSyncGameAPI::DespawnProxyActor] queued disable and delete for actor=0x%p", actor);
    }

    void DisableWorldActor(TESObjectREFR* refr)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::DisableWorldActor", "Needs a non-null, still-alive reference (checked). Calls the game's own internal disable function directly on the game thread — unlike the script/console Disable command, this also works on references that have an enable-state parent (like camp NPCs placed in the editor). Don't use this on player proxies — use DespawnProxyActor for those instead.");
        if (!refr)
            return;
        if (!IsFormStillLive(static_cast<TESForm*>(refr)))
        {
            LOG_WARN("[CoSyncGameAPI::DisableWorldActor] ref=%p is gone, skipping", static_cast<void*>(refr));
            return;
        }
        if (refr->flags & (TESObjectREFR::kFlag_IsDisabled | TESObjectREFR::kFlag_IsDeleted))
            return;

        const bool ok = EngineDisableRefrSafe(refr);
        LOG_INFO("[CoSyncGameAPI::DisableWorldActor] disable call %s, ref=0x%08X",
            ok ? "OK" : "FAULTED (caught)", refr->formID);
    }

    void EnableWorldActor(TESObjectREFR* refr)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::EnableWorldActor", "Needs a non-null, still-alive reference (checked). Uses the same enable path as EnableProxyActor, but works on any reference, not just actors.");
        if (!refr)
            return;
        if (!IsFormStillLive(static_cast<TESForm*>(refr)))
        {
            LOG_WARN("[CoSyncGameAPI::EnableWorldActor] ref=%p is gone, skipping", static_cast<void*>(refr));
            return;
        }

        const bool ok = EngineEnableRefrSafe(refr);
        LOG_INFO("[CoSyncGameAPI::EnableWorldActor] enable call %s, ref=0x%08X",
            ok ? "OK" : "FAULTED (caught)", refr->formID);
    }

    int GetActorPowerArmorExtraState(Actor* actor)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::GetActorPowerArmorExtraState", "Needs a non-null, still-alive actor (checked). Just reads a bit of memory to check power armor state — no engine calls, no locks, can't hang. Wrapped in a crash guard so a bad pointer returns -1 instead of crashing.");
        if (!actor || !IsFormStillLive(static_cast<TESForm*>(actor)))
            return -1;
        return ReadExtraPresenceBit(static_cast<TESObjectREFR*>(actor), kExtraTypePowerArmor);
    }

    bool SwitchToPowerArmorDirect(Actor* actor, TESObjectREFR* chassis)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::SwitchToPowerArmorDirect", "Needs a non-null, still-alive actor and chassis reference (both checked), and must run on the game thread. Calls the game's real power-armor-enter function directly, skipping the script wrapper's silent refusal checks. Wrapped in a crash guard.");
        if (!actor || !chassis)
            return false;
        if (!IsFormStillLive(static_cast<TESForm*>(actor)) ||
            !IsFormStillLive(static_cast<TESForm*>(chassis)))
        {
            LOG_WARN("[CoSyncGameAPI::SwitchToPowerArmorDirect] actor or chassis is gone, skipping");
            return false;
        }

        const int before = ReadExtraPresenceBit(static_cast<TESObjectREFR*>(actor), kExtraTypePowerArmor);
        const bool ok = EngineSwitchToPowerArmorSafe(actor, chassis);
        const int after = ReadExtraPresenceBit(static_cast<TESObjectREFR*>(actor), kExtraTypePowerArmor);

        LOG_UPDATED("[CoSyncGameAPI::SwitchToPowerArmorDirect] power armor enter %s actor=0x%08X chassis=0x%08X state before=%d after=%d",
            ok ? "OK" : "FAULTED (caught)", actor->formID, chassis->formID, before, after);
        return ok;
    }

    // Round 123: proxies spawn as invisible skeletons — anim graph (299 vars),
    // root node, and position all correct (nameplate renders at the right
    // spot), but no body geometry. Our direct CreateReferenceAtLocation call
    // skips the template/facegen/outfit resolution a normal PlaceAtMe runs.
    // A Disable()+Enable() cycle makes the engine rebuild the 3D through its
    // full appearance pipeline. Both calls queue in order on the same ref's
    // VM queue (same trusted plumbing as EnableProxyActor/DespawnProxyActor).
    // 2026-07-21: was queuing ObjectReference.Disable(false)/Enable(false) via
    // CallFunctionNoWait (async Papyrus). IDA + a live crash proved this
    // leaves a real window: Disable() actually executes on some VM tick,
    // detaching the actor from its cell (TESObjectREFR::parentCell -> null)
    // -- and the queued Enable() doesn't necessarily run the same tick, since
    // Papyrus VM queue processing isn't instant. A direct register match
    // (crash R12/RSI == this exact proxy actor's pointer/formID, logged null
    // parentCell by the PreSaveGame diagnostic microseconds earlier) confirmed
    // the engine's own BGSSaveLoadGame::SaveGame() can land inside that gap
    // and crash reading a null location descriptor for this actor while it's
    // mid-cycle. Switched to the same native, synchronous Disable/Enable pair
    // already used elsewhere in this file for exactly this reason (no VM
    // queue, no gap between the two calls) -- same visual effect (forces the
    // engine to tear down and rebuild the 3D/anim graph), just atomic.
    void RefreshActor3D(Actor* actor)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::RefreshActor3D", "Needs a non-null, still-alive actor (checked). Disables then re-enables it directly on the game thread, back to back, so there's no gap between the two steps.");
        if (!actor)
        {
            LOG_WARN("[CoSyncGameAPI::RefreshActor3D] no actor was given");
            return;
        }
        if (!IsFormStillLive(static_cast<TESForm*>(actor)))
        {
            LOG_WARN("[CoSyncGameAPI::RefreshActor3D] actor=%p is gone, skipping", static_cast<void*>(actor));
            return;
        }

        DisableWorldActor(static_cast<TESObjectREFR*>(actor));
        EnableProxyActor(actor);
        LOG_INFO("[CoSyncGameAPI::RefreshActor3D] disable/enable refresh done for actor=0x%p", actor);
    }

    bool TryRefreshActor3DThrottled(uint32_t entityID, Actor* actor, double cooldownSeconds)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::TryRefreshActor3DThrottled", "Needs a non-null actor (checked; the actual liveness check happens inside RefreshActor3D). Main thread only. Shares one cooldown tracker across every caller so two different systems can't both refresh the same entity at once.");
        if (!actor) return false;

        static std::unordered_map<uint32_t, double> s_lastRefreshAt;

        LARGE_INTEGER freq{}, cnt{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&cnt);
        const double now = static_cast<double>(cnt.QuadPart) / static_cast<double>(freq.QuadPart);

        double& last = s_lastRefreshAt[entityID];
        if (now - last < cooldownSeconds)
            return false;
        last = now;

        RefreshActor3D(actor);
        return true;
    }

    void InitNpcProxy(Actor* actor)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::InitNpcProxy", "Needs a non-null actor (checked). Runs after RefreshActor3D so these calls happen after the actor is fully enabled. Clears any essential/protected flags the base NPC might carry (like quest-essential enemies), and sends an idle animation event to get the actor out of its default pose once its animation graph is running.");
        if (!actor)
            return;
        if (!IsFormStillLive(static_cast<TESForm*>(actor)))
        {
            LOG_WARN("[CoSyncGameAPI::InitNpcProxy] actor=%p is gone, skipping", static_cast<void*>(actor));
            return;
        }

        // Quest-essential NPCs inherit kFlagEssential from their base form;
        // override at the instance level so the proxy can take damage.
        {
            static const BSFixedString kSetEssential("SetEssential");
            VMArray<VMVariable> args;
            bool v = false;
            VMVariable vv; vv.Set(&v); args.Push(&vv);
            CallFunctionNoWait(actor, kSetEssential, args);
        }
        {
            static const BSFixedString kSetProtected("SetProtected");
            VMArray<VMVariable> args;
            bool v = false;
            VMVariable vv; vv.Set(&v); args.Push(&vv);
            CallFunctionNoWait(actor, kSetProtected, args);
        }

        // Fire the default-idle event after Enable() to break out of A-pose.
        // This uses SendAnimationEvent(String) — the BSFixedString arg is
        // packed into a VMVariable via Set<BSFixedString>.
        {
            static BSFixedString evName("IdleForceDefaultState");
            static const BSFixedString kSendAnimationEvent("SendAnimationEvent");
            VMArray<VMVariable> args;
            VMVariable vv; vv.Set(&evName); args.Push(&vv);
            CallFunctionNoWait(actor, kSendAnimationEvent, args);
        }

        LOG_INFO("[CoSyncGameAPI::InitNpcProxy] queued essential/protected clear and idle animation for actor=%p",
            static_cast<void*>(actor));
    }

    // Scan the local player's inventory to find a live BGSObjectInstanceExtra
    // (kExtraData_ObjectInstance = 0x35) and cache its vtable. All instances of
    // BGSObjectInstanceExtra share the same vtable regardless of weapon/armor type,
    // so one successful lookup persists for the whole session.
    static uintptr_t s_instanceExtraVtable = 0;

    static uintptr_t FindOrCacheInstanceExtraVtable()
    {
        if (s_instanceExtraVtable) return s_instanceExtraVtable;

        PlayerCharacter* player = CoSyncGetPlayer();
        if (!player) return 0;
        BGSInventoryList* pinv = player->inventoryList;
        if (!pinv) return 0;

        for (UInt32 i = 0; i < pinv->items.count; ++i)
        {
            BGSInventoryItem::Stack* stk = pinv->items.entries[i].stack;
            while (stk)
            {
                ExtraDataList* edl = stk->extraData;
                if (edl)
                {
                    BSExtraData* node = edl->m_data;
                    while (node)
                    {
                        if (node->type == kExtraData_ObjectInstance)
                        {
                            s_instanceExtraVtable = *(const uintptr_t*)node;
                            return s_instanceExtraVtable;
                        }
                        node = node->next;
                    }
                }
                stk = stk->next;
            }
        }
        return 0;
    }

    // ── Engine-native item add / remove (IDA 1.11.240, 2026-09-26) ──────────
    // The workers Papyrus AddItem / RemoveItem run on the game thread (their
    // queued functors: AddItemFunctor::Run 0x14114E210 -- "Cannot add a None
    // item to a container" -- and RemoveItemFunctor::Run 0x14114E930 --
    // "Cannot remove a None item from a container"):
    //   add    0x141173A90 (container, item, count, silent, stackID, vm)
    //          builds the item list (leveled lists resolved) and adds it via
    //          the container's own add path (0x140503FD0) -> real stacks.
    //   remove 0x141182A90 (container, item, count, silent, destination,
    //          a6, stackID, vm) -- destination null = the items are destroyed.
    // The VM and stackID are only used to report script errors, so the game
    // VM is passed for those. Papyrus itself can't be used on proxies: they
    // have no script object, so the queued call never runs (round 145).
    // (0.0.92/0.0.93 wired 0x141182A90 as "add" by mistake -- IDA's vtable
    // labels for the two functors are swapped. It never added anything.)
    constexpr uintptr_t kRVA_AddItemWorker    = 0x01173A90;
    constexpr uint8_t   kAddItemWorkerPrologue[10]    = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08 };
    constexpr uintptr_t kRVA_RemoveItemWorker = 0x01182A90;
    constexpr uint8_t   kRemoveItemWorkerPrologue[10] = { 0x48, 0x8B, 0xC4, 0x44, 0x89, 0x40, 0x18, 0x48, 0x89, 0x48 };
    using AddItemWorker_t    = int64_t (*)(TESObjectREFR* container, TESForm* item, UInt32 count, bool silent,
                                           UInt32 stackID, void* vm);
    using RemoveItemWorker_t = int64_t (*)(TESObjectREFR* container, TESForm* item, UInt32 count, bool silent,
                                           TESObjectREFR* destination, bool a6, UInt32 stackID, void* vm);

    static void* NativeVM()
    {
        GameVM* gvm = *g_gameVM;
        return gvm ? static_cast<void*>(gvm->m_virtualMachine) : nullptr;
    }

    static bool WorkerOk(uintptr_t rva, const uint8_t (&prologue)[10], const char* what)
    {
        if (std::memcmp(reinterpret_cast<const void*>(RelocationManager::s_baseAddr + rva), prologue, sizeof(prologue)) == 0)
            return true;
        static bool s_warned[2] = {};
        bool& warned = s_warned[rva == kRVA_AddItemWorker ? 0 : 1];
        if (!warned) { warned = true; LOG_ERROR("[%s] prologue mismatch (not 1.11.240?) -- disabled", what); }
        return false;
    }

    __declspec(noinline) static bool NativeAddItem_Impl(TESObjectREFR* target, TESForm* item, UInt32 count)
    {
        if (!WorkerOk(kRVA_AddItemWorker, kAddItemWorkerPrologue, "NativeAddItem"))
            return false;
        void* vm = NativeVM();
        if (!vm)
            return false;
        reinterpret_cast<AddItemWorker_t>(RelocationManager::s_baseAddr + kRVA_AddItemWorker)(target, item, count, true, 0, vm);
        return true;
    }

    __declspec(noinline) static bool NativeRemoveItem_Impl(TESObjectREFR* target, TESForm* item, UInt32 count)
    {
        if (!WorkerOk(kRVA_RemoveItemWorker, kRemoveItemWorkerPrologue, "NativeRemoveItem"))
            return false;
        void* vm = NativeVM();
        if (!vm)
            return false;
        reinterpret_cast<RemoveItemWorker_t>(RelocationManager::s_baseAddr + kRVA_RemoveItemWorker)(
            target, item, count, true, nullptr, false, 0, vm);
        return true;
    }

    bool NativeRemoveItem(TESObjectREFR* target, TESForm* item, UInt32 count)
    {
        if (!target || !item || !IsFormStillLive(static_cast<TESForm*>(target)))
            return false;
        __try
        {
            return NativeRemoveItem_Impl(target, item, count);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[NativeRemoveItem] removing 0x%08X from ref 0x%08X crashed but was caught", item->formID, target->formID);
            return false;
        }
    }

    bool NativeAddItem(TESObjectREFR* target, TESForm* item, UInt32 count)
    {
        if (!target || !item || !IsFormStillLive(static_cast<TESForm*>(target)))
            return false;
        __try
        {
            return NativeAddItem_Impl(target, item, count);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[NativeAddItem] adding 0x%08X to ref 0x%08X crashed but was caught", item->formID, target->formID);
            return false;
        }
    }

    // Papyrus AttachModToInventoryItem's worker (0x141180DE0, called directly
    // by the native 0x141153A50 -- synchronous): (vm, stackID, ref, item, mod,
    // attach). Needs exactly ONE of the item in ref; afterwards it refreshes
    // the item (0x141181E20), so an equipped piece updates on the actor.
    constexpr uintptr_t kRVA_AttachModWorker = 0x01180DE0;
    constexpr uint8_t   kAttachModWorkerPrologue[10] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10 };
    using AttachModWorker_t = int64_t (*)(void* vm, UInt32 stackID, TESObjectREFR* ref, TESForm* item, TESForm* mod, bool attach);

    __declspec(noinline) static bool NativeAttachMod_Impl(TESObjectREFR* ref, TESForm* item, TESForm* mod)
    {
        const uintptr_t fn = RelocationManager::s_baseAddr + kRVA_AttachModWorker;
        if (std::memcmp(reinterpret_cast<const void*>(fn), kAttachModWorkerPrologue, sizeof(kAttachModWorkerPrologue)) != 0)
        {
            static bool s_warned = false;
            if (!s_warned) { s_warned = true; LOG_ERROR("[NativeAttachMod] prologue mismatch (not 1.11.240?) -- disabled"); }
            return false;
        }
        GameVM* gvm = *g_gameVM;
        void* vm = gvm ? static_cast<void*>(gvm->m_virtualMachine) : nullptr;
        if (!vm)
            return false;
        return (reinterpret_cast<AttachModWorker_t>(fn)(vm, 0, ref, item, mod, true) & 0xFF) != 0;
    }

    bool NativeAttachMod(TESObjectREFR* ref, TESForm* item, TESForm* mod)
    {
        if (!ref || !item || !mod || !IsFormStillLive(static_cast<TESForm*>(ref)))
            return false;
        __try
        {
            return NativeAttachMod_Impl(ref, item, mod);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[NativeAttachMod] mod 0x%08X on 0x%08X (ref 0x%08X) crashed but was caught",
                mod->formID, item->formID, ref->formID);
            return false;
        }
    }

    // ExtraDataList::ExtraDataList() -- IDA 1.11.240, 2026-09-26: 33 engine
    // callers; zeroes refcount/data/presence, points the tail at m_data and
    // constructs the lock. sub_14034A310 creates a stack's missing list with
    // exactly Allocate(40) -> this ctor -> InterlockedIncrement(refcount).
    typedef void* (*_ExtraDataListCtor)(void* mem);
    static RelocAddr<_ExtraDataListCtor> g_ExtraDataListCtor(0x272A50);
    static constexpr uint8_t kExtraDataListCtorPrologue[9] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };
    static constexpr size_t  kExtraDataListBytes = 0x28;

    int EnsureStackExtraLists(TESObjectREFR* ref, TESForm* item)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::EnsureStackExtraLists", "Main thread. Needs a live ref and a real item form; builds missing stack lists with the engine's own allocator and constructor, under the inventory write lock.");
        if (!ref || !item || !IsFormStillLive(static_cast<TESForm*>(ref)))
            return -1;

        static int s_ctorOk = -1;
        if (s_ctorOk < 0)
        {
            s_ctorOk = (std::memcmp(reinterpret_cast<const void*>(g_ExtraDataListCtor.GetUIntPtr()),
                kExtraDataListCtorPrologue, sizeof(kExtraDataListCtorPrologue)) == 0) ? 1 : 0;
            if (!s_ctorOk)
                LOG_ERROR("[GameAPI] ExtraDataList constructor bytes don't match 1.11.240 -- stack lists won't be created");
        }
        if (!s_ctorOk)
            return -1;

        BGSInventoryList* inv = SafeReadInventoryList(ref);
        if (!inv)
            return 0;

        int created = 0;
        BSWriteLocker lock(&inv->inventoryLock);
        for (UInt32 i = 0; i < inv->items.count; ++i)
        {
            BGSInventoryItem& entry = inv->items.entries[i];
            if (entry.form != item)
                continue;
            for (BGSInventoryItem::Stack* s = entry.stack; s; s = s->next)
            {
                if (s->extraData)
                    continue;
                void* mem = Heap_Allocate(kExtraDataListBytes);
                if (!mem)
                    break;
                auto* list = static_cast<ExtraDataList*>(g_ExtraDataListCtor(mem));
                _InterlockedIncrement(reinterpret_cast<volatile long*>(list));   // refcount (+0x00), as the engine does
                s->extraData = list;
                ++created;
            }
            break;
        }

        if (created > 0)
            LOG_UPDATED("[GameAPI] gave %d stack(s) of item 0x%08X on ref 0x%08X an extra-data list (equip precondition)",
                created, item->formID, ref->formID);
        return created;
    }

    bool DirectInjectInventoryItem(TESObjectREFR* actor, TESForm* form)
    {
        if (!actor || !form) return false;
        // Weapons on actors (remote-player and NPC proxies): add them the
        // engine's own way. The hand-built stack below never got a weapon
        // MODEL -- the proxy played the weapon animations empty-handed (user
        // test 2026-09-26); a natively added weapon has everything its 3D
        // needs. Armor and Power Armor frame contents keep the path below.
        // Phase 8: armor too, so its mods (linings, paint -- Power Armor paint
        // included) can be attached to a real stack.
        // Also Power Armor frames (a FURN ref): the chassis spawned for a
        // remote player's Power Armor gets real piece stacks, so paint/mods
        // attach -- and the hand-built ExtraDataList (the 2026-09-25 crash
        // class) is no longer involved there.
        const bool isFrameRef = actor->formType != kFormType_ACHR && actor->baseForm &&
                                actor->baseForm->formType == kFormType_FURN;
        if ((form->formType == kFormType_WEAP || form->formType == kFormType_ARMO) &&
            (actor->formType == kFormType_ACHR || isFrameRef) &&
            IsFormStillLive(static_cast<TESForm*>(actor)))
        {
            BGSInventoryList* have = SafeReadInventoryList(actor);
            bool present = false;
            if (have)
                for (UInt32 i = 0; i < have->items.count; ++i)
                    if (have->items[i].form && have->items[i].form->formID == form->formID) { present = true; break; }
            if (present)
                return true;
            if (NativeAddItem(actor, form, 1))
            {
                BGSInventoryList* inv2 = SafeReadInventoryList(actor);
                if (inv2)
                    for (UInt32 i = 0; i < inv2->items.count; ++i)
                        if (inv2->items[i].form && inv2->items[i].form->formID == form->formID)
                        {
                            LOG_UPDATED("[DirectInject] item 0x%08X added natively to ref 0x%08X", form->formID, actor->formID);
                            return true;
                        }
                LOG_UPDATED_WARN("[DirectInject] native add of item 0x%08X did not land -- falling back", form->formID);
            }
        }
        // 2026-08-02: this was the ONLY caller-facing entry point in this file
        // without a liveness check, and it crashed for real -- client READ AV at
        // fault 0xFFFFFFFFFFFFFFFF, PDB-resolved to DirectInjectInventoryItem+0x46
        // (the actor->inventoryList read below) from FlushPendingOutfitOps+0x73B,
        // after an exterior->interior transition freed the proxy while OutfitSync
        // still held its pointer. Same freed-actor class as [0.0.30], which was
        // fixed for the tick loop but never for this path.
        if (!IsFormStillLive(static_cast<TESForm*>(actor)))
            return false;
        BGSInventoryList* inv = SafeReadInventoryList(actor);
        if (!inv)
        {
            // 2026-08-26: this WAS an unconditional bail, which is exactly why
            // widening this function to TESObjectREFR* for PA chassis pieces
            // (same session, see CoSyncPowerArmorSync.cpp) failed every single
            // time -- a freshly CreateReferenceAtLocation-spawned FURN chassis
            // has inventoryList == NULL outright (confirmed live via
            // LogInventoryDiag: "inventoryList=NULL (never allocated)"), unlike
            // an actor, which always has one already (populated from its base
            // NPC template's default inventory at spawn -- the assumption this
            // whole function was originally built around and never had reason
            // to question until now).
            //
            // BGSInventoryList (GameFormComponents.h) has NO vtable/virtual
            // methods -- unlike the Stack/ExtraDataList/BGSObjectInstanceExtra
            // structs allocated further down in this same function, which all
            // needed a vtable pointer borrowed from an existing instance
            // because they ARE polymorphic. Its layout is a plain struct of
            // arrays/PODs, so it can be safely default-constructed in place
            // (placement new zero/default-initializes every member -- the
            // tArray items and BSReadWriteLock members get their own real
            // default constructors run, not a raw memset guess) rather than
            // needing anything borrowed from elsewhere.
            void* mem = Heap_Allocate(sizeof(BGSInventoryList));
            if (!mem)
                return false;
            inv = new (mem) BGSInventoryList();
            inv->inventoryWeight = -1.0f;  // matches the engine's own "not yet calculated" convention
            actor->inventoryList = inv;
            LOG_UPDATED("[DirectInject] created a new inventory list for ref=0x%08X (it had none)",
                actor->formID);
        }

        // Already present? (idempotent — counts as success)
        for (UInt32 i = 0; i < inv->items.count; ++i)
            if (inv->items[i].form && inv->items[i].form->formID == form->formID)
                return true;

        // Borrow the vtable pointer from the first existing Stack. The proxy
        // always has ≥2 default ARMO items from the RMPlayer outfit template, so
        // this source is always available when weapon/armor sync fires.
        //
        // 2026-08-26: a fresh chassis (or any other ref with zero starting
        // items -- the whole reason the NULL-inventoryList case above now
        // gets this far at all) has nothing of its own to borrow from. Every
        // BGSInventoryItem::Stack in the game shares the same one vtable
        // (it's one C++ class), so it doesn't matter WHOSE existing item
        // supplies it -- fall back to the local player's own inventory, the
        // same already-proven "guaranteed to have items" source
        // FindOrCacheInstanceExtraVtable uses for exactly this reason.
        uintptr_t vtable = 0;
        if (inv->items.count != 0 && inv->items.entries[0].stack)
        {
            vtable = *(const uintptr_t*)(inv->items.entries[0].stack);
        }
        else
        {
            PlayerCharacter* lp = CoSyncGetPlayer();
            BGSInventoryList* linv = lp ? lp->inventoryList : nullptr;
            if (linv && linv->items.count != 0 && linv->items.entries[0].stack)
                vtable = *(const uintptr_t*)(linv->items.entries[0].stack);
        }
        if (!vtable)
            return false;

        // BGSInventoryItem::Stack 64-bit layout:
        //   +0x00  vtable*          (8)  — virtual ~Stack()
        //   +0x08  refCount UInt32  (4)  — BSIntrusiveRefCounted
        //   +0x0C  pad              (4)
        //   +0x10  Stack* next      (8)
        //   +0x18  ExtraDataList*   (8)
        //   +0x20  SInt32 count     (4)
        //   +0x24  UInt16 flags     (2)
        //   +0x26  pad              (2)
        static constexpr size_t kStackBytes = 0x28;
        uint8_t* mem = (uint8_t*)Heap_Allocate(kStackBytes);
        if (!mem) return false;
        memset(mem, 0, kStackBytes);
        *(uintptr_t*)(mem + 0x00) = vtable;  // vtable pointer
        *(UInt32*)  (mem + 0x08) = 1;        // refCount = 1
        *(SInt32*)  (mem + 0x20) = 1;        // count = 1
        // next=nullptr @0x10, flags=0 @0x24 — zeroed above

        // ExtraDataList layout (BSIntrusiveRefCounted base, no vtable):
        //   +0x00  refCount        (UInt32)          = 1
        //   +0x08  m_data          (BSExtraData*)     → weapon: BGSObjectInstanceExtra
        //   +0x10  unk10           (void*)            = nullptr
        //   +0x18  m_presence      (PresenceBitfield*) → points to pbfMem
        //   +0x20  m_lock          (BSReadWriteLock)  = {0,0} unlocked
        //
        // EquipObject calls LockForRead on m_lock; with extraData=nullptr that
        // was address 0x20 → crash in LockForRead+0xF. Fixed by providing a valid
        // ExtraDataList so the lock is in real memory.
        static constexpr size_t kEDLBytes = 0x28;   // sizeof(ExtraDataList)
        static constexpr size_t kPBFBytes = 0x1B;   // sizeof(PresenceBitfield::bits)
        uint8_t* edlMem = (uint8_t*)Heap_Allocate(kEDLBytes);
        uint8_t* pbfMem = (uint8_t*)Heap_Allocate(kPBFBytes);
        if (!edlMem || !pbfMem)
        {
            if (edlMem) Heap_Free(edlMem);
            if (pbfMem) Heap_Free(pbfMem);
            Heap_Free(mem);
            return false;
        }
        memset(edlMem, 0, kEDLBytes);
        memset(pbfMem, 0, kPBFBytes);
        *(UInt32*)(edlMem + 0x00) = 1;      // refCount = 1
        *(void**) (edlMem + 0x18) = pbfMem; // m_presence → PresenceBitfield
        // +0x10 is NOT padding: it is the list's TAIL -- the address the next
        // appended BSExtraData gets written to (ExtraDataList::Add, 0x1402726A0:
        // `*tail = node; tail = &node->next`). Left null, the first engine-side
        // append wrote to address 0: the 2026-09-25 Power Armor crash
        // (Fallout4.exe+0x2726E3, WRITE fault=0, inside SwitchToPowerArmor on a
        // proxy whose chassis pieces were injected here). Empty list: tail = &m_data.
        *(void**) (edlMem + 0x10) = edlMem + 0x08;

        // FO4 GAMES weapons assemble their NIF geometry from component parts listed
        // in a BGSObjectInstanceExtra record (kExtraData_ObjectInstance=0x35) inside
        // the Stack's ExtraDataList. With no such record, EquipObject's AttachWeapon
        // call finds no parts to assemble and the weapon model is never spawned.
        // Armor geometry comes from the ARMO form's base mesh so it doesn't need this.
        //
        // We clone BGSObjectInstanceExtra::Data from the local player's matching
        // weapon so GAMES gets real OMOD component entries and assembles the same
        // geometry. The forms array is DEEP-copied (see below): it used to be
        // shared, and the engine freeing it destroyed the player's own weapon.
        // Vtable stolen from the first live
        // BGSObjectInstanceExtra in player inventory — all instances share one vtable.
        //
        // BGSObjectInstanceExtra layout (BSExtraData base 0x18 + own fields):
        //   BSExtraData:
        //     +0x00  vtable*        (8)
        //     +0x08  next           (BSExtraData*)  = nullptr
        //     +0x10  unk10          (UInt16)        = 0
        //     +0x12  type           (UInt8)         = 0x35 (kExtraData_ObjectInstance)
        //     +0x13  unk13          (UInt8)         = 0
        //     +0x14  unk14          (UInt32)        = 0
        //   BGSObjectInstanceExtra own:
        //     +0x18  data           (Data*)         → empty Data struct
        //     +0x20  unk20          (SInt16)        = 0
        //     +0x22  unk22          (UInt16)        = 0
        //     +0x24  unk24          (UInt32)        = 0
        if (form->formType == kFormType_WEAP)
        {
            const uintptr_t ievtbl = FindOrCacheInstanceExtraVtable();
            if (ievtbl)
            {
                // BGSObjectInstanceExtra::Data: Form* forms + UInt32 blockSize (+ pad)
                // blockSize / sizeof(Form{UInt32,UInt32}) = number of mods
                // 0 mods → engine uses weapon's default base configuration
                static constexpr size_t kIEXBytes  = 0x28; // sizeof(BGSObjectInstanceExtra)
                static constexpr size_t kDataBytes = 0x10; // sizeof(Data) aligned to 16

                uint8_t* iexMem  = (uint8_t*)Heap_Allocate(kIEXBytes);
                uint8_t* dataMem = (uint8_t*)Heap_Allocate(kDataBytes);
                if (iexMem && dataMem)
                {
                    memset(iexMem,  0, kIEXBytes);
                    memset(dataMem, 0, kDataBytes);

                    // Shallow-clone Data from local player's matching weapon.
                    // Copies forms pointer (8 bytes) + blockSize (4 bytes) so
                    // GAMES can assemble the weapon NIF from real OMOD entries.
                    {
                        PlayerCharacter* lp = CoSyncGetPlayer();
                        BGSInventoryList* linv = lp ? lp->inventoryList : nullptr;
                        bool cloned = false;
                        if (linv)
                        {
                            for (UInt32 ii = 0; ii < linv->items.count && !cloned; ++ii)
                            {
                                if (!linv->items.entries[ii].form ||
                                    linv->items.entries[ii].form->formID != form->formID)
                                    continue;
                                BGSInventoryItem::Stack* stk = linv->items.entries[ii].stack;
                                while (stk && !cloned)
                                {
                                    ExtraDataList* edl = stk->extraData;
                                    if (edl)
                                    {
                                        BSExtraData* node = edl->m_data;
                                        while (node)
                                        {
                                            if (node->type == kExtraData_ObjectInstance)
                                            {
                                                void* srcData = *(void**)((uint8_t*)node + 0x18);
                                                if (srcData)
                                                {
                                                    memcpy(dataMem, srcData, kDataBytes);
                                                    // DEEP-copy the mod array. A shallow copy made the
                                                    // proxy's instance share the LOCAL PLAYER's own
                                                    // forms block: when the proxy (or its weapon) was
                                                    // destroyed the engine freed it, leaving the real
                                                    // player's weapon pointing at freed memory -- the
                                                    // weapon broke/vanished and later saves kept it that
                                                    // way ("host and client weapons missing, even solo",
                                                    // 2026-09-26).
                                                    void*  srcForms  = *(void**)dataMem;
                                                    UInt32 formBytes = *(UInt32*)(dataMem + 0x08);
                                                    if (srcForms && formBytes > 0 && formBytes <= 0x4000)
                                                    {
                                                        void* ownForms = Heap_Allocate(formBytes);
                                                        if (ownForms)
                                                            memcpy(ownForms, srcForms, formBytes);
                                                        else
                                                            formBytes = 0;
                                                        *(void**)dataMem = ownForms;
                                                        *(UInt32*)(dataMem + 0x08) = formBytes;
                                                    }
                                                    else
                                                    {
                                                        *(void**)dataMem = nullptr;   // never share the player's block
                                                        *(UInt32*)(dataMem + 0x08) = 0;
                                                    }
                                                    LOG_UPDATED("[DirectInject] copied weapon mod data for weapon 0x%08X, blockSize=%u",
                                                        form->formID, *(UInt32*)(dataMem + 0x08));
                                                    cloned = true;
                                                }
                                                break;
                                            }
                                            node = node->next;
                                        }
                                    }
                                    stk = stk->next;
                                }
                            }
                        }
                        if (!cloned)
                            LOG_UPDATED_WARN("[DirectInject] player has no mod data for weapon 0x%08X, using empty data instead", form->formID);
                    }

                    *(uintptr_t*)(iexMem + 0x00) = ievtbl;                   // vtable
                    *(uint8_t*) (iexMem + 0x12) = kExtraData_ObjectInstance; // type = 0x35
                    *(void**)   (iexMem + 0x18) = dataMem;                   // data → cloned

                    // Link as the head of ExtraDataList.m_data (offset +0x08),
                    // exactly as ExtraDataList::Add does: tail -> this node's
                    // next (+0x08); the presence bit is set just below.
                    *(BSExtraData**)(edlMem + 0x08) = reinterpret_cast<BSExtraData*>(iexMem);
                    *(void**)(edlMem + 0x10) = iexMem + 0x08;

                    // Mark type 0x35 present in PresenceBitfield
                    // bit index = 0x35 = 53: byte 6 (53/8), bit 5 (53%8)
                    pbfMem[kExtraData_ObjectInstance / 8] |=
                        static_cast<uint8_t>(1u << (kExtraData_ObjectInstance % 8));

                    LOG_UPDATED("[DirectInject] added weapon mod data (vtable=0x%llX) for weapon 0x%08X",
                        (unsigned long long)ievtbl, form->formID);
                }
                else
                {
                    if (iexMem)  Heap_Free(iexMem);
                    if (dataMem) Heap_Free(dataMem);
                    LOG_UPDATED_WARN("[DirectInject] memory allocation failed for weapon mod data 0x%08X", form->formID);
                }
            }
            else
            {
                LOG_UPDATED_WARN("[DirectInject] couldn't find mod-data setup in the player's inventory; weapon 0x%08X might not render properly", form->formID);
            }
        }

        *(ExtraDataList**)(mem + 0x18) = reinterpret_cast<ExtraDataList*>(edlMem);

        // Grow items array if at capacity, then append the new entry.
        if (inv->items.count >= inv->items.capacity)
            inv->items.Grow(1);

        BGSInventoryItem& slot = inv->items.entries[inv->items.count];
        slot.form  = form;
        slot.stack = reinterpret_cast<BGSInventoryItem::Stack*>(mem);
        inv->items.count++;
        inv->inventoryWeight = -1.0f;  // force weight recalculation
        return true;
    }

    void TeleportActorTo(Actor* actor, TESObjectREFR* target)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::TeleportActorTo", "Needs a non-null actor and target (checked), and both must still be valid game forms (checked) before queuing the move.");
        if (!actor || !target)
            return;
        if (!IsFormStillLive(static_cast<TESForm*>(actor)) || !IsFormStillLive(static_cast<TESForm*>(target)))
        {
            LOG_WARN("[CoSyncGameAPI::TeleportActorTo] actor=%p or target=%p is gone, skipping",
                static_cast<void*>(actor), static_cast<void*>(target));
            return;
        }

        VMArray<VMVariable> args;

        VMVariable vTarget;
        CoSyncVM::SetFormArg(vTarget, static_cast<TESForm*>(target));
        args.Push(&vTarget);

        float zero = 0.0f;
        VMVariable vX; vX.Set(&zero); args.Push(&vX);
        VMVariable vY; vY.Set(&zero); args.Push(&vY);
        VMVariable vZ; vZ.Set(&zero); args.Push(&vZ);

        bool matchRot = false;
        VMVariable vRot; vRot.Set(&matchRot); args.Push(&vRot);

        static const BSFixedString kMoveTo("MoveTo");
        CallFunctionNoWait(actor, kMoveTo, args);

        LOG_INFO("[CoSyncGameAPI] queued teleport of actor=%p to target=%p",
            static_cast<void*>(actor), static_cast<void*>(target));
    }

    // Two __try blocks are fine here because the only locals are POD types
    // (uint32_t, TESForm*) — no C++ destructors that would trigger C2712.
    bool IsFormStillLive(TESForm* form)
    {
        if (!form)
            return false;
        uint32_t fid = 0;
        __try { fid = form->formID; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (!fid)
            return false;
        TESForm* looked = nullptr;
        __try { looked = LookupFormByID(fid); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return looked == form;
    }

    void SetLocalPlayerEssential(bool essential)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::SetLocalPlayerEssential", "Needs a valid player whose base form is an NPC record (both checked). Toggles the essential flag on that record directly. This is purely a crash-prevention measure — it keeps the player from hitting the death menu and reload path.");
        Actor* player = CoSyncGetPlayer();
        if (!player || !player->baseForm)
            return;
        if (player->baseForm->formType != kFormType_NPC_)
            return;

        TESNPC* npcBase = static_cast<TESNPC*>(player->baseForm);
        if (essential)
            npcBase->actorData.flags |= TESActorBaseData::kFlagEssential;
        else
            npcBase->actorData.flags &= ~static_cast<uint32_t>(TESActorBaseData::kFlagEssential);

        LOG_INFO("[CoSyncGameAPI] set local player essential=%d, base=0x%08X, flags=0x%llX",
            essential ? 1 : 0, npcBase->formID, (unsigned long long)npcBase->actorData.flags);
    }

    // ── On-screen notification ──────────────────────────────────────────────

    // See CoSyncGameAPI.h for the full derivation of this RVA from the
    // Debug.Notification registration. Signature confirmed from the call the
    // Papyrus native makes: (text, nullptr, true, true).
    typedef void (*_ShowHUDMessage)(const char* text, void* unk, bool a3, bool a4);
    static RelocAddr<_ShowHUDMessage> g_ShowHUDMessage(0x00A37270);

    __declspec(noinline) static void ShowNotification_Impl(const char* text)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::ShowNotification_Impl", "Must run on the game thread. Needs a non-null, non-empty string (checked by the caller) and the HUD to be ready, which it is any time a save is loaded. The caller wraps this in a crash guard.");
        g_ShowHUDMessage(text, nullptr, true, true);
    }

    void ShowNotification(const char* text)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::ShowNotification", "Must run on the game thread. Wraps the actual engine call in a crash guard so a bad UI state can never bring down the game. Silently does nothing if given null or empty text.");
        if (!text || !*text)
            return;

        __try
        {
            ShowNotification_Impl(text);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_WARN("[CoSyncGameAPI] showing a notification crashed but was recovered from — text='%s'", text);
        }
    }

    namespace
    {
        std::mutex               s_notifyMutex;
        std::vector<std::string> s_pendingNotifications;
        constexpr size_t         kMaxQueuedNotifications = 16;
    }

    void QueueNotification(const char* text)
    {
        LOG_REQUIREMENT("CoSyncGameAPI::QueueNotification", "Safe to call from any thread — just locks a mutex and copies the string. Doesn't touch any game state until FlushNotifications runs on the game thread.");
        if (!text || !*text)
            return;

        std::lock_guard<std::mutex> lk(s_notifyMutex);
        // A disconnect storm must not queue unbounded work for the UI.
        if (s_pendingNotifications.size() >= kMaxQueuedNotifications)
            return;
        s_pendingNotifications.emplace_back(text);
    }

    void FlushNotifications()
    {
        LOG_REQUIREMENT("CoSyncGameAPI::FlushNotifications", "Must run on the game thread. Empties the queue under the lock first, so the actual engine calls happen after the lock is released.");
        std::vector<std::string> pending;
        {
            std::lock_guard<std::mutex> lk(s_notifyMutex);
            if (s_pendingNotifications.empty())
                return;
            pending.swap(s_pendingNotifications);
        }

        for (const std::string& s : pending)
            ShowNotification(s.c_str());
    }

}
