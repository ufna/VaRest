// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "VaRestSocketClientTypes.h"
#include "VaRestSocketCodec.h"
#include "VaRestSocketText.h"
#include "VaRestSocketTraceState.h"
#include "VaRestTrace.h"

namespace VaRest::WebSocket
{
	struct FSocketRequestControl;
	struct FSocketSendUnit
	{
		TArray<FVaRestSocketFrame> Frames;
		TArray<FSocketFrameStamp> TraceStamps;
		EVaRestSend Flags = EVaRestSend::Default;
		bool bReadyLane = false;
		bool bSkipStages = false;
		FString Channel;
		uint64 QueueEpoch = 0;
		int64 Bytes = 0;
		uint64 Sequence = 0;
		TOptional<FVaRestSocketBatchPin> Pin;
		FString RequestKey;
		TWeakPtr<FSocketRequestControl, ESPMode::ThreadSafe> RequestControl;
	};
	struct FSocketSendErrors { int64 Config = 0, Offline = 0, Canceled = 0; };
	struct FSocketDropNotes { int64 Counts[5] = {}; };
	struct FSocketDroppedRequest { FString Key; EVaRestErrorKind Kind = EVaRestErrorKind::Offline; };
	struct FSocketEncoderSnapshot
	{
		TSharedPtr<IVaRestSocketCodec, ESPMode::ThreadSafe> Codec;
		FVaRestSocketCodecContext Context;
		FVaRestJsonConvertOptions ConvertOptions;
		uint64 Epoch = 0;
		uint64 EncoderRevision = 0;
		FString Channel;
		uint64 QueueEpoch = 1, ChannelEpoch = 0;
	};
	struct FSocketChannelSendState
	{
		uint64 Epoch = 1, QueueEpoch = 1;
		bool bUp = false, bReady = false;
	};
	/** Native MPSC staging/backlog. Locks protect only native bytes/configuration; no UObject or callback payload. */
	class FSocketOutbox
	{
	public:
		uint64 GetEpoch() const;
		void SetTraceClockOnGT(TSharedRef<FSocketTraceClock, ESPMode::ThreadSafe> ClockView);
		void Stamp(FSocketSendUnit& Unit) const;
		void SetLimits(FVaRestSocketLimits Limits);
		void SetPolicy(FVaRestSocketQueuePolicy Policy);
		EVaRestSendResult Post(FSocketSendUnit Unit, uint64 ExpectedEpoch, uint64& Sequence);
		bool TryImmediate(const FSocketSendUnit& Unit, uint64 ExpectedEpoch, bool bOpen, bool bReady, int64 Buffered, EVaRestSendResult& Result);
		bool PopNext(bool bOpen, bool bReady, int64 Buffered, FSocketSendUnit& Unit, EVaRestSendResult& Result);
		void Clear(bool bPermanent = false);
		int32 Count() const;
		FSocketSendErrors TakeErrors();
		FSocketSendErrors GetTotalErrors() const;
		int64 GetQueuedBytes() const;
		void CaptureDropNotes(bool bEnabled);
		FSocketDropNotes TakeDropNotes();
		void FailedSend();
		void EncodingFailed();
		void SetEncoder(TSharedRef<IVaRestSocketCodec, ESPMode::ThreadSafe> Codec, FVaRestSocketCodecContext Context);
		void SetConversionOptions(const FVaRestJsonConvertOptions& Options);
		FSocketEncoderSnapshot GetEncoder(FStringView Channel = {}) const;
		void SetConnectionGeneration(uint64 Generation);
		void SetChannelEpoch(FStringView Channel, uint64 Epoch);
		void PublishChannel(FStringView Channel, uint64 Epoch, bool bUp, bool bReady);
		void ForgetChannel(FStringView Channel);
		bool IsChannelReady(FStringView Channel, bool DefaultReady) const;
		int32 CountChannel(FStringView Channel, uint64 BeforeOrEqual = MAX_uint64) const;
		void ClearChannel(FStringView Channel);
		uint64 GetLastSequence() const;
		void RemoveRequest(FStringView Key);
		TArray<FSocketDroppedRequest> TakeDroppedRequests();
		void SetSendLimit(int64 InBytes);
		EVaRestSendResult CheckDetached(const FSocketSendUnit& Unit, bool bOpen, bool bReady, int64 Buffered, bool bHonorChannelReady = true);
	private:
		bool Validate(const FSocketSendUnit& Unit) const;
		EVaRestTraceSocketDropReason ValidationDropReason(const FSocketSendUnit& Unit) const;
		void DropNote(EVaRestTraceSocketDropReason Reason);
		EVaRestSendResult Preflight(const FSocketSendUnit& Unit, bool bOpen, bool bReady, int64 Buffered, bool bHonorChannelReady = true);
		void RecordDroppedRequest(const FSocketSendUnit& Unit, EVaRestErrorKind Kind = EVaRestErrorKind::Offline);
		mutable FCriticalSection Mutex;
		TArray<FSocketSendUnit> Queue;
		FVaRestSocketLimits Limits;
		FVaRestSocketQueuePolicy Policy;
		FSocketSendErrors Errors, TotalErrors;
		FSocketDropNotes DropNotes;
		bool bCaptureDropNotes = false;
		FSocketEncoderSnapshot Encoder;
		uint64 Epoch = 1, NextSequence = 1;
		uint64 EncoderRevision = 1;
		int64 Bytes = 0;
		bool bClosed = false;
		uint64 ConnectionGeneration = 1;
		TMap<FSocketTextKey, uint64> ChannelEpochs;
		TMap<FSocketTextKey, FSocketChannelSendState> ChannelStates;
		TArray<FSocketDroppedRequest> DroppedRequests;
		int64 SendLimit = MAX_int64;
		TSharedPtr<FSocketTraceClock, ESPMode::ThreadSafe> TraceClock;
	};
}
