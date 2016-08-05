// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPluginPrivatePCH.h"
#include "CoreMisc.h"

template <class T> void FVaRestLatentAction<T>::Cancel()
{
	UObject *Obj = Request.Get();
	if (Obj != nullptr)
	{
		((UVaRestRequestJSON*)Obj)->Cancel();
	}
}

UVaRestRequestJSON::UVaRestRequestJSON(const class FObjectInitializer& PCIP)
  : Super(PCIP),
    BinaryContentType(TEXT("application/octet-stream"))
{
	RequestVerb = ERequestVerb::GET;
	RequestContentType = ERequestContentType::x_www_form_urlencoded_url;

	ResetData();
}

UVaRestRequestJSON* UVaRestRequestJSON::ConstructRequest(UObject* WorldContextObject)
{
	return NewObject<UVaRestRequestJSON>();
}

UVaRestRequestJSON* UVaRestRequestJSON::ConstructRequestExt(
	UObject* WorldContextObject, 
	ERequestVerb Verb, 
	ERequestContentType ContentType)
{
	UVaRestRequestJSON* Request = ConstructRequest(WorldContextObject);

	Request->SetVerb(Verb);
	Request->SetContentType(ContentType);

	return Request;
}

void UVaRestRequestJSON::SetVerb(ERequestVerb Verb)
{
	RequestVerb = Verb;
}

void UVaRestRequestJSON::SetCustomVerb(FString Verb)
{
	CustomVerb = Verb;
}

void UVaRestRequestJSON::SetContentType(ERequestContentType ContentType)
{
	RequestContentType = ContentType;
}

void UVaRestRequestJSON::SetBinaryContentType(const FString &ContentType)
{
	BinaryContentType = ContentType;
}

void UVaRestRequestJSON::SetBinaryRequestContent(const TArray<uint8> &Bytes)
{
	RequestBytes = Bytes;
}

void UVaRestRequestJSON::SetHeader(const FString& HeaderName, const FString& HeaderValue)
{
	RequestHeaders.Add(HeaderName, HeaderValue);
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
	if (RequestJsonObj != nullptr)
	{
		RequestJsonObj->Reset();
	}
	else
	{
		RequestJsonObj = NewObject<UVaRestJsonObject>();
	}

	HttpRequest = FHttpModule::Get().CreateRequest();
}

void UVaRestRequestJSON::ResetResponseData()
{
	if (ResponseJsonObj != nullptr)
	{
		ResponseJsonObj->Reset();
	}
	else
	{
		ResponseJsonObj = NewObject<UVaRestJsonObject>();
	}

	ResponseHeaders.Empty();
	ResponseCode = -1;

	bIsValidJsonResponse = false;
}

void UVaRestRequestJSON::Cancel()
{
	ContinueAction = nullptr;

	ResetResponseData();
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


///////////////////////////////////////////////////////////////////////////
// Response data access

FString UVaRestRequestJSON::GetURL()
{
	return HttpRequest->GetURL();
}

ERequestStatus UVaRestRequestJSON::GetStatus()
{
	return ERequestStatus((uint8)HttpRequest->GetStatus());
}

int32 UVaRestRequestJSON::GetResponseCode()
{
	return ResponseCode;
}

FString UVaRestRequestJSON::GetResponseHeader(const FString HeaderName)
{
	FString Result;

	FString* Header = ResponseHeaders.Find(HeaderName);
	if (Header != nullptr)
	{
		Result = *Header;
	}

	return Result;
}

TArray<FString> UVaRestRequestJSON::GetAllResponseHeaders()
{
	TArray<FString> Result;
	for (TMap<FString, FString>::TConstIterator It(ResponseHeaders); It; ++It)
	{
		Result.Add(It.Key() + TEXT(": ") + It.Value());
	}
	return Result;
}


//////////////////////////////////////////////////////////////////////////
// URL processing

void UVaRestRequestJSON::ProcessURL(const FString& Url)
{
	HttpRequest->SetURL(Url);

	ProcessRequest();
}

void UVaRestRequestJSON::ApplyURL(const FString& Url, UVaRestJsonObject *&Result, UObject* WorldContextObject, FLatentActionInfo LatentInfo)
{
	HttpRequest->SetURL(Url);

	// Prepare latent action
	if (UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject))
	{
		FLatentActionManager& LatentActionManager = World->GetLatentActionManager();
		FVaRestLatentAction<UVaRestJsonObject*> *Kont = LatentActionManager.FindExistingAction<FVaRestLatentAction<UVaRestJsonObject*>>(LatentInfo.CallbackTarget, LatentInfo.UUID);

		if (Kont != nullptr)
		{
			Kont->Cancel();
			LatentActionManager.RemoveActionsForObject(LatentInfo.CallbackTarget);
		}

		LatentActionManager.AddNewAction(LatentInfo.CallbackTarget, LatentInfo.UUID, ContinueAction = new FVaRestLatentAction<UVaRestJsonObject*>(this, Result, LatentInfo));
	}

	ProcessRequest();
}

void UVaRestRequestJSON::ProcessRequest()
{
	// Set verb
	switch (RequestVerb)
	{
	case ERequestVerb::GET:
		HttpRequest->SetVerb(TEXT("GET"));
		break;

	case ERequestVerb::POST:
		HttpRequest->SetVerb(TEXT("POST"));
		break;

	case ERequestVerb::PUT:
		HttpRequest->SetVerb(TEXT("PUT"));
		break;
			
	case ERequestVerb::DEL:
		HttpRequest->SetVerb(TEXT("DELETE"));
		break;

	case ERequestVerb::CUSTOM:
		HttpRequest->SetVerb(CustomVerb);
		break;

	default:
		break;
	}

	// Set content-type
	switch (RequestContentType)
	{
	case ERequestContentType::x_www_form_urlencoded_url:
	{
		HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/x-www-form-urlencoded"));

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
				UrlParams += UVaRestLibrary::PercentEncode(Key) + "=" + UVaRestLibrary::PercentEncode(Value);
			}

			ParamIdx++;
		}

		// Apply params
		HttpRequest->SetURL(HttpRequest->GetURL() + UrlParams);

		UE_LOG(LogVaRest, Log, TEXT("Request (urlencoded): %s %s %s"), *HttpRequest->GetVerb(), *HttpRequest->GetURL(), *UrlParams);

		break;
	}
	case ERequestContentType::x_www_form_urlencoded_body:
	{
		HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/x-www-form-urlencoded"));

		FString UrlParams = "";
		uint16 ParamIdx = 0;

		// Loop through all the values and prepare additional url part
		for (auto RequestIt = RequestJsonObj->GetRootObject()->Values.CreateIterator(); RequestIt; ++RequestIt)
		{
			FString Key = RequestIt.Key();
			FString Value = RequestIt.Value().Get()->AsString();

			if (!Key.IsEmpty() && !Value.IsEmpty())
			{
				UrlParams += ParamIdx == 0 ? "" : "&";
				UrlParams += UVaRestLibrary::PercentEncode(Key) + "=" + UVaRestLibrary::PercentEncode(Value);
			}

			ParamIdx++;
		}

		// Apply params
		HttpRequest->SetContentAsString(UrlParams);

		UE_LOG(LogVaRest, Log, TEXT("Request (url body): %s %s %s"), *HttpRequest->GetVerb(), *HttpRequest->GetURL(), *UrlParams);

		break;
	}
	case ERequestContentType::binary:
	{
		HttpRequest->SetHeader(TEXT("Content-Type"), BinaryContentType);
		HttpRequest->SetContent(RequestBytes);

		UE_LOG(LogVaRest, Log, TEXT("Request (binary): %s %s"), *HttpRequest->GetVerb(), *HttpRequest->GetURL());

		break;
	}
	case ERequestContentType::json:
	{
		HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

		// Serialize data to json string
		FString OutputString;
		TSharedRef< TJsonWriter<> > Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(RequestJsonObj->GetRootObject().ToSharedRef(), Writer);

		// Set Json content
		HttpRequest->SetContentAsString(OutputString);

		UE_LOG(LogVaRest, Log, TEXT("Request (json): %s %s %s"), *HttpRequest->GetVerb(), *HttpRequest->GetURL(), *OutputString);

		break;
	}

	default:
		break;
	}

	// Apply additional headers
	for (TMap<FString, FString>::TConstIterator It(RequestHeaders); It; ++It)
	{
		HttpRequest->SetHeader(It.Key(), It.Value());
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

	// Check we have a response and save response code as int32
	if(Response.IsValid())
	{
		ResponseCode = Response->GetResponseCode();
	}

	// Check we have result to process futher
	if (!bWasSuccessful || !Response.IsValid())
	{
		UE_LOG(LogVaRest, Error, TEXT("Request failed (%d): %s"), ResponseCode, *Request->GetURL());

		// Broadcast the result event
		OnRequestFail.Broadcast(this);
		OnStaticRequestFail.Broadcast(this);

		return;
	}

	// Save response data as a string
	ResponseContent = Response->GetContentAsString();

	// Log response state
	UE_LOG(LogVaRest, Log, TEXT("Response (%d): %s"), ResponseCode, *ResponseContent);

	// Process response headers
	TArray<FString> Headers = Response->GetAllHeaders();
	for (FString Header : Headers)
	{
		FString Key;
		FString Value;
		if (Header.Split(TEXT(": "), &Key, &Value))
		{
			ResponseHeaders.Add(Key, Value);
		}
	}

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
	OnRequestComplete.Broadcast(this);
	OnStaticRequestComplete.Broadcast(this);

	// Finish the latent action
	if (ContinueAction)
	{
          FVaRestLatentAction<UVaRestJsonObject*> *K = ContinueAction;
          ContinueAction = nullptr;

          K->Call(ResponseJsonObj);
	}
}


//////////////////////////////////////////////////////////////////////////
// Tags

void UVaRestRequestJSON::AddTag(FName Tag)
{
	if (Tag != NAME_None)
	{
		Tags.AddUnique(Tag);
	}
}

int32 UVaRestRequestJSON::RemoveTag(FName Tag)
{
	return Tags.Remove(Tag);
}

bool UVaRestRequestJSON::HasTag(FName Tag) const
{
	return (Tag != NAME_None) && Tags.Contains(Tag);
}
