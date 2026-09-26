"""
IDA batch: Dump disassembly of ActorValue init functions to find SPECIAL formIDs
Run: idat.exe -A -S"find_special_disasm.py" "Fallout4.exe.i64"
"""
import idc, idaapi, idautils, ida_funcs, sys

IMAGE_BASE = 0x140000000
OUT_FILE   = r"C:\Users\hotto\source\repos\DoxCoSync\F4 Update\special_disasm_result.txt"

_fh = open(OUT_FILE, 'w')
def log(s=""):
    _fh.write(s + "\n")
    _fh.flush()

def to_rva(a): return a - IMAGE_BASE
def rva(r):    return IMAGE_BASE + r

idaapi.auto_wait()
log("SPECIAL formID search via disassembly — IDA Pro 9.3")
log()

# ── 1. Dump full disasm of AddHardcodedActorValuesToDataHandler ──────────────
log("=" * 60)
log("[1] Full disasm: AddHardcodedActorValuesToDataHandler (RVA 0x0069B220)")
log("=" * 60)
func = ida_funcs.get_func(rva(0x0069B220))
if func:
    log("  Size: %d bytes, end RVA: 0x%08X" % (func.end_ea - func.start_ea, to_rva(func.end_ea)))
    count = 0
    for head in idautils.Heads(func.start_ea, func.end_ea):
        disline = idc.generate_disasm_line(head, 0) or ""
        # Highlight immediates that could be formIDs (0x100 - 0x1000000)
        ops = disline.upper()
        mark = ""
        for op_idx in range(8):
            val = idc.get_operand_value(head, op_idx)
            if 0x100 <= val <= 0x00FFFFFF:
                mark = "*** "
                break
        log("%s%08X: %s" % (mark, to_rva(head), disline))
        count += 1
    log("  Total instructions: %d" % count)
else:
    log("  Function not found at RVA 0x0069B220")

# ── 2. Also try the actual InitializeActorValues entry ──────────────────────
log()
log("=" * 60)
log("[2] Full disasm: InitializeActorValues (RVA 0x0069B1B0)")
log("    (looking for the real entry — may JUMPOUT to 0x0069B1AE)")
log("=" * 60)
# Try the JUMPOUT target 0x0069B1AE
for target_rva in [0x0069B1B0, 0x0069B1AE, 0x0069B160]:
    func = ida_funcs.get_func(rva(target_rva))
    if func:
        log("  At 0x%08X: func starts 0x%08X, size %d bytes" % (
            target_rva, to_rva(func.start_ea), func.end_ea - func.start_ea))
        if func.size() > 600:  # The 636-byte function
            log("  Dumping first 100 instructions:")
            n = 0
            for head in idautils.Heads(func.start_ea, func.end_ea):
                disline = idc.generate_disasm_line(head, 0) or ""
                mark = ""
                for op_idx in range(8):
                    val = idc.get_operand_value(head, op_idx)
                    if 0x100 <= val <= 0x00FFFFFF:
                        mark = "*** "
                        break
                log("%s%08X: %s" % (mark, to_rva(head), disline))
                n += 1
                if n >= 150:
                    log("  [... truncated at 150 instructions ...]")
                    break
        break

# ── 3. Search .data for formID pattern: look for 8 consecutive DWORDs ────────
# SPECIAL form IDs are assigned at startup; look in data section for a table.
# ActorValueInfo* globals in .data — each points to an ActorValueInfo TESForm.
# The formID is at TESForm+0x14. We can't dereference at static analysis time,
# but we can look for the global pointers that hold the SPECIAL AVIs.
log()
log("=" * 60)
log("[3] Named globals that look like AV pointer tables")
log("=" * 60)
for addr_n, sym in idautils.Names():
    sym_lower = sym.lower()
    if any(x in sym_lower for x in ['strength', 'perception', 'endurance', 'charisma',
                                      'intelligence', 'agility', 'luck', 'special',
                                      'avinfo', 'actorval', 'g_av', 'g_special']):
        log("  RVA 0x%08X : %s" % (to_rva(addr_n), sym))

# ── 4. Search for all calls to LookupFormByID near ActorValue init ────────────
log()
log("=" * 60)
log("[4] XREFs to LookupFormByID (RVA 0x00311850) from AV-related functions")
log("=" * 60)
lookup_ea = rva(0x00311850)
xref_count = 0
for xref in idautils.XrefsTo(lookup_ea):
    if xref.iscode:
        frm = xref.frm
        fname = idc.get_func_name(frm) or "???"
        func = ida_funcs.get_func(frm)
        frva = to_rva(func.start_ea) if func else 0
        # Show 5 instructions around the call to see what formID is being loaded
        prev_ea = frm
        context_lines = []
        for _ in range(5):
            prev_ea = idc.prev_head(prev_ea, func.start_ea if func else 0)
            if prev_ea == idc.BADADDR:
                break
            disline = idc.generate_disasm_line(prev_ea, 0) or ""
            context_lines.insert(0, "    %08X: %s" % (to_rva(prev_ea), disline))
        log("  <- RVA 0x%08X in func 0x%08X (%s)" % (to_rva(frm), frva, fname[:50]))
        for cl in context_lines:
            log(cl)
        log("    %08X: [call LookupFormByID]" % to_rva(frm))
        log()
        xref_count += 1
log("  Total xrefs: %d" % xref_count)

log()
log("Done.")
_fh.close()
idc.qexit(0)
