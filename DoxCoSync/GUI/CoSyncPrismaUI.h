#pragma once

// PrismaUI bridge: owns CoSync's PrismaUI API handle and its two views
// (Data\PrismaUI_F4\views\DoxCoSync\):
//   * multiplayer.html -- the Multiplayer menu (interactive panel)
//   * hud.html         -- teammates' health / AP / rads (passive widget)
//
// PrismaUI itself is a separate F4SE plugin (PrismaUI_F4.dll) and is required
// for CoSync's UI; without it every call here is a logged no-op.
//
// Threading contract:
//   * Init() runs on the game thread (kMessage_GameLoaded).
//   * Request*() / IsMultiplayerOpen() / ConsumeClassicMenuRequest() are safe
//     from ANY thread (WndProc, menu hooks, ...): they only touch atomics and
//     hand work to PrismaUI's verified game-thread dispatcher.
//   * Every PrismaUI view call (CreateView/Show/Focus/InteropCall/...) is made
//     from the game thread, and JS->C++ events are bound with
//     BindGameThreadUIEvent so their handlers also run on the game thread.
namespace CoSyncPrismaUI
{
    // Acquires the PrismaUI V12 interface. Does NOT create any view -- PrismaUI
    // refuses callback-bearing views until its window-thread dispatcher is
    // ready, so the view is created lazily on first open.
    bool Init();

    // True once Init() obtained the API.
    bool IsAvailable();

    // Opens the Multiplayer menu. fromMainMenu: opened from the main-menu
    // MULTIPLAYER row (CoSyncMenuInject owns hiding/restoring the vanilla list
    // and switching to the classic overlay). Any thread.
    void RequestOpenMultiplayer(bool fromMainMenu);

    // Developer hotkey (F10): open if closed, close if open. Any thread.
    void RequestToggleMultiplayer();

    // True from the moment an open is requested until the menu closes.
    // Any thread.
    bool IsMultiplayerOpen();

    // Closes the menu immediately if called on PrismaUI's game/window thread
    // (e.g. from WndProc on WM_CLOSE); otherwise queues the close. Any thread.
    void CloseMultiplayerNow();

    // Teammate HUD (health / AP / rads). Call every frame from the main-thread
    // tick; throttles itself and only pushes to the page when data changed.
    void TickHud();

    // Text chat box (views/DoxCoSync/chat.html): shown during a session,
    // takes keyboard focus while typing. Any thread.
    void RequestOpenChat();
    bool IsChatOpen();
}
