#pragma once

// Guards the engine's per-reference locomotion/speed calculator at
// Fallout4.exe+0xCC8AB0 (fastcall(actor, context) -> float). Rounds 75/76
// found one specific internal crash in this function (an unguarded read at
// actor+0x418, reached via a tail-called sub-function) and patched that one
// call site -- the very next live test crashed again, in a DIFFERENT
// internal call site of the SAME function (a virtual call through
// actor+0x58's own vtable, then through a second pointer at +0x130 of
// whatever that resolves to), for the same underlying reason: a freshly
// PlaceAtMe-spawned actor's movement/physics-controller sub-object chain
// (rooted at actor+0x58) isn't fully populated by Load3D yet, and this
// function unconditionally walks several different pointers in it
// depending on actor state/flags. Rather than keep patching individual
// offsets one crash at a time, this hooks the function's own true entry
// (confirmed via pdata: real prologue starts at 0xCC8AB0, not the 0xCC8AD8
// mid-function point earlier rounds anchored on) and wraps the whole call
// in SEH, returning 0.0f (its own "nothing to add" value, matching every
// internal early-out already observed) on ANY fault from ANY internal
// call site, current or not-yet-discovered. See
// [[project_cosync_crash_fixes]] round 77.

void InstallLocomotionCalcGuard();
void UninstallLocomotionCalcGuard();
