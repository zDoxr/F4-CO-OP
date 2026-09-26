#ifndef STEAMNETWORKINGTYPES
#define STEAMNETWORKINGTYPES
#pragma once

#include <string.h>
#include <stdint.h>
#include "steamtypes.h"
#include "steamclientpublic.h"

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#define STEAMNETWORKINGSOCKETS_OPENSOURCE
#endif
#define STEAMNETWORKINGSOCKETS_STANDALONELIB

#ifdef NN_NINTENDO_SDK
	#define STEAMNETWORKINGSOCKETS_STATIC_LINK
#endif
#if defined( STEAMNETWORKINGSOCKETS_STATIC_LINK )
	#define STEAMNETWORKINGSOCKETS_INTERFACE extern "C"
#elif defined( STEAMNETWORKINGSOCKETS_FOREXPORT )
	#if defined( _WIN32 ) || defined( __ORBIS__ ) || defined( __PROSPERO__ )
		#define STEAMNETWORKINGSOCKETS_INTERFACE extern "C" __declspec( dllexport )
	#else
		#define STEAMNETWORKINGSOCKETS_INTERFACE extern "C" __attribute__((visibility("default")))
	#endif
#else
	#ifdef _WIN32
		#define STEAMNETWORKINGSOCKETS_INTERFACE extern "C" __declspec( dllimport )
	#else
		#define STEAMNETWORKINGSOCKETS_INTERFACE extern "C"
	#endif
#endif

#if defined( VALVE_CALLBACK_PACK_SMALL )
#pragma pack( push, 4 )
#elif defined( VALVE_CALLBACK_PACK_LARGE )
#pragma pack( push, 8 )
#else
#error "Must define VALVE_CALLBACK_PACK_SMALL or VALVE_CALLBACK_PACK_LARGE"
#endif

struct SteamDatagramRelayAuthTicket;
struct SteamDatagramHostedAddress;
struct SteamDatagramGameCoordinatorServerLogin;
struct SteamNetConnectionStatusChangedCallback_t;
struct SteamNetAuthenticationStatus_t;
struct SteamRelayNetworkStatus_t;
struct SteamNetworkingMessagesSessionRequest_t;
struct SteamNetworkingMessagesSessionFailed_t;
struct SteamNetworkingFakeIPResult_t;

typedef void (*FnSteamNetConnectionStatusChanged)( SteamNetConnectionStatusChangedCallback_t * );
typedef void (*FnSteamNetAuthenticationStatusChanged)( SteamNetAuthenticationStatus_t * );
typedef void (*FnSteamRelayNetworkStatusChanged)(SteamRelayNetworkStatus_t *);
typedef void (*FnSteamNetworkingMessagesSessionRequest)(SteamNetworkingMessagesSessionRequest_t *);
typedef void (*FnSteamNetworkingMessagesSessionFailed)(SteamNetworkingMessagesSessionFailed_t *);
typedef void (*FnSteamNetworkingFakeIPResult)(SteamNetworkingFakeIPResult_t *);


typedef uint32 HSteamNetConnection;
const HSteamNetConnection k_HSteamNetConnection_Invalid = 0;

typedef uint32 HSteamListenSocket;
const HSteamListenSocket k_HSteamListenSocket_Invalid = 0;

typedef uint32 HSteamNetPollGroup;
const HSteamNetPollGroup k_HSteamNetPollGroup_Invalid = 0;


const int k_cchMaxSteamNetworkingErrMsg = 1024;

typedef char SteamNetworkingErrMsg[ k_cchMaxSteamNetworkingErrMsg ];

typedef uint32 SteamNetworkingPOPID;

typedef int64 SteamNetworkingMicroseconds;


enum ESteamNetworkingAvailability
{

	k_ESteamNetworkingAvailability_CannotTry = -102,
	k_ESteamNetworkingAvailability_Failed = -101,
	k_ESteamNetworkingAvailability_Previously = -100,

	k_ESteamNetworkingAvailability_Retrying = -10,


	k_ESteamNetworkingAvailability_NeverTried = 1,
	k_ESteamNetworkingAvailability_Waiting = 2,
	k_ESteamNetworkingAvailability_Attempting = 3,

	k_ESteamNetworkingAvailability_Current = 100,


	k_ESteamNetworkingAvailability_Unknown = 0,
	k_ESteamNetworkingAvailability__Force32bit = 0x7fffffff,
};

enum ESteamNetworkingIdentityType
{

	k_ESteamNetworkingIdentityType_Invalid = 0,

	k_ESteamNetworkingIdentityType_SteamID = 16,
	k_ESteamNetworkingIdentityType_XboxPairwiseID = 17,
	k_ESteamNetworkingIdentityType_SonyPSN = 18,

	k_ESteamNetworkingIdentityType_IPAddress = 1,

	k_ESteamNetworkingIdentityType_GenericString = 2,
	k_ESteamNetworkingIdentityType_GenericBytes = 3,

	k_ESteamNetworkingIdentityType_UnknownType = 4,


	k_ESteamNetworkingIdentityType__Force32bit = 0x7fffffff,
};

enum ESteamNetworkingFakeIPType
{
	k_ESteamNetworkingFakeIPType_Invalid,
	k_ESteamNetworkingFakeIPType_NotFake,
	k_ESteamNetworkingFakeIPType_GlobalIPv4,
	k_ESteamNetworkingFakeIPType_LocalIPv4,

	k_ESteamNetworkingFakeIPType__Force32Bit = 0x7fffffff
};

#pragma pack(push,1)

struct SteamNetworkingIPAddr
{
	void Clear();
	bool IsIPv6AllZeros() const;
	void SetIPv6( const uint8 *ipv6, uint16 nPort );
	void SetIPv4( uint32 nIP, uint16 nPort );
	bool IsIPv4() const;
	uint32 GetIPv4() const;
	void SetIPv6LocalHost( uint16 nPort = 0);
	bool IsLocalHost() const;

	enum { k_cchMaxString = 48 };

	inline void ToString( char *buf, size_t cbBuf, bool bWithPort ) const;

	inline bool ParseString( const char *pszStr );


	struct IPv4MappedAddress {
		uint64 m_8zeros;
		uint16 m_0000;
		uint16 m_ffff;
		uint8 m_ip[ 4 ];
	};

	union
	{
		uint8 m_ipv6[ 16 ];
		IPv4MappedAddress m_ipv4;
	};
	uint16 m_port;


	bool operator==(const SteamNetworkingIPAddr &x ) const;

	ESteamNetworkingFakeIPType GetFakeIPType() const;


	bool IsFakeIP() const { return GetFakeIPType() > k_ESteamNetworkingFakeIPType_NotFake; }
};

struct SteamNetworkingIdentity
{

	ESteamNetworkingIdentityType m_eType;

	void Clear();
	bool IsInvalid() const;

	void SetSteamID( CSteamID steamID );
	CSteamID GetSteamID() const;
	void SetSteamID64( uint64 steamID );
	uint64 GetSteamID64() const;

	bool SetXboxPairwiseID( const char *pszString );
	const char *GetXboxPairwiseID() const;

	void SetPSNID( uint64 id );
	uint64 GetPSNID() const;

	void SetIPAddr( const SteamNetworkingIPAddr &addr );
	const SteamNetworkingIPAddr *GetIPAddr() const;
	void SetIPv4Addr( uint32 nIPv4, uint16 nPort );
	uint32 GetIPv4() const;

	ESteamNetworkingFakeIPType GetFakeIPType() const;
	bool IsFakeIP() const { return GetFakeIPType() > k_ESteamNetworkingFakeIPType_NotFake; }

	void SetLocalHost();
	bool IsLocalHost() const;

	bool SetGenericString( const char *pszString );
	const char *GetGenericString() const;

	bool SetGenericBytes( const void *data, size_t cbLen );
	const uint8 *GetGenericBytes( int &cbLen ) const;


	bool operator==(const SteamNetworkingIdentity &x ) const;

	void ToString( char *buf, size_t cbBuf ) const;

	bool ParseString( const char *pszStr );


	enum {
		k_cchMaxString = 128,
		k_cchMaxGenericString = 32,
		k_cchMaxXboxPairwiseID = 33,
		k_cbMaxGenericBytes = 32,
	};

	int m_cbSize;
	union {
		uint64 m_steamID64;
		uint64 m_PSNID;
		char m_szGenericString[ k_cchMaxGenericString ];
		char m_szXboxPairwiseID[ k_cchMaxXboxPairwiseID ];
		uint8 m_genericBytes[ k_cbMaxGenericBytes ];
		char m_szUnknownRawString[ k_cchMaxString ];
		SteamNetworkingIPAddr m_ip;
		uint32 m_reserved[ 32 ];
	};
};
#pragma pack(pop)

enum ESteamNetworkingConnectionState
{

	k_ESteamNetworkingConnectionState_None = 0,

	k_ESteamNetworkingConnectionState_Connecting = 1,

	k_ESteamNetworkingConnectionState_FindingRoute = 2,

	k_ESteamNetworkingConnectionState_Connected = 3,

	k_ESteamNetworkingConnectionState_ClosedByPeer = 4,

	k_ESteamNetworkingConnectionState_ProblemDetectedLocally = 5,

	k_ESteamNetworkingConnectionState_FinWait = -1,

	k_ESteamNetworkingConnectionState_Linger = -2,


	k_ESteamNetworkingConnectionState_Dead = -3,

	k_ESteamNetworkingConnectionState__Force32Bit = 0x7fffffff
};

enum ESteamNetConnectionEnd
{

	k_ESteamNetConnectionEnd_Invalid = 0,

	k_ESteamNetConnectionEnd_App_Min = 1000,
		k_ESteamNetConnectionEnd_App_Generic = k_ESteamNetConnectionEnd_App_Min,

	k_ESteamNetConnectionEnd_App_Max = 1999,

	k_ESteamNetConnectionEnd_AppException_Min = 2000,
		k_ESteamNetConnectionEnd_AppException_Generic = k_ESteamNetConnectionEnd_AppException_Min,

	k_ESteamNetConnectionEnd_AppException_Max = 2999,

	k_ESteamNetConnectionEnd_Local_Min = 3000,


		k_ESteamNetConnectionEnd_Local_OfflineMode = 3001,

		k_ESteamNetConnectionEnd_Local_ManyRelayConnectivity = 3002,

		k_ESteamNetConnectionEnd_Local_HostedServerPrimaryRelay = 3003,

		k_ESteamNetConnectionEnd_Local_NetworkConfig = 3004,

		k_ESteamNetConnectionEnd_Local_Rights = 3005,

		k_ESteamNetConnectionEnd_Local_P2P_ICE_NoPublicAddresses = 3006,

	k_ESteamNetConnectionEnd_Local_Max = 3999,

	k_ESteamNetConnectionEnd_Remote_Min = 4000,

		k_ESteamNetConnectionEnd_Remote_Timeout = 4001,

		k_ESteamNetConnectionEnd_Remote_BadCrypt = 4002,

		k_ESteamNetConnectionEnd_Remote_BadCert = 4003,

		k_ESteamNetConnectionEnd_Remote_BadProtocolVersion = 4006,

		k_ESteamNetConnectionEnd_Remote_P2P_ICE_NoPublicAddresses = 4007,

	k_ESteamNetConnectionEnd_Remote_Max = 4999,


	k_ESteamNetConnectionEnd_Misc_Min = 5000,

		k_ESteamNetConnectionEnd_Misc_Generic = 5001,


		k_ESteamNetConnectionEnd_Misc_InternalError = 5002,

		k_ESteamNetConnectionEnd_Misc_Timeout = 5003,

		k_ESteamNetConnectionEnd_Misc_SteamConnectivity = 5005,

		k_ESteamNetConnectionEnd_Misc_NoRelaySessionsToClient = 5006,

		k_ESteamNetConnectionEnd_Misc_P2P_Rendezvous = 5008,

		k_ESteamNetConnectionEnd_Misc_P2P_NAT_Firewall = 5009,

		k_ESteamNetConnectionEnd_Misc_PeerSentNoConnection = 5010,

	k_ESteamNetConnectionEnd_Misc_Max = 5999,

	k_ESteamNetConnectionEnd__Force32Bit = 0x7fffffff
};

const int k_cchSteamNetworkingMaxConnectionCloseReason = 128;

const int k_cchSteamNetworkingMaxConnectionDescription = 128;


const int k_cchSteamNetworkingMaxConnectionAppName = 32;

const int k_nSteamNetworkConnectionInfoFlags_Unauthenticated = 1;
const int k_nSteamNetworkConnectionInfoFlags_Unencrypted = 2;
const int k_nSteamNetworkConnectionInfoFlags_LoopbackBuffers = 4;
const int k_nSteamNetworkConnectionInfoFlags_Fast = 8;
const int k_nSteamNetworkConnectionInfoFlags_Relayed = 16;
const int k_nSteamNetworkConnectionInfoFlags_DualWifi = 32;


struct SteamNetConnectionInfo_t
{


	SteamNetworkingIdentity m_identityRemote;


	int64 m_nUserData;


	HSteamListenSocket m_hListenSocket;

	SteamNetworkingIPAddr m_addrRemote;
	uint16 m__pad1;


	SteamNetworkingPOPID m_idPOPRemote;

	SteamNetworkingPOPID m_idPOPRelay;


	ESteamNetworkingConnectionState m_eState;

	int m_eEndReason;

	char m_szEndDebug[ k_cchSteamNetworkingMaxConnectionCloseReason ];

	char m_szConnectionDescription[ k_cchSteamNetworkingMaxConnectionDescription ];


	int m_nFlags;


	uint32 reserved[63];
};

struct SteamNetConnectionRealTimeStatus_t
{


	ESteamNetworkingConnectionState m_eState;


	int m_nPing;

	float m_flConnectionQualityLocal;


	float m_flConnectionQualityRemote;


	float m_flOutPacketsPerSec;
	float m_flOutBytesPerSec;
	float m_flInPacketsPerSec;
	float m_flInBytesPerSec;

	int m_nSendRateBytesPerSecond;

	int m_cbPendingUnreliable;
	int m_cbPendingReliable;

	int m_cbSentUnackedReliable;

	SteamNetworkingMicroseconds m_usecQueueTime;

	int32 m_usecMaxJitter;


	uint32 reserved[15];
};


struct SteamNetConnectionRealTimeLaneStatus_t
{


	int m_cbPendingUnreliable;
	int m_cbPendingReliable;
	int m_cbSentUnackedReliable;
	int _reservePad1;

	SteamNetworkingMicroseconds m_usecQueueTime;


	uint32 reserved[10];
};

#pragma pack( pop )

const int k_cbMaxSteamNetworkingSocketsMessageSizeSend = 512 * 1024;


struct SteamNetworkingMessage_t
{


	void *m_pData;


	int m_cbSize;

	HSteamNetConnection m_conn;

	SteamNetworkingIdentity m_identityPeer;

	int64 m_nConnUserData;

	SteamNetworkingMicroseconds m_usecTimeReceived;

	int64 m_nMessageNumber;

	void (*m_pfnFreeData)( SteamNetworkingMessage_t *pMsg );

	void (*m_pfnRelease)( SteamNetworkingMessage_t *pMsg );

	int m_nChannel;

	int m_nFlags;

	int64 m_nUserData;

	uint16 m_idxLane;
	uint16 _pad1__;

	inline void Release();


#ifndef API_GEN
	inline uint32 GetSize() const { return m_cbSize; }
	inline const void *GetData() const { return m_pData; }
	inline int GetChannel() const { return m_nChannel; }
	inline HSteamNetConnection GetConnection() const { return m_conn; }
	inline int64 GetConnectionUserData() const { return m_nConnUserData; }
	inline SteamNetworkingMicroseconds GetTimeReceived() const { return m_usecTimeReceived; }
	inline int64 GetMessageNumber() const { return m_nMessageNumber; }
#endif
protected:

	inline ~SteamNetworkingMessage_t() {}
};

const int k_nSteamNetworkingSend_Unreliable = 0;

const int k_nSteamNetworkingSend_NoNagle = 1;

const int k_nSteamNetworkingSend_UnreliableNoNagle = k_nSteamNetworkingSend_Unreliable|k_nSteamNetworkingSend_NoNagle;

const int k_nSteamNetworkingSend_NoDelay = 4;

const int k_nSteamNetworkingSend_UnreliableNoDelay = k_nSteamNetworkingSend_Unreliable|k_nSteamNetworkingSend_NoDelay|k_nSteamNetworkingSend_NoNagle;

const int k_nSteamNetworkingSend_Reliable = 8;

const int k_nSteamNetworkingSend_ReliableNoNagle = k_nSteamNetworkingSend_Reliable|k_nSteamNetworkingSend_NoNagle;

const int k_nSteamNetworkingSend_UseCurrentThread = 16;

const int k_nSteamNetworkingSend_AutoRestartBrokenSession = 32;

struct SteamNetworkPingLocation_t
{
	uint8 m_data[ 512 ];
};

const int k_cchMaxSteamNetworkingPingLocationString = 1024;


const int k_nSteamNetworkingPing_Failed = -1;
const int k_nSteamNetworkingPing_Unknown = -2;

enum ESteamNetworkingConfigScope
{

	k_ESteamNetworkingConfig_Global = 1,

	k_ESteamNetworkingConfig_SocketsInterface = 2,

	k_ESteamNetworkingConfig_ListenSocket = 3,


	k_ESteamNetworkingConfig_Connection = 4,

	k_ESteamNetworkingConfigScope__Force32Bit = 0x7fffffff
};


enum ESteamNetworkingConfigDataType
{
	k_ESteamNetworkingConfig_Int32 = 1,
	k_ESteamNetworkingConfig_Int64 = 2,
	k_ESteamNetworkingConfig_Float = 3,
	k_ESteamNetworkingConfig_String = 4,
	k_ESteamNetworkingConfig_Ptr = 5,

	k_ESteamNetworkingConfigDataType__Force32Bit = 0x7fffffff
};


enum ESteamNetworkingConfigValue
{
	k_ESteamNetworkingConfig_Invalid = 0,

	k_ESteamNetworkingConfig_TimeoutInitial = 24,


	k_ESteamNetworkingConfig_TimeoutConnected = 25,

	k_ESteamNetworkingConfig_SendBufferSize = 9,

	k_ESteamNetworkingConfig_RecvBufferSize = 47,

	k_ESteamNetworkingConfig_RecvBufferMessages = 48,

	k_ESteamNetworkingConfig_RecvMaxMessageSize = 49,

	k_ESteamNetworkingConfig_RecvMaxSegmentsPerPacket = 50,

	k_ESteamNetworkingConfig_ConnectionUserData = 40,

	k_ESteamNetworkingConfig_SendRateMin = 10,
	k_ESteamNetworkingConfig_SendRateMax = 11,

	k_ESteamNetworkingConfig_NagleTime = 12,

	k_ESteamNetworkingConfig_IP_AllowWithoutAuth = 23,

	k_ESteamNetworkingConfig_IPLocalHost_AllowWithoutAuth = 52,

	k_ESteamNetworkingConfig_MTU_PacketSize = 32,

	k_ESteamNetworkingConfig_MTU_DataSize = 33,

	k_ESteamNetworkingConfig_Unencrypted = 34,

	k_ESteamNetworkingConfig_SymmetricConnect = 37,

	k_ESteamNetworkingConfig_LocalVirtualPort = 38,

	k_ESteamNetworkingConfig_DualWifi_Enable = 39,

	k_ESteamNetworkingConfig_EnableDiagnosticsUI = 46,

	k_ESteamNetworkingConfig_SendTimeSincePreviousPacket = 59,

	k_ESteamNetworkingConfig_FakePacketLoss_Send = 2,
	k_ESteamNetworkingConfig_FakePacketLoss_Recv = 3,


	k_ESteamNetworkingConfig_FakePacketLag_Send = 4,
	k_ESteamNetworkingConfig_FakePacketLag_Recv = 5,

	k_ESteamNetworkingConfig_FakePacketJitter_Send_Avg = 53,
	k_ESteamNetworkingConfig_FakePacketJitter_Send_Max = 54,
	k_ESteamNetworkingConfig_FakePacketJitter_Send_Pct = 55,
	k_ESteamNetworkingConfig_FakePacketJitter_Recv_Avg = 56,
	k_ESteamNetworkingConfig_FakePacketJitter_Recv_Max = 57,
	k_ESteamNetworkingConfig_FakePacketJitter_Recv_Pct = 58,

	k_ESteamNetworkingConfig_FakePacketReorder_Send = 6,
	k_ESteamNetworkingConfig_FakePacketReorder_Recv = 7,

	k_ESteamNetworkingConfig_FakePacketReorder_Time = 8,


	k_ESteamNetworkingConfig_FakePacketDup_Send = 26,
	k_ESteamNetworkingConfig_FakePacketDup_Recv = 27,

	k_ESteamNetworkingConfig_FakePacketDup_TimeMax = 28,

	k_ESteamNetworkingConfig_PacketTraceMaxBytes = 41,

	k_ESteamNetworkingConfig_FakeRateLimit_Send_Rate = 42,
	k_ESteamNetworkingConfig_FakeRateLimit_Send_Burst = 43,
	k_ESteamNetworkingConfig_FakeRateLimit_Recv_Rate = 44,
	k_ESteamNetworkingConfig_FakeRateLimit_Recv_Burst = 45,

	k_ESteamNetworkingConfig_OutOfOrderCorrectionWindowMicroseconds = 51,

	k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged = 201,

	k_ESteamNetworkingConfig_Callback_AuthStatusChanged = 202,

	k_ESteamNetworkingConfig_Callback_RelayNetworkStatusChanged = 203,

	k_ESteamNetworkingConfig_Callback_MessagesSessionRequest = 204,

	k_ESteamNetworkingConfig_Callback_MessagesSessionFailed = 205,

	k_ESteamNetworkingConfig_Callback_CreateConnectionSignaling = 206,

	k_ESteamNetworkingConfig_Callback_FakeIPResult = 207,

	k_ESteamNetworkingConfig_P2P_STUN_ServerList = 103,

	k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable = 104,

	k_ESteamNetworkingConfig_P2P_Transport_ICE_Penalty = 105,
	k_ESteamNetworkingConfig_P2P_Transport_SDR_Penalty = 106,
	k_ESteamNetworkingConfig_P2P_TURN_ServerList = 107,
	k_ESteamNetworkingConfig_P2P_TURN_UserList = 108,
	k_ESteamNetworkingConfig_P2P_TURN_PassList = 109,

	k_ESteamNetworkingConfig_P2P_Transport_ICE_Implementation = 110,

	k_ESteamNetworkingConfig_SDRClient_ConsecutitivePingTimeoutsFailInitial = 19,

	k_ESteamNetworkingConfig_SDRClient_ConsecutitivePingTimeoutsFail = 20,

	k_ESteamNetworkingConfig_SDRClient_MinPingsBeforePingAccurate = 21,

	k_ESteamNetworkingConfig_SDRClient_SingleSocket = 22,

	k_ESteamNetworkingConfig_SDRClient_ForceRelayCluster = 29,

	k_ESteamNetworkingConfig_SDRClient_DevTicket = 30,

	k_ESteamNetworkingConfig_SDRClient_ForceProxyAddr = 31,

	k_ESteamNetworkingConfig_SDRClient_FakeClusterPing = 36,

	k_ESteamNetworkingConfig_SDRClient_LimitPingProbesToNearestN = 60,

	k_ESteamNetworkingConfig_LogLevel_AckRTT = 13,
	k_ESteamNetworkingConfig_LogLevel_PacketDecode = 14,
	k_ESteamNetworkingConfig_LogLevel_Message = 15,
	k_ESteamNetworkingConfig_LogLevel_PacketGaps = 16,
	k_ESteamNetworkingConfig_LogLevel_P2PRendezvous = 17,
	k_ESteamNetworkingConfig_LogLevel_SDRRelayPings = 18,

	k_ESteamNetworkingConfig_ECN = 999,

	k_ESteamNetworkingConfig_SDRClient_EnableTOSProbes = 998,

	k_ESteamNetworkingConfig_DELETED_EnumerateDevVars = 35,

	k_ESteamNetworkingConfigValue__Force32Bit = 0x7fffffff
};


const int k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Default = -1;
const int k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Disable = 0;
const int k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Relay = 1;
const int k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Private = 2;
const int k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Public = 4;
const int k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All = 0x7fffffff;

struct SteamNetworkingConfigValue_t
{

	ESteamNetworkingConfigValue m_eValue;


	ESteamNetworkingConfigDataType m_eDataType;


	union
	{
		int32_t m_int32;
		int64_t m_int64;
		float m_float;
		const char *m_string;
		void *m_ptr;
	} m_val;

	inline void SetInt32( ESteamNetworkingConfigValue eVal, int32_t data )
	{
		m_eValue = eVal;
		m_eDataType = k_ESteamNetworkingConfig_Int32;
		m_val.m_int32 = data;
	}
	inline void SetInt64( ESteamNetworkingConfigValue eVal, int64_t data )
	{
		m_eValue = eVal;
		m_eDataType = k_ESteamNetworkingConfig_Int64;
		m_val.m_int64 = data;
	}
	inline void SetFloat( ESteamNetworkingConfigValue eVal, float data )
	{
		m_eValue = eVal;
		m_eDataType = k_ESteamNetworkingConfig_Float;
		m_val.m_float = data;
	}
	inline void SetPtr( ESteamNetworkingConfigValue eVal, void *data )
	{
		m_eValue = eVal;
		m_eDataType = k_ESteamNetworkingConfig_Ptr;
		m_val.m_ptr = data;
	}
	inline void SetString( ESteamNetworkingConfigValue eVal, const char *data )
	{
		m_eValue = eVal;
		m_eDataType = k_ESteamNetworkingConfig_Ptr;
		m_val.m_string = data;
	}
};


enum ESteamNetworkingGetConfigValueResult
{
	k_ESteamNetworkingGetConfigValue_BadValue = -1,
	k_ESteamNetworkingGetConfigValue_BadScopeObj = -2,
	k_ESteamNetworkingGetConfigValue_BufferTooSmall = -3,
	k_ESteamNetworkingGetConfigValue_OK = 1,
	k_ESteamNetworkingGetConfigValue_OKInherited = 2,

	k_ESteamNetworkingGetConfigValueResult__Force32Bit = 0x7fffffff
};

enum ESteamNetworkingSocketsDebugOutputType
{
	k_ESteamNetworkingSocketsDebugOutputType_None = 0,
	k_ESteamNetworkingSocketsDebugOutputType_Bug = 1,
	k_ESteamNetworkingSocketsDebugOutputType_Error = 2,
	k_ESteamNetworkingSocketsDebugOutputType_Important = 3,
	k_ESteamNetworkingSocketsDebugOutputType_Warning = 4,
	k_ESteamNetworkingSocketsDebugOutputType_Msg = 5,
	k_ESteamNetworkingSocketsDebugOutputType_Verbose = 6,
	k_ESteamNetworkingSocketsDebugOutputType_Debug = 7,
	k_ESteamNetworkingSocketsDebugOutputType_Everything = 8,

	k_ESteamNetworkingSocketsDebugOutputType__Force32Bit = 0x7fffffff
};


typedef void (*FSteamNetworkingSocketsDebugOutput)( ESteamNetworkingSocketsDebugOutputType nType, const char *pszMsg );

inline SteamNetworkingPOPID CalculateSteamNetworkingPOPIDFromString( const char *pszCode )
{

	SteamNetworkingPOPID result = (uint32)(uint8)pszCode[0] << 16U;
	if ( result && pszCode[1] )
	{
		result |= ( (uint32)(uint8)pszCode[1] << 8U );
		if ( pszCode[2] )
		{
			result |= (uint32)(uint8)pszCode[2] | ( (uint32)(uint8)pszCode[3] << 24U );
		}
	}
	return result;
}

template <int N>
inline void GetSteamNetworkingLocationPOPStringFromID( SteamNetworkingPOPID id, char (&szCode)[N] )
{
	static_assert( N >= 5, "Fixed-size buffer not big enough to hold SDR POP ID" );
	szCode[0] = char( id >> 16U );
	szCode[1] = char( id >> 8U );
	szCode[2] = char( id );
	szCode[3] = char( id >> 24U );
	szCode[4] = 0;
}


const SteamNetworkingPOPID k_SteamDatagramPOPID_dev = ( (uint32)'d' << 16U ) | ( (uint32)'e' << 8U ) | (uint32)'v';

#ifndef API_GEN


struct SteamNetworkingPOPIDRender
{
	SteamNetworkingPOPIDRender( SteamNetworkingPOPID x ) { GetSteamNetworkingLocationPOPStringFromID( x, buf ); }
	inline const char *c_str() const { return buf; }
private:
	char buf[ 8 ];
};

#endif

#ifndef API_GEN


typedef SteamNetworkingMessage_t ISteamNetworkingMessage;
typedef SteamNetworkingErrMsg SteamDatagramErrMsg;

inline void SteamNetworkingIPAddr::Clear() { memset( this, 0, sizeof(*this) ); }
inline bool SteamNetworkingIPAddr::IsIPv6AllZeros() const { const uint64 *q = (const uint64 *)m_ipv6; return q[0] == 0 && q[1] == 0; }
inline void SteamNetworkingIPAddr::SetIPv6( const uint8 *ipv6, uint16 nPort ) { memcpy( m_ipv6, ipv6, 16 ); m_port = nPort; }
inline void SteamNetworkingIPAddr::SetIPv4( uint32 nIP, uint16 nPort ) { m_ipv4.m_8zeros = 0; m_ipv4.m_0000 = 0; m_ipv4.m_ffff = 0xffff; m_ipv4.m_ip[0] = uint8(nIP>>24); m_ipv4.m_ip[1] = uint8(nIP>>16); m_ipv4.m_ip[2] = uint8(nIP>>8); m_ipv4.m_ip[3] = uint8(nIP); m_port = nPort; }
inline bool SteamNetworkingIPAddr::IsIPv4() const { return m_ipv4.m_8zeros == 0 && m_ipv4.m_0000 == 0 && m_ipv4.m_ffff == 0xffff; }
inline uint32 SteamNetworkingIPAddr::GetIPv4() const { return IsIPv4() ? ( (uint32(m_ipv4.m_ip[0])<<24) | (uint32(m_ipv4.m_ip[1])<<16) | (uint32(m_ipv4.m_ip[2])<<8) | uint32(m_ipv4.m_ip[3]) ) : 0; }
inline void SteamNetworkingIPAddr::SetIPv6LocalHost( uint16 nPort ) { m_ipv4.m_8zeros = 0; m_ipv4.m_0000 = 0; m_ipv4.m_ffff = 0; m_ipv6[12] = 0; m_ipv6[13] = 0; m_ipv6[14] = 0; m_ipv6[15] = 1; m_port = nPort; }
inline bool SteamNetworkingIPAddr::IsLocalHost() const { return ( m_ipv4.m_8zeros == 0 && m_ipv4.m_0000 == 0 && m_ipv4.m_ffff == 0 && m_ipv6[12] == 0 && m_ipv6[13] == 0 && m_ipv6[14] == 0 && m_ipv6[15] == 1 ) || ( GetIPv4() == 0x7f000001 ); }
inline bool SteamNetworkingIPAddr::operator==(const SteamNetworkingIPAddr &x ) const { return memcmp( this, &x, sizeof(SteamNetworkingIPAddr) ) == 0; }

inline void SteamNetworkingIdentity::Clear() { memset( this, 0, sizeof(*this) ); }
inline bool SteamNetworkingIdentity::IsInvalid() const { return m_eType == k_ESteamNetworkingIdentityType_Invalid; }
inline void SteamNetworkingIdentity::SetSteamID( CSteamID steamID ) { SetSteamID64( steamID.ConvertToUint64() ); }
inline CSteamID SteamNetworkingIdentity::GetSteamID() const { return CSteamID( GetSteamID64() ); }
inline void SteamNetworkingIdentity::SetSteamID64( uint64 steamID ) { m_eType = k_ESteamNetworkingIdentityType_SteamID; m_cbSize = sizeof( m_steamID64 ); m_steamID64 = steamID; }
inline uint64 SteamNetworkingIdentity::GetSteamID64() const { return m_eType == k_ESteamNetworkingIdentityType_SteamID ? m_steamID64 : 0; }
inline bool SteamNetworkingIdentity::SetXboxPairwiseID( const char *pszString ) { size_t l = strlen( pszString ); if ( l < 1 || l >= sizeof(m_szXboxPairwiseID) ) return false;
	m_eType = k_ESteamNetworkingIdentityType_XboxPairwiseID; m_cbSize = int(l+1); memcpy( m_szXboxPairwiseID, pszString, m_cbSize ); return true; }
inline const char *SteamNetworkingIdentity::GetXboxPairwiseID() const { return m_eType == k_ESteamNetworkingIdentityType_XboxPairwiseID ? m_szXboxPairwiseID : NULL; }
inline void SteamNetworkingIdentity::SetPSNID( uint64 id ) { m_eType = k_ESteamNetworkingIdentityType_SonyPSN; m_cbSize = sizeof( m_PSNID ); m_PSNID = id; }
inline uint64 SteamNetworkingIdentity::GetPSNID() const { return m_eType == k_ESteamNetworkingIdentityType_SonyPSN ? m_PSNID : 0; }
inline void SteamNetworkingIdentity::SetIPAddr( const SteamNetworkingIPAddr &addr ) { m_eType = k_ESteamNetworkingIdentityType_IPAddress; m_cbSize = (int)sizeof(m_ip); m_ip = addr; }
inline const SteamNetworkingIPAddr *SteamNetworkingIdentity::GetIPAddr() const { return m_eType == k_ESteamNetworkingIdentityType_IPAddress ? &m_ip : NULL; }
inline void SteamNetworkingIdentity::SetIPv4Addr( uint32 nIPv4, uint16 nPort ) { m_eType = k_ESteamNetworkingIdentityType_IPAddress; m_cbSize = (int)sizeof(m_ip); m_ip.SetIPv4( nIPv4, nPort ); }
inline uint32 SteamNetworkingIdentity::GetIPv4() const { return m_eType == k_ESteamNetworkingIdentityType_IPAddress ? m_ip.GetIPv4() : 0; }
inline ESteamNetworkingFakeIPType SteamNetworkingIdentity::GetFakeIPType() const { return m_eType == k_ESteamNetworkingIdentityType_IPAddress ? m_ip.GetFakeIPType() : k_ESteamNetworkingFakeIPType_Invalid; }
inline void SteamNetworkingIdentity::SetLocalHost() { m_eType = k_ESteamNetworkingIdentityType_IPAddress; m_cbSize = (int)sizeof(m_ip); m_ip.SetIPv6LocalHost(); }
inline bool SteamNetworkingIdentity::IsLocalHost() const { return m_eType == k_ESteamNetworkingIdentityType_IPAddress && m_ip.IsLocalHost(); }
inline bool SteamNetworkingIdentity::SetGenericString( const char *pszString ) { size_t l = strlen( pszString ); if ( l >= sizeof(m_szGenericString) ) return false;
	m_eType = k_ESteamNetworkingIdentityType_GenericString; m_cbSize = int(l+1); memcpy( m_szGenericString, pszString, m_cbSize ); return true; }
inline const char *SteamNetworkingIdentity::GetGenericString() const { return m_eType == k_ESteamNetworkingIdentityType_GenericString ? m_szGenericString : NULL; }
inline bool SteamNetworkingIdentity::SetGenericBytes( const void *data, size_t cbLen ) { if ( cbLen > sizeof(m_genericBytes) ) return false;
	m_eType = k_ESteamNetworkingIdentityType_GenericBytes; m_cbSize = int(cbLen); memcpy( m_genericBytes, data, m_cbSize ); return true; }
inline const uint8 *SteamNetworkingIdentity::GetGenericBytes( int &cbLen ) const { if ( m_eType != k_ESteamNetworkingIdentityType_GenericBytes ) return NULL;
	cbLen = m_cbSize; return m_genericBytes; }
inline bool SteamNetworkingIdentity::operator==(const SteamNetworkingIdentity &x ) const { return m_eType == x.m_eType && m_cbSize == x.m_cbSize && memcmp( m_genericBytes, x.m_genericBytes, m_cbSize ) == 0; }
inline void SteamNetworkingMessage_t::Release() { (*m_pfnRelease)( this ); }

#endif

#endif
