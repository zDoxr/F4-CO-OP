#pragma once

// Guards the engine's per-actor audio scale/attenuation resolver at
// Fallout4.exe+0xE5B780 (fastcall(subsystem, Actor*) -> float, verified
// against Fallout4_disk.i64, build 1.11.221.0).
//
// SISTER BUG TO CoSyncMusicChangerCellTeardownGuard -- read that header first.
// Both faults come from the SAME teardown: sub_140C31370 writes both
// subsystem globals (0xC314D8 -> qword_1430EEF50, this one; 0xC31675 ->
// qword_1430E0290, the music-changer one). The 2026-08-12 guard covered the
// music-changer global; this covers the neighbour that killed us anyway.
//
// 2026-08-11 TTD trace (Traces\Fallout401.run, recorded during a connected
// p1-p2 beta session, DoxCoSync.dll built 22:42 that evening):
//   * t=6D9D7C  READ AV at Fallout4+0x4CAF53, qword_1430E0290 == null.
//               CAUGHT by Detour_MusicChangerResolveFn's SEH -- process
//               survived. (Visible in the trace's own call stack as
//               DoxCoSync!CallOriginalSafe+0x130.) The existing guard works.
//   * t=6DA0AA  READ AV at Fallout4+0xE5B7B1 -- `mov rcx,[rcx+0A0h]` with
//               rcx read from qword_1430EEF50, which was ALSO null.
//               NOT guarded -> unhandled -> NtTerminateProcess, exit code
//               0xC0000005. This is that crash.
// The faulting instruction is 0x31 bytes into the function; its caller
// (sub_140CABA20 + 0x6E) loads the global into rcx and passes it straight in
// without a null check. This is vanilla engine code -- CoSync never calls it
// directly; it only runs when the engine itself tears the subsystem down,
// which lines up with the long-standing "interior <-> exterior hard crash,
// only while connected" report.
//
// RETURN VALUE (this is the part that matters -- do not "simplify" it to 0):
// Hex-Rays on the disk IDB shows the function's own default is 1.0f:
//     v8 = (__m128)0x3F800000u;            // 1.0f
//     ...
//     return v8 * (dword_142F40A4C / dword_142F40A48);   // both == 1.0f
// so the natural no-op result is 1.0f. It only returns 0.0f from a degenerate
// branch (`if (c1 <= 0.0) return 0;`) that cannot fire while c1 == 1.0f. All
// the arithmetic is single-precision (movss/mulss/divss/maxss) against a
// 0.66f clamp, i.e. this is a multiplicative attenuation/scale factor -- and
// its one traced caller does `mulss xmm7, xmm0` on the result. Returning 0.0f
// here would therefore silently zero whatever it scales instead of passing
// through neutral. Return 1.0f.
//
// Every path in the function dereferences the first argument (+0x40, +0x48,
// +0xA0, +0xD0), so when it is null the function cannot do anything
// meaningful at all -- substituting its own default is the only available
// containment. Like the music-changer guard this is containment around a
// known fault, NOT a root-cause fix: why these globals go null while callers
// are still live is still unexplained (see sub_140C31370).
//
// Hook target sanity (this codebase has been burned by bad RVAs -- rounds 19,
// 24, 129, 132): 0xE5B780 is a REAL function entry per .pdata
// (0xE5B780-0xE5B954, 468 bytes) with a clean decompile, NOT a mid-function
// splice. Note FalloutDumperOutput's functions.json disagrees (claims size
// 122 and invents sub-functions at 0xE5B7FA/0xE5B819/0xE5B8C0/0xE5B916) --
// the dumper is wrong here, the disk IDB is authoritative.

void InstallActorAudioScaleGuard();
void UninstallActorAudioScaleGuard();
