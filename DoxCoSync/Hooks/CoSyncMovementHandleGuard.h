#pragma once

// Guards a tiny engine accessor at Fallout4.exe+0x6C9730 (tail-calls into a
// true leaf at +0x256B80) that every per-tick actor speed/locomotion
// calculation routes through. It unconditionally reads a pointer-sized
// field at actor+0x418 and dereferences [field+0xA8+8] with zero validity
// check. For a freshly PlaceAtMe-spawned actor that field can still hold
// Bethesda's own "never initialized" sentinel (top byte 0xFF, rest zero --
// the same convention already observed on TESObjectREFR+0x7C as
// 0xFF000000) instead of a real pointer or null, causing
// STATUS_ACCESS_VIOLATION inside engine code we don't otherwise touch.
// Confirmed via capstone disassembly of the live 1.11.221 binary (round 75):
// crash RCX=0xFF000000000000A8 == sentinel(0xFF00000000000000) + 0xA8,
// exactly matching this function's own `[rbx+0x418] + 0xA8` computation.
// See [[project_cosync_crash_fixes]] round 75.

void InstallMovementHandleGuard();
void UninstallMovementHandleGuard();
