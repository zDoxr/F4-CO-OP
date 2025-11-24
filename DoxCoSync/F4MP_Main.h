#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <cstdio>
#include <windows.h>

#include "PluginApi.h"
#include "F4MP_Librg.h"       // your new Steam backend
#include "RuntimeVersion.h"
#include "Version.h"

namespace f4mp {

    // ---------------------------------------------------------
    // Logging replacement (no F4SE required)
    // ---------------------------------------------------------
    inline void LOG(const char* fmt, ...)
    {
        char buffer[1024];

        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        OutputDebugStringA(buffer);  // print to debugger
        OutputDebugStringA("\n");
    }

    class F4MP_Main
    {
    public:

        static F4MP_Main& Get()
        {
            static F4MP_Main instance;
            return instance;
        }

        // ---------------------------------------------------------
        // Startup / Shutdown
        // ---------------------------------------------------------
        static void Init()
        {
            auto& self = Get();

            if (self.initialized)
                return;

            LOG("[F4MP] Initializing main system...");

            self.isServer.store(false);

            // Create networking backend (client mode default)
            self.backend = std::make_unique<f4mp::librg::Librg>(false);

            self.initialized = true;
        }

        static void Shutdown()
        {
            auto& self = Get();
            if (!self.initialized)
                return;

            LOG("[F4MP] Shutting down main system...");

            if (self.backend)
                self.backend->Stop();

            self.backend.reset();
            self.initialized = false;
        }

        // ---------------------------------------------------------
        // Networking control
        // ---------------------------------------------------------
        void StartClient(const std::string& ip, int port)
        {
            if (!backend) return;

            LOG("[F4MP] Starting client connection to %s:%d", ip.c_str(), port);

            isServer.store(false);
            backend->Start(ip, port);
        }

        void StartServer(int port)
        {
            if (!backend) return;

            LOG("[F4MP] Starting dedicated server on port %d", port);

            isServer.store(true);
            backend->Start("", port);
        }

        // ---------------------------------------------------------
        // Tick(), called every frame
        // ---------------------------------------------------------
        static void Tick()
        {
            auto& self = Get();
            if (!self.initialized || !self.backend)
                return;

            self.backend->Tick();
        }

        // ---------------------------------------------------------
        // Accessors
        // ---------------------------------------------------------
        bool IsServer() const { return isServer.load(); }
        bool Connected() const { return backend ? backend->Connected() : false; }

        f4mp::librg::Librg* GetNetwork() { return backend.get(); }

    private:
        F4MP_Main() = default;
        ~F4MP_Main() = default;

        F4MP_Main(const F4MP_Main&) = delete;
        F4MP_Main& operator=(const F4MP_Main&) = delete;

        bool initialized = false;
        std::atomic<bool> isServer{ false };

        std::unique_ptr<f4mp::librg::Librg> backend;
    };

} // namespace f4mp
