#pragma once

#include "common/ITypes.h"   // Provides UInt8, UInt32
#include <cstddef>

// Forward declare message
struct SteamP2PMessage;

class INetwork
{
public:
    virtual ~INetwork() = default;

    // ----------------------------
    // Lifecycle
    // ----------------------------
    virtual bool Init() = 0;
    virtual void Shutdown() = 0;
    virtual void Tick() = 0;

    // ----------------------------
    // Host/Client start
    // ----------------------------
    virtual bool StartHost(int port) = 0;
    virtual bool StartClient(const char* remoteID, int port) = 0;

    // ----------------------------
    // Send API
    // ----------------------------
    virtual bool Send(const void* data, size_t size) = 0;
    virtual bool SendToAll(const UInt8* data, UInt32 size) = 0;
    virtual bool SendToHost(const UInt8* data, UInt32 size) = 0;

    // ----------------------------
    // Receive API
    // ----------------------------
    virtual bool PopMessage(SteamP2PMessage& outMsg) = 0;

    // ----------------------------
    // Status API
    // ----------------------------
    virtual bool IsConnected() const = 0;
};
