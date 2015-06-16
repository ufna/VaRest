// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.
// Update to work with PlayFab 2015 Joshua Lyons

#pragma once

#include "VaRestPlayFabManager.generated.h"

// Enum for the UserDataPermission
UENUM(BlueprintType)
namespace EUserDataPermision
{
	enum Type
	{
		PRIVATE,
		PUBLIC
	};
}

// Enum for Region
UENUM(BlueprintType)
namespace ERegion
{
	enum Type
	{
		ANY,
		USCENTRAL,
		USEAST,
		EUWEST,
		SINGAPORE,
		JAPAN,
		BRAZIL,
		AUSTRALIA
	};
}

/**
 * Helper class to make PlayFab communication easier
 */
UCLASS(BlueprintType, Blueprintable)
class UVaRestPlayFabManager : public UVaRestRequestJSON
{
	GENERATED_UCLASS_BODY()

	/** Creates new PlayFab request with defined verb and content type */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Construct PlayFab Request", HidePin = "WorldContextObject", DefaultToSelf = "WorldContextObject"), Category = "VaRest|PlayFab")
	static UVaRestPlayFabManager* ConstructPlayFabRequest(UObject* WorldContextObject, ERequestVerb::Type Verb, ERequestContentType::Type ContentType);

	void ProcessPlayFabURL(const FString& PlayFabClass = "", const FString& PlayFabSessionToken = "", bool isAdmin = false,
		bool isServer = false, bool isClient = false, bool useSecretKey = false, bool useSessionTicket = false,
		bool cloudScript = false, FString cloudScriptVersion = "");

	// This will process the PlayFab Request
	UFUNCTION(BlueprintCallable, Category = "VaRest|PlayFab")
	void ProcessPlayFabRequest();

	/** Set PlayFab authenication data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|PlayFab")
	static void SetPlayFabAuthData(FString AppId, FString ApiKey);

	//////////////////////////////////////////////////////////////////////////
	// Play Fab Construction Helpers
	//////////////////////////////////////////////////////////////////////////

	//// Client

	// Authentication

	/** Create Json object that contains play fab login data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
	static UVaRestPlayFabManager* ConstructLoginWithPlayFab(FString Username, FString Password);

	/** Create Json object that contains play fab login data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
	static UVaRestPlayFabManager* ConstructRegisterWithPlayFab(FString Username, FString Password, FString Email);

	/** Create Get Photon Authentication Toeken Request */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
	static UVaRestPlayFabManager* ConstructGetPhotonAuthenticationToken(FString SessionTicket, bool PhotonRealtime, bool PhotonTurnbased, bool PhotonChat);

	/** Create a Login with Android Device Request */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
	static UVaRestPlayFabManager* ConstructLoginWithAndroidDeviceID(FString AndroidDeviceId, FString OSVersion = "", FString AndroidDeviceType = "", bool CreateAccount = false);

	/** Create Login with email request */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
	static UVaRestPlayFabManager* ConstructLoginWithEmail(FString Username, FString Password);

	/** Create a Login with Facebook request */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
	static UVaRestPlayFabManager* ConstructLoginWithFacebook(FString FacebookAccessToken, bool CreateAccount = false);

	/** Create a Login with Google request */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
	static UVaRestPlayFabManager* ConstructLoginWithGoogle(FString GoogleAccessToken, bool CreateAccount = false);

	/** Create a Login with IOS Device Request */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
	static UVaRestPlayFabManager* ConstructLoginWithIOSDeviceID(FString IOSDeviceId, FString OSVersion = "", FString DeviceModel = "", bool CreateAccount = false);

	/** Create a Login with Steam request */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
	static UVaRestPlayFabManager* ConstructLoginWithSteam(FString SteamAccessToken, bool CreateAccount = false);

	// Account Management

	/** Add Username Password Request */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructAddUsernamePassword(FString SessionTicket, FString Email, FString Username, FString Password);

	/** Get Account Info Request */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructGetAccountInfo(FString SessionTicket, FString PlayFabId, FString Username = "", FString Email = "", FString TitleDisplayName = "");
	
	/** Get Play Fab Ids from Facebook Ids */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructGetPlayFabIDsFromFacebookIDs(FString SessionTicket, TArray<FString> FacebookIDs);

	/** Get User combined info all inputs are optional except the session ticket.*/
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
		static UVaRestPlayFabManager* ConstructGetUserCombinedInfo(FString SessionTicket, TArray<FString> UserDataKeys, 
		TArray<FString> ReadOnlyDataKey, FString PlayFabId = "", FString Username = "",
		FString Email = "", FString TitleDisplayName = "", bool GetAccountInfo = false, bool GetInventory = true, bool GetVirtualCurrency = true, 
		bool GetUserData = true, bool GetReadOnlyData = true);

	/** Create Link android device ID */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructLinkAndroidDeviceID(FString SessionTicket, FString AndroidDeviceId, FString OSVersion = "", FString AndroidDeviceType = "");

	/** Create Link FacebookAccount */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructLinkFacebookAccount(FString SessionTicket, FString FacebookAccessToken);

	/** Create Link GameCenterAccount */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructLinkGameCenterAccount(FString SessionTicket, FString GameCenterId);

	/** Create Link GoogleAccount */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructLinkGoogleAccount(FString SessionTicket, FString GoogleAccessToken);

	/** Create Link IOS device ID */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructLinkIOSDeviceID(FString SessionTicket, FString IOSDeviceId, FString OSVersion = "", FString IOSDeviceModel = "");

	/** Create Link SteamAccount */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructLinkSteamAccount(FString SessionTicket, FString SteamAccessToken);

	/** Create SendAccountRecoverEmail */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructSendAccountRecoverEmail(FString Email);

	/** Create UnLink android device ID */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructUnlinkAndroidDeviceID(FString SessionTicket, FString AndroidDeviceId);

	/** Create UnLink FacebookAccount */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructUnlinkFacebookAccount(FString SessionTicket);

	/** Create UnLink GameCenterAccount */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructUnlinkGameCenterAccount(FString SessionTicket);

	/** Create UnLink GoogleAccount */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructUnlinkGoogleAccount(FString SessionTicket);

	/** Create UnLink IOS device ID */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructUnlinkIOSDeviceID(FString SessionTicket, FString IOSDeviceId);

	/** Create UnLink SteamAccount */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructUnlinkSteamAccount(FString SessionTicket);

	/** Create UpdateUserTitleDisplayName */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructUpdateUserTitleDisplayName(FString SessionTicket, FString DisplayName);

	// Player Data Management

	/** Create GetFriendLeaderboard */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructGetFriendLeaderboard(FString SessionTicket, FString StatisticName, int32 MaxResultsCount, int32 StartPosition = 0);

	/** Create GetLeaderboard */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructGetLeaderboard(FString SessionTicket, FString StatisticName, int32 MaxResultsCount, int32 StartPosition = 0);

	/** Create GetLeaderboardAroundCurrentUser */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructGetLeaderboardAroundCurrentUser(FString SessionTicket, FString StatisticName, int32 MaxResultsCount);

	/** Get User Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructGetUserData(FString SessionTicket, TArray<FString> Keys, FString PlayFabId = "");

	/** Get User Publisher Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructGetUserPublisherData(FString SessionTicket, TArray<FString> Keys, FString PlayFabId = "");

	/** Get User Publisher Read Only Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructGetUserPublisherReadOnlyData(FString SessionTicket, TArray<FString> Keys, FString PlayFabId = "");

	/** Get User Read Only Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructGetUserReadOnlyData(FString SessionTicket, TArray<FString> Keys, FString PlayFabId = "");

	/** Get User Statistics */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructGetUserStatistics(FString SessionTicket);

	/** Update User Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructUpdateUserData(FString SessionTicket, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType);

	/** Update User Publisher Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructUpdateUserPublisherData(FString SessionTicket, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType);

	/** Update User Statistics */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructUpdateUserStatistics(FString SessionTicket, UVaRestJsonObject* Statistics);

	// Title-Wide Data Management

	/** Get Catalog Items */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Title-Wide Data Management")
		static UVaRestPlayFabManager* ConstructGetCatalogItems(FString SessionTicket, FString CatalogVersion);

	/** Get Store Items */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Title-Wide Data Management")
		static UVaRestPlayFabManager* ConstructGetStoreItems(FString SessionTicket, FString StoreId);

	/** Get Title Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Title-Wide Data Management")
		static UVaRestPlayFabManager* ConstructGetTitleData(FString SessionTicket, TArray<FString> Keys);

	/** Get Title News */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Title-Wide Data Management")
		static UVaRestPlayFabManager* ConstructGetTitleNews(FString SessionTicket, int32 NumberofEntries);

	// Player Item Management

	/** Add User Virtual Currency */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Management")
		static UVaRestPlayFabManager* ConstructAddUserVirtualCurrency(FString SessionTicket, FString VirtualCurrency, int32 Amount);

	/** Consume Item */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructConsumeItem(FString SessionTicket, FString ItemInstanceId, int32 ConsumeCount);

	/** Get Character Inventory */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructGetCharacterInventory(FString SessionTicket, FString PlayFabId, FString CharacterId, FString CatalogVersion = "");

	/** Get User Inventory */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructGetUserInventory(FString SessionTicket);

	/** Redeem Coupon */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructRedeemCoupon(FString SessionTicket, FString CouponCode, FString CatalogVersion = "");

	/** Get Report Player */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructReportPlayer(FString SessionTicket, FString ReportedId, FString Reason);

	/** Subtract User Virtual Currency */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructSubtractUserVirtualCurrency(FString SessionTicket, FString VirtualCurrency, int32 Amount);

	/** Unlock Container Item */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructUnlockContainerItem(FString SessionTicket, FString ContainerItemId, FString CatalogVersion = "");

	/** Start Purchase */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructStartPurchase(FString SessionTicket, UVaRestJsonObject* Items, FString CatalogVersion = "", FString StoreId = "");

	/** Pay For Purchase */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructPayForPurchase(FString SessionTicket, FString OrderId, FString ProviderName, FString Currency);

	/** Confirm Purchase */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructConfirmPurchase(FString SessionTicket, FString OrderId);

	/** PurchaseItem */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructPurchaseItem(FString SessionTicket, FString ItemId, FString VirtualCurrency, int32 Price, FString CatalogVersion = "", FString StoreId = "");

	// Friend List Managaement

	/** Add Friend */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Friend List Management")
		static UVaRestPlayFabManager* ConstructAddFriend(FString SessionTicket, FString FriendPlayFabId = "", FString FriendUsername = "",
			FString FriendEmail = "", FString FriendTitleDisplayName = "");

	/** Get Friends List */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Friend List Management")
		static UVaRestPlayFabManager* ConstructGetFriendsList(FString SessionTicket, bool IncludeSteamFriends = true, bool IncludeFacebookFriends = true);

	/** Remove Friend */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Friend List Management")
		static UVaRestPlayFabManager* ConstructRemoveFriend(FString SessionTicket, FString FriendPlayFabId);

	/** Set Friend Tags */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Friend List Management")
		static UVaRestPlayFabManager* ConstructSetFriendTags(FString SessionTicket, FString FriendPlayFabId, TArray<FString> Tags);

	// IOS-Specific APIs

	/** Register For IOS Push Notifications */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | IOS-Specific")
		static UVaRestPlayFabManager* ConstructRegisterFORIOSPushNotifications(FString SessionTicket, FString DeviceToken, bool SendPushNotificationConfirmation = false, FString ConfirmationMessage = "");

	/** Restore IOS Purchases */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | IOS-Specific")
		static UVaRestPlayFabManager* ConstructRestoreIOSPurchases(FString SessionTicket, FString Base64ReceiptData);

	/** Validate IOS Reciept */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | IOS-Specific")
		static UVaRestPlayFabManager* ConstructValidateIOSReceipt(FString SessionTicket, FString Base64ReceiptData, FString CurrencyCode, FString PurchasePrice);

	// Matchmaking APIs

	/** Get Current Games */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Matchmaking")
		static UVaRestPlayFabManager* ConstructGetCurrentGames(FString SessionTicket, ERegion::Type Region, FString BuildVersion = "", FString GameMode = "", FString StatisticName = "");

	/** Get Game Server Regions */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Matchmaking")
		static UVaRestPlayFabManager* ConstructGetGameServerRegions(FString SessionTicket, FString BuildVersion);

	/** Matchmake */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Matchmaking")
		static UVaRestPlayFabManager* ConstructMatchmake(FString SessionTicket, ERegion::Type Region, FString BuildVersion = "", FString GameMode = "", 
			FString LobbyId = "", FString StatisticName = "", FString CharacterId = "");

	/** Start Game */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Matchmaking")
		static UVaRestPlayFabManager* ConstructStartGame(FString SessionTicket, FString BuildVersion, ERegion::Type Region, FString GameMode, FString StatisticName = "",
			FString CharacterId = "", bool PasswordRestricted = false, FString CustomCommandLineData = "");

	// Android-Specific APIs

	/** Android Device Push Notification Registration */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Android-Specific")
		static UVaRestPlayFabManager* ConstructAndroidDevicePushNotificationRegistration(FString SessionTicket, FString DeviceToken, bool SendPushNotificationConfirmation = false, 
			FString ConfirmationMessage = "");

	/** Validate Google Play Purchase */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Android-Specific")
		static UVaRestPlayFabManager* ConstructValidateGooglePlayPurchase(FString SessionTicket, FString ReceiptJsonString, FString Signature);

	// Analytics

	/** Log Event */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Analytics")
		static UVaRestPlayFabManager* ConstructLogEvent(FString SessionTicket, FString EventName, UVaRestJsonObject* Body, bool ProfileSetEvent = false);

	// Shared Group Data

	/** Add Shared Group Memebers */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Shared Group Data")
		static UVaRestPlayFabManager* ConstructAddSharedGroupMembers(FString SessionTicket, FString SharedGroupId, TArray<FString> PlayFabIds);

	/** Create Shared Group */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Shared Group Data")
		static UVaRestPlayFabManager* ConstructCreateSharedGroup(FString SessionTicket, FString SharedGroupId);

	/** Get Publisher Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Shared Group Data")
		static UVaRestPlayFabManager* ConstructGetPublisherData(FString SessionTicket, TArray<FString> Keys);

	/** Get Shared Group Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Shared Group Data")
		static UVaRestPlayFabManager* ConstructGetSharedGroupData(FString SessionTicket, FString SharedGroupId, TArray<FString> Keys, bool GetMembers = true);

	/** Remove Shared Group Memebers */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Shared Group Data")
		static UVaRestPlayFabManager* ConstructRemoveSharedGroupMembers(FString SessionTicket, FString SharedGroupId, TArray<FString> PlayFabIds);

	/** Update Shared Group Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Shared Group Data")
		static UVaRestPlayFabManager* ConstructUpdateSharedGroupData(FString SessionTicket, FString SharedGroupId, UVaRestJsonObject* Data, EUserDataPermision::Type Permission);

	// Sony Specific APIs
	
	/** RefreshPSNAuthToken */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Sony Specific")
		static UVaRestPlayFabManager* ConstructRefreshPSNAuthToken(FString SessionTicket, FString PSNAuthCode);

	// Server-Side Cloud Script

	/** Get Cloud Script Url */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Server-Side Cloud Script")
		static UVaRestPlayFabManager* ConstructGetCloudScriptUrl(FString SessionTicket, int32 Version, bool Testing);

	/** Run Cloud Script */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Server-Side Cloud Script")
		static UVaRestPlayFabManager* ConstructRunCloudScript(FString SessionTicket, FString ActionId, UVaRestJsonObject* Params, FString ParamsEncoded = "");

	// Content

	/** Get Content Download Url */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Content")
		static UVaRestPlayFabManager* ConstructGetContentDownloadUrl(FString SessionTicket, FString Key, FString HttpMethod = "", bool ThruCDN = true);

	// Characters

	/** Get Character Leaderboard */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Characters")
		static UVaRestPlayFabManager* ConstructGetCharacterLeaderboard(FString SessionTicket, FString StatisticName, int32 MaxResultsCount, 
			FString CharacterType = "", int32 StartPosition = 0);

	/** Get Leaderboard Around Character */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Characters")
		static UVaRestPlayFabManager* ConstructGetLeaderboardAroundCharacter(FString SessionTicket, FString CharacterId, FString StatisticName, int32 MaxResultsCount,
		FString CharacterType = "");

	/** Get Leaderboard For User Character */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Characters")
		static UVaRestPlayFabManager* ConstructGetLeaderboardForUserCharacters(FString SessionTicket, FString StatisticName, int32 MaxResultsCount);

	/** Grant Character To User */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Characters")
		static UVaRestPlayFabManager* ConstructGrantCharacterToUser(FString SessionTicket, FString ItemId, FString CharacterName, FString CatalogVersion = "");

	// Character Data

	/** Get Character Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Character Data")
		static UVaRestPlayFabManager* ConstructGetCharacterData(FString SessionTicket, FString PlayFabId, FString CharacterId, TArray<FString> Keys);

	/** Get Character ReadOnly Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Character Data")
		static UVaRestPlayFabManager* ConstructGetCharacterReadOnlyData(FString SessionTicket, FString PlayFabId, FString CharacterId, TArray<FString> Keys);

	/** Update Character Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Character Data")
		static UVaRestPlayFabManager* ConstructUpdateCharacterData(FString SessionTicket, FString CharacterId, UVaRestJsonObject* Data, EUserDataPermision::Type Permission);

	/// Server

	// Authentication

	/** Create Json object that contains authentication request object */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Authentication")
	static UVaRestPlayFabManager* ConstructAuthenticateSessionTicket(FString SessionTicket);

	// Account Management

	/** Get User Account Info */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Account Management")
		static UVaRestPlayFabManager* ConstructGetUserAccountInfo(FString PlayFabId);

	/** Send Push Notification */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Account Management")
		static UVaRestPlayFabManager* ConstructSendPushNotification(FString RecipientPlayFabId, FString Message, FString Subject = "");

	// Player Data Management

	/** Create GetLeaderboard */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerGetLeaderboard(FString StatisticName, int32 MaxResultsCount, int32 StartPosition = 0);

	/** Create GetLeaderboardAroundUser */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructGetLeaderboardAroundUser(FString StatisticName, FString PlayFabId, int32 MaxResultsCount);

	/** Get User Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerGetUserData(TArray<FString> Keys, FString PlayFabId);

	/** Get User Internal Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerGetUserInternalData(TArray<FString> Keys, FString PlayFabId);

	/** Get User Publisher Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerGetUserPublisherData(TArray<FString> Keys, FString PlayFabId);

	/** Get User Publisher Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerGetUserPublisherInternalData(TArray<FString> Keys, FString PlayFabId);

	/** Get User Publisher Read Only Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerGetUserPublisherReadOnlyData(TArray<FString> Keys, FString PlayFabId);

	/** Get User Read Only Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerGetUserReadOnlyData(TArray<FString> Keys, FString PlayFabId);

	/** Get User Statistics */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerGetUserStatistics(FString PlayFabId);

	/** Update User Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerUpdateUserData(FString PlayFabId, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType);

	/** Update User Internal Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerUpdateUserInternalData(FString PlayFabId, UVaRestJsonObject* Data);

	/** Update User Publisher Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerUpdateUserPublisherData(FString PlayFabId, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType);

	/** Update User Publisher Internal Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerUpdateUserPublisherInternalData(FString PlayFabId, UVaRestJsonObject* Data);

	/** Update User Publisher ReadOnly Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerUpdateUserPublisherReadOnlyData(FString PlayFabId, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType);

	/** Update User ReadOnly Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerUpdateUserReadOnlyData(FString PlayFabId, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType);

	/** Update User Statistics */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerUpdateUserStatistics(FString PlayFabId, UVaRestJsonObject* Statistics);

	// Title-Wide Data Management

	/** Get Catalog Items */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Title-Wide Data Management")
		static UVaRestPlayFabManager* ConstructServerGetCatalogItems(FString CatalogVersion);

	/** Get Title Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Title-Wide Data Management")
		static UVaRestPlayFabManager* ConstructServerGetTitleData(TArray<FString> Keys);

	/** Get Title Internal Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Title-Wide Data Management")
		static UVaRestPlayFabManager* ConstructServerGetTitleInternalData(TArray<FString> Keys);

	/** Set Title Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Title-Wide Data Management")
		static UVaRestPlayFabManager* ConstructServerSetTitleData(FString Key, FString Value);

	/** Get Title nternal Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Title-Wide Data Management")
		static UVaRestPlayFabManager* ConstructServerSetTitleInternalData(FString Key, FString Value);

	// Player Item Management

	/** Add Character Virtual Currency */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerAddCharacterVirtualCurrency(FString PlayFabId, FString CharacterId, FString VirtualCurrency, int32 Amount);

	/** Add User Virtual Currency */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerAddUserVirtualCurrency(FString PlayFabId, FString VirtualCurrency, int32 Amount);

	/** Get Character Inventory */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerGetCharacterInventory(FString PlayFabId, FString CharacterId, FString CatalogVersion = "");

	/** Get User Inventory */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerGetUserInventory(FString PlayFabId, FString CatalogVersion = "");

	/** Grant Items To Character */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerGrantItemsToCharacter(FString PlayFabId, FString CharacterId, TArray<FString> ItemIds, FString CatalogVersion = "", 
		FString Annotation = "");

	/** Grant Items To User */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerGrantItemsToUser(FString PlayFabId, TArray<FString> ItemIds, FString CatalogVersion = "",
		FString Annotation = "");

	/** Grant Items To Users */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerGrantItemsToUsers(UVaRestJsonObject* ItemGrants, FString CatalogVersion = "");

	/** Modify Item Uses */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerModifyItemUses(FString PlayFabId, FString ItemInstanceId, int32 UsesToAdd);

	/** Move Item To Character From Character */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerMoveItemToCharacterFromCharacter(FString PlayFabId, FString GivingCharacterId, 
			FString ReceivingCharacterId, FString ItemInstanceId);

	/** Move Item To Character From User */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerMoveItemToCharacterFromUser(FString PlayFabId, FString CharacterId,
		FString ItemInstanceId);

	/** Move Item To User From Character */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerMoveItemToUserFromCharacter(FString PlayFabId, FString CharacterId,
		FString ItemInstanceId);

	/** Report Player */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerReportPlayer(FString ReporterPlayFabId, FString ReporteePlayFabId, 
			FString TitleId = "", FString Comment = "");

	/** Subtract Character Virtual Currency */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerSubtractCharacterVirtualCurrency(FString PlayFabId, FString CharacterId, FString VirtualCurrency, int32 Amount);

	/** Subtract User Virtual Currency */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerSubtractUserVirtualCurrency(FString PlayFabId, FString VirtualCurrency, int32 Amount);

	/** Update User Inventory Item Custom Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerUpdateUserInventoryItemCustomData(FString PlayFabId, FString ItemInstanceId, 
			UVaRestJsonObject* Data, FString CharacterId = "");
	
	// Matchmaking

	/** Notify Matchmaker Player Left */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Matchmaking")
		static UVaRestPlayFabManager* ConstructServerNotifyMatchmakerPlayerLeft(FString PlayFabId, FString LobbyId);

	/** Redeem Matchmaker Ticket */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Matchmaking")
		static UVaRestPlayFabManager* ConstructServerRedeemMatchmakerTicket(FString Ticket, FString LobbyId);

	// Steam-Specific

	/** Award Steam Achievement */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Steam-Specific")
		static UVaRestPlayFabManager* ConstructServerAwardSteamAchievement(UVaRestJsonObject* Achievements);

	// Analytics

	/** Log Event */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Analytics")
		static UVaRestPlayFabManager* ConstructServerLogEvent(FString EventName, UVaRestJsonObject* Body, FString PlayFabId = "", 
			FString EntityId = "", FString EntityType = "", bool PlayerEvent = true, bool ProfileSetEvent = false);

	// Shared Group Data

	/** Add Shared Group Memebers */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Shared Group Data")
		static UVaRestPlayFabManager* ConstructServerAddSharedGroupMembers(FString SharedGroupId, TArray<FString> PlayFabIds);

	/** Create Shared Group */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Shared Group Data")
		static UVaRestPlayFabManager* ConstructServerCreateSharedGroup(FString SharedGroupId);

	/** Delete Shared Group */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Shared Group Data")
		static UVaRestPlayFabManager* ConstructServerDeleteSharedGroup(FString SharedGroupId);

	/** Get Publisher Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Shared Group Data")
		static UVaRestPlayFabManager* ConstructServerGetPublisherData(TArray<FString> Keys);

	/** Get Shared Group Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Shared Group Data")
		static UVaRestPlayFabManager* ConstructServerGetSharedGroupData(FString SharedGroupId, TArray<FString> Keys, bool GetMembers = true);

	/** Remove Shared Group Memebers */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Shared Group Data")
		static UVaRestPlayFabManager* ConstructServerRemoveSharedGroupMembers(FString SharedGroupId, TArray<FString> PlayFabIds);

	/** Set Publisher Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Shared Group Data")
		static UVaRestPlayFabManager* ConstructServerSetPublisherData(FString Key, FString Value);

	/** Update Shared Group Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Shared Group Data")
		static UVaRestPlayFabManager* ConstructServerUpdateSharedGroupData(FString SharedGroupId, UVaRestJsonObject* Data, EUserDataPermision::Type Permission);
	
	// Content

	/** Get Content Download Url */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Content")
		static UVaRestPlayFabManager* ConstructServerGetContentDownloadUrl(FString Key, FString HttpMethod = "", bool ThruCDN = true);

	// Characters

	/** Delete Character From User */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Characters")
		static UVaRestPlayFabManager* ConstructServerDeleteCharacterFromUser(FString PlayFabId, FString CharacterId, bool SaveCharacterInventory = false);

	/** Get All Users Characters */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Characters")
		static UVaRestPlayFabManager* ConstructServerGetAllUsersCharacters(FString PlayFabId);

	/** Get Character Leaderboard */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Characters")
		static UVaRestPlayFabManager* ConstructServerGetCharacterLeaderboard(FString CharacterId, FString StatisticName, int32 MaxResultsCount,
		FString CharacterType = "", int32 StartPosition = 0);

	/** Get Character Statistics */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Characters")
		static UVaRestPlayFabManager* ConstructServerGetCharacterStatistics(FString PlayFabId, FString CharacterId);

	/** Get Leaderboard Around Character */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Characters")
		static UVaRestPlayFabManager* ConstructServerGetLeaderboardAroundCharacter(FString PlayFabId, FString CharacterId, FString StatisticName, int32 MaxResultsCount,
		FString CharacterType = "");

	/** Get Leaderboard For User Character */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Characters")
		static UVaRestPlayFabManager* ConstructServerGetLeaderboardForUserCharacters(FString PlayFabId, FString StatisticName, int32 MaxResultsCount);

	/** Grant Character To User */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Characters")
		static UVaRestPlayFabManager* ConstructServerGrantCharacterToUser(FString PlayFabId, FString CharacterType, FString CharacterName);

	/** Update Character Statistics */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Characters")
		static UVaRestPlayFabManager* ConstructServerUpdateCharacterStatistics(FString PlayFabId, FString CharacterId, UVaRestJsonObject* CharacterStatistics);

	// Character Data

	/** Get Character Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Character Data")
		static UVaRestPlayFabManager* ConstructServerGetCharacterData(FString PlayFabId, FString CharacterId, TArray<FString> Keys, int32 IfChangedFromDataVersion = 0);

	/** Get Character Internal Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Character Data")
		static UVaRestPlayFabManager* ConstructServerGetCharacterInternalData(FString PlayFabId, FString CharacterId, TArray<FString> Keys, int32 IfChangedFromDataVersion = 0);

	/** Get Character ReadOnly Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Character Data")
		static UVaRestPlayFabManager* ConstructServerGetCharacterReadOnlyData(FString PlayFabId, FString CharacterId, TArray<FString> Keys, int32 IfChangedFromDataVersion = 0);

	/** Update Character Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Character Data")
		static UVaRestPlayFabManager* ConstructServerUpdateCharacterData(FString PlayFabId, FString CharacterId, UVaRestJsonObject* Data, EUserDataPermision::Type Permission);

	/** Update Character Internal Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Character Data")
		static UVaRestPlayFabManager* ConstructServerUpdateCharacterInternalData(FString PlayFabId, FString CharacterId, UVaRestJsonObject* Data, EUserDataPermision::Type Permission);

	/** Update Character ReadOnly Data */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Character Data")
		static UVaRestPlayFabManager* ConstructServerUpdateCharacterReadOnlyData(FString PlayFabId, FString CharacterId, UVaRestJsonObject* Data, EUserDataPermision::Type Permission);

	// Admin

	/** Create Json object that contains current version request request object */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Admin | CloudScript")
	static UVaRestPlayFabManager* ConstructGetCloudScriptRevision(FString Version, FString Revision);

	/** PlayFab Request Info */
	FString PlayFabClass;
	FString PlayFabSessionToken;
	FString CloudScriptVersion;
	bool isAdmin = false;
	bool isServer = false;
	bool isClient = false;
	bool useSecretKey = false;
	bool useSessionTicket = false;
	bool cloudScript = false;

	//////////////////////////////////////////////////////////////////////////
	// Internal data
private:
	/** PlayFab App Id */
	static FString PlayFabAppId;

	/** PlayFab Api Key */
	static FString PlayFabApiKey;
	static FString PhotonRealtimeAppId;
	static FString PhotonTurnbasedAppId;
	static FString PhotonChatAppId;

	/** PlayFab URL */
	static FString PlayFabURL;
	static FString PlayFabLogicURL;

	
};
