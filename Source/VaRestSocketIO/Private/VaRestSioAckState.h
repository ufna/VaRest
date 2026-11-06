// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "VaRestSioEmitWithAck.h"
#include "VaRestSioCodec.h"
#include <atomic>

namespace VaRest::Sio
{
struct FAckControl
{
	std::atomic<bool> bActive{true}, bCancelRequested{false};
	FCriticalSection Mutex;
	FVaRestSocketRequestHandle Native;
};
/** GT-only payload. The UV state tracks even unsent builders so retirement does not wait for their destructor. */
struct FLogicalAck : TSharedFromThis<FLogicalAck>
{
	~FLogicalAck();
	TWeakObjectPtr<UVaRestSioState> State;
	FVaRestSocketWeak Socket;
	TSharedPtr<FVaRestSioCodec, ESPMode::ThreadSafe> Codec;
	TArray<FVaRestSocketFrame> Frames;
	TOptional<FVaRestError> PreparationFailure;
	TSharedRef<FAckControl, ESPMode::ThreadSafe> Control = MakeShared<FAckControl, ESPMode::ThreadSafe>();
	uint64 Slot = 0, Serial = 0;
	FString Namespace, Event;
	double Timeout = 10.0;
	int32 Retries = 0;
	bool bErrorFirst = false, bOwned = false, bStarted = false, bRetired = false;
	TWeakObjectPtr<const UObject> Owner;
	TFunction<void(const FVaRestSioEvent&)> Success;
	TFunction<void(const FVaRestError&)> Error;
	TFunction<FVaRestError(const FVaRestSioEvent&)> ReplyTransform;
	TFunction<void()> Cleanup;
	void Start();
	void Retire();
	void Failed(FVaRestError Failure);
	void Replied(const FVaRestSocketMessage& Message, const FVaRestJson& Value);
};
}
