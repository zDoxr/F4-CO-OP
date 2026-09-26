"""
Parses the DoxCoSync source tree for RelocAddr<>/RelocPtr<> declarations
and writes registry.json: the list of RVA sites this tool tracks.

RelocAddr<> = function entry point in .text -> code signature scan applies.
RelocPtr<>  = raw global pointer in .data/.rdata -> resolved via an xref
              instruction in .text (see capture_signatures.py).
"""
import json
import re
from pathlib import Path

SRC_ROOT = Path(r"C:\Users\hotto\Desktop\New folder (2)\DoxCoSync")
OUT = Path(__file__).parent / "registry.json"

PATTERN = re.compile(
    r"Reloc(?P<kind>Addr|Ptr)<(?P<type>[^>]*)>\s+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\(\s*0x(?P<rva>[0-9A-Fa-f]+)\s*\)"
)

def main():
    entries = []
    for ext in ("*.cpp", "*.h"):
        for path in SRC_ROOT.rglob(ext):
            try:
                text = path.read_text(encoding="utf-8", errors="ignore")
            except Exception:
                continue
            for lineno, line in enumerate(text.splitlines(), start=1):
                m = PATTERN.search(line)
                if not m:
                    continue
                # RelocAddr<uintptr_t> is used in this codebase to hold raw
                # data addresses (vtables etc), not function entry points --
                # despite the "Addr" name it needs ptr-style xref resolution.
                type_param = m.group("type").strip()
                kind = "ptr" if type_param == "uintptr_t" else (
                    "func" if m.group("kind") == "Addr" else "ptr")
                entries.append({
                    "name": m.group("name"),
                    "kind": kind,
                    "type": type_param,
                    "rva": int(m.group("rva"), 16),
                    "file": str(path.relative_to(SRC_ROOT)),
                    "line": lineno,
                })

    # de-dupe identical (name, rva) pairs that may appear via header+cpp echoes
    seen = set()
    deduped = []
    for e in entries:
        key = (e["name"], e["rva"])
        if key in seen:
            continue
        seen.add(key)
        deduped.append(e)

    deduped.sort(key=lambda e: e["rva"])
    OUT.write_text(json.dumps(deduped, indent=2))
    print(f"{len(deduped)} RVA sites written to {OUT}")
    print(f"  func (RelocAddr): {sum(1 for e in deduped if e['kind']=='func')}")
    print(f"  ptr  (RelocPtr):  {sum(1 for e in deduped if e['kind']=='ptr')}")

if __name__ == "__main__":
    main()
