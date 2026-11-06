// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestOpenApiModel.h"
#include "VaRestTransport.h"
#include "Misc/ScopeLock.h"

namespace VaRest::OpenApi
{
/** Published through a lock because a logical cancellation can precede physical curl completion. */
struct FSourceFetchStatus
{
	FCriticalSection Mutex;
	FString Error,CanonicalUri;
	void SetError(FString Value) { FScopeLock Lock(&Mutex);Error=MoveTemp(Value); }
	void SetUri(FString Value) { FScopeLock Lock(&Mutex);CanonicalUri=MoveTemp(Value); }
	void Read(FString& OutError,FString& OutUri) { FScopeLock Lock(&Mutex);OutError=Error;OutUri=CanonicalUri; }
};
TSharedRef<IVaRestTransport,ESPMode::ThreadSafe> SourceTransport(const FVaRestOpenApiReadRequest& Boundary,TSharedRef<FSourceFetchStatus,ESPMode::ThreadSafe> Status);
/** GT: refuse new starts, cancel and wait for owned network tasks before SSL dependencies shut down. */
void ShutdownSourceTransport();
}
