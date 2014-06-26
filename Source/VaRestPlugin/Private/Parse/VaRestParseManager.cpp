// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPluginPrivatePCH.h"

FString UVaRestParseManager::ParseURL(TEXT("https://api.parse.com/1/"));
FString UVaRestParseManager::ParseAppId(TEXT(""));
FString UVaRestParseManager::ParseApiKey(TEXT(""));

UVaRestParseManager::UVaRestParseManager(const class FPostConstructInitializeProperties& PCIP)
	: Super(PCIP)
{

}

UVaRestParseManager* UVaRestParseManager::ConstructParseRequest(
	UObject* WorldContextObject, 
	ERequestVerb::Type Verb,
	ERequestContentType::Type ContentType)
{
	return (UVaRestParseManager*)ConstructRequestExt(WorldContextObject, Verb, ContentType);
}

void UVaRestParseManager::ProcessParseURL(
	const FString& ParseModule, 
	const FString& ParseClass, 
	const FString& ParseObjectId, 
	const FString& ParseSessionToken)
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

	if (!ParseSessionToken.IsEmpty())
	{
		HttpRequest->SetHeader("X-Parse-Session-Token", ParseSessionToken);
	}

	ProcessRequest(HttpRequest);
}

void UVaRestParseManager::SetParseAuthData(FString AppId, FString ApiKey)
{
	ParseAppId = AppId;
	ParseApiKey = ApiKey;
}

//////////////////////////////////////////////////////////////////////////
// Quering helpers

FString UVaRestParseManager::ConstructPointer(const FString& ClassName, const FString& ObjectId)
{
	return FString::Printf(TEXT("{\"__type\":\"Pointer\",\"className\":\"%s\",\"objectId\":\"%s\"}"), *ClassName, *ObjectId);
}

FString UVaRestParseManager::ConstructWhereQuerySimple(const FString& Key, const FString& Value)
{
	return FString::Printf(TEXT("where={\"%s\":%s}"), *Key, *Value);
}

FString UVaRestParseManager::ConstructWhereQuery(UVaRestJsonObject* JsonObject)
{
	// Serialize json data to string
	FString OutputString;
	TSharedRef< TJsonWriter<> > Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject->GetRootObject().ToSharedRef(), Writer);

	return TEXT("where=") + OutputString;
}

FString UVaRestParseManager::GetHappyMessage()
{
	return FString("Happy New Year!");
}
