#pragma once

#include <cstdint>

// Hardcoded-RVA integrity scanner.
//
// Every raw address this mod calls is only valid for one Fallout4.exe build.
// When the game updates they all shift, usually by different amounts per code
// region, and the failure mode is silent: an address that used to be a
// function start becomes mid-function code or int3 padding, and the mod either
// crashes somewhere unrelated or -- worse -- quietly does the wrong thing.
// That has already happened here twice (an "UpdateDownwardPass" that was
// really NiAVObject::IsEqual, and a MoveTo that pointed at an inventory
// functor and silently no-op'd cross-cell moves for months).
//
// This checks, at load, that each hardcoded RVA still points at the bytes it
// pointed at when the signature table was harvested. Signatures are
// wildcarded across rip-relative displacements and rel32 branch targets, so
// they survive code motion and identify the SAME function at its new address.
//
// Reports only -- it does not repatch anything. The addresses are constexpr
// spread across the codebase, so the useful output is a list saying "this
// constant should now be that value", which a human applies. Auto-repair would
// need every use site routed through a registry first.
//
// OFF by default. Enable with:
//     [Diagnostics]
//     bScanAddresses=1
// in Data\F4SE\Plugins\CoSync.ini. Costs nothing when disabled, and on a
// matching build costs one memcmp per entry. Only a MISMATCH triggers a scan
// of .text, so the expensive path only runs when something is already wrong.
namespace CoSyncAddrScan
{
    // Reads the ini gate; safe to call every load. moduleBase is Fallout4.exe's
    // base address (the same value passed to the other Init functions).
    void Run(uintptr_t moduleBase) noexcept;

    // Ini state, cached. Exposed so callers can skip building arguments for a
    // disabled scan.
    bool IsEnabled() noexcept;
}
