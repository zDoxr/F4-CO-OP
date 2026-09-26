#ifndef STEAMNETWORKINGCUSTOMSIGNALING
#define STEAMNETWORKINGCUSTOMSIGNALING
#pragma once

#include "steamnetworkingtypes.h"

class ISteamNetworkingSockets;

class ISteamNetworkingConnectionSignaling
{
public:

	virtual bool SendSignal( HSteamNetConnection hConn, const SteamNetConnectionInfo_t &info, const void *pMsg, int cbMsg ) = 0;

	virtual void Release() = 0;
};

class ISteamNetworkingSignalingRecvContext
{
public:

	virtual ISteamNetworkingConnectionSignaling *OnConnectRequest( HSteamNetConnection hConn, const SteamNetworkingIdentity &identityPeer, int nLocalVirtualPort ) = 0;

	virtual void SendRejectionSignal( const SteamNetworkingIdentity &identityPeer, const void *pMsg, int cbMsg ) = 0;
};

typedef ISteamNetworkingConnectionSignaling * (*FnSteamNetworkingSocketsCreateConnectionSignaling)( ISteamNetworkingSockets *pLocalInterface, const SteamNetworkingIdentity &identityPeer, int nLocalVirtualPort, int nRemoteVirtualPort );

#endif
