// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestRequestJSON.generated.h"

/** Verb (GET, PUT, POST) used by the request */
UENUM(BlueprintType)
namespace ERequestVerb
{
	enum Type
	{
		GET,
		POST,
		PUT
	};
}

/** Content type (json, urlencoded, etc.) used by the request */
UENUM(BlueprintType)
namespace ERequestContentType
{
	enum Type
	{
		x_www_form_urlencoded,
		json
	};
}

/** Generate a delegates for callback events */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnRequestComplete);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnRequestFail);

/**
 * Genaral helper class http requests via blueprints
 */
UCLASS(BlueprintType, Blueprintable)
class UVaRestRequestJSON : public UObject
{
	GENERATED_UCLASS_BODY()

public:
	//////////////////////////////////////////////////////////////////////////
	// Construction

	/** Creates new request (totally empty) */
	UFUNCTION(BlueprintPure, meta = (FriendlyName = "Construct JSON Request (Empty)", HidePin = "WorldContextObject", DefaultToSelf = "WorldContextObject"), Category = "VaRest")
	static UVaRestRequestJSON* ConstructRequest(UObject* WorldContextObject);

	/** Creates new request with defined verb and content type */
	UFUNCTION(BlueprintPure, meta = (FriendlyName = "Construct JSON Request", HidePin = "WorldContextObject", DefaultToSelf = "WorldContextObject"), Category = "VaRest")
	static UVaRestRequestJSON* ConstructRequestExt(UObject* WorldContextObject, ERequestVerb::Type Verb, ERequestContentType::Type ContentType);

	/** Set verb to the request */
	UFUNCTION(BlueprintCallable, Category = "VaRest")
	void SetVerb(ERequestVerb::Type NewVerb);

	/** Set content type to the request */
	UFUNCTION(BlueprintCallable, Category = "VaRest")
	void SetContentType(ERequestContentType::Type NewContentType);


	//////////////////////////////////////////////////////////////////////////
	// Destruction and reset

	/** Reset all internal saved data */
	UFUNCTION(BlueprintCallable, Category = "VaRest")
	void ResetData();

	/** Reset saved request data */
	UFUNCTION(BlueprintCallable, Category = "VaRest")
	void ResetRequestData();

	/** Reset saved response data */
	UFUNCTION(BlueprintCallable, Category = "VaRest")
	void ResetResponseData();


	//////////////////////////////////////////////////////////////////////////
	// JSON API

	// @TODO

	//////////////////////////////////////////////////////////////////////////
	// Request callbacks

private:
	/** Internal bind function for the IHTTPRequest::OnProcessRequestCompleted() event */
	void OnProcessRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

public:
	/** Event occured when the request has been completed */
	UPROPERTY(BlueprintAssignable, Category = "VaRest")
	FOnRequestComplete OnRequestComplete;

	/** Event occured when the request wasn't successfull */
	UPROPERTY(BlueprintAssignable, Category = "VaRest")
	FOnRequestComplete OnRequestFail;


	//////////////////////////////////////////////////////////////////////////
	// Data

public:
	/** Request response stored as a string */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VaRest")
	FString ResponseContent;

	/** Is the response valid JSON? */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VaRest")
	bool bIsValidJsonResponse;

private:
	/** Internal request data stored as JSON */
	TSharedPtr<FJsonObject> RequestJsonObj;

	/** Responce data stored as JSON */
	TSharedPtr<FJsonObject> ResponseJsonObj;

	/** Parameters to be applied to the http request */
	ERequestVerb::Type Verb;
	ERequestContentType::Type ContentType;

};