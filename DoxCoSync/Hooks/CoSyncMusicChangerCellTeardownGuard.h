#pragma once

// Guards the engine's per-cell "relevant actor for music" resolver at
// Fallout4.exe+0x4CAEA0 (fastcall(cellOrRefPtr) -> pointer-sized value,
// verified against Fallout4_disk.i64, build 1.11.221.0). Reached from
// TESObjectCELL's own destructor (confirmed via RTTI: the caller two frames
// up sets both vftable slots to TESObjectCELL::`vftable') as part of
// BGSPlayerMusicChanger's cell-teardown cleanup (the global that gets
// notified is qword_1431E2BF0, initialized elsewhere as
// &BGSPlayerMusicChanger::`vftable').
//
// 2026-08-12, caught live via a TTD trace recorded during a beta-test
// session (see Ultimate Reversing workstation notes): the process crashed
// with a READ access violation at Fallout4.exe+0x4CAF53 (0xB3 bytes into
// this function) -- `mov rdi, qword ptr [rax+50h]` with rax read from a
// global (qword_1430E0290) that was null at that exact moment. This is
// vanilla engine code, not something this project calls directly; it only
// runs when the engine itself unloads a cell. Whatever makes that global
// go null between cell-teardown notifications is not yet understood --
// this is a containment guard around the specific known fault, not a real
// fix, and the function may still misbehave in ways SEH doesn't cover if
// the underlying state corruption is worse than a single null read.

void InstallMusicChangerCellTeardownGuard();
void UninstallMusicChangerCellTeardownGuard();
