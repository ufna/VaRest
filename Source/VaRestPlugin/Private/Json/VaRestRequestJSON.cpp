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
	ERequestVerb::Type Verb, 
	ERequestContentType::Type ContentType)
{
	UVaRestRequestJSON* Request = ConstructRequest(WorldContextObject);

	Request->SetVerb(Verb);
	Request->SetContentType(ContentType);

	return Request;
}

void UVaRestRequestJSON::SetVerb(ERequestVerb::Type Verb)
{
	RequestVerb = Verb;
}

void UVaRestRequestJSON::SetContentType(ERequestContentType::Type ContentType)
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

FString UVaRestRequestJSON::PercentEncode(const FString& Text)
{
	FString OutText = Text;

	OutText = OutText.Replace(TEXT(" "), TEXT("%20"));
	OutText = OutText.Replace(TEXT("!"), TEXT("%21"));
	OutText = OutText.Replace(TEXT("\""), TEXT("%22"));
	OutText = OutText.Replace(TEXT("#"), TEXT("%23"));
	OutText = OutText.Replace(TEXT("$"), TEXT("%24"));
	OutText = OutText.Replace(TEXT("&"), TEXT("%26"));
	OutText = OutText.Replace(TEXT("'"), TEXT("%27"));
	OutText = OutText.Replace(TEXT("("), TEXT("%28"));
	OutText = OutText.Replace(TEXT(")"), TEXT("%29"));
	OutText = OutText.Replace(TEXT("*"), TEXT("%2A"));
	OutText = OutText.Replace(TEXT("+"), TEXT("%2B"));
	OutText = OutText.Replace(TEXT(","), TEXT("%2C"));
	OutText = OutText.Replace(TEXT("/"), TEXT("%2F"));
	OutText = OutText.Replace(TEXT(":"), TEXT("%3A"));
	OutText = OutText.Replace(TEXT(";"), TEXT("%3B"));
	OutText = OutText.Replace(TEXT("="), TEXT("%3D"));
	OutText = OutText.Replace(TEXT("?"), TEXT("%3F"));
	OutText = OutText.Replace(TEXT("@"), TEXT("%40"));
	OutText = OutText.Replace(TEXT("["), TEXT("%5B"));
	OutText = OutText.Replace(TEXT("]"), TEXT("%5D"));
	OutText = OutText.Replace(TEXT("{"), TEXT("%7B"));
	OutText = OutText.Replace(TEXT("}"), TEXT("%7D"));

	return OutText;
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
		RequestJsonObj = NewObject<UVaRestJsonObject>();
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

int32 UVaRestRequestJSON::GetResponseCode()
{
	return ResponseCode;
}

FString UVaRestRequestJSON::GetResponseHeader(const FString HeaderName)
{
	FString Result;

	FString* Header = ResponseHeaders.Find(HeaderName);
	if (Header != NULL)
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
	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(Url);

	ProcessRequest(HttpRequest);
}

void UVaRestRequestJSON::ApplyURL(const FString& Url, UVaRestJsonObject *&Result, UObject* WorldContextObject, FLatentActionInfo LatentInfo)
{
	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(Url);

	// Prepare latent action
	if (UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject))
	{
		FLatentActionManager& LatentActionManager = World->GetLatentActionManager();
		FVaRestLatentAction<UVaRestJsonObject*> *Kont = LatentActionManager.FindExistingAction<FVaRestLatentAction<UVaRestJsonObject*>>(LatentInfo.CallbackTarget, LatentInfo.UUID);

		if (Kont != NULL)
		{
			Kont->Cancel();
			LatentActionManager.RemoveActionsForObject(LatentInfo.CallbackTarget);
		}

		LatentActionManager.AddNewAction(LatentInfo.CallbackTarget, LatentInfo.UUID, ContinueAction = new FVaRestLatentAction<UVaRestJsonObject*>(this, Result, LatentInfo));
	}

	ProcessRequest(HttpRequest);
}

void UVaRestRequestJSON::ProcessRequest(TSharedRef<IHttpRequest> HttpRequest)
{
	// Set verb
	switch (RequestVerb)
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
			
	case ERequestVerb::DEL:
		HttpRequest->SetVerb("DELETE");
		break;

	default:
		break;
	}

	// Set content-type
	switch (RequestContentType)
	{
	case ERequestContentType::x_www_form_urlencoded_url:
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
				UrlParams += UVaRestRequestJSON::PercentEncode(Key) + "=" + UVaRestRequestJSON::PercentEncode(Value);
			}

			ParamIdx++;
		}

		// Apply params
		HttpRequest->SetURL(HttpRequest->GetURL() + UrlParams);

		break;
	}
	case ERequestContentType::x_www_form_urlencoded_body:
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
				UrlParams += ParamIdx == 0 ? "" : "&";
				UrlParams += UVaRestRequestJSON::PercentEncode(Key) + "=" + UVaRestRequestJSON::PercentEncode(Value);
			}

			ParamIdx++;
		}

		// Apply params
		HttpRequest->SetContentAsString(UrlParams);

		break;
	}
	case ERequestContentType::binary:
	{
		HttpRequest->SetHeader("Content-Type", BinaryContentType);
		HttpRequest->SetContent(RequestBytes);

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

	// Check we have result to process futher
	if (!bWasSuccessful)
	{
		UE_LOG(LogVaRest, Error, TEXT("Request failed: %s"), *Request->GetURL());

		// Broadcast the result event
		OnRequestFail.Broadcast(this);

		return;
	}

	// Save response data as a string
	ResponseContent = Response->GetContentAsString();

	// Save response code as int32
	ResponseCode = Response->GetResponseCode();

	// Log response state
	UE_LOG(LogVaRest, Log, TEXT("Response (%d): %s"), Response->GetResponseCode(), *Response->GetContentAsString());

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

	// Finish the latent action
	if (ContinueAction)
	{
          FVaRestLatentAction<UVaRestJsonObject*> *K = ContinueAction;
          ContinueAction = nullptr;

          K->Call(ResponseJsonObj);
	}
}
