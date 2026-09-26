#pragma once

// Game-window message hook (WndProc subclass). Replaces the non-rendering half
// of the retired DX11/ImGui hook:
//   * Enter / left-click -> PauseMenu DISCONNECT row activation
//   * F10                -> Multiplayer menu hotkey
//   * WM_CLOSE           -> release the PrismaUI menu's focus before the window
//                           goes away (PrismaUI crashed quitting while focused)
//
// No DirectX, no Present hook: installed from the main-thread tick, which runs
// on the thread that owns the game window.

// Game thread, every frame until it succeeds (cheap no-op afterwards).
void CoSyncWindowHook_EnsureInstalled();

// Restores the original window procedure. Safe if never installed.
void CoSyncWindowHook_Shutdown();
