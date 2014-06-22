// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPluginPrivatePCH.h"

UVaRestRequestJSON::UVaRestRequestJSON(const class FPostConstructInitializeProperties& PCIP)
	: Super(PCIP)
{
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

void UVaRestRequestJSON::SetVerb(ERequestVerb::Type Verb)
{

}

void UVaRestRequestJSON::SetContentType(ERequestContentType::Type ContentType)
{

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
	if (RequestJsonObj.IsValid())
	{
		RequestJsonObj.Reset();
	}

	RequestJsonObj = MakeShareable(new FJsonObject());
}

void UVaRestRequestJSON::ResetResponseData()
{
	if (ResponseJsonObj.IsValid())
	{
		ResponseJsonObj.Reset();
	}

	ResponseJsonObj = MakeShareable(new FJsonObject());
	bIsValidJsonResponse = false;
}


//////////////////////////////////////////////////////////////////////////
// Request callbacks

void UVaRestRequestJSON::OnProcessRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	// Be sure that we have no data from previous response
	ResetResponseData();

	// Save response data as a string
	ResponseContent = Response->GetContentAsString();

	// Log response state
	UE_LOG(LogVaRest, Log, TEXT("Response: %s"), *Response->GetContentAsString());

	// Check we have result to process futher
	if (!bWasSuccessful)
	{
		UE_LOG(LogVaRest, Error, TEXT("Request failed: %d"), Response->GetResponseCode());

		// Broadcast the result event
		OnRequestFail.Broadcast();

		return;
	}

	// Try to deserialize data to JSON
	TSharedRef<TJsonReader<TCHAR>> JsonReader = TJsonReaderFactory<TCHAR>::Create(ResponseContent);
	FJsonSerializer::Deserialize(JsonReader, ResponseJsonObj);

	// Decide whether the request was successful
	bIsValidJsonResponse = bWasSuccessful && ResponseJsonObj.IsValid();

	// Log errors
	if (!bIsValidJsonResponse)
	{
		if (!ResponseJsonObj.IsValid())
		{
			// As we assume it's recommended way to use current class, but not the only one,
			// it will be the warning instead of error
			UE_LOG(LogVaRest, Warning, TEXT("JSON could not be decoded!"));
		}
	}

	// Broadcast the result event
	OnRequestComplete.Broadcast();
}
