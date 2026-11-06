// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "VaRestOpenApiModel.h"

struct FVaRestOpenApiEnumValue
{
	FString InternalName,DisplayName,WireValue;
	bool bUnset=false;
};

struct VARESTOPENAPI_API FVaRestOpenApiEnumPlan
{
	TArray<FVaRestOpenApiEnumValue> Values;
	FString StringFallbackReason;
	bool bStringFallback=false,bHasUnset=false;
	/** Missing default selects unset only for a use that requests it; explicit defaults always match exact wire text. */
	TOptional<FString> DefaultInternalName(const FVaRestJson& Default,bool bUnsetWhenAbsent,bool bAllowNull=false) const;
};

struct VARESTOPENAPI_API FVaRestOpenApiEnumPlanner final
{
	/** bNeedsUnset is combined over all uses of a shared enum before its asset is built. Param-only enums pass false. */
	static FVaRestOpenApiEnumPlan Build(const FVaRestJson& StringValues,bool bNeedsUnset,bool bEnumsAsString=false);
};
