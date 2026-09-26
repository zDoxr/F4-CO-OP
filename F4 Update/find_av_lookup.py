"""
IDA Pro batch script: Find correct RVA for GetActorValueInfoByName
Run: idat64.exe -A -S"find_av_lookup.py" "Fallout4.exe.i64"
Output written to: F4 Update/av_lookup_result.txt

Strategy:
  The string "Invalid ActorValue '%s' for parameter %s." is at RVA 0x024ADAE0.
  A LEA referencing it exists at RVA 0x005B7FB8, inside a Papyrus dispatcher.
  We decompile that function, trace calls near the NULL check that precedes
  the error, and identify the actual lookup function entry point.
"""
import idc
import idaapi
import idautils
import ida_hexrays
import ida_funcs
import ida_xref
import ida_bytes
import os

IMAGE_BASE     = 0x140000000
STRING_RVA     = 0x024ADAE0  # "Invalid ActorValue '%s' for parameter %s."
LEA_RVA        = 0x005B7FB8  # confirmed LEA address
OUT_FILE       = r"C:\Users\hotto\source\repos\DoxCoSync\F4 Update\av_lookup_result.txt"

def rva(r):
    return IMAGE_BASE + r

out_lines = []
def log(s=""):
    print(s)
    out_lines.append(s)

log("=" * 70)
log("GetActorValueInfoByName RVA Search — IDA Pro Analysis")
log("Fallout 4 v1.11.191  (TimeDateStamp 0x693A1A4C)")
log("=" * 70)

# Wait for auto-analysis to finish
idaapi.auto_wait()

STRING_ADDR = rva(STRING_RVA)
LEA_ADDR    = rva(LEA_RVA)

# ── 1. Find all xrefs TO the Invalid ActorValue string ──────────────────────
log()
log("[1] XREFs to 'Invalid ActorValue' string at 0x%X:" % STRING_ADDR)
xref_list = []
for xref in idautils.XrefsTo(STRING_ADDR):
    frm = xref.frm
    xref_list.append(frm)
    fname = idc.get_func_name(frm) or "???"
    log("    0x%X  (RVA 0x%08X)  in %s" % (frm, frm - IMAGE_BASE, fname))

# ── 2. Function containing the LEA ─────────────────────────────────────────
log()
log("[2] Function containing LEA at RVA 0x%08X:" % LEA_RVA)
func = ida_funcs.get_func(LEA_ADDR)
if func:
    fstart = func.start_ea
    fend   = func.end_ea
    fname  = idc.get_func_name(fstart) or "???"
    log("    Name:  %s" % fname)
    log("    Start: 0x%X  (RVA 0x%08X)" % (fstart, fstart - IMAGE_BASE))
    log("    End:   0x%X  (RVA 0x%08X)" % (fend,   fend   - IMAGE_BASE))
    log("    Size:  %d bytes" % (fend - fstart))
else:
    log("    WARNING: no function found at 0x%X" % LEA_ADDR)
    func = None

# ── 3. All CALL instructions in the dispatcher ──────────────────────────────
if func:
    log()
    log("[3] All CALL instructions in dispatcher (sorted by address):")
    calls = []
    for head in idautils.Heads(func.start_ea, func.end_ea):
        mnem = idc.print_insn_mnem(head).upper()
        if mnem == "CALL":
            target = idc.get_operand_value(head, 0)
            tname  = idc.get_func_name(target) or ("0x%X" % target)
            tname_short = (tname[:60] + "...") if len(tname) > 60 else tname
            dist = head - LEA_ADDR
            calls.append((head, target, tname_short, dist))

    for addr, tgt, tname, dist in calls:
        marker = ""
        if -8192 < dist < 0:
            marker = "  <<< BEFORE error LEA (%+d bytes)" % dist
        elif 0 <= dist < 2048:
            marker = "  --- AFTER error LEA (+%d bytes)" % dist
        log("    CALL@0x%08X -> RVA 0x%08X  [%s]%s" % (
            addr - IMAGE_BASE, tgt - IMAGE_BASE, tname, marker))
    log("    Total: %d calls" % len(calls))

    # ── 4. Prime candidates: calls 1-8192 bytes before the error LEA ────────
    log()
    log("[4] Prime candidates (CALL within 8KB before error LEA):")
    candidates = [(a, t, n, d) for a, t, n, d in calls if -8192 < d < 0]
    if candidates:
        for addr, tgt, tname, dist in candidates:
            log("    RVA 0x%08X -> TARGET RVA 0x%08X  [%s]  (%+d from LEA)" % (
                addr - IMAGE_BASE, tgt - IMAGE_BASE, tname, dist))

        # For each candidate target, read 32 bytes of prologue
        log()
        log("[4b] Prologue bytes of candidate call targets:")
        seen = set()
        for addr, tgt, tname, dist in candidates:
            if tgt in seen:
                continue
            seen.add(tgt)
            prologue = " ".join("%02X" % idc.get_wide_byte(tgt + i) for i in range(32))
            log("    RVA 0x%08X: %s  [%s]" % (tgt - IMAGE_BASE, prologue, tname))
    else:
        log("    (none — dispatcher likely uses only indirect/vtable calls)")
        # If no direct calls, look for indirect calls (CALL [REG] / CALL [RIP+x])
        log()
        log("[4c] Checking for indirect CALLs near LEA (within 2KB before):")
        for head in idautils.Heads(max(func.start_ea, LEA_ADDR - 2048), LEA_ADDR):
            mnem = idc.print_insn_mnem(head).upper()
            if mnem == "CALL":
                op_type = idc.get_operand_type(head, 0)
                # op_type 1=direct, 2=mem, 3=phrase, 4=displ, etc.
                disasm = idc.generate_disasm_line(head, 0)
                dist = head - LEA_ADDR
                log("    0x%08X (RVA): %s  op_type=%d  dist=%+d" % (
                    head - IMAGE_BASE, disasm, op_type, dist))

# ── 5. Decompile and show context around "Invalid ActorValue" ───────────────
if func and ida_hexrays.init_hexrays_plugin():
    log()
    log("[5] Hex-Rays decompiler output (context around 'Invalid ActorValue'):")
    try:
        cfunc = ida_hexrays.decompile(func.start_ea)
        if cfunc:
            code = str(cfunc)
            lines = code.split('\n')
            for i, line in enumerate(lines):
                if 'Invalid' in line or ('ActorValue' in line and 'param' in line.lower()):
                    start = max(0, i - 15)
                    end   = min(len(lines), i + 15)
                    log("    [decompiler context — line %d]" % i)
                    for j in range(start, end):
                        marker = ">>>" if j == i else "   "
                        log("    %s %s" % (marker, lines[j]))
                    log()
    except Exception as e:
        log("    Decompiler error: %s" % e)
else:
    log()
    log("[5] Hex-Rays not available or not licensed — skipping decompilation.")

# ── 6. Search for GetActorValueInfoByName by name in symbol table ────────────
log()
log("[6] Symbol search for 'GetActorValueInfo' or 'ActorValueList':")
for name in idautils.Names():
    addr_n, sym = name
    if 'ActorValue' in sym and ('ByName' in sym or 'Lookup' in sym or 'List' in sym or 'Info' in sym):
        log("    0x%X (RVA 0x%08X): %s" % (addr_n, addr_n - IMAGE_BASE, sym))

# ── Write output file ────────────────────────────────────────────────────────
log()
log("=" * 70)
log("Output written to: %s" % OUT_FILE)
log("=" * 70)

try:
    with open(OUT_FILE, 'w') as f:
        f.write('\n'.join(out_lines))
    print("Output file written OK.")
except Exception as e:
    print("Failed to write output: %s" % e)

idc.qexit(0)
