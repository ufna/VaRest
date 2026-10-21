// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestStage.h"

#include <atomic>

namespace VaRest::Http
{
	/**
	 * FVaRestExchange's own state: everything a stage may not touch (final review I1). Defined here, in a Private header, so
	 * a module outside VaRestHttp -- a game, a custom stage -- cannot name a field even by mistake; the exchange holds it by
	 * TUniquePtr, never null between its constructor and its destructor. Reach it with Internals().
	 */
	struct FExchangeState
	{
		/** Any thread; FVaRestExchange::IsCancelRequested() reads the same flag */
		bool IsCancelRequested() const { return bCancelRequested.load(); }

		// ---- Set by Send on the sending thread, read on the game thread after the hop:
		uint64 RequestId = 0;
		uint64 ClientId = 0;
		TSharedPtr<FClientState, ESPMode::ThreadSafe> State;
		FString Verb;
		FString RawPath;
		TArray<TPair<FString, FString>> PathParams;
		TArray<TPair<FString, FString>> Query;
		FVaRestHeaders OwnHeaders;
		FVaRestRequestBody Body;
		FString BuildError;
		FName Tag;
		EVaRestPriority Priority = EVaRestPriority::Normal;
		bool bIdempotent = false;
		bool bKeepBody = false;
		bool bOnHttpThread = false;
		FString SaveTo;
		TOptional<float> TimeoutOverride;
		TOptional<float> ActivityTimeoutOverride;
		TOptional<int64> MaxJsonBytesOverride;
		EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase;
		bool bStrictDecode = false;
		TWeakObjectPtr<const UObject> Owner;
		bool bHasOwner = false;
		const char* SourceFile = "";
		uint32 SourceLine = 0;
		/** Send ran on the game thread, so Enter runs inside it and the Blueprint stack is the caller's (IN-15) */
		bool bSentOnGameThread = false;
		FVaRestTraceInitiator Initiator;
		TUniquePtr<IResultHandler> Handler;
		/** Spec 3.1, decision 27: the configuration snapshot, taken by Send on the sending thread (Enter fills it only for an exchange made another way) */
		TSharedPtr<const FClientConfig, ESPMode::ThreadSafe> Config;

		// Resolved on entry (game thread):
		FString Url;
		FString PathTemplate;
		FVaRestHeaders RequestHeaders;
		float Timeout = 0.f;
		float ActivityTimeout = 0.f;
		int64 MaxJsonBytes = 0;
		FVaRestJsonConvertOptions DecodeOptions;
		EVaRestLogLevel LogLevel = EVaRestLogLevel::Basic;
		double StartedAt = 0.0;
		FDateTime StartUtc;

		// The current attempt (game thread):
		int32 Attempt = 0;
		double AttemptStartedAt = 0.0;
		double LimiterWait = 0.0;
		FVaRestHeaders AttemptHeaders;
		FString TempFile;
		TSharedPtr<IVaRestTransportCall, ESPMode::ThreadSafe> TransportCall;
		TSharedPtr<FVaRestTransportProbe, ESPMode::ThreadSafe> Probe;
		FVaRestTimerHandle ProgressTimer;
		FVaRestProgress LastProgress;
		bool bInLimiterQueue = false;

		// Life (game thread, except the atomics):
		EVaRestExchangePhase Phase = EVaRestExchangePhase::Created;
		EVaRestCancelReason CancelReason = EVaRestCancelReason::None;
		/** Complete ran: nothing else is delivered */
		bool bFinished = false;
		/**
		 * Inside Enter or Cancel() on this exchange: Complete defers the callbacks (decision 3). The scope is taken in Enter, so
		 * a game-thread Send() is covered, and so is the Enter task of a request sent from another thread (one extra hop).
		 */
		int32 ApiDepth = 0;
		std::atomic<bool> bCancelRequested{ false };
		std::atomic<bool> bDone{ false };
	};

	/**
	 * The one friend of FVaRestExchange. Declared in the public header, defined only here, so only code that includes this
	 * header can call it. Use Internals().
	 */
	struct FExchangeAccess
	{
		static FExchangeState& Get(FVaRestExchange& Exchange) { return *Exchange.Impl; }
		static const FExchangeState& Get(const FVaRestExchange& Exchange) { return *Exchange.Impl; }
	};

	/** Any thread (the state is made with the exchange and never replaced); each field says where it may be used */
	inline FExchangeState& Internals(FVaRestExchange& Exchange)
	{
		return FExchangeAccess::Get(Exchange);
	}

	inline const FExchangeState& Internals(const FVaRestExchange& Exchange)
	{
		return FExchangeAccess::Get(Exchange);
	}
}
