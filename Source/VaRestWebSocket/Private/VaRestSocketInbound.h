// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include "Tasks/Pipe.h"
#include "VaRestSocketMessage.h"
#include "VaRestSocketClientTypes.h"
#include "VaRestSocketCodec.h"
#include "VaRestSocketTransport.h"
#include "VaRestSocketStats.h"
#include "VaRestTrace.h"
#include "VaRestRedactor.h"
#include "VaRestSocketTraceState.h"
#include "VaRestSocketLog.h"
#include <atomic>

namespace VaRest::WebSocket
{
	struct FSocketMessagePayload
	{
		TArray<uint8> Raw;
		FVaRestJson Json;
		FVaRestJson Data;
		FVaRestError ReplyError;
	};
	struct FSocketDelivery
	{
		FVaRestSocketEvent Event;
		FVaRestSocketMessage Message;
		FVaRestJson ReasonJson;
		TSharedPtr<IVaRestSocketCodec, ESPMode::ThreadSafe> MessageCodec;
		FVaRestSocketCodecContext MessageContext;
		TArray<FVaRestTraceSocketFrame> TraceFrames;
		uint64 Generation = 0;
		/** One current transport input's W3 delivery credit; zero after bounded W4 hold admission. Not a payload credit. */
		uint64 BatchId = 0;
		int64 Charge = 0;
		int32 EventCharge = 1;
		int32 MessageCharge = 0;
		bool bDecodeFailed = false;
		bool bOverflow = false;
		bool bHeartbeatPing = false, bHeartbeatPong = false;
		bool bProtocolError = false;
	};
	/** Pure native worker owner: no reflection, client shells, or affine user callbacks.
	 * One transport batch is admitted through decode and GTK acceptance at a time. Bounded decoder fragments and the
	 * separately bounded W4 ready-hold queue retain payload charges until final delivery/discard, independently of batch
	 * credit. Existing byte/event overflow limits stay; actual W1b transport pressure qualification remains separate. */
	class FSocketInbound final : public TSharedFromThis<FSocketInbound, ESPMode::ThreadSafe>
	{
	public:
		explicit FSocketInbound(uint64 Generation, int64 MaxBytes = 16 * 1024 * 1024, int64 MaxMessageBytes = 1024 * 1024);
		void SetLimitsOnGT(int64 Bytes, int64 MessageBytes);
		void SetHeartbeatOnGT(FVaRestHeartbeat Heartbeat);
		void ConfigureOnGT(TSharedRef<IVaRestSocketCodec, ESPMode::ThreadSafe> Codec, FVaRestSocketCodecContext Context);
		void SetTraceOnGT(uint64 InSocketId, uint64 InAttemptId, TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> InRedactor,
			TSharedPtr<FSocketTraceClock, ESPMode::ThreadSafe> InClockView);
		void AttachOnGT(TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> InConnection);
		void SetFrameLogOnGT(TSharedRef<FSocketFrameLog, ESPMode::ThreadSafe> Log);
		void SchedulePump(); // producer threads; only schedules native pipe work
		bool PopOnGT(FSocketDelivery& Out);
		int32 GetDeliveryCountOnGT() const;
		FVaRestSocketStats GetStatsOnGT() const;
		void ConsumedOnGT(const FSocketDelivery& Delivery);
		/** GTK has accepted this input into its separately bounded ready-hold queue; payload charges remain held there. */
		void CompleteBatchDeliveryOnGT(const FSocketDelivery& Delivery);
		void StopAndWaitOnGT(); // closes scheduling, waits without executing parser work on GT, discards delivery
#if WITH_DEV_AUTOMATION_TESTS
		/** Only this native worker pipe's quiescence, not a task-graph/GTK delivery drain. */
		void WaitForPipeForTests() { check(IsInGameThread()); Pipe.WaitUntilEmpty(); }
#endif
	private:
		void Pump();
		bool Decode(FVaRestSocketEvent Event, uint64 BatchId);
		void RetireBatchInput(uint64 BatchId);
		void FlushConsumedOnPipe();
		FCriticalSection Admission;
		UE::Tasks::FPipe Pipe{TEXT("VaRest.Socket.Decode")};
		TSharedPtr<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection;
		TQueue<FSocketDelivery, EQueueMode::Spsc> Delivery;
		std::atomic<bool> bStopped{false}, bPumpScheduled{false};
		std::atomic<uint64> Wakes{0};
		std::atomic<int32> BatchRemaining{0};
		std::atomic<uint64> ActiveBatch{0};
		uint64 NextBatch = 0; // pipe only; zero is the already-delivered W4 marker
		std::atomic<int64> PendingBytes{0};
		std::atomic<int64> TotalFrames{0}, TotalBytes{0}, TotalDecodeErrors{0};
		std::atomic<int32> PendingEvents{0};
		std::atomic<int32> DeliveryCount{0};
		std::atomic<int64> PendingTransportConsumed{0};
		std::atomic<int32> PendingTransportMessages{0};
		const uint64 Generation;
		std::atomic<int64> MaxBytes, MaxMessageBytes;
		FVaRestHeartbeat Heartbeat;
		TArray<uint8> PingBytes, PongBytes; // immutable snapshot copied under Admission before decode
		TSharedPtr<IVaRestSocketCodec, ESPMode::ThreadSafe> Codec;
		FVaRestSocketCodecContext CodecContext;
		uint64 TraceSocketId = 0, TraceAttemptId = 0;
		TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> TraceRedactor;
		TSharedPtr<FSocketTraceClock, ESPMode::ThreadSafe> TraceClock;
		TArray<FVaRestTraceSocketFrame> PendingTraceFrames; // pipe only, shares the existing charged-event cap
		TSharedPtr<FSocketFrameLog, ESPMode::ThreadSafe> FrameLog;
		TArray<FSocketFrameLogCapture> PendingLogFrames; // pipe only; slots/bytes also bounded by FrameLog admission
		// Pipe only; cleanup is also a pipe task before StopAndWait returns.
		TSharedPtr<IVaRestSocketCodec, ESPMode::ThreadSafe> DecoderCodec;
		TUniquePtr<IVaRestSocketDecoder> Decoder;
		int64 PendingDecodeCharge = 0;
		int32 PendingDecodeEvents = 0;
		bool bDecoderPoisoned = false;
		bool bOverflow = false; // pipe only
		static constexpr int32 MaxEvents = 4096;
	};
}
