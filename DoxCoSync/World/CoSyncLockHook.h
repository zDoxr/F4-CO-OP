#pragma once

#include <string>

// Lock/unlock sync. Hooks ForceUnlock (sub_140563320) and ForceLock
// (sub_140563360) — the canonical engine entry points covering lockpick
// success, terminal hack, AI packages, and perk auto-unlock.
//
// Packet format: "LOCK|refFormID|locked"
//   locked=1 → object is now locked, locked=0 → now unlocked
//
// Receiver applies via the Papyrus ObjectReference.Lock binding
// (sub_141158640) which handles ExtraLock allocation cleanly.

void InstallLockHook();
void UninstallLockHook();

namespace CoSyncLockHook
{
    bool IsLock(const std::string& msg);
    void OnReceiveLock(const std::string& msg);
    void FlushPendingLockOps();
}
