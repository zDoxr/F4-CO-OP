#include "F4MP_Main.h"
#include "ConsoleLogger.h"
#include "GNS_Session.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "GNS_Core.h"
#include "CoSyncOverlay.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace f4mp
{
    F4MP_Main& F4MP_Main::Get()
    {
        static F4MP_Main inst;
        return inst;
    }

    // ------------------------------------------------------------
    // Init — basic bootstrap of GNS (SteamNetworkingSockets)
    // ------------------------------------------------------------
    void F4MP_Main::Init()
    {
        if (initialized.load())
        {
            LOG_INFO("[MAIN] Init() ignored — already initialized");
            return;
        }

        LOG_INFO("[MAIN] Init() — CoSync Multiplayer Core");

        if (!GNS_Init())
        {
            LOG_ERROR("[MAIN] GNS_Init FAILED");
            initialized.store(false);
            return;
        }

        initialized.store(true);
        LOG_INFO("[MAIN] CoSync Init OK");
    }

    // ------------------------------------------------------------
    // HOST — via Hamachi IP (25.x.x.x)
    // ------------------------------------------------------------
    void F4MP_Main::StartHosting(const std::string& ip)
    {
        if (!initialized.load())
            Init();

        if (!initialized.load())
        {
            LOG_ERROR("[MAIN] StartHosting FAILED — CoSync not initialized");
            return;
        }

        LOG_INFO("[MAIN] Hosting on IP: %s", ip.c_str());

        if (!GNS_Session::Get().StartHost(ip.c_str()))
        {
            LOG_ERROR("[MAIN] Host FAILED (GNS StartHost)");
            return;
        }

        if (!CoSyncTransport::InitAsHost())
        {
            LOG_ERROR("[MAIN] Host FAILED (Transport InitAsHost)");
            return;
        }

        CoSyncNet::SetMyName("Host");
        CoSyncNet::ScheduleInit(true);

        isServer.store(true);
        LOG_INFO("[MAIN] Host READY");
    }

    // ------------------------------------------------------------
    // JOIN — IP:PORT (from overlay)
    // ------------------------------------------------------------
    void F4MP_Main::StartJoining(const std::string& connectStr)
    {
        if (!initialized.load())
            Init();

        if (!initialized.load())
        {
            LOG_ERROR("[MAIN] StartJoining FAILED — CoSync not initialized");
            return;
        }

        LOG_INFO("[MAIN] Joining host at: %s", connectStr.c_str());

        if (!GNS_Session::Get().StartClient(connectStr))
        {
            LOG_ERROR("[MAIN] Client FAILED (GNS StartClient)");
            return;
        }

        if (!CoSyncTransport::InitAsClient(connectStr))
        {
            LOG_ERROR("[MAIN] Client FAILED (Transport InitAsClient)");
            return;
        }

        CoSyncNet::SetMyName("Client");
        CoSyncNet::ScheduleInit(false);

        isServer.store(false);
        LOG_INFO("[MAIN] Client connected OK");
    }

    // ------------------------------------------------------------
    // PER-FRAME TICK (GLOBAL)
    // ------------------------------------------------------------
    void F4MP_Main::Tick(double now)
    {
        if (!initialized.load())
            return;

        // ✅ This is the ONLY place CoSyncNet ticks
        CoSyncNet::Tick(now);
    }

    // ------------------------------------------------------------
    // Shutdown — clean networking
    // ------------------------------------------------------------
    void F4MP_Main::Shutdown()
    {
        LOG_INFO("[MAIN] Shutdown()");

        CoSyncNet::Shutdown();
        CoSyncTransport::Shutdown();

        if (initialized.load())
            GNS_Shutdown();

        initialized.store(false);
        isServer.store(false);
    }

} // namespace f4mp
