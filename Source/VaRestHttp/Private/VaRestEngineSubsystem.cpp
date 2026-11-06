// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestEngineSubsystem.h"

#include "CoreGlobals.h"
#include "VaRestApi.h"
#include "VaRestHttpLog.h"
#include "VaRestHttpSettings.h"
#include "VaRestHub.h"

void UVaRestEngineSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// Cooking gets the explicit soft package list through ModifyCook; loading it here would mask that dependency proof.
	if (IsRunningCookCommandlet())
	{
		return;
	}
	for (const TSoftObjectPtr<UVaRestApi>& Entry : GetDefault<UVaRestHttpSettings>()->Apis)
	{
		if (Entry.IsNull())
		{
			UE_LOG(LogVaRestHttp, Warning, TEXT("VaRest Settings.Apis contains an empty entry"));
			continue;
		}
		UVaRestApi* Api = Entry.LoadSynchronous();
		if (!Api)
		{
			UE_LOG(LogVaRestHttp, Warning, TEXT("VaRest could not load configured API asset %s"), *Entry.ToSoftObjectPath().ToString());
			continue;
		}
		LoadedApis.AddUnique(Api); // UPROPERTY owns the asset for the registration lifetime, before GI creation
	}
	Hub=NewObject<UVaRestHub>(this);
	FString Error;
	if (!Hub->InitializeOnGT(nullptr,true,Error)) { UE_LOG(LogVaRestHttp,Warning,TEXT("VaRest fallback Hub refused: %s"),*Error); Hub=nullptr; }
}

void UVaRestEngineSubsystem::Deinitialize()
{
	if (Hub) Hub->DeinitializeOnGT();
	Hub=nullptr;
	LoadedApis.Reset();
	Super::Deinitialize();
}
