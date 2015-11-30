// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestRequestJSON.generated.h"

/**
 * @author Original latent action class by https://github.com/unktomi
 */
template <class T> class FVaRestLatentAction : public FPendingLatentAction
{
public:
	virtual void Call(const T &Value) 
	{
		Result = Value;
		Called = true;
	}

	void operator()(const T &Value)
	{
		Call(Value);
	}

	void Cancel();
  
	FVaRestLatentAction(FWeakObjectPtr RequestObj, T& ResultParam, const FLatentActionInfo& LatentInfo) :
		Called(false),
		Request(RequestObj),
		ExecutionFunction(LatentInfo.ExecutionFunction),
		OutputLink(LatentInfo.Linkage),
		CallbackTarget(LatentInfo.CallbackTarget),
		Result(ResultParam)
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
	T &Result;

};

/** Verb (GET, PUT, POST) used by the request */
UENUM(BlueprintType)
namespace ERequestVerb
{
	enum Type
	{
		GET,
		POST,
		PUT,
		DEL UMETA(DisplayName="DELETE")
	};
}

/** Content type (json, urlencoded, etc.) used by the request */
UENUM(BlueprintType)
namespace ERequestContentType
{
	enum Type
	{
		x_www_form_urlencoded_url	UMETA(DisplayName = "x-www-form-urlencoded (URL)"),
		x_www_form_urlencoded_body	UMETA(DisplayName = "x-www-form-urlencoded (Request Body)"),
		json,
		binary
	};
}

/** Generate a delegates for callback events */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRequestComplete, class UVaRestRequestJSON*, Request);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRequestFail, class UVaRestRequestJSON*, Request);

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
	static UVaRestRequestJSON* ConstructRequestExt(UObject* WorldContextObject, ERequestVerb::Type Verb, ERequestContentType::Type ContentType);

	/** Set verb to the request */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	void SetVerb(ERequestVerb::Type Verb);

	/** Set content type to the request. If you're using the x-www-form-urlencoded, 
	 * params/constaints should be defined as key=ValueString pairs from Json data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	void SetContentType(ERequestContentType::Type ContentType);

	/** Set content type of the request for binary post data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	void SetBinaryContentType(const FString &ContentType);

	/** Set content of the request for binary post data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	void SetBinaryRequestContent(const TArray<uint8> &Content);

	/** Sets optional header info */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	void SetHeader(const FString& HeaderName, const FString& HeaderValue);

	/** Applies percent-encoding to text */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility")
	static FString PercentEncode(const FString& Text);


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
	// Response data access

	/** Get the responce code of the last query */
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

	/** Open URL with current setup */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request")
	virtual void ProcessURL(const FString& Url = TEXT("http://alyamkin.com"));

	/** Open URL in latent mode */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Request", meta = (Latent, LatentInfo = "LatentInfo", HidePin = "WorldContextObject", DefaultToSelf = "WorldContextObject"))
	virtual void ApplyURL(const FString& Url, UVaRestJsonObject *&Result, UObject* WorldContextObject, struct FLatentActionInfo LatentInfo);

	/** Apply current internal setup to request and process it */
	void ProcessRequest(TSharedRef<IHttpRequest> HttpRequest);


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


	//////////////////////////////////////////////////////////////////////////
	// Data

public:
	/** Request response stored as a string */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VaRest|Response")
	FString ResponseContent;

	/** Is the response valid JSON? */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VaRest|Response")
	bool bIsValidJsonResponse;

protected:
	/** Latent action helper */
	FVaRestLatentAction <UVaRestJsonObject*> *ContinueAction;

	/** Internal request data stored as JSON */
	UPROPERTY()
	UVaRestJsonObject* RequestJsonObj;

	UPROPERTY()
	TArray<uint8> RequestBytes;

	UPROPERTY()
	FString BinaryContentType;

	/** Response data stored as JSON */
	UPROPERTY()
	UVaRestJsonObject* ResponseJsonObj;

	/** Verb for making request (GET,POST,etc) */
	ERequestVerb::Type RequestVerb;

	/** Content type to be applied for request */
	ERequestContentType::Type RequestContentType;

	/** Mapping of header section to values. Used to generate final header string for request */
	TMap<FString, FString> RequestHeaders;

	/** Cached key/value header pairs. Parsed once request completes */
	TMap<FString, FString> ResponseHeaders;

	/** Http Response code */
	int32 ResponseCode;
};
