// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestM5PolicyTestTypes.generated.h"

/** Cooked GUID-suffix field, matching the converter's accepted GUID-unavailable fallback. */
USTRUCT()
struct FVaRestM5PolicyPayload
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Value_1_00000000000000000000000000000001 = 7;
};
