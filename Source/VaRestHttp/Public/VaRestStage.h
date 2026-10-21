// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "VaRestBody.h"
#include "VaRestClock.h"
#include "VaRestHttpTypes.h"
#include "VaRestJsonConvert.h"
#include "VaRestResultHandler.h"
#include "VaRestTrace.h"
#include "VaRestTransport.h"

#include <atomic>

class FVaRestExchange;
class IVaRestStage;

namespace VaRest::Http
{
	class FClientState;
	struct FClientConfig;
}

/** What one attempt produced, travelling back up the chain (spec 4) */
struct VARESTHTTP_API FVaRestAttemptOutcome
{
	/** 0 = no response */
	int32 Status = 0;
	FVaRestHeaders Headers;
	FString EffectiveUrl;
	FString ContentType;
	/** With one byte of slack for ParseOwned (spec 2.1); empty when the body went to a file */
	TArray<uint8> Body;
	bool bBodyInFile = false;
	EVaRestTransportFailure Failure = EVaRestTransportFailure::None;
	FName FailureDetail;
	int64 BytesUp = 0;
	int64 BytesDown = 0;

	bool IsTransportFailure() const { return Failure != EVaRestTransportFailure::None; }

	/** What Proceed reports for an exchange that is finished or being canceled */
	static FVaRestAttemptOutcome Canceled();
};

using FVaRestOutcomeCallback = TUniqueFunction<void(FVaRestAttemptOutcome&&)>;
using FVaRestStageRef = TSharedRef<IVaRestStage, ESPMode::ThreadSafe>;
using FVaRestStageChain = TArray<FVaRestStageRef>;
using FVaRestExchangeRef = TSharedRef<FVaRestExchange, ESPMode::ThreadSafe>;

/** The rest of the chain after one stage: a cheap value, so a stage may keep it and proceed later (after a timer, a refresh) */
class VARESTHTTP_API FVaRestStageNext
{
public:
	FVaRestStageNext(TSharedRef<const FVaRestStageChain, ESPMode::ThreadSafe> InChain, int32 InIndex)
		: Chain(MoveTemp(InChain))
		, Index(InIndex)
	{
	}

	/** Game thread. Runs the remaining stages for Exchange; OnOutcome receives what they produce. A finished or canceled exchange reports Canceled at once. */
	void Proceed(const FVaRestExchangeRef& Exchange, FVaRestOutcomeCallback&& OnOutcome) const;

private:
	TSharedRef<const FVaRestStageChain, ESPMode::ThreadSafe> Chain;
	int32 Index = 0;
};

/** Spec 4: asynchronous middleware. Add one with FVaRestClient::Use(Stage, EVaRestStageSlot). */
class IVaRestStage
{
public:
	virtual ~IVaRestStage() = default;

	virtual FName GetStageName() const = 0;

	/** True when Process may call Next.Proceed more than once (retry, replay): the transport then copies the body instead of moving it (decision 12) */
	virtual bool MayReissue() const { return false; }

	/**
	 * Game thread. Pass the exchange on with Next.Proceed (now or later; each call is another trip through the stages after
	 * this one), or answer it yourself through OnOutcome (a cache hit). Call OnOutcome exactly once. A PerAttempt stage may
	 * edit Exchange->GetAttemptHeaders() before proceeding (request signing).
	 */
	virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) = 0;
};

enum class EVaRestExchangePhase : uint8
{
	/** Sent, not yet on the game thread */
	Created,
	/** In the stage chain */
	InChain,
	/** A background task holds the result handler */
	Decoding,
	Done
};

/**
 * One logical request moving through a client's stages (spec 4). Game thread, except where marked. Stages use the
 * accessors; the fields below them are VaRestHttp's own and not a stable API.
 */
class VARESTHTTP_API FVaRestExchange : public TSharedFromThis<FVaRestExchange, ESPMode::ThreadSafe>
{
public:
	FVaRestExchange() = default;
	/**
	 * Out of line (VaRestPipeline.cpp). Off the game thread it sends its State and Config there to die (review I-1
	 * backstop): with them go the client state, custom stages, the transport and the clock, whose destructors may clear
	 * game-thread timers.
	 */
	~FVaRestExchange();
	FVaRestExchange(const FVaRestExchange&) = delete;
	FVaRestExchange& operator=(const FVaRestExchange&) = delete;

	// ---- for stages
	uint64 GetRequestId() const { return RequestId; }
	uint64 GetClientId() const { return ClientId; }
	const FString& GetVerb() const { return Verb; }
	/** Base URL + expanded path + query */
	const FString& GetUrl() const { return Url; }
	/** The path before {param} substitution, without the query (Inspector IN-1) */
	const FString& GetPathTemplate() const { return PathTemplate; }
	FName GetTag() const { return Tag; }
	EVaRestPriority GetPriority() const { return Priority; }
	bool IsIdempotent() const { return bIdempotent; }
	/** Client defaults, then the request's own */
	const FVaRestHeaders& GetRequestHeaders() const { return RequestHeaders; }
	/** A fresh copy of GetRequestHeaders() for each attempt; PerAttempt stages may edit it */
	FVaRestHeaders& GetAttemptHeaders() { return AttemptHeaders; }
	const FVaRestHeaders& GetAttemptHeaders() const { return AttemptHeaders; }
	const FVaRestRequestBody& GetBody() const { return Body; }
	/** 0 before the attempt boundary, then 1, 2... */
	int32 GetAttempt() const { return Attempt; }
	/** Any thread */
	bool IsCancelRequested() const { return bCancelRequested.load(); }
	/** Any thread: the callbacks have been delivered (or dropped) */
	bool IsDone() const { return bDone.load(); }

	// ---- VaRestHttp internals. Set by Send on the sending thread, read on the game thread after the hop:
	uint64 RequestId = 0;
	uint64 ClientId = 0;
	TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State;
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
	FVaRestTraceInitiator Initiator;
	TUniquePtr<VaRest::Http::IResultHandler> Handler;
	/** Spec 3.1, decision 27: the configuration snapshot, taken by Send on the sending thread (Enter fills it only for an exchange made another way) */
	TSharedPtr<const VaRest::Http::FClientConfig, ESPMode::ThreadSafe> Config;

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
