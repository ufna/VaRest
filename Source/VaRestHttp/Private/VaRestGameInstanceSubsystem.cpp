// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestGameInstanceSubsystem.h"
#include "VaRestHub.h"
#include "VaRestHttpLog.h"

void UVaRestGameInstanceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Hub=NewObject<UVaRestHub>(this);
	FString Error;
	if (!Hub->InitializeOnGT(GetGameInstance(),false,Error))
	{
		UE_LOG(LogVaRestHttp,Warning,TEXT("VaRest Hub initialization refused: %s"),*Error);
		Hub=nullptr;
	}
}
void UVaRestGameInstanceSubsystem::Deinitialize()
{
	if (Hub) Hub->DeinitializeOnGT();
	Hub=nullptr; Super::Deinitialize();
}
