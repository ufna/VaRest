// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Engine/LatentActionManager.h"
#include "Http.h"
#include "LatentActions.h"

#include "VaRestTypes.h"

#include "VaRestRequestJSON.generated.h"

/**
 * @author Original latent action class by https://github.com/unktomi
 */
template <class T>
class FVaRestLatentAction : public FPendingLatentAction
{
public:
	virtual void Call(const T& Value)
	{
		Result = Value;
		Called = true;
	}

	void operator()(const T& Value)
	{
		Call(Value);
	}

	void Cancel();

	FVaRestLatentAction(FWeakObjectPtr RequestObj, T& ResultParam, const FLatentActionInfo& LatentInfo)
		: Called(false)
		, Request(RequestObj)
		, ExecutionFunction(LatentInfo.ExecutionFunction)
		, OutputLink(LatentInfo.Linkage)
		, CallbackTarget(LatentInfo.CallbackTarget)
		, Result(ResultParam)
	{
	}

	virtual void UpdateOperation(FLatentResponse& Response) override
	{
		Response.FinishAndTriggerIf(Called, ExecutionFunction, OutputLink, CallbackTarget);
	}

	virtual void NotifyObjectDestroyed()
	{
		Cancel();
	}

	virtual void NotifyActionAborted()
	{
		Cancel();
	}

private:
	bool Called;
	FWeakObjectPtr Request;

public:
	const FName ExecutionFunction;
	const int32 OutputLink;
	const FWeakObjectPtr CallbackTarget;
	T& Result;
};

/** Generate a delegates for callback events */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRequestComplete, class UVaRestRequestJSON*, Request);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRequestFail, class UVaRestRequestJSON*, Request);

DECLARE_MULTICAST_DELEGATE_OneParam(FOnStaticRequestComplete, class UVaRestRequestJSON*);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnStaticRequestFail, class UVaRestRequestJSON*);

/**
 * General helper class http requests via blueprints
 */
UCLASS(BlueprintType, Blueprintable)
class VARESTPLUGIN_API UVaRestRequestJSON : public UObject
{
	GENERATED_UCLASS_BODY()

public:
	//////////////////////////////////////////////////////////////////////////
	// Construction

	/** Creates new request (totally empty) */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Construct Json Request (Empty)", HidePin = "WorldContextObject", DefaultToSelf = "WorldContextObject"), Category = "VaRest|Request")
	static UVaRestRequestJSON* ConstructRequest(UObject* WorldContextObject);

	/** Creates new request with defined verb and content type */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Construct Json Request", HidePin = "WorldContextObject", DefaultToSelf = "WorldContextObject"), Category = "VaRest|Request")
	static UVaRestRequestJSON* ConstructRequestExt(UObject* WorldContextObject, ERequestVerb Verb, ERequestContentType ContentType);

	/** Set verb to the request */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	void SetVerb(ERequestVerb Verb);

	/** Set custom verb to the request */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	void SetCustomVerb(FString Verb);

	/** Set content type to the request. If you're using the x-www-form-urlencoded, 
	 * params/constaints should be defined as key=ValueString pairs from Json data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	void SetContentType(ERequestContentType ContentType);

	/** Set content type of the request for binary post data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	void SetBinaryContentType(const FString& ContentType);

	/** Set content of the request for binary post data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	void SetBinaryRequestContent(const TArray<uint8>& Content);

	/** Set content of the request as a plain string */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	void SetStringRequestContent(const FString& Content);

	/** Sets optional header info */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	void SetHeader(const FString& HeaderName, const FString& HeaderValue);

	//////////////////////////////////////////////////////////////////////////
	// Destruction and reset

	/** Reset all internal saved data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility")
	void ResetData();

	/** Reset saved request data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	void ResetRequestData();

	/** Reset saved response data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Response")
	void ResetResponseData();

	/** Cancel latent response waiting  */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Response")
	void Cancel();

	//////////////////////////////////////////////////////////////////////////
	// JSON data accessors

	/** Get the Request Json object */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	UVaRestJsonObject* GetRequestObject();

	/** Set the Request Json object */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	void SetRequestObject(UVaRestJsonObject* JsonObject);

	/** Get the Response Json object */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Response")
	UVaRestJsonObject* GetResponseObject();

	/** Set the Response Json object */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Response")
	void SetResponseObject(UVaRestJsonObject* JsonObject);

	///////////////////////////////////////////////////////////////////////////
	// Request/response data access

	/** Get url of http request */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	FString GetURL();

	/** Get status of http request */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	ERequestStatus GetStatus();

	/** Get the response code of the last query */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Response")
	int32 GetResponseCode();

	/** Get value of desired response header */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Response")
	FString GetResponseHeader(const FString HeaderName);

	/** Get list of all response headers */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Response")
	TArray<FString> GetAllResponseHeaders();

	//////////////////////////////////////////////////////////////////////////
	// URL processing

public:
	/** Setting request URL */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	void SetURL(const FString& Url = TEXT("http://alyamkin.com"));

	/** Open URL with current setup */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	virtual void ProcessURL(const FString& Url = TEXT("http://alyamkin.com"));

	/** Open URL in latent mode */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request", meta = (Latent, LatentInfo = "LatentInfo", HidePin = "WorldContextObject", DefaultToSelf = "WorldContextObject"))
	virtual void ApplyURL(const FString& Url, UVaRestJsonObject*& Result, UObject* WorldContextObject, struct FLatentActionInfo LatentInfo);

	/** Check URL and execute process request */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	virtual void ExecuteProcessRequest();

protected:
	/** Apply current internal setup to request and process it */
	void ProcessRequest();

	//////////////////////////////////////////////////////////////////////////
	// Request callbacks

private:
	/** Internal bind function for the IHTTPRequest::OnProcessRequestCompleted() event */
	void OnProcessRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

public:
	/** Event occured when the request has been completed */
	UPROPERTY(BlueprintAssignable, Category = "VaRest|Event")
	FOnRequestComplete OnRequestComplete;

	/** Event occured when the request wasn't successfull */
	UPROPERTY(BlueprintAssignable, Category = "VaRest|Event")
	FOnRequestFail OnRequestFail;

	/** Event occured when the request has been completed */
	FOnStaticRequestComplete OnStaticRequestComplete;

	/** Event occured when the request wasn't successfull */
	FOnStaticRequestFail OnStaticRequestFail;

	//////////////////////////////////////////////////////////////////////////
	// Tags

public:
	/** Add tag to this request */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility")
	void AddTag(FName Tag);

	/** 
	 * Remove tag from this request 
	 *
	 * @return Number of removed elements 
	 */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility")
	int32 RemoveTag(FName Tag);

	/** See if this request contains the supplied tag */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility")
	bool HasTag(FName Tag) const;

protected:
	/** Array of tags that can be used for grouping and categorizing */
	TArray<FName> Tags;

	//////////////////////////////////////////////////////////////////////////
	// Data

public:
	/**
	 * Get request response stored as a string 
	 * @param bCacheResponseContent - Set true if you plan to use it few times to prevent deserialization each time
	 */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Response")
	FString GetResponseContentAsString(bool bCacheResponseContent = true);

public:
	/** Response size */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VaRest|Response")
	int32 ResponseSize;

	/** DEPRECATED: Please use GetResponseContentAsString() instead */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VaRest|Response")
	FString ResponseContent;

	/** Is the response valid JSON? */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VaRest|Response")
	bool bIsValidJsonResponse;

protected:
	/** Default value for deprecated ResponseContent variable */
	static FString DeprecatedResponseString;

protected:
	/** Latent action helper */
	FVaRestLatentAction<UVaRestJsonObject*>* ContinueAction;

	/** Internal request data stored as JSON */
	UPROPERTY()
	UVaRestJsonObject* RequestJsonObj;

	TArray<uint8> RequestBytes;
	FString BinaryContentType;

	/** Used for special cases when used wants to have plain string data in request. 
	 * Attn.! Content-type x-www-form-urlencoded only. */
	FString StringRequestContent;

	/** Response data stored as JSON */
	UPROPERTY()
	UVaRestJsonObject* ResponseJsonObj;

	/** Verb for making request (GET,POST,etc) */
	ERequestVerb RequestVerb;

	/** Content type to be applied for request */
	ERequestContentType RequestContentType;

	/** Mapping of header section to values. Used to generate final header string for request */
	TMap<FString, FString> RequestHeaders;

	/** Cached key/value header pairs. Parsed once request completes */
	TMap<FString, FString> ResponseHeaders;

	/** Http Response code */
	int32 ResponseCode;

	/** Custom verb that will be used with RequestContentType == CUSTOM */
	FString CustomVerb;

	/** Request we're currently processing */
	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();

public:
	/** Returns reference to internal request object */
	TSharedRef<IHttpRequest> GetHttpRequest() const { return HttpRequest; };
};
