// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestDynamicResult.h"

namespace VaRest::Http
{
/** Immutable after publication. This exact record must remain reflection-free, including all future fields. */
struct FDynamicResultData
{
	~FDynamicResultData();
	FVaRestDynamicTypeInfo Type, ErrorType;
	FVaRestJson Json, ErrorJson;
	FString Text;
	TArray<uint8> Bytes;
	bool bHasBody = false;
#if WITH_DEV_AUTOMATION_TESTS
	mutable TSharedPtr<FVaRestDynamicReleaseProbe,ESPMode::ThreadSafe> ReleaseProbe;
#endif
};
struct FDynamicResultAccess
{
	static FVaRestDynamicResult Publish(TSharedRef<const FDynamicResultData, ESPMode::ThreadSafe> Data)
	{
		return FVaRestDynamicResult(MoveTemp(Data));
	}
};
}
