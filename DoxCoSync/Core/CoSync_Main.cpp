#include "CoSync_Main.h"
#include "ConsoleLogger.h"
#include "GNS_Session.h"
#include "CoSyncNet.h"
#include "CoSyncPapyrusHelper.h"

#include "CoSyncTransport.h"
#include "GNS_Core.h"
#include "CoSyncOverlay.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncDiscovery.h"
#include "CoSyncWorldTransfer.h"
#include "CoSyncProfiles.h"
#include "CoSyncProxyLooks.h"
#include "CoSyncCompanions.h"
#include "CoSyncItemMods.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace cosync
{
    CoSync_Main& CoSync_Main::Get()
    {
        LOG_REQUIREMENT("cosync::CoSync_Main::Get", "Doesn't touch the game — just returns the one shared instance, safe to call any time.");
        static CoSync_Main inst;
        return inst;
    }

    void CoSync_Main::Init()
    {
        LOG_REQUIREMENT("cosync::CoSync_Main::Init", "Safe to call more than once — does nothing after the first successful call. Sets up networking and the Papyrus helper at plugin startup.");
        if (initialized.load())
        {
            LOG_INFO("[MAIN] Already initialized — skipping");
            return;
        }

        LOG_INFO("[MAIN] Starting up CoSync Multiplayer");

        if (!GNS_Init())
        {
            LOG_ERROR("[MAIN] Networking failed to start");
            initialized.store(false);
            return;
        }

        initialized.store(true);
        LOG_INFO("[MAIN] Networking is up");


        LOG_INFO("[MAIN] Setting up the Papyrus helper...");
        CoSyncPapyrusHelper::Init();
        LOG_INFO("[MAIN] Papyrus helper setup done");
		LOG_DEBUG("[MAIN] Papyrus helper initialized");
    }

    void CoSync_Main::StartHosting(const std::string& ip)
    {
        LOG_REQUIREMENT("cosync::CoSync_Main::StartHosting", "Only starts if we're currently idle — does nothing otherwise. Expects a valid, bindable IP address, which gets used on a background thread.");

        CoSyncNetState expected = CoSyncNetState::Idle;
        if (!m_netState.compare_exchange_strong(expected, CoSyncNetState::Busy))
        {
            LOG_WARN("[MAIN] Not starting to host — something's already in progress (state=%d)",
                (int)m_netState.load());
            return;
        }

        { std::lock_guard<std::mutex> lk(m_argMutex); m_pendingArg = ip; }

        LOG_INFO("[MAIN] Queued hosting for IP: %s", ip.c_str());

        // Read on the caller's thread (UI), not the start-up thread.
        const std::string hostName = CoSyncNet::GetMyName();

        std::thread([this, hostName]()
        {
            LOG_REQUIREMENT("cosync::CoSync_Main::StartHosting::{lambda}", "Needs this object to stay alive for as long as this background thread runs. Also relies on Init() having already run.");
            std::string ip;
            { std::lock_guard<std::mutex> lk(m_argMutex); ip = m_pendingArg; }


            if (!initialized.load())
                Init();

            if (!initialized.load())
            {
                LOG_ERROR("[MAIN] Can't host — CoSync hasn't been initialized");
                m_netState.store(CoSyncNetState::Failed);
                return;
            }

            LOG_INFO("[MAIN] Hosting on IP: %s", ip.c_str());

            if (!GNS_Session::Get().StartHost(ip.c_str()))
            {
                LOG_ERROR("[MAIN] Hosting failed to start (networking layer)");
                m_netState.store(CoSyncNetState::Failed);
                return;
            }

            if (!CoSyncTransport::InitAsHost())
            {
                LOG_ERROR("[MAIN] Hosting failed to start (transport layer)");
                m_netState.store(CoSyncNetState::Failed);
                return;
            }

            CoSyncNet::ScheduleInit(true);

            // Make the session findable on the LAN. The player count is read
            // live from GNS (host + connected clients), never from the UI.
            CoSyncDiscovery::StartAdvertising(
                GNS_Session::Get().GetBoundPort(),
                hostName + "'s Game",
                hostName,
                CoSyncNet::GetBuildNumber(),
                []() { return 1 + GNS_Session::Get().GetConnectedClientCount(); });

            isServer.store(true);
            m_netState.store(CoSyncNetState::Done);
            LOG_INFO("[MAIN] Hosting is ready");
        }).detach();
    }

    void CoSync_Main::StartJoining(const std::string& connectStr)
    {
        LOG_REQUIREMENT("cosync::CoSync_Main::StartJoining", "Only starts if we're currently idle — does nothing otherwise. Expects a valid host address/port string, which gets used on a background thread.");

        CoSyncNetState expected = CoSyncNetState::Idle;
        if (!m_netState.compare_exchange_strong(expected, CoSyncNetState::Busy))
        {
            LOG_WARN("[MAIN] Not joining — something's already in progress (state=%d)",
                (int)m_netState.load());
            return;
        }

        { std::lock_guard<std::mutex> lk(m_argMutex); m_pendingArg = connectStr; }

        LOG_INFO("[MAIN] Queued joining: %s", connectStr.c_str());

        std::thread([this]()
        {
            LOG_REQUIREMENT("cosync::CoSync_Main::StartJoining::{lambda}", "Needs this object to stay alive for as long as this background thread runs. Also relies on Init() having already run.");
            std::string addr;
            { std::lock_guard<std::mutex> lk(m_argMutex); addr = m_pendingArg; }


            if (!initialized.load())
                Init();

            if (!initialized.load())
            {
                LOG_ERROR("[MAIN] Can't join — CoSync hasn't been initialized");
                m_netState.store(CoSyncNetState::Failed);
                return;
            }

            LOG_INFO("[MAIN] Joining host at: %s", addr.c_str());

            if (!GNS_Session::Get().StartClient(addr))
            {
                LOG_ERROR("[MAIN] Joining failed to start (networking layer)");
                m_netState.store(CoSyncNetState::Failed);
                return;
            }

            if (!CoSyncTransport::InitAsClient(addr))
            {
                LOG_ERROR("[MAIN] Joining failed to start (transport layer)");
                m_netState.store(CoSyncNetState::Failed);
                return;
            }

            CoSyncNet::ScheduleInit(false);

            isServer.store(false);
            m_netState.store(CoSyncNetState::Done);
            LOG_INFO("[MAIN] Connected to the host");
        }).detach();
    }

    void CoSync_Main::Tick(double now)
    {
        LOG_REQUIREMENT("cosync::CoSync_Main::Tick", "Does nothing unless Init() has already run. Expects an always-increasing time value, which it passes along to the networking tick.");
        if (!initialized.load())
            return;


        CoSyncNet::Tick(now);
    }

    void CoSync_Main::Shutdown()
    {
        LOG_REQUIREMENT("cosync::CoSync_Main::Shutdown", "Safe to call even if we were never initialized. Assumes networking and transport shutdown are themselves safe to call regardless of state.");
        LOG_INFO("[MAIN] Shutting down");

        CoSyncDiscovery::StopAdvertising();
        CoSyncProfiles::ClientUploadNow();   // client: last snapshot to the host
        CoSyncNet::Shutdown();
        CoSyncWorldTransfer::Reset();
        CoSyncProfiles::Reset();
        CoSyncProxyLooks::Reset();
        CoSyncCompanions::Reset();
        CoSyncItemMods::Reset();
        CoSyncTransport::Shutdown();

        if (initialized.load())
        {
            GNS_Session::Get().EndSession();   // stop its thread before the library goes away
            GNS_Shutdown();
        }

        initialized.store(false);
        isServer.store(false);
    }

}
