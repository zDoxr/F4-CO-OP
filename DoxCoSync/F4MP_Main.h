#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <windows.h>

#include "PluginApi.h"
#include "F4MP_Librg.h"       // your Steam backend
#include "RuntimeVersion.h"
#include "Version.h"

namespace f4mp {

    // ---------------------------------------------------------
    // Debug logging (to Visual Studio Output window)
    // ---------------------------------------------------------
    inline void LOG(const char* fmt, ...)
    {
        char buffer[1024];

        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        OutputDebugStringA(buffer);
        OutputDebugStringA("\n");
    }

    class F4MP_Main
    {
    public:
        // Singleton access
        static F4MP_Main& Get()
        {
            static F4MP_Main instance;
            return instance;
        }

        // -----------------------------------------------------
        // Called from F4SE when the game is ready
        // -----------------------------------------------------
        static void Init()
        {
            auto& self = Get();
            if (self.initialized.load())
                return;

            LOG("[F4MP] Init()");

            // client mode by default (false = client, true = server)
            self.isServer.store(false);

            // Create networking backend
            self.backend = std::make_unique<f4mp::librg::Librg>(false);

            // Start background tick thread
            self.running.store(true);
            self.tickThread = std::thread(&F4MP_Main::TickLoop, &self);

            self.initialized.store(true);
        }

        static void Shutdown()
        {
            auto& self = Get();
            if (!self.initialized.load())
                return;

            LOG("[F4MP] Shutdown()");

            self.running.store(false);

            if (self.tickThread.joinable())
                self.tickThread.join();

            if (self.backend)
                self.backend->Stop();

            self.backend.reset();
            self.initialized.store(false);
        }

        // Optional manual tick (if we ever call it from elsewhere)
        static void Tick()
        {
            auto& self = Get();
            if (!self.initialized.load() || !self.backend)
                return;

            self.backend->Tick();
        }

        // -----------------------------------------------------
        // Networking control API (for later use)
        // -----------------------------------------------------
        void StartClient(const std::string& ip, int port)
        {
            if (!backend)
                return;

            LOG("[F4MP] StartClient %s:%d", ip.c_str(), port);

            isServer.store(false);
            backend->Start(ip, port);
        }

        void StartServer(int port)
        {
            if (!backend)
                return;

            LOG("[F4MP] StartServer port %d", port);

            isServer.store(true);
            backend->Start("", port);
        }

        bool IsServer() const { return isServer.load(); }
        bool Connected() const { return backend ? backend->Connected() : false; }

        f4mp::librg::Librg* GetNetwork() { return backend.get(); }

    private:
        F4MP_Main() = default;
        ~F4MP_Main()
        {
            // Safety net if game shuts down without explicit Shutdown()
            running.store(false);
            if (tickThread.joinable())
                tickThread.join();
        }

        F4MP_Main(const F4MP_Main&) = delete;
        F4MP_Main& operator=(const F4MP_Main&) = delete;

        // -----------------------------------------------------
        // Background tick loop
        // -----------------------------------------------------
        void TickLoop()
        {
            LOG("[F4MP] TickLoop started");

            while (running.load())
            {
                if (backend)
                {
                    backend->Tick();
                }

                // ~60 Hz. You can tweak this (e.g. 10–30ms)
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }

            LOG("[F4MP] TickLoop stopped");
        }

        std::atomic<bool> initialized{ false };
        std::atomic<bool> isServer{ false };
        std::atomic<bool> running{ false };

        std::unique_ptr<f4mp::librg::Librg> backend;
        std::thread tickThread;
    };

} // namespace f4mp
