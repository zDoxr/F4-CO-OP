#include "CoSync_Main.h"
#include "ConsoleLogger.h"
#include "GNS_Session.h"
#include "CoSyncNet.h"
#include "CoSyncPapyrusHelper.h"

#include "CoSyncTransport.h"
#include "GNS_Core.h"
#include "CoSyncOverlay.h"
#include "CoSyncDiscovery.h"
#include "CoSyncWorldTransfer.h"
#include "CoSyncContentSync.h"
#include "CoSyncProfiles.h"
#include "CoSyncProxyLooks.h"
#include "CoSyncCompanions.h"
#include "CoSyncItemMods.h"
#include "CoSyncSteamLobby.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace cosync
{
    CoSync_Main& CoSync_Main::Get()
    {
        static CoSync_Main inst;
        return inst;
    }

    void CoSync_Main::Init()
    {
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

        JoinStartThread();
        m_startThread = std::thread([this, hostName]()
        {
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
            // A Steam-hosted session has no UDP game port -- friends find it
            // through its Steam lobby instead (CoSyncSteamLobby).
            if (!GNS_Session::Get().IsSteamBackend())
            CoSyncDiscovery::StartAdvertising(
                GNS_Session::Get().GetBoundPort(),
                hostName + "'s Game",
                hostName,
                CoSyncNet::GetBuildNumber(),
                []() { return 1 + GNS_Session::Get().GetConnectedClientCount(); });

            isServer.store(true);
            m_netState.store(CoSyncNetState::Done);
            LOG_INFO("[MAIN] Hosting is ready");
        });
    }

    void CoSync_Main::StartJoining(const std::string& connectStr)
    {

        CoSyncNetState expected = CoSyncNetState::Idle;
        if (!m_netState.compare_exchange_strong(expected, CoSyncNetState::Busy))
        {
            LOG_WARN("[MAIN] Not joining — something's already in progress (state=%d)",
                (int)m_netState.load());
            return;
        }

        { std::lock_guard<std::mutex> lk(m_argMutex); m_pendingArg = connectStr; }

        LOG_INFO("[MAIN] Queued joining: %s", connectStr.c_str());

        JoinStartThread();
        m_startThread = std::thread([this]()
        {
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
        });
    }

    void CoSync_Main::Tick(double now)
    {
        if (!initialized.load())
            return;


        CoSyncNet::Tick(now);
    }

    void CoSync_Main::Shutdown()
    {
        LOG_INFO("[MAIN] Shutting down");

        // P10: never tear down while host/join set-up is still running.
        JoinStartThread();

        CoSyncDiscovery::StopAdvertising();
        CoSyncSteamLobby::Leave();           // Steam lobby + rich presence
        CoSyncProfiles::ClientUploadNow();   // client: last snapshot to the host
        CoSyncNet::Shutdown();
        CoSyncWorldTransfer::Reset();
        CoSyncContentSync::OnSessionEnded();
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
