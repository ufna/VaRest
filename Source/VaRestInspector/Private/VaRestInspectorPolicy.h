// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "VaRestTrace.h"

struct FVaRestInspectorLimits;

namespace VaRest::Inspector::Private
{
struct FNormalizedLimits
{
	int64 StoreBytes = 64ll * 1024 * 1024;
	int32 MaxRecords = 10000;
	int32 BodyBytes = 65536, BinaryBytes = 0, FrameBytes = 4096;
	int32 MaxFramesPerSocket = 5000;
	int32 PendingEvents = 65536;
	int64 PendingBytes = 16ll * 1024 * 1024;
	static constexpr int32 MaxHeaders = 256, MaxWarnings = 32;
	static constexpr int32 MaxStringChars = 16384;
	static constexpr int64 MaxMetadataBytes = 1024 * 1024;
	static constexpr int32 DispatchSlots = 16, TimelineSlots = 128, NoteSlots = 64;
	static constexpr int32 Clients = 1024, StatsGroups = 1024, KeysPerClient = 256;
	static constexpr int32 Links = 4096, LinksPerTarget = 64, Tombstones = 4096;
	static constexpr int32 SnapshotControllers = 4;
};
struct FCapturePolicy
{
	FNormalizedLimits Limits;
	bool bHeaders = true;
	bool bBodies = true;
	FVaRestTraceDetailSettings Detail() const
	{
		FVaRestTraceDetailSettings Out;
		Out.Flags = EVaRestTraceDetail::Metadata;
		if (bHeaders)
			Out.Flags |= EVaRestTraceDetail::Headers;
		if (bBodies)
			Out.Flags |= EVaRestTraceDetail::Bodies;
		Out.BodyCap = Limits.BodyBytes;
		Out.BinaryCap = Limits.BinaryBytes;
		Out.FrameCap = bBodies ? Limits.FrameBytes : 0;
		Out.MaxPendingExcerptBytes = Limits.PendingBytes;
		return Out;
	}
};

struct FSupportedAllocator
{
	enum class EFamily : uint8
	{
		Unsupported,
		Binned2,
		Mimalloc200
	};
	EFamily Family = EFamily::Unsupported;
	bool bVerifiedModuleNew = false;
	bool bVerifiedNoInflatingProxy = false;
	bool bCalibrationPassed = false;
	bool SupportsController(SIZE_T Request, uint32 TypeAlignment) const
	{
		return Family != EFamily::Unsupported && bVerifiedModuleNew && bVerifiedNoInflatingProxy && bCalibrationPassed && sizeof(void*) == 8 && Request > 0 && Request <= 1024 && TypeAlignment <= 16 && __STDCPP_DEFAULT_NEW_ALIGNMENT__ <= 16;
	}
};

// GT-only, value copies; neither operation loads a module or changes settings.
FNormalizedLimits NormalizeLimits(const FVaRestInspectorLimits& Limits);
FSupportedAllocator GetSupportedAllocator();
} // namespace VaRest::Inspector::Private
