// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspectorPacket.h"

#include "HAL/UnrealMemory.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/AssertionMacros.h"
#include "Templates/AlignmentTemplates.h"

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
namespace
{
class FLayoutBuilder
{
public:
	FLayoutBuilder(FPacketLayout& InLayout, int32 HeaderCount, int32 WarningCount = 0)
		: Layout(InLayout)
	{
		bFits = HeaderCount >= 0 && HeaderCount <= FNormalizedLimits::MaxHeaders &&
				WarningCount >= 0 && WarningCount <= FNormalizedLimits::MaxWarnings;
		if (!bFits)
			return;
		Cursor = Align(int64(sizeof(FPacketFixed)), int64(alignof(FPairSlice)));
		AddArray(Layout.Fixed.Headers, HeaderCount);
		AddArray(Layout.Fixed.Warnings, WarningCount);
		Cursor = Align(Cursor, int64(alignof(TCHAR)));
	}
	void Text(FStringView Borrowed, FTextSlice& Destination)
	{
		if (!bFits || Borrowed.IsEmpty())
			return;
		if (Borrowed.Len() < 0 || Borrowed.Len() > FNormalizedLimits::MaxStringChars || Layout.TextCount >= FPacketLayout::MaxTexts)
		{
			bFits = false;
			return;
		}
		const int64 Bytes = (int64(Borrowed.Len()) + 1) * sizeof(TCHAR);
		const int64 Offset = Cursor;
		if (!AddChecked(Cursor, Bytes, FNormalizedLimits::MaxMetadataBytes))
		{
			bFits = false;
			return;
		}
		Destination = {uint32(Offset), uint32(Borrowed.Len())};
		Layout.BorrowedText[Layout.TextCount] = Borrowed;
		Layout.TextDestination[Layout.TextCount++] = Destination;
	}
	void Field(EText Which, const FString& Borrowed) { Text(FStringView(Borrowed), Layout.Fixed.Text[static_cast<int32>(Which)]); }
	void Headers(const FVaRestHeaders& Borrowed)
	{
		if (!bFits || Layout.Fixed.Headers.Count == 0)
			return;
		int32 Index = 0;
		for (const auto& Pair : Borrowed.GetPairs())
		{
			if (!bFits)
				break;
			check(Index < FNormalizedLimits::MaxHeaders);
			Text(FStringView(Pair.Key), Layout.Headers[Index].First);
			Text(FStringView(Pair.Value), Layout.Headers[Index].Second);
			++Index;
		}
	}
	void Warnings(const TArray<FVaRestTraceDecodeWarning>& Borrowed)
	{
		if (!bFits)
			return;
		for (int32 Index = 0; Index < Borrowed.Num() && bFits; ++Index)
		{
			Text(FStringView(Borrowed[Index].Path), Layout.Warnings[Index].First);
			Text(FStringView(Borrowed[Index].Message), Layout.Warnings[Index].Second);
		}
	}
	EMeasure Finish(const FAllocationAdapter& Allocator)
	{
		if (!bFits)
		{
			const FPacketHeader Header = Layout.Fixed.Header;
			const FPacketScalars Scalars = Layout.Fixed.Scalars;
			Layout = FPacketLayout{};
			Layout.Fixed.Header = Header;
			Layout.Fixed.Scalars = Scalars;
			Layout.Fixed.Header.bMetadataOmitted = true;
			Cursor = sizeof(FPacketFixed);
		}
		const auto Bound = Allocator.Bound(SIZE_T(Cursor), alignof(FPacketFixed));
		if (!Bound.IsSet() || Bound.GetValue() > SIZE_T(MAX_int64))
			return EMeasure::Unsupported;
		Layout.BackingRequest = SIZE_T(Cursor);
		Layout.BackingBound = Bound.GetValue();
		Layout.PendingCharge = int64(Bound.GetValue());
		if (!Layout.Fixed.Header.bSnapshot)
		{
			const SIZE_T Node = Allocator.QueueNodeBound();
			if (Node == 0 || Node > SIZE_T(MAX_int64) || !AddChecked(Layout.PendingCharge, int64(Node), MAX_int64))
				return EMeasure::Unsupported;
		}
		if (!AddChecked(Layout.PendingCharge, Layout.ExcerptCapacity, MAX_int64))
			return EMeasure::Unsupported;
		return bFits ? EMeasure::Full : EMeasure::ScalarOnly;
	}

private:
	void AddArray(FArraySlice& Slice, int32 Count)
	{
		if (!Count || !bFits)
			return;
		const int64 Offset = Cursor;
		if (!AddChecked(Cursor, int64(Count) * sizeof(FPairSlice), FNormalizedLimits::MaxMetadataBytes))
		{
			bFits = false;
			return;
		}
		Slice = {uint32(Offset), uint32(Count)};
	}
	FPacketLayout& Layout;
	int64 Cursor = sizeof(FPacketFixed);
	bool bFits = true;
};
} // namespace

EMeasure FPacketCodec::MeasureStarted(const FVaRestTraceStarted& Borrowed, FPacketHeader Header,
	const FCapturePolicy& Policy, const FAllocationAdapter& Allocator, FPacketLayout& Out)
{
	Out = FPacketLayout{};
	if (Header.Kind != EVaRestTraceKind::Started || Header.Source != EVaRestTraceSource::Http)
		return EMeasure::Unsupported;
	Out.Fixed.Header = Header;
	auto& S = Out.Fixed.Scalars;
	S.RequestId = Borrowed.RequestId;
	S.ScopeId = Borrowed.ScopeId;
	S.ClientId = Borrowed.ClientId;
	S.Endpoint = Borrowed.Endpoint;
	S.Tag = Borrowed.Tag;
	S.Priority = Borrowed.Priority;
	S.Flags = Borrowed.Flags;
	S.OriginalRequestId = Borrowed.QueueReplayOf.OriginalRequestId;
	S.CachePolicy = Borrowed.CachePolicy;
	S.CacheLookup = Borrowed.CacheLookup;
	S.bCacheLookupPending = Borrowed.bCacheLookupPending;
	S.BodyBytes = Borrowed.Body.Bytes;
	S.BodyKind = Borrowed.Body.Kind;
	S.bExcerptPending = Policy.bBodies && Borrowed.bExcerptPending;
	S.StartUtc = Borrowed.StartUtc;
	S.CurrentAttempt = Borrowed.CurrentAttempt;
	S.StartedAt = Borrowed.StartedAt;
	S.ScriptCodeOffset = Borrowed.Initiator.ScriptCodeOffset;
	S.SourceLine = Borrowed.Initiator.SourceLine;
	if (Header.bSnapshot)
		S.Flags |= EVaRestTraceRequestFlags::Partial;
	FLayoutBuilder Builder(Out, Policy.bHeaders ? Borrowed.Headers.Num() : 0);
	Builder.Field(EText::Verb, Borrowed.Verb);
	Builder.Field(EText::Url, Borrowed.Url);
	Builder.Field(EText::PathTemplate, Borrowed.PathTemplate);
	Builder.Field(EText::QueueId, Borrowed.QueueReplayOf.QueueId);
	Builder.Field(EText::OwnerPath, Borrowed.Initiator.OwnerPath);
	Builder.Field(EText::ScriptObject, Borrowed.Initiator.ScriptObject);
	Builder.Field(EText::ScriptFunction, Borrowed.Initiator.ScriptFunction);
	Builder.Field(EText::SourceFile, Borrowed.Initiator.SourceFile);
	Builder.Field(EText::BodyContentType, Borrowed.Body.ContentType);
	if (Policy.bHeaders)
		Builder.Headers(Borrowed.Headers);
	return Builder.Finish(Allocator);
}
EMeasure FPacketCodec::Measure(const FVaRestTraceEvent& Borrowed, const FCapturePolicy& Policy,
	const FAllocationAdapter& Allocator, FPacketLayout& Out)
{
	Out = FPacketLayout{};
	if (Borrowed.Source != EVaRestTraceSource::Http)
		return EMeasure::Unsupported;
	FPacketHeader Header;
	Header.Kind = Borrowed.Kind;
	Header.Source = Borrowed.Source;
	Header.Seq = Borrowed.Seq;
	Header.Time = Borrowed.Time;
	Out.Fixed.Header = Header;
	auto& S = Out.Fixed.Scalars;
	switch (Borrowed.Kind)
	{
	case EVaRestTraceKind::Started:
		if (!Borrowed.Payload.IsType<FVaRestTraceStarted>())
			return EMeasure::Unsupported;
		return MeasureStarted(Borrowed.Payload.Get<FVaRestTraceStarted>(), Header, Policy, Allocator, Out);
	case EVaRestTraceKind::AttemptDispatched:
	{
		if (!Borrowed.Payload.IsType<FVaRestTraceAttemptDispatched>())
			return EMeasure::Unsupported;
		const auto& B = Borrowed.Payload.Get<FVaRestTraceAttemptDispatched>();
		S.RequestId = B.RequestId;
		S.DispatchId = B.DispatchId;
		S.Attempt = B.Attempt;
		S.AttemptStart = B.AttemptStart;
		S.GapKind = B.GapKind;
		S.GapSeconds = B.GapSeconds;
		S.AuthWait = B.AuthWait;
		S.AuthRefreshId = B.AuthRefreshId;
		S.LimiterWait = B.LimiterWait;
		S.bConditional = B.bConditional;
		FLayoutBuilder Builder(Out, Policy.bHeaders ? B.Headers.Num() : 0);
		if (Policy.bHeaders)
			Builder.Headers(B.Headers);
		return Builder.Finish(Allocator);
	}
	case EVaRestTraceKind::AttemptEnded:
	{
		if (!Borrowed.Payload.IsType<FVaRestTraceAttemptEnded>())
			return EMeasure::Unsupported;
		const auto& B = Borrowed.Payload.Get<FVaRestTraceAttemptEnded>();
		S.RequestId = B.RequestId;
		S.DispatchId = B.DispatchId;
		S.Attempt = B.Attempt;
		S.Status = B.Status;
		S.BytesUp = B.BytesUp;
		S.BytesDown = B.BytesDown;
		S.Failure = B.Failure;
		S.FailureDetail = B.FailureDetail;
		S.UploadDoneAt = B.UploadDoneAt;
		S.StatusAt = B.StatusAt;
		S.TransportDoneAt = B.TransportDoneAt;
		S.bExcerptPending = Policy.bBodies && B.bExcerptPending;
		FLayoutBuilder Builder(Out, Policy.bHeaders ? B.Headers.Num() : 0);
		Builder.Field(EText::EffectiveUrl, B.EffectiveUrl);
		Builder.Field(EText::ResponseContentType, B.ContentType);
		if (Policy.bHeaders)
			Builder.Headers(B.Headers);
		return Builder.Finish(Allocator);
	}
	case EVaRestTraceKind::Note:
	{
		if (!Borrowed.Payload.IsType<FVaRestTraceNote>())
			return EMeasure::Unsupported;
		const auto& B = Borrowed.Payload.Get<FVaRestTraceNote>();
		S.RequestId = B.RequestId;
		S.Attempt = B.Attempt;
		S.NoteKind = B.Kind;
		S.Reason = B.Reason;
		S.Delay = B.Delay;
		S.RetryAfter = B.RetryAfter;
		S.NoteBytes = B.Bytes;
		FLayoutBuilder Builder(Out, 0);
		Builder.Field(EText::QueueId, B.QueueId);
		return Builder.Finish(Allocator);
	}
	case EVaRestTraceKind::Completed:
	{
		if (!Borrowed.Payload.IsType<FVaRestTraceCompleted>())
			return EMeasure::Unsupported;
		const auto& B = Borrowed.Payload.Get<FVaRestTraceCompleted>();
		S.RequestId = B.RequestId;
		S.Outcome = B.Outcome;
		S.CancelReason = B.CancelReason;
		S.ErrorKind = B.Error.Kind;
		S.ErrorHttpStatus = B.Error.HttpStatus;
		S.ResultType = B.ResultType;
		S.DecodeStartAt = B.DecodeStartAt;
		S.DecodeEndAt = B.DecodeEndAt;
		S.DecodeWarningCount = B.DecodeWarningCount;
		S.bEnvelopeApplied = B.Envelope.bApplied;
		S.CacheLookup = B.CacheLookup;
		S.bFromCache = B.bFromCache;
		S.AuthRefreshId = B.AuthRefreshId;
		S.Status = B.Response.Status;
		S.DeliveredBodyBytes = B.Response.BodyBytes;
		S.Storage = B.Response.Storage;
		FLayoutBuilder Builder(Out, Policy.bHeaders ? B.Response.Headers.Num() : 0, B.DecodeWarnings.Num());
		Builder.Field(EText::ErrorCode, B.Error.Code);
		Builder.Field(EText::ErrorMessage, B.Error.Message);
		Builder.Field(EText::DataPath, B.Envelope.DataPath);
		Builder.Field(EText::PredicatePath, B.Envelope.PredicatePath);
		Builder.Field(EText::Expected, B.Envelope.Expected);
		Builder.Field(EText::Actual, B.Envelope.Actual);
		Builder.Field(EText::EnvelopeCode, B.Envelope.Code);
		Builder.Field(EText::EnvelopeMessage, B.Envelope.Message);
		Builder.Field(EText::EffectiveUrl, B.Response.EffectiveUrl);
		Builder.Field(EText::ResponseContentType, B.Response.ContentType);
		if (Policy.bHeaders)
			Builder.Headers(B.Response.Headers);
		Builder.Warnings(B.DecodeWarnings);
		return Builder.Finish(Allocator);
	}
	case EVaRestTraceKind::Excerpt:
	{
		if (!Policy.bBodies || !Borrowed.Payload.IsType<FVaRestTraceExcerptEvent>())
			return EMeasure::Unsupported;
		const auto& B = Borrowed.Payload.Get<FVaRestTraceExcerptEvent>();
		if (B.Owner == EVaRestTraceExcerptOwner::Frame)
			return EMeasure::Unsupported;
		S.RequestId = B.RequestId;
		S.DispatchId = B.DispatchId;
		S.Attempt = B.Attempt;
		S.Owner = B.Owner;
		S.SocketId = B.SocketId;
		S.FrameId = B.FrameId;
		S.ExcerptKind = B.Excerpt.Kind;
		S.TotalBytes = B.Excerpt.TotalBytes;
		S.bTruncated = B.Excerpt.bTruncated;
		S.bRedacted = B.Excerpt.bRedacted;
		S.Skip = B.Excerpt.SkipReason;
		FLayoutBuilder Builder(Out, 0);
		Builder.Field(EText::StopReason, B.Excerpt.StopReason);
		Builder.Field(EText::Sha256Prefix, B.Excerpt.Sha256Prefix);
		if (B.Excerpt.Bytes.IsValid())
		{
			const SIZE_T Capacity = B.Excerpt.Bytes->GetAllocatedSize();
			if (Capacity > SIZE_T(MAX_int64))
				return EMeasure::Unsupported;
			Out.bKeepExcerpt = true;
			Out.ExcerptCapacity = int64(Capacity);
		}
		return Builder.Finish(Allocator);
	}
	case EVaRestTraceKind::Scope:
	{
		if (!Borrowed.Payload.IsType<FVaRestTraceScope>())
			return EMeasure::Unsupported;
		const auto& B = Borrowed.Payload.Get<FVaRestTraceScope>();
		S.bOpened = B.bOpened;
		S.ScopeId = B.ScopeId;
		S.PIEInstance = B.PIEInstance;
		S.bEditorScope = B.bEditorScope;
		FLayoutBuilder Builder(Out, 0);
		Builder.Field(EText::ScopeLabel, B.Label);
		return Builder.Finish(Allocator);
	}
	case EVaRestTraceKind::Client:
	{
		if (!Borrowed.Payload.IsType<FVaRestTraceClient>())
			return EMeasure::Unsupported;
		const auto& B = Borrowed.Payload.Get<FVaRestTraceClient>();
		S.bCreated = B.bCreated;
		S.ClientId = B.ClientId;
		S.ScopeId = B.ScopeId;
		FLayoutBuilder Builder(Out, 0);
		Builder.Field(EText::ClientLabel, B.DisplayName);
		return Builder.Finish(Allocator);
	}
	case EVaRestTraceKind::Marker:
	{
		if (!Borrowed.Payload.IsType<FVaRestTraceMarker>())
			return EMeasure::Unsupported;
		const auto& B = Borrowed.Payload.Get<FVaRestTraceMarker>();
		S.MarkerKind = B.Kind;
		S.ScopeId = B.ScopeId;
		S.ClientId = B.ClientId;
		S.CauseRequestId = B.CauseRequestId;
		FLayoutBuilder Builder(Out, 0);
		Builder.Field(EText::MarkerText, B.Text);
		return Builder.Finish(Allocator);
	}
	default: return EMeasure::Unsupported;
	}
}

FPacket::FPacket(FPacket&& Other) noexcept
	: Pending(MoveTemp(Other.Pending))
	, Allocator(Other.Allocator)
	, Backing(Other.Backing)
	, BackingCapacity(Other.BackingCapacity)
	, BackingRequest(Other.BackingRequest)
	, ExcerptOwner(MoveTemp(Other.ExcerptOwner))
{
	Other.Allocator = nullptr;
	Other.Backing = nullptr;
	Other.BackingCapacity = 0;
	Other.BackingRequest = 0;
}
FPacket& FPacket::operator=(FPacket&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Pending = MoveTemp(Other.Pending);
		Allocator = Other.Allocator;
		Backing = Other.Backing;
		BackingCapacity = Other.BackingCapacity;
		BackingRequest = Other.BackingRequest;
		ExcerptOwner = MoveTemp(Other.ExcerptOwner);
		Other.Allocator = nullptr;
		Other.Backing = nullptr;
		Other.BackingCapacity = 0;
		Other.BackingRequest = 0;
	}
	return *this;
}
FPacket::~FPacket()
{
	Reset();
}
void FPacket::Reset()
{
	ExcerptOwner.Reset();
	if (Backing)
	{
		static_cast<FPacketFixed*>(Backing)->~FPacketFixed();
		check(Allocator);
		Allocator->Free(Backing);
	}
	Backing = nullptr;
	BackingCapacity = 0;
	BackingRequest = 0;
	Allocator = nullptr;
	Pending.Reset();
}
const FPacketFixed& FPacket::Fixed() const
{
	check(Backing);
	return *static_cast<const FPacketFixed*>(Backing);
}
FStringView FPacket::Text(EText Field) const
{
	const int32 Index = static_cast<int32>(Field);
	check(Index >= 0 && Index < static_cast<int32>(EText::Count));
	return Read(Fixed().Text[Index]);
}
FStringView FPacket::Read(FTextSlice Slice) const
{
	if (!Slice.Chars)
		return {};
	check(Backing && Slice.Offset % alignof(TCHAR) == 0 && Slice.Chars <= uint32(MAX_int32));
	check(uint64(Slice.Offset) + (uint64(Slice.Chars) + 1) * sizeof(TCHAR) <= BackingRequest);
	return FStringView(reinterpret_cast<const TCHAR*>(static_cast<const uint8*>(Backing) + Slice.Offset), int32(Slice.Chars));
}
TConstArrayView<FPairSlice> FPacket::Headers() const
{
	const auto Slice = Fixed().Headers;
	if (!Slice.Count)
		return {};
	check(Slice.Offset % alignof(FPairSlice) == 0 && Slice.Count <= FNormalizedLimits::MaxHeaders);
	check(uint64(Slice.Offset) + uint64(Slice.Count) * sizeof(FPairSlice) <= BackingRequest);
	return {reinterpret_cast<const FPairSlice*>(static_cast<const uint8*>(Backing) + Slice.Offset), int32(Slice.Count)};
}
TConstArrayView<FPairSlice> FPacket::Warnings() const
{
	const auto Slice = Fixed().Warnings;
	if (!Slice.Count)
		return {};
	check(Slice.Offset % alignof(FPairSlice) == 0 && Slice.Count <= FNormalizedLimits::MaxWarnings);
	check(uint64(Slice.Offset) + uint64(Slice.Count) * sizeof(FPairSlice) <= BackingRequest);
	return {reinterpret_cast<const FPairSlice*>(static_cast<const uint8*>(Backing) + Slice.Offset), int32(Slice.Count)};
}
const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& FPacket::Excerpt() const
{
	return ExcerptOwner;
}
int64 FPacket::RetainedCapacity() const
{
	return int64(BackingCapacity) + (ExcerptOwner ? ExcerptOwner->GetAllocatedSize() : 0);
}
FPacket FPacketCodec::WriteLayout(const FPacketLayout& Layout, FPendingTicket&& Reservation,
	FAllocationAdapter& Allocator, const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>* BorrowedExcerpt)
{
	check(Reservation.IsValid() && Reservation.Bytes() == Layout.PendingCharge);
	FPacket Result;
	Result.Pending = MoveTemp(Reservation);
	Result.Allocator = &Allocator;
	Result.BackingCapacity = Layout.BackingBound;
	Result.BackingRequest = Layout.BackingRequest;
	Result.Backing = Allocator.AllocateReserved(Layout.BackingRequest, alignof(FPacketFixed), Layout.BackingBound);
	if (!Result.Backing)
	{
		Result.Reset();
		return Result;
	}
	new (Result.Backing) FPacketFixed(Layout.Fixed);
	auto* Bytes = static_cast<uint8*>(Result.Backing);
	if (Layout.Fixed.Headers.Count)
		FMemory::Memcpy(Bytes + Layout.Fixed.Headers.Offset, Layout.Headers, SIZE_T(Layout.Fixed.Headers.Count) * sizeof(FPairSlice));
	if (Layout.Fixed.Warnings.Count)
		FMemory::Memcpy(Bytes + Layout.Fixed.Warnings.Offset, Layout.Warnings, SIZE_T(Layout.Fixed.Warnings.Count) * sizeof(FPairSlice));
	for (int32 Index = 0; Index < Layout.TextCount; ++Index)
	{
		const FTextSlice Dest = Layout.TextDestination[Index];
		const SIZE_T Count = SIZE_T(Dest.Chars) * sizeof(TCHAR);
		check(SIZE_T(Dest.Offset) + Count + sizeof(TCHAR) <= Layout.BackingRequest);
		FMemory::Memcpy(Bytes + Dest.Offset, Layout.BorrowedText[Index].GetData(), Count);
		*reinterpret_cast<TCHAR*>(Bytes + Dest.Offset + Count) = TCHAR(0);
		Allocator.Counters().TextBytesCopied.fetch_add(int64(Count + sizeof(TCHAR)));
	}
	if (Layout.bKeepExcerpt)
	{
		check(BorrowedExcerpt && BorrowedExcerpt->IsValid());
		Result.ExcerptOwner = *BorrowedExcerpt;
		Allocator.Counters().ExcerptRefsAcquired.fetch_add(1);
	}
	return Result;
}
FPacket FPacketCodec::WriteStarted(const FVaRestTraceStarted& Borrowed, const FPacketLayout& Layout,
	FPendingTicket&& Reservation, FAllocationAdapter& Allocator)
{
	check(Layout.Fixed.Header.Kind == EVaRestTraceKind::Started && Layout.Fixed.Scalars.RequestId == Borrowed.RequestId && !Layout.bKeepExcerpt);
	return WriteLayout(Layout, MoveTemp(Reservation), Allocator, nullptr);
}
FPacket FPacketCodec::Write(const FVaRestTraceEvent& Borrowed, const FPacketLayout& Layout,
	FPendingTicket&& Reservation, FAllocationAdapter& Allocator)
{
	const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>* Excerpt = nullptr;
	if (Layout.bKeepExcerpt)
	{
		check(Borrowed.Source == EVaRestTraceSource::Http && Borrowed.Kind == EVaRestTraceKind::Excerpt && Borrowed.Payload.IsType<FVaRestTraceExcerptEvent>());
		Excerpt = &Borrowed.Payload.Get<FVaRestTraceExcerptEvent>().Excerpt.Bytes;
	}
	return WriteLayout(Layout, MoveTemp(Reservation), Allocator, Excerpt);
}
} // namespace VaRest::Inspector::Private
#endif
