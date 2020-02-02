// Copyright 2014-2020 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSubsystem.h"

#include "VaRestDefines.h"
#include "VaRestJsonObject.h"
#include "VaRestSettings.h"

#include "Developer/Settings/Public/ISettingsModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "FVaRest"

UVaRestSubsystem::UVaRestSubsystem()
	: UGameInstanceSubsystem()
{
}

void UVaRestSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Settings = NewObject<UVaRestSettings>(GetTransientPackage(), "VaRestSettings", RF_Standalone);

	// Register settings
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->RegisterSettings("Project", "Plugins", "VaRest",
			LOCTEXT("RuntimeSettingsName", "VaRest"),
			LOCTEXT("RuntimeSettingsDescription", "Configure VaRest plugin settings"),
			Settings);
	}

	UE_LOG(LogVaRest, Log, TEXT("%s: VaRest subsystem initialized"), *VA_FUNC_LINE);
}

void UVaRestSubsystem::Deinitialize()
{
	// Do nothing for now
	Super::Deinitialize();
}

void UVaRestSubsystem::CallURL(const FString& URL, ERequestVerb Verb, ERequestContentType ContentType, UVaRestJsonObject* VaRestJson, const FVaRestCallDelegate& Callback)
{
	UWorld* World = GetWorld();
	check(World);

	// Check we have valid data json
	if (VaRestJson == nullptr)
	{
		VaRestJson = ConstructVaRestJsonObject();
	}

	UVaRestRequestJSON* Request = ConstructVaRestRequest();

	Request->SetVerb(Verb);
	Request->SetContentType(ContentType);
	Request->SetRequestObject(VaRestJson);

	FVaRestCallResponse Response;
	Response.Request = Request;
	Response.Callback = Callback;

	Response.CompleteDelegateHandle = Request->OnStaticRequestComplete.AddUObject(this, &UVaRestSubsystem::OnCallComplete);
	Response.FailDelegateHandle = Request->OnStaticRequestFail.AddUObject(this, &UVaRestSubsystem::OnCallComplete);

	RequestMap.Add(Request, Response);

	Request->ResetResponseData();
	Request->ProcessURL(URL);
}

void UVaRestSubsystem::OnCallComplete(UVaRestRequestJSON* Request)
{
	if (!RequestMap.Contains(Request))
	{
		return;
	}

	auto Response = RequestMap.Find(Request);
	Request->OnStaticRequestComplete.Remove(Response->CompleteDelegateHandle);
	Request->OnStaticRequestFail.Remove(Response->FailDelegateHandle);

	Response->Callback.ExecuteIfBound(Request);
	Response->Request = nullptr;
	RequestMap.Remove(Request);
}

UVaRestRequestJSON* UVaRestSubsystem::ConstructVaRestRequest()
{
	return NewObject<UVaRestRequestJSON>(this);
}

UVaRestRequestJSON* UVaRestSubsystem::ConstructVaRestRequestExt(ERequestVerb Verb, ERequestContentType ContentType)
{
	UVaRestRequestJSON* Request = ConstructVaRestRequest();

	Request->SetVerb(Verb);
	Request->SetContentType(ContentType);

	return Request;
}

UVaRestJsonObject* UVaRestSubsystem::ConstructVaRestJsonObject()
{
	return NewObject<UVaRestJsonObject>(this);
}

class UVaRestJsonObject* UVaRestSubsystem::LoadJsonFromFile(const FString& Path, const bool bIsRelativeToContentDir)
{
	auto* Json = ConstructVaRestJsonObject();

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

UVaRestSettings* UVaRestSubsystem::GetSettings() const
{
	check(Settings);
	return Settings;
}

#undef LOCTEXT_NAMESPACE
