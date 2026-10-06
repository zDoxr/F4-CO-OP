#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncPapyrusHelper.h"

#include "ConsoleLogger.h"
#include "GameForms.h"
#include "GameData.h"
#include "GameRTTI.h"
#include "GameReferences.h"
#include "PapyrusVM.h"
#include "PapyrusEvents.h"
#include "PapyrusArgs.h"
#include "CoSyncEntityRegistry.h"
#include "CoSyncVMFormArg.h"
#include "CoSyncGameAPI.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"

extern RelocPtr<DataHandler*> g_dataHandler;
extern RelocPtr<GameVM*> g_gameVM;
extern RelocAddr<_LookupFormByID> LookupFormByID;
extern CoSyncPlayerManager g_CoSyncPlayerManager;

namespace
{
    TESQuest* s_coSyncQuest = nullptr;
    bool s_initialized = false;

    // Round 109: this hardcoded top byte (0x01) is the exact same
    // load-order-dependent bug already found and fixed in this file's own
    // GetRemotePlayerBase() (see its comment for the full mechanism) --
    // CoSync.esp's real mod-index slot depends on the user's actual load
    // order, not whatever slot it happened to occupy in Creation Kit when
    // this constant was baked in. For a load order where CoSync.esp isn't
    // really at slot 0x01, LookupFormByID(kCoSyncQuestFormID) resolves to
    // some OTHER mod's form-table entry at local ID 0x00359A instead of our
    // own Quest record -- confirmed live: the resulting form's formType was
    // 80 (not TESQuest's real type), and DYNAMIC_CAST-ing that wrong object
    // crashed fatally with zero forensic trace (not a normal AV -- see
    // round 109 notes). Resolved the same way GetRemotePlayerBase() already
    // does: via DataHandler::GetLoadedModIndex() instead of trusting the
    // baked top byte.
    constexpr UInt32 kCoSyncQuestLocalFormID = 0x00359A;
}

static UInt32 ResolveCoSyncQuestFormID()
{
    DataHandler* dataHandler = *g_dataHandler;
    if (!dataHandler)
    {
        LOG_ERROR("[PapyrusHelper] ResolveCoSyncQuestFormID: DataHandler not available!");
        return 0;
    }

    UInt8 modIndex = dataHandler->GetLoadedModIndex("CoSync.esp");
    if (modIndex == 0xFF)
    {
        LOG_ERROR("[PapyrusHelper] ResolveCoSyncQuestFormID: CoSync.esp not found in loaded mods!");
        return 0;
    }

    const UInt32 formID = (static_cast<UInt32>(modIndex) << 24) | kCoSyncQuestLocalFormID;
    LOG_INFO("[PapyrusHelper] ResolveCoSyncQuestFormID: CoSync.esp resolved to mod index 0x%02X -> formID 0x%08X",
        modIndex, formID);
    return formID;
}

// Round 109 (final): round 108's RTTI_TESForm/RTTI_TESQuest constant fix did
// NOT actually resolve this -- live breadcrumb logging proved the looked-up
// form's formType is 80 (== kFormType_QUST, confirmed by counting F4SE's own
// FormType enum in GameForms.h: NONE=0 ... QUST=80), i.e. LookupFormByID is
// finding our genuine, correctly-typed Quest record every time, and
// DYNAMIC_CAST still crashes fatally processing a provably-correct object.
// That means __RTDynamicCast itself is still broken for this codebase in some
// way the round-108 TypeDescriptor-header check didn't catch (possibly
// vfDelta, possibly something else in how Runtime_DynamicCast_Internal is
// invoked) -- not yet root-caused. Same fix as round 107's player-lookup
// crash, applied for the same reason: kCoSyncQuestFormID is OUR OWN
// CreationKit-authored record, always guaranteed to be a TESQuest by
// construction, so RTTI verification is redundant here regardless of whether
// the underlying DYNAMIC_CAST machinery is trustworthy. static_cast sidesteps
// it entirely. NOTE: this does not fix DYNAMIC_CAST itself -- every other
// call site in the codebase (CoSyncWorldSync, CoSyncPlayerManager,
// CoSyncMapSync, CoSyncCompanionSync, etc., still gated behind world-ready/
// connected-multiplayer checks that haven't fired in any test yet) may hit
// the exact same wall once actually exercised. If/when that happens, the fix
// is the same: replace with static_cast wherever the real type is already
// guaranteed by the calling context, otherwise this needs real IDA-level
// tracing of __RTDynamicCast's actual implementation against the exact
// arguments Runtime_DynamicCast_Internal passes it.
__declspec(noinline) static TESQuest* SafeLookupAndCastQuest(uint32_t formID)
{
    __try
    {
        TESForm* form = LookupFormByID(formID);
        return form ? static_cast<TESQuest*>(form) : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[PapyrusHelper] SafeLookupAndCastQuest caught AV and recovered (returning nullptr)");
        return nullptr;
    }
}

void CoSyncPapyrusHelper::Init()
{
    LOG_INFO("[PapyrusHelper] Init() entered, s_initialized=%d", (int)s_initialized);
    if (s_initialized)
        return;

    s_initialized = true;

    const UInt32 questFormID = ResolveCoSyncQuestFormID();
    if (questFormID == 0)
    {
        LOG_ERROR("[PapyrusHelper] Could not resolve CoSyncQuest formID -- CoSync.esp not loaded?");
        return;
    }

    LOG_INFO("[PapyrusHelper] Init() calling SafeLookupAndCastQuest");
    s_coSyncQuest = SafeLookupAndCastQuest(questFormID);
    LOG_INFO("[PapyrusHelper] Init() SafeLookupAndCastQuest returned, s_coSyncQuest=%p", static_cast<void*>(s_coSyncQuest));
    if (!s_coSyncQuest)
    {
        LOG_ERROR("[PapyrusHelper] Quest form not found or not a quest: 0x%08X", questFormID);
        LOG_ERROR("[PapyrusHelper] Make sure CoSync.esp is loaded!");
        return;
    }

    LOG_INFO("[PapyrusHelper] Found CoSyncQuest: 0x%08X name='%s'",
        questFormID,
        s_coSyncQuest->fullName.name.c_str());
}

TESQuest* CoSyncPapyrusHelper::GetCoSyncQuest()
{
    if (!s_initialized)
        Init();

    return s_coSyncQuest;
}

void CoSyncPapyrusHelper::SpawnRemotePlayer(uint32_t entityID)
{
    if (!s_coSyncQuest)
    {
        LOG_WARN("[PapyrusHelper] Cannot spawn - quest not found");
        return;
    }

    GameVM* gameVM = *g_gameVM;
    if (!gameVM || !gameVM->m_virtualMachine)
    {
        LOG_ERROR("[PapyrusHelper] VM not available");
        return;
    }


    VMArray<VMVariable> args;
    VMVariable entityIDVar;
    SInt32 entityIDValue = static_cast<SInt32>(entityID);
    entityIDVar.Set(&entityIDValue);
    args.Push(&entityIDVar);


    static const BSFixedString kSpawnRemotePlayer("SpawnRemotePlayer");
    CallFunctionNoWait(
        s_coSyncQuest,
        kSpawnRemotePlayer,
        args
    );

    LOG_INFO("[PapyrusHelper] Called SpawnRemotePlayer(%u)", entityID);
}

void CoSyncPapyrusHelper::DespawnRemotePlayer(uint32_t entityID)
{
    if (!s_coSyncQuest)
    {
        LOG_WARN("[PapyrusHelper] Cannot despawn - quest not found");
        return;
    }

    GameVM* gameVM = *g_gameVM;
    if (!gameVM || !gameVM->m_virtualMachine)
    {
        LOG_ERROR("[PapyrusHelper] VM not available");
        return;
    }

    VMArray<VMVariable> args;
    VMVariable entityIDVar;
    SInt32 entityIDValue = static_cast<SInt32>(entityID);
    entityIDVar.Set(&entityIDValue);
    args.Push(&entityIDVar);

    static const BSFixedString kDespawnRemotePlayer("DespawnRemotePlayer");
    CallFunctionNoWait(
        s_coSyncQuest,
        kDespawnRemotePlayer,
        args
    );

    LOG_INFO("[PapyrusHelper] Called DespawnRemotePlayer(%u)", entityID);
}

Actor* CoSyncPapyrusHelper::GetRemotePlayerActor(uint32_t entityID)
{
    if (!s_coSyncQuest)
    {
        LOG_WARN("[PapyrusHelper] Cannot get actor - quest not found");
        return nullptr;
    }

    GameVM* gameVM = *g_gameVM;
    if (!gameVM || !gameVM->m_virtualMachine)
    {
        LOG_ERROR("[PapyrusHelper] VM not available");
        return nullptr;
    }

    LOG_WARN("[PapyrusHelper] GetRemotePlayerActor not fully implemented yet");
    return nullptr;
}

void CoSyncPapyrusHelper::SendProxyAnimEvent(Actor* actorRef, const char* eventName)
{
    if (!s_coSyncQuest || !actorRef || !eventName) return;

    VMArray<VMVariable> args;

    VMVariable vID;
    SInt32 fid = static_cast<SInt32>(actorRef->formID);
    vID.Set(&fid);
    args.Push(&vID);

    VMVariable vEv;
    BSFixedString ev(eventName);
    vEv.Set(&ev);
    args.Push(&vEv);

    static const BSFixedString kSendProxyAnimEvent("SendProxyAnimEvent");
    CallFunctionNoWait(s_coSyncQuest, kSendProxyAnimEvent, args);
}

TESNPC* CoSyncPapyrusHelper::GetRemotePlayerBase()
{

    // UPDATE 2026-06-20 (round 66): rounds 62-65 all patched
    // CoSync_DebugProxyNPC's ACBS/TPLT data repeatedly with zero effect on
    // the STATUS_HEAP_CORRUPTION crash, including round 65's full
    // re-templating off a complete, proven-stable vanilla NPC -- which
    // should have masked ANY data-completeness problem and didn't change
    // the crash at all. That means the crash was never caused by this
    // record's data in the first place.
    //
    // LookupFormByID takes an ALREADY-RUNTIME-RESOLVED FormID -- it does NOT
    // do master-table translation. The hardcoded constant below has top byte
    // 0x01, baked in from whatever load-order slot CoSync.esp happened to
    // occupy in Creation Kit when this NPC was authored. The user's actual
    // load order (checked via Plugins.txt: only "*CoSync.esp" is listed,
    // meaning Fallout4.esm + all 6 DLC ESMs occupy slots 00-06 implicitly
    // and CoSync.esp is really at slot 0x07) means 0x01001ECC has been
    // resolving to slot 01's form table this entire time -- i.e. some
    // DLCRobot.esm form, NOT CoSync_DebugProxyNPC -- which is exactly why
    // editing our own ESP record never changed anything: we were never
    // spawning the form we thought we were. Resolve the real current slot
    // via DataHandler::GetLoadedModIndex() instead of trusting CK's
    // load-order-dependent top byte.
    DataHandler* dataHandler = *g_dataHandler;
    if (!dataHandler)
    {
        LOG_ERROR("[PapyrusHelper] GetRemotePlayerBase: DataHandler not available!");
        return nullptr;
    }

    UInt8 modIndex = dataHandler->GetLoadedModIndex("CoSync.esp");
    if (modIndex == 0xFF)
    {
        LOG_ERROR("[PapyrusHelper] GetRemotePlayerBase: CoSync.esp not found in loaded mods!");
        return nullptr;
    }

    // UPDATE 2026-06-21 (round 88, mechanism CORRECTED round 89): this
    // pointed at "CoSync_DebugProxyNPC" (EDID confirmed via direct .esp
    // inspection -- a bare-bones placeholder record: no TPLT, no perks, no
    // keywords, no facegen/morph data, no default outfit). Round 88
    // originally claimed the crash family (Fallout4.exe+0x256B80/+0xCC934D)
    // was npcBase->actorValueOwner.GetValue() reading a "garbage vtable
    // pointer" at npcBase+0x130 caused by missing CK data. Round 89 DISPROVED
    // that specific mechanism: direct disassembly of the engine's own
    // TESActorBase init routine (Fallout4.exe+0xE4A3F3) shows
    // [npcBase+0x130] is not a vtable at all -- it's a plain self-referential
    // pointer (set via `lea rax,[this+0x140]; mov [this+0x130], rax`)
    // written UNCONDITIONALLY with no data-dependent branch. A vtable-style
    // call through it (`mov rax,[rcx+0x130]; call [rax+8]`) only crashes if
    // this init routine never ran for that specific form at all.
    //
    // That points to the REAL mechanism: a cold/lazy-initialization race,
    // not a data-completeness issue -- and it's the exact same class of bug
    // already proven on the sibling field npcBase+0xB8 (see round 70's note
    // below: that field is non-null/cached for the literal Player NPC,
    // which the engine touches constantly, but cold/null for a debug NPC
    // nothing else in the game ever references). CoSync's SpawnRemoteActor
    // forces this form into existence directly, ahead of whatever normal
    // game-data-load pass would otherwise warm it up. The complete record
    // (formID-local 0x003599, EDID "CoSyncRemotePlayerBase" -- TPLT/TPTA
    // template chain, PRKZ/PRKR perks, KSIZ/KWDA keywords, DOFT, HCLF,
    // MSDK/MSDV/FMRI/FMRS facegen) is far more likely to already be warm by
    // spawn time, since other game systems (templates/perks/keywords)
    // reference it during normal data load -- unlike the debug NPC, which
    // nothing else points at. Switching to this record is still the right
    // fix, just confirmed via a different and better-evidenced mechanism
    // than round 88 originally claimed. If this does NOT fully resolve the
    // crash, the next step is to force the lazy-init explicitly (same idea
    // as the +0xB8 pre-warm in CoSyncGameAPI.cpp) rather than relying on
    // record selection alone. NOTE: rounds 62-65 already tried "improve this
    // record's data" once before (full re-templating off a vanilla NPC, per
    // round 66's comment above) with "zero effect" -- but that test ran
    // against the OLD STATUS_HEAP_CORRUPTION bug (rounds 58-65, fixed in
    // round 72), at a point in the spawn process that never reached anywhere
    // near this code; it doesn't necessarily transfer to the current crash.
    // Round 134: back to CoSync.esp's own CoSyncRemotePlayerBase (0x003599).
    // The vanilla raider 0x20749 is a confirmed dud: it spawns as a permanent
    // A-pose even from the console with NO plugin loaded (user-verified;
    // Dogmeat 0x1D15C spawned the same way animates normally) — the form has
    // no usable animation set when spawned directly. The rounds-62-66 "zero
    // effect" verdict against the esp record predates the round-117 spawn fix
    // (no real actor ever existed then) and is void.
    // Round 137: switched from CoSyncRemotePlayerBase (0x003599) to RMPlayer
    // (0x00A797). Direct .esp inspection (2026-07-05) showed 0x003599 has
    // TPLT = 0x00000007 — it TEMPLATES OFF THE LITERAL PLAYER RECORD, which
    // is why the spawned proxy still presented as the default "Player"
    // character (user report: "still 00000007 npc"), and it has no FULL name.
    // RMPlayer is the CK-authored standalone proxy: no template chain,
    // FULL name "2nd Player", PlayerFaction membership (SNAM 0x1C21C) and a
    // default outfit (DOFT 0x1F0DF) baked in.
    const UInt32 kRemotePlayerBaseFormID = (static_cast<UInt32>(modIndex) << 24) | 0x0000A797u;
    LOG_INFO("[PapyrusHelper] GetRemotePlayerBase: using CoSync.esp RMPlayer formID 0x%08X (modIndex=0x%02X)",
        kRemotePlayerBaseFormID, modIndex);

    // UPDATE 2026-06-20 (round 70): round 70's diagnostic swap to the literal
    // Player NPC_ confirmed its npcBase+0xB8 is already cached (non-null),
    // which is why it never hits the buggy cold-resolve path that crashes
    // for our never-before-touched proxy NPC. Reverted back to the real
    // proxy now that SpawnRemoteActor pre-warms +0xB8 itself using the
    // player NPC's own cached pointer (see CoSyncGameAPI.cpp round 70 notes).
    TESForm* form = LookupFormByID(kRemotePlayerBaseFormID);
    if (!form)
    {
        LOG_ERROR("[PapyrusHelper] GetRemotePlayerBase: form 0x%08X not found", kRemotePlayerBaseFormID);
        return nullptr;
    }

    // DYNAMIC_CAST (__RTDynamicCast) is confirmed broken in this codebase
    // even for provably-correct types (rounds 106-109; confirmed crashing
    // live right here, 2026-06-24, caught only by this function's caller's
    // SEH wrapper). kRemotePlayerBaseFormID is CoSync.esp's own
    // CoSyncRemotePlayerBase record, authored as an NPC_ in Creation Kit --
    // formType is checked explicitly below instead, then static_cast.
    if (!form || form->formType != TESNPC::kTypeID)
    {
        LOG_ERROR("[PapyrusHelper] GetRemotePlayerBase: form 0x%08X is not TESNPC (type=%u)",
            kRemotePlayerBaseFormID, form ? form->formType : 0);
        return nullptr;
    }

    return static_cast<TESNPC*>(form);
}

UInt32 CoSyncPapyrusHelper::GetRemotePlayerBaseFormID()
{
    static UInt32 s_cached = 0;
    if (s_cached)
        return s_cached;

    DataHandler* dataHandler = *g_dataHandler;
    if (dataHandler)
    {
        const UInt8 modIndex = dataHandler->GetLoadedModIndex("CoSync.esp");
        if (modIndex != 0xFF)
        {
            // Round 137: RMPlayer (see GetRemotePlayerBase above for why).
            s_cached = (static_cast<UInt32>(modIndex) << 24) | 0x0000A797u;
            LOG_INFO("[PapyrusHelper] proxy base formID resolved: 0x%08X (CoSync.esp slot 0x%02X)",
                s_cached, modIndex);
            return s_cached;
        }
        LOG_ERROR("[PapyrusHelper] GetRemotePlayerBaseFormID: CoSync.esp not loaded — falling back to vanilla 0x20749 (KNOWN A-POSE DUD)");
    }
    // Do not cache the fallback: DataHandler may simply not be ready yet.
    return 0x00020749u;
}
