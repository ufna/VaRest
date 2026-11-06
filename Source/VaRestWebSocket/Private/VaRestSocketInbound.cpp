// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketInbound.h"
#include "VaRestSocketText.h"
#include "VaRestSocketStatsInternal.h"
#include "Misc/ScopeLock.h"

FVaRestJson FVaRestSocketMessage::Json() const { return Payload ? Payload->Json : FVaRestJson(); }
FVaRestJson FVaRestSocketMessage::Data() const { return Payload ? Payload->Data : FVaRestJson(); }
FVaRestError FVaRestSocketMessage::GetReplyError() const { return Payload ? Payload->ReplyError : FVaRestError(); }
FVaRestSocketMessage FVaRestSocketMessage::FromDecoded(EVaRestSocketFrameKind FrameType, TArray<uint8> Raw, FVaRestJson Root, FVaRestJson Data, FVaRestError ReplyError)
{
	FVaRestSocketMessage Out; Out.FrameKind = FrameType;
	auto Storage = MakeShared<VaRest::WebSocket::FSocketMessagePayload, ESPMode::ThreadSafe>();
	Storage->Raw = MoveTemp(Raw); Storage->Json = MoveTemp(Root); Storage->Data = MoveTemp(Data); Storage->ReplyError = MoveTemp(ReplyError);
	Out.Payload = MoveTemp(Storage); return Out;
}
TConstArrayView<uint8> FVaRestSocketMessage::Bytes() const { return Payload ? TConstArrayView<uint8>(Payload->Raw) : TConstArrayView<uint8>(); }
FString FVaRestSocketMessage::Text() const
{
	const auto Raw = Bytes();
	if (Raw.IsEmpty()) return {};
	const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Raw.GetData()), Raw.Num());
	return FString(Text.Length(), Text.Get());
}

namespace VaRest::WebSocket
{
FSocketInbound::FSocketInbound(uint64 InGeneration, int64 InMaxBytes, int64 InMaxMessageBytes)
	: Generation(InGeneration), MaxBytes(FMath::Max<int64>(0, InMaxBytes)), MaxMessageBytes(FMath::Max<int64>(0, InMaxMessageBytes)), Codec(MakeJsonCodec()) {}

void FSocketInbound::ConfigureOnGT(TSharedRef<IVaRestSocketCodec, ESPMode::ThreadSafe> InCodec, FVaRestSocketCodecContext Context)
{
	check(IsInGameThread());
	FScopeLock Lock(&Admission); Codec = MoveTemp(InCodec); CodecContext = MoveTemp(Context);
}

void FSocketInbound::SetTraceOnGT(uint64 InSocketId, uint64 InAttemptId, TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> InRedactor,
	TSharedPtr<FSocketTraceClock, ESPMode::ThreadSafe> InClockView)
{
	check(IsInGameThread()); FScopeLock Lock(&Admission);
	TraceSocketId = InSocketId; TraceAttemptId = InAttemptId; TraceRedactor = MoveTemp(InRedactor); TraceClock = MoveTemp(InClockView);
}
void FSocketInbound::SetFrameLogOnGT(TSharedRef<FSocketFrameLog, ESPMode::ThreadSafe> Log)
{
	check(IsInGameThread()); FScopeLock Lock(&Admission); FrameLog = MoveTemp(Log);
}

void FSocketInbound::SetLimitsOnGT(int64 Bytes, int64 MessageBytes)
{
	check(IsInGameThread());
	MaxBytes.store(Bytes, std::memory_order_release); MaxMessageBytes.store(MessageBytes, std::memory_order_release);
}
void FSocketInbound::SetHeartbeatOnGT(FVaRestHeartbeat Config)
{
	check(IsInGameThread());
	FScopeLock Lock(&Admission);
	Heartbeat = MoveTemp(Config); PingBytes.Reset(); PongBytes.Reset();
	const auto Ping = FVaRestSocketFrame::Text(Heartbeat.Ping);
	const auto Pong = FVaRestSocketFrame::Text(Heartbeat.Pong);
	PingBytes.Append(Ping.GetPayload()); PongBytes.Append(Pong.GetPayload());
}

void FSocketInbound::AttachOnGT(TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> InConnection)
{
	check(IsInGameThread());
	{
		FScopeLock Lock(&Admission);
		check(!Connection && !bStopped.load(std::memory_order_acquire));
		Connection = MoveTemp(InConnection);
	}
	SchedulePump(); // covers an event queued before Connect returned
}

void FSocketInbound::SchedulePump()
{
	Wakes.fetch_add(1, std::memory_order_acq_rel);
	FScopeLock Lock(&Admission);
	if (bStopped.load(std::memory_order_acquire) || !Connection || BatchRemaining.load(std::memory_order_acquire) != 0
		|| bPumpScheduled.exchange(true, std::memory_order_acq_rel)) return;
	Pipe.Launch(UE_SOURCE_LOCATION, [Native = AsShared()] { Native->Pump(); });
}

void FSocketInbound::Pump()
{
	check(!IsInGameThread());
	const uint64 ObservedWake = Wakes.load(std::memory_order_acquire);
	if (!bStopped.load(std::memory_order_acquire))
	{
		TArray<FVaRestSocketEvent> Events;
		Connection->PopEvents(Events);
		check(NextBatch != MAX_uint64 && BatchRemaining.load(std::memory_order_acquire) == 0);
		const uint64 BatchId = ++NextBatch;
		ActiveBatch.store(BatchId, std::memory_order_release);
		// Publish the entire budget before any GTK delivery can retire an input while this loop is still decoding.
		BatchRemaining.store(Events.Num(), std::memory_order_release);
		for (auto& Event : Events)
		{
			if (bStopped.load(std::memory_order_acquire) || bOverflow || (bDecoderPoisoned && Event.Type == EVaRestSocketEventType::Message))
			{
				if (Event.Type == EVaRestSocketEventType::Message) Connection->ConsumedMessages(Event.Bytes.Num(), 1);
				RetireBatchInput(BatchId);
				continue;
			}
			// NeedMore owns bounded decoder state, not an undelivered GTK item. It must permit the next attachment batch.
			if (!Decode(MoveTemp(Event), BatchId)) RetireBatchInput(BatchId);
		}
	}
	FlushConsumedOnPipe();
	bPumpScheduled.store(false, std::memory_order_release);
	if (Wakes.load(std::memory_order_acquire) != ObservedWake) SchedulePump();
}

bool FSocketInbound::Decode(FVaRestSocketEvent Event, uint64 BatchId)
{
	check(!IsInGameThread());
	const bool bOriginalMessage = Event.Type == EVaRestSocketEventType::Message;
	if (Event.Type == EVaRestSocketEventType::Message)
	{
		TotalFrames.fetch_add(1, std::memory_order_relaxed); TotalBytes.fetch_add(Event.Bytes.Num(), std::memory_order_relaxed);
		Stats::Received(Event.Bytes.Num());
	}
	FSocketDelivery Out;
	Out.Generation = Generation;
	Out.BatchId = BatchId;
	TSharedPtr<IVaRestSocketCodec, ESPMode::ThreadSafe> FrameCodec;
	TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> FrameRedactor;
	TSharedPtr<FSocketTraceClock, ESPMode::ThreadSafe> ClockView;
	TSharedPtr<FSocketFrameLog, ESPMode::ThreadSafe> Log;
	uint64 SocketId = 0, AttemptId = 0;
	FVaRestSocketCodecContext Context;
	bool bInterleavedControl = false;
	bool bLogPrepared = false;
	{
		FScopeLock Lock(&Admission); FrameCodec = Codec; Context = CodecContext;
		FrameRedactor = TraceRedactor; ClockView = TraceClock; SocketId = TraceSocketId; AttemptId = TraceAttemptId;
		Log = FrameLog;
	}
	const uint64 AssembledId = Event.Type == EVaRestSocketEventType::Message ? FVaRestTrace::NewSocketFrameId() : 0;
	Out.Charge = Event.Type == EVaRestSocketEventType::Message ? Event.Bytes.Num() : 0;
	const int64 Charged = PendingBytes.fetch_add(Out.Charge, std::memory_order_acq_rel) + Out.Charge;
	const int32 Events = PendingEvents.fetch_add(1, std::memory_order_acq_rel) + 1;
	if (Charged > MaxBytes.load(std::memory_order_acquire) || Out.Charge > MaxMessageBytes.load(std::memory_order_acquire) || Events >= MaxEvents)
	{
		// Keep exactly one ordered overflow event; all following native events are discarded until GT closes it.
		bOverflow = true; Out.bOverflow = true;
		Out.Event.Type = EVaRestSocketEventType::Closed;
		Out.Event.End.Code = 1009; Out.Event.End.By = EVaRestSocketCloseBy::Transport;
		Out.Event.End.Failure = EVaRestSocketFailure::MessageTooBig;
		Out.Event.End.bWhileReadPaused = Connection && Connection->IsReadPaused();
		Out.Event.End.Detail = TEXT("Socket delivery backlog exceeded its byte or event limit");
		Out.Charge += PendingDecodeCharge; Out.EventCharge += PendingDecodeEvents;
		PendingDecodeCharge = 0; PendingDecodeEvents = 0;
	}
	else
	{
		if (Event.Type == EVaRestSocketEventType::Open)
		{
			DecoderCodec = FrameCodec; Decoder = FrameCodec->MakeDecoder(); bDecoderPoisoned = false;
		}
		else if (Event.Type == EVaRestSocketEventType::Message)
		{
			const int32 JsonOffset = Event.Kind == EVaRestSocketFrameKind::Text ? FrameCodec->GetExcerptJsonOffset(Event.Bytes) : -1;
			if (Log && FrameRedactor) { PendingLogFrames.Add(Log->Prepare(SocketId, AttemptId, false, Event.Kind, Event.Bytes, FrameRedactor.ToSharedRef(), JsonOffset)); bLogPrepared = true; }
			if (FVaRestTrace::Detail() != EVaRestTraceDetail::None)
			{
				FVaRestTraceSocketFrame Trace; Trace.SocketId = SocketId; Trace.AttemptId = AttemptId; Trace.FrameId = AssembledId;
				Trace.Bytes = Event.Bytes.Num(); Trace.ReceivedAt = ClockView ? ClockView->Now() : 0.0;
				Trace.Kind = Event.Kind == EVaRestSocketFrameKind::Binary ? EVaRestTraceSocketFrameKind::Binary : EVaRestTraceSocketFrameKind::Text;
				Trace.ProtocolTag = FrameCodec->GetProtocolTag();
#if VAREST_WITH_TRACE_EXCERPTS
				if (FVaRestTrace::Wants(EVaRestTraceDetail::Bodies) && FrameRedactor)
				{
					const auto Settings = FVaRestTrace::GetDetailSettings();
					const int32 Cap = FMath::Max(0, Settings.FrameCap);
					if (Cap > 0)
					{
						const bool bBinary = Event.Kind == EVaRestSocketFrameKind::Binary;
						const int32 Take = bBinary ? FMath::Min(Cap, FMath::Max(0, Settings.BinaryCap)) : Cap;
						TConstArrayView<uint8> Body(Event.Bytes); if (JsonOffset >= 0) Body = Body.RightChop(FMath::Min(JsonOffset, Body.Num()));
						const FStringView ContentType = bBinary ? FStringView(TEXTVIEW("application/octet-stream")) : (JsonOffset >= 0 ? FStringView(TEXTVIEW("application/json")) : FStringView(TEXTVIEW("text/plain")));
						// The decoder already owns this immutable frame; default binary descriptors need no body copy or scan.
						const auto Prefix = Body.Left(FMath::Min(Take, Body.Num()));
						Trace.Excerpt = FrameRedactor->RedactBody(ContentType, Prefix, Body.Num(), Take, bBinary ? Take : 0);
					}
				}
#endif
				PendingTraceFrames.Add(MoveTemp(Trace));
			}
			// Explicit codec replacement resets its native decoder on this pipe. Routing updates retain it.
			if (DecoderCodec != FrameCodec && Decoder) { Decoder.Reset(); DecoderCodec = FrameCodec; Decoder = FrameCodec->MakeDecoder(); }
			PendingDecodeCharge += Out.Charge; ++PendingDecodeEvents;
			FVaRestSocketInboundFrame Frame; Frame.Kind = Event.Kind; Frame.Bytes = MoveTemp(Event.Bytes); Frame.bAutoReplied = Event.bAutoReplied;
			FVaRestSocketDecodeResult Decoded;
			if (Decoder) Decoded = Decoder->Decode(MoveTemp(Frame), Context);
			else { Decoded.Status = EVaRestSocketDecode::ProtocolError; Decoded.Reason = TEXT("Socket codec did not create an active decoder"); }
			if (Decoded.Status == EVaRestSocketDecode::NeedMore) return false;
			bInterleavedControl = Decoded.bInterleavedControl && Decoded.Status == EVaRestSocketDecode::Message && Decoded.Message.IsSet();
			if (bInterleavedControl)
			{
				PendingDecodeCharge -= Out.Charge; --PendingDecodeEvents;
			}
			else
			{
				Out.Charge = PendingDecodeCharge; Out.EventCharge = PendingDecodeEvents;
				PendingDecodeCharge = 0; PendingDecodeEvents = 0;
			}
			if (Decoded.Status == EVaRestSocketDecode::ProtocolError || !Decoded.Message.IsSet())
			{
				bDecoderPoisoned = true; Out.bProtocolError = true; TotalDecodeErrors.fetch_add(1, std::memory_order_relaxed);
				Event.Type = EVaRestSocketEventType::Closed; Event.End.Code = Decoded.CloseCode == 1009 ? 1009 : 1002; Event.End.By = EVaRestSocketCloseBy::Transport;
				if (Event.End.Code == 1009) Event.End.Failure = EVaRestSocketFailure::MessageTooBig;
				Event.End.bWhileReadPaused = Connection && Connection->IsReadPaused();
				Event.End.Detail = Decoded.Reason.IsEmpty() ? TEXT("Socket decoder refused the packet") : MoveTemp(Decoded.Reason);
			}
			else
			{
				Out.Message = MoveTemp(Decoded.Message.GetValue()); Out.bDecodeFailed = Decoded.bJsonError;
				if (Out.bDecodeFailed) TotalDecodeErrors.fetch_add(1, std::memory_order_relaxed);
				if (Out.Message.Channel.IsEmpty()) Out.Message.Channel = Context.DefaultChannel;
				Out.MessageCodec = FrameCodec; Out.MessageContext = Context;
				FScopeLock Lock(&Admission);
				if (Heartbeat.Mode != EVaRestHeartbeatMode::None && Event.Kind == EVaRestSocketFrameKind::Text)
				{
					const auto Bytes = Out.Message.Bytes();
					const bool bPing = Bytes.Num() == PingBytes.Num() && (Bytes.IsEmpty() || FMemory::Memcmp(Bytes.GetData(), PingBytes.GetData(), Bytes.Num()) == 0);
					const bool bPong = Bytes.Num() == PongBytes.Num() && (Bytes.IsEmpty() || FMemory::Memcmp(Bytes.GetData(), PongBytes.GetData(), Bytes.Num()) == 0);
					Out.bHeartbeatPing = Heartbeat.bJson ? SameText(Out.Message.Route, Heartbeat.Ping) : bPing;
					Out.bHeartbeatPong = Heartbeat.bJson ? SameText(Out.Message.Route, Heartbeat.Pong) : bPong;
				}
			}
		}
		if (Event.IsTerminal())
		{
			Out.Charge += PendingDecodeCharge; Out.EventCharge += PendingDecodeEvents;
			PendingDecodeCharge = 0; PendingDecodeEvents = 0;
			Decoder.Reset(); DecoderCodec.Reset();
			if (!Event.End.Reason.IsEmpty())
			{
				auto Parsed = FVaRestJson::Parse(Event.End.Reason);
				if (Parsed.HasValue() && Parsed.GetValue().IsObject()) Out.ReasonJson = Parsed.StealValue();
			}
		}
		Out.Event = MoveTemp(Event);
	}
	if (Out.Event.Type == EVaRestSocketEventType::Message || Out.Event.IsTerminal() || Out.bOverflow)
	{
		if (bInterleavedControl)
		{
			if (!PendingTraceFrames.IsEmpty() && PendingTraceFrames.Last().FrameId == AssembledId) Out.TraceFrames.Add(PendingTraceFrames.Pop());
			if (bLogPrepared)
			{
				auto Capture = PendingLogFrames.Pop(); Capture.SetMetadata(Out.Message.Channel, Out.Message.Route, Out.Message.ProtocolTag); Capture.Publish();
			}
		}
		else
		{
			Out.TraceFrames = MoveTemp(PendingTraceFrames);
			for (auto& Capture : PendingLogFrames) { Capture.SetMetadata(Out.Message.Channel, Out.Message.Route, Out.Message.ProtocolTag); Capture.Publish(); }
			PendingLogFrames.Reset();
		}
	}
	for (auto& Trace : Out.TraceFrames)
	{
		Trace.Channel = Out.Message.Channel; Trace.Route = Out.Message.Route; Trace.MsgId = Out.Message.Id;
		Trace.MsgKind = static_cast<EVaRestTraceSocketMessageKind>(Out.Message.Kind);
		if (!Out.Message.ProtocolTag.IsNone()) Trace.ProtocolTag = Out.Message.ProtocolTag;
		Trace.bAutoReplied = Out.Message.bAutoReplied;
		for (int32 Index = 0; Index < FMath::Min(10, Out.Message.Attachments.Num()); ++Index)
			Trace.AttachmentSizes.Add(Out.Message.Attachments[Index] ? Out.Message.Attachments[Index]->Num() : 0);
		if (FrameRedactor)
		{
			Trace.Channel = FrameRedactor->ScrubText(Trace.Channel); Trace.Route = FrameRedactor->ScrubText(Trace.Route); Trace.MsgId = FrameRedactor->ScrubText(Trace.MsgId);
		}
	}
	Out.MessageCharge = Out.EventCharge - (bOriginalMessage ? 0 : 1);
	Delivery.Enqueue(MoveTemp(Out));
	DeliveryCount.fetch_add(1, std::memory_order_release);
	return true;
}

FVaRestSocketStats FSocketInbound::GetStatsOnGT() const
{
	check(IsInGameThread()); FVaRestSocketStats Out;
	Out.FramesIn = TotalFrames.load(std::memory_order_relaxed); Out.BytesIn = TotalBytes.load(std::memory_order_relaxed);
	Out.DecodeErrors = TotalDecodeErrors.load(std::memory_order_relaxed); Out.PendingInboundBytes = PendingBytes.load(std::memory_order_acquire);
	return Out;
}
int32 FSocketInbound::GetDeliveryCountOnGT() const { check(IsInGameThread()); return DeliveryCount.load(std::memory_order_acquire); }

bool FSocketInbound::PopOnGT(FSocketDelivery& Out)
{
	check(IsInGameThread());
	if (!Delivery.Dequeue(Out)) return false;
	DeliveryCount.fetch_sub(1, std::memory_order_acq_rel); return true;
}
void FSocketInbound::ConsumedOnGT(const FSocketDelivery& Item)
{
	check(IsInGameThread());
	PendingBytes.fetch_sub(Item.Charge, std::memory_order_acq_rel);
	PendingEvents.fetch_sub(Item.EventCharge, std::memory_order_acq_rel);
	if (Item.Charge || Item.MessageCharge)
	{
		PendingTransportConsumed.fetch_add(Item.Charge, std::memory_order_acq_rel);
		PendingTransportMessages.fetch_add(Item.MessageCharge, std::memory_order_acq_rel);
	}
	if (Item.BatchId) CompleteBatchDeliveryOnGT(Item);
	else if (Item.Charge || Item.MessageCharge) SchedulePump();
}
void FSocketInbound::CompleteBatchDeliveryOnGT(const FSocketDelivery& Item)
{
	check(IsInGameThread());
	if (Item.BatchId) RetireBatchInput(Item.BatchId);
}
void FSocketInbound::RetireBatchInput(uint64 BatchId)
{
	check(BatchId != 0 && ActiveBatch.load(std::memory_order_acquire) == BatchId);
	const int32 Before = BatchRemaining.fetch_sub(1, std::memory_order_acq_rel);
	check(Before > 0);
	// Always request a pull on release, even with no new producer notification. SchedulePump coalesces and rechecks.
	if (Before == 1) SchedulePump();
}
void FSocketInbound::FlushConsumedOnPipe()
{
	check(!IsInGameThread());
	const int64 Bytes = PendingTransportConsumed.exchange(0, std::memory_order_acq_rel);
	const int32 Messages = PendingTransportMessages.exchange(0, std::memory_order_acq_rel);
	if ((Bytes || Messages) && Connection) Connection->ConsumedMessages(Bytes, Messages);
}
void FSocketInbound::StopAndWaitOnGT()
{
	check(IsInGameThread());
	{
		FScopeLock Lock(&Admission);
		if (!bStopped.exchange(true, std::memory_order_acq_rel))
			Pipe.Launch(UE_SOURCE_LOCATION, [Native = AsShared()]
			{
				check(!IsInGameThread()); Native->Decoder.Reset(); Native->DecoderCodec.Reset(); Native->PendingTraceFrames.Reset(); Native->PendingLogFrames.Reset(); Native->FlushConsumedOnPipe();
			});
	}
	// FPipe::WaitUntilEmpty waits on its event; it does not execute a decode task on this thread.
	Pipe.WaitUntilEmpty();
	FSocketDelivery Item;
	while (PopOnGT(Item)) ConsumedOnGT(Item);
	PendingBytes.fetch_sub(PendingDecodeCharge, std::memory_order_acq_rel);
	PendingEvents.fetch_sub(PendingDecodeEvents, std::memory_order_acq_rel);
	PendingTransportConsumed.fetch_add(PendingDecodeCharge, std::memory_order_acq_rel);
	PendingTransportMessages.fetch_add(PendingDecodeEvents, std::memory_order_acq_rel);
	PendingDecodeCharge = 0; PendingDecodeEvents = 0;
	if (PendingTransportConsumed.load(std::memory_order_acquire) || PendingTransportMessages.load(std::memory_order_acquire))
	{
		// Scheduling stays closed. Flush the discarded GTK deliveries on the same consumer pipe before releasing it.
		Pipe.Launch(UE_SOURCE_LOCATION, [Native = AsShared()] { Native->FlushConsumedOnPipe(); });
		Pipe.WaitUntilEmpty();
	}
	Connection.Reset();
}
}
