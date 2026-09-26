"""
IDA Pro batch script: Decompile candidate GetActorValueInfoByName functions
Run: idat.exe -A -S"find_av_final.py" "Fallout4.exe.i64"

Targets:
  Script::GetActorValueInfoFunction  RVA 0x00521410  (from symbol table)
  sub_140C6F350                      RVA 0x00C6F350  (called before every SetActorValue)
"""
import idc
import idaapi
import idautils
import ida_hexrays
import ida_funcs
import ida_xref

IMAGE_BASE = 0x140000000
OUT_FILE   = r"C:\Users\hotto\source\repos\DoxCoSync\F4 Update\av_final_result.txt"

out_lines = []
def log(s=""):
    print(s)
    out_lines.append(s)

log("=" * 70)
log("GetActorValueInfoByName — Final Candidate Decompilation")
log("=" * 70)

idaapi.auto_wait()

def rva(r):
    return IMAGE_BASE + r

def to_rva(a):
    return a - IMAGE_BASE

def decompile_rva(target_rva, label):
    addr = rva(target_rva)
    log()
    log("━" * 60)
    log("DECOMPILE: %s  (RVA 0x%08X)" % (label, target_rva))
    log("━" * 60)

    func = ida_funcs.get_func(addr)
    if not func:
        log("  ERROR: No function at this address.")
        return

    log("  Function name: %s" % (idc.get_func_name(addr) or "???"))
    log("  Range: RVA 0x%08X - 0x%08X  size=%d bytes" % (
        to_rva(func.start_ea), to_rva(func.end_ea), func.end_ea - func.start_ea))

    # List all calls in this function
    log("  Calls:")
    for head in idautils.Heads(func.start_ea, func.end_ea):
        if idc.print_insn_mnem(head).upper() == "CALL":
            tgt = idc.get_operand_value(head, 0)
            tname = idc.get_func_name(tgt) or ("0x%X" % tgt)
            log("    CALL@0x%08X -> RVA 0x%08X [%s]" % (to_rva(head), to_rva(tgt), tname[:80]))

    # Decompile
    if not ida_hexrays.init_hexrays_plugin():
        log("  Hex-Rays not available.")
        return
    try:
        cfunc = ida_hexrays.decompile(func.start_ea)
        if cfunc:
            code = str(cfunc)
            lines = code.split('\n')
            log("  [Decompiled — %d lines]" % len(lines))
            for i, line in enumerate(lines):
                log("  %4d: %s" % (i, line))
        else:
            log("  Decompile returned None.")
    except Exception as e:
        log("  Decompiler error: %s" % e)

# Primary targets
decompile_rva(0x00521410, "Script::GetActorValueInfoFunction")
decompile_rva(0x00C6F350, "sub_140C6F350 (called before SetActorValue)")

# Also decompile the Papyrus GetActorValue native handler (sub_14059AB30)
# This takes an ActorValueInfo* (from runtime) and reads the value
decompile_rva(0x0059AB30, "GetActorValue Papyrus handler (sub_14059AB30)")

# Also check what calls Script::GetActorValueInfoFunction
log()
log("=" * 60)
log("CALLERS of Script::GetActorValueInfoFunction (RVA 0x00521410):")
log("=" * 60)
target = rva(0x00521410)
for xref in idautils.XrefsTo(target):
    frm = xref.frm
    if not xref.iscode:
        continue
    fname = idc.get_func_name(frm) or "???"
    func = ida_funcs.get_func(frm)
    frva = to_rva(func.start_ea) if func else 0
    log("    CALL from 0x%08X in %s (func start RVA 0x%08X)" % (to_rva(frm), fname, frva))

# Write output
log()
log("=" * 70)
try:
    with open(OUT_FILE, 'w') as f:
        f.write('\n'.join(out_lines))
    log("Output written to: %s" % OUT_FILE)
except Exception as e:
    log("Write failed: %s" % e)

idc.qexit(0)
