#ifndef STEAMNETWORKINGSOCKETS_H
#define STEAMNETWORKINGSOCKETS_H
#ifdef _WIN32
#pragma once
#endif

#include "isteamnetworkingsockets.h"

extern "C" {

STEAMNETWORKINGSOCKETS_INTERFACE bool GameNetworkingSockets_Init( const SteamNetworkingIdentity *pIdentity, SteamNetworkingErrMsg &errMsg );


STEAMNETWORKINGSOCKETS_INTERFACE void GameNetworkingSockets_Kill();

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetCustomMemoryAllocator(
	void* (*pfn_malloc)( size_t s ),
	void (*pfn_free)( void *p ),
	void* (*pfn_realloc)( void *p, size_t s )
);

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetLockWaitWarningThreshold( SteamNetworkingMicroseconds usecThreshold );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetLockAcquiredCallback( void (*callback)( const char *tags, SteamNetworkingMicroseconds usecWaited ) );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetLockHeldCallback( void (*callback)( const char *tags, SteamNetworkingMicroseconds usecWaited ) );

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetServiceThreadInitCallback( void (*callback)() );

}

#endif
