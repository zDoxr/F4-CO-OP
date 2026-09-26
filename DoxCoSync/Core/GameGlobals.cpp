#include <Windows.h>
#include "ConsoleLogger.h"
#include "GameReferences.h"
#include "GameExtraData.h"
#include "GameRTTI.h"
#include "GameMenus.h"
#include "GameForms.h"
#include "TickHook.h"
#include "LocalPlayerState.h"
#include "CoSyncRequirementsLog.h"

RelocPtr<PlayerCharacter*> g_player(0x032DD370);

extern RelocAddr<_LookupFormByID> LookupFormByID;

// UPDATE 2026-06-17: re-verified via IDA on 1.11.221 -- 0x032D2260 is exactly
// the global the real Game.GetPlayer() native reads (traced through its
// NativeFunction0 registration, sub_141123910 -> sub_141122670 -> reads
// qword_1432D2260, RVA 0x032D2260). The OLD, actually-stale address was
// 0x032D22E0 (still sitting dead in the unused vendor snapshot
// ThirdParty/Pap/GameReferences.cpp, excluded from the build) -- that's what
// the original "stays NULL" observation below was almost certainly hitting,
// before someone updated this file's value to the correct one without
// removing the comment or this workaround. Keeping CoSyncGetPlayer() anyway:
// it's now the established pattern everywhere else in the codebase (every
// *g_player direct-dereference call site outside this file was migrated to
// it 2026-06-17 too).
//
// IMPORTANT correction to the original note below: LookupFormByID's RVA was
// NOT actually correct -- 0x00311860 landed mid-instruction (inside an LEA's
// displacement bytes; confirmed at the raw byte level), one of the same
// stale-post-NG-update RVAs this whole investigation has been finding
// elsewhere. Fixed to the verified function start, 0x00311850 (see
// ThirdParty/Pap/GameForms.cpp). The "every session" claim below is most
// likely stale information from testing against an older game build, same
// as the g_player claim above.
//
// Original note, kept for history:
// g_player's RVA is stale post-Next-Gen-update: confirmed by 9000+ frames of
// *g_player staying NULL across an entire live session on both host and
// client, despite both clearly playing (menus responding, PauseMenu opening,
// etc). FormID 0x14 is the hardcoded engine constant for the player reference
// in every Creation Engine game, and LookupFormByID's RVA is independently
// confirmed correct (it resolves CoSyncQuest's known formID every session) --
// use that instead of trusting the singleton RVA.
// UPDATE 2026-06-18: confirmed live -- this lookup+cast crashed inside
// VCRUNTIME140's __RTDynamicCast (CoSyncLocalPlayer::Tick -> GetLocalPlayerActor
// -> CoSyncGetPlayer, called every frame from the render-thread Present hook).
// Register dump showed the cast reading a garbage vtable pointer off the
// looked-up form, AV READ on [vtable-8]. Fault landed in the same instant as
// the known EventSinkTeardown engine-reset event (Fallout4.exe+0x1A7FC54),
// so this is likely a downstream symptom of whatever triggers that reset --
// not yet proven, but either way this was the one unguarded raw engine-memory
// touch in this function, matching every other Safe* helper's pattern in this
// codebase. noinline is load-bearing under this project's /GL+/LTCG config --
// see CoSyncTimeSync.cpp's SafeCallGetSkyManagerSingleton for the same note.
// RVA CORRECTED round 107 (build-and-test crash, first launch after round 106):
// this exact call -- DYNAMIC_CAST(form, TESForm, PlayerCharacter), fired every
// single rendered frame from the DX11 Present hook via TickHook_RetryInstallIfNeeded,
// starting the instant the plugin loads, well before any save is loaded --
// crashed fatally on the very first test of the round-106 GameRTTI.cpp fix.
// Minidump (cdb, real PDB symbols, far more reliable than the VEH's lightweight
// walker) showed the fault 3 frames deep inside ucrtbase's vsscanf-family
// internals (fault addr=0x3), NOT the "vtable-8" AV the 2026-06-18 note
// describes -- a different signature, confirming the round-106 RVA fix changed
// what actually executes here. The existing __try/__except's own recovery log
// line never appeared in the crash log, confirming (per that same 2026-06-18
// note's own designed diagnostic test) that this guard does not actually catch
// this fault -- it's genuinely fatal, not VEH noise.
// Real fix instead of chasing the RTTI plumbing further under time pressure:
// formID 0x14 is the Creation Engine's hardcoded constant for the player
// reference in every game built on this engine (see the original 2026-06-18
// note above) -- the type is already 100% determined by the formID itself,
// so there is no need to runtime-verify it via RTTI at all. A plain
// static_cast is exactly as correct here and skips the broken DYNAMIC_CAST
// path entirely for this single hottest-of-all-call-sites (every frame,
// forever, while waiting for a player to spawn). LookupFormByID itself is
// independently proven safe (IDA-decompiled: a mask-bounded hash lookup that
// cannot fault on a bad/garbage formID), so the only remaining risk this
// __try still guards against is a stale/garbage *pointer value* returned by
// LookupFormByID for some other reason -- kept as a backstop.
__declspec(noinline) static PlayerCharacter* SafeLookupAndCastPlayer()
{
    LOG_REQUIREMENT("(global)::SafeLookupAndCastPlayer", "Looks up the player using the engine's fixed player ID (0x14). The safety wrapper here is just a backup in case of a bad pointer, not a sign of a known crash.");
    __try
    {
        TESForm* form = LookupFormByID(0x00000014);
        return form ? static_cast<PlayerCharacter*>(form) : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_WARN("[GameGlobals] SafeLookupAndCastPlayer caught a crash and recovered "
                 "(returning nothing) -- the player lookup failure was not fatal");
        return nullptr;
    }
}

PlayerCharacter* CoSyncGetPlayer()
{
    LOG_REQUIREMENT("(global)::CoSyncGetPlayer", "Just calls SafeLookupAndCastPlayer(). Safe to call every frame, even before a save is loaded — returns nothing until the player exists.");
    return SafeLookupAndCastPlayer();
}

// 2026-09-26 (IDA, 1.11.240): the UI singleton is 0x030E8930 -- LooksMenu
// registers on it (0x140A863F0) and Game.ForceThirdPerson calls
// IsMenuOpen(qword_1430E8930, "PipboyMenu"); f4se 0.7.9 agrees. The old
// 0x030E88B0 is BSInputEnableManager, so every IsMenuOpen/menuTable lookup
// through it failed ("menuTable size=0").
RelocPtr<UI*>              g_ui(0x030E8930);


// UPDATE 2026-06-20 (round 44): both fixed via PE .pdata/RUNTIME_FUNCTION
// lookup against the live 1.11.221 binary (authoritative function
// boundaries, independent of IDA -- which this session has repeatedly
// confirmed unreliable). HasDetectionLOS is currently dead code (never
// actually called anywhere in CoSync), fixed for correctness anyway.
// MoveRefrToPosition IS actively used (CoSyncPowerArmorSync.cpp,
// CoSyncWorldSync.cpp, CoSyncGameAPI.cpp) -- was +0xA0 (160 bytes) into the
// real function, skipping its prologue (`mov [rsp+8],rbx; mov [rsp+0x10],rsi;
// push rbp; push rdi; push r12; push r14; push r15; lea rbp,[rsp-0x370]`),
// meaning every call ran with RBX/RSI/RBP/RDI/R12/R14/R15 left at whatever
// the CALLER had instead of this function's own setup -- the same class of
// corrupted-register-propagation bug as round 44's PlaceAtMe_Native and
// CallFunctionNoWait_Internal fixes.
RelocAddr<_HasDetectionLOS>        HasDetectionLOS(0x010DE2C0);
RelocAddr<_GetLinkedRef_Native>    GetLinkedRef_Native(0x005648A0);
RelocAddr<_SetLinkedRef_Native>    SetLinkedRef_Native(0x005648C0);
// 2026-08-02 IDA-VERIFIED against the 1.11.221 dump (FalloutDumperOutput): the
// old 0x01180A90 was WRONG — it is BGSInventoryItem::ModifyModDataFunctor::ctor,
// a completely unrelated inventory function, so every cross-cell MoveRefrToPosition
// call silently did nothing (this is why proxies never followed through doors).
// The REAL MoveTo/MoveRefrToPosition is 0x01180CD0 (the value the f4se SDK already
// shipped) — confirmed the Papyrus MoveTo native ("<X> cannot be moved to a None
// object", fn @0x114F58A) calls it, and its disasm reads pos x/y/z from the 5th
// arg with signature (source, handle, cell, worldspace, pos, rot). Same function
// FalloutTogether/TiltedEvolution uses (their VersionDb ID 1332435).
RelocAddr<_MoveRefrToPosition>     MoveRefrToPosition(0x01181020);

LocalPlayerState g_localPlayerState;
