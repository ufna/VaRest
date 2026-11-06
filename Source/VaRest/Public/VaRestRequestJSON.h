// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Engine/LatentActionManager.h"
#include "Http.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "LatentActions.h"

#include "VaRestTypes.h"
#include "VaRestClient.h"

#include "VaRestRequestJSON.generated.h"

class UVaRestJsonValue;
class UVaRestJsonObject;
class UVaRestSettings;
class UVaRestHub;

/**
 * @author Original latent action class by https://github.com/unktomi
 */
template <class T>
class FVaRestLatentAction : public FPendingLatentAction
{
public:
	virtual void Call(const T& Value)
	{
		if (bCancelled) return;
		Result = Value;
		Called = true;
	}

	void operator()(const T& Value)
	{
		Call(Value);
	}

	void Cancel();
	void Abort() { bCancelled=true; }

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
		if (bCancelled) Response.DoneIf(true);else Response.FinishAndTriggerIf(Called, ExecutionFunction, OutputLink, CallbackTarget);
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
	bool bCancelled=false;
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
class VAREST_API UVaRestRequestJSON : public UObject
{
	GENERATED_UCLASS_BODY()

public:
	//////////////////////////////////////////////////////////////////////////
	// Construction

	/** Set verb to the request */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request Verb input, or an API endpoint Verb.", BlueprintInternalUseOnly="true"))
	void SetVerb(EVaRestRequestVerb Verb);

	/** Set custom verb to the request */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request Verb input.", BlueprintInternalUseOnly="true"))
	void SetCustomVerb(FString Verb);

	/** Set content type to the request. If you're using the x-www-form-urlencoded,
	 * params/constaints should be defined as key=ValueString pairs from Json data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request Body Kind input.", BlueprintInternalUseOnly="true"))
	void SetContentType(EVaRestRequestContentType ContentType);

	/** Set content type of the request for binary post data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request Content Type input.", BlueprintInternalUseOnly="true"))
	void SetBinaryContentType(const FString& ContentType);

	/** Set content of the request for binary post data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request with Binary body.", BlueprintInternalUseOnly="true"))
	void SetBinaryRequestContent(const TArray<uint8>& Content);

	/** Set content of the request as a plain string */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request with Text or Form body.", BlueprintInternalUseOnly="true"))
	void SetStringRequestContent(const FString& Content);

	/** Sets optional header info */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request Headers input, or client session headers.", BlueprintInternalUseOnly="true"))
	void SetHeader(const FString& HeaderName, const FString& HeaderValue);

	//////////////////////////////////////////////////////////////////////////
	// Destruction and reset

	/** Reset all internal saved data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility", meta=(DeprecatedFunction, DeprecationMessage="Use v2 start a new Send Request invocation.", BlueprintInternalUseOnly="true"))
	void ResetData();

	/** Reset saved request data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request", meta=(DeprecatedFunction, DeprecationMessage="Use v2 build a new Send Request body and headers.", BlueprintInternalUseOnly="true"))
	void ResetRequestData();

	/** Reset saved response data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Response", meta=(DeprecatedFunction, DeprecationMessage="Use v2 replace the stored response/result variable.", BlueprintInternalUseOnly="true"))
	void ResetResponseData();

	/** Cancel latent response waiting  */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Response", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Cancel on VaRest Request Handle.", BlueprintInternalUseOnly="true"))
	void Cancel();

	//////////////////////////////////////////////////////////////////////////
	// JSON data accessors

	/** Get the Request Json object */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request", meta=(DeprecatedFunction, DeprecationMessage="Use v2 keep the VaRest Json body passed to Send Request.", BlueprintInternalUseOnly="true"))
	UVaRestJsonObject* GetRequestObject() const;

	/** Set the Request Json object */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request Body input.", BlueprintInternalUseOnly="true"))
	void SetRequestObject(UVaRestJsonObject* JsonObject);

	/** Get the Response Json object */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Response", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request JSON output.", BlueprintInternalUseOnly="true"))
	UVaRestJsonObject* GetResponseObject() const;

	/** Set the Response Json object */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Response", meta=(DeprecatedFunction, DeprecationMessage="Use v2 assign a VaRest Json variable.", BlueprintInternalUseOnly="true"))
	void SetResponseObject(UVaRestJsonObject* JsonObject);

	/** Get the Response Json value */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Response", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request JSON output.", BlueprintInternalUseOnly="true"))
	UVaRestJsonValue* GetResponseValue() const;

	///////////////////////////////////////////////////////////////////////////
	// Request/response data access

	/** Get url of http request */
	UFUNCTION(BlueprintPure, Category = "VaRest|Request", meta=(DeprecatedFunction, DeprecationMessage="Use v2 response metadata and the configured client/endpoint URL.", BlueprintInternalUseOnly="true"))
	FString GetURL() const;

	/** Get verb to the request */
	UFUNCTION(BlueprintPure, Category = "VaRest|Request", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request Verb input or API endpoint Verb.", BlueprintInternalUseOnly="true"))
	EVaRestRequestVerb GetVerb() const;

	/** Get status of http request */
	UFUNCTION(BlueprintPure, Category = "VaRest|Request", meta=(DeprecatedFunction, DeprecationMessage="Use v2 On Success / On Error and response metadata.", BlueprintInternalUseOnly="true"))
	EVaRestRequestStatus GetStatus() const;

	/** Get the response code of the last query */
	UFUNCTION(BlueprintPure, Category = "VaRest|Response", meta=(DeprecatedFunction, DeprecationMessage="Use v2 response Status.", BlueprintInternalUseOnly="true"))
	int32 GetResponseCode() const;

	/** Get value of desired response header */
	UFUNCTION(BlueprintPure, Category = "VaRest|Response", meta=(DeprecatedFunction, DeprecationMessage="Use v2 response Headers.", BlueprintInternalUseOnly="true"))
	FString GetResponseHeader(const FString& HeaderName);

	/** Get list of all response headers */
	UFUNCTION(BlueprintPure, Category = "VaRest|Response", meta=(DeprecatedFunction, DeprecationMessage="Use v2 response Headers.", BlueprintInternalUseOnly="true"))
	TArray<FString> GetAllResponseHeaders() const;

	/**
	 * Shortcut to get the Content-Length header value. Will not always return non-zero.
	 * If you want the real length of the payload, get the payload and check it's length.
	 *
	 * @return the content length (if available)
	 */
	UFUNCTION(BlueprintPure, Category = "VaRest|Response", meta=(DeprecatedFunction, DeprecationMessage="Use v2 response Headers or response BytesDown.", BlueprintInternalUseOnly="true"))
	int32 GetResponseContentLength() const;

	/**
	 * Get the content payload of the request or response.
	 *
	 * @param Content - array that will be filled with the content.
	 */
	UFUNCTION(BlueprintPure, Category = "VaRest|Response", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request Bytes output.", BlueprintInternalUseOnly="true"))
	const TArray<uint8>& GetResponseContent() const;

	//////////////////////////////////////////////////////////////////////////
	// URL processing

public:
	/** Setting request URL */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request Path input, or an API environment BaseUrl.", BlueprintInternalUseOnly="true"))
	void SetURL(const FString& Url = TEXT("http://alyamkin.com"));

	/** Open URL with current setup */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request.", BlueprintInternalUseOnly="true"))
	virtual void ProcessURL(const FString& Url = TEXT("http://alyamkin.com"));

	/** Open URL in latent mode */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request", meta = (Latent, LatentInfo = "LatentInfo", HidePin = "WorldContextObject", DefaultToSelf = "WorldContextObject", DeprecatedFunction, DeprecationMessage="Use v2 Send Request async completion pins.", BlueprintInternalUseOnly="true"))
	virtual void ApplyURL(const FString& Url, UVaRestJsonObject*& Result, UObject* WorldContextObject, struct FLatentActionInfo LatentInfo);

	/** Check URL and execute process request */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request.", BlueprintInternalUseOnly="true"))
	virtual void ExecuteProcessRequest();

protected:
	/** Apply current internal setup to request and process it */
	void ProcessRequest();

	//////////////////////////////////////////////////////////////////////////
	// Request callbacks

private:
	template <class T> friend class FVaRestLatentAction;
	friend class FVaRestModule;
	static void ShutdownRequestsOnGT();
	/** Internal bind function for the IHTTPRequest::OnProcessRequestCompleted() event */
	void OnProcessRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	void FinishCoreOnGT(uint64 Epoch,FVaRestResponse Response,EVaRestErrorKind Kind);
	void RetireCoreOnGT(bool bClearContinuation);
	void FreshEngineRequestOnGT();

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
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request Tag input.", BlueprintInternalUseOnly="true"))
	void AddTag(FName Tag);

	/**
	 * Remove tag from this request
	 *
	 * @return Number of removed elements
	 */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility", meta=(DeprecatedFunction, DeprecationMessage="Use v2 set Tag on the next Send Request invocation.", BlueprintInternalUseOnly="true"))
	int32 RemoveTag(FName Tag);

	/** See if this request contains the supplied tag */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility", meta=(DeprecatedFunction, DeprecationMessage="Use v2 keep application tag state; use request Tag in metadata.", BlueprintInternalUseOnly="true"))
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
	UFUNCTION(BlueprintCallable, Category = "VaRest|Response", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request String output, or To String (Json).", BlueprintInternalUseOnly="true"))
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

	/** Raw response storage */
	TArray<uint8> ResponseBytes;
	int32 ResponseContentLength;

	/** Used for special cases when used wants to have plain string data in request.
	 * Attn.! Content-type x-www-form-urlencoded only. */
	FString StringRequestContent;

	/** Response data stored as JSON */
	UPROPERTY()
	UVaRestJsonObject* ResponseJsonObj;

	/** Response data stored as JSON value */
	UPROPERTY()
	UVaRestJsonValue* ResponseJsonValue;

	/** Verb for making request (GET,POST,etc) */
	EVaRestRequestVerb RequestVerb;

	/** Content type to be applied for request */
	EVaRestRequestContentType RequestContentType;

	/** Mapping of header section to values. Used to generate final header string for request */
	TMap<FString, FString> RequestHeaders;

	/** Cached key/value header pairs. Parsed once request completes */
	TMap<FString, FString> ResponseHeaders;

	/** Http Response code */
	int32 ResponseCode;

	/** Custom verb that will be used with RequestContentType == CUSTOM */
	FString CustomVerb;

	/** Request we're currently processing */
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	FVaRestRequestHandle CoreHandle;
	TWeakPtr<IHttpRequest,ESPMode::ThreadSafe> ActiveEngineRequest;
	TWeakObjectPtr<UVaRestHub> CoreHub;
	TWeakObjectPtr<UObject> LegacyWorldContext;
	uint64 CoreRegistration=0,CoreEpoch=0,ContinueEpoch=0;
	EVaRestRequestStatus CoreStatus=EVaRestRequestStatus::NotStarted;
	FString InputUrl,PreparedUrl;

public:
	virtual void BeginDestroy() override;
	/** Returns reference to internal request object */
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> GetHttpRequest() const { return HttpRequest; };
};
