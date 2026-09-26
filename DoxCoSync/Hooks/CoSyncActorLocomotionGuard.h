#pragma once

// Guards Fallout4.exe+0xCC8AD8 (fastcall(actor, context) -> float; signature
// inferred by analogy to its sibling guards in this same neighborhood, not
// yet IDA-confirmed -- watch the heartbeat/log on the next live test).
// Confirmed via pdata ground-truth (round 15) to be a real, 872-byte
// function entry, and via two separate live crashes (rounds 14 and 15 of
// this campaign) to be the actual "every per-tick actor locomotion
// calculation" routine rounds 73-77 originally described and tried to hook
// -- CoSyncLocomotionCalcGuard.cpp's 0xCC88F0 and CoSyncMovementHandleGuard's
// 0x6C96E0 are both real function entries per pdata, but their heartbeats
// fire once-or-never per session, proving neither is actually the hot,
// per-frame routine the original bug reports describe. This one has crashed
// twice already on two DIFFERENT internal call sites:
//   - round 14: tail-call into the shared leaf at +0x256B80 (now also
//     guarded directly at that leaf, see CoSyncSharedHandleLeafGuard)
//   - round 15: a virtual call (`call [rax+8]`) through a garbage vtable
//     pointer, downstream at +0xCC934D via +0xCC9101 via +0xCC8DD7
// Rather than chase individual internal call sites one at a time, this
// hooks the function's own entry and wraps the whole call in SEH, catching
// any fault from any internal site -- current or not-yet-discovered -- the
// same approach already proven on this function's siblings.

void InstallActorLocomotionGuard();
void UninstallActorLocomotionGuard();
