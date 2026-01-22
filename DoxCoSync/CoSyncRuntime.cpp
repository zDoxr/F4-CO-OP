#include "CoSyncRuntime.h"

#include "ConsoleLogger.h"
#include "CoSyncWorld.h"
#include "CoSyncTransport.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncNet.h"
#include "CoSyncLocalPlayer.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

static double NowSeconds()
{
    static double s_freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
        }();

    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / s_freq;
}

void CoSyncRuntime_TickGameThread()
{
    const double now = NowSeconds();

    // Ensure init happens once world is ready
    if (!CoSyncNet::IsInitialized())
        CoSyncNet::PerformPendingInit();

    // Drain transport inbox on game thread (F4MP rule)
    if (CoSyncTransport::IsInitialized())
        CoSyncTransport::Tick(now);

    // Apply CREATE/UPDATE -> queue spawns -> run spawn tasks (game thread)
    g_CoSyncPlayerManager.Tick();

    // Host-side publish + NPC scheduler etc.
    if (CoSyncNet::IsInitialized())
        CoSyncNet::Tick(now);

    // Send local player position updates (clients and host)
    CoSyncLocalPlayer::Tick(now);
}