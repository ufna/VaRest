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
 * General helper class http requests via blueprints
 */
UCLASS(BlueprintType, Blueprintable)
class UVaRestRequestJSON : public UObject
{
	GENERATED_UCLASS_BODY()

public:
	//////////////////////////////////////////////////////////////////////////
	// Construction

	/** Creates new request (totally empty) */
	UFUNCTION(BlueprintPure, meta = (FriendlyName = "Construct Json Request (Empty)", HidePin = "WorldContextObject", DefaultToSelf = "WorldContextObject"), Category = "VaRest")
	static UVaRestRequestJSON* ConstructRequest(UObject* WorldContextObject);

	/** Creates new request with defined verb and content type */
	UFUNCTION(BlueprintPure, meta = (FriendlyName = "Construct Json Request", HidePin = "WorldContextObject", DefaultToSelf = "WorldContextObject"), Category = "VaRest")
	static UVaRestRequestJSON* ConstructRequestExt(UObject* WorldContextObject, ERequestVerb::Type Verb, ERequestContentType::Type ContentType);

	/** Set verb to the request */
	UFUNCTION(BlueprintCallable, Category = "VaRest")
	void SetVerb(ERequestVerb::Type Verb);

	/** Set content type to the request. If you're using the x-www-form-urlencoded, 
	 * params/constaints should be defined as key=ValueString pairs from Json data */
	UFUNCTION(BlueprintCallable, Category = "VaRest")
	void SetContentType(ERequestContentType::Type ContentType);

	/** Sets optional header info */
	UFUNCTION(BlueprintCallable, Category = "VaRest")
	void SetHeader(const FString& HeaderName, const FString& HeaderValue);

	/** Applies percent-encoding to text */
	UFUNCTION(BlueprintCallable, Category = "VaRest")
	static FString PercentEncode(const FString& Text);


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
	// JSON data accessors

	/** Get the Request Json object */
	UFUNCTION(BlueprintCallable, Category = "VaRest")
	UVaRestJsonObject* GetRequestObject();

	/** Set the Request Json object */
	UFUNCTION(BlueprintCallable, Category = "VaRest")
	void SetRequestObject(UVaRestJsonObject* JsonObject);

	/** Get the Response Json object */
	UFUNCTION(BlueprintCallable, Category = "VaRest")
	UVaRestJsonObject* GetResponseObject();

	/** Set the Response Json object */
	UFUNCTION(BlueprintCallable, Category = "VaRest")
	void SetResponseObject(UVaRestJsonObject* JsonObject);


	//////////////////////////////////////////////////////////////////////////
	// URL processing

	/** Open URL with current setup */
	UFUNCTION(BlueprintCallable, Category = "VaRest")
	virtual void ProcessURL(const FString& Url = TEXT("http://alyamkin.com"));

	/** Apply current internal setup to request and process it */
	void ProcessRequest(TSharedRef<IHttpRequest> HttpRequest);


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
	UVaRestJsonObject* RequestJsonObj;

	/** Responce data stored as JSON */
	UVaRestJsonObject* ResponseJsonObj;

	/** Verb for making request (GET,POST,etc) */
	ERequestVerb::Type RequestVerb;

	/** Content type to be applied for request */
	ERequestContentType::Type RequestContentType;

	/** Mapping of header section to values. Used to generate final header string for request */
	TMap<FString, FString> RequestHeaders;

};
