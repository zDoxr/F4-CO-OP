# FO4 RVA Updater

Prep tool for the 2026-08-19 Fallout 4 update. Finds which of DoxCoSync's ~49
hardcoded game-exe RVAs moved, without touching any source file.

## How it works

1. `build_registry.py` -- greps the DoxCoSync source tree for every
   `RelocAddr<>`/`RelocPtr<>` declaration and writes `registry.json`
   (name, file, line, current RVA, func-vs-ptr kind).
2. `capture_signatures.py` -- for each site, disassembles the CURRENT
   (pre-update) `Fallout4.exe` around that RVA and builds a wildcarded byte
   signature (call/jmp targets, RIP-relative displacements, and movabs
   imm64s are masked out so the signature survives relinking). Writes
   `signatures.json`. Already run against the 1.11.221 baseline.
3. `rescan_after_update.py <path-to-updated-Fallout4.exe>` -- **run this
   tomorrow, after the update installs.** Re-finds every signature in the
   new exe and writes `rva_diff_report.txt` / `.json`: old RVA -> new RVA
   per site, plus a status:
   - `OK` / `UNCHANGED` -- confidently resolved, safe to hand-apply
   - `AMBIGUOUS` -- pattern matched more than once, needs manual pick
   - `NOT_FOUND` -- the function's code actually changed, needs a fresh IDA look
   - `SKIPPED` -- already had no usable baseline pattern (see below)

Nothing here edits `DoxCoSync` source files. The report is meant to be
applied by hand, one hex literal at a time, per [[feedback-preserve-uniqueness-when-replacing-synthetic-ids]] and the "report only" call made when this was built (2026-08-18) -- an automated blind rewrite risks touching a
lookalike hex literal that's actually a formID/flag/mask, not an address.

## Files

- `Fallout4_1.11.221_baseline.exe` -- backup of the pre-update exe (the diff anchor; the live Steam install will be overwritten by the update).
- `registry.json` -- the 49 tracked sites.
- `signatures.json` -- captured baseline signatures.
- `rva_diff_report.*` -- output of the last `rescan_after_update.py` run. The
  copy currently in this folder is a **round-trip sanity check** (baseline
  rescanned against itself, confirms the tool works) -- rerun after the real
  update to get the actual diff.

## Known gaps as of 2026-08-18 (3 sites need manual attention either way)

- `g_UI_IsMenuOpen_Fn` / `s_IsMenuOpen` / `UI_IsMenuOpen` (all resolve to the
  IsMenuOpen family) -- 3 byte-identical matches in the exe even at baseline.
  These are almost certainly trivial same-bytes wrapper functions the
  compiler emitted more than once (nothing this tool can fix -- there's no
  unique signature to find, since the bytes really are identical). Needs
  manual disambiguation (e.g. a probe hook per Mr. Pie's tiered method) if
  any of the 3 ever needs re-deriving.
- `g_realSetAnimVarBool` (0x00818D60) -- doesn't decode as a valid function
  start at all. This is very likely still the OLD, already-known-wrong
  mid-function address described in
  [[reference-fo4-anim-pipeline-rvas]] ("old 0x818D60/80 were mid-function =
  the SetInt hang") -- `g_realSetAnimVarFloat` was fixed to 0x818DA0 but
  `g_realSetAnimVarInt` (0x00818D80) and this one were apparently never
  updated to the DA0/C0/E0 trio. Worth fixing independent of tomorrow's
  update.
- `g_player` (0x032D2260) and `NiRTTI_BSLightingShaderProperty` (0x03E47310)
  -- no direct RIP-relative code reference found anywhere in `.text`.
  Neighboring offsets (0x2248/0x2250/0x2258/0x2268/0x2270 for the g_player
  struct) ARE referenced, just not this exact one -- worth double checking
  this RVA is actually right, independent of tomorrow's update.
