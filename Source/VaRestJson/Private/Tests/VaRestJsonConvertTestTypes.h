// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestJson.h"

#include "VaRestJsonConvertTestTypes.generated.h"

// Test-only types. Like VaRestJsonTestTypes.h they ship in the module's reflection data until the VaRest-rfd
// M3 item moves test types out of the runtime module.

USTRUCT()
struct FVaRestJsonTestInner
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Id = 0;

	UPROPERTY()
	FString Name;
};

USTRUCT()
struct FVaRestJsonTestNaming
{
	GENERATED_BODY()

	UPROPERTY()
	int32 UserId = 0;

	UPROPERTY()
	bool bIsActive = false;

	UPROPERTY()
	FString URLPath;

	UPROPERTY()
	int32 HTTPStatus2 = 0;

	UPROPERTY()
	FString user_name;

	UPROPERTY(Transient)
	int32 TransientValue = 0;
};

/** Both fields become "userId" in camelCase: the second is skipped */
USTRUCT()
struct FVaRestJsonTestCollision
{
	GENERATED_BODY()

	UPROPERTY()
	int32 user_id = 1;

	UPROPERTY()
	int32 UserId = 2;
};

/** Fields encode/decode do not convert: each is skipped with one warning per field-map build */
USTRUCT()
struct FVaRestJsonTestSkipped
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Kept = 0;

	UPROPERTY()
	TObjectPtr<UObject> ObjectRef;

	UPROPERTY()
	TMap<FGuid, int32> GuidKeys;

	UPROPERTY()
	TArray<TObjectPtr<UObject>> Objects;
};

/** Decode accepts a bool's AsIs spelling ("bOn") only for a field whose 'b' the naming policy dropped */
USTRUCT()
struct FVaRestJsonTestBoolPrefix
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOn = false;

	UPROPERTY()
	bool Old = false;
};
