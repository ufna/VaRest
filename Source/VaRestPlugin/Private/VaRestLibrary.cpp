// Copyright 2016 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLibrary.h"
#include "Misc/Base64.h"
#include "VaRestJsonObject.h"
#include "VaRestPluginPrivatePCH.h"
#include "VaRestRequestJSON.h"

//////////////////////////////////////////////////////////////////////////
// Helpers

FString UVaRestLibrary::PercentEncode(const FString& Source)
{
	return FGenericPlatformHttp::UrlEncode(Source);
}

FString UVaRestLibrary::Base64Encode(const FString& Source)
{
	return FBase64::Encode(Source);
}

bool UVaRestLibrary::Base64Decode(const FString& Source, FString& Dest)
{
	return FBase64::Decode(Source, Dest);
}

bool UVaRestLibrary::Base64EncodeData(const TArray<uint8>& Data, FString& Dest)
{
	if (Data.Num() > 0)
	{
		Dest = FBase64::Encode(Data);
		return true;
	}

	return false;
}

bool UVaRestLibrary::Base64DecodeData(const FString& Source, TArray<uint8>& Dest)
{
	return FBase64::Decode(Source, Dest);
}

//////////////////////////////////////////////////////////////////////////
// File system integration

class UVaRestJsonObject* UVaRestLibrary::LoadJsonFromFile(UObject* WorldContextObject, const FString& Path, const bool bIsRelativeToContentDir)
{
	UVaRestJsonObject* Json = UVaRestJsonObject::ConstructJsonObject(WorldContextObject);

	FString JSONString;
	if (FFileHelper::LoadFileToString(JSONString, *(bIsRelativeToContentDir ? FPaths::ProjectContentDir() / Path : Path)))
	{
		if (Json->DecodeJson(JSONString))
		{
			return Json;
		}
		else
		{
			UE_LOG(LogVaRest, Error, TEXT("%s: Can't decode json from file %s"), *VA_FUNC_LINE, *Path);
		}
	}
	else
	{
		UE_LOG(LogVaRest, Error, TEXT("%s: Can't open file %s"), *VA_FUNC_LINE, *Path);
	}

	return nullptr;
}

//////////////////////////////////////////////////////////////////////////
// Easy URL processing

TMap<UVaRestRequestJSON*, FVaRestCallResponse> UVaRestLibrary::RequestMap;

void UVaRestLibrary::CallURL(UObject* WorldContextObject, const FString& URL, ERequestVerb Verb, ERequestContentType ContentType, UVaRestJsonObject* VaRestJson, const FVaRestCallDelegate& Callback)
{
	UWorld* World = GEngine->GetWorldFromContextObjectChecked(WorldContextObject);
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
