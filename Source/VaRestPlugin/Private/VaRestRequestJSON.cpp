// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPluginPrivatePCH.h"

UVaRestRequestJSON::UVaRestRequestJSON(const class FPostConstructInitializeProperties& PCIP)
	: Super(PCIP)
{
	Verb = ERequestVerb::GET;
	ContentType = ERequestContentType::x_www_form_urlencoded;

	ResetData();
}

UVaRestRequestJSON* UVaRestRequestJSON::ConstructRequest(UObject* WorldContextObject)
{
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject);

	return (UVaRestRequestJSON*)StaticConstructObject(UVaRestRequestJSON::StaticClass());
}

UVaRestRequestJSON* UVaRestRequestJSON::ConstructRequestExt(UObject* WorldContextObject, ERequestVerb::Type Verb, ERequestContentType::Type ContentType)
{
	UVaRestRequestJSON* Request = ConstructRequest(WorldContextObject);

	Request->SetVerb(Verb);
	Request->SetContentType(ContentType);

	return Request;
}

void UVaRestRequestJSON::SetVerb(ERequestVerb::Type NewVerb)
{
	Verb = NewVerb;
}

void UVaRestRequestJSON::SetContentType(ERequestContentType::Type NewContentType)
{
	ContentType = NewContentType;
}


//////////////////////////////////////////////////////////////////////////
// Destruction and reset

void UVaRestRequestJSON::ResetData()
{
	ResetRequestData();
	ResetResponseData();
}

void UVaRestRequestJSON::ResetRequestData()
{
	if (RequestJsonObj != NULL)
	{
		RequestJsonObj->Reset();
	}
	else
	{
		RequestJsonObj = (UVaRestJsonObject*)StaticConstructObject(UVaRestJsonObject::StaticClass());
	}
}

void UVaRestRequestJSON::ResetResponseData()
{
	if (ResponseJsonObj != NULL)
	{
		ResponseJsonObj->Reset();
	}
	else
	{
		ResponseJsonObj = (UVaRestJsonObject*)StaticConstructObject(UVaRestJsonObject::StaticClass());
	}

	bIsValidJsonResponse = false;
}


//////////////////////////////////////////////////////////////////////////
// JSON data accessors

UVaRestJsonObject* UVaRestRequestJSON::GetRequestObject()
{
	return RequestJsonObj;
}

void UVaRestRequestJSON::SetRequestObject(UVaRestJsonObject* JsonObject)
{
	RequestJsonObj = JsonObject;
}

UVaRestJsonObject* UVaRestRequestJSON::GetResponseObject()
{
	return ResponseJsonObj;
}

void UVaRestRequestJSON::SetResponseObject(UVaRestJsonObject* JsonObject)
{
	ResponseJsonObj = JsonObject;
}


//////////////////////////////////////////////////////////////////////////
// URL processing

void UVaRestRequestJSON::ProcessURL(const FString& Url)
{
	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(Url);

	ProcessRequest(HttpRequest);
}

void UVaRestRequestJSON::ProcessRequest(TSharedRef<IHttpRequest> HttpRequest)
{
	// Set verb
	switch (Verb)
	{
	case ERequestVerb::GET:
		HttpRequest->SetVerb("GET");
		break;

	case ERequestVerb::POST:
		HttpRequest->SetVerb("POST");
		break;

	case ERequestVerb::PUT:
		HttpRequest->SetVerb("PUT");
		break;

	default:
		break;
	}

	// Set content-type
	switch (ContentType)
	{
	case ERequestContentType::x_www_form_urlencoded:
	{
		HttpRequest->SetHeader("Content-Type", "application/x-www-form-urlencoded");

		FString UrlParams = "";
		uint16 ParamIdx = 0;

		// Loop through all the values and prepare additional url part
		for (auto RequestIt = RequestJsonObj->GetRootObject()->Values.CreateIterator(); RequestIt; ++RequestIt)
		{
			FString Key = RequestIt.Key();
			FString Value = RequestIt.Value().Get()->AsString();

			if (!Key.IsEmpty() && !Value.IsEmpty())
			{
				UrlParams += ParamIdx == 0 ? "?" : "&";
				UrlParams += Key + "=" + Value;
			}

			ParamIdx++;
		}

		// Apply params to the url
		HttpRequest->SetURL(HttpRequest->GetURL() + UrlParams);

		break;
	}

	case ERequestContentType::json:
	{
		HttpRequest->SetHeader("Content-Type", "application/json");

		// Serialize data to json string
		FString OutputString;
		TSharedRef< TJsonWriter<> > Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(RequestJsonObj->GetRootObject().ToSharedRef(), Writer);

		// Set Json content
		HttpRequest->SetContentAsString(OutputString);

		break;
	}

	default:
		break;
	}
	
	// Bind event
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UVaRestRequestJSON::OnProcessRequestComplete);

	// Execute the request
	HttpRequest->ProcessRequest();
}


//////////////////////////////////////////////////////////////////////////
// Request callbacks

void UVaRestRequestJSON::OnProcessRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	// Be sure that we have no data from previous response
	ResetResponseData();

	// Check we have result to process futher
	if (!bWasSuccessful)
	{
		UE_LOG(LogVaRest, Error, TEXT("Request failed: %s"), *Request->GetURL());

		// Broadcast the result event
		OnRequestFail.Broadcast();

		return;
	}

	// Save response data as a string
	ResponseContent = Response->GetContentAsString();

	// Log response state
	UE_LOG(LogVaRest, Log, TEXT("Response (%d): %s"), Response->GetResponseCode(), *Response->GetContentAsString());

	// Try to deserialize data to JSON
	TSharedRef<TJsonReader<TCHAR>> JsonReader = TJsonReaderFactory<TCHAR>::Create(ResponseContent);
	FJsonSerializer::Deserialize(JsonReader, ResponseJsonObj->GetRootObject());

	// Decide whether the request was successful
	bIsValidJsonResponse = bWasSuccessful && ResponseJsonObj->GetRootObject().IsValid();

	// Log errors
	if (!bIsValidJsonResponse)
	{
		if (!ResponseJsonObj->GetRootObject().IsValid())
		{
			// As we assume it's recommended way to use current class, but not the only one,
			// it will be the warning instead of error
			UE_LOG(LogVaRest, Warning, TEXT("JSON could not be decoded!"));
		}
	}

	// Broadcast the result event
	OnRequestComplete.Broadcast();
}
