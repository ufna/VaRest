// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "Containers/UnrealString.h"
#include "UObject/NameTypes.h"

enum class EVaRestErrorKind : uint8;

struct FVaRestInspectorErrorCounts
{
	uint64 Canceled = 0, Connection = 0, Timeout = 0, Http = 0, Server = 0;
	uint64 Decode = 0, Auth = 0, Offline = 0, Config = 0, Unknown = 0;
	bool bSaturated = false;
	VARESTINSPECTOR_API void Add(EVaRestErrorKind Kind);
};

enum class EVaRestInspectorQuantileKind : uint8
{
	Unknown,
	Zero,
	LogInterval,
	Underflow,
	Overflow,
	Saturated
};
struct FVaRestInspectorQuantile
{
	EVaRestInspectorQuantileKind Kind = EVaRestInspectorQuantileKind::Unknown;
	double Value = 0, Lower = 0, Upper = 0;
	bool bHasValue = false, bLowerInclusive = true, bUpperInclusive = false;
};

// Observations accumulated during this capture, independently of retained rows.
// Snapshot visitors borrow this complete immutable value; copying it explicitly
// gives the caller its own FString allocation.
struct FVaRestInspectorHttpStats
{
	uint64 ScopeId = 0, ClientId = 0;
	FName Endpoint;
	FString Key;
	bool bOverflow = false, bSaturated = false, bWireBytesUnknown = false, bDurationCountsSaturated = false;
	uint64 Started = 0, Completed = 0, Success = 0, Error = 0, Queued = 0, Canceled = 0;
	uint64 Dispatches = 0, Retries = 0, AuthReplays = 0, RetryScheduled = 0;
	uint64 UnknownDispatchClassification = 0;
	uint64 CacheHit = 0, CacheDisk = 0, Revalidated = 0, Stale = 0, Replayed = 0;
	uint64 WireBytesUp = 0, WireBytesDown = 0, Partial = 0, Incomplete = 0, Dropped = 0;
	FVaRestInspectorErrorCounts ErrorsByKind;
	uint64 Histogram[512] = {};
	uint64 DurationCount = 0, DurationUnderflow = 0, DurationOverflow = 0, DurationUnknown = 0, DurationNonFinite = 0;
	double DurationSum = 0, DurationMin = 0, DurationMax = 0;
	// Rational rank, max(1, ceil(DurationCount * Numerator / Denominator)).
	// Exceptional intervals expose a range without an ordinary-bin estimate.
	VARESTINSPECTOR_API FVaRestInspectorQuantile Quantile(uint32 Numerator, uint32 Denominator) const;
};
