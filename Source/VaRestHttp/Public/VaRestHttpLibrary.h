// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "VaRestHttpTypes.h"
#include "VaRestHttpLibrary.generated.h"

UCLASS()
class VARESTHTTP_API UVaRestHttpLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/** Uses the native description and escapes controls for one display/log line. Error.Url is already redacted. */
	UFUNCTION(BlueprintPure,Category="VaRest|Http") static FString DescribeError(const FVaRestError& Error);
};
