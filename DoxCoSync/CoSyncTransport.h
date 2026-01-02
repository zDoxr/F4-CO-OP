#pragma once

#include <string>
#include <functional>

namespace CoSyncTransport
{
    bool InitAsHost();
    bool InitAsClient(const std::string& connectStr);
    void Shutdown();

    void Tick(double now);

    void Send(const std::string& msg);

    void ForwardMessage(const std::string& msg);
    void SetReceiveCallback(std::function<void(const std::string&)> fn);

    std::string GetHostConnectString();
}
