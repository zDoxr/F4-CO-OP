#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>

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
            CoSyncNetState cur = m_netState.load();
            if (cur == CoSyncNetState::Failed || cur == CoSyncNetState::Done)
                m_netState.store(CoSyncNetState::Idle);
        }

    private:
        CoSync_Main() = default;
        // The static instance is destroyed during the CRT's exit teardown: a
        // still-joinable std::thread there would call std::terminate (the T2
        // crash class), so release the start-up thread without blocking.
        ~CoSync_Main() { if (m_startThread.joinable()) m_startThread.detach(); }

        // P10 (2026-09-26): host/join set-up runs on this thread. It used to be
        // detached, so Shutdown() could kill GNS while it was still binding
        // sockets / initialising the transport and discovery. Everything that
        // tears down or restarts joins it first (a few ms of socket set-up).
        void JoinStartThread()
        {
            if (m_startThread.joinable() && m_startThread.get_id() != std::this_thread::get_id())
                m_startThread.join();
        }
        std::thread m_startThread;

        std::atomic<bool>           initialized{ false };
        std::atomic<bool>           isServer{ false };
        std::atomic<CoSyncNetState> m_netState{ CoSyncNetState::Idle };


        std::mutex  m_argMutex;
        std::string m_pendingArg;
    };
}
