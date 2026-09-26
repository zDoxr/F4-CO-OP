#pragma once

struct F4SETaskInterface;

void CoSyncRuntime_TickGameThread();

// GAME THREAD. True while the named menu (e.g. "LooksMenu", "LoadingMenu") is
// open. Uses the same UI singleton + IsMenuOpen call as the loading-menu poll.
bool CoSyncRuntime_IsMenuOpen(const char* menuName);

// UPDATE 2026-06-19: CoSyncRuntime_TickGameThread() was being called directly
// from HookedPresent (DX11Hook.cpp), which is the DX11 render thread, not
// FO4's main game thread -- despite the function's own name. This raced
// against the engine's own main-thread state (confirmed: a connect-time
// engine event-sink array gets walked by both threads at once, see
// project_cosync_crash_fixes.md round 33/37) and reproduced byte-identically
// under Application Verifier page-heap, ruling out memory corruption as the
// cause -- a genuine cross-thread race, not a bad write. Call this once at
// plugin load to move the tick onto F4SE's main-thread-permanent task queue
// (TaskInterface::AddTaskPermanent, drained from the hooked
// MessageQueueProcessTask -- the same main-thread message pump F4SE's other
// per-tick plugin work already relies on) instead of the render thread.
void CoSyncRuntime_InstallMainThreadTick(const F4SETaskInterface* taskInterface);
