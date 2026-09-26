#ifndef ISTEAMNETWORKINGUTILS
#define ISTEAMNETWORKINGUTILS
#pragma once

#include "steamnetworkingtypes.h"
#include "steam_api_common.h"

struct SteamDatagramRelayAuthTicket;
struct SteamRelayNetworkStatus_t;

class ISteamNetworkingUtils
{
public:

	virtual SteamNetworkingMessage_t *AllocateMessage( int cbAllocateBuffer ) = 0;

	inline void InitRelayNetworkAccess();

	virtual ESteamNetworkingAvailability GetRelayNetworkStatus( SteamRelayNetworkStatus_t *pDetails ) = 0;

	virtual float GetLocalPingLocation( SteamNetworkPingLocation_t &result ) = 0;

	virtual int EstimatePingTimeBetweenTwoLocations( const SteamNetworkPingLocation_t &location1, const SteamNetworkPingLocation_t &location2 ) = 0;

	virtual int EstimatePingTimeFromLocalHost( const SteamNetworkPingLocation_t &remoteLocation ) = 0;

	virtual void ConvertPingLocationToString( const SteamNetworkPingLocation_t &location, char *pszBuf, int cchBufSize ) = 0;

	virtual bool ParsePingLocationString( const char *pszString, SteamNetworkPingLocation_t &result ) = 0;

	virtual bool CheckPingDataUpToDate( float flMaxAgeSeconds ) = 0;

	virtual int GetPingToDataCenter( SteamNetworkingPOPID popID, SteamNetworkingPOPID *pViaRelayPoP ) = 0;


	virtual int GetDirectPingToPOP( SteamNetworkingPOPID popID ) = 0;


	virtual int GetPOPCount() = 0;

	virtual int GetPOPList( SteamNetworkingPOPID *list, int nListSz ) = 0;

	virtual SteamNetworkingMicroseconds GetLocalTimestamp() = 0;

	virtual void SetDebugOutputFunction( ESteamNetworkingSocketsDebugOutputType eDetailLevel, FSteamNetworkingSocketsDebugOutput pfnFunc ) = 0;

	inline bool IsFakeIPv4( uint32 nIPv4 ) { return GetIPv4FakeIPType( nIPv4 ) > k_ESteamNetworkingFakeIPType_NotFake; }
	virtual ESteamNetworkingFakeIPType GetIPv4FakeIPType( uint32 nIPv4 ) = 0;

	virtual EResult GetRealIdentityForFakeIP( const SteamNetworkingIPAddr &fakeIP, SteamNetworkingIdentity *pOutRealIdentity ) = 0;

	bool SetGlobalConfigValueInt32( ESteamNetworkingConfigValue eValue, int32 val );
	bool SetGlobalConfigValueFloat( ESteamNetworkingConfigValue eValue, float val );
	bool SetGlobalConfigValueString( ESteamNetworkingConfigValue eValue, const char *val );
	bool SetGlobalConfigValuePtr( ESteamNetworkingConfigValue eValue, void *val );
	bool SetConnectionConfigValueInt32( HSteamNetConnection hConn, ESteamNetworkingConfigValue eValue, int32 val );
	bool SetConnectionConfigValueFloat( HSteamNetConnection hConn, ESteamNetworkingConfigValue eValue, float val );
	bool SetConnectionConfigValueString( HSteamNetConnection hConn, ESteamNetworkingConfigValue eValue, const char *val );

	bool SetGlobalCallback_SteamNetConnectionStatusChanged( FnSteamNetConnectionStatusChanged fnCallback );
	bool SetGlobalCallback_SteamNetAuthenticationStatusChanged( FnSteamNetAuthenticationStatusChanged fnCallback );
	bool SetGlobalCallback_SteamRelayNetworkStatusChanged( FnSteamRelayNetworkStatusChanged fnCallback );
	bool SetGlobalCallback_FakeIPResult( FnSteamNetworkingFakeIPResult fnCallback );
	bool SetGlobalCallback_MessagesSessionRequest( FnSteamNetworkingMessagesSessionRequest fnCallback );
	bool SetGlobalCallback_MessagesSessionFailed( FnSteamNetworkingMessagesSessionFailed fnCallback );

	virtual bool SetConfigValue( ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigScope eScopeType, intptr_t scopeObj,
		ESteamNetworkingConfigDataType eDataType, const void *pArg ) = 0;

	bool SetConfigValueStruct( const SteamNetworkingConfigValue_t &opt, ESteamNetworkingConfigScope eScopeType, intptr_t scopeObj );

	virtual ESteamNetworkingGetConfigValueResult GetConfigValue( ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigScope eScopeType, intptr_t scopeObj,
		ESteamNetworkingConfigDataType *pOutDataType, void *pResult, size_t *cbResult ) = 0;

	virtual const char *GetConfigValueInfo( ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigDataType *pOutDataType,
		ESteamNetworkingConfigScope *pOutScope ) = 0;

	virtual ESteamNetworkingConfigValue IterateGenericEditableConfigValues( ESteamNetworkingConfigValue eCurrent, bool bEnumerateDevVars ) = 0;

	virtual void SteamNetworkingIPAddr_ToString( const SteamNetworkingIPAddr &addr, char *buf, size_t cbBuf, bool bWithPort ) = 0;
	virtual bool SteamNetworkingIPAddr_ParseString( SteamNetworkingIPAddr *pAddr, const char *pszStr ) = 0;
	virtual ESteamNetworkingFakeIPType SteamNetworkingIPAddr_GetFakeIPType( const SteamNetworkingIPAddr &addr ) = 0;
	virtual void SteamNetworkingIdentity_ToString( const SteamNetworkingIdentity &identity, char *buf, size_t cbBuf ) = 0;
	virtual bool SteamNetworkingIdentity_ParseString( SteamNetworkingIdentity *pIdentity, const char *pszStr ) = 0;

protected:
	~ISteamNetworkingUtils();
};
#define STEAMNETWORKINGUTILS_INTERFACE_VERSION "SteamNetworkingUtils004"

#ifdef STEAMNETWORKINGSOCKETS_STANDALONELIB


	static_assert( STEAMNETWORKINGUTILS_INTERFACE_VERSION[22] == '4', "Version mismatch" );
	STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingUtils *SteamNetworkingUtils_LibV4();
	inline ISteamNetworkingUtils *SteamNetworkingUtils_Lib() { return SteamNetworkingUtils_LibV4(); }

	#ifndef STEAMNETWORKINGSOCKETS_STEAMAPI
		inline ISteamNetworkingUtils *SteamNetworkingUtils() { return SteamNetworkingUtils_LibV4(); }
	#endif
#endif


#ifdef STEAMNETWORKINGSOCKETS_STEAMAPI
	STEAM_DEFINE_INTERFACE_ACCESSOR( ISteamNetworkingUtils *, SteamNetworkingUtils_SteamAPI,

		SteamInternal_FindOrCreateUserInterface( 0, STEAMNETWORKINGUTILS_INTERFACE_VERSION ) ?
		SteamInternal_FindOrCreateUserInterface( 0, STEAMNETWORKINGUTILS_INTERFACE_VERSION ) :
		SteamInternal_FindOrCreateGameServerInterface( 0, STEAMNETWORKINGUTILS_INTERFACE_VERSION ),
		"global",
		STEAMNETWORKINGUTILS_INTERFACE_VERSION
	)

	#ifndef STEAMNETWORKINGSOCKETS_STANDALONELIB
		inline ISteamNetworkingUtils *SteamNetworkingUtils() { return SteamNetworkingUtils_SteamAPI(); }
	#endif
#endif

struct SteamRelayNetworkStatus_t
{
	enum { k_iCallback = k_iSteamNetworkingUtilsCallbacks + 1 };

	ESteamNetworkingAvailability m_eAvail;

	int m_bPingMeasurementInProgress;

	ESteamNetworkingAvailability m_eAvailNetworkConfig;

	ESteamNetworkingAvailability m_eAvailAnyRelay;

	char m_debugMsg[ 256 ];
};

#ifndef API_GEN

struct SteamNetworkingIdentityRender
{
	SteamNetworkingIdentityRender( const SteamNetworkingIdentity &x ) { x.ToString( buf, sizeof(buf) ); }
	inline const char *c_str() const { return buf; }
private:
	char buf[ SteamNetworkingIdentity::k_cchMaxString ];
};


struct SteamNetworkingIPAddrRender
{
	SteamNetworkingIPAddrRender( const SteamNetworkingIPAddr &x, bool bWithPort = true ) { x.ToString( buf, sizeof(buf), bWithPort ); }
	inline const char *c_str() const { return buf; }
private:
	char buf[ SteamNetworkingIPAddr::k_cchMaxString ];
};

#endif

inline void ISteamNetworkingUtils::InitRelayNetworkAccess() { CheckPingDataUpToDate( 1e10f ); }
inline bool ISteamNetworkingUtils::SetGlobalConfigValueInt32( ESteamNetworkingConfigValue eValue, int32 val ) { return SetConfigValue( eValue, k_ESteamNetworkingConfig_Global, 0, k_ESteamNetworkingConfig_Int32, &val ); }
inline bool ISteamNetworkingUtils::SetGlobalConfigValueFloat( ESteamNetworkingConfigValue eValue, float val ) { return SetConfigValue( eValue, k_ESteamNetworkingConfig_Global, 0, k_ESteamNetworkingConfig_Float, &val ); }
inline bool ISteamNetworkingUtils::SetGlobalConfigValueString( ESteamNetworkingConfigValue eValue, const char *val ) { return SetConfigValue( eValue, k_ESteamNetworkingConfig_Global, 0, k_ESteamNetworkingConfig_String, val ); }
inline bool ISteamNetworkingUtils::SetGlobalConfigValuePtr( ESteamNetworkingConfigValue eValue, void *val ) { return SetConfigValue( eValue, k_ESteamNetworkingConfig_Global, 0, k_ESteamNetworkingConfig_Ptr, &val ); }
inline bool ISteamNetworkingUtils::SetConnectionConfigValueInt32( HSteamNetConnection hConn, ESteamNetworkingConfigValue eValue, int32 val ) { return SetConfigValue( eValue, k_ESteamNetworkingConfig_Connection, hConn, k_ESteamNetworkingConfig_Int32, &val ); }
inline bool ISteamNetworkingUtils::SetConnectionConfigValueFloat( HSteamNetConnection hConn, ESteamNetworkingConfigValue eValue, float val ) { return SetConfigValue( eValue, k_ESteamNetworkingConfig_Connection, hConn, k_ESteamNetworkingConfig_Float, &val ); }
inline bool ISteamNetworkingUtils::SetConnectionConfigValueString( HSteamNetConnection hConn, ESteamNetworkingConfigValue eValue, const char *val ) { return SetConfigValue( eValue, k_ESteamNetworkingConfig_Connection, hConn, k_ESteamNetworkingConfig_String, val ); }
inline bool ISteamNetworkingUtils::SetGlobalCallback_SteamNetConnectionStatusChanged( FnSteamNetConnectionStatusChanged fnCallback ) { return SetGlobalConfigValuePtr( k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)fnCallback ); }
inline bool ISteamNetworkingUtils::SetGlobalCallback_SteamNetAuthenticationStatusChanged( FnSteamNetAuthenticationStatusChanged fnCallback ) { return SetGlobalConfigValuePtr( k_ESteamNetworkingConfig_Callback_AuthStatusChanged, (void*)fnCallback ); }
inline bool ISteamNetworkingUtils::SetGlobalCallback_SteamRelayNetworkStatusChanged( FnSteamRelayNetworkStatusChanged fnCallback ) { return SetGlobalConfigValuePtr( k_ESteamNetworkingConfig_Callback_RelayNetworkStatusChanged, (void*)fnCallback ); }
inline bool ISteamNetworkingUtils::SetGlobalCallback_FakeIPResult( FnSteamNetworkingFakeIPResult fnCallback ) { return SetGlobalConfigValuePtr( k_ESteamNetworkingConfig_Callback_FakeIPResult, (void*)fnCallback ); }
inline bool ISteamNetworkingUtils::SetGlobalCallback_MessagesSessionRequest( FnSteamNetworkingMessagesSessionRequest fnCallback ) { return SetGlobalConfigValuePtr( k_ESteamNetworkingConfig_Callback_MessagesSessionRequest, (void*)fnCallback ); }
inline bool ISteamNetworkingUtils::SetGlobalCallback_MessagesSessionFailed( FnSteamNetworkingMessagesSessionFailed fnCallback ) { return SetGlobalConfigValuePtr( k_ESteamNetworkingConfig_Callback_MessagesSessionFailed, (void*)fnCallback ); }

inline bool ISteamNetworkingUtils::SetConfigValueStruct( const SteamNetworkingConfigValue_t &opt, ESteamNetworkingConfigScope eScopeType, intptr_t scopeObj )
{

	const void *pVal = ( opt.m_eDataType == k_ESteamNetworkingConfig_String ) ? (const void *)opt.m_val.m_string : (const void *)&opt.m_val;
	return SetConfigValue( opt.m_eValue, eScopeType, scopeObj, opt.m_eDataType, pVal );
}


#if defined( STEAMNETWORKINGSOCKETS_STATIC_LINK ) || defined(STEAMNETWORKINGSOCKETS_FOREXPORT) || defined( STEAMNETWORKINGSOCKETS_STANDALONELIB )


	STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingIPAddr_ToString( const SteamNetworkingIPAddr *pAddr, char *buf, size_t cbBuf, bool bWithPort );
	STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingIPAddr_ParseString( SteamNetworkingIPAddr *pAddr, const char *pszStr );
	STEAMNETWORKINGSOCKETS_INTERFACE ESteamNetworkingFakeIPType SteamNetworkingIPAddr_GetFakeIPType( const SteamNetworkingIPAddr *pAddr );
	STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingIdentity_ToString( const SteamNetworkingIdentity *pIdentity, char *buf, size_t cbBuf );
	STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingIdentity_ParseString( SteamNetworkingIdentity *pIdentity, size_t sizeofIdentity, const char *pszStr );
	inline void SteamNetworkingIPAddr::ToString( char *buf, size_t cbBuf, bool bWithPort ) const { SteamNetworkingIPAddr_ToString( this, buf, cbBuf, bWithPort ); }
	inline bool SteamNetworkingIPAddr::ParseString( const char *pszStr ) { return SteamNetworkingIPAddr_ParseString( this, pszStr ); }
	inline ESteamNetworkingFakeIPType SteamNetworkingIPAddr::GetFakeIPType() const { return SteamNetworkingIPAddr_GetFakeIPType( this ); }
	inline void SteamNetworkingIdentity::ToString( char *buf, size_t cbBuf ) const { SteamNetworkingIdentity_ToString( this, buf, cbBuf ); }
	inline bool SteamNetworkingIdentity::ParseString( const char *pszStr ) { return SteamNetworkingIdentity_ParseString( this, sizeof(*this), pszStr ); }

#elif defined( STEAMNETWORKINGSOCKETS_STEAMAPI )

	inline void SteamNetworkingIPAddr::ToString( char *buf, size_t cbBuf, bool bWithPort ) const { SteamNetworkingUtils()->SteamNetworkingIPAddr_ToString( *this, buf, cbBuf, bWithPort ); }
	inline bool SteamNetworkingIPAddr::ParseString( const char *pszStr ) { return SteamNetworkingUtils()->SteamNetworkingIPAddr_ParseString( this, pszStr ); }
	inline ESteamNetworkingFakeIPType SteamNetworkingIPAddr::GetFakeIPType() const { return SteamNetworkingUtils()->SteamNetworkingIPAddr_GetFakeIPType( *this ); }
	inline void SteamNetworkingIdentity::ToString( char *buf, size_t cbBuf ) const { SteamNetworkingUtils()->SteamNetworkingIdentity_ToString( *this, buf, cbBuf ); }
	inline bool SteamNetworkingIdentity::ParseString( const char *pszStr ) { return SteamNetworkingUtils()->SteamNetworkingIdentity_ParseString( this, pszStr ); }
#else
	#error "Invalid config"
#endif

#endif
