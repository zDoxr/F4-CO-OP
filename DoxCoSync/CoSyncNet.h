#pragma once
#include <string>
#include "LocalPlayerState.h"

namespace CoSyncNet
{
    void Init(bool host);
    void Shutdown();
    void Tick(double now);

    // MUST MATCH EXACTLY
    void SendLocalPlayerState(const LocalPlayerState& state);

    // Called by GNS_Session
    void OnReceive(const std::string& msg);
}
