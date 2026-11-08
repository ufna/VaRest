// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "VaRestSioReferenceTestTypes.generated.h"

USTRUCT()
struct FVaRestSioReferenceInner
{
	GENERATED_BODY()
	UPROPERTY() TArray<uint8> Bytes;
};
USTRUCT()
struct FVaRestSioReferenceOuter
{
	GENERATED_BODY()
	UPROPERTY() FVaRestSioReferenceInner Inner;
};
