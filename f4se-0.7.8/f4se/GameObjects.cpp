#include "f4se/GameObjects.h"
#include "f4se/GameReferences.h"
#include "f4se/GameUtilities.h"

// UPDATE 2026-06-20 (round 52): EVERY prior fix to this constant (0x1159B30
// -> 0x1159990 -> round44's 0x1159910) was chasing the WRONG FUNCTION
// entirely -- not a wrong offset into the right one, a different function
// that happens to sit immediately next to the real PlaceAtMe in memory.
// Proof: the user pointed at a 2026-06-07 backup whose extracted
// Fallout4.exe is version 1.11.191 (pre-Next-Gen) and whose CoSyncGameAPI.cpp
// uses this exact same 8-arg shape CoSync has had all along, at RVA
// 0x1159990 -- the user's recollection that this version "worked" sent the
// search in the right direction. Cross-checking FO4_Wrld's OWN documented
// PlaceAtMe RVA (0x1159C10, decomp-sourced, "size 0x393") against that same
// 1.11.191 binary's .pdata confirms it as a REAL, different function
// (begin=0x1159C10, end=0x1159FA3, size=0x393 -- exact match) sitting right
// after a 4-entry function chain that starts at 0x1159990 (sizes 0xAC, 0x1A0,
// 0x23) -- i.e. CoSync's RVA was always pointing at the START of an unrelated
// neighboring function, not PlaceAtMe itself, going back to whatever
// original RE pass first identified it (a stale "native registration table"
// claim per the old comment below, never decompile-verified).
// Confirmed the same chain exists in 1.11.221, shifted by a constant +0x1A0
// (a new, unrelated 0x21A-byte function -- the one every fix this session
// was actually patching -- was inserted immediately before it by Next-Gen):
//   1.11.191                  1.11.221                 size
//   0x1159990-0x1159A3C  ->   0x1159B30-0x1159BDC   (0xAC)
//   0x1159A3C-0x1159BDC  ->   0x1159BDC-0x1159D7C   (0x1A0)
//   0x1159BDC-0x1159BFF  ->   0x1159D7C-0x1159D9F   (0x23)
//   0x1159C10-0x1159FA3  ->   0x1159DB0-0x115A143   (0x393)  <- real PlaceAtMe
// Disassembled 0x1159DB0 directly: `mov rbx,rcx(arg1); mov esi,edx(arg2);
// mov rdi,r8(arg3); mov r14,r9(arg4); test r9,r9; je <error>` -- arg4 is
// null-checked immediately, exactly matching FO4_Wrld's documented "PlaceAtMe
// validates anchor_refr != NULL" behavior. This is the real function;
// FO4_Wrld's clean 6-arg signature (vm, stackId, formPair, anchorRefr, count,
// persistent) applies directly with no extra trailing arguments needed --
// every "extra garbage argument" found in rounds 49-51 was a symptom of
// disassembling the wrong, Next-Gen-only neighboring function, not a real
// property of PlaceAtMe itself.
//
// Old history (now fully superseded, kept for context): round 44 confirmed
// 0x1159910 was a real function ENTRY per .pdata and called it correct on
// that basis alone -- entry validity was never sufficient proof of identity.
// Before that, "Fixed via IDA against Fallout4.exe 1.11.221: 0x01159B30 was
// mid-function ... Confirmed correct address found via the ObjectReference
// native registration table: sub_141159990 is registered directly for
// "PlaceAtMe"/"ObjectReference"" -- this registration-table claim is what
// was actually wrong from the start.
RelocAddr <_PlaceAtMe_Native> PlaceAtMe_Native(0x01159DB0);

bool TESObjectARMA::GetNodeName(char * dstBuff, TESNPC * npc, TESObjectARMO * armor)
{
	if(npc) {
		UInt32 sex = npc ? CALL_MEMBER_FN(npc, GetSex)() : 0;
		sprintf_s(dstBuff, MAX_PATH, " (%08X)[%d]/ (%08X)", formID, sex, armor->formID);
		return true;
	}

	return false;
}

void TESNPC::ChangeHeadPart(BGSHeadPart * headPart, bool bRemovePart, bool bRemoveExtraParts)
{
	if(bRemovePart)
		CALL_MEMBER_FN(this, ChangeHeadPartRemovePart)(headPart, bRemoveExtraParts);
	else
		CALL_MEMBER_FN(this, ChangeHeadPart)(headPart);
}

BGSHeadPart * TESNPC::GetHeadPartByType(UInt32 type, bool bOverlays)
{
	BGSHeadPart ** parts = headParts;
	UInt32 numParts = numHeadParts;
	if(bOverlays) {
		parts = CALL_MEMBER_FN(this, GetOverlayHeadParts)();
		numParts = CALL_MEMBER_FN(this, GetNumOverlayHeadParts)();
	}

	for(UInt32 i = 0; i < numParts; ++i)
	{
		BGSHeadPart * part = parts[i];
		if(part && part->type == type)
		{
			return part;
		}
	}

	return nullptr;
}

BGSColorForm * TESNPC::GetHairColor()
{
	if(!headData || headData->hairColor == nullptr)
	{
		TESRace * raceForm = race.race;
		if(raceForm) {
			CharacterCreation::CharGenData * charGenData = raceForm->chargenData[ CALL_MEMBER_FN(this, GetSex)()];
			if(charGenData) {
				return charGenData->defaultColor;
			}
		}
	}

	return headData->hairColor;
}
