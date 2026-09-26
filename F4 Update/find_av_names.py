"""
IDA batch: Find all ActorValueInfo-related symbols and decompile SetActorValue callers
Run: idat.exe -A -S"find_av_names.py" "Fallout4.exe.i64"
"""
import idc
import idaapi
import idautils
import ida_hexrays
import ida_funcs
import sys

IMAGE_BASE = 0x140000000
OUT_FILE   = r"C:\Users\hotto\source\repos\DoxCoSync\F4 Update\av_names_result.txt"

_fh = open(OUT_FILE, 'w')
def log(s=""):
    sys.stdout.write(s + "\n")
    _fh.write(s + "\n")
    _fh.flush()

def to_rva(a):
    return a - IMAGE_BASE

def rva(r):
    return IMAGE_BASE + r

idaapi.auto_wait()
log("ActorValueInfo Symbol & Decompile Search — IDA Pro")
log()

# ── 1. All symbols containing "ActorValue" ──────────────────────────────────
log("=" * 60)
log("[1] ALL IDA symbols containing 'ActorValue' or 'ActorVal':")
log("=" * 60)
count = 0
for addr_n, sym in idautils.Names():
    if 'ActorValue' in sym or 'actorvalue' in sym.lower():
        log("  RVA 0x%08X : %s" % (to_rva(addr_n), sym))
        count += 1
log("  Total: %d" % count)

# ── 2. Decompile sub_140C6F6A0 (simplest Actor_SetActorValue caller) ────────
log()
log("=" * 60)
log("[2] DECOMPILE sub_140C6F6A0  (RVA 0x00C6F6A0)  -- simplest SetAV caller")
log("=" * 60)
if ida_hexrays.init_hexrays_plugin():
    func = ida_funcs.get_func(rva(0x00C6F6A0))
    if func:
        log("  Size: %d bytes" % (func.end_ea - func.start_ea))
        try:
            cfunc = ida_hexrays.decompile(func.start_ea)
            if cfunc:
                code = str(cfunc)
                for i, line in enumerate(code.split('\n')):
                    log("  %3d: %s" % (i, line))
            else:
                log("  decompile() returned None")
        except Exception as e:
            log("  Error: %s" % e)
else:
    log("  Hex-Rays not available")

# ── 3. Decompile sub_140C6F850 (another SetAV caller) ──────────────────────
log()
log("=" * 60)
log("[3] DECOMPILE sub_140C6F850  (RVA 0x00C6F850)  -- another SetAV caller")
log("=" * 60)
if ida_hexrays.init_hexrays_plugin():
    func = ida_funcs.get_func(rva(0x00C6F850))
    if func:
        log("  Size: %d bytes" % (func.end_ea - func.start_ea))
        try:
            cfunc = ida_hexrays.decompile(func.start_ea)
            if cfunc:
                code = str(cfunc)
                for i, line in enumerate(code.split('\n')):
                    log("  %3d: %s" % (i, line))
        except Exception as e:
            log("  Error: %s" % e)

# ── 4. Search for functions that access offset 0x68 (avName field) ──────────
# avName is at +0x68 on ActorValueInfo. Functions that read [reg+68h] and
# compare or return it are likely related to name lookup.
# We look for the byte pattern: 48 8B ?? 68 (MOV r64, [reg+0x68])
log()
log("=" * 60)
log("[4] Functions accessing offset +0x68 that are near ActorValueInfo RTTI:")
log("    (scanning .text for MOV r64,[reg+68h] patterns near AV RTTI callers)")
log("=" * 60)

# Look for the exact instruction `mov rax, [rcx+68h]` etc.
TEXT_START = IMAGE_BASE + 0x00001000
TEXT_END   = IMAGE_BASE + 0x0242B1CC

import ida_bytes
# Pattern: 48 8B ?? 68 = MOV r64, [reg+0x68]
pattern = "48 8B ? 68"
# Use IDA's find_binary
addr = idc.find_binary(TEXT_START, idc.SEARCH_DOWN, pattern)
av_access_count = 0
while addr != idc.BADADDR and addr < TEXT_END and av_access_count < 50:
    func = ida_funcs.get_func(addr)
    fname = idc.get_func_name(addr) if func else "???"
    # Only print if in a function that deals with ActorValue (by name heuristic)
    if 'ActorValue' in fname or 'actorvalue' in fname.lower():
        log("  RVA 0x%08X in %s: %s" % (to_rva(addr), fname, idc.generate_disasm_line(addr, 0)))
    elif func:
        # Check if this function also calls Actor_SetActorValue
        for head in idautils.Heads(func.start_ea, func.end_ea):
            if idc.print_insn_mnem(head).upper() == "CALL":
                if idc.get_operand_value(head, 0) == rva(0x00C6F760):
                    log("  RVA 0x%08X in %s (also calls SetActorValue)" % (to_rva(addr), idc.get_func_name(func.start_ea)))
                    break
    addr = idc.find_binary(addr + 1, idc.SEARCH_DOWN, pattern)
    av_access_count += 1

# ── 5. Look at callers of ActorValueInfo::GetActorValueName ─────────────────
log()
log("=" * 60)
log("[5] Callers of ActorValueInfo::GetActorValueName (RVA 0x0069F620):")
log("=" * 60)
target = rva(0x0069F620)
for xref in idautils.XrefsTo(target):
    if not xref.iscode:
        continue
    frm = xref.frm
    fname = idc.get_func_name(frm) or "???"
    func  = ida_funcs.get_func(frm)
    fstart = to_rva(func.start_ea) if func else 0
    log("  CALL from RVA 0x%08X in %s (func start 0x%08X)" % (to_rva(frm), fname, fstart))

# ── 6. Look for any function that takes (const char*) and returns near AV RTTI
log()
log("=" * 60)
log("[6] DataHandler LookupFormByEditorID callers (search for 'LookupByEditorID' in names):")
log("=" * 60)
for addr_n, sym in idautils.Names():
    if 'LookupByEditorID' in sym or 'GetFormByEditorID' in sym or 'LookupForm' in sym:
        log("  RVA 0x%08X : %s" % (to_rva(addr_n), sym))

log()
log("Done.")
_fh.close()
idc.qexit(0)
