"""
Run this AFTER tomorrow's Fallout 4 update installs.

Usage:
    python rescan_after_update.py "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Fallout 4\\Fallout4.exe"

Reads signatures.json (captured from the 1.11.221 baseline by
capture_signatures.py) and re-finds every site's byte pattern in the updated
exe. Writes rva_diff_report.txt (human-readable) and rva_diff_report.json.

This does NOT touch any source file -- it only reports what it found. For
every site the report gives one of:
  OK          - pattern matched exactly once, new RVA resolved with confidence
  UNCHANGED   - new RVA is identical to the old one
  AMBIGUOUS   - pattern matched more than once, needs manual disambiguation
  NOT_FOUND   - pattern didn't match at all; the function's code likely
                changed in the update and needs a fresh IDA look
  SKIPPED     - this site had no usable pattern even in the baseline capture
                (already flagged manual-review in signatures.json)
"""
import json
import re
import struct
import sys
from pathlib import Path

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

ROOT = Path(__file__).parent
SIGNATURES = ROOT / "signatures.json"
OUT_JSON = ROOT / "rva_diff_report.json"
OUT_TXT = ROOT / "rva_diff_report.txt"


def load_pe(path):
    pe = pefile.PE(str(path), fast_load=True)
    pe.parse_data_directories()
    image = bytes(pe.get_memory_mapped_image())
    imagebase = pe.OPTIONAL_HEADER.ImageBase
    return pe, image, imagebase


def pattern_to_regex(pattern):
    parts = []
    for t in pattern.split(" "):
        if t == "??":
            parts.append(b".")
        else:
            parts.append(re.escape(bytes([int(t, 16)])))
    return re.compile(b"".join(parts), re.DOTALL)


def find_all(image, pattern, cap=6):
    regex = pattern_to_regex(pattern)
    hits = []
    for m in regex.finditer(image):
        hits.append(m.start())
        if len(hits) >= cap:
            break
    return hits


def resolve_ptr_rva(image, imagebase, match_start, entry):
    """Given a matched instruction window in the new image, re-disassemble the
    instruction at insn_offset_in_pattern to recover its new displacement and
    therefore the new target (pointer) RVA."""
    insn_offset = entry["insn_offset_in_pattern"]
    insn_size = entry["insn_size"]
    insn_rva = match_start + insn_offset
    insn_bytes = image[insn_rva:insn_rva + insn_size]

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    for insn in md.disasm(bytes(insn_bytes), imagebase + insn_rva):
        for op in insn.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                target_va = insn.address + insn.size + op.mem.disp
                return target_va - imagebase
        break
    return None


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    new_exe_path = Path(sys.argv[1])
    if not new_exe_path.exists():
        print(f"exe not found: {new_exe_path}")
        sys.exit(1)

    signatures = json.loads(SIGNATURES.read_text())
    pe, image, imagebase = load_pe(new_exe_path)
    print(f"Scanning {new_exe_path} (imagebase {hex(imagebase)}, {len(image)} bytes mapped)")

    report = []
    for entry in signatures:
        name, old_rva, kind = entry["name"], entry["rva"], entry["kind"]
        pattern = entry.get("pattern")

        if not pattern:
            report.append({**entry, "status": "SKIPPED",
                            "reason": entry.get("note", "no baseline pattern")})
            continue

        hits = find_all(image, pattern)

        if not hits:
            report.append({**entry, "status": "NOT_FOUND", "candidates": []})
            continue

        if kind == "func":
            new_rva = hits[0]
            status = "OK" if len(hits) == 1 else "AMBIGUOUS"
            status = "UNCHANGED" if (status == "OK" and new_rva == old_rva) else status
            report.append({**entry, "status": status, "new_rva": new_rva,
                            "candidates": hits})
        else:  # ptr
            resolved = []
            for h in hits:
                r = resolve_ptr_rva(image, imagebase, h, entry)
                if r is not None:
                    resolved.append(r)
            resolved_unique = sorted(set(resolved))
            if not resolved_unique:
                report.append({**entry, "status": "NOT_FOUND", "candidates": []})
            elif len(resolved_unique) == 1:
                new_rva = resolved_unique[0]
                status = "UNCHANGED" if new_rva == old_rva else "OK"
                report.append({**entry, "status": status, "new_rva": new_rva,
                                "candidates": resolved_unique, "xref_matches_used": len(hits)})
            else:
                report.append({**entry, "status": "AMBIGUOUS",
                                "candidates": resolved_unique, "xref_matches_used": len(hits)})

    OUT_JSON.write_text(json.dumps(report, indent=2))

    lines = []
    by_status = {}
    for r in report:
        by_status.setdefault(r["status"], []).append(r)

    lines.append(f"FO4 RVA update report vs {new_exe_path.name}")
    lines.append("=" * 70)
    for status in ("NOT_FOUND", "AMBIGUOUS", "SKIPPED", "OK", "UNCHANGED"):
        items = by_status.get(status, [])
        if not items:
            continue
        lines.append(f"\n[{status}] ({len(items)})")
        for r in items:
            loc = f"{r['file']}:{r['line']}"
            if status in ("OK",):
                lines.append(f"  {r['name']:35s} old=0x{r['rva']:08X} -> new=0x{r['new_rva']:08X}  ({loc})")
            elif status == "UNCHANGED":
                lines.append(f"  {r['name']:35s} 0x{r['rva']:08X} unchanged  ({loc})")
            elif status == "AMBIGUOUS":
                cands = ", ".join(f"0x{c:08X}" for c in r.get("candidates", []))
                lines.append(f"  {r['name']:35s} old=0x{r['rva']:08X} candidates=[{cands}]  ({loc})")
            elif status == "NOT_FOUND":
                lines.append(f"  {r['name']:35s} old=0x{r['rva']:08X} -- needs fresh IDA look  ({loc})")
            elif status == "SKIPPED":
                lines.append(f"  {r['name']:35s} old=0x{r['rva']:08X} -- {r.get('reason','')}  ({loc})")

    OUT_TXT.write_text("\n".join(lines))
    print("\n".join(lines))
    print(f"\nWrote {OUT_JSON} and {OUT_TXT}")


if __name__ == "__main__":
    main()
