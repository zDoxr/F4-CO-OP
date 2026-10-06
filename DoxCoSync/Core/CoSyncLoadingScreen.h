#pragma once

// The vanilla loading menu's background (LoadingMenuBG.dds, embedded).
void InjectLoadingScreenTexture();

// 0.5.1 (Bug #9): THE CoSync loading screen -- one place decides when it is
// up and what it says; every load goes through it.
//
// Shown while:
//   * the engine's LoadingMenu exists -- every engine load: New Game,
//     Continue, a save load, doors, interior <-> exterior, load elevators,
//     fast travel, a respawn that changes cell. Detected by patching
//     LoadingMenu's own vtable (per-frame slot 0x10 + destructor slot 0,
//     ??_7LoadingMenu@@6B@ = 0x25B0458, IDA 1.11.240) -- those run on the
//     main thread during a load, where CoSync's tick (the F4SE task queue in
//     the world-update job) does not: the 2026-10-05 logs never saw the
//     LoadingMenu through the tick's poll.
//   * CoSync runs a load of its own: the host's world downloading / loading
//     on a joining client, a respawn, the lobby's New Game starting.
// The view is DoxCoSync/loading.html (the design from
// DoxCoSync/LoadingScreen/Loading screen.html), drawn by PrismaUI above
// everything. Visual only: the engine's own loading is untouched.
namespace CoSyncLoading
{
    // Plugin load (vtable patch, like the MainMenu hooks).
    void InstallLoadingMenuHooks();

    enum class Reason : int { Respawn = 0, NewGame = 1, Count };
    // CoSync-driven loads. The reason expires by itself after `seconds`
    // (a load that never happened cannot pin the screen).
    void Begin(Reason reason, double seconds);
    void End(Reason reason);

    // CoSync tick and the MainMenu frame: re-evaluates and updates the view.
    void Tick();

    bool IsShowing();
}
