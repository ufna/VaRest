// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "Misc/DateTime.h"
#include "UObject/NameTypes.h"
#include "VaRestTrace.h"

namespace VaRest::Inspector::View
{
enum class EText : uint8
{
	Verb,
	Url,
	PathTemplate,
	QueueId,
	OwnerPath,
	ScriptObject,
	ScriptFunction,
	SourceFile,
	BodyContentType,
	EffectiveUrl,
	ResponseContentType,
	ErrorCode,
	ErrorMessage,
	DataPath,
	PredicatePath,
	Expected,
	Actual,
	EnvelopeCode,
	EnvelopeMessage,
	StopReason,
	Sha256Prefix,
	ScopeLabel,
	ClientLabel,
	MarkerText,
	Count
};
struct FTextSlice
{
	uint32 Offset = 0;
	uint32 Chars = 0;
};
struct FPairSlice
{
	FTextSlice First;
	FTextSlice Second;
};
struct FArraySlice
{
	uint32 Offset = 0;
	uint32 Count = 0;
};
struct FPacketHeader
{
	EVaRestTraceKind Kind = EVaRestTraceKind::Started;
	EVaRestTraceSource Source = EVaRestTraceSource::Http;
	uint64 Seq = 0, Generation = 0, MetadataOrdinal = 0;
	double Time = 0;
	bool bSnapshot = false, bMetadataOmitted = false;
};
struct FPacketScalars
{
	uint64 RequestId = 0, DispatchId = 0, ScopeId = 0, ClientId = 0;
	uint64 OriginalRequestId = 0, AuthRefreshId = 0, CauseRequestId = 0;
	uint64 SocketId = 0, FrameId = 0;
	int32 Attempt = 0, CurrentAttempt = 0, Status = 0, ErrorHttpStatus = 0;
	int32 ScriptCodeOffset = INDEX_NONE, SourceLine = 0, PIEInstance = INDEX_NONE;
	int32 DecodeWarningCount = 0;
	int64 BodyBytes = 0, BytesUp = 0, BytesDown = 0, NoteBytes = 0;
	int64 DeliveredBodyBytes = -1, TotalBytes = 0;
	double StartedAt = 0, AttemptStart = 0, GapSeconds = 0, AuthWait = 0;
	double LimiterWait = 0, UploadDoneAt = -1, StatusAt = -1, TransportDoneAt = -1;
	double DecodeStartAt = -1, DecodeEndAt = -1, Delay = 0, RetryAfter = -1;
	FDateTime StartUtc;
	FName Endpoint, Tag, CachePolicy, FailureDetail, ResultType, Reason;
	EVaRestPriority Priority = EVaRestPriority::Normal;
	EVaRestTraceRequestFlags Flags = EVaRestTraceRequestFlags::None;
	EVaRestBodyKind BodyKind = EVaRestBodyKind::None;
	EVaRestTraceCacheLookup CacheLookup = EVaRestTraceCacheLookup::None;
	EVaRestTraceGap GapKind = EVaRestTraceGap::None;
	EVaRestTransportFailure Failure = EVaRestTransportFailure::None;
	EVaRestTraceOutcome Outcome = EVaRestTraceOutcome::Success;
	EVaRestCancelReason CancelReason = EVaRestCancelReason::None;
	EVaRestErrorKind ErrorKind = EVaRestErrorKind::None;
	EVaRestTraceResponseStorage Storage = EVaRestTraceResponseStorage::Unknown;
	EVaRestTraceNoteKind NoteKind = EVaRestTraceNoteKind::RetryScheduled;
	EVaRestTraceExcerptOwner Owner = EVaRestTraceExcerptOwner::Request;
	EVaRestExcerptKind ExcerptKind = EVaRestExcerptKind::Text;
	EVaRestExcerptSkip Skip = EVaRestExcerptSkip::None;
	EVaRestTraceMarkerKind MarkerKind = EVaRestTraceMarkerKind::Online;
	bool bCacheLookupPending = false, bExcerptPending = false, bConditional = false;
	bool bFromCache = false, bEnvelopeApplied = false, bTruncated = false;
	bool bRedacted = false, bOpened = false, bEditorScope = false, bCreated = false;
};
struct FPacketFixed
{
	FPacketHeader Header;
	FPacketScalars Scalars;
	FTextSlice Text[static_cast<int32>(EText::Count)];
	FArraySlice Headers, Warnings;
};
} // namespace VaRest::Inspector::View
