#pragma once

#include <string>
#include <atomic>

namespace f4mp
{
    class F4MP_Main
    {
    public:
        static F4MP_Main& Get();

        // Called once when DLL loads (steam/network initialization)
        void Init();

        // Host a multiplayer session on given IP (Hamachi)
        void StartHosting(const std::string& ip);

        // Join a host using "IP:PORT"
        void StartJoining(const std::string& connectStr);

        // Shutdown all networking (called on plugin unload)
        void Shutdown();

        

        bool IsInitialized() const { return initialized.load(); }
        bool IsServer() const { return isServer.load(); }

    private:
        F4MP_Main() = default;

        std::atomic<bool> initialized{ false };
        std::atomic<bool> isServer{ false };
    };
}
