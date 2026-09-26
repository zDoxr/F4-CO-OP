#include "PapyrusVM.h"
#include "Relocation.h"
#include "PapyrusEvents.h"


// UPDATE 2026-06-20 (round 44): the address below (0x01132200), despite the
// extensive IDA-based reasoning in the old comment kept below, is ALSO
// wrong -- confirmed via direct pefile+Capstone disassembly AND a PE
// .pdata/RUNTIME_FUNCTION lookup against the actual installed Fallout4.exe
// (1.11.221), independent of IDA entirely (this session has repeatedly
// confirmed the IDA database unreliable -- see CoSyncEngineResumeHook.cpp,
// CoSyncDoorHook.cpp, GameObjects.cpp's PlaceAtMe_Native). The
// RUNTIME_FUNCTION entry covering 0x01132200 has BeginAddress=0x01132130 --
// 0x01132200 is +0xD0 (208 bytes) INTO that function. Worse: the bytes at
// 0x01132200 (`add byte ptr [rdi],cl; sub byte ptr [rsp+0x40],dh;
// add rsp,0x50; pop r14; pop rdi; pop rsi; ret; int3`) are this function's
// own EPILOGUE, not an entry -- meaning every CallFunctionNoWait call this
// whole time has executed a WRITE through whatever garbage RDI held in the
// caller's context (`add byte ptr [rdi], cl`) and then immediately
// returned, having done none of the real work and having potentially
// corrupted memory at an arbitrary address. This function is used by
// SafeCallFunctionNoWaitStart and, transitively, by CallFunctionNoWait<T>
// across well over a dozen files (every quest/companion/health/equip/
// outfit/inventory/loot sync path that touches Papyrus) -- this is a strong
// candidate for the actual root cause of the "blast radius" memory
// corruption chased since round 19, far more directly than anything found
// before round 44's live-binary sweep. Fixed to the real entry
// (`mov [rsp+0x10],rbx; mov [rsp+0x18],rbp; push rsi; push rdi; push r14;
// sub rsp,0x50; mov rdi,[rsp+0x98]; mov rsi,rcx`).
//
// Old comment (now superseded, kept for history): "Found via IDA against
// Fallout4.exe 1.11.221: the registered ScriptObject native at 0x01132340
// is wrapped in a NativeFunction2 template and is only ever invoked
// indirectly through that wrapper's own dispatch (zero direct `call` xrefs
// anywhere in the binary) -- calling it raw with F4SE's assumed 5-arg
// convention bypasses the wrapper's argument marshaling and crashes with
// RIP=0/AV EXEC. 0x01132200 is the real simple internal sibling: same shape
// as the confirmed-good SendCustomEvent_Internal (uses the same
// qword_1430E0208 VM-singleton global, the same sub_14167BDF0 BSFixedString
// packing helper for the eventName arg, the same AddRef call on the
// identifier), with a genuine 5-param signature matching this typedef
// exactly."
// ============================================================================
// RVA CORRECTED 2026-08-04 (round 44's fix was itself wrong): 0x011324E0.
//
// CRASH THIS FIXES: client log 20:01:25.982 -- four "[QuestSync] Started
// quest=0x000229E5" lines followed immediately by
//   [CoSync-VEH] Known bad-function-pointer crash (RIP=..., AV type=EXEC)
//   pre-recovery regs: RBP=0x00000000000229E5   <-- the quest formID itself
//
// WHY 0x01132130 WAS WRONG: it is a NativeFunction dispatch thunk for an
// unrelated 2-float-argument native. Its 6-param body does float math (xmm6),
// unpacks two args via sub_141107C90(edx=0/1), then executes
// `mov rax,[rsi+50h]; call rax`. Called with OUR arguments, rsi is our
// VirtualMachine*, so it reads [vm+0x50] as a function pointer and CALLS it --
// exactly the EXEC AV above. Every Papyrus form-argument call in this codebase
// went through it.
//
// HOW 0x011324E0 WAS DERIVED (registration table, not guesswork): the Papyrus
// string "CallFunctionNoWait" (0x025C03A8) is used at 0x01136BA6, which calls
// the NativeFunction ctor (0x020F9DE0) and then stores the implementation
// pointer with `lea rax,[rel 11324E0h]; mov [rbx+50h],rax`. IDA confirms
// 0x011324E0 is a function entry taking FIVE params -- matching this file's
// typedef exactly -- whose body builds a
// std::_Func_impl<lambda, BSTThreadScrapSTLAllocator<void>, bool,
// BSScrapArray<BSScript::Variable>&> functor and AddRefs the identifier: a
// deferred Papyrus call, which is precisely what CallFunctionNoWait is.
//
// This is f4se 0.7.8's ORIGINAL declared address. Round 44 rejected it partly
// because it has "zero direct call xrefs" -- but that is true of EVERY Papyrus
// native (they are only ever reached through the NativeFunction wrapper's
// stored +0x50 pointer), so it proves nothing. The same is true of the
// confirmed-good Debug.Notification impl at 0x0111C510.
//
// Registration-derived sibling, same technique, for reference:
//   Utility.CallGlobalFunctionNoWait -> 0x0118BB90 (also f4se's original).
RelocAddr<_CallFunctionNoWait> CallFunctionNoWait_Internal(0x01132830);

// RVA CORRECTED round 102: 0x01134E00 is NOT a function entry -- independently
// re-verified via IDA (ida_funcs.get_func(ea).start_ea == ea returns False at
// this address; the containing function's real bounds are
// 0x1134D80-0x1134E3B). 0x1134E00 is 0x80 bytes into that function, AFTER the
// BSFixedString event-name packing (sub_14167BDF0) and the v12=a3/AddRef setup
// -- a direct call there skips that setup and reads this function's own
// uninitialized stack locals (v11/v12) instead of the real caller-supplied
// arguments. This is the exact same function CoSyncModEventHook.cpp's
// kSendCustomEvent_RVA already correctly targets at 0x1134D80 (round 93) --
// that fix was never propagated to this second declaration of the same
// function, even though every CoSyncModEventHook.cpp::ReFireOne() call (firing
// a peer's relayed custom mod event locally) goes through this exact global.
RelocAddr<_SendCustomEvent>    SendCustomEvent_Internal(0x01135150);
