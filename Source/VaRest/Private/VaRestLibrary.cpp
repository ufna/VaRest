// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLibrary.h"

#include "VaRestDefines.h"
#include "VaRestJsonObject.h"
#include "VaRestRequestJSON.h"

#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

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
	UVaRestJsonObject* Json = UVaRestJsonObject::ConstructVaRestJsonObject(WorldContextObject);

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
