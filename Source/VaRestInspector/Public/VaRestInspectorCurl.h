// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "VaRestInspectorRecords.h"

/** Copies only redacted capture data, synchronously while the borrowed view is valid. Never executes curl. */
struct VARESTINSPECTOR_API FVaRestInspectorCurl
{
	static FString Build(const FVaRestInspectorRecordView& Row, uint64 DispatchId = 0);
};
