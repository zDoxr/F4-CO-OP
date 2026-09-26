#pragma once

// Guards a tiny (0x46-byte) engine function at Fallout4.exe+0x4C4E00 that
// AddRefs a handle living at TESObjectREFR::animGraphHolder(+0x48)+0x20 with
// zero null-check on animGraphHolder itself. Bethesda's own background
// worker thread calls into this (via Fallout4.exe+0xC766D0, reached from a
// large per-reference dispatcher at +0xC75FD8) moments after any reference
// is placed in the world, to check whether it has an anim-event-sink
// registered. For an ordinary spawn this is safe because Load3D has long
// since populated animGraphHolder by the time this runs; for our
// PlaceAtMe-spawned remote-player proxy it fires before Load3D (driven by
// the engine's own async cell-attach/ProcessLists pipeline, never
// synchronous inside PlaceAtMe) has attached anything, so animGraphHolder
// is still null -- rdi=0, rbx=rdi+0x20=0x20, and the AddRef call dereferences
// address 0x20 -> STATUS_ACCESS_VIOLATION on that worker thread. Confirmed
// via direct disassembly (capstone) of the live 1.11.221 binary: crash
// registers RBX=RCX=RDI=0x20 match this exact null+0x20 arithmetic exactly.
// See [[project_cosync_crash_fixes]] round 73.

void InstallAnimGraphHandleGuard();
void UninstallAnimGraphHandleGuard();
