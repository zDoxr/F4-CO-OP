#include "F4MP_Main.h"
#include "ConsoleLogger.h"
#include "GNS_Session.h"
#include "CoSyncNet.h"

namespace f4mp
{
    F4MP_Main& F4MP_Main::Get()
    {
        static F4MP_Main inst;
        return inst;
    }

    // ------------------------------------------------------------
    // Init() — called by Plugin after GameLoaded
    // ------------------------------------------------------------
    void F4MP_Main::Init()
    {
        auto& self = Get();

        if (self.initialized.load())
            return;

        LOG_INFO("Main::Init() - Overlay-driven multiplayer");

        // Nothing to auto-start anymore.
        // Overlay UI will call StartHosting() or StartJoining().

        self.initialized.store(true);
    }

    // ------------------------------------------------------------
    // HOST START (called from Overlay)
    // ------------------------------------------------------------
    void F4MP_Main::StartHosting()
    {
        LOG_INFO("[MAIN] Starting HOST session...");

        if (!GNS_Session::Get().StartHost())
        {
            LOG_ERROR("[MAIN] HOST start FAILED");
            return;
        }

        // 🔥 initialize state sync
        CoSyncNet::Init(true);

        isServer.store(true);
    }

    // ------------------------------------------------------------
    // CLIENT START (called from Overlay)
    // ------------------------------------------------------------
    void F4MP_Main::StartJoining(const std::string& connectStr)
    {
        LOG_INFO("[MAIN] Starting CLIENT session with connect string: %s",
            connectStr.c_str());

        if (!GNS_Session::Get().StartClient(connectStr))
        {
            LOG_ERROR("[MAIN] CLIENT join FAILED");
            return;
        }

        // 🔥 initialize state sync
        CoSyncNet::Init(false);

        isServer.store(false);
    }

    // ------------------------------------------------------------
    // Shutdown
    // ------------------------------------------------------------
    void F4MP_Main::Shutdown()
    {
        auto& self = Get();

        LOG_INFO("[MAIN] Shutdown");

        CoSyncNet::Shutdown();
        self.isServer.store(false);
        self.initialized.store(false);
    }

    // ------------------------------------------------------------
    // GameTick — called every frame by TickHook
    // ------------------------------------------------------------
    void F4MP_Main::GameTick(float dt)
    {
        (void)dt;

        // Pump GNS messages
        GNS_Session::Get().Tick();

        // Networking logic inside CoSyncNet
        CoSyncNet::Tick(0.0);
    }

} // namespace f4mp
