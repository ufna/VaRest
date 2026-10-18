// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestJson.h"

#include "VaRestJsonTestTypes.generated.h"

/** Test-only: a handle as a field between two others, to prove ImportTextItem consumes exactly its value */
USTRUCT()
struct FVaRestJsonTestHolder
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Before = 0;

	UPROPERTY()
	FVaRestJson Json;

	UPROPERTY()
	int32 After = 0;
};
