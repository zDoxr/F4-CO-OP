"""
IDA batch: Find SPECIAL stat formIDs and correct ActorValueInfo* access pattern
Run: idat.exe -A -S"find_special_formids.py" "Fallout4.exe.i64"
"""
import idc, idaapi, idautils, ida_hexrays, ida_funcs, sys

IMAGE_BASE = 0x140000000
OUT_FILE   = r"C:\Users\hotto\source\repos\DoxCoSync\F4 Update\special_formids_result.txt"

_fh = open(OUT_FILE, 'w')
def log(s=""):
    sys.stdout.write(s + "\n")
    _fh.write(s + "\n")
    _fh.flush()

def to_rva(a): return a - IMAGE_BASE
def rva(r):    return IMAGE_BASE + r

idaapi.auto_wait()
log("SPECIAL Stat FormID & ActorValueInfo* Access — IDA Pro 9.3")
log()

# ── 1. Decompile ActorValue::QSTRENGTH (only verified-prologue Q* getter) ───
log("=" * 60)
log("[1] ActorValue::QSTRENGTH  (RVA 0x0065EC60 — verified prologue)")
log("=" * 60)
if ida_hexrays.init_hexrays_plugin():
    func = ida_funcs.get_func(rva(0x0065EC60))
    if func:
        log("  Size: %d bytes" % (func.end_ea - func.start_ea))
        try:
            cfunc = ida_hexrays.decompile(func.start_ea)
            if cfunc:
                for i, line in enumerate(str(cfunc).split('\n')[:60]):
                    log("  %3d: %s" % (i, line))
        except Exception as e:
            log("  Error: %s" % e)

# ── 2. Disasm QSTRENGTH to see what global it loads ─────────────────────────
log()
log("=" * 60)
log("[2] Disassembly of ActorValue::QSTRENGTH (RVA 0x0065EC60):")
log("=" * 60)
func = ida_funcs.get_func(rva(0x0065EC60))
if func:
    for head in idautils.Heads(func.start_ea, func.end_ea):
        disline = idc.generate_disasm_line(head, 0) or ""
        mnem = idc.print_insn_mnem(head).upper()
        mark = "* " if mnem == "CALL" else "  "
        log("%s%08X: %s" % (mark, to_rva(head), disline))

# ── 3. Decompile ActorValue::InitializeActorValues (builds the AV list) ─────
log()
log("=" * 60)
log("[3] ActorValue::InitializeActorValues  (RVA 0x0069B1B0)")
log("=" * 60)
if ida_hexrays.init_hexrays_plugin():
    func = ida_funcs.get_func(rva(0x0069B1B0))
    if func:
        log("  Size: %d bytes" % (func.end_ea - func.start_ea))
        try:
            cfunc = ida_hexrays.decompile(func.start_ea)
            if cfunc:
                lines = str(cfunc).split('\n')
                log("  [First 120 lines]")
                for i, line in enumerate(lines[:120]):
                    # Highlight SPECIAL references
                    h = ">>>" if any(x in line for x in ['Strength','Perception','Endurance','Charisma','Intelligence','Agility','Luck','SPECIAL','special']) else "   "
                    log("  %s %3d: %s" % (h, i, line))
        except Exception as e:
            log("  Error: %s" % e)

# ── 4. Find all string refs to "Strength", "Perception" etc in rdata ─────────
log()
log("=" * 60)
log("[4] String references to SPECIAL names in .text (LEA to string literals):")
log("=" * 60)
SPECIAL_NAMES = ["Strength", "Perception", "Endurance", "Charisma", "Intelligence", "Agility", "Luck"]
for name_str in SPECIAL_NAMES:
    # Find the string in rdata
    rdata_start = IMAGE_BASE + 0x02438000
    rdata_end   = IMAGE_BASE + 0x02438000 + 0x00A94B42
    addr = idc.find_binary(rdata_start, idc.SEARCH_DOWN | idc.SEARCH_CASE,
                           ' '.join('%02X' % ord(c) for c in name_str + '\x00'))
    if addr != idc.BADADDR and addr < rdata_end:
        str_rva = to_rva(addr)
        log("  String '%s' at RVA 0x%08X" % (name_str, str_rva))
        # Find xrefs to this string
        xref_count = 0
        for xref in idautils.XrefsTo(addr):
            if xref.iscode:
                frm = xref.frm
                fname = idc.get_func_name(frm) or "???"
                func = ida_funcs.get_func(frm)
                frva = to_rva(func.start_ea) if func else 0
                log("    <- RVA 0x%08X in %s (func 0x%08X)" % (to_rva(frm), fname[:50], frva))
                xref_count += 1
        if xref_count == 0:
            log("    (no code xrefs found)")
    else:
        log("  String '%s': not found in .rdata as null-terminated" % name_str)

# ── 5. Look at ActorValue::RegisterActorValue to find SPECIAL registration ───
log()
log("=" * 60)
log("[5] Disasm ActorValue::AddHardcodedActorValuesToDataHandler (RVA 0x0069B220)")
log("=" * 60)
func = ida_funcs.get_func(rva(0x0069B220))
if func:
    log("  Size: %d bytes" % (func.end_ea - func.start_ea))
    if ida_hexrays.init_hexrays_plugin():
        try:
            cfunc = ida_hexrays.decompile(func.start_ea)
            if cfunc:
                lines = str(cfunc).split('\n')
                for i, line in enumerate(lines[:200]):
                    log("  %3d: %s" % (i, line))
        except Exception as e:
            log("  Error: %s" % e)

# ── 6. Check the ActorValueInfo formID field ─────────────────────────────────
# ActorValueInfo is a TESForm, formID is at +0x14 (offset from GameForms.h)
# Let's find what the game registers for Strength etc. by looking at globals
log()
log("=" * 60)
log("[6] Symbol search for SPECIAL-related globals and form IDs:")
log("=" * 60)
for addr_n, sym in idautils.Names():
    if any(x in sym for x in ['pSPECIAL', 'SPECIAL_AV', 'g_strength', 'g_perception',
                               'StrengthAV', 'HealthAV', 'g_avStrength']):
        log("  RVA 0x%08X : %s" % (to_rva(addr_n), sym))

log()
log("Done.")
_fh.close()
idc.qexit(0)
