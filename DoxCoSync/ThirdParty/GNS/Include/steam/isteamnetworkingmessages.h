#ifndef ISTEAMNETWORKINGMESSAGES
#define ISTEAMNETWORKINGMESSAGES
#pragma once

#include "steamnetworkingtypes.h"
#include "steam_api_common.h"

class ISteamNetworkingMessages
{
public:

	virtual EResult SendMessageToUser( const SteamNetworkingIdentity &identityRemote, const void *pubData, uint32 cubData, int nSendFlags, int nRemoteChannel ) = 0;

	virtual int ReceiveMessagesOnChannel( int nLocalChannel, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) = 0;

	virtual bool AcceptSessionWithUser( const SteamNetworkingIdentity &identityRemote ) = 0;

	virtual bool CloseSessionWithUser( const SteamNetworkingIdentity &identityRemote ) = 0;

	virtual bool CloseChannelWithUser( const SteamNetworkingIdentity &identityRemote, int nLocalChannel ) = 0;

	virtual ESteamNetworkingConnectionState GetSessionConnectionInfo( const SteamNetworkingIdentity &identityRemote, SteamNetConnectionInfo_t *pConnectionInfo, SteamNetConnectionRealTimeStatus_t *pQuickStatus ) = 0;
};
#define STEAMNETWORKINGMESSAGES_INTERFACE_VERSION "SteamNetworkingMessages002"

#pragma pack( push, 1 )


struct SteamNetworkingMessagesSessionRequest_t
{
	enum { k_iCallback = k_iSteamNetworkingMessagesCallbacks + 1 };
	SteamNetworkingIdentity m_identityRemote;
};

struct SteamNetworkingMessagesSessionFailed_t
{
	enum { k_iCallback = k_iSteamNetworkingMessagesCallbacks + 2 };

	SteamNetConnectionInfo_t m_info;
};

#pragma pack(pop)

#ifdef STEAMNETWORKINGSOCKETS_STANDALONELIB

	static_assert( STEAMNETWORKINGMESSAGES_INTERFACE_VERSION[25] == '2', "Version mismatch" );

	STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingMessages *SteamNetworkingMessages_LibV2();
	inline ISteamNetworkingMessages *SteamNetworkingMessages_Lib() { return SteamNetworkingMessages_LibV2(); }


	STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingMessages *SteamGameServerNetworkingMessages_LibV2();
	inline ISteamNetworkingMessages *SteamGameServerNetworkingMessages_Lib() { return SteamGameServerNetworkingMessages_LibV2(); }

	#ifndef STEAMNETWORKINGSOCKETS_STEAMAPI
		inline ISteamNetworkingMessages *SteamNetworkingMessages() { return SteamNetworkingMessages_LibV2(); }
		inline ISteamNetworkingMessages *SteamGameServerNetworkingMessages() { return SteamGameServerNetworkingMessages_LibV2(); }
	#endif
#endif


#ifdef STEAMNETWORKINGSOCKETS_STEAMAPI


	STEAM_DEFINE_USER_INTERFACE_ACCESSOR( ISteamNetworkingMessages *, SteamNetworkingMessages_SteamAPI, STEAMNETWORKINGMESSAGES_INTERFACE_VERSION );
	STEAM_DEFINE_GAMESERVER_INTERFACE_ACCESSOR( ISteamNetworkingMessages *, SteamGameServerNetworkingMessages_SteamAPI, STEAMNETWORKINGMESSAGES_INTERFACE_VERSION );

	#ifndef STEAMNETWORKINGSOCKETS_STANDALONELIB
		inline ISteamNetworkingMessages *SteamNetworkingMessages() { return SteamNetworkingMessages_SteamAPI(); }
		inline ISteamNetworkingMessages *SteamGameServerNetworkingMessages() { return SteamGameServerNetworkingMessages_SteamAPI(); }
	#endif
#endif

#endif
