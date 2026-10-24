// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Containers/ArrayView.h"
#include "Containers/StringView.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "Misc/AssertionMacros.h"
#include "Misc/DateTime.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"
#include "VaRestInspectorPacketView.h"
#include "VaRestInspectorStats.h"

namespace VaRest::Inspector::Private
{
class FSnapshotBuilder;
class FStore;
}

enum class EVaRestInspectorRecordKind : uint8
{
	Http,
	Marker
};
enum class EVaRestInspectorEndState : uint8
{
	InFlight,
	Success,
	Error,
	Queued,
	Canceled,
	CaptureInterrupted
};
enum class EVaRestInspectorDetailState : uint8
{
	Absent,
	Pending,
	Present,
	Backlog,
	Discarded,
	TimedOut,
	CaptureStopped,
	Evicted,
	NoProducer,
	MetadataOmitted
};
struct FVaRestInspectorPhase
{
	double Seconds = 0.0;
	bool bKnown = false;
};
struct FVaRestInspectorTimelineEntry
{
	EVaRestTraceKind Kind = EVaRestTraceKind::Started;
	uint64 Seq = 0;
	double Time = 0.0;
	int32 Attempt = 0;
	uint64 DispatchId = 0;
	// Scalar history survives optional-owner loss. No packet pointer escapes.
	EVaRestInspectorDetailState DetailState = EVaRestInspectorDetailState::Absent;
};
struct FVaRestInspectorLink
{
	uint64 FromRequestId = 0;
	uint64 ToRequestId = 0;
	FString QueueId;
	bool bAuth = false;
	bool bTargetEvicted = false;
};

struct FVaRestInspectorPacketView
{
	const VaRest::Inspector::View::FPacketFixed* Fixed = nullptr;
	TConstArrayView<uint8> Backing, Excerpt;
	FStringView Read(VaRest::Inspector::View::FTextSlice S) const
	{
		if (!Fixed || S.Chars == 0)
			return {};
		const uint64 End = uint64(S.Offset) + uint64(S.Chars) * sizeof(TCHAR);
		check(End <= uint64(Backing.Num()) && S.Offset % alignof(TCHAR) == 0);
		return FStringView(reinterpret_cast<const TCHAR*>(Backing.GetData() + S.Offset), S.Chars);
	}
	FStringView Text(VaRest::Inspector::View::EText Field) const
	{
		return Fixed ? Read(Fixed->Text[static_cast<int32>(Field)]) : FStringView();
	}
	TConstArrayView<VaRest::Inspector::View::FPairSlice> Pairs(
		VaRest::Inspector::View::FArraySlice S) const
	{
		using FPair = VaRest::Inspector::View::FPairSlice;
		if (!Fixed || S.Count == 0)
			return {};
		check(uint64(S.Offset) + uint64(S.Count) * sizeof(FPair) <= uint64(Backing.Num()));
		check(S.Offset % alignof(FPair) == 0);
		return {reinterpret_cast<const FPair*>(Backing.GetData() + S.Offset), int32(S.Count)};
	}
};
// The codec cannot distinguish captured-empty from capture-disabled headers.
// Neither is evidence that an earlier dispatch's headers remain current.
enum class EVaRestInspectorHeadersState : uint8
{
	Unknown,
	Captured,
	MetadataOmitted
};
struct FVaRestInspectorDispatchView
{
	uint64 DispatchId = 0;
	int32 Attempt = 0;
	bool bHasDispatched = false, bHasEnded = false, bCombinedSendWait = false;
	double AttemptStart = -1, DispatchAt = -1, UploadDoneAt = -1, StatusAt = -1, TransportDoneAt = -1;
	EVaRestTraceGap GapKind = EVaRestTraceGap::None;
	double GapSeconds = 0;
	EVaRestInspectorHeadersState HeadersState = EVaRestInspectorHeadersState::Unknown;
	EVaRestInspectorDetailState ResponseExcerptState = EVaRestInspectorDetailState::Absent;
	FVaRestInspectorPacketView Dispatched, Ended, ResponseExcerpt;
	FVaRestInspectorPhase Auth, Queued, Prepare, Send, Wait, Receive, Wall, Gap;
};
// Byte counters report admitted payload/retained-owner charges, not RSS or
// hidden post-destruction controller free tails; see decision8.
struct FVaRestInspectorDiagnostics
{
	uint64 DroppedEvents = 0, DroppedBytes = 0, RefusedRows = 0;
	uint64 MetadataOmitted = 0, UnsupportedAllocator = 0, InterruptedRows = 0;
	uint64 AttachLosses = 0, OmittedClients = 0, ContractViolations = 0;
	int64 StoreBytes = 0, StorePeakBytes = 0, PendingBytes = 0, PendingPeakBytes = 0;
	int32 SnapshotControllers = 0;
	bool bCapturing = false, bPendingLimits = false;
	int64 EffectiveStoreLimit = 0, RequestedStoreLimit = 0;
};
struct FVaRestInspectorRecordView
{
	EVaRestInspectorRecordKind Kind = EVaRestInspectorRecordKind::Http;
	uint64 RequestId = 0, Revision = 0;
	uint64 ScopeId = 0, ClientId = 0, ArrivalOrdinal = 0, BirthMetadataOrdinal = 0;
	int32 CurrentAttempt = 0;
	double StartedAt = 0;
	FDateTime StartUtc;
	bool bStartedAtKnown = false, bStartUtcKnown = false, bInitiatorKnown = false, bMetadataOmitted = false;
	FStringView ScopeLabel, ClientLabel;
	EVaRestInspectorEndState EndState = EVaRestInspectorEndState::CaptureInterrupted;
	bool bPartial = false, bPinned = false, bSealed = false, bTerminalKnown = false;
	// Observations survive optional detail refusal. Identities not retained in
	// the bounded slots have no perfect duplicate-suppression guarantee.
	uint64 ObservedDispatches = 0, ObservedEnds = 0;
	uint64 OmittedDispatches = 0, OmittedEnds = 0, OmittedExcerpts = 0;
	uint64 OmittedNotes = 0, OmittedTimeline = 0;
	uint64 WireBytesUp = 0, WireBytesDown = 0;
	bool bWireBytesUnknown = false, bCountersSaturated = false;
	int32 FinalStatus = 0;
	int64 FinalBodyBytes = -1;
	EVaRestTraceResponseStorage FinalStorage = EVaRestTraceResponseStorage::Unknown;
	double CompletedAt = -1, DecodeStartAt = -1, DecodeEndAt = -1;
	// Receipt time belongs to the Inspector clock, independently of source phases.
	bool bActualCompletionObserved = false;
	double CompletedObservedAt = 0;
	EVaRestInspectorDetailState CompletedMetadataState = EVaRestInspectorDetailState::Absent;
	EVaRestInspectorDetailState FinalPreviewState = EVaRestInspectorDetailState::Absent;
	EVaRestInspectorDetailState RequestExcerptState = EVaRestInspectorDetailState::Absent;
	FVaRestInspectorPhase Total, Decode, Deliver;
	// Completed may expose only pre-admitted scalars with empty Backing/Excerpt.
	// Consult CompletedMetadataState; nonnull Fixed does not imply retained text.
	FVaRestInspectorPacketView Started, Completed, Marker;
	TConstArrayView<FVaRestInspectorDispatchView> Dispatches;
	TConstArrayView<FVaRestInspectorPacketView> DispatchEvents, Notes, Excerpts;
	TConstArrayView<FVaRestInspectorTimelineEntry> Timeline;
};
/** Read operations are GT-only. Borrowed record views expire when the visitor returns. */
class VARESTINSPECTOR_API FVaRestInspectorSnapshot final
{
public:
	FVaRestInspectorSnapshot();
	~FVaRestInspectorSnapshot();
	FVaRestInspectorSnapshot(const FVaRestInspectorSnapshot&) = delete;
	FVaRestInspectorSnapshot& operator=(const FVaRestInspectorSnapshot&) = delete;
	uint64 GetRevision() const;
	void ForEachRecord(TFunctionRef<void(const FVaRestInspectorRecordView&)>) const;
	void ForEachStats(TFunctionRef<void(const FVaRestInspectorHttpStats&)>) const;
	TConstArrayView<FVaRestInspectorLink> GetLinks() const;
	const FVaRestInspectorDiagnostics& GetDiagnostics() const;

private:
	friend class VaRest::Inspector::Private::FSnapshotBuilder;
	friend class VaRest::Inspector::Private::FStore;
	struct FData;
	struct FDataDeleter
	{
		void operator()(FData* Object) const;
	};
	TUniquePtr<FData, FDataDeleter> Data;
};
