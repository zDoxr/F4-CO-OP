#pragma once

// Guards a tiny shared engine leaf at Fallout4.exe+0x256B80 (fastcall(handle)
// -> float; no unwind info, no .pdata entry -- a true raw leaf with no own
// stack frame). Its entire body is:
//   rcx = [rcx+8]
//   if (rcx) goto <continuation, resolves further>
//   xmm0 = 0; return 0.0f
// It dereferences its own incoming rcx with zero validation. Confirmed via
// pdata ground-truth (round 13) to be tail-called from at least 3 distinct
// engine functions: LocomotionCalc, MovementHandle (both already guarded at
// their own entries), and a third, previously-unguarded caller at
// Fallout4.exe+0xCC8AD8 discovered chasing this exact crash live. Rounds
// 73-94 chased this same crash signature (Fallout4.exe+0x256B80, +0xCC934D)
// caller-by-caller without ever closing it for good -- hooking the shared
// leaf itself catches every caller, current and not-yet-discovered, at the
// one chokepoint they all funnel through.
//
// Crash signature seen live (rounds 73-94 and again round 14): incoming rcx
// holds Bethesda's "field never initialized" sentinel (top byte 0xFF, rest
// zero -- e.g. 0xFF000000000000A8), not a real pointer or null.

void InstallSharedHandleLeafGuard();
void UninstallSharedHandleLeafGuard();
