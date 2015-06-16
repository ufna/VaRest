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

	/** Signs the user into the PlayFab account, returning a session identifier that can subsequently be used for API calls which require an authenticated user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
		static UVaRestPlayFabManager* ConstructLoginWithPlayFab(FString Username, FString Password);

	/** Registers a new Playfab user account, returning a session identifier that can subsequently be used for API calls which require an authenticated user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
		static UVaRestPlayFabManager* ConstructRegisterWithPlayFab(FString Username, FString Password, FString Email);

	/** Gets a Photon custom authentication token that can be used to securely join the player into a Photon room */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
		static UVaRestPlayFabManager* ConstructGetPhotonAuthenticationToken(FString SessionTicket, bool PhotonRealtime, bool PhotonTurnbased, bool PhotonChat);

	/** Signs the user in using the Android device identifier, returning a session identifier that can subsequently be used for API calls which require an authenticated user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
		static UVaRestPlayFabManager* ConstructLoginWithAndroidDeviceID(FString AndroidDeviceId, FString OSVersion = "", FString AndroidDeviceType = "", bool CreateAccount = false);

	/** Signs the user into the PlayFab account, returning a session identifier that can subsequently be used for API calls which require an authenticated user */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
		static UVaRestPlayFabManager* ConstructLoginWithEmail(FString Username, FString Password);

	/** Signs the user in using a Facebook access token, returning a session identifier that can subsequently be used for API calls which require an authenticated user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
		static UVaRestPlayFabManager* ConstructLoginWithFacebook(FString FacebookAccessToken, bool CreateAccount = false);

	/** Signs the user in using a Google account access token, returning a session identifier that can subsequently be used for API calls which require an authenticated user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
		static UVaRestPlayFabManager* ConstructLoginWithGoogle(FString GoogleAccessToken, bool CreateAccount = false);

	/** Signs the user in using the vendor-specific iOS device identifier, returning a session identifier that can subsequently be used for API calls which require an authenticated user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
		static UVaRestPlayFabManager* ConstructLoginWithIOSDeviceID(FString IOSDeviceId, FString OSVersion = "", FString DeviceModel = "", bool CreateAccount = false);

	/** Signs the user in using a Steam authentication ticket, returning a session identifier that can subsequently be used for API calls which require an authenticated user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Authentication")
		static UVaRestPlayFabManager* ConstructLoginWithSteam(FString SteamAccessToken, bool CreateAccount = false);

	// Account Management

	/** Adds playfab username/password auth to an existing semi-anonymous account created via a 3rd party auth method.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
		static UVaRestPlayFabManager* ConstructAddUsernamePassword(FString SessionTicket, FString Email, FString Username, FString Password);

	/** Retrieves the user's PlayFab account details  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
		static UVaRestPlayFabManager* ConstructGetAccountInfo(FString SessionTicket, FString PlayFabId, FString Username = "", FString Email = "", FString TitleDisplayName = "");
	
	/** Retrieves the unique PlayFab identifiers for the given set of Facebook identifiers */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
		static UVaRestPlayFabManager* ConstructGetPlayFabIDsFromFacebookIDs(FString SessionTicket, TArray<FString> FacebookIDs);

	/** Retrieves the unique PlayFab identifiers for the given set of Game Center identifiers (referenced in the Game Center Programming Guide as the Player Identifier) */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
		static UVaRestPlayFabManager* ConstructGetPlayFabIDsFromGameCenterIDs(FString SessionTicket, TArray<FString> GameCenterIDs);

	/** Retrieves the unique PlayFab identifiers for the given set of PlayStation Network identifiers */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
		static UVaRestPlayFabManager* ConstructGetPlayFabIDsFromPSNAccountIDs(FString SessionTicket, TArray<FString> PSNAccountIDs);

	/** Retrieves the unique PlayFab identifiers for the given set of Steam identifiers. The Steam identifiers are the profile IDs for the user accounts, available as SteamId in 
	the Steamworks Community API calls */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
		static UVaRestPlayFabManager* ConstructGetPlayFabIDsFromSteamIDs(FString SessionTicket, TArray<FString> SteamIDs);

	/** Retrieves all requested data for a user in one unified request. By default, this API returns all data for the locally signed-in user. The input parameters may be used to 
	limit the data retrieved any any subset of the available data, as well as retrieve the available data for a different user. Note that certain data, including inventory, virtual 
	currency balances, and personally identifying information, may only be retrieved for the locally signed-in user. In the example below, a request is made for the account details, 
	virtual currency balances, and specified user data for the locally signed-in user */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
		static UVaRestPlayFabManager* ConstructGetUserCombinedInfo(FString SessionTicket, TArray<FString> UserDataKeys, 
		TArray<FString> ReadOnlyDataKey, FString PlayFabId = "", FString Username = "",
		FString Email = "", FString TitleDisplayName = "", bool GetAccountInfo = false, bool GetInventory = true, bool GetVirtualCurrency = true, 
		bool GetUserData = true, bool GetReadOnlyData = true);

	/** Links the Android device identifier to the user's PlayFab account  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructLinkAndroidDeviceID(FString SessionTicket, FString AndroidDeviceId, FString OSVersion = "", FString AndroidDeviceType = "");

	/** Links the Facebook account associated with the provided Facebook access token to the user's PlayFab account  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructLinkFacebookAccount(FString SessionTicket, FString FacebookAccessToken);

	/** Links the Game Center account associated with the provided Game Center ID to the user's PlayFab account  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructLinkGameCenterAccount(FString SessionTicket, FString GameCenterId);

	/** Links the currently signed-in user account to the Google account specified by the Google account access token  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructLinkGoogleAccount(FString SessionTicket, FString GoogleAccessToken);

	/** Links the vendor-specific iOS device identifier to the user's PlayFab account  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructLinkIOSDeviceID(FString SessionTicket, FString IOSDeviceId, FString OSVersion = "", FString IOSDeviceModel = "");

	/** Links the Steam account associated with the provided Steam authentication ticket to the user's PlayFab account  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructLinkSteamAccount(FString SessionTicket, FString SteamAccessToken);

	/** Forces an email to be sent to the registered email address for the user's account, with a link allowing the user to change the password  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructSendAccountRecoverEmail(FString Email);

	/** Unlinks the related Android device identifier from the user's PlayFab account  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructUnlinkAndroidDeviceID(FString SessionTicket, FString AndroidDeviceId);

	/** Unlinks the related Facebook account from the user's PlayFab account  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructUnlinkFacebookAccount(FString SessionTicket);

	/** Unlinks the related Game Center account from the user's PlayFab account  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructUnlinkGameCenterAccount(FString SessionTicket);

	/** Unlinks the related Google account from the user's PlayFab account  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructUnlinkGoogleAccount(FString SessionTicket);

	/** Unlinks the related iOS device identifier from the user's PlayFab account  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructUnlinkIOSDeviceID(FString SessionTicket, FString IOSDeviceId);

	/** Unlinks the related Steam account from the user's PlayFab account  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructUnlinkSteamAccount(FString SessionTicket);

	/** Updates the title specific display name for the user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Account Management")
	static UVaRestPlayFabManager* ConstructUpdateUserTitleDisplayName(FString SessionTicket, FString DisplayName);

	// Player Data Management

	/** Retrieves a list of ranked friends of the current player for the given statistic, starting from the indicated point in the leaderboard  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructGetFriendLeaderboard(FString SessionTicket, FString StatisticName, int32 MaxResultsCount, int32 StartPosition = 0);

	/** Retrieves a list of ranked users for the given statistic, starting from the indicated point in the leaderboard  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructGetLeaderboard(FString SessionTicket, FString StatisticName, int32 MaxResultsCount, int32 StartPosition = 0);

	/** Retrieves a list of ranked users for the given statistic, centered on the currently signed-in user */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructGetLeaderboardAroundCurrentUser(FString SessionTicket, FString StatisticName, int32 MaxResultsCount);

	/** Retrieves the title-specific custom data for the user which is readable and writable by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructGetUserData(FString SessionTicket, TArray<FString> Keys, FString PlayFabId = "");

	/** Retrieves the publisher-specific custom data for the user which is readable and writable by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructGetUserPublisherData(FString SessionTicket, TArray<FString> Keys, FString PlayFabId = "");

	/** Retrieves the publisher-specific custom data for the user which can only be read by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructGetUserPublisherReadOnlyData(FString SessionTicket, TArray<FString> Keys, FString PlayFabId = "");

	/** Retrieves the title-specific custom data for the user which can only be read by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructGetUserReadOnlyData(FString SessionTicket, TArray<FString> Keys, FString PlayFabId = "");

	/** Retrieves the details of all title-specific statistics for the user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructGetUserStatistics(FString SessionTicket);

	/** Creates and updates the title-specific custom data for the user which is readable and writable by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructUpdateUserData(FString SessionTicket, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType);

	/** Creates and updates the publisher-specific custom data for the user which is readable and writable by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructUpdateUserPublisherData(FString SessionTicket, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType);

	/** Updates the values of the specified title-specific statistics for the user */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Data Management")
	static UVaRestPlayFabManager* ConstructUpdateUserStatistics(FString SessionTicket, UVaRestJsonObject* Statistics);

	// Title-Wide Data Management

	/** Retrieves the specified version of the title's catalog of virtual goods, including all defined properties  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Title Wide Data Management")
		static UVaRestPlayFabManager* ConstructGetCatalogItems(FString SessionTicket, FString CatalogVersion);

	/** Retrieves the set of items defined for the specified store, including all prices defined  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Title Wide Data Management")
		static UVaRestPlayFabManager* ConstructGetStoreItems(FString SessionTicket, FString StoreId);

	/** Retrieves the key-value store of custom title settings  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Title Wide Data Management")
		static UVaRestPlayFabManager* ConstructGetTitleData(FString SessionTicket, TArray<FString> Keys);

	/** Retrieves the title news feed, as configured in the developer portal */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Title Wide Data Management")
		static UVaRestPlayFabManager* ConstructGetTitleNews(FString SessionTicket, int32 NumberofEntries);

	// Player Item Management

	/** Increments the user's balance of the specified virtual currency by the stated amount  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Management")
		static UVaRestPlayFabManager* ConstructAddUserVirtualCurrency(FString SessionTicket, FString VirtualCurrency, int32 Amount);

	/** Consume uses of a consumable item. When all uses are consumed, it will be removed from the player's inventory. */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructConsumeItem(FString SessionTicket, FString ItemInstanceId, int32 ConsumeCount);

	/** Retrieves the specified character's current inventory of virtual goods  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructGetCharacterInventory(FString SessionTicket, FString PlayFabId, FString CharacterId, FString CatalogVersion = "");

	/** Retrieves the user's current inventory of virtual goods  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructGetUserInventory(FString SessionTicket);

	/** Adds the virtual goods associated with the coupon to the user's inventory  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructRedeemCoupon(FString SessionTicket, FString CouponCode, FString CatalogVersion = "");

	/** Submit a report for another player (due to bad bahavior, etc.), so that customer service representatives for the title can take 
	action concerning potentially toxic players.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructReportPlayer(FString SessionTicket, FString ReportedId, FString Reason);

	/** Decrements the user's balance of the specified virtual currency by the stated amount  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructSubtractUserVirtualCurrency(FString SessionTicket, FString VirtualCurrency, int32 Amount);

	/** Unlocks a container item in the user's inventory and consumes a key item of the type indicated by the container item  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructUnlockContainerItem(FString SessionTicket, FString ContainerItemId, FString CatalogVersion = "");

	/** Creates an order for a list of items from the title catalog  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructStartPurchase(FString SessionTicket, UVaRestJsonObject* Items, FString CatalogVersion = "", FString StoreId = "");

	/** Selects a payment option for purchase order created via StartPurchase  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructPayForPurchase(FString SessionTicket, FString OrderId, FString ProviderName, FString Currency);

	/** Confirms with the payment provider that the purchase was approved (if applicable) and adjusts inventory and virtual currency balances as appropriate  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructConfirmPurchase(FString SessionTicket, FString OrderId);

	/** Buys a single item with virtual currency. You must specify both the virtual currency to use to purchase, as well as what the client believes the price to be. 
	This lets the server fail the purchase if the price has changed.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Player Item Data Management")
		static UVaRestPlayFabManager* ConstructPurchaseItem(FString SessionTicket, FString ItemId, FString VirtualCurrency, int32 Price, FString CatalogVersion = "", FString StoreId = "");

	// Friend List Managaement

	/** Adds the PlayFab user, based upon a match against a supplied unique identifier, to the friend list of the local user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Friend List Management")
		static UVaRestPlayFabManager* ConstructAddFriend(FString SessionTicket, FString FriendPlayFabId = "", FString FriendUsername = "",
			FString FriendEmail = "", FString FriendTitleDisplayName = "");

	/** Retrieves the current friend list for the local user, constrained to users who have PlayFab accounts. Friends from linked accounts (Facebook, Steam) are also included. 
	You may optionally exclude some linked services' friends.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Friend List Management")
		static UVaRestPlayFabManager* ConstructGetFriendsList(FString SessionTicket, bool IncludeSteamFriends = true, bool IncludeFacebookFriends = true);

	/** Removes a specified user from the friend list of the local user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Friend List Management")
		static UVaRestPlayFabManager* ConstructRemoveFriend(FString SessionTicket, FString FriendPlayFabId);

	/** Updates the tag list for a specified user in the friend list of the local user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Friend List Management")
		static UVaRestPlayFabManager* ConstructSetFriendTags(FString SessionTicket, FString FriendPlayFabId, TArray<FString> Tags);

	// IOS-Specific APIs

	/** Registers the iOS device to receive push notifications */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | IOS Specific")
		static UVaRestPlayFabManager* ConstructRegisterFORIOSPushNotifications(FString SessionTicket, FString DeviceToken, bool SendPushNotificationConfirmation = false, FString ConfirmationMessage = "");

	/** Restores all in-app purchases based on the given refresh receipt.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | IOS Specific")
		static UVaRestPlayFabManager* ConstructRestoreIOSPurchases(FString SessionTicket, FString Base64ReceiptData);

	/** Validates with the Apple store that the receipt for an iOS in-app purchase is valid and that it matches the purchased catalog item */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | IOS Specific")
		static UVaRestPlayFabManager* ConstructValidateIOSReceipt(FString SessionTicket, FString Base64ReceiptData, FString CurrencyCode, FString PurchasePrice);

	// Matchmaking APIs

	/** Get details about all current running game servers matching the given parameters.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Matchmaking")
		static UVaRestPlayFabManager* ConstructGetCurrentGames(FString SessionTicket, ERegion::Type Region, FString BuildVersion = "", FString GameMode = "", FString StatisticName = "");

	/** Get details about the regions hosting game servers matching the given parameters.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Matchmaking")
		static UVaRestPlayFabManager* ConstructGetGameServerRegions(FString SessionTicket, FString BuildVersion);

	/** Attempts to locate a game session matching the given parameters. Note that parameters specified in the search are required (they are not weighting factors). 
	If a slot is found in a server instance matching the parameters, the slot will be assigned to that player, removing it from the availabe set. In that case, the 
	information on the game session will be returned, otherwise the Status returned will be GameNotFound. Note that EnableQueue is deprecated at this time.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Matchmaking")
		static UVaRestPlayFabManager* ConstructMatchmake(FString SessionTicket, ERegion::Type Region, FString BuildVersion = "", FString GameMode = "", 
			FString LobbyId = "", FString StatisticName = "", FString CharacterId = "");

	/** Start a new game server with a given configuration, add the current player and return the connection information.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Matchmaking")
		static UVaRestPlayFabManager* ConstructStartGame(FString SessionTicket, FString BuildVersion, ERegion::Type Region, FString GameMode, FString StatisticName = "",
			FString CharacterId = "", bool PasswordRestricted = false, FString CustomCommandLineData = "");

	// Android-Specific APIs

	/** Registers the Android device to receive push notifications  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Android Specific")
		static UVaRestPlayFabManager* ConstructAndroidDevicePushNotificationRegistration(FString SessionTicket, FString DeviceToken, bool SendPushNotificationConfirmation = false, 
			FString ConfirmationMessage = "");

	/** Validates a Google Play purchase and gives the corresponding item to the player.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Android Specific")
		static UVaRestPlayFabManager* ConstructValidateGooglePlayPurchase(FString SessionTicket, FString ReceiptJsonString, FString Signature);

	// Analytics

	/** Logs a custom analytics event  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Analytics")
		static UVaRestPlayFabManager* ConstructLogEvent(FString SessionTicket, FString EventName, UVaRestJsonObject* Body, bool ProfileSetEvent = false);

	// Shared Group Data

	/** Adds users to the set of those able to update both the shared data, as well as the set of users in the group. Only users in the group can add new members.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Shared Group Data")
		static UVaRestPlayFabManager* ConstructAddSharedGroupMembers(FString SessionTicket, FString SharedGroupId, TArray<FString> PlayFabIds);

	/** Requests the creation of a shared group object, containing key/value pairs which may be updated by all members of the group. Upon creation, 
	the current user will be the only member of the group.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Shared Group Data")
		static UVaRestPlayFabManager* ConstructCreateSharedGroup(FString SessionTicket, FString SharedGroupId);

	/** Retrieves the key-value store of custom publisher settings  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Shared Group Data")
		static UVaRestPlayFabManager* ConstructGetPublisherData(FString SessionTicket, TArray<FString> Keys);

	/** Retrieves data stored in a shared group object, as well as the list of members in the group. Non-members of the group may use this to retrieve group data, 
	including membership, but they will not receive data for keys marked as private.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Shared Group Data")
		static UVaRestPlayFabManager* ConstructGetSharedGroupData(FString SessionTicket, FString SharedGroupId, TArray<FString> Keys, bool GetMembers = true);

	/** Removes users from the set of those able to update the shared data and the set of users in the group. Only users in the group can remove members. 
	If as a result of the call, zero users remain with access, the group and its associated data will be deleted.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Shared Group Data")
		static UVaRestPlayFabManager* ConstructRemoveSharedGroupMembers(FString SessionTicket, FString SharedGroupId, TArray<FString> PlayFabIds);

	/** Adds, updates, and removes data keys for a shared group object. If the permission is set to Public, all fields updated or added in this call will be readable 
	by users not in the group. By default, data permissions are set to Private. Regardless of the permission setting, only members of the group can update the data.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Shared Group Data")
		static UVaRestPlayFabManager* ConstructUpdateSharedGroupData(FString SessionTicket, FString SharedGroupId, UVaRestJsonObject* Data, EUserDataPermision::Type Permission);

	// Sony Specific APIs
	
	/** Uses the supplied OAuth code to refresh the internally cached player PSN auth token  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Sony Specific")
		static UVaRestPlayFabManager* ConstructRefreshPSNAuthToken(FString SessionTicket, FString PSNAuthCode);

	// Server-Side Cloud Script

	/** Retrieves the title-specific URL for Cloud Script servers. This must be queried once, prior to making any calls to RunCloudScript. */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Server Side Cloud Script")
		static UVaRestPlayFabManager* ConstructGetCloudScriptUrl(FString SessionTicket, int32 Version, bool Testing);

	/** Triggers a particular server action, passing the provided inputs to the hosted Cloud Script. An action in this context is a handler in the JavaScript. NOTE: Before calling this API, 
	you must call GetCloudScriptUrl to be assigned a Cloud Script server URL. When using an official PlayFab SDK, this URL is stored internally in the SDK, but GetCloudScriptUrl must still be manually called.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Server Side Cloud Script")
		static UVaRestPlayFabManager* ConstructRunCloudScript(FString SessionTicket, FString ActionId, UVaRestJsonObject* Params, FString ParamsEncoded = "");

	// Content

	/** Retrieves the pre-authorized URL for accessing a content file for the title. A subsequent HTTP GET to the returned URL downloads the content; or a HEAD query to the returned URL retrieves 
	the metadata of the content. This API only generates a pre-signed URL to access the content. A success result does not guarantee the existence of the content.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Content")
		static UVaRestPlayFabManager* ConstructGetContentDownloadUrl(FString SessionTicket, FString Key, FString HttpMethod = "", bool ThruCDN = true);

	// Characters

	/** Retrieves a list of ranked characters for the given statistic, starting from the indicated point in the leaderboard  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Characters")
		static UVaRestPlayFabManager* ConstructGetCharacterLeaderboard(FString SessionTicket, FString StatisticName, int32 MaxResultsCount, 
			FString CharacterType = "", int32 StartPosition = 0);

	/** Retrieves a list of ranked characters for the given statistic, centered on the currently signed-in user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Characters")
		static UVaRestPlayFabManager* ConstructGetLeaderboardAroundCharacter(FString SessionTicket, FString CharacterId, FString StatisticName, int32 MaxResultsCount,
		FString CharacterType = "");

	/** Retrieves a list of all of the user's characters for the given statistic.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Characters")
		static UVaRestPlayFabManager* ConstructGetLeaderboardForUserCharacters(FString SessionTicket, FString StatisticName, int32 MaxResultsCount);

	/** Grants the specified character type to the user.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Characters")
		static UVaRestPlayFabManager* ConstructGrantCharacterToUser(FString SessionTicket, FString ItemId, FString CharacterName, FString CatalogVersion = "");

	// Character Data

	/** Retrieves the title-specific custom data for the character which is readable and writable by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Character Data")
		static UVaRestPlayFabManager* ConstructGetCharacterData(FString SessionTicket, FString PlayFabId, FString CharacterId, TArray<FString> Keys);

	/** Retrieves the title-specific custom data for the character which can only be read by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Character Data")
		static UVaRestPlayFabManager* ConstructGetCharacterReadOnlyData(FString SessionTicket, FString PlayFabId, FString CharacterId, TArray<FString> Keys);

	/** Creates and updates the title-specific custom data for the user's character which is readable and writable by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Client | Character Data")
		static UVaRestPlayFabManager* ConstructUpdateCharacterData(FString SessionTicket, FString CharacterId, UVaRestJsonObject* Data, EUserDataPermision::Type Permission);

	/// Server

	// Authentication

	/** Validated a client's session ticket, and if successful, returns details for that user */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Authentication")
	static UVaRestPlayFabManager* ConstructAuthenticateSessionTicket(FString SessionTicket);

	// Account Management

	/** Retrieves the relevant details for a specified user */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Account Management")
		static UVaRestPlayFabManager* ConstructGetUserAccountInfo(FString PlayFabId);

	/** Sends an iOS/Android Push Notification to a specific user, if that user's device has 
	been configured for Push Notifications in PlayFab. If a user has linked both Android and iOS devices, both will be notified.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Account Management")
		static UVaRestPlayFabManager* ConstructSendPushNotification(FString RecipientPlayFabId, FString Message, FString Subject = "");

	// Player Data Management

	/** Retrieves a list of ranked users for the given statistic, starting from the indicated point in the leaderboard  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerGetLeaderboard(FString StatisticName, int32 MaxResultsCount, int32 StartPosition = 0);

	/** Retrieves a list of ranked users for the given statistic, centered on the currently signed-in user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructGetLeaderboardAroundUser(FString StatisticName, FString PlayFabId, int32 MaxResultsCount);

	/** Retrieves the title-specific custom data for the user which is readable and writable by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerGetUserData(TArray<FString> Keys, FString PlayFabId);

	/** Retrieves the title-specific custom data for the user which cannot be accessed by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerGetUserInternalData(TArray<FString> Keys, FString PlayFabId);

	/** Retrieves the publisher-specific custom data for the user which is readable and writable by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerGetUserPublisherData(TArray<FString> Keys, FString PlayFabId);

	/** Retrieves the publisher-specific custom data for the user which cannot be accessed by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerGetUserPublisherInternalData(TArray<FString> Keys, FString PlayFabId);

	/** Retrieves the publisher-specific custom data for the user which can only be read by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerGetUserPublisherReadOnlyData(TArray<FString> Keys, FString PlayFabId);

	/** Retrieves the title-specific custom data for the user which can only be read by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerGetUserReadOnlyData(TArray<FString> Keys, FString PlayFabId);

	/** Retrieves the details of all title-specific statistics for the user */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerGetUserStatistics(FString PlayFabId);

	/** Updates the title-specific custom data for the user which is readable and writable by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerUpdateUserData(FString PlayFabId, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType);

	/** Updates the title-specific custom data for the user which cannot be accessed by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerUpdateUserInternalData(FString PlayFabId, UVaRestJsonObject* Data);

	/** Updates the publisher-specific custom data for the user which is readable and writable by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerUpdateUserPublisherData(FString PlayFabId, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType);

	/** Updates the publisher-specific custom data for the user which cannot be accessed by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerUpdateUserPublisherInternalData(FString PlayFabId, UVaRestJsonObject* Data);

	/** Updates the publisher-specific custom data for the user which can only be read by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerUpdateUserPublisherReadOnlyData(FString PlayFabId, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType);

	/** Updates the title-specific custom data for the user which can only be read by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerUpdateUserReadOnlyData(FString PlayFabId, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType);

	/** Updates the values of the specified title-specific statistics for the user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Data Management")
		static UVaRestPlayFabManager* ConstructServerUpdateUserStatistics(FString PlayFabId, UVaRestJsonObject* Statistics);

	// Title-Wide Data Management

	/** Retrieves the specified version of the title's catalog of virtual goods, including all defined properties  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Title Wide Data Management")
		static UVaRestPlayFabManager* ConstructServerGetCatalogItems(FString CatalogVersion);

	/** Retrieves the key-value store of custom title settings  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Title Wide Data Management")
		static UVaRestPlayFabManager* ConstructServerGetTitleData(TArray<FString> Keys);

	/** Retrieves the key-value store of custom internal title settings  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Title Wide Data Management")
		static UVaRestPlayFabManager* ConstructServerGetTitleInternalData(TArray<FString> Keys);

	/** Updates the key-value store of custom title settings  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Title Wide Data Management")
		static UVaRestPlayFabManager* ConstructServerSetTitleData(FString Key, FString Value);

	/** Updates the key-value store of custom title settings  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Title Wide Data Management")
		static UVaRestPlayFabManager* ConstructServerSetTitleInternalData(FString Key, FString Value);

	// Player Item Management

	/** Increments the character's balance of the specified virtual currency by the stated amount  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerAddCharacterVirtualCurrency(FString PlayFabId, FString CharacterId, FString VirtualCurrency, int32 Amount);

	/** Increments the user's balance of the specified virtual currency by the stated amount */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerAddUserVirtualCurrency(FString PlayFabId, FString VirtualCurrency, int32 Amount);

	/** Retrieves the specified character's current inventory of virtual goods  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerGetCharacterInventory(FString PlayFabId, FString CharacterId, FString CatalogVersion = "");

	/** Retrieves the specified user's current inventory of virtual goods  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerGetUserInventory(FString PlayFabId, FString CatalogVersion = "");

	/** Adds the specified items to the specified character's inventory */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerGrantItemsToCharacter(FString PlayFabId, FString CharacterId, TArray<FString> ItemIds, FString CatalogVersion = "", 
		FString Annotation = "");

	/** Adds the specified items to the specified user's inventory  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerGrantItemsToUser(FString PlayFabId, TArray<FString> ItemIds, FString CatalogVersion = "",
		FString Annotation = "");

	/** Adds the specified items to the specified user inventories  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerGrantItemsToUsers(UVaRestJsonObject* ItemGrants, FString CatalogVersion = "");

	/** Modifies the number of remaining uses of a player's inventory item  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerModifyItemUses(FString PlayFabId, FString ItemInstanceId, int32 UsesToAdd);

	/** Moves an item from a character's inventory into another of the users's character's inventory.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerMoveItemToCharacterFromCharacter(FString PlayFabId, FString GivingCharacterId, 
			FString ReceivingCharacterId, FString ItemInstanceId);

	/** Moves an item from a user's inventory into their character's inventory. */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerMoveItemToCharacterFromUser(FString PlayFabId, FString CharacterId,
		FString ItemInstanceId);

	/** Moves an item from a character's inventory into the owning user's inventory.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerMoveItemToUserFromCharacter(FString PlayFabId, FString CharacterId,
		FString ItemInstanceId);

	/** Submit a report about a player (due to bad bahavior, etc.) on behalf of another player, so that customer service 
	representatives for the title can take action concerning potentially poxic players */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerReportPlayer(FString ReporterPlayFabId, FString ReporteePlayFabId, 
			FString TitleId = "", FString Comment = "");

	/** Decrements the character's balance of the specified virtual currency by the stated amount */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerSubtractCharacterVirtualCurrency(FString PlayFabId, FString CharacterId, FString VirtualCurrency, int32 Amount);

	/** Decrements the user's balance of the specified virtual currency by the stated amount  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerSubtractUserVirtualCurrency(FString PlayFabId, FString VirtualCurrency, int32 Amount);

	/** Updates the key-value pair data tagged to the specified item, which is read-only from the client.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Player Item Management")
		static UVaRestPlayFabManager* ConstructServerUpdateUserInventoryItemCustomData(FString PlayFabId, FString ItemInstanceId, 
			UVaRestJsonObject* Data, FString CharacterId = "");
	
	// Matchmaking

	/** Informs the PlayFab match-making service that the user specified has left the Game Server Instance  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Matchmaking")
		static UVaRestPlayFabManager* ConstructServerNotifyMatchmakerPlayerLeft(FString PlayFabId, FString LobbyId);

	/** Validates a Game Server session ticket and returns details about the user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Matchmaking")
		static UVaRestPlayFabManager* ConstructServerRedeemMatchmakerTicket(FString Ticket, FString LobbyId);

	// Steam-Specific

	/** Awards the specified users the specified Steam achievements  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Steam Specific")
		static UVaRestPlayFabManager* ConstructServerAwardSteamAchievement(UVaRestJsonObject* Achievements);

	// Analytics

	/** Logs a custom analytics event  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Analytics")
		static UVaRestPlayFabManager* ConstructServerLogEvent(FString EventName, UVaRestJsonObject* Body, FString PlayFabId = "", 
			FString EntityId = "", FString EntityType = "", bool PlayerEvent = true, bool ProfileSetEvent = false);

	// Shared Group Data

	/** Adds users to the set of those able to update both the shared data, as well as the set of users in the group. 
	Only users in the group (and the server) can add new members.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Shared Group Data")
		static UVaRestPlayFabManager* ConstructServerAddSharedGroupMembers(FString SharedGroupId, TArray<FString> PlayFabIds);

	/** Requests the creation of a shared group object, containing key/value pairs which may be updated by all members of 
	the group. When created by a server, the group will initially have no members.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Shared Group Data")
		static UVaRestPlayFabManager* ConstructServerCreateSharedGroup(FString SharedGroupId);

	/** Deletes a shared group, freeing up the shared group ID to be reused for a new group */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Shared Group Data")
		static UVaRestPlayFabManager* ConstructServerDeleteSharedGroup(FString SharedGroupId);

	/** Retrieves the key-value store of custom publisher settings */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Shared Group Data")
		static UVaRestPlayFabManager* ConstructServerGetPublisherData(TArray<FString> Keys);

	/** Retrieves data stored in a shared group object, as well as the list of members in the group. 
	The server can access all public and private group data.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Shared Group Data")
		static UVaRestPlayFabManager* ConstructServerGetSharedGroupData(FString SharedGroupId, TArray<FString> Keys, bool GetMembers = true);

	/** Removes users from the set of those able to update the shared data and the set of users in the group. Only users in the group can remove members. 
	If as a result of the call, zero users remain with access, the group and its associated data will be deleted.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Shared Group Data")
		static UVaRestPlayFabManager* ConstructServerRemoveSharedGroupMembers(FString SharedGroupId, TArray<FString> PlayFabIds);

	/** Updates the key-value store of custom publisher settings  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Shared Group Data")
		static UVaRestPlayFabManager* ConstructServerSetPublisherData(FString Key, FString Value);

	/** Adds, updates, and removes data keys for a shared group object. If the permission is set to Public, all fields updated or added in this call will be readable 
	by users not in the group. By default, data permissions are set to Private. Regardless of the permission setting, only members of the group (and the server) can 
	update the data. */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Shared Group Data")
		static UVaRestPlayFabManager* ConstructServerUpdateSharedGroupData(FString SharedGroupId, UVaRestJsonObject* Data, EUserDataPermision::Type Permission);
	
	// Content

	/** Retrieves the pre-authorized URL for accessing a content file for the title. A subsequent HTTP GET to the returned URL downloads the content; or a HEAD 
	query to the returned URL retrieves the metadata of the content. This API only generates a pre-signed URL to access the content. A success result does not 
	guarantee the existence of the content.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Content")
		static UVaRestPlayFabManager* ConstructServerGetContentDownloadUrl(FString Key, FString HttpMethod = "", bool ThruCDN = true);

	// Characters

	/** Deletes the specific character ID from the specified user.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Characters")
		static UVaRestPlayFabManager* ConstructServerDeleteCharacterFromUser(FString PlayFabId, FString CharacterId, bool SaveCharacterInventory = false);

	/** Lists all of the characters that belong to a specific user. */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Characters")
		static UVaRestPlayFabManager* ConstructServerGetAllUsersCharacters(FString PlayFabId);

	/** Retrieves a list of ranked characters for the given statistic, starting from the indicated point in the leaderboard  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Characters")
		static UVaRestPlayFabManager* ConstructServerGetCharacterLeaderboard(FString CharacterId, FString StatisticName, int32 MaxResultsCount,
		FString CharacterType = "", int32 StartPosition = 0);

	/** Retrieves the details of all title-specific statistics for the specific character */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Characters")
		static UVaRestPlayFabManager* ConstructServerGetCharacterStatistics(FString PlayFabId, FString CharacterId);

	/** Retrieves a list of ranked characters for the given statistic, centered on the requested user  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Characters")
		static UVaRestPlayFabManager* ConstructServerGetLeaderboardAroundCharacter(FString PlayFabId, FString CharacterId, FString StatisticName, int32 MaxResultsCount,
		FString CharacterType = "");

	/** Retrieves a list of all of the user's characters for the given statistic.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Characters")
		static UVaRestPlayFabManager* ConstructServerGetLeaderboardForUserCharacters(FString PlayFabId, FString StatisticName, int32 MaxResultsCount);

	/** Grants the specified character type to the user. */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Characters")
		static UVaRestPlayFabManager* ConstructServerGrantCharacterToUser(FString PlayFabId, FString CharacterType, FString CharacterName);

	/** Updates the values of the specified title-specific statistics for the specific character  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Characters")
		static UVaRestPlayFabManager* ConstructServerUpdateCharacterStatistics(FString PlayFabId, FString CharacterId, UVaRestJsonObject* CharacterStatistics);

	// Character Data

	/** Retrieves the title-specific custom data for the user which is readable and writable by the client */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Character Data")
		static UVaRestPlayFabManager* ConstructServerGetCharacterData(FString PlayFabId, FString CharacterId, TArray<FString> Keys, int32 IfChangedFromDataVersion = 0);

	/** Retrieves the title-specific custom data for the user's character which cannot be accessed by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Character Data")
		static UVaRestPlayFabManager* ConstructServerGetCharacterInternalData(FString PlayFabId, FString CharacterId, TArray<FString> Keys, int32 IfChangedFromDataVersion = 0);

	/** Retrieves the title-specific custom data for the user's character which can only be read by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Character Data")
		static UVaRestPlayFabManager* ConstructServerGetCharacterReadOnlyData(FString PlayFabId, FString CharacterId, TArray<FString> Keys, int32 IfChangedFromDataVersion = 0);

	/** Updates the title-specific custom data for the user's chjaracter which is readable and writable by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Character Data")
		static UVaRestPlayFabManager* ConstructServerUpdateCharacterData(FString PlayFabId, FString CharacterId, UVaRestJsonObject* Data, EUserDataPermision::Type Permission);

	/** Updates the title-specific custom data for the user's character which cannot be accessed by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Character Data")
		static UVaRestPlayFabManager* ConstructServerUpdateCharacterInternalData(FString PlayFabId, FString CharacterId, UVaRestJsonObject* Data, EUserDataPermision::Type Permission);

	/** Updates the title-specific custom data for the user's character which can only be read by the client  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Server | Character Data")
		static UVaRestPlayFabManager* ConstructServerUpdateCharacterReadOnlyData(FString PlayFabId, FString CharacterId, UVaRestJsonObject* Data, EUserDataPermision::Type Permission);

	/// Admin

	// Account Management

	// Player Data Management

	// Title-Wide Management

	// Player Item Management

	// Matchmaking

	// Custom Server Management

	// Shared Group Data

	// Server-Side Cloud Script

	/** Gets the contents and information of a specific Cloud Script revision.  */
	UFUNCTION(BlueprintPure, Category = "PlayFab | Admin | Server Side Cloud Script")
	static UVaRestPlayFabManager* ConstructGetCloudScriptRevision(FString Version, FString Revision);

	// Content

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
