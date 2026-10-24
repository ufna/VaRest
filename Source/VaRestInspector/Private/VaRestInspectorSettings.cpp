// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspectorSettings.h"

#include "CoreGlobals.h"
#include "Math/UnrealMathUtility.h"
#include "VaRestInspectorPolicy.h"

UVaRestInspectorSettings::UVaRestInspectorSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("VaRestInspector");
	Packaged.BudgetMiB = 8;
	Packaged.MaxRecords = 2000;
	Packaged.MaxBodyBytes = 16384;
	Packaged.MaxBinaryExcerptBytes = 0;
	Packaged.MaxFrameExcerptBytes = 1024;
	Packaged.MaxFramesPerSocket = 1000;
	Packaged.MaxPendingEvents = 16384;
	Packaged.MaxPendingMiB = 4;
}

namespace VaRest::Inspector::Private
{
FNormalizedLimits NormalizeLimits(const FVaRestInspectorLimits& Limits)
{
	check(IsInGameThread());
	FNormalizedLimits Out;
	// Widen before multiplication; int32 MiB inputs fit int64 byte products.
	// Clamp before any allocation and leave the settings object unchanged.
	Out.StoreBytes = int64(FMath::Clamp(Limits.BudgetMiB, 1, 1024)) * 1024 * 1024;
	Out.MaxRecords = FMath::Clamp(Limits.MaxRecords, 1, 65536);
	Out.BodyBytes = FMath::Clamp(Limits.MaxBodyBytes, 0, 16 * 1024 * 1024);
	Out.BinaryBytes = FMath::Clamp(Limits.MaxBinaryExcerptBytes, 0, 16 * 1024 * 1024);
	Out.FrameBytes = FMath::Clamp(Limits.MaxFrameExcerptBytes, 0, 16 * 1024 * 1024);
	Out.MaxFramesPerSocket = FMath::Clamp(Limits.MaxFramesPerSocket, 1, 65536);
	Out.PendingEvents = FMath::Clamp(Limits.MaxPendingEvents, 1, 262144);
	Out.PendingBytes = FMath::Clamp(int64(Limits.MaxPendingMiB) * 1024 * 1024, 64ll * 1024, 256ll * 1024 * 1024);
	return Out;
}
} // namespace VaRest::Inspector::Private
