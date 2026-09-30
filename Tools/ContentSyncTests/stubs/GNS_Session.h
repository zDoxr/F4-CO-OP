#pragma once
#include "TestStubs.h"
class GNS_Session
{
public:
    static GNS_Session& Get();
    void SendText(const std::string& text, bool reliable = true);                       // client -> host
    void SendTextTo(HSteamNetConnection conn, const std::string& text, bool reliable = true); // host -> client
    void LeaveWithReason(int reason, const char* debug);
    void KickClient(HSteamNetConnection conn, int reason, const char* debug);
    bool IsClientConnected(HSteamNetConnection conn) const;
};
