#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include "CoSyncRequirementsLog.h"

namespace cosync
{

    enum class CoSyncNetState : int
    {
        Idle    = 0,
        Busy    = 1,
        Done    = 2,
        Failed  = 3,
    };

    class CoSync_Main
    {
    public:
        static CoSync_Main& Get();

        void Init();


        void StartHosting(const std::string& ip);


        void StartJoining(const std::string& connectStr);

        void Tick(double now);


        void Shutdown();

        bool IsInitialized() const { return initialized.load(); }
        bool IsServer() const { return isServer.load(); }
        CoSyncNetState GetNetState() const { return m_netState.load(); }


        void ResetNetState()
        {
            LOG_REQUIREMENT("cosync::CoSync_Main::ResetNetState", "Doesn't touch the game itself. Just moves the network state back to Idle if it's currently Failed or Done, and leaves Busy alone.");
            CoSyncNetState cur = m_netState.load();
            if (cur == CoSyncNetState::Failed || cur == CoSyncNetState::Done)
                m_netState.store(CoSyncNetState::Idle);
        }

    private:
        CoSync_Main() = default;

        std::atomic<bool>           initialized{ false };
        std::atomic<bool>           isServer{ false };
        std::atomic<CoSyncNetState> m_netState{ CoSyncNetState::Idle };


        std::mutex  m_argMutex;
        std::string m_pendingArg;
    };
}
