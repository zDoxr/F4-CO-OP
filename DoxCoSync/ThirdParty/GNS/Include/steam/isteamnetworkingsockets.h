#ifndef ISTEAMNETWORKINGSOCKETS
#define ISTEAMNETWORKINGSOCKETS
#pragma once

#include "steamnetworkingtypes.h"
#include "steam_api_common.h"

struct SteamNetAuthenticationStatus_t;
struct SteamNetworkingFakeIPResult_t;
class ISteamNetworkingConnectionSignaling;
class ISteamNetworkingSignalingRecvContext;
class ISteamNetworkingFakeUDPPort;

class ISteamNetworkingSockets
{
public:

	virtual HSteamListenSocket CreateListenSocketIP( const SteamNetworkingIPAddr &localAddress, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) = 0;

	virtual HSteamNetConnection ConnectByIPAddress( const SteamNetworkingIPAddr &address, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) = 0;

	virtual HSteamListenSocket CreateListenSocketP2P( int nLocalVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) = 0;

	virtual HSteamNetConnection ConnectP2P( const SteamNetworkingIdentity &identityRemote, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) = 0;

	virtual EResult AcceptConnection( HSteamNetConnection hConn ) = 0;

	virtual bool CloseConnection( HSteamNetConnection hPeer, int nReason, const char *pszDebug, bool bEnableLinger ) = 0;

	virtual bool CloseListenSocket( HSteamListenSocket hSocket ) = 0;

	virtual bool SetConnectionUserData( HSteamNetConnection hPeer, int64 nUserData ) = 0;

	virtual int64 GetConnectionUserData( HSteamNetConnection hPeer ) = 0;


	virtual void SetConnectionName( HSteamNetConnection hPeer, const char *pszName ) = 0;


	virtual bool GetConnectionName( HSteamNetConnection hPeer, char *pszName, int nMaxLen ) = 0;

	virtual EResult SendMessageToConnection( HSteamNetConnection hConn, const void *pData, uint32 cbData, int nSendFlags, int64 *pOutMessageNumber ) = 0;

	virtual void SendMessages( int nMessages, SteamNetworkingMessage_t *const *pMessages, int64 *pOutMessageNumberOrResult ) = 0;

	virtual EResult FlushMessagesOnConnection( HSteamNetConnection hConn ) = 0;

	virtual int ReceiveMessagesOnConnection( HSteamNetConnection hConn, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) = 0;


	virtual bool GetConnectionInfo( HSteamNetConnection hConn, SteamNetConnectionInfo_t *pInfo ) = 0;

	virtual EResult GetConnectionRealTimeStatus( HSteamNetConnection hConn, SteamNetConnectionRealTimeStatus_t *pStatus,
		int nLanes, SteamNetConnectionRealTimeLaneStatus_t *pLanes ) = 0;

	virtual int GetDetailedConnectionStatus( HSteamNetConnection hConn, char *pszBuf, int cbBuf ) = 0;

	virtual bool GetListenSocketAddress( HSteamListenSocket hSocket, SteamNetworkingIPAddr *address ) = 0;

	virtual bool CreateSocketPair( HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback, const SteamNetworkingIdentity *pIdentity1, const SteamNetworkingIdentity *pIdentity2 ) = 0;

	virtual EResult ConfigureConnectionLanes( HSteamNetConnection hConn, int nNumLanes, const int *pLanePriorities, const uint16 *pLaneWeights ) = 0;

	virtual bool GetIdentity( SteamNetworkingIdentity *pIdentity ) = 0;

	virtual ESteamNetworkingAvailability InitAuthentication() = 0;

	virtual ESteamNetworkingAvailability GetAuthenticationStatus( SteamNetAuthenticationStatus_t *pDetails ) = 0;

	virtual HSteamNetPollGroup CreatePollGroup() = 0;

	virtual bool DestroyPollGroup( HSteamNetPollGroup hPollGroup ) = 0;

	virtual bool SetConnectionPollGroup( HSteamNetConnection hConn, HSteamNetPollGroup hPollGroup ) = 0;

	virtual int ReceiveMessagesOnPollGroup( HSteamNetPollGroup hPollGroup, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) = 0;

	virtual bool ReceivedRelayAuthTicket( const void *pvTicket, int cbTicket, SteamDatagramRelayAuthTicket *pOutParsedTicket ) = 0;

	virtual int FindRelayAuthTicketForServer( const SteamNetworkingIdentity &identityGameServer, int nRemoteVirtualPort, SteamDatagramRelayAuthTicket *pOutParsedTicket ) = 0;

	virtual HSteamNetConnection ConnectToHostedDedicatedServer( const SteamNetworkingIdentity &identityTarget, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) = 0;

	virtual uint16 GetHostedDedicatedServerPort() = 0;

	virtual SteamNetworkingPOPID GetHostedDedicatedServerPOPID() = 0;

	virtual EResult GetHostedDedicatedServerAddress( SteamDatagramHostedAddress *pRouting ) = 0;

	virtual HSteamListenSocket CreateHostedDedicatedServerListenSocket( int nLocalVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) = 0;

	virtual EResult GetGameCoordinatorServerLogin( SteamDatagramGameCoordinatorServerLogin *pLoginInfo, int *pcbSignedBlob, void *pBlob ) = 0;

	virtual HSteamNetConnection ConnectP2PCustomSignaling( ISteamNetworkingConnectionSignaling *pSignaling, const SteamNetworkingIdentity *pPeerIdentity, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) = 0;

	virtual bool ReceivedP2PCustomSignal( const void *pMsg, int cbMsg, ISteamNetworkingSignalingRecvContext *pContext ) = 0;

	virtual bool GetCertificateRequest( int *pcbBlob, void *pBlob, SteamNetworkingErrMsg &errMsg ) = 0;

	virtual bool SetCertificate( const void *pCertificate, int cbCertificate, SteamNetworkingErrMsg &errMsg ) = 0;

	virtual void ResetIdentity( const SteamNetworkingIdentity *pIdentity ) = 0;

	virtual void RunCallbacks() = 0;

	virtual bool BeginAsyncRequestFakeIP( int nNumPorts ) = 0;

	virtual void GetFakeIP( int idxFirstPort, SteamNetworkingFakeIPResult_t *pInfo ) = 0;

	virtual HSteamListenSocket CreateListenSocketP2PFakeIP( int idxFakePort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) = 0;

	virtual EResult GetRemoteFakeIPForConnection( HSteamNetConnection hConn, SteamNetworkingIPAddr *pOutAddr ) = 0;

	virtual ISteamNetworkingFakeUDPPort *CreateFakeUDPPort( int idxFakeServerPort ) = 0;

protected:
	~ISteamNetworkingSockets();
};
#define STEAMNETWORKINGSOCKETS_INTERFACE_VERSION "SteamNetworkingSockets012"

#ifdef STEAMNETWORKINGSOCKETS_STANDALONELIB

	static_assert( STEAMNETWORKINGSOCKETS_INTERFACE_VERSION[24] == '2', "Version mismatch" );
	STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingSockets *SteamNetworkingSockets_LibV12();
	inline ISteamNetworkingSockets *SteamNetworkingSockets_Lib() { return SteamNetworkingSockets_LibV12(); }

	STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingSockets *SteamGameServerNetworkingSockets_LibV12();
	inline ISteamNetworkingSockets *SteamGameServerNetworkingSockets_Lib() { return SteamGameServerNetworkingSockets_LibV12(); }

	#ifndef STEAMNETWORKINGSOCKETS_STEAMAPI
		inline ISteamNetworkingSockets *SteamNetworkingSockets() { return SteamNetworkingSockets_LibV12(); }
		inline ISteamNetworkingSockets *SteamGameServerNetworkingSockets() { return SteamGameServerNetworkingSockets_LibV12(); }
	#endif
#endif


#ifdef STEAMNETWORKINGSOCKETS_STEAMAPI
	STEAM_DEFINE_USER_INTERFACE_ACCESSOR( ISteamNetworkingSockets *, SteamNetworkingSockets_SteamAPI, STEAMNETWORKINGSOCKETS_INTERFACE_VERSION );
	STEAM_DEFINE_GAMESERVER_INTERFACE_ACCESSOR( ISteamNetworkingSockets *, SteamGameServerNetworkingSockets_SteamAPI, STEAMNETWORKINGSOCKETS_INTERFACE_VERSION );

	#ifndef STEAMNETWORKINGSOCKETS_STANDALONELIB
		inline ISteamNetworkingSockets *SteamNetworkingSockets() { return SteamNetworkingSockets_SteamAPI(); }
		inline ISteamNetworkingSockets *SteamGameServerNetworkingSockets() { return SteamGameServerNetworkingSockets_SteamAPI(); }
	#endif
#endif


#if defined( VALVE_CALLBACK_PACK_SMALL )
#pragma pack( push, 4 )
#elif defined( VALVE_CALLBACK_PACK_LARGE )
#pragma pack( push, 8 )
#else
#error "Must define VALVE_CALLBACK_PACK_SMALL or VALVE_CALLBACK_PACK_LARGE"
#endif

struct SteamNetConnectionStatusChangedCallback_t
{
	enum { k_iCallback = k_iSteamNetworkingSocketsCallbacks + 1 };


	HSteamNetConnection m_hConn;


	SteamNetConnectionInfo_t m_info;


	ESteamNetworkingConnectionState m_eOldState;
};

struct SteamNetAuthenticationStatus_t
{
	enum { k_iCallback = k_iSteamNetworkingSocketsCallbacks + 2 };


	ESteamNetworkingAvailability m_eAvail;

	char m_debugMsg[ 256 ];
};

#pragma pack( pop )

#endif
