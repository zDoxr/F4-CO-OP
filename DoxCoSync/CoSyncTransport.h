#pragma once
#include <string>
#include <functional>

// Transport interface shared by host/client.
// This layer does NOT open sockets — GNS_Session does.
// Transport simply forwards messages to CoSyncNet.
namespace CoSyncTransport
{
    // Host attaches to existing GNS host socket.
    bool InitAsHost();

    // Client attaches to existing GNS client connection.
    bool InitAsClient(const std::string& connectStr);

    // Shutdown transport runtime (does NOT shut down GNS itself).
    void Shutdown();

    // Pump transport if needed (currently optional / no-op).
    void Tick(double now);

    // Send a text packet to the connected peer(s).
    void Send(const std::string& msg);

    // Called by GNS_Session whenever a raw text message arrives.
    void ForwardMessage(const std::string& msg);

    // CoSyncNet sets its receive callback here.
    void SetReceiveCallback(std::function<void(const std::string&)> fn);

    // Host returns "xx.xx.xx.xx:48000" connect string.
    std::string GetHostConnectString();
}
