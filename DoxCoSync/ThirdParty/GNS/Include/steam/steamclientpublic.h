#ifndef STEAMCLIENTPUBLIC_H
#define STEAMCLIENTPUBLIC_H

#include "steamtypes.h"
#include "steamuniverse.h"


enum EResult
{
	k_EResultNone = 0,
	k_EResultOK	= 1,
	k_EResultFail = 2,
	k_EResultNoConnection = 3,

	k_EResultInvalidPassword = 5,
	k_EResultLoggedInElsewhere = 6,
	k_EResultInvalidProtocolVer = 7,
	k_EResultInvalidParam = 8,
	k_EResultFileNotFound = 9,
	k_EResultBusy = 10,
	k_EResultInvalidState = 11,
	k_EResultInvalidName = 12,
	k_EResultInvalidEmail = 13,
	k_EResultDuplicateName = 14,
	k_EResultAccessDenied = 15,
	k_EResultTimeout = 16,
	k_EResultBanned = 17,
	k_EResultAccountNotFound = 18,
	k_EResultInvalidSteamID = 19,
	k_EResultServiceUnavailable = 20,
	k_EResultNotLoggedOn = 21,
	k_EResultPending = 22,
	k_EResultEncryptionFailure = 23,
	k_EResultInsufficientPrivilege = 24,
	k_EResultLimitExceeded = 25,
	k_EResultRevoked = 26,
	k_EResultExpired = 27,
	k_EResultAlreadyRedeemed = 28,
	k_EResultDuplicateRequest = 29,
	k_EResultAlreadyOwned = 30,
	k_EResultIPNotFound = 31,
	k_EResultPersistFailed = 32,
	k_EResultLockingFailed = 33,
	k_EResultLogonSessionReplaced = 34,
	k_EResultConnectFailed = 35,
	k_EResultHandshakeFailed = 36,
	k_EResultIOFailure = 37,
	k_EResultRemoteDisconnect = 38,
	k_EResultShoppingCartNotFound = 39,
	k_EResultBlocked = 40,
	k_EResultIgnored = 41,
	k_EResultNoMatch = 42,
	k_EResultAccountDisabled = 43,
	k_EResultServiceReadOnly = 44,
	k_EResultAccountNotFeatured = 45,
	k_EResultAdministratorOK = 46,
	k_EResultContentVersion = 47,
	k_EResultTryAnotherCM = 48,
	k_EResultPasswordRequiredToKickSession = 49,
	k_EResultAlreadyLoggedInElsewhere = 50,
	k_EResultSuspended = 51,
	k_EResultCancelled = 52,
	k_EResultDataCorruption = 53,
	k_EResultDiskFull = 54,
	k_EResultRemoteCallFailed = 55,
	k_EResultPasswordUnset = 56,
	k_EResultExternalAccountUnlinked = 57,
	k_EResultPSNTicketInvalid = 58,
	k_EResultExternalAccountAlreadyLinked = 59,
	k_EResultRemoteFileConflict = 60,
	k_EResultIllegalPassword = 61,
	k_EResultSameAsPreviousValue = 62,
	k_EResultAccountLogonDenied = 63,
	k_EResultCannotUseOldPassword = 64,
	k_EResultInvalidLoginAuthCode = 65,
	k_EResultAccountLogonDeniedNoMail = 66,
	k_EResultHardwareNotCapableOfIPT = 67,
	k_EResultIPTInitError = 68,
	k_EResultParentalControlRestricted = 69,
	k_EResultFacebookQueryError = 70,
	k_EResultExpiredLoginAuthCode = 71,
	k_EResultIPLoginRestrictionFailed = 72,
	k_EResultAccountLockedDown = 73,
	k_EResultAccountLogonDeniedVerifiedEmailRequired = 74,
	k_EResultNoMatchingURL = 75,
	k_EResultBadResponse = 76,
	k_EResultRequirePasswordReEntry = 77,
	k_EResultValueOutOfRange = 78,
	k_EResultUnexpectedError = 79,
	k_EResultDisabled = 80,
	k_EResultInvalidCEGSubmission = 81,
	k_EResultRestrictedDevice = 82,
	k_EResultRegionLocked = 83,
	k_EResultRateLimitExceeded = 84,
	k_EResultAccountLoginDeniedNeedTwoFactor = 85,
	k_EResultItemDeleted = 86,
	k_EResultAccountLoginDeniedThrottle = 87,
	k_EResultTwoFactorCodeMismatch = 88,
	k_EResultTwoFactorActivationCodeMismatch = 89,
	k_EResultAccountAssociatedToMultiplePartners = 90,
	k_EResultNotModified = 91,
	k_EResultNoMobileDevice = 92,
	k_EResultTimeNotSynced = 93,
	k_EResultSmsCodeFailed = 94,
	k_EResultAccountLimitExceeded = 95,
	k_EResultAccountActivityLimitExceeded = 96,
	k_EResultPhoneActivityLimitExceeded = 97,
	k_EResultRefundToWallet = 98,
	k_EResultEmailSendFailure = 99,
	k_EResultNotSettled = 100,
	k_EResultNeedCaptcha = 101,
	k_EResultGSLTDenied = 102,
	k_EResultGSOwnerDenied = 103,
	k_EResultInvalidItemType = 104,
	k_EResultIPBanned = 105,
	k_EResultGSLTExpired = 106,
	k_EResultInsufficientFunds = 107,
	k_EResultTooManyPending = 108,
	k_EResultNoSiteLicensesFound = 109,
	k_EResultWGNetworkSendExceeded = 110,
	k_EResultAccountNotFriends = 111,
	k_EResultLimitedUserAccount = 112,
	k_EResultCantRemoveItem = 113,
	k_EResultAccountDeleted = 114,
	k_EResultExistingUserCancelledLicense = 115,
	k_EResultCommunityCooldown = 116,
	k_EResultNoLauncherSpecified = 117,
	k_EResultMustAgreeToSSA = 118,
	k_EResultLauncherMigrated = 119,
	k_EResultSteamRealmMismatch = 120,
	k_EResultInvalidSignature = 121,
	k_EResultParseFailure = 122,
	k_EResultNoVerifiedPhone = 123,
	k_EResultInsufficientBattery = 124,
	k_EResultChargerRequired = 125,
	k_EResultCachedCredentialInvalid = 126,
	K_EResultPhoneNumberIsVOIP = 127,
	k_EResultNotSupported = 128,
	k_EResultFamilySizeLimitExceeded = 129,
};


enum EVoiceResult
{
	k_EVoiceResultOK = 0,
	k_EVoiceResultNotInitialized = 1,
	k_EVoiceResultNotRecording = 2,
	k_EVoiceResultNoData = 3,
	k_EVoiceResultBufferTooSmall = 4,
	k_EVoiceResultDataCorrupted = 5,
	k_EVoiceResultRestricted = 6,
	k_EVoiceResultUnsupportedCodec = 7,
	k_EVoiceResultReceiverOutOfDate = 8,
	k_EVoiceResultReceiverDidNotAnswer = 9,

};


enum EDenyReason
{
	k_EDenyInvalid = 0,
	k_EDenyInvalidVersion = 1,
	k_EDenyGeneric = 2,
	k_EDenyNotLoggedOn = 3,
	k_EDenyNoLicense = 4,
	k_EDenyCheater = 5,
	k_EDenyLoggedInElseWhere = 6,
	k_EDenyUnknownText = 7,
	k_EDenyIncompatibleAnticheat = 8,
	k_EDenyMemoryCorruption = 9,
	k_EDenyIncompatibleSoftware = 10,
	k_EDenySteamConnectionLost = 11,
	k_EDenySteamConnectionError = 12,
	k_EDenySteamResponseTimedOut = 13,
	k_EDenySteamValidationStalled = 14,
	k_EDenySteamOwnerLeftGuestUser = 15,
};


typedef uint32 HAuthTicket;
const HAuthTicket k_HAuthTicketInvalid = 0;


enum EBeginAuthSessionResult
{
	k_EBeginAuthSessionResultOK = 0,
	k_EBeginAuthSessionResultInvalidTicket = 1,
	k_EBeginAuthSessionResultDuplicateRequest = 2,
	k_EBeginAuthSessionResultInvalidVersion = 3,
	k_EBeginAuthSessionResultGameMismatch = 4,
	k_EBeginAuthSessionResultExpiredTicket = 5,
};


enum EAuthSessionResponse
{
	k_EAuthSessionResponseOK = 0,
	k_EAuthSessionResponseUserNotConnectedToSteam = 1,
	k_EAuthSessionResponseNoLicenseOrExpired = 2,
	k_EAuthSessionResponseVACBanned = 3,
	k_EAuthSessionResponseLoggedInElseWhere = 4,
	k_EAuthSessionResponseVACCheckTimedOut = 5,
	k_EAuthSessionResponseAuthTicketCanceled = 6,
	k_EAuthSessionResponseAuthTicketInvalidAlreadyUsed = 7,
	k_EAuthSessionResponseAuthTicketInvalid = 8,
	k_EAuthSessionResponsePublisherIssuedBan = 9,
	k_EAuthSessionResponseAuthTicketNetworkIdentityFailure = 10,
};


enum EUserHasLicenseForAppResult
{
	k_EUserHasLicenseResultHasLicense = 0,
	k_EUserHasLicenseResultDoesNotHaveLicense = 1,
	k_EUserHasLicenseResultNoAuth = 2,
};

enum EAccountType
{
	k_EAccountTypeInvalid = 0,
	k_EAccountTypeIndividual = 1,
	k_EAccountTypeMultiseat = 2,
	k_EAccountTypeGameServer = 3,
	k_EAccountTypeAnonGameServer = 4,
	k_EAccountTypePending = 5,
	k_EAccountTypeContentServer = 6,
	k_EAccountTypeClan = 7,
	k_EAccountTypeChat = 8,
	k_EAccountTypeConsoleUser = 9,
	k_EAccountTypeAnonUser = 10,


	k_EAccountTypeMax
};

enum EChatEntryType
{
	k_EChatEntryTypeInvalid = 0,
	k_EChatEntryTypeChatMsg = 1,
	k_EChatEntryTypeTyping = 2,
	k_EChatEntryTypeInviteGame = 3,
	k_EChatEntryTypeEmote = 4,

	k_EChatEntryTypeLeftConversation = 6,

	k_EChatEntryTypeEntered = 7,
	k_EChatEntryTypeWasKicked = 8,
	k_EChatEntryTypeWasBanned = 9,
	k_EChatEntryTypeDisconnected = 10,
	k_EChatEntryTypeHistoricalChat = 11,


	k_EChatEntryTypeLinkBlocked = 14,
};

enum EChatRoomEnterResponse
{
	k_EChatRoomEnterResponseSuccess = 1,
	k_EChatRoomEnterResponseDoesntExist = 2,
	k_EChatRoomEnterResponseNotAllowed = 3,
	k_EChatRoomEnterResponseFull = 4,
	k_EChatRoomEnterResponseError = 5,
	k_EChatRoomEnterResponseBanned = 6,
	k_EChatRoomEnterResponseLimited = 7,
	k_EChatRoomEnterResponseClanDisabled = 8,
	k_EChatRoomEnterResponseCommunityBan = 9,
	k_EChatRoomEnterResponseMemberBlockedYou = 10,
	k_EChatRoomEnterResponseYouBlockedMember = 11,

	k_EChatRoomEnterResponseRatelimitExceeded = 15,
};


const unsigned int k_unSteamAccountIDMask = 0xFFFFFFFF;
const unsigned int k_unSteamAccountInstanceMask = 0x000FFFFF;
const unsigned int k_unSteamUserDefaultInstance	= 1;

enum EChatSteamIDInstanceFlags
{
	k_EChatAccountInstanceMask = 0x00000FFF,

	k_EChatInstanceFlagClan = ( k_unSteamAccountInstanceMask + 1 ) >> 1,
	k_EChatInstanceFlagLobby = ( k_unSteamAccountInstanceMask + 1 ) >> 2,
	k_EChatInstanceFlagMMSLobby = ( k_unSteamAccountInstanceMask + 1 ) >> 3,


};

enum ENotificationPosition
{
	k_EPositionInvalid = -1,
	k_EPositionTopLeft = 0,
	k_EPositionTopRight = 1,
	k_EPositionBottomLeft = 2,
	k_EPositionBottomRight = 3,
};

enum EBroadcastUploadResult
{
	k_EBroadcastUploadResultNone = 0,
	k_EBroadcastUploadResultOK = 1,
	k_EBroadcastUploadResultInitFailed = 2,
	k_EBroadcastUploadResultFrameFailed = 3,
	k_EBroadcastUploadResultTimeout = 4,
	k_EBroadcastUploadResultBandwidthExceeded = 5,
	k_EBroadcastUploadResultLowFPS = 6,
	k_EBroadcastUploadResultMissingKeyFrames = 7,
	k_EBroadcastUploadResultNoConnection = 8,
	k_EBroadcastUploadResultRelayFailed = 9,
	k_EBroadcastUploadResultSettingsChanged = 10,
	k_EBroadcastUploadResultMissingAudio = 11,
	k_EBroadcastUploadResultTooFarBehind = 12,
	k_EBroadcastUploadResultTranscodeBehind = 13,
	k_EBroadcastUploadResultNotAllowedToPlay = 14,
	k_EBroadcastUploadResultBusy = 15,
	k_EBroadcastUploadResultBanned = 16,
	k_EBroadcastUploadResultAlreadyActive = 17,
	k_EBroadcastUploadResultForcedOff = 18,
	k_EBroadcastUploadResultAudioBehind = 19,
	k_EBroadcastUploadResultShutdown = 20,
	k_EBroadcastUploadResultDisconnect = 21,
	k_EBroadcastUploadResultVideoInitFailed = 22,
	k_EBroadcastUploadResultAudioInitFailed = 23,
};

enum EMarketNotAllowedReasonFlags
{
	k_EMarketNotAllowedReason_None = 0,


	k_EMarketNotAllowedReason_TemporaryFailure = (1 << 0),


	k_EMarketNotAllowedReason_AccountDisabled = (1 << 1),


	k_EMarketNotAllowedReason_AccountLockedDown = (1 << 2),


	k_EMarketNotAllowedReason_AccountLimited = (1 << 3),


	k_EMarketNotAllowedReason_TradeBanned = (1 << 4),

	k_EMarketNotAllowedReason_AccountNotTrusted = (1 << 5),


	k_EMarketNotAllowedReason_SteamGuardNotEnabled = (1 << 6),

	k_EMarketNotAllowedReason_SteamGuardOnlyRecentlyEnabled = (1 << 7),


	k_EMarketNotAllowedReason_RecentPasswordReset = (1 << 8),


	k_EMarketNotAllowedReason_NewPaymentMethod = (1 << 9),


	k_EMarketNotAllowedReason_InvalidCookie = (1 << 10),


	k_EMarketNotAllowedReason_UsingNewDevice = (1 << 11),


	k_EMarketNotAllowedReason_RecentSelfRefund = (1 << 12),


	k_EMarketNotAllowedReason_NewPaymentMethodCannotBeVerified = (1 << 13),


	k_EMarketNotAllowedReason_NoRecentPurchases = (1 << 14),


	k_EMarketNotAllowedReason_AcceptedWalletGift = (1 << 15),
};

enum EDurationControlProgress
{
	k_EDurationControlProgress_Full = 0,
	k_EDurationControlProgress_Half = 1,
	k_EDurationControlProgress_None = 2,

	k_EDurationControl_ExitSoon_3h = 3,
	k_EDurationControl_ExitSoon_5h = 4,
	k_EDurationControl_ExitSoon_Night = 5,
};

enum EDurationControlNotification
{
	k_EDurationControlNotification_None = 0,
	k_EDurationControlNotification_1Hour = 1,

	k_EDurationControlNotification_3Hours = 2,
	k_EDurationControlNotification_HalfProgress = 3,
	k_EDurationControlNotification_NoProgress = 4,

	k_EDurationControlNotification_ExitSoon_3h = 5,
	k_EDurationControlNotification_ExitSoon_5h = 6,
	k_EDurationControlNotification_ExitSoon_Night = 7,
};

enum EDurationControlOnlineState
{
	k_EDurationControlOnlineState_Invalid = 0,
	k_EDurationControlOnlineState_Offline = 1,
	k_EDurationControlOnlineState_Online = 2,
	k_EDurationControlOnlineState_OnlineHighPri = 3,
};


enum EBetaBranchFlags
{
	k_EBetaBranch_None			= 0,
	k_EBetaBranch_Default		= 1,
	k_EBetaBranch_Available		= 2,
	k_EBetaBranch_Private		= 4,
	k_EBetaBranch_Selected		= 8,
	k_EBetaBranch_Installed		= 16,
};

#pragma pack( push, 1 )

#define CSTEAMID_DEFINED


class CSteamID
{
public:

	CSteamID()
	{
		m_steamid.m_comp.m_unAccountID = 0;
		m_steamid.m_comp.m_EAccountType = k_EAccountTypeInvalid;
		m_steamid.m_comp.m_EUniverse = k_EUniverseInvalid;
		m_steamid.m_comp.m_unAccountInstance = 0;
	}

	CSteamID( uint32 unAccountID, EUniverse eUniverse, EAccountType eAccountType )
	{
		Set( unAccountID, eUniverse, eAccountType );
	}

	CSteamID( uint32 unAccountID, unsigned int unAccountInstance, EUniverse eUniverse, EAccountType eAccountType )
	{
#if defined(_SERVER) && defined(Assert)
		Assert( ( k_EAccountTypeIndividual != eAccountType ) || ( unAccountInstance == k_unSteamUserDefaultInstance ) );
#endif
		InstancedSet( unAccountID, unAccountInstance, eUniverse, eAccountType );
	}

	CSteamID( uint64 ulSteamID )
	{
		SetFromUint64( ulSteamID );
	}
#ifdef INT64_DIFFERENT_FROM_INT64_T
	CSteamID( uint64_t ulSteamID )
	{
		SetFromUint64( (uint64)ulSteamID );
	}
#endif

	void Set( uint32 unAccountID, EUniverse eUniverse, EAccountType eAccountType )
	{
		m_steamid.m_comp.m_unAccountID = unAccountID;
		m_steamid.m_comp.m_EUniverse = eUniverse;
		m_steamid.m_comp.m_EAccountType = eAccountType;

		if ( eAccountType == k_EAccountTypeClan || eAccountType == k_EAccountTypeGameServer )
		{
			m_steamid.m_comp.m_unAccountInstance = 0;
		}
		else
		{
			m_steamid.m_comp.m_unAccountInstance = k_unSteamUserDefaultInstance;
		}
	}

	void InstancedSet( uint32 unAccountID, uint32 unInstance, EUniverse eUniverse, EAccountType eAccountType )
	{
		m_steamid.m_comp.m_unAccountID = unAccountID;
		m_steamid.m_comp.m_EUniverse = eUniverse;
		m_steamid.m_comp.m_EAccountType = eAccountType;
		m_steamid.m_comp.m_unAccountInstance = unInstance;
	}

	void FullSet( uint64 ulIdentifier, EUniverse eUniverse, EAccountType eAccountType )
	{
		m_steamid.m_comp.m_unAccountID = ( ulIdentifier & k_unSteamAccountIDMask );
		m_steamid.m_comp.m_unAccountInstance = ( ( ulIdentifier >> 32 ) & k_unSteamAccountInstanceMask );
		m_steamid.m_comp.m_EUniverse = eUniverse;
		m_steamid.m_comp.m_EAccountType = eAccountType;
	}

	void SetFromUint64( uint64 ulSteamID )
	{
		m_steamid.m_unAll64Bits = ulSteamID;
	}

    void Clear()
	{
		m_steamid.m_comp.m_unAccountID = 0;
		m_steamid.m_comp.m_EAccountType = k_EAccountTypeInvalid;
		m_steamid.m_comp.m_EUniverse = k_EUniverseInvalid;
		m_steamid.m_comp.m_unAccountInstance = 0;
	}

	uint64 ConvertToUint64() const
	{
		return m_steamid.m_unAll64Bits;
	}

	uint64 GetStaticAccountKey() const
	{


		return (uint64) ( ( ( (uint64) m_steamid.m_comp.m_EUniverse ) << 56 ) + ((uint64) m_steamid.m_comp.m_EAccountType << 52 ) + m_steamid.m_comp.m_unAccountID );
	}

	void CreateBlankAnonLogon( EUniverse eUniverse )
	{
		m_steamid.m_comp.m_unAccountID = 0;
		m_steamid.m_comp.m_EAccountType = k_EAccountTypeAnonGameServer;
		m_steamid.m_comp.m_EUniverse = eUniverse;
		m_steamid.m_comp.m_unAccountInstance = 0;
	}

	void CreateBlankAnonUserLogon( EUniverse eUniverse )
	{
		m_steamid.m_comp.m_unAccountID = 0;
		m_steamid.m_comp.m_EAccountType = k_EAccountTypeAnonUser;
		m_steamid.m_comp.m_EUniverse = eUniverse;
		m_steamid.m_comp.m_unAccountInstance = 0;
	}

	bool BBlankAnonAccount() const
	{
		return m_steamid.m_comp.m_unAccountID == 0 && BAnonAccount() && m_steamid.m_comp.m_unAccountInstance == 0;
	}

	bool BGameServerAccount() const
	{
		return m_steamid.m_comp.m_EAccountType == k_EAccountTypeGameServer || m_steamid.m_comp.m_EAccountType == k_EAccountTypeAnonGameServer;
	}

	bool BPersistentGameServerAccount() const
	{
		return m_steamid.m_comp.m_EAccountType == k_EAccountTypeGameServer;
	}

	bool BAnonGameServerAccount() const
	{
		return m_steamid.m_comp.m_EAccountType == k_EAccountTypeAnonGameServer;
	}

	bool BContentServerAccount() const
	{
		return m_steamid.m_comp.m_EAccountType == k_EAccountTypeContentServer;
	}

	bool BClanAccount() const
	{
		return m_steamid.m_comp.m_EAccountType == k_EAccountTypeClan;
	}

	bool BChatAccount() const
	{
		return m_steamid.m_comp.m_EAccountType == k_EAccountTypeChat;
	}

	bool IsLobby() const
	{
		return ( m_steamid.m_comp.m_EAccountType == k_EAccountTypeChat )
			&& ( m_steamid.m_comp.m_unAccountInstance & k_EChatInstanceFlagLobby );
	}

	bool BIndividualAccount() const
	{
		return m_steamid.m_comp.m_EAccountType == k_EAccountTypeIndividual || m_steamid.m_comp.m_EAccountType == k_EAccountTypeConsoleUser;
	}

	bool BAnonAccount() const
	{
		return m_steamid.m_comp.m_EAccountType == k_EAccountTypeAnonUser || m_steamid.m_comp.m_EAccountType == k_EAccountTypeAnonGameServer;
	}

	bool BAnonUserAccount() const
	{
		return m_steamid.m_comp.m_EAccountType == k_EAccountTypeAnonUser;
	}

	bool BConsoleUserAccount() const
	{
		return m_steamid.m_comp.m_EAccountType == k_EAccountTypeConsoleUser;
	}


	void SetAccountID( uint32 unAccountID )		{ m_steamid.m_comp.m_unAccountID = unAccountID; }
	void SetAccountInstance( uint32 unInstance ){ m_steamid.m_comp.m_unAccountInstance = unInstance; }

	AccountID_t GetAccountID() const			{ return m_steamid.m_comp.m_unAccountID; }
	uint32 GetUnAccountInstance() const			{ return m_steamid.m_comp.m_unAccountInstance; }
	EAccountType GetEAccountType() const		{ return ( EAccountType ) m_steamid.m_comp.m_EAccountType; }
	EUniverse GetEUniverse() const				{ return m_steamid.m_comp.m_EUniverse; }
	void SetEUniverse( EUniverse eUniverse )	{ m_steamid.m_comp.m_EUniverse = eUniverse; }
	bool IsValid() const;


	explicit CSteamID( const char *pchSteamID, EUniverse eDefaultUniverse = k_EUniverseInvalid );
	const char * Render() const;
	static const char * Render( uint64 ulSteamID );

	void SetFromString( const char *pchSteamID, EUniverse eDefaultUniverse );

	bool SetFromStringStrict( const char *pchSteamID, EUniverse eDefaultUniverse );

	inline bool operator==( const CSteamID &val ) const { return m_steamid.m_unAll64Bits == val.m_steamid.m_unAll64Bits; }
	inline bool operator!=( const CSteamID &val ) const { return !operator==( val ); }
	inline bool operator<( const CSteamID &val ) const { return m_steamid.m_unAll64Bits < val.m_steamid.m_unAll64Bits; }
	inline bool operator>( const CSteamID &val ) const { return m_steamid.m_unAll64Bits > val.m_steamid.m_unAll64Bits; }


	bool BValidExternalSteamID() const;

private:

	CSteamID( uint32 );
	CSteamID( int32 );


	union SteamID_t
	{
		struct SteamIDComponent_t
		{
#ifdef VALVE_BIG_ENDIAN
			EUniverse			m_EUniverse : 8;
			unsigned int		m_EAccountType : 4;
			unsigned int		m_unAccountInstance : 20;
			uint32				m_unAccountID : 32;
#else
			uint32				m_unAccountID : 32;
			unsigned int		m_unAccountInstance : 20;
			unsigned int		m_EAccountType : 4;
			EUniverse			m_EUniverse : 8;
#endif
		} m_comp;

		uint64 m_unAll64Bits;
	} m_steamid;
};

inline bool CSteamID::IsValid() const
{
	if ( m_steamid.m_comp.m_EAccountType <= k_EAccountTypeInvalid || m_steamid.m_comp.m_EAccountType >= k_EAccountTypeMax )
		return false;

	if ( m_steamid.m_comp.m_EUniverse <= k_EUniverseInvalid || m_steamid.m_comp.m_EUniverse >= k_EUniverseMax )
		return false;

	if ( m_steamid.m_comp.m_EAccountType == k_EAccountTypeIndividual )
	{
		if ( m_steamid.m_comp.m_unAccountID == 0 || m_steamid.m_comp.m_unAccountInstance != k_unSteamUserDefaultInstance )
			return false;
	}

	if ( m_steamid.m_comp.m_EAccountType == k_EAccountTypeClan )
	{
		if ( m_steamid.m_comp.m_unAccountID == 0 || m_steamid.m_comp.m_unAccountInstance != 0 )
			return false;
	}

	if ( m_steamid.m_comp.m_EAccountType == k_EAccountTypeGameServer )
	{
		if ( m_steamid.m_comp.m_unAccountID == 0 )
			return false;

	}
	return true;
}

#if defined( INCLUDED_STEAM2_USERID_STRUCTS )

inline CSteamID SteamIDFromSteam2UserID( TSteamGlobalUserID *pTSteamGlobalUserID, EUniverse eUniverse )
{
	uint32 unAccountID = pTSteamGlobalUserID->m_SteamLocalUserID.Split.Low32bits * 2 +
		pTSteamGlobalUserID->m_SteamLocalUserID.Split.High32bits;

	return CSteamID( unAccountID, k_unSteamUserDefaultInstance, eUniverse, k_EAccountTypeIndividual );
}

bool SteamIDFromSteam2String( const char *pchSteam2ID, EUniverse eUniverse, CSteamID *pSteamIDOut );

inline TSteamGlobalUserID SteamIDToSteam2UserID( CSteamID steamID )
{
	TSteamGlobalUserID steamGlobalUserID;

	steamGlobalUserID.m_SteamInstanceID = 0;
	steamGlobalUserID.m_SteamLocalUserID.Split.High32bits = steamID.GetAccountID() % 2;
	steamGlobalUserID.m_SteamLocalUserID.Split.Low32bits = steamID.GetAccountID() / 2;

	return steamGlobalUserID;
}


#endif


#define k_steamIDNil CSteamID()

#define k_steamIDOutofDateGS CSteamID( 0, 0, k_EUniverseInvalid, k_EAccountTypeInvalid )

#define k_steamIDLanModeGS CSteamID( 0, 0, k_EUniversePublic, k_EAccountTypeInvalid )


#define k_steamIDNotInitYetGS CSteamID( 1, 0, k_EUniverseInvalid, k_EAccountTypeInvalid )


#define k_steamIDNonSteamGS CSteamID( 2, 0, k_EUniverseInvalid, k_EAccountTypeInvalid )


#ifdef STEAM


CSteamID ChatIDFromSteamID( const CSteamID &steamID );


CSteamID ClanIDFromSteamID( const CSteamID &steamID );

CSteamID ChatIDFromClanID( const CSteamID &steamIDClan );

CSteamID ClanIDFromChatID( const CSteamID &steamIDChat );

#endif

class CGameID
{
public:

	enum EGameIDType
	{
		k_EGameIDTypeApp		= 0,
		k_EGameIDTypeGameMod	= 1,
		k_EGameIDTypeShortcut	= 2,
		k_EGameIDTypeP2P		= 3,
	};

	CGameID()
	{
		m_gameID.m_nType = k_EGameIDTypeApp;
		m_gameID.m_nAppID = k_uAppIdInvalid;
		m_gameID.m_nModID = 0;
	}

	explicit CGameID( uint64 ulGameID )
	{
		m_ulGameID = ulGameID;
	}
#ifdef INT64_DIFFERENT_FROM_INT64_T
	CGameID( uint64_t ulGameID )
	{
		m_ulGameID = (uint64)ulGameID;
	}
#endif

	explicit CGameID( int32 nAppID )
	{
		m_ulGameID = 0;
		m_gameID.m_nAppID = nAppID;
	}

	explicit CGameID( uint32 nAppID )
	{
		m_ulGameID = 0;
		m_gameID.m_nAppID = nAppID;
	}


	explicit CGameID( uint32 nAppID, uint32 nModID, CGameID::EGameIDType nType )
	{
		m_gameID.m_nAppID = nAppID;
		m_gameID.m_nModID = nModID;
		m_gameID.m_nType = nType;
	}

	CGameID( const CGameID &that )
	{
		m_ulGameID = that.m_ulGameID;
	}

	CGameID& operator=( const CGameID & that )
	{
		m_ulGameID = that.m_ulGameID;
		return *this;
	}


	explicit CGameID( const char *pchGameID );
	const char *Render() const;
	static const char *Render( uint64 ulGameID );

	uint64 ToUint64() const
	{
		return m_ulGameID;
	}

	uint64 *GetUint64Ptr()
	{
		return &m_ulGameID;
	}

	void Set( uint64 ulGameID )
	{
		m_ulGameID = ulGameID;
	}

	bool IsMod() const
	{
		return ( m_gameID.m_nType == k_EGameIDTypeGameMod );
	}

	bool IsShortcut() const
	{
		return ( m_gameID.m_nType == k_EGameIDTypeShortcut );
	}

	bool IsP2PFile() const
	{
		return ( m_gameID.m_nType == k_EGameIDTypeP2P );
	}

	bool IsSteamApp() const
	{
		return ( m_gameID.m_nType == k_EGameIDTypeApp );
	}

	uint32 ModID() const
	{
		return m_gameID.m_nModID;
	}

#if !defined(VALVE_SHORTCUT_DEBUG)
	uint32 AppID( bool = false ) const
	{
		return m_gameID.m_nAppID;
	}
#else
	uint32 AppID( bool bShortcutOK = false ) const;
#endif

	bool operator == ( const CGameID &rhs ) const
	{
		return m_ulGameID == rhs.m_ulGameID;
	}

	bool operator != ( const CGameID &rhs ) const
	{
		return !(*this == rhs);
	}

	bool operator < ( const CGameID &rhs ) const
	{
		return ( m_ulGameID < rhs.m_ulGameID );
	}

	bool IsValid() const
	{

		switch( m_gameID.m_nType )
		{
		case k_EGameIDTypeApp:
			return m_gameID.m_nAppID != k_uAppIdInvalid;

		case k_EGameIDTypeGameMod:
			return m_gameID.m_nAppID != k_uAppIdInvalid && (m_gameID.m_nModID & 0x80000000);

		case k_EGameIDTypeShortcut:
			return m_gameID.m_nAppID == k_uAppIdInvalid
				&& (m_gameID.m_nModID & 0x80000000)
				&& m_gameID.m_nModID >= (5000 | 0x80000000);

		case k_EGameIDTypeP2P:
			return m_gameID.m_nAppID == k_uAppIdInvalid && (m_gameID.m_nModID & 0x80000000);

		default:
			return false;
		}

	}

	void Reset()
	{
		m_ulGameID = 0;
	}

	struct GameID_t
	{
#ifdef VALVE_BIG_ENDIAN
		unsigned int m_nModID : 32;
		unsigned int m_nType : 8;
		unsigned int m_nAppID : 24;
#else
		unsigned int m_nAppID : 24;
		unsigned int m_nType : 8;
		unsigned int m_nModID : 32;
#endif
	};

	union
	{
		uint64 m_ulGameID;
		GameID_t m_gameID;
	};

	friend CGameID GameIDFromAppAndModPath( uint32 nAppID, const char *pchModPath );
};

#pragma pack( pop )

const int k_cchGameExtraInfoMax = 64;

typedef void (*PFNPreMinidumpCallback)(void *context);

enum EGameSearchErrorCode_t
{
	k_EGameSearchErrorCode_OK = 1,
	k_EGameSearchErrorCode_Failed_Search_Already_In_Progress = 2,
	k_EGameSearchErrorCode_Failed_No_Search_In_Progress = 3,
	k_EGameSearchErrorCode_Failed_Not_Lobby_Leader = 4,
	k_EGameSearchErrorCode_Failed_No_Host_Available = 5,
	k_EGameSearchErrorCode_Failed_Search_Params_Invalid = 6,
	k_EGameSearchErrorCode_Failed_Offline = 7,
	k_EGameSearchErrorCode_Failed_NotAuthorized = 8,
	k_EGameSearchErrorCode_Failed_Unknown_Error = 9,
};

enum EPlayerResult_t
{
	k_EPlayerResultFailedToConnect = 1,
	k_EPlayerResultAbandoned = 2,
	k_EPlayerResultKicked = 3,
	k_EPlayerResultIncomplete = 4,
	k_EPlayerResultCompleted = 5,
};


enum ESteamIPv6ConnectivityProtocol
{
	k_ESteamIPv6ConnectivityProtocol_Invalid = 0,
	k_ESteamIPv6ConnectivityProtocol_HTTP = 1,
	k_ESteamIPv6ConnectivityProtocol_UDP = 2,
};


enum ESteamIPv6ConnectivityState
{
	k_ESteamIPv6ConnectivityState_Unknown = 0,
	k_ESteamIPv6ConnectivityState_Good = 1,
	k_ESteamIPv6ConnectivityState_Bad = 2,
};

#define VALVE_COMPILE_TIME_ASSERT( pred ) typedef char compile_time_assert_type[(pred) ? 1 : -1];

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)

#define VALVE_CALLBACK_PACK_SMALL
#else
#define VALVE_CALLBACK_PACK_LARGE
#endif

#if defined( VALVE_CALLBACK_PACK_SMALL )
#pragma pack( push, 4 )
#elif defined( VALVE_CALLBACK_PACK_LARGE )
#pragma pack( push, 8 )
#else
#error ???
#endif

typedef struct ValvePackingSentinel_t
{
    uint32 m_u32;
    uint64 m_u64;
    uint16 m_u16;
    double m_d;
} ValvePackingSentinel_t;

#pragma pack( pop )


#if defined(VALVE_CALLBACK_PACK_SMALL)
VALVE_COMPILE_TIME_ASSERT( sizeof(ValvePackingSentinel_t) == 24 )
#elif defined(VALVE_CALLBACK_PACK_LARGE)
VALVE_COMPILE_TIME_ASSERT( sizeof(ValvePackingSentinel_t) == 32 )
#else
#error ???
#endif

#endif
