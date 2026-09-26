#include "f4se/GameForms.h"
#include "f4se/GameUtilities.h"

// 2026-07-14: disk-IDB re-verified (Fallout4_disk.i64, imagebase 0x140000000).
// 0x311850 is the 3rd byte of "add rsp,20h" (ModRM 0xC4) — calling it
// executes an invalid 3-byte VEX prefix → #UD fault. The 2026-06-17 comment
// claiming 0x311860 was "mid-instruction" was derived from the dump-based IDB
// which has per-region address skew; same issue as the equip hook RVAs.
// sub_140311860: is_head=True, 1395 code callers, takes uint32 formID,
// returns *(_QWORD*)(bucket+8) = TESForm* — the real hash-table lookup.
// sub_140311830 (the thin wrapper at 0x311830) returns formType byte at +0x1A,
// which is NOT what we want — we need the TESForm* itself.
RelocAddr <_LookupFormByID> LookupFormByID(0x00311860);

// 
RelocPtr <IFormFactory*> g_formFactoryList(0x030E0950);

// 
RelocPtr <tHashSet<ObjectModMiscPair, BGSMod::Attachment::Mod*>> g_modAttachmentMap(0x02ED5758-0x08);
