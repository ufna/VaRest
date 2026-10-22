// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestAuthTypes.h"
#include "VaRestEnvelope.h"
#include "VaRestHttpStatsInternal.h"
#include "VaRestRetry.h"
#include "VaRestStage.h"

#include <atomic>

class FVaRestRedactor;

namespace VaRest::Http
{
	/**
	 * M4a final review A1 (VaRest-tqt): one transport dispatch's identity, made by the transport stage at each start and
	 * carried by its completion guard into FClientState::OnAttemptDone -- a parameter of its own, never a field of
	 * FVaRestTransportResult, so a transport cannot spoof it. The attempt number alone does not name a dispatch: a
	 * PerAttempt stage that proceeds twice starts two under one number (Decode.SaveToReissue), so the serial does. Probe
	 * and ResponseFile are that dispatch's own (the probe is made per FVaRestTransportRequest), so a stale end is stamped
	 * with its own transport times and deletes its own SaveTo file.
	 */
	struct FDispatchTag
	{
		int32 Attempt = 0;
		/** Per exchange, from 1 (FExchangeState::DispatchCount); 0 = no dispatch */
		uint32 Serial = 0;
		TSharedPtr<FVaRestTransportProbe, ESPMode::ThreadSafe> Probe;
		FString ResponseFile;
	};

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
		/** Spec 4.8: the sender's LLM tags, under which the callbacks run (FLlmSenderTags) */
		FLlmSenderTags SenderTags;
		TUniquePtr<IResultHandler> Handler;
		/** Spec 3.1, decision 27: the configuration snapshot, taken by Send on the sending thread (Enter fills it only for an exchange made another way) */
		TSharedPtr<const FClientConfig, ESPMode::ThreadSafe> Config;
		/**
		 * Game thread, RedactorFor's cache (review I-1): the union of Config's redactor and a later current one, and the
		 * current one it was built from -- held, so a new redactor at a freed one's address is never taken for it. Both
		 * go when the exchange completes.
		 */
		mutable TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> UnionRedactor;
		mutable TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> UnionBuiltFrom;
		/**
		 * Game thread (Task 12 review M-7): what this exchange's attempts carried beyond the Send() snapshot. AuthApply writes
		 * the session as it is when an attempt leaves the limiter's queue, so a token or session header set after Send() goes
		 * out, and a logout before the outcome makes the client forget it while the request still runs. When AuthApply finds
		 * the client's redactor is not the one this exchange knows, it joins it in here: Config's redactor and every such one,
		 * across attempts. Null while every attempt applied under Config's. AppliedKeys are its layer-2 keys (FVaRestRedactor
		 * does not list them). Kept until the exchange dies: a late outcome after Complete is still logged (RedactorFor).
		 */
		TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> AppliedRedactor;
		TArray<FString> AppliedKeys;

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
		/** A1: transport starts so far; each FDispatchTag takes the next serial */
		uint32 DispatchCount = 0;
		/**
		 * A1: the dispatch whose call TransportCall holds -- its serial (0 = none, exactly when TransportCall is null) and
		 * attempt. Only that dispatch's end is the current one: it alone resets TransportCall, clears the progress timer,
		 * delivers the final progress and touches the excerpt marker (OnAttemptDone).
		 */
		uint32 LiveDispatch = 0;
		int32 LiveDispatchAttempt = 0;
		/** The latest dispatch's probe (DeliverProgress reads it while that dispatch is live) */
		TSharedPtr<FVaRestTransportProbe, ESPMode::ThreadSafe> Probe;
		FVaRestTimerHandle ProgressTimer;
		FVaRestProgress LastProgress;
		bool bInLimiterQueue = false;

		// Retry (Tasks 8-9), set by Resolve:
		/** The request's Retry()/NoRetry(), from Send */
		TOptional<FVaRestRetryPolicy> RetryOverride;
		/** That, else the client's (WithRetry) */
		FVaRestRetryPolicy RetryPolicy = FVaRestRetryPolicy::Disabled();
		/** Spec 4.1: an enabled policy, and an idempotent verb or Idempotent() */
		bool bRetryEligible = false;
		/** Plan decision 8: a later attempt can need the body again, so the transport stage copies it */
		bool bMayReissue = false;
		/** Plan decision 10: StartedAt + the policy's Deadline, clock seconds; 0 = none. Set for any policy with one, eligible or not. */
		double DeadlineAt = 0.0;
		/** Concurrency review M-1: the deadline's timer while an attempt waits in the limiter's queue */
		FVaRestTimerHandle QueueDeadline;
		/** Set by that timer before it takes the entry out of the queue: the abort answers Deadline, not Canceled */
		bool bQueueDeadlinePassed = false;
		int32 RetriesDone = 0;

		// Envelope (Task 11):
		/** The request's Envelope(), from Send */
		TOptional<FVaRestEnvelope> EnvelopeOverride;
		/** NoEnvelope(), from Send */
		bool bNoEnvelope = false;
		/** Spec 3.4, set by Resolve: the request's, else the client's; null with NoEnvelope() or none configured */
		TSharedPtr<const FVaRestEnvelope, ESPMode::ThreadSafe> Envelope;

		// Auth (Tasks 14-17): AuthMode from Send, the rest per attempt (game thread)
		EVaRestAuthMode AuthMode = EVaRestAuthMode::Required;
		/** The refresh request itself (plan decision 23): the gate (Task 17), AuthApply and the limiter pass it through */
		bool bIsAuthRefresh = false;
		/** The URL of the current attempt: Url (the attempt boundary) plus the auth's query pairs (AuthApply) */
		FString AttemptUrl;
		/**
		 * FVaRestSessionData::CredentialGeneration when AuthApply wrote this attempt's credentials: the auth gate's "newer
		 * credentials" replay compares it (Task 17). Not Generation: a session header must not use up the replay (M-6).
		 */
		uint32 AppliedCredentialGeneration = 0;
		/**
		 * IN-2: seconds this attempt waited behind a refresh inside it (a park before it started, its Inspector 3.3 Auth
		 * phase), and that refresh's id (also for a replay, whose park is its Gap instead); the auth gate sets both for each
		 * attempt it lets through, and the transport stage resets them after the dispatch
		 */
		double AuthWait = 0.0;
		uint64 AuthRefreshId = 0;
		/**
		 * Spec review I-1: when an attempt that has not started parked behind a refresh; the boundary starts it there. -1 =
		 * none. M4a final review C-I-1: that park took the attempt's number (the gate incremented Attempt when it set this),
		 * so the boundary does not number the attempt again.
		 */
		double PendingAttemptStart = -1.0;
		/**
		 * M4a final review C-I-1: the refresh this request's last park waited on (0 = none), before an attempt or for a
		 * replay -- IN-5's Completed.AuthRefreshId, so a request a park ended unsent still names its refresh
		 */
		uint64 ParkRefreshId = 0;

		/**
		 * IN-14, game thread: the attempt whose 2xx response the decode task excerpts (0 = none), and that response's size.
		 * AttemptEnded sets it when it announces that excerpt, at the current dispatch's end only (M4a final review A1: a
		 * stale end never touches it); OnChainDone hands it to the decode task, and clears it, only
		 * for the outcome of that same attempt. One still set at the next AttemptEnded or at Completed was never handed
		 * over: Excerpts::ResolveDiscarded resolves it (Task 21 review M-1).
		 */
		int32 ExcerptOnDecodeAttempt = 0;
		int64 ExcerptOnDecodeBytes = 0;

		/**
		 * Plan decision 7. Game thread. Set by a stage while it holds this exchange waiting (a backoff, a park behind a
		 * refresh); FClientState::Cancel takes it and calls it, and the stage answers Canceled through the chain. When
		 * something else answered the request while it waited, FClientState::EndWaits calls it too, so the wait's timer or
		 * park goes with the answer: from Complete (concurrency review M-3), from a deferred Complete (Task 8 review minor
		 * 1) and from OnChainDone before it hands a result to the decode task (re-review N-6).
		 * It returns true when it answered; false (the stage's state is already gone) lets Cancel finish the request itself
		 * (review M-4). The stage clears it when the wait ends. It holds the stage's state weakly, so it makes no cycle
		 * through this exchange.
		 */
		TUniqueFunction<bool()> Interrupt;

		/**
		 * IN-2: what the next AttemptDispatched reports; the transport stage resets both after the dispatch. For a Backoff
		 * gap the attempt boundary measures NextGapSeconds from LastAttemptEndedAt (spec review M-10).
		 */
		EVaRestTraceGap NextGap = EVaRestTraceGap::None;
		double NextGapSeconds = 0.0;
		/** Clock seconds: when the last attempt's outcome reached the retry stage (the same game-thread call as its end) */
		double LastAttemptEndedAt = 0.0;

		// Life (game thread, except the atomics):
		EVaRestExchangePhase Phase = EVaRestExchangePhase::Created;
		EVaRestCancelReason CancelReason = EVaRestCancelReason::None;
		/** Complete ran: nothing else is delivered */
		bool bFinished = false;
		/**
		 * Complete accepted an answer and queued its delivery for a later game-thread task (ApiDepth > 0, decision 3). From
		 * then on the request is finished in all but name: Proceed starts no attempt and OnChainDone takes no second outcome
		 * (Task 8 review minor 1).
		 */
		bool bCompletionQueued = false;
		/**
		 * Inside Enter or Cancel() on this exchange: Complete defers the callbacks (decision 3). The scope is taken in Enter, so
		 * a game-thread Send() is covered, and so is the Enter task of a request sent from another thread (one extra hop).
		 */
		int32 ApiDepth = 0;
		/**
		 * Spec 4.8: Enter counted this exchange as a request in flight (FHttpStats::RequestStarted). Set once, never cleared:
		 * an exchange entered a second time (the VaRest.Http.Auth.InFlightEntryIsTheExchange case) is still one request, and
		 * Complete -- whose body runs once, behind bFinished -- counts out only one that Enter counted.
		 */
		bool bCounted = false;
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

	/** Game thread. IsValid() is false for an object marked as garbage, not only for one collected (WeakObjectPtr.h:290) */
	inline bool IsOwnerGone(const FExchangeState& Ex)
	{
		return Ex.bHasOwner && !Ex.Owner.IsValid();
	}

	/** Game thread */
	inline void MarkOwnerGone(FExchangeState& Ex)
	{
		// Silence wins over an earlier User cancel (review C-2); Shutdown is silent too, and stays
		if (Ex.CancelReason != EVaRestCancelReason::Shutdown)
		{
			Ex.CancelReason = EVaRestCancelReason::OwnerGone;
		}
		Ex.bCancelRequested = true;
	}
}
