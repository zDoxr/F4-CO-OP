#pragma once

class GFxMovieView;

// Patches MainMenu::vftable[InitMainList] (vtable index 0x15) to call our
// injection right after vanilla populates entryList. Call once, as early as
// possible (F4SEPlugin_Load) -- this is a static binary patch, no UI/menu
// state required.
void CoSyncMenuInject_InstallHooks();

// Injects the MULTIPLAYER row into the vanilla MainMenu.swf entryList at
// runtime (no SWF patching) and wires up its click/select handler.
// Call once after g_ui is valid (kMessage_GameLoaded).
void CoSyncMenuInject_Init();

// Attempts the same MULTIPLAYER row injection directly against a
// GFxMovieView, e.g. one handed to us via the F4SE Scaleform plugin
// registration callback (which fires for every menu's movie, including
// MainMenu, well before g_ui's menuTable/MenuOpenCloseEvent machinery is
// usable). Safe to call for any menu's view -- if it isn't MainMenu (no
// root.Menu_mc.MainPanel_mc.List_mc.entryList), it just logs and returns.
void CoSyncMenuInject_OnScaleformView(GFxMovieView* view);

// Called from HookedWndProc on WM_KEYDOWN / VK_RETURN while the PauseMenu is
// open. Reads the currently-selected list entry directly from GFx (safe on the
// main thread) and fires the disconnect if index 99 is highlighted.
void CoSyncMenuInject_PauseMenuKeyActivate();

// Multiplayer hotkey (F10), any thread. On the main menu the request is
// handed to the MainMenu per-frame poll, which opens the menu exactly like the
// MULTIPLAYER row (vanilla list hidden AND frozen). In-game it toggles the
// PrismaUI menu directly.
void CoSyncMenuInject_OnMultiplayerHotkey();

