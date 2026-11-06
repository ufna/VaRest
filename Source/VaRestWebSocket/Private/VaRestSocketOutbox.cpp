// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketOutbox.h"
#include "VaRestSocketRequestState.h"
#include "VaRestTrace.h"
#include "Misc/ScopeLock.h"

namespace VaRest::WebSocket
{
void FSocketOutbox::SetTraceClockOnGT(TSharedRef<FSocketTraceClock, ESPMode::ThreadSafe> ClockView)
{
	check(IsInGameThread()); FScopeLock Lock(&Mutex); TraceClock = MoveTemp(ClockView);
}
void FSocketOutbox::Stamp(FSocketSendUnit& Unit) const
{
	if (Unit.TraceStamps.Num() == Unit.Frames.Num()) return;
	TSharedPtr<FSocketTraceClock, ESPMode::ThreadSafe> View;
	{ FScopeLock Lock(&Mutex); View = TraceClock; }
	const double Time = View ? View->Now() : 0.0; // no queue mutex is held while borrowing Now()
	Unit.TraceStamps.Reset(Unit.Frames.Num());
	for (int32 Index = 0; Index < Unit.Frames.Num(); ++Index) Unit.TraceStamps.Add({FVaRestTrace::NewSocketFrameId(), Time});
}
void FSocketOutbox::CaptureDropNotes(bool bEnabled)
{
	FScopeLock Lock(&Mutex); if (bCaptureDropNotes == bEnabled) return;
	bCaptureDropNotes = bEnabled; DropNotes = {};
}
FSocketDropNotes FSocketOutbox::TakeDropNotes() { FScopeLock Lock(&Mutex); auto Out = DropNotes; DropNotes = {}; return Out; }
void FSocketOutbox::DropNote(EVaRestTraceSocketDropReason Reason)
{
	if (bCaptureDropNotes) ++DropNotes.Counts[static_cast<uint8>(Reason)];
}
EVaRestTraceSocketDropReason FSocketOutbox::ValidationDropReason(const FSocketSendUnit& Unit) const
{
	for (const auto& Frame : Unit.Frames) if (Frame.IsValid() && Frame.Length > FMath::Min(Limits.MaxSendBytes, SendLimit)) return EVaRestTraceSocketDropReason::SendLimit;
	return EVaRestTraceSocketDropReason::Rejected;
}
FSocketSendErrors FSocketOutbox::GetTotalErrors() const { FScopeLock Lock(&Mutex); return TotalErrors; }
int64 FSocketOutbox::GetQueuedBytes() const { FScopeLock Lock(&Mutex); return Bytes; }
uint64 FSocketOutbox::GetEpoch() const { FScopeLock Lock(&Mutex); return Epoch; }
int32 FSocketOutbox::Count() const { FScopeLock Lock(&Mutex); return Queue.Num(); }
void FSocketOutbox::SetLimits(FVaRestSocketLimits InLimits) { FScopeLock Lock(&Mutex); Limits = MoveTemp(InLimits); }
void FSocketOutbox::SetPolicy(FVaRestSocketQueuePolicy InPolicy) { FScopeLock Lock(&Mutex); Policy = MoveTemp(InPolicy); }
bool FSocketOutbox::Validate(const FSocketSendUnit& Unit) const
{
	if (Unit.Frames.IsEmpty() || Unit.Frames.Num() > 4096) return false;
	for (const auto& Frame : Unit.Frames) if (!Frame.IsValid() || Frame.Length > FMath::Min(Limits.MaxSendBytes, SendLimit)) return false;
	return true;
}
EVaRestSendResult FSocketOutbox::Preflight(const FSocketSendUnit& Unit, bool bOpen, bool bReady, int64 Buffered, bool bHonorChannelReady)
{
	const auto* ChannelState = ChannelStates.Find(FSocketTextKey(Unit.Channel));
	if (!ChannelState && !SameText(Unit.Channel, Encoder.Context.DefaultChannel)) { ++Errors.Config; ++TotalErrors.Config; DropNote(EVaRestTraceSocketDropReason::Rejected); return EVaRestSendResult::Rejected; }
	if (ChannelState)
	{
		if (Unit.QueueEpoch && Unit.QueueEpoch != ChannelState->QueueEpoch) { ++Errors.Canceled; ++TotalErrors.Canceled; DropNote(EVaRestTraceSocketDropReason::StalePin); return EVaRestSendResult::Dropped; }
		if (bHonorChannelReady) bReady = ChannelState->bUp && ChannelState->bReady;
	}
	if (!Unit.RequestKey.IsEmpty())
	{
		const auto Control = Unit.RequestControl.Pin();
		if (!Control || !Control->bActive.load(std::memory_order_acquire) || Control->bCancelRequested.load(std::memory_order_acquire))
			{ ++Errors.Canceled; ++TotalErrors.Canceled; DropNote(EVaRestTraceSocketDropReason::StalePin); return EVaRestSendResult::Dropped; }
	}
	if (Unit.Pin.IsSet())
	{
		const auto& Pin = Unit.Pin.GetValue();
		const auto* Channel = ChannelEpochs.Find(FSocketTextKey(Pin.Channel));
		if (Pin.ConnectionGeneration != ConnectionGeneration || (Pin.ChannelEpoch && (!Channel || *Channel != Pin.ChannelEpoch)))
			{ ++Errors.Canceled; ++TotalErrors.Canceled; DropNote(EVaRestTraceSocketDropReason::StalePin); return EVaRestSendResult::Dropped; }
	}
	if (!Validate(Unit)) { ++Errors.Config; ++TotalErrors.Config; DropNote(ValidationDropReason(Unit)); return EVaRestSendResult::Rejected; }
	if (Unit.Flags == EVaRestSend::Volatile && (!bOpen || !bReady || Buffered >= Limits.SendHighWater))
		{ ++Errors.Offline; ++TotalErrors.Offline; DropNote(EVaRestTraceSocketDropReason::Volatile); return EVaRestSendResult::Dropped; }
	if (!bOpen || (!bReady && Unit.Flags != EVaRestSend::Handshake)) return EVaRestSendResult::Queued;
	if (Unit.Bytes > Limits.MaxBufferedBytes || Buffered > Limits.MaxBufferedBytes - Unit.Bytes)
		{ ++Errors.Offline; ++TotalErrors.Offline; DropNote(EVaRestTraceSocketDropReason::Rejected); return EVaRestSendResult::Rejected; }
	return EVaRestSendResult::Sent;
}
bool FSocketOutbox::TryImmediate(const FSocketSendUnit& Unit, uint64 ExpectedEpoch, bool bOpen, bool bReady, int64 Buffered, EVaRestSendResult& Result)
{
	FScopeLock Lock(&Mutex);
	if (bClosed || ExpectedEpoch != Epoch) { ++Errors.Canceled; ++TotalErrors.Canceled; DropNote(EVaRestTraceSocketDropReason::StalePin); Result = EVaRestSendResult::Dropped; return true; }
	if (Queue.ContainsByPredicate([&Unit](const auto& Other) { return SameText(Other.Channel, Unit.Channel) || Other.Flags == EVaRestSend::Handshake; })) return false;
	Result = Preflight(Unit, bOpen, bReady, Buffered);
	return Result != EVaRestSendResult::Queued;
}
EVaRestSendResult FSocketOutbox::Post(FSocketSendUnit Unit, uint64 ExpectedEpoch, uint64& Sequence)
{
	Stamp(Unit);
	FScopeLock Lock(&Mutex);
	if (bClosed || ExpectedEpoch != Epoch) { ++Errors.Canceled; ++TotalErrors.Canceled; DropNote(EVaRestTraceSocketDropReason::StalePin); return EVaRestSendResult::Dropped; }
	if (!ChannelStates.Contains(FSocketTextKey(Unit.Channel)) && !SameText(Unit.Channel, Encoder.Context.DefaultChannel)) { ++Errors.Config; ++TotalErrors.Config; DropNote(EVaRestTraceSocketDropReason::Rejected); return EVaRestSendResult::Rejected; }
	if (const auto* Channel = ChannelStates.Find(FSocketTextKey(Unit.Channel)); Channel && Unit.QueueEpoch && Unit.QueueEpoch != Channel->QueueEpoch)
		{ ++Errors.Canceled; ++TotalErrors.Canceled; DropNote(EVaRestTraceSocketDropReason::StalePin); return EVaRestSendResult::Dropped; }
	if (!Validate(Unit)) { ++Errors.Config; ++TotalErrors.Config; DropNote(ValidationDropReason(Unit)); return EVaRestSendResult::Rejected; }
	if (Policy.MaxQueued == 0 || Unit.Bytes > Policy.MaxQueuedBytes)
		{ ++Errors.Offline; ++TotalErrors.Offline; DropNote(EVaRestTraceSocketDropReason::Overflow); return Policy.Overflow == EVaRestSocketQueueOverflow::Reject ? EVaRestSendResult::Rejected : EVaRestSendResult::Dropped; }
	auto Full = [&]
	{
		int32 Count = 0; int64 Charged = 0;
		for (const auto& Other : Queue) if (SameText(Other.Channel, Unit.Channel)) { ++Count; Charged += Other.Bytes; }
		return Count >= Policy.MaxQueued || Charged > Policy.MaxQueuedBytes - Unit.Bytes;
	};
	if (Full() && Policy.Overflow != EVaRestSocketQueueOverflow::DropOldest)
		{ ++Errors.Offline; ++TotalErrors.Offline; DropNote(EVaRestTraceSocketDropReason::Overflow); return Policy.Overflow == EVaRestSocketQueueOverflow::Reject ? EVaRestSendResult::Rejected : EVaRestSendResult::Dropped; }
	while (Full())
	{
		const int32 Oldest = Queue.IndexOfByPredicate([&Unit](const auto& Other) { return SameText(Other.Channel, Unit.Channel); });
		if (Oldest == INDEX_NONE) break;
		RecordDroppedRequest(Queue[Oldest]); Bytes -= Queue[Oldest].Bytes; Queue.RemoveAt(Oldest); ++Errors.Offline; ++TotalErrors.Offline; DropNote(EVaRestTraceSocketDropReason::Overflow);
	}
	if (NextSequence == MAX_uint64) { ++Errors.Config; ++TotalErrors.Config; DropNote(EVaRestTraceSocketDropReason::Rejected); return EVaRestSendResult::Rejected; }
	Unit.Sequence = NextSequence++; Sequence = Unit.Sequence;
	Bytes += Unit.Bytes; Queue.Add(MoveTemp(Unit));
	return EVaRestSendResult::Queued;
}
bool FSocketOutbox::PopNext(bool bOpen, bool bReady, int64 Buffered, FSocketSendUnit& Unit, EVaRestSendResult& Result)
{
	FScopeLock Lock(&Mutex);
	if (bClosed) return false;
	int32 Best = INDEX_NONE, BestLane = 4;
	for (int32 Index = 0; Index < Queue.Num(); ++Index)
	{
		const auto& Item = Queue[Index];
		const auto* State = ChannelStates.Find(FSocketTextKey(Item.Channel));
		const bool bItemReady = State ? State->bUp && State->bReady : bReady;
		const bool bUnknown = !State && !SameText(Item.Channel, Encoder.Context.DefaultChannel);
		const bool bOldQueue = State && Item.QueueEpoch && Item.QueueEpoch != State->QueueEpoch;
		const bool bVolatileDrop = Item.Flags == EVaRestSend::Volatile && (!bOpen || !bItemReady || Buffered >= Limits.SendHighWater);
		const auto* Channel = Item.Pin.IsSet() ? ChannelEpochs.Find(FSocketTextKey(Item.Pin->Channel)) : nullptr;
		const bool bStale = Item.Pin.IsSet() && (Item.Pin->ConnectionGeneration != ConnectionGeneration
			|| (Item.Pin->ChannelEpoch && (!Channel || *Channel != Item.Pin->ChannelEpoch)));
		const auto Request = Item.RequestControl.Pin();
		const bool bCanceled = !Item.RequestKey.IsEmpty() && (!Request || !Request->bActive.load(std::memory_order_acquire) || Request->bCancelRequested.load(std::memory_order_acquire));
		const bool bInvalid = !Validate(Item) || bStale || bCanceled || bUnknown || bOldQueue;
		if (!bVolatileDrop && !bInvalid && (!bOpen || (!bItemReady && Item.Flags != EVaRestSend::Handshake))) continue;
		const int32 Lane = Item.Flags == EVaRestSend::Handshake ? 0 : (Item.bReadyLane ? 1 : 2);
		if (Best == INDEX_NONE || Lane < BestLane) { Best = Index; BestLane = Lane; }
	}
	if (Best == INDEX_NONE) return false;
	Unit = MoveTemp(Queue[Best]); Queue.RemoveAt(Best); Bytes -= Unit.Bytes;
	Result = Preflight(Unit, bOpen, bReady, Buffered); // current limits, including frames queued under an older config
	if (Result != EVaRestSendResult::Sent)
	{
		const auto Control = Unit.RequestControl.Pin();
		const bool bCanceled = !Unit.RequestKey.IsEmpty() && (!Control || !Control->bActive.load(std::memory_order_acquire) || Control->bCancelRequested.load(std::memory_order_acquire));
		const auto* Channel = Unit.Pin.IsSet() ? ChannelEpochs.Find(FSocketTextKey(Unit.Pin->Channel)) : nullptr;
		const auto* State = ChannelStates.Find(FSocketTextKey(Unit.Channel));
		const bool bStale = Unit.Pin.IsSet() && (Unit.Pin->ConnectionGeneration != ConnectionGeneration || (Unit.Pin->ChannelEpoch && (!Channel || *Channel != Unit.Pin->ChannelEpoch)));
		const bool bOldQueue = State && Unit.QueueEpoch && Unit.QueueEpoch != State->QueueEpoch;
		RecordDroppedRequest(Unit, !Validate(Unit) ? EVaRestErrorKind::Config : (bCanceled || bStale || bOldQueue ? EVaRestErrorKind::Canceled : EVaRestErrorKind::Offline));
	}
	return true;
}
void FSocketOutbox::Clear(bool bPermanent)
{
	FScopeLock Lock(&Mutex);
	Errors.Canceled += Queue.Num(); TotalErrors.Canceled += Queue.Num();
	for (const auto& Unit : Queue) { RecordDroppedRequest(Unit, EVaRestErrorKind::Canceled); DropNote(EVaRestTraceSocketDropReason::StalePin); } Queue.Reset(); Bytes = 0;
	if (Epoch == MAX_uint64) bClosed = true; else ++Epoch;
	bClosed = bClosed || bPermanent;
}
FSocketSendErrors FSocketOutbox::TakeErrors() { FScopeLock Lock(&Mutex); auto Out = Errors; Errors = {}; return Out; }
void FSocketOutbox::FailedSend() { FScopeLock Lock(&Mutex); ++Errors.Offline; ++TotalErrors.Offline; DropNote(EVaRestTraceSocketDropReason::Rejected); }
void FSocketOutbox::EncodingFailed() { FScopeLock Lock(&Mutex); ++Errors.Config; ++TotalErrors.Config; DropNote(EVaRestTraceSocketDropReason::Rejected); }
void FSocketOutbox::SetEncoder(TSharedRef<IVaRestSocketCodec, ESPMode::ThreadSafe> Codec, FVaRestSocketCodecContext Context)
{
	FScopeLock Lock(&Mutex); Encoder.Codec = MoveTemp(Codec); Encoder.Context = MoveTemp(Context);
	if (EncoderRevision == MAX_uint64) bClosed = true; else ++EncoderRevision;
}
void FSocketOutbox::SetConversionOptions(const FVaRestJsonConvertOptions& Options)
{
	FScopeLock Lock(&Mutex);
	Encoder.ConvertOptions.Naming = Options.Naming; Encoder.ConvertOptions.bStrict = Options.bStrict; Encoder.ConvertOptions.Rules = Options.Rules;
	// Bytes hooks, arbitrary callbacks and GTK service state are intentionally not published.
}
FSocketEncoderSnapshot FSocketOutbox::GetEncoder(FStringView Channel) const
{
	FScopeLock Lock(&Mutex); auto Out = Encoder; Out.Epoch = Epoch; Out.EncoderRevision = EncoderRevision;
	Out.Channel = Channel.IsEmpty() ? Encoder.Context.DefaultChannel : FString(Channel);
	if (const auto* State = ChannelStates.Find(FSocketTextKey(Out.Channel))) { Out.QueueEpoch = State->QueueEpoch; Out.ChannelEpoch = State->Epoch; }
	return Out;
}
void FSocketOutbox::SetConnectionGeneration(uint64 Generation) { FScopeLock Lock(&Mutex); ConnectionGeneration = Generation; }
void FSocketOutbox::SetChannelEpoch(FStringView Channel, uint64 InEpoch) { FScopeLock Lock(&Mutex); ChannelEpochs.Add(FSocketTextKey(Channel), InEpoch); }
void FSocketOutbox::PublishChannel(FStringView Channel, uint64 InEpoch, bool bUp, bool bReady)
{
	FScopeLock Lock(&Mutex); const FSocketTextKey Key(Channel);
	auto& State = ChannelStates.FindOrAdd(Key); State.Epoch = InEpoch; State.bUp = bUp; State.bReady = bReady; ChannelEpochs.Add(Key, InEpoch);
}
void FSocketOutbox::ForgetChannel(FStringView Channel)
{
	FScopeLock Lock(&Mutex); const FSocketTextKey Key(Channel); ChannelStates.Remove(Key); ChannelEpochs.Remove(Key);
}
bool FSocketOutbox::IsChannelReady(FStringView Channel, bool DefaultReady) const
{
	FScopeLock Lock(&Mutex); const auto* State = ChannelStates.Find(FSocketTextKey(Channel)); return State ? State->bUp && State->bReady : DefaultReady;
}
int32 FSocketOutbox::CountChannel(FStringView Channel, uint64 BeforeOrEqual) const
{
	FScopeLock Lock(&Mutex); int32 Count = 0;
	for (const auto& Unit : Queue) if (SameText(Unit.Channel, Channel) && Unit.Sequence <= BeforeOrEqual) ++Count;
	return Count;
}
void FSocketOutbox::ClearChannel(FStringView Channel)
{
	FScopeLock Lock(&Mutex);
	for (int32 Index = Queue.Num() - 1; Index >= 0; --Index)
		if (SameText(Queue[Index].Channel, Channel))
		{
			RecordDroppedRequest(Queue[Index], EVaRestErrorKind::Canceled); Bytes -= Queue[Index].Bytes; Queue.RemoveAt(Index); ++Errors.Canceled; ++TotalErrors.Canceled; DropNote(EVaRestTraceSocketDropReason::StalePin);
		}
	auto& State = ChannelStates.FindOrAdd(FSocketTextKey(Channel)); check(State.QueueEpoch != MAX_uint64); ++State.QueueEpoch;
}
uint64 FSocketOutbox::GetLastSequence() const { FScopeLock Lock(&Mutex); return NextSequence - 1; }
void FSocketOutbox::RecordDroppedRequest(const FSocketSendUnit& Unit, EVaRestErrorKind Kind)
{
	if (!Unit.RequestKey.IsEmpty() && !DroppedRequests.ContainsByPredicate([&Unit](const auto& Entry) { return SameText(Entry.Key, Unit.RequestKey); })) DroppedRequests.Add({Unit.RequestKey, Kind});
}
void FSocketOutbox::RemoveRequest(FStringView Key)
{
	FScopeLock Lock(&Mutex);
	for (int32 Index = Queue.Num() - 1; Index >= 0; --Index)
		if (SameText(Queue[Index].RequestKey, Key)) { Bytes -= Queue[Index].Bytes; Queue.RemoveAt(Index); DropNote(EVaRestTraceSocketDropReason::StalePin); }
}
TArray<FSocketDroppedRequest> FSocketOutbox::TakeDroppedRequests() { FScopeLock Lock(&Mutex); return MoveTemp(DroppedRequests); }
void FSocketOutbox::SetSendLimit(int64 InBytes) { FScopeLock Lock(&Mutex); SendLimit = FMath::Max<int64>(0, InBytes); }
EVaRestSendResult FSocketOutbox::CheckDetached(const FSocketSendUnit& Unit, bool bOpen, bool bReady, int64 Buffered, bool bHonorChannelReady)
{
	FScopeLock Lock(&Mutex); return Preflight(Unit, bOpen, bReady, Buffered, bHonorChannelReady);
}
}
