// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaRestJson.generated.h"

/** Type of the value a FVaRestJson handle points at. None = invalid handle (no value). */
UENUM(BlueprintType)
enum class EVaRestJsonType : uint8
{
	None,
	Null,
	Bool,
	Number,
	String,
	Array,
	Object
};

USTRUCT(BlueprintType, meta = (DisplayName = "VaRest Json"))
struct VARESTJSON_API FVaRestJson
{
	GENERATED_BODY()

	bool IsValid() const { return false; }
};
