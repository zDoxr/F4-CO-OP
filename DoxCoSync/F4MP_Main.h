#pragma once

#include <string>
#include <atomic>

namespace f4mp
{
    class F4MP_Main
    {
    public:
        static F4MP_Main& Get();

        // Core lifecycle (called by plugin)
        static void Init();
        static void Shutdown();
        static void GameTick(float dt);

        // Overlay-driven multiplayer controls
        void StartHosting();
        void StartJoining(const std::string& connectStr);

        // Query network status
        bool IsServer() const { return isServer.load(); }
        bool IsNetworkReady() const;   // Connected host <-> client

        // Incoming message handler (future use)
        void OnGNSMessage(const std::string& msg);

    private:
        F4MP_Main() = default;
        ~F4MP_Main() = default;

        F4MP_Main(const F4MP_Main&) = delete;
        F4MP_Main& operator=(const F4MP_Main&) = delete;

        std::atomic<bool> initialized{ false };
        std::atomic<bool> isServer{ false };
    };

} // namespace f4mp
