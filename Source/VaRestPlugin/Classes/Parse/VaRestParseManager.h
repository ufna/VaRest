// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestParseManager.generated.h"

/**
 * Helper class to make Parse communication easier
 */
UCLASS(BlueprintType, Blueprintable)
class UVaRestParseManager : public UVaRestRequestJSON
{
	GENERATED_UCLASS_BODY()

	/** A bit more happiness for our beautiful world */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Parse")
	static FString GetHappyMessage();

	/** Set Parse authenication data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Parse")
	static void SetParseAuthData(FString AppId, FString ApiKey);

	/** Creates new Parse request with defined verb and content type */
	UFUNCTION(BlueprintPure, meta = (FriendlyName = "Construct Parse Request", HidePin = "WorldContextObject", DefaultToSelf = "WorldContextObject"), Category = "VaRest|Parse")
	static UVaRestParseManager* ConstructParseRequest(UObject* WorldContextObject, ERequestVerb::Type Verb, ERequestContentType::Type ContentType);

	/** Open URL with current setup (with applied Parse auth data) */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Parse")
	void ProcessParseURL(const FString& ParseModule = TEXT("classes"), const FString& ParseClass = TEXT("GameScore"), const FString& ParseObjectId = "", const FString& ParseSessionToken = "");


	//////////////////////////////////////////////////////////////////////////
	// Internal data
private:
	/** Parse App Id */
	static FString ParseAppId;

	/** Parse Api Key */
	static FString ParseApiKey;

	/** Parse URL */
	static FString ParseURL;

};
