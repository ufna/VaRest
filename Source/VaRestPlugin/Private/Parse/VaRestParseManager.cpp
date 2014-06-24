// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPluginPrivatePCH.h"

FString UVaRestParseManager::ParseURL(TEXT("https://api.parse.com/1/"));
FString UVaRestParseManager::ParseAppId(TEXT(""));
FString UVaRestParseManager::ParseApiKey(TEXT(""));

UVaRestParseManager::UVaRestParseManager(const class FPostConstructInitializeProperties& PCIP)
	: Super(PCIP)
{

}

FString UVaRestParseManager::GetHappyMessage()
{
	return FString("Happy New Year!");
}

void UVaRestParseManager::SetParseAuthData(FString AppId, FString ApiKey)
{
	ParseAppId = AppId;
	ParseApiKey = ApiKey;
}

UVaRestParseManager* UVaRestParseManager::ConstructParseRequest(UObject* WorldContextObject, ERequestVerb::Type Verb, ERequestContentType::Type ContentType)
{
	return (UVaRestParseManager*)ConstructRequestExt(WorldContextObject, Verb, ContentType);
}

void UVaRestParseManager::ProcessParseURL(const FString& ParseModule, const FString& ParseClass, const FString& ParseObjectId)
{
	FString RequestUrl = ParseURL;

	// Build the full request url
	if (!ParseModule.IsEmpty())
	{
		RequestUrl += ParseModule;

		if (!ParseClass.IsEmpty())
		{
			RequestUrl += "/" + ParseClass;

			if (!ParseObjectId.IsEmpty())
			{
				RequestUrl += "/" + ParseObjectId;
			}
		}
	}

	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(RequestUrl);

	HttpRequest->SetHeader("X-Parse-Application-Id", ParseAppId);
	HttpRequest->SetHeader("X-Parse-REST-API-Key", ParseApiKey);

	ProcessRequest(HttpRequest);
}
