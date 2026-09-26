"""
IDA Pro batch script: Find runtime GetActorValueInfoByName
Run: idat.exe -A -S"find_av_runtime.py" "Fallout4.exe.i64"

The Papyrus compiler function was a dead end. The runtime lookup function:
  - Takes a const char* (ActorValue name like "Health")
  - Returns ActorValueInfo*
  - Is called from Papyrus native bindings for GetActorValue/SetActorValue

Strategy:
  1. Find Actor_SetActorValue at RVA 0x00C6F760 (verified OK)
     Look at its callers — they must get an ActorValueInfo* somehow
  2. Search for Papyrus native bindings that deal with actor values at runtime
     (search for functions calling Actor_SetActorValue that also do a string->AV lookup)
  3. Check the strings from the Health_Combat filtered list:
     "GetActorValue: %s >> %0.2f" at RVA 0x024A7B70 — this is the RUNTIME log string
     Find what function references it
  4. Check callers of Actor_SetActorValue and look for a pattern like:
     [call something(string)] -> [check not null] -> [call Actor_SetActorValue]
"""
import idc
import idaapi
import idautils
import ida_hexrays
import ida_funcs
import ida_xref
import ida_bytes

IMAGE_BASE = 0x140000000
OUT_FILE   = r"C:\Users\hotto\source\repos\DoxCoSync\F4 Update\av_runtime_result.txt"

# Known verified RVAs
ACTOR_SET_AV_RVA    = 0x00C6F760  # Actor_SetActorValue (verified)
GETAV_LOG_STR_RVA   = 0x024A7B70  # "GetActorValue: %s >> %0.2f" (runtime log)
GETBASE_LOG_STR_RVA = 0x024A7BE0  # "GetBaseActorValue: %s >> %0.2f"

out_lines = []
def log(s=""):
    print(s)
    out_lines.append(s)

log("=" * 70)
log("GetActorValueInfoByName Runtime Search — IDA Pro")
log("=" * 70)

idaapi.auto_wait()

def rva(r):
    return IMAGE_BASE + r

def to_rva(addr):
    return addr - IMAGE_BASE

ACTOR_SET_AV_ADDR = rva(ACTOR_SET_AV_RVA)
GETAV_LOG_ADDR    = rva(GETAV_LOG_STR_RVA)
GETBASE_LOG_ADDR  = rva(GETBASE_LOG_STR_RVA)

# ── 1. Find function referencing "GetActorValue: %s >> %0.2f" ──────────────
log()
log("[1] Functions referencing runtime 'GetActorValue: %%s >> %%0.2f' string (RVA 0x%08X):" % GETAV_LOG_STR_RVA)
av_log_refs = []
for xref in idautils.XrefsTo(GETAV_LOG_ADDR):
    frm = xref.frm
    fname = idc.get_func_name(frm) or "???"
    func = ida_funcs.get_func(frm)
    fstart_rva = to_rva(func.start_ea) if func else 0
    av_log_refs.append((frm, fname, fstart_rva))
    log("    0x%X (RVA 0x%08X) in %s  [func start RVA 0x%08X]" % (
        frm, to_rva(frm), fname, fstart_rva))

log()
log("[2] Functions referencing runtime 'GetBaseActorValue: %%s >> %%0.2f' string (RVA 0x%08X):" % GETBASE_LOG_STR_RVA)
for xref in idautils.XrefsTo(GETBASE_LOG_ADDR):
    frm = xref.frm
    fname = idc.get_func_name(frm) or "???"
    func = ida_funcs.get_func(frm)
    fstart_rva = to_rva(func.start_ea) if func else 0
    log("    0x%X (RVA 0x%08X) in %s  [func start RVA 0x%08X]" % (
        frm, to_rva(frm), fname, fstart_rva))

# ── 3. Callers of Actor_SetActorValue ─────────────────────────────────────
log()
log("[3] All callers of Actor_SetActorValue (RVA 0x%08X):" % ACTOR_SET_AV_RVA)
set_av_callers = []
for xref in idautils.XrefsTo(ACTOR_SET_AV_ADDR):
    frm = xref.frm
    if not xref.iscode:
        continue
    fname = idc.get_func_name(frm) or "???"
    func = ida_funcs.get_func(frm)
    fstart_rva = to_rva(func.start_ea) if func else 0
    set_av_callers.append((frm, fname, fstart_rva, func))
    log("    0x%08X (RVA 0x%08X) in %s  [func RVA 0x%08X]" % (
        frm, to_rva(frm), fname, fstart_rva))

# ── 4. For each caller of SetActorValue, find what calls are made BEFORE it ─
log()
log("[4] Calls made WITHIN each Actor_SetActorValue caller function:")
for caller_addr, fname, fstart_rva, func in set_av_callers[:10]:  # limit to first 10
    if not func:
        continue
    log()
    log("  Function %s (start RVA 0x%08X):" % (fname, fstart_rva))
    calls_in_func = []
    for head in idautils.Heads(func.start_ea, func.end_ea):
        mnem = idc.print_insn_mnem(head).upper()
        if mnem == "CALL":
            target = idc.get_operand_value(head, 0)
            tname  = idc.get_func_name(target) or ("0x%X" % target)
            tname_s = (tname[:50] + "...") if len(tname) > 50 else tname
            calls_in_func.append((head, target, tname_s))
    for caddr, ctgt, ctname in calls_in_func:
        log("    CALL@RVA 0x%08X -> 0x%08X [%s]" % (to_rva(caddr), to_rva(ctgt), ctname))

# ── 5. Decompile GetActorValue log function ──────────────────────────────────
log()
log("[5] Decompile function containing 'GetActorValue: %%s >> %%0.2f':")
if av_log_refs and ida_hexrays.init_hexrays_plugin():
    for frm_addr, fname, fstart_rva in av_log_refs[:2]:
        func = ida_funcs.get_func(frm_addr)
        if not func:
            continue
        log()
        log("  Function: %s  start=RVA 0x%08X  size=%d" % (
            fname, fstart_rva, func.end_ea - func.start_ea))
        try:
            cfunc = ida_hexrays.decompile(func.start_ea)
            if cfunc:
                code = str(cfunc)
                # Print full decompiled function (it's likely small)
                lines = code.split('\n')
                log("  [First 80 lines of decompiled code]")
                for i, line in enumerate(lines[:80]):
                    log("  %4d: %s" % (i, line))
        except Exception as e:
            log("  Decompiler error: %s" % e)

# ── 6. Also look for functions with "avName" access — offset 0x68 ────────────
log()
log("[6] RTTI / symbol search for ActorValueList or GetActorValueInfoByName:")
found_syms = []
for name in idautils.Names():
    addr_n, sym = name
    sym_lower = sym.lower()
    if any(kw in sym_lower for kw in ['actorvaluelist', 'getactorvalue', 'avbyname', 'avlookup', 'avinfo']):
        found_syms.append((addr_n, sym))
        log("    0x%08X (RVA 0x%08X): %s" % (addr_n, to_rva(addr_n), sym))

if not found_syms:
    log("    (no symbols found with those names)")

# ── 7. Find 'ActorValueList::GetSingleton' via RTTI string ──────────────────
log()
log("[7] XREFs to ActorValueInfo RTTI (.?AVActorValueInfo@@) at RVA 0x02F94628:")
av_rtti_addr = rva(0x02F94628)
rtti_refs = []
for xref in idautils.XrefsTo(av_rtti_addr):
    frm = xref.frm
    fname = idc.get_func_name(frm) or "???"
    rtti_refs.append((frm, fname))
    log("    0x%08X (RVA 0x%08X) in %s" % (frm, to_rva(frm), fname))

# ── Write output ─────────────────────────────────────────────────────────────
log()
log("=" * 70)
try:
    with open(OUT_FILE, 'w') as f:
        f.write('\n'.join(out_lines))
    log("Output written to: %s" % OUT_FILE)
except Exception as e:
    log("Write failed: %s" % e)
log("=" * 70)

idc.qexit(0)
