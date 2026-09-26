#pragma once

// Guards Fallout4.exe+0xDB8A60 (fastcall(actor, flag) -> void), a shared
// per-actor "process" entry point reached by the background worker thread
// (frames trace through 0xDA94C8 -> 0xDB8A60 -> ... -> KERNEL32/ntdll
// thread-start, confirmed via real-unwind stack walks since round 73).
// Round 77's CoSyncLocomotionCalcGuard (Fallout4.exe+0xCC8AB0) only covers
// ONE of at least two independent callers into the same underlying
// actor+0x58/+0x130 vtable-dereference crash -- round 86 found a SECOND
// call chain (Fallout4.exe+0xCEFB48 onward) that reaches the identical crash
// site without ever passing through 0xCC8AB0, so that guard never sees it.
// Both chains converge here, at 0xDB8A60, which (unlike the several
// compiler-outlined intermediate fragments below it -- 0xCC9322, 0xCC90E5,
// 0xCEFB48, 0xCF537A, 0xC63E4B, none of which use a real ABI calling
// convention and are therefore unsafe to hook directly) has a genuine,
// independent, ABI-respecting prologue (`mov rbp,rcx; movzx r14d,dl`),
// confirmed via pdata as its own function's true BeginAddress. Wraps the
// call in SEH, with the same per-actor-pointer circuit breaker as
// CoSyncLocomotionCalcGuard/CoSyncMovementHandleGuard (round 84's lesson:
// catching the fault isn't enough by itself if the real function still
// gets re-entered and re-executes its partial broken state on every visit).
// See [[project_cosync_crash_fixes]] round 86.

void InstallActorProcessGuard();
void UninstallActorProcessGuard();
