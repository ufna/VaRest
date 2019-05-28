// Copyright 2016 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"

#include "VaRestTypes.h"

#include "VaRestLibrary.generated.h"

class UVaRestRequestJSON;
class UVaRestJsonObject;

DECLARE_DYNAMIC_DELEGATE_OneParam(FVaRestCallDelegate, UVaRestRequestJSON*, Request);

USTRUCT()
struct FVaRestCallResponse
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY()
	UVaRestRequestJSON* Request;

	UPROPERTY()
	UObject* WorldContextObject;

	UPROPERTY()
	FVaRestCallDelegate Callback;

	FDelegateHandle CompleteDelegateHandle;
	FDelegateHandle FailDelegateHandle;

	FVaRestCallResponse()
		: Request(nullptr)
		, WorldContextObject(nullptr)
	{
	}
};

/**
 * Useful tools for REST communications
 */
UCLASS()
class VARESTPLUGIN_API UVaRestLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

	//////////////////////////////////////////////////////////////////////////
	// Helpers

public:
	/** Applies percent-encoding to text */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility")
	static FString PercentEncode(const FString& Source);

	/**
	 * Encodes a FString into a Base64 string
	 *
	 * @param Source	The string data to convert
	 * @return			A string that encodes the binary data in a way that can be safely transmitted via various Internet protocols
	 */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility", meta = (DisplayName = "Base64 Encode"))
	static FString Base64Encode(const FString& Source);

	/**
	 * Decodes a Base64 string into a FString
	 *
	 * @param Source	The stringified data to convert
	 * @param Dest		The out buffer that will be filled with the decoded data
	 * @return			True if the buffer was decoded, false if it failed to decode
	 */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility", meta = (DisplayName = "Base64 Decode"))
	static bool Base64Decode(const FString& Source, FString& Dest);

	/**
	 * Encodes a byte array into a Base64 string
	 *
	 * @param Dara		The data to convert
	 * @return			A string that encodes the binary data in a way that can be safely transmitted via various Internet protocols
	 */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility", meta = (DisplayName = "Base64 Encode Data"))
	static bool Base64EncodeData(const TArray<uint8>& Data, FString& Dest);

	/**
	 * Decodes a Base64 string into a byte array
	 *
	 * @param Source	The stringified data to convert
	 * @param Dest		The out buffer that will be filled with the decoded data
	 * @return			True if the buffer was decoded, false if it failed to decode
	 */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility", meta = (DisplayName = "Base64 Decode Data"))
	static bool Base64DecodeData(const FString& Source, TArray<uint8>& Dest);

	//////////////////////////////////////////////////////////////////////////
	// File system integration

public:
	/** 
	 * Load JSON from formatted text file
	 * @param    bIsRelativeToContentDir    if set to 'false' path is treated as absolute
	 */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility", meta = (WorldContext = "WorldContextObject"))
	static class UVaRestJsonObject* LoadJsonFromFile(UObject* WorldContextObject, const FString& Path, const bool bIsRelativeToContentDir = true);

	//////////////////////////////////////////////////////////////////////////
	// Easy URL processing

public:
	/** Easy way to process http requests */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility", meta = (WorldContext = "WorldContextObject"))
	static void CallURL(UObject* WorldContextObject, const FString& URL, ERequestVerb Verb, ERequestContentType ContentType, UVaRestJsonObject* VaRestJson, const FVaRestCallDelegate& Callback);

	/** Called when URL is processed (one for both success/unsuccess events)*/
	static void OnCallComplete(UVaRestRequestJSON* Request);

private:
	static TMap<UVaRestRequestJSON*, FVaRestCallResponse> RequestMap;
};
