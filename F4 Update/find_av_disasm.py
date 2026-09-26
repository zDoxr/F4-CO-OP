"""
IDA Pro batch: Find GetActorValueInfoByName via disassembly (no decompiler needed)
Run: idat.exe -A -S"find_av_disasm.py" "Fallout4.exe.i64"
"""
import idc
import idaapi
import idautils
import ida_hexrays
import ida_funcs
import ida_xref
import sys

IMAGE_BASE = 0x140000000
OUT_FILE   = r"C:\Users\hotto\source\repos\DoxCoSync\F4 Update\av_disasm_result.txt"

# Write incrementally
_fh = open(OUT_FILE, 'w')

def log(s=""):
    sys.stdout.write(s + "\n")
    _fh.write(s + "\n")
    _fh.flush()

def rva(r):
    return IMAGE_BASE + r

def to_rva(a):
    return a - IMAGE_BASE

def disasm_func(target_rva, label, max_lines=200):
    addr = rva(target_rva)
    log()
    log("=" * 60)
    log("DISASM: %s  (RVA 0x%08X)" % (label, target_rva))
    log("=" * 60)

    func = ida_funcs.get_func(addr)
    if not func:
        log("  ERROR: No function at RVA 0x%08X" % target_rva)
        return

    fname = idc.get_func_name(addr) or "???"
    log("  Name:  %s" % fname)
    log("  Range: RVA 0x%08X - 0x%08X  size=%d" % (
        to_rva(func.start_ea), to_rva(func.end_ea), func.end_ea - func.start_ea))
    log()

    count = 0
    for head in idautils.Heads(func.start_ea, func.end_ea):
        if count >= max_lines:
            log("  [... truncated at %d lines ...]" % max_lines)
            break
        disline = idc.generate_disasm_line(head, 0) or ""
        mnem = idc.print_insn_mnem(head).upper()
        prefix = "  "
        if mnem == "CALL":
            tgt = idc.get_operand_value(head, 0)
            tname = idc.get_func_name(tgt) or ""
            if tname:
                disline += "  ; -> [%s]" % tname[:60]
            prefix = "* "
        log("%s%08X: %s" % (prefix, to_rva(head), disline))
        count += 1

log("GetActorValueInfoByName Search — IDA Disassembly")
log("Fallout 4 v1.11.191")
log()

idaapi.auto_wait()
log("Auto-analysis complete. Starting disassembly...")

# 1. Script::GetActorValueInfoFunction at RVA 0x00521410
disasm_func(0x00521410, "Script::GetActorValueInfoFunction")

# 2. sub_140C6F350 at RVA 0x00C6F350 (called before every Actor_SetActorValue)
disasm_func(0x00C6F350, "sub_140C6F350 (called before Actor_SetActorValue)")

# 3. Callers of Script::GetActorValueInfoFunction
log()
log("=" * 60)
log("CALLERS of Script::GetActorValueInfoFunction (RVA 0x00521410):")
log("=" * 60)
for xref in idautils.XrefsTo(rva(0x00521410)):
    if not xref.iscode:
        continue
    frm = xref.frm
    fname = idc.get_func_name(frm) or "???"
    func = ida_funcs.get_func(frm)
    frva = to_rva(func.start_ea) if func else 0
    log("  CALL from RVA 0x%08X in %s  (func start RVA 0x%08X)" % (to_rva(frm), fname, frva))

# 4. Also try decompiler on sub_140C6F350 if available
log()
log("=" * 60)
log("DECOMPILE sub_140C6F350 (RVA 0x00C6F350):")
log("=" * 60)
try:
    if ida_hexrays.init_hexrays_plugin():
        func = ida_funcs.get_func(rva(0x00C6F350))
        if func:
            cfunc = ida_hexrays.decompile(func.start_ea)
            if cfunc:
                code = str(cfunc)
                for i, line in enumerate(code.split('\n')[:100]):
                    log("  %3d: %s" % (i, line))
            else:
                log("  decompile() returned None")
    else:
        log("  Hex-Rays not initialized")
except Exception as e:
    log("  Decompiler exception: %s" % e)

# 5. Decompile Script::GetActorValueInfoFunction
log()
log("=" * 60)
log("DECOMPILE Script::GetActorValueInfoFunction (RVA 0x00521410):")
log("=" * 60)
try:
    if ida_hexrays.init_hexrays_plugin():
        func = ida_funcs.get_func(rva(0x00521410))
        if func:
            cfunc = ida_hexrays.decompile(func.start_ea)
            if cfunc:
                code = str(cfunc)
                for i, line in enumerate(code.split('\n')[:150]):
                    log("  %3d: %s" % (i, line))
            else:
                log("  decompile() returned None")
    else:
        log("  Hex-Rays not initialized")
except Exception as e:
    log("  Decompiler exception: %s" % e)

log()
log("Done.")
_fh.close()

idc.qexit(0)
