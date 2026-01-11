#pragma once

#include <string>
#include <atomic>

namespace f4mp
{
    class F4MP_Main
    {
    public:
        static F4MP_Main& Get();

        // ------------------------------------------------------------
        // Lifecycle
        // ------------------------------------------------------------

        // Called once when DLL loads (Steam / GNS bootstrap)
        void Init();

        // Host a multiplayer session on given IP (Hamachi)
        void StartHosting(const std::string& ip);

        // Join a host using "IP:PORT"
        void StartJoining(const std::string& connectStr);

        // Global per-frame tick (GAME THREAD ONLY)
        // This is the ONLY place CoSyncNet::Tick is driven.
        void Tick(double now);

        // Shutdown all networking (called on plugin unload)
        void Shutdown();

        // ------------------------------------------------------------
        // State
        // ------------------------------------------------------------
        bool IsInitialized() const { return initialized.load(); }
        bool IsServer() const { return isServer.load(); }

    private:
        F4MP_Main() = default;

        std::atomic<bool> initialized{ false };
        std::atomic<bool> isServer{ false };
    };
}
