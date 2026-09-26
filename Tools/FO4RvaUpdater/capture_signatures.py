"""
Reads registry.json, disassembles each known RVA in the baseline Fallout4.exe
(capstone), and writes signatures.json: a wildcarded AOB pattern per site that
should still match after Bethesda relinks the binary, as long as the function's
own code bytes didn't change.

func sites: signature = the function's own instruction bytes, with
    relocation-dependent operands (call/jmp rel32, rip-relative disp32,
    movabs imm64) wildcarded out.
ptr sites: these are raw data addresses (globals/vtables), not code, so
    there's nothing to disassemble AT the RVA. Instead this finds an
    instruction in .text that references the pointer via RIP-relative
    addressing (e.g. `mov rax, cs:g_player`) and signs THAT instruction, with
    the displacement masked. Re-finding it after the update + reading its new
    displacement recovers the new pointer RVA. Built from a single resync-safe
    disassembly pass over .text (Cs.disasm silently stops at the first
    undecodable byte -- e.g. an embedded jump table or literal pool inside
    .text -- so a naive one-shot disasm misses almost everything after the
    first such blob; this resyncs by skipping a byte and retrying).

Every func signature's match count against the baseline exe is recorded
(match_count_in_baseline). It should be exactly 1 -- if it's not, the pattern
isn't unique enough and needs a longer capture window or manual review before
being trusted for the post-update rescan.
"""
import json
import re
import struct
from pathlib import Path

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP

ROOT = Path(__file__).parent
REGISTRY = ROOT / "registry.json"
EXE_PATH = ROOT / "Fallout4_1.11.221_baseline.exe"
OUT = ROOT / "signatures.json"

SIG_LEN = 64
PTR_CTX_BEFORE = 8
PTR_CTX_AFTER = 8


def load_pe(path):
    pe = pefile.PE(str(path), fast_load=True)
    pe.parse_data_directories()
    image = bytes(pe.get_memory_mapped_image())
    imagebase = pe.OPTIONAL_HEADER.ImageBase
    return pe, image, imagebase


def make_disassembler():
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    return md


def disasm_resync(md, data, base_addr):
    """Like md.disasm(), but when the linear sweep hits an undecodable byte
    (embedded data inside .text) it skips one byte and keeps going instead
    of silently stopping for the rest of the buffer."""
    pos = 0
    n = len(data)
    while pos < n:
        progressed = False
        for insn in md.disasm(data[pos:], base_addr + pos):
            yield insn
            pos += insn.size
            progressed = True
        if not progressed:
            pos += 1


def mask_instruction(insn):
    """Byte ranges (offset, length) within insn.bytes that are
    relocation/address-dependent and must be wildcarded."""
    masks = []
    b = insn.bytes

    if b[0] in (0xE8, 0xE9) and len(b) >= 5:            # call/jmp rel32
        masks.append((len(b) - 4, 4))
    elif b[0] == 0xEB or (0x70 <= b[0] <= 0x7F):          # short jmp/jcc rel8
        masks.append((len(b) - 1, 1))
    elif len(b) >= 2 and b[0] == 0x0F and 0x80 <= b[1] <= 0x8F:  # near jcc rel32
        masks.append((len(b) - 4, 4))

    try:
        for op in insn.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                disp_bytes = struct.pack('<i', op.mem.disp)
                idx = b.rfind(disp_bytes)
                if idx != -1:
                    masks.append((idx, 4))
            elif op.type == X86_OP_IMM and op.size == 8:
                imm = op.imm & 0xFFFFFFFFFFFFFFFF
                idx = b.rfind(struct.pack('<Q', imm))
                if idx != -1:
                    masks.append((idx, 8))
    except Exception:
        pass
    return masks


def sig_to_pattern(raw, mask):
    return " ".join("??" if m else f"{byte:02X}" for byte, m in zip(raw, mask))


def pattern_to_regex(pattern):
    parts = []
    for t in pattern.split(" "):
        if t == "??":
            parts.append(b".")
        else:
            parts.append(re.escape(bytes([int(t, 16)])))
    return re.compile(b"".join(parts), re.DOTALL)


def count_matches(image, pattern, search_start, search_end, cap=6):
    regex = pattern_to_regex(pattern)
    region = image[search_start:search_end]
    count = 0
    for _ in regex.finditer(region):
        count += 1
        if count >= cap:
            break
    return count


def find_text_section(pe, image):
    for s in pe.sections:
        if s.Name.rstrip(b"\x00").decode(errors="ignore") == ".text":
            return s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize
    return 0, len(image)


def build_func_signature(image, imagebase, rva, length):
    md = make_disassembler()
    raw = bytearray(image[rva:rva + length])
    mask = bytearray(len(raw))
    consumed = 0
    for insn in disasm_resync(md, bytes(raw), imagebase + rva):
        off = insn.address - (imagebase + rva)
        if off != consumed:
            break  # resync skipped a byte inside our short capture window -- stop, don't sign across a gap
        for (o, l) in mask_instruction(insn):
            for k in range(l):
                if off + o + k < len(mask):
                    mask[off + o + k] = 1
        consumed = off + insn.size
    return raw[:consumed], mask[:consumed]


def capture_func_signature(image, imagebase, entry, text_bounds):
    rva = entry["rva"]
    raw, mask = build_func_signature(image, imagebase, rva, SIG_LEN)
    if len(raw) < 16:
        return {**entry, "pattern": None, "note": f"only decoded {len(raw)} clean bytes, needs manual capture"}
    pattern = sig_to_pattern(raw, mask)
    count = count_matches(image, pattern, *text_bounds)
    return {
        **entry,
        "pattern": pattern,
        "pattern_bytes_total": len(raw),
        "pattern_wildcard_bytes": sum(mask),
        "match_count_in_baseline": count,
    }


def build_text_xref_index(image, imagebase, text_bounds):
    """Single pass over .text: map target_va -> list of (insn_rva, insn_bytes, insn_size)
    for every RIP-relative memory operand found. Reused for all ptr-kind entries."""
    text_start, text_end = text_bounds
    md = make_disassembler()
    data = image[text_start:text_end]
    index = {}
    for insn in disasm_resync(md, data, imagebase + text_start):
        try:
            for op in insn.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    target = insn.address + insn.size + op.mem.disp
                    index.setdefault(target, []).append(
                        (insn.address - imagebase, insn.bytes, insn.size))
        except Exception:
            continue
    return index


def capture_ptr_signature(image, imagebase, entry, xref_index):
    target_va = imagebase + entry["rva"]
    hits = xref_index.get(target_va, [])
    if not hits:
        return {**entry, "pattern": None, "xref_count": 0,
                "note": "no RIP-relative xref found in .text; needs manual re-derivation"}

    insn_rva, insn_bytes, insn_size = hits[0]
    start = insn_rva - PTR_CTX_BEFORE
    length = PTR_CTX_BEFORE + insn_size + PTR_CTX_AFTER
    raw = bytearray(image[start:start + length])
    mask = bytearray(len(raw))

    md = make_disassembler()
    for insn in md.disasm(bytes(insn_bytes), imagebase + insn_rva):
        for (o, l) in mask_instruction(insn):
            for k in range(l):
                idx = PTR_CTX_BEFORE + o + k
                if 0 <= idx < len(mask):
                    mask[idx] = 1
        break

    return {
        **entry,
        "pattern": sig_to_pattern(raw, mask),
        "pattern_bytes_total": len(raw),
        "insn_offset_in_pattern": PTR_CTX_BEFORE,
        "insn_size": insn_size,
        "xref_count": len(hits),
        "xref_rva_used": insn_rva,
    }


def main():
    registry = json.loads(REGISTRY.read_text())
    pe, image, imagebase = load_pe(EXE_PATH)
    text_bounds = find_text_section(pe, image)

    print("Building .text xref index (single pass, used by all ptr-kind sites)...")
    xref_index = build_text_xref_index(image, imagebase, text_bounds)
    print(f"  indexed {sum(len(v) for v in xref_index.values())} RIP-relative references "
          f"to {len(xref_index)} distinct targets")

    results = []
    for entry in registry:
        try:
            if entry["kind"] == "func":
                r = capture_func_signature(image, imagebase, entry, text_bounds)
            else:
                r = capture_ptr_signature(image, imagebase, entry, xref_index)
        except Exception as ex:
            r = {**entry, "pattern": None, "error": str(ex)}
        results.append(r)

    OUT.write_text(json.dumps(results, indent=2))

    unique = sum(1 for r in results if r.get("pattern") and r.get("match_count_in_baseline") == 1)
    ambiguous = sum(1 for r in results if r.get("kind") == "func" and r.get("pattern")
                     and r.get("match_count_in_baseline", 1) != 1)
    ptr_ok = sum(1 for r in results if r.get("kind") == "ptr" and r.get("pattern"))
    failed = sum(1 for r in results if not r.get("pattern"))
    print(f"Captured {len(results)} signatures -> {OUT}")
    print(f"  func unique match in baseline: {unique}")
    print(f"  func ambiguous (match_count != 1): {ambiguous}")
    print(f"  ptr xref found: {ptr_ok}")
    print(f"  failed / needs manual attention: {failed}")


if __name__ == "__main__":
    main()
