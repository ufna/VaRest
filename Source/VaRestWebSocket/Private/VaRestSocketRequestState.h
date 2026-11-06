// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestSocketRequest.h"
#include <atomic>

namespace VaRest::WebSocket
{
	/** Only this pure native control is shared with the outbox or request handle. */
	struct FSocketRequestControl
	{
		std::atomic<bool> bActive{true}, bCancelRequested{false};
	};
	/** GTK-only callback/owner payload. Never captured by a codec task or native outbox unit. */
	struct FSocketJsonRequestOptions
	{
		FString Route;
		FString Channel;
		FVaRestJson Payload;
		/** GTK-only owned struct snapshot. Never enters a codec worker or native outbox. */
		TUniqueFunction<bool(VaRest::Json::FCondensedWriter&, bool, TConstArrayView<FString>, const FVaRestJsonConvertOptions&, FVaRestJsonError&)> PayloadWriter;
		TOptional<FVaRestError> PayloadFailure;
		FVaRestSocket::FRequestFrameFactory FrameFactory;
		/** Eager Blueprint encoding owns only native frames; timeout/pending admission still starts at Send. */
		TOptional<FVaRestSocketId> PreparedId;
		uint64 PreparedEpoch = 0, PreparedEncoderRevision = 0, PreparedQueueEpoch = 0;
		TUniqueFunction<FVaRestError(const FVaRestSocketMessage&, FVaRestJson&)> ReplyTransform;
		TUniqueFunction<void()> Retired;
		const UScriptStruct* PayloadStruct = nullptr; // borrowed from the GTK-only writer's owned GC carrier
		const void* PayloadData = nullptr;
		double Timeout = 10.0;
		TWeakObjectPtr<const UObject> Owner;
		bool bOwned = false;
		TFunction<void(const FVaRestJson&)> Success;
		TFunction<void(const FVaRestSocketMessage&)> MessageSuccess;
		TFunction<void(const FVaRestSocketMessage&, const FVaRestJson&)> ProtocolSuccess;
		TFunction<void(const FVaRestError&)> Error;
	};
	struct FSocketJsonPending
	{
		~FSocketJsonPending() { check(IsInGameThread()); if (Options.Retired) { auto Cleanup = MoveTemp(Options.Retired); Cleanup(); } }
		FSocketJsonRequestOptions Options;
		FVaRestSocketId Id;
		FString Channel;
		double Deadline = 0.0, StartedAt = 0.0;
		uint64 SentAttemptId = 0;
		TSharedPtr<FSocketRequestControl, ESPMode::ThreadSafe> Control;
		bool bSent = false;
		TOptional<FVaRestError> Failure;
	};
}
