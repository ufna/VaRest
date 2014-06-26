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

	/** Creates new Parse request with defined verb and content type */
	UFUNCTION(BlueprintPure, meta = (FriendlyName = "Construct Parse Request", HidePin = "WorldContextObject", DefaultToSelf = "WorldContextObject"), Category = "VaRest|Parse")
	static UVaRestParseManager* ConstructParseRequest(UObject* WorldContextObject, ERequestVerb::Type Verb, ERequestContentType::Type ContentType);

	/** Open the Parse URL (Attn.!! App Id and Api Key should be set before) */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Parse")
	void ProcessParseURL(const FString& ParseModule = TEXT("classes"), const FString& ParseClass = TEXT("GameScore"), const FString& ParseObjectId = "", const FString& ParseSessionToken = "");

	/** Set Parse authenication data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Parse")
	static void SetParseAuthData(FString AppId, FString ApiKey);


	//////////////////////////////////////////////////////////////////////////
	// Quering helpers

	/** Create Json record that contains Pointer to the Parse Object */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Parse")
	static FString ConstructPointer(const FString& ClassName, const FString& ObjectId);

	/** Construct simple WHERE query that contains only one condition.
	 * Attn!! String Values should containt quotes! */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Parse")
	static FString ConstructWhereQuerySimple(const FString& Key, const FString& Value);

	/** Construct WHERE query based on conditions defined as Json */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Parse")
	static FString ConstructWhereQuery(UVaRestJsonObject* JsonObject);

	/** A bit more happiness for our beautiful world */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Parse")
	static FString GetHappyMessage();


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
