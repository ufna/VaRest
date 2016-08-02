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
 * Usefull tools for REST communications
 */
UCLASS()
class UVaRestLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()


	//////////////////////////////////////////////////////////////////////////
	// Helpers

public:
	/** Applies percent-encoding to text */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility")
	static FString PercentEncode(const FString& Text);


	//////////////////////////////////////////////////////////////////////////
	// Easy URL processing

public:
	/** Easy way to process http requests */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Library", meta = (WorldContext = "WorldContextObject"))
	static void CallURL(UObject* WorldContextObject, const FString& URL, ERequestVerb Verb, ERequestContentType ContentType, UVaRestJsonObject* VaRestJson, const FVaRestCallDelegate& Callback);

	/** Called when URL is processed (one for both success/unsuccess events)*/
	static void OnCallComplete(UVaRestRequestJSON* Request);

private:
	static TMap<UVaRestRequestJSON*, FVaRestCallResponse> RequestMap;

};
