// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaRestHttpTestTypes.generated.h"

USTRUCT()
struct FVaRestHttpTestItem
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	int32 Level = 0;
};

USTRUCT()
struct FVaRestHttpTestProfile
{
	GENERATED_BODY()

	/** Past 2^53: must stay exact */
	UPROPERTY()
	int64 Id = 0;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	TArray<FVaRestHttpTestItem> Items;

	/** A default the JSON does not set */
	UPROPERTY()
	int32 Score = 7;
};
