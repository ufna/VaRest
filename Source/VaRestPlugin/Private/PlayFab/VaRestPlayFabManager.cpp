// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPluginPrivatePCH.h"

FString UVaRestPlayFabManager::PlayFabURL(TEXT("https://<AppID>.playfabapi.com/"));
FString UVaRestPlayFabManager::PlayFabLogicURL(TEXT("https://<AppID>.playfablogic.com/"));
FString UVaRestPlayFabManager::PlayFabAppId(TEXT(""));
FString UVaRestPlayFabManager::PlayFabApiKey(TEXT(""));
FString UVaRestPlayFabManager::PhotonRealtimeAppId(TEXT(""));
FString UVaRestPlayFabManager::PhotonTurnbasedAppId(TEXT(""));
FString UVaRestPlayFabManager::PhotonChatAppId(TEXT(""));

UVaRestPlayFabManager::UVaRestPlayFabManager(const class FObjectInitializer& PCIP)
	: Super(PCIP)
{

}

UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructPlayFabRequest(
	UObject* WorldContextObject, 
	ERequestVerb::Type Verb,
	ERequestContentType::Type ContentType)
{
	return (UVaRestPlayFabManager*)ConstructRequestExt(WorldContextObject, Verb, ContentType);
}

void UVaRestPlayFabManager::ProcessPlayFabURL(
	const FString& PlayFabClass,
	const FString& PlayFabSessionToken,
	bool isAdmin, bool isServer, bool isClient,
	bool useSecretKey, bool useSessionTicket,
	bool cloudScript, FString cloudScriptVersion)
{
	FString RequestUrl = PlayFabURL;

	// Build the full request url
	if (cloudScript)
	{
		RequestUrl = PlayFabLogicURL + cloudScriptVersion + TEXT("/prod/");
	}

	if (isAdmin)
	{
		RequestUrl += TEXT("Admin");
	}
	if (isClient)
	{
		RequestUrl += TEXT("Client");
	}
	if (isServer)
	{
		RequestUrl += TEXT("Server");
	}



	if (!PlayFabClass.IsEmpty())
	{
		RequestUrl += "/" + PlayFabClass;
	}

	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(RequestUrl);

	//HttpRequest->SetHeader("X-PlayFab-Application-Id", PlayFabAppId);
	//HttpRequest->SetHeader("X-PlayFab-REST-API-Key", PlayFabApiKey);

	if (!PlayFabSessionToken.IsEmpty())
	{
		HttpRequest->SetHeader("X-Authentication", PlayFabSessionToken);
	}
	if (useSecretKey)
	{
		HttpRequest->SetHeader("X-SecretKey", PlayFabApiKey);
	}

	ProcessRequest(HttpRequest);
}

void UVaRestPlayFabManager::ProcessPlayFabRequest()
{
	ProcessPlayFabURL(PlayFabClass, PlayFabSessionToken, isAdmin, isServer, isClient, useSecretKey, useSessionTicket, cloudScript, CloudScriptVersion);
}

void UVaRestPlayFabManager::SetPlayFabAuthData(FString AppId, FString ApiKey)
{
	PlayFabAppId = AppId;
	PlayFabApiKey = ApiKey;
}

//////////////////////////////////////////////////////////////////////////
// Play Fab Construction Helpers
//////////////////////////////////////////////////////////////////////////

//// Client
/** Create Login with Play Fab Request */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructLoginWithPlayFab(FString Username, FString Password)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "LoginWithPlayFab";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("TitleId"), PlayFabAppId);
	OutRestJsonObj->SetStringField(TEXT("Username"), Username);
	OutRestJsonObj->SetStringField(TEXT("Password"), Password);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create Register with play fab request */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructRegisterWithPlayFab(FString Username, FString Password, FString Email)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "RegisterPlayFabUser";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("TitleId"), PlayFabAppId);
	OutRestJsonObj->SetStringField(TEXT("Username"), Username);
	OutRestJsonObj->SetStringField(TEXT("Email"), Email);
	OutRestJsonObj->SetStringField(TEXT("Password"), Password);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create Get Photon Authentication Toeken Request */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetPhotonAuthenticationToken(FString SessionTicket, bool PhotonRealtime, bool PhotonTurnbased, bool PhotonChat)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetPhotonAuthenticationToken";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	FString tmpString;
	if (PhotonRealtime)
	{
		tmpString = PhotonRealtimeAppId;
	}
	if (PhotonTurnbased)
	{
		tmpString = PhotonTurnbasedAppId;
	}
	if (PhotonChat)
	{
		tmpString = PhotonChatAppId;
	}
	OutRestJsonObj->SetStringField(TEXT("PhotonApplicationId"), tmpString);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create a Login with Android Device Request */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructLoginWithAndroidDeviceID(FString AndroidDeviceId, FString OSVersion, FString AndroidDeviceType, bool CreateAccount)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "LoginWithAndroidDeviceID";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("TitleId"), PlayFabAppId);
	OutRestJsonObj->SetStringField(TEXT("AndroidDeviceId"), AndroidDeviceId);

	if (OSVersion != "")
	{
		OutRestJsonObj->SetStringField(TEXT("OS"), OSVersion);
	}
	if (AndroidDeviceType != "")
	{
		OutRestJsonObj->SetStringField(TEXT("AndroidDevice"), AndroidDeviceType);
	}
	if (CreateAccount)
	{
		OutRestJsonObj->SetStringField(TEXT("CreateAccount"), "True");
	}

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create Login with Play Fab  Email Request */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructLoginWithEmail(FString Email, FString Password)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "LoginWithEmailAddress";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("TitleId"), PlayFabAppId);
	OutRestJsonObj->SetStringField(TEXT("Email"), Email);
	OutRestJsonObj->SetStringField(TEXT("Password"), Password);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create Login with Facebook Request */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructLoginWithFacebook(FString FacebookAccessToken, bool CreateAccount)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "LoginWithFacebook";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("TitleId"), PlayFabAppId);
	OutRestJsonObj->SetStringField(TEXT("AccessToken"), FacebookAccessToken);

	if (CreateAccount) OutRestJsonObj->SetBoolField(TEXT("CreateAccount"), CreateAccount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create Login with Google Request */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructLoginWithGoogle(FString GoogleAccessToken, bool CreateAccount)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "LoginWithGoogleAccount";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("TitleId"), PlayFabAppId);
	OutRestJsonObj->SetStringField(TEXT("AccessToken"), GoogleAccessToken);

	if (CreateAccount) OutRestJsonObj->SetBoolField(TEXT("CreateAccount"), CreateAccount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create a Login with IOSDevice Request */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructLoginWithIOSDeviceID(FString IOSDeviceId, FString OSVersion, FString DeviceModel, bool CreateAccount)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "LoginWithIOSDeviceID";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("TitleId"), PlayFabAppId);
	OutRestJsonObj->SetStringField(TEXT("DeviceId"), IOSDeviceId);

	if (OSVersion != "") OutRestJsonObj->SetStringField(TEXT("OS"), OSVersion);
	if (DeviceModel != "") OutRestJsonObj->SetStringField(TEXT("DeviceModel"), DeviceModel);
	if (CreateAccount) OutRestJsonObj->SetBoolField(TEXT("CreateAccount"), CreateAccount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create Login with Steam Request */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructLoginWithSteam(FString SteamAccessToken, bool CreateAccount)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "LoginWithSteam";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("TitleId"), PlayFabAppId);
	OutRestJsonObj->SetStringField(TEXT("SteamTicket"), SteamAccessToken);
	if (CreateAccount) OutRestJsonObj->SetBoolField(TEXT("CreateAccount"), CreateAccount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Add Username Password Request */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructAddUsernamePassword(FString SessionTicket, FString Email, FString Username, FString Password)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "AddUsernamePassword";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("Username"), Username);
	OutRestJsonObj->SetStringField(TEXT("Email"), Email);
	OutRestJsonObj->SetStringField(TEXT("Password"), Password);


	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Account Info Request */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetAccountInfo(FString SessionTicket, FString PlayFabId, FString Username, FString Email, FString TitleDisplayName)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetAccountInfo";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);

	if (Username != "")
	{
		OutRestJsonObj->SetStringField(TEXT("Username"), Username);
	}
	if (Email != "")
	{
		OutRestJsonObj->SetStringField(TEXT("Email"), Email);
	}
	if (TitleDisplayName != "")
	{
		OutRestJsonObj->SetStringField(TEXT("TitleDisplayName"), TitleDisplayName);
	}

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get PlayFabIds from Facebook Ids */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetPlayFabIDsFromFacebookIDs(FString SessionTicket, TArray<FString> FacebookIDs)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetPlayFabIDsFromFacebookIDs";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringArrayField(TEXT("FacebookIDs"), FacebookIDs);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get User combined info all inputs are optional except the session ticket.*/
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetUserCombinedInfo(FString SessionTicket, TArray<FString> UserDataKeys,
	TArray<FString> ReadOnlyDataKey, FString PlayFabId, FString Username,
	FString Email, FString TitleDisplayName, bool GetAccountInfo, bool GetInventory, bool GetVirtualCurrency,
	bool GetUserData, bool GetReadOnlyData)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetUserCombinedInfo";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	if (PlayFabId != "") OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	if (Username!= "") OutRestJsonObj->SetStringField(TEXT("Username"), Username);
	if (Email != "") OutRestJsonObj->SetStringField(TEXT("Email"), Email);
	if (TitleDisplayName != "") OutRestJsonObj->SetStringField(TEXT("TitleDisplayName"), TitleDisplayName);
	if (GetAccountInfo) OutRestJsonObj->SetBoolField(TEXT("GetAccountInfo"), GetAccountInfo);
	if (GetInventory) OutRestJsonObj->SetBoolField(TEXT("GetInventory"), GetInventory);
	if (GetVirtualCurrency) OutRestJsonObj->SetBoolField(TEXT("GetVirtualCurrency"), GetVirtualCurrency);
	if (GetUserData) OutRestJsonObj->SetBoolField(TEXT("GetUserData"), GetUserData);
	if (UserDataKeys.Num() > 0) OutRestJsonObj->SetStringArrayField(TEXT("UserDataKeys"), UserDataKeys);
	if (GetReadOnlyData) OutRestJsonObj->SetBoolField(TEXT("GetReadOnlyData"), GetReadOnlyData);
	if (ReadOnlyDataKey.Num() > 0) OutRestJsonObj->SetStringArrayField(TEXT("ReadOnlyDataKeys"), ReadOnlyDataKey);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create Link android device ID */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructLinkAndroidDeviceID(FString SessionTicket, FString AndroidDeviceId, FString OSVersion, FString AndroidDeviceType)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "LinkAndroidDeviceID";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("AndroidDeviceId"), AndroidDeviceId);

	if (OSVersion != "") OutRestJsonObj->SetStringField(TEXT("OS"), OSVersion);
	if (AndroidDeviceType != "") OutRestJsonObj->SetStringField(TEXT("AndroidDevice"), AndroidDeviceType);
	

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create Link FacebookAccount */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructLinkFacebookAccount(FString SessionTicket, FString FacebookAccessToken)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "LinkFacebookAccount";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("AccessToken"), FacebookAccessToken);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create Link GameCenterAccount */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructLinkGameCenterAccount(FString SessionTicket, FString GameCenterId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "LinkGameCenterAccount";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("GameCenterId"), GameCenterId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create Link GoogleAccount */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructLinkGoogleAccount(FString SessionTicket, FString GoogleAccessToken)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "LinkGoogleAccount";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("AccessToken"), GoogleAccessToken);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create Link IOS device ID */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructLinkIOSDeviceID(FString SessionTicket, FString IOSDeviceId, FString OSVersion, FString IOSDeviceModel)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "LinkIOSDeviceID";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("DeviceId"), IOSDeviceId);

	if (OSVersion != "") OutRestJsonObj->SetStringField(TEXT("OS"), OSVersion);
	if (IOSDeviceModel != "") OutRestJsonObj->SetStringField(TEXT("DeviceModel"), IOSDeviceModel);


	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create Link SteamAccount */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructLinkSteamAccount(FString SessionTicket, FString SteamAccessToken)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "LinkSteamAccount";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("SteamTicket"), SteamAccessToken);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create SendAccountRecoverEmail */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructSendAccountRecoverEmail(FString Email)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "SendAccountRecoverEmail";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("Email"), Email);
	OutRestJsonObj->SetStringField(TEXT("TitleId"), PlayFabAppId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create UnLink android device ID */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructUnlinkAndroidDeviceID(FString SessionTicket, FString AndroidDeviceId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UnlinkAndroidDeviceID";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("AndroidDeviceId"), AndroidDeviceId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create UnLink FacebookAccount */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructUnlinkFacebookAccount(FString SessionTicket)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UnlinkFacebookAccount";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create UnLink GameCenterAccount */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructUnlinkGameCenterAccount(FString SessionTicket)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UnlinkGameCenterAccount";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create UnLink GoogleAccount */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructUnlinkGoogleAccount(FString SessionTicket)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UnlinkGoogleAccount";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create UnLink IOS device ID */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructUnlinkIOSDeviceID(FString SessionTicket, FString IOSDeviceId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UnlinkIOSDeviceID";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("DeviceId"), IOSDeviceId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create UnLink SteamAccount */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructUnlinkSteamAccount(FString SessionTicket)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UnlinkSteamAccount";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create UpdateUserTitleDisplayName */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructUpdateUserTitleDisplayName(FString SessionTicket, FString DisplayName)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateUserTitleDisplayName";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("DisplayName"), DisplayName);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create GetFriendLeaderboard */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetFriendLeaderboard(FString SessionTicket, FString StatisticName, int32 MaxResultsCount, int32 StartPosition)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetFriendLeaderboard";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("StatisticName"), StatisticName);
	if (StartPosition > 0) OutRestJsonObj->SetNumberField(TEXT("StatisticName"), StartPosition);
	OutRestJsonObj->SetNumberField(TEXT("MaxResultsCount"), MaxResultsCount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create GetLeaderboard */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetLeaderboard(FString SessionTicket, FString StatisticName, int32 MaxResultsCount, int32 StartPosition)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetLeaderboard";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("StatisticName"), StatisticName);
	if (StartPosition > 0) OutRestJsonObj->SetNumberField(TEXT("StatisticName"), StartPosition);
	OutRestJsonObj->SetNumberField(TEXT("MaxResultsCount"), MaxResultsCount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create GetLeaderboardAroundCurrentUser */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetLeaderboardAroundCurrentUser(FString SessionTicket, FString StatisticName, int32 MaxResultsCount)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetLeaderboardAroundCurrentUser";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("StatisticName"), StatisticName);
	OutRestJsonObj->SetNumberField(TEXT("MaxResultsCount"), MaxResultsCount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get User Data*/
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetUserData(FString SessionTicket, TArray<FString> Keys, FString PlayFabId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetUserData";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	if (Keys.Num() > 0) OutRestJsonObj->SetStringArrayField(TEXT("StatisticName"), Keys);
	if (PlayFabId != "") OutRestJsonObj->SetStringField(TEXT("MaxResultsCount"), PlayFabId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get User Publisher Data*/
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetUserPublisherData(FString SessionTicket, TArray<FString> Keys, FString PlayFabId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetUserPublisherData";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	if (Keys.Num() > 0) OutRestJsonObj->SetStringArrayField(TEXT("StatisticName"), Keys);
	if (PlayFabId != "") OutRestJsonObj->SetStringField(TEXT("MaxResultsCount"), PlayFabId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get User Publisher Read Only Data*/
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetUserPublisherReadOnlyData(FString SessionTicket, TArray<FString> Keys, FString PlayFabId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetUserPublisherReadOnlyData";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	if (Keys.Num() > 0) OutRestJsonObj->SetStringArrayField(TEXT("StatisticName"), Keys);
	if (PlayFabId != "") OutRestJsonObj->SetStringField(TEXT("MaxResultsCount"), PlayFabId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get User Read Only Data*/
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetUserReadOnlyData(FString SessionTicket, TArray<FString> Keys, FString PlayFabId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetUserReadOnlyData";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	if (Keys.Num() > 0) OutRestJsonObj->SetStringArrayField(TEXT("StatisticName"), Keys);
	if (PlayFabId != "") OutRestJsonObj->SetStringField(TEXT("MaxResultsCount"), PlayFabId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get User Statistics */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetUserStatistics(FString SessionTicket)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateUserData";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update User Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructUpdateUserData(FString SessionTicket, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateUserData";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetObjectField(TEXT("Data"), Data);
	if (PermissionType == EUserDataPermision::Type::PUBLIC) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Public"));
	if (PermissionType == EUserDataPermision::Type::PRIVATE) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Private"));

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update User Publisher Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructUpdateUserPublisherData(FString SessionTicket, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateUserPublisherData";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetObjectField(TEXT("Data"), Data);
	if (PermissionType == EUserDataPermision::Type::PUBLIC) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Public"));
	if (PermissionType == EUserDataPermision::Type::PRIVATE) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Private"));

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update User Statistics */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructUpdateUserStatistics(FString SessionTicket, UVaRestJsonObject* Statistics)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateUserStatistics";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetObjectField(TEXT("UserStatistics"), Statistics);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Catalog Items */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetCatalogItems(FString SessionTicket, FString CatalogVersion)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetCatalogItems";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("CatalogVersion"), CatalogVersion);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Store Items */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetStoreItems(FString SessionTicket, FString StoreId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetStoreItems";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("StoreId"), StoreId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Title Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetTitleData(FString SessionTicket, TArray<FString> Keys)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetTitleData";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Title News */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetTitleNews(FString SessionTicket, int32 NumberofEntries)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetTitleNews";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetNumberField(TEXT("Count"), NumberofEntries);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Add User Virtual Currency */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructAddUserVirtualCurrency(FString SessionTicket, FString VirtualCurrency, int32 Amount)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "AddUserVirtualCurrency";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("VirtualCurrency"), VirtualCurrency);
	OutRestJsonObj->SetNumberField(TEXT("Amount"), Amount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Consume Item */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructConsumeItem(FString SessionTicket, FString ItemInstanceId, int32 ConsumeCount)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "ConsumeItem";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("ItemInstanceId"), ItemInstanceId);
	OutRestJsonObj->SetNumberField(TEXT("ConsumeCount"), ConsumeCount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Character Inventory */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetCharacterInventory(FString SessionTicket, FString PlayFabId, FString CharacterId, FString CatalogVersion)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetCharacterInventory";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	if (CatalogVersion != "") OutRestJsonObj->SetStringField(TEXT("CatalogVersion"), CatalogVersion);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get User Inventory */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetUserInventory(FString SessionTicket)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetUserInventory";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Redeem Coupon */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructRedeemCoupon(FString SessionTicket, FString CouponCode, FString CatalogVersion)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "RedeemCoupon";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("CouponCode"), CouponCode);
	if (CatalogVersion != "") OutRestJsonObj->SetStringField(TEXT("CatalogVersion"), CatalogVersion);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Report Player */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructReportPlayer(FString SessionTicket, FString ReportedId, FString Reason)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "ReportPlayer";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("ReporteeId"), ReportedId);
	OutRestJsonObj->SetStringField(TEXT("Comment"), Reason);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Subtract User Virtual Currency */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructSubtractUserVirtualCurrency(FString SessionTicket, FString VirtualCurrency, int32 Amount)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "SubtractUserVirtualCurrency";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("VirtualCurrency"), VirtualCurrency);
	OutRestJsonObj->SetNumberField(TEXT("Amount"), Amount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Unlock Container Item */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructUnlockContainerItem(FString SessionTicket, FString ContainerItemId, FString CatalogVersion)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UnlockContainerItem";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("ContainerItemId"), ContainerItemId);
	if (CatalogVersion != "") OutRestJsonObj->SetStringField(TEXT("CatalogVersion"), CatalogVersion);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Start Purchase */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructStartPurchase(FString SessionTicket, UVaRestJsonObject* Items, FString CatalogVersion, FString StoreId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "StartPurchase";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	if (CatalogVersion != "") OutRestJsonObj->SetStringField(TEXT("CatalogVersion"), CatalogVersion);
	if (StoreId != "") OutRestJsonObj->SetStringField(TEXT("StoreId"), StoreId);
	OutRestJsonObj->SetObjectField(TEXT("Items"), Items);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Pay For Purchase */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructPayForPurchase(FString SessionTicket, FString OrderId, FString ProviderName, FString Currency)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "PayForPurchase";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("OrderId"), OrderId);
	OutRestJsonObj->SetStringField(TEXT("ProviderName"), ProviderName);
	OutRestJsonObj->SetStringField(TEXT("Currency"), Currency);


	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Confirm Purchase */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructConfirmPurchase(FString SessionTicket, FString OrderId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "ConfirmPurchase";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("OrderId"), OrderId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** PurchaseItem */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructPurchaseItem(FString SessionTicket, FString ItemId, FString VirtualCurrency,
	int32 Price, FString CatalogVersion, FString StoreId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "PurchaseItem";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("ItemId"), ItemId);
	OutRestJsonObj->SetStringField(TEXT("VirtualCurrency"), VirtualCurrency);
	OutRestJsonObj->SetNumberField(TEXT("Price"), Price);
	if (CatalogVersion != "") OutRestJsonObj->SetStringField(TEXT("CatalogVersion"), CatalogVersion);
	if (StoreId != "") OutRestJsonObj->SetStringField(TEXT("StoreId"), StoreId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Add Friend */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructAddFriend(FString SessionTicket, FString FriendPlayFabId, FString FriendUsername,
	FString FriendEmail, FString FriendTitleDisplayName)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "AddFriend";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	if (FriendPlayFabId != "") OutRestJsonObj->SetStringField(TEXT("FrienPlayFabId"), FriendPlayFabId);
	if (FriendUsername != "") OutRestJsonObj->SetStringField(TEXT("FriendUsername"), FriendUsername);
	if (FriendEmail != "") OutRestJsonObj->SetStringField(TEXT("FriendEmail"), FriendEmail);
	if (FriendTitleDisplayName != "") OutRestJsonObj->SetStringField(TEXT("FriendTitleDisplayName"), FriendTitleDisplayName);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Friends List */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetFriendsList(FString SessionTicket, bool IncludeSteamFriends, bool IncludeFacebookFriends)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetFriendsList";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	if (IncludeSteamFriends) OutRestJsonObj->SetBoolField(TEXT("IncludeSteamFriends"), IncludeSteamFriends);
	if (IncludeFacebookFriends) OutRestJsonObj->SetBoolField(TEXT("IncludeFacebookFriends"), IncludeFacebookFriends);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Remove Friend */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructRemoveFriend(FString SessionTicket, FString FriendPlayFabId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "RemoveFriend";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("FriendPlayFabId"), FriendPlayFabId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Set Friend Tags */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructSetFriendTags(FString SessionTicket, FString FriendPlayFabId, TArray<FString> Tags)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "SetFriendTags";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("FriendPlayFabId"), FriendPlayFabId);
	OutRestJsonObj->SetStringArrayField(TEXT("Tags"), Tags);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Register For IOS Push Notifications */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructRegisterFORIOSPushNotifications(FString SessionTicket, FString DeviceToken, bool SendPushNotificationConfirmation, FString ConfirmationMessage)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "RegisterForIOSPushNotifications";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("DeviceToken"), DeviceToken);
	if (SendPushNotificationConfirmation) OutRestJsonObj->SetBoolField(TEXT("SendPushNotificationConfirmation"), SendPushNotificationConfirmation);
	if (SendPushNotificationConfirmation) OutRestJsonObj->SetStringField(TEXT("ConfirmationMessage"), ConfirmationMessage);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Restore IOS Purchases */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructRestoreIOSPurchases(FString SessionTicket, FString Base64ReceiptData)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "RestoreIOSPurchases";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("ReceiptData"), Base64ReceiptData);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Validate IOS Reciept */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructValidateIOSReceipt(FString SessionTicket, FString Base64ReceiptData, FString CurrencyCode, FString PurchasePrice)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "ValidateIOSReceipt";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("ReceiptData"), Base64ReceiptData);
	OutRestJsonObj->SetStringField(TEXT("CurrencyCode"), CurrencyCode);
	OutRestJsonObj->SetStringField(TEXT("PurchasePrice"), PurchasePrice);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Current Games */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetCurrentGames(FString SessionTicket, ERegion::Type Region, FString BuildVersion, FString GameMode, FString StatisticName)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetCurrentGames";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	if (Region != ERegion::Type::ANY)
	{
		if (Region == ERegion::Type::AUSTRALIA) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("Australia"));
		if (Region == ERegion::Type::BRAZIL) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("Brazil"));
		if (Region == ERegion::Type::EUWEST) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("EUWest"));
		if (Region == ERegion::Type::JAPAN) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("Japan"));
		if (Region == ERegion::Type::SINGAPORE) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("Singapore"));
		if (Region == ERegion::Type::USCENTRAL) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("USCentral"));
		if (Region == ERegion::Type::USEAST) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("USEast"));
	}
	if (BuildVersion != "") OutRestJsonObj->SetStringField(TEXT("BuildVersion"), BuildVersion);
	if (GameMode != "") OutRestJsonObj->SetStringField(TEXT("GameMode"), GameMode);
	if (StatisticName != "") OutRestJsonObj->SetStringField(TEXT("StatisticName"), StatisticName);


	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Game Server Regions */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetGameServerRegions(FString SessionTicket, FString BuildVersion)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetGameServerRegions";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("BuildVersion"), BuildVersion);
	OutRestJsonObj->SetStringField(TEXT("TitleId"), PlayFabAppId);


	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Matchmake */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructMatchmake(FString SessionTicket, ERegion::Type Region, FString BuildVersion, FString GameMode,
	FString LobbyId, FString StatisticName, FString CharacterId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "Matchmake";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	if (BuildVersion != "") OutRestJsonObj->SetStringField(TEXT("BuildVersion"), BuildVersion);
	if (Region != ERegion::Type::ANY)
	{
		if (Region == ERegion::Type::AUSTRALIA) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("Australia"));
		if (Region == ERegion::Type::BRAZIL) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("Brazil"));
		if (Region == ERegion::Type::EUWEST) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("EUWest"));
		if (Region == ERegion::Type::JAPAN) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("Japan"));
		if (Region == ERegion::Type::SINGAPORE) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("Singapore"));
		if (Region == ERegion::Type::USCENTRAL) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("USCentral"));
		if (Region == ERegion::Type::USEAST) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("USEast"));
	}
	if (GameMode != "") OutRestJsonObj->SetStringField(TEXT("GameMode"), GameMode);
	if (LobbyId != "") OutRestJsonObj->SetStringField(TEXT("LobbyId"), BuildVersion);
	if (StatisticName!= "") OutRestJsonObj->SetStringField(TEXT("StatisticName"), StatisticName);
	if (CharacterId != "") OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;

}

/** Start Game */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructStartGame(FString SessionTicket, FString BuildVersion, ERegion::Type Region, FString GameMode, FString StatisticName,
	FString CharacterId, bool PasswordRestricted, FString CustomCommandLineData)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "Matchmake";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("BuildVersion"), BuildVersion);
	if (Region != ERegion::Type::ANY)
	{
		if (Region == ERegion::Type::AUSTRALIA) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("Australia"));
		if (Region == ERegion::Type::BRAZIL) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("Brazil"));
		if (Region == ERegion::Type::EUWEST) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("EUWest"));
		if (Region == ERegion::Type::JAPAN) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("Japan"));
		if (Region == ERegion::Type::SINGAPORE) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("Singapore"));
		if (Region == ERegion::Type::USCENTRAL) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("USCentral"));
		if (Region == ERegion::Type::USEAST) OutRestJsonObj->SetStringField(TEXT("Region"), TEXT("USEast"));
	}
	OutRestJsonObj->SetStringField(TEXT("GameMode"), GameMode);
	if (StatisticName != "") OutRestJsonObj->SetStringField(TEXT("StatisticName"), StatisticName);
	if (CharacterId != "") OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	if (PasswordRestricted) OutRestJsonObj->SetBoolField(TEXT("PasswordRestrcted"), PasswordRestricted);
	if (CustomCommandLineData != "") OutRestJsonObj->SetStringField(TEXT("CustomCommandLineData"), CustomCommandLineData);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Android Device Push Notification Registration */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructAndroidDevicePushNotificationRegistration(FString SessionTicket, 
	FString DeviceToken, bool SendPushNotificationConfirmation,	FString ConfirmationMessage)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "AndroidDevicePushNotificationRegistration";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("DeviceToken"), DeviceToken);
	if (SendPushNotificationConfirmation) OutRestJsonObj->SetBoolField(TEXT("SendPushNotificationConfirmation"), SendPushNotificationConfirmation);
	if (SendPushNotificationConfirmation) OutRestJsonObj->SetStringField(TEXT("ConfirmationMessage"), ConfirmationMessage);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Validate Google Play Purchase */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructValidateGooglePlayPurchase(FString SessionTicket, FString ReceiptJsonString, FString Signature)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "ValidateGooglePlayPurchase";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("ReceiptJson"), ReceiptJsonString);
	OutRestJsonObj->SetStringField(TEXT("Signature"), Signature);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Log Event */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructLogEvent(FString SessionTicket, FString EventName, UVaRestJsonObject* Body, bool ProfileSetEvent)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "LogEvent";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("EventName"), EventName);
	OutRestJsonObj->SetObjectField(TEXT("Body"), Body);
	if (ProfileSetEvent) OutRestJsonObj->SetBoolField(TEXT("ProfileSetEvent"), ProfileSetEvent);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Add Shared Group Memebers */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructAddSharedGroupMembers(FString SessionTicket, FString SharedGroupId, TArray<FString> PlayFabIds)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "AddSharedGroupMembers";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("SharedGroupId"), SharedGroupId);
	OutRestJsonObj->SetStringArrayField(TEXT("PlayFabIds"), PlayFabIds);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create Shared Group */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructCreateSharedGroup(FString SessionTicket, FString SharedGroupId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "CreateSharedGroup";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("SharedGroupId"), SharedGroupId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Publisher Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetPublisherData(FString SessionTicket, TArray<FString> Keys)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetPublisherData";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Shared Group Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetSharedGroupData(FString SessionTicket, FString SharedGroupId, TArray<FString> Keys, bool GetMembers)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetSharedGroupData";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("SharedGroupId"), SharedGroupId);
	OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);
	OutRestJsonObj->SetBoolField(TEXT("ProfileSetEvent"), GetMembers);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Remove Shared Group Memebers */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructRemoveSharedGroupMembers(FString SessionTicket, FString SharedGroupId, TArray<FString> PlayFabIds)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "RemoveSharedGroupMembers";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("SharedGroupId"), SharedGroupId);
	OutRestJsonObj->SetStringArrayField(TEXT("PlayFabIds"), PlayFabIds);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update Shared Group Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructUpdateSharedGroupData(FString SessionTicket, FString SharedGroupId, UVaRestJsonObject* Data, EUserDataPermision::Type Permission)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateSharedGroupData";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("SharedGroupId"), SharedGroupId);
	OutRestJsonObj->SetObjectField(TEXT("Data"), Data);
	if (Permission == EUserDataPermision::Type::PUBLIC) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Public"));
	if (Permission == EUserDataPermision::Type::PRIVATE) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Private"));

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** RefreshPSNAuthToken */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructRefreshPSNAuthToken(FString SessionTicket, FString PSNAuthCode)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "RefreshPSNAuthToken";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("AuthToken"), PSNAuthCode);
	

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Cloud Script Url */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetCloudScriptUrl(FString SessionTicket, int32 Version, bool Testing)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetCloudScriptUrl";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	if (Version > 0) OutRestJsonObj->SetNumberField(TEXT("Version"), Version);
	if (Testing) OutRestJsonObj->SetBoolField(TEXT("Testing"), Testing);


	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Run Cloud Script */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructRunCloudScript(FString SessionTicket, FString ActionId, UVaRestJsonObject* Params, FString ParamsEncoded)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "RunCloudScript";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = true;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("ActionId"), ActionId);
	OutRestJsonObj->SetObjectField(TEXT("Params"), Params);
	if (ParamsEncoded != "") OutRestJsonObj->SetStringField(TEXT("ParamsEncoded"), ParamsEncoded);
	
	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Content Download Url */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetContentDownloadUrl(FString SessionTicket, FString Key, FString HttpMethod, bool ThruCDN)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetContentDownloadUrl";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("Key"), Key);
	if (HttpMethod != "") OutRestJsonObj->SetStringField(TEXT("HttpMethod"), HttpMethod);
	OutRestJsonObj->SetBoolField(TEXT("ThruCDN"), ThruCDN);
	
	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Character Leaderboard */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetCharacterLeaderboard(FString SessionTicket, FString StatisticName, int32 MaxResultsCount,
	FString CharacterType, int32 StartPosition)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetCharacterLeaderboard";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("StatisticName"), StatisticName);
	OutRestJsonObj->SetNumberField(TEXT("MaxResultsCount"), MaxResultsCount);
	if (CharacterType != "") OutRestJsonObj->SetStringField(TEXT("CharacterType"), CharacterType);
	if (StartPosition > 0) OutRestJsonObj->SetNumberField(TEXT("StartPosition"), StartPosition);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Leaderboard Around Character */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetLeaderboardAroundCharacter(FString SessionTicket, FString CharacterId, FString StatisticName, 
	int32 MaxResultsCount, FString CharacterType)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetLeaderboardAroundCharacter";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("StatisticName"), StatisticName);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetNumberField(TEXT("MaxResultsCount"), MaxResultsCount);
	if (CharacterType != "") OutRestJsonObj->SetStringField(TEXT("CharacterType"), CharacterType);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Leaderboard For User Character */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetLeaderboardForUserCharacters(FString SessionTicket, FString StatisticName, int32 MaxResultsCount)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetLeaderboardForUserCharacters";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("StatisticName"), StatisticName);
	OutRestJsonObj->SetNumberField(TEXT("MaxResultsCount"), MaxResultsCount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Grant Character To User */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGrantCharacterToUser(FString SessionTicket, FString ItemId, FString CharacterName, FString CatalogVersion)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GrantCharacterToUser";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("ItemId"), ItemId);
	OutRestJsonObj->SetStringField(TEXT("CharacterName"), CharacterName);
	if (CatalogVersion != "") OutRestJsonObj->SetStringField(TEXT("CatalogVersion"), CatalogVersion);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Character Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetCharacterData(FString SessionTicket, FString PlayFabId, FString CharacterId, TArray<FString> Keys)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetCharacterData";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	if (Keys.Num() > 0) OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Character ReadOnly Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetCharacterReadOnlyData(FString SessionTicket, FString PlayFabId, FString CharacterId, TArray<FString> Keys)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetCharacterReadOnlyData";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	if (Keys.Num() > 0) OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update Character Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructUpdateCharacterData(FString SessionTicket, FString CharacterId, UVaRestJsonObject* Data, EUserDataPermision::Type Permission)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateCharacterData";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = false;
	manager->isClient = true;
	manager->useSecretKey = false;
	manager->useSessionTicket = true;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetObjectField(TEXT("Data"), Data);
	if (Permission == EUserDataPermision::Type::PUBLIC) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Public"));
	if (Permission == EUserDataPermision::Type::PRIVATE) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Private"));

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

//// Server
/** Create AuthenticateSessionTicket Request */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructAuthenticateSessionTicket(FString SessionTicket)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "AuthenticateSessionTicket";
	manager->PlayFabSessionToken = SessionTicket;
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("SessionTicket"), SessionTicket);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get User Account Info */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetUserAccountInfo(FString PlayFabId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetUserAccountInfo";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Send Push Notification */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructSendPushNotification(FString RecipientPlayFabId, FString Message, FString Subject)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "SendPushNotification";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("Recipient"), RecipientPlayFabId);
	OutRestJsonObj->SetStringField(TEXT("Message"), Message);
	if (Subject != "") OutRestJsonObj->SetStringField(TEXT("Subject"), Subject);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create GetLeaderboard */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetLeaderboard(FString StatisticName, int32 MaxResultsCount, int32 StartPosition)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetLeaderboard";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("StatisticName"), StatisticName);
	if (StartPosition > 0) OutRestJsonObj->SetNumberField(TEXT("StatisticName"), StartPosition);
	OutRestJsonObj->SetNumberField(TEXT("MaxResultsCount"), MaxResultsCount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create GetLeaderboardAroundUser */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetLeaderboardAroundUser(FString StatisticName, FString PlayFabId, int32 MaxResultsCount)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetLeaderboardAroundUser";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("StatisticName"), StatisticName);
	OutRestJsonObj->SetStringField(TEXT("StatisticName"), PlayFabId);
	OutRestJsonObj->SetNumberField(TEXT("MaxResultsCount"), MaxResultsCount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get User Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetUserData(TArray<FString> Keys, FString PlayFabId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetUserData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get User Internal Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetUserInternalData(TArray<FString> Keys, FString PlayFabId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetUserInternal";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get User Publisher Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetUserPublisherData(TArray<FString> Keys, FString PlayFabId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetUserPublisherData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get User Publisher Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetUserPublisherInternalData(TArray<FString> Keys, FString PlayFabId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetUserPublisherInternalData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get User Publisher Read Only Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetUserPublisherReadOnlyData(TArray<FString> Keys, FString PlayFabId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetUserPublisherReadOnlyData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get User Read Only Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetUserReadOnlyData(TArray<FString> Keys, FString PlayFabId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetUserReadOnlyData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get User Statistics */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetUserStatistics(FString PlayFabId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetUserStatistics";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update User Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerUpdateUserData(FString PlayFabId, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateUserData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetObjectField(TEXT("Data"), Data);
	if (PermissionType == EUserDataPermision::Type::PUBLIC) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Public"));
	else OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Private"));

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update User Internal Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerUpdateUserInternalData(FString PlayFabId, UVaRestJsonObject* Data)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateUserInternalData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetObjectField(TEXT("Data"), Data);
	//if (PermissionType == EUserDataPermision::Type::PUBLIC) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Public"));
	//if (Permissiontype == EUserDataPermision::Type::PRIVATE) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Private"));

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update User Publisher Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerUpdateUserPublisherData(FString PlayFabId, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateUserPublisherData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetObjectField(TEXT("Data"), Data);
	if (PermissionType == EUserDataPermision::Type::PUBLIC) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Public"));
	else OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Private"));

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update User Publisher Internal Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerUpdateUserPublisherInternalData(FString PlayFabId, UVaRestJsonObject* Data)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateUserPublisherInternalData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetObjectField(TEXT("Data"), Data);
	//if (PermissionType == EUserDataPermision::Type::PUBLIC) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Public"));
	//if (Permissiontype == EUserDataPermision::Type::PRIVATE) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Private"));

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update User Publisher ReadOnly Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerUpdateUserPublisherReadOnlyData(FString PlayFabId, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateUserPublisherReadOnlyData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetObjectField(TEXT("Data"), Data);
	if (PermissionType == EUserDataPermision::Type::PUBLIC) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Public"));
	else OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Private"));

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update User ReadOnly Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerUpdateUserReadOnlyData(FString PlayFabId, UVaRestJsonObject* Data, EUserDataPermision::Type PermissionType)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateUserReadOnlyData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetObjectField(TEXT("Data"), Data);
	if (PermissionType == EUserDataPermision::Type::PUBLIC) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Public"));
	else OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Private"));

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update User Statistics */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerUpdateUserStatistics(FString PlayFabId, UVaRestJsonObject* Statistics)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateUserStatistics";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetObjectField(TEXT("UserStatistics"), Statistics);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Catalog Items */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetCatalogItems(FString CatalogVersion)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetCatalogItems";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("CatalogVersion"), CatalogVersion);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Title Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetTitleData(TArray<FString> Keys)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetTitleData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Title Internal Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetTitleInternalData(TArray<FString> Keys)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetTitleInternalData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Set Title Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerSetTitleData(FString Key, FString Value)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "SetTitleData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("Key"), Key);
	OutRestJsonObj->SetStringField(TEXT("Value"), Value);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Title nternal Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerSetTitleInternalData(FString Key, FString Value)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "SetTitleInternalData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("Key"), Key);
	OutRestJsonObj->SetStringField(TEXT("Value"), Value);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Add Character Virtual Currency */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerAddCharacterVirtualCurrency(FString PlayFabId, FString CharacterId, FString VirtualCurrency, int32 Amount)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "AddCharacterVirtualCurrency";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetStringField(TEXT("VirtualCurrency"), VirtualCurrency);
	OutRestJsonObj->SetNumberField(TEXT("Amount"), Amount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Add User Virtual Currency */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerAddUserVirtualCurrency(FString PlayFabId, FString VirtualCurrency, int32 Amount)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "AddUserVirtualCurrency";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	//OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetStringField(TEXT("VirtualCurrency"), VirtualCurrency);
	OutRestJsonObj->SetNumberField(TEXT("Amount"), Amount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Character Inventory */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetCharacterInventory(FString PlayFabId, FString CharacterId, FString CatalogVersion)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetCharacterInventory";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	if (CatalogVersion != "") OutRestJsonObj->SetStringField(TEXT("CatalogVersion"), CatalogVersion);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get User Inventory */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetUserInventory(FString PlayFabId, FString CatalogVersion)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetUserInventory";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	//OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	if (CatalogVersion != "") OutRestJsonObj->SetStringField(TEXT("CatalogVersion"), CatalogVersion);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Grant Items To Character */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGrantItemsToCharacter(FString PlayFabId, FString CharacterId, TArray<FString> ItemIds, FString CatalogVersion,
	FString Annotation)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GrantItemsToCharacter";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	if (CatalogVersion != "") OutRestJsonObj->SetStringField(TEXT("CatalogVersion"), CatalogVersion);
	if (Annotation!= "") OutRestJsonObj->SetStringField(TEXT("Annotation"), Annotation);
	OutRestJsonObj->SetStringArrayField(TEXT("ItemIds"), ItemIds);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Grant Items To User */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGrantItemsToUser(FString PlayFabId, TArray<FString> ItemIds, FString CatalogVersion,
	FString Annotation)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GrantItemsToUser";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	//OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	if (CatalogVersion != "") OutRestJsonObj->SetStringField(TEXT("CatalogVersion"), CatalogVersion);
	if (Annotation != "") OutRestJsonObj->SetStringField(TEXT("Annotation"), Annotation);
	OutRestJsonObj->SetStringArrayField(TEXT("ItemIds"), ItemIds);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Grant Items To Users */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGrantItemsToUsers(UVaRestJsonObject* ItemGrants, FString CatalogVersion)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GrantItemsToUsers";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	if (CatalogVersion != "") OutRestJsonObj->SetStringField(TEXT("CatalogVersion"), CatalogVersion);
	OutRestJsonObj->SetObjectField(TEXT("ItemGrants"), ItemGrants);


	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Modify Item Uses */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerModifyItemUses(FString PlayFabId, FString ItemInstanceId, int32 UsesToAdd)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "ModifyItemUses";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("ItemInstanceId"), ItemInstanceId);
	OutRestJsonObj->SetNumberField(TEXT("UsesToAdd"), UsesToAdd);


	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Move Item To Character From Character */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerMoveItemToCharacterFromCharacter(FString PlayFabId, FString GivingCharacterId,
	FString ReceivingCharacterId, FString ItemInstanceId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "MoveItemToCharacterFromCharacter";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("GivingCharacterId"), GivingCharacterId);
	OutRestJsonObj->SetStringField(TEXT("ReceivingCharacterId"), ReceivingCharacterId);
	OutRestJsonObj->SetStringField(TEXT("ItemInstanceId"), ItemInstanceId);
	
	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Move Item To Character From User */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerMoveItemToCharacterFromUser(FString PlayFabId, FString CharacterId,
	FString ItemInstanceId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "MoveItemToCharacterFromUser";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetStringField(TEXT("ItemInstanceId"), ItemInstanceId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Move Item To User From Character */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerMoveItemToUserFromCharacter(FString PlayFabId, FString CharacterId,
	FString ItemInstanceId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "MoveItemToUserFromCharacter";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetStringField(TEXT("ItemInstanceId"), ItemInstanceId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Report Player */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerReportPlayer(FString ReporterPlayFabId, FString ReporteePlayFabId,
	FString TitleId, FString Comment)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "ReportPlayer";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("ReporterPlayFabId"), ReporterPlayFabId);
	OutRestJsonObj->SetStringField(TEXT("ReporteePlayFabId"), ReporteePlayFabId);
	if (TitleId != "") OutRestJsonObj->SetStringField(TEXT("TitleId"), TitleId);
	if (Comment != "") OutRestJsonObj->SetStringField(TEXT("Comment"), Comment);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Subtract Character Virtual Currency */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerSubtractCharacterVirtualCurrency(FString PlayFabId, FString CharacterId, FString VirtualCurrency, int32 Amount)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "SubtractCharacterVirtualCurrency";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetStringField(TEXT("VirtualCurrency"), VirtualCurrency);
	OutRestJsonObj->SetNumberField(TEXT("Amount"), Amount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Subtract User Virtual Currency */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerSubtractUserVirtualCurrency(FString PlayFabId, FString VirtualCurrency, int32 Amount)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "SubtractUserVirtualCurrency";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	//OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetStringField(TEXT("VirtualCurrency"), VirtualCurrency);
	OutRestJsonObj->SetNumberField(TEXT("Amount"), Amount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update User Inventory Item Custom Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerUpdateUserInventoryItemCustomData(FString PlayFabId, FString ItemInstanceId,
	UVaRestJsonObject* Data, FString CharacterId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateUserInventoryCustomData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("ItemInstanceId"), ItemInstanceId);
	if (CharacterId != "") OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetObjectField(TEXT("Data"), Data);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Notify Matchmaker Player Left */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerNotifyMatchmakerPlayerLeft(FString PlayFabId, FString LobbyId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "NotifyMatchmakerPlayerLeft";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("LobbyId"), LobbyId);
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Redeem Matchmaker Ticket */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerRedeemMatchmakerTicket(FString Ticket, FString LobbyId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "RedeemMatchmakerTicket";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("Ticket"), Ticket);
	OutRestJsonObj->SetStringField(TEXT("LobbyId"), LobbyId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Award Steam Achievement */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerAwardSteamAchievement(UVaRestJsonObject* Achievements)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "AwardSteamAchievement";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetObjectField(TEXT("Achievements"), Achievements);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Log Event */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerLogEvent(FString EventName, UVaRestJsonObject* Body, FString PlayFabId,
	FString EntityId, FString EntityType, bool PlayerEvent, bool ProfileSetEvent)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "LogEvent";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("EventName"), EventName);
	OutRestJsonObj->SetObjectField(TEXT("Body"), Body);

	if (PlayerEvent) OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	else
	{
		OutRestJsonObj->SetStringField(TEXT("EntityId"), EntityId);
		OutRestJsonObj->SetStringField(TEXT("EntityType"), EntityType);
	}

	if (ProfileSetEvent) OutRestJsonObj->SetBoolField(TEXT("ProfileSetEvent"), ProfileSetEvent);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Add Shared Group Memebers */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerAddSharedGroupMembers(FString SharedGroupId, TArray<FString> PlayFabIds)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "AddSharedGroupMembers";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("SharedGroupId"), SharedGroupId);
	OutRestJsonObj->SetStringArrayField(TEXT("PlayFabIds"), PlayFabIds);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Create Shared Group */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerCreateSharedGroup(FString SharedGroupId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "CreateSharedGroup";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("SharedGroupId"), SharedGroupId);
	//OutRestJsonObj->SetStringArrayField(TEXT("PlayFabIds"), PlayFabIds);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Delete Shared Group */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerDeleteSharedGroup(FString SharedGroupId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "DeleteSharedGroup";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("SharedGroupId"), SharedGroupId);
	//OutRestJsonObj->SetStringArrayField(TEXT("PlayFabIds"), PlayFabIds);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Publisher Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetPublisherData(TArray<FString> Keys)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetPublisherData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	//OutRestJsonObj->SetStringField(TEXT("SharedGroupId"), SharedGroupId);
	OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Shared Group Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetSharedGroupData(FString SharedGroupId, TArray<FString> Keys, bool GetMembers)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetSharedGroupData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("SharedGroupId"), SharedGroupId);
	OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);
	if (GetMembers) OutRestJsonObj->SetBoolField(TEXT("GetMembers"), GetMembers);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Remove Shared Group Memebers */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerRemoveSharedGroupMembers(FString SharedGroupId, TArray<FString> PlayFabIds)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "RemoveSharedGroupMembers";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("SharedGroupId"), SharedGroupId);
	OutRestJsonObj->SetStringArrayField(TEXT("PlayFabIds"), PlayFabIds);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Set Publisher Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerSetPublisherData(FString Key, FString Value)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "SetPublisherData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("Key"), Key);
	OutRestJsonObj->SetStringField(TEXT("Value"), Value);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update Shared Group Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerUpdateSharedGroupData(FString SharedGroupId, UVaRestJsonObject* Data, EUserDataPermision::Type Permission)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateSharedGroupData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("SharedGroupId"), SharedGroupId);
	OutRestJsonObj->SetObjectField(TEXT("Data"), Data);
	if (Permission == EUserDataPermision::Type::PUBLIC) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Public"));
	else OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Private"));
	
	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Content Download Url */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetContentDownloadUrl(FString Key, FString HttpMethod, bool ThruCDN)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetContentDownloadUrl";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("Key"), Key);
	if (HttpMethod != "") OutRestJsonObj->SetStringField(TEXT("HttpMethod"), HttpMethod);
	OutRestJsonObj->SetBoolField(TEXT("ThruCDN"), ThruCDN);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Delete Character From User */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerDeleteCharacterFromUser(FString PlayFabId, FString CharacterId, bool SaveCharacterInventory)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "DeleteCharacterFromUser";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	if (SaveCharacterInventory) OutRestJsonObj->SetBoolField(TEXT("SaveCharacterInventory"), SaveCharacterInventory);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get All Users Characters */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetAllUsersCharacters(FString PlayFabId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetAllUsersCharacters";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Character Leaderboard */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetCharacterLeaderboard(FString CharacterId, FString StatisticName, int32 MaxResultsCount,
	FString CharacterType, int32 StartPosition)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetCharacterLeaderboard";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetStringField(TEXT("StatisticName"), StatisticName);
	OutRestJsonObj->SetNumberField(TEXT("MaxResultsCount"), MaxResultsCount);
	if (CharacterType != "") OutRestJsonObj->SetStringField(TEXT("CharacterType"), CharacterType);
	if (StartPosition > 0) OutRestJsonObj->SetNumberField(TEXT("StartPosition"), StartPosition);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Character Statistics */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetCharacterStatistics(FString PlayFabId, FString CharacterId)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetCharacterStatistics";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Leaderboard Around Character */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetLeaderboardAroundCharacter(FString PlayFabId, FString CharacterId, FString StatisticName, int32 MaxResultsCount,
	FString CharacterType)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetLeaderboardAroundCharacter";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("StatisticName"), StatisticName);
	OutRestJsonObj->SetNumberField(TEXT("MaxResultsCount"), MaxResultsCount);
	if (CharacterType != "") OutRestJsonObj->SetStringField(TEXT("CharacterType"), CharacterType);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Leaderboard For User Character */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetLeaderboardForUserCharacters(FString PlayFabId, FString StatisticName, int32 MaxResultsCount)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetLeaderboardForUserCharacters";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("StatisticName"), StatisticName);
	OutRestJsonObj->SetNumberField(TEXT("MaxResultsCount"), MaxResultsCount);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Grant Character To User */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGrantCharacterToUser(FString PlayFabId, FString CharacterType, FString CharacterName)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GrantCharacterToUser";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterName"), CharacterName);
	OutRestJsonObj->SetStringField(TEXT("CharacterType"), CharacterType);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update Character Statistics */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerUpdateCharacterStatistics(FString PlayFabId, FString CharacterId, UVaRestJsonObject* CharacterStatistics)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateCharacterStatistics";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetObjectField(TEXT("CharacterStatistics"), CharacterStatistics);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Character Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetCharacterData(FString PlayFabId, FString CharacterId, TArray<FString> Keys,
	int32 IfChangedFromDataVersion)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetCharacterData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);
	if (IfChangedFromDataVersion > 0) OutRestJsonObj->SetNumberField(TEXT("IfChangedFromDataVersion"), IfChangedFromDataVersion);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Character Internal Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetCharacterInternalData(FString PlayFabId, FString CharacterId, TArray<FString> Keys,
	int32 IfChangedFromDataVersion)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetCharacterInternalData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);
	if (IfChangedFromDataVersion > 0) OutRestJsonObj->SetNumberField(TEXT("IfChangedFromDataVersion"), IfChangedFromDataVersion);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Get Character ReadOnly Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerGetCharacterReadOnlyData(FString PlayFabId, FString CharacterId, TArray<FString> Keys,
	int32 IfChangedFromDataVersion )
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetCharacterReadOnlyData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetStringArrayField(TEXT("Keys"), Keys);
	if (IfChangedFromDataVersion > 0) OutRestJsonObj->SetNumberField(TEXT("IfChangedFromDataVersion"), IfChangedFromDataVersion);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update Character Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerUpdateCharacterData(FString PlayFabId, FString CharacterId, UVaRestJsonObject* Data,
	EUserDataPermision::Type Permission)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateCharacterData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetObjectField(TEXT("Data"), Data);
	if (Permission == EUserDataPermision::Type::PUBLIC) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Public"));
	else OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Private"));

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update Character Internal Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerUpdateCharacterInternalData(FString PlayFabId, FString CharacterId, UVaRestJsonObject* Data,
	EUserDataPermision::Type Permission)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateCharacterInternalData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetObjectField(TEXT("Data"), Data);
	if (Permission == EUserDataPermision::Type::PUBLIC) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Public"));
	else OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Private"));

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}

/** Update Character ReadOnly Data */
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructServerUpdateCharacterReadOnlyData(FString PlayFabId, FString CharacterId, UVaRestJsonObject* Data,
	EUserDataPermision::Type Permission)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "UpdateCharacterReadOnlyData";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = false;
	manager->isServer = true;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("PlayFabId"), PlayFabId);
	OutRestJsonObj->SetStringField(TEXT("CharacterId"), CharacterId);
	OutRestJsonObj->SetObjectField(TEXT("Data"), Data);
	if (Permission == EUserDataPermision::Type::PUBLIC) OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Public"));
	else OutRestJsonObj->SetStringField(TEXT("Permission"), TEXT("Private"));

	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}




//// Admin
UVaRestPlayFabManager* UVaRestPlayFabManager::ConstructGetCloudScriptRevision(FString Version, FString Revision)
{
	// Object containing request data
	UVaRestPlayFabManager* manager = NewObject<UVaRestPlayFabManager>();
	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();

	// Setup request
	manager->SetVerb(ERequestVerb::POST);
	manager->SetContentType(ERequestContentType::json);

	// Setup the request variables based on this request
	manager->PlayFabClass = "GetCloudScriptRevision";
	manager->PlayFabSessionToken = "";
	manager->CloudScriptVersion = "";
	manager->isAdmin = true;
	manager->isServer = false;
	manager->isClient = false;
	manager->useSecretKey = true;
	manager->useSessionTicket = false;
	manager->cloudScript = false;

	// Setup request object
	OutRestJsonObj->SetStringField(TEXT("Version"), Version);
	OutRestJsonObj->SetStringField(TEXT("Revision"), Revision);


	// Add Request to manager
	manager->SetRequestObject(OutRestJsonObj);

	return manager;
}
