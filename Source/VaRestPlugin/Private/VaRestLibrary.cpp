// Copyright 2016 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPluginPrivatePCH.h"
#include "Base64.h"

//////////////////////////////////////////////////////////////////////////
// Helpers

FString UVaRestLibrary::PercentEncode(const FString& Source)
{
	FString OutText = Source;

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

FString UVaRestLibrary::Base64Encode(const FString& Source)
{
	return FBase64::Encode(Source);
}

bool UVaRestLibrary::Base64Decode(const FString& Source, FString& Dest)
{
	return FBase64::Decode(Source, Dest);
}


//////////////////////////////////////////////////////////////////////////
// Easy URL processing

TMap<UVaRestRequestJSON*, FVaRestCallResponse> UVaRestLibrary::RequestMap;

void UVaRestLibrary::CallURL(UObject* WorldContextObject, const FString& URL, ERequestVerb Verb, ERequestContentType ContentType, UVaRestJsonObject* VaRestJson, const FVaRestCallDelegate& Callback)
{
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject);
	if (World == nullptr)
	{
		UE_LOG(LogVaRest, Error, TEXT("UVaRestLibrary: Wrong world context"))
		return;
	}

	// Check we have valid data json
	if (VaRestJson == nullptr)
	{
		VaRestJson = UVaRestJsonObject::ConstructJsonObject(WorldContextObject);
	}
	
	UVaRestRequestJSON* Request = NewObject<UVaRestRequestJSON>();
	
	Request->SetVerb(Verb);
	Request->SetContentType(ContentType);
	Request->SetRequestObject(VaRestJson);
	
	FVaRestCallResponse Response;
	Response.Request = Request;
	Response.WorldContextObject = WorldContextObject;
	Response.Callback = Callback;
	
	Response.CompleteDelegateHandle = Request->OnStaticRequestComplete.AddStatic(&UVaRestLibrary::OnCallComplete);
	Response.FailDelegateHandle = Request->OnStaticRequestFail.AddStatic(&UVaRestLibrary::OnCallComplete);
	
	RequestMap.Add(Request, Response);
	
	Request->ResetResponseData();
	Request->ProcessURL(URL);
}

void UVaRestLibrary::OnCallComplete(UVaRestRequestJSON* Request)
{
	if (!RequestMap.Contains(Request))
	{
		return;
	}
	
	FVaRestCallResponse* Response = RequestMap.Find(Request);
	
	Request->OnStaticRequestComplete.Remove(Response->CompleteDelegateHandle);
	Request->OnStaticRequestFail.Remove(Response->FailDelegateHandle);
	
	Response->Callback.ExecuteIfBound(Request);
	
	Response->WorldContextObject = nullptr;
	Response->Request = nullptr;
	RequestMap.Remove(Request);
}
