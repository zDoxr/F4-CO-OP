// HamachiUtil.cpp

#define WIN32_LEAN_AND_MEAN      // Trim down <Windows.h>
#define _WINSOCKAPI_             // Prevent <Windows.h> from including winsock.h

#include <winsock2.h>            // WinSock2 FIRST
#include <ws2tcpip.h>
#include <Windows.h>             // After winsock2.h
#include <iphlpapi.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Iphlpapi.lib")

#include "HamachiUtil.h"
#include <string>


#include <vector>

std::string HamachiUtil::GetHamachiIPv4()
{
    ULONG bufLen = 16000;
    std::vector<unsigned char> buffer(bufLen);

    IP_ADAPTER_ADDRESSES* addresses =
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

    // Query adapter list
    ULONG ret = GetAdaptersAddresses(
        AF_INET,
        GAA_FLAG_SKIP_DNS_SERVER |
        GAA_FLAG_SKIP_MULTICAST |
        GAA_FLAG_SKIP_ANYCAST,
        nullptr,
        addresses,
        &bufLen
    );

    if (ret != NO_ERROR)
        return "";

    for (auto* adapter = addresses; adapter != nullptr; adapter = adapter->Next)
    {
        // Convert friendly adapter name from wide-string
        std::wstring wname(adapter->FriendlyName);
        std::string  aname(wname.begin(), wname.end());

        // Hamachi adapter identification
        if (aname.find("Hamachi") == std::string::npos)
            continue;

        // Walk assigned IPv4 addresses
        for (auto* ua = adapter->FirstUnicastAddress; ua != nullptr; ua = ua->Next)
        {
            if (ua->Address.lpSockaddr == nullptr)
                continue;

            SOCKADDR_IN* ipv4 = reinterpret_cast<SOCKADDR_IN*>(ua->Address.lpSockaddr);

            if (ipv4->sin_family != AF_INET)
                continue;

            unsigned char* b = reinterpret_cast<unsigned char*>(&ipv4->sin_addr.S_un.S_addr);

            // Hamachi virtual IPv4 always starts with 25.x.x.x
            if (b[0] == 25)
            {
                char ipbuf[32];
                sprintf_s(ipbuf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                return std::string(ipbuf);
            }
        }
    }

    return "";
}
