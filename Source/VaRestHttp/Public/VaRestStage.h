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

class FVaRestExchange;
class IVaRestStage;

namespace VaRest::Http
{
	class FClientState;
	struct FClientConfig;
	struct FExchangeState;
	struct FExchangeAccess;
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
	bool bFromCache = false;
    bool bQueued = false;
    FString QueueId;
	EVaRestTransportFailure Failure = EVaRestTransportFailure::None;
	FName FailureDetail;
	/** A stage's own reason for a failure it made (an auth refresh rejected, no credentials): the error's Message when set */
	FString FailureMessage;
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

	/**
	 * Game thread. Runs the remaining stages for Exchange; OnOutcome receives what they produce. An exchange that is
	 * canceled, or whose answer is already accepted (being decoded, delivered, or its delivery queued), reports Canceled
	 * at once and starts no attempt.
	 */
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
	 * A PerAttempt stage holds one of the client's MaxConcurrent slots from the limiter's grant until it proceeds or answers,
	 * and Cancel() does not return it sooner (ruling I-3: the limiter opens the attempt). Work that waits on another request
	 * through the same client belongs in an Outer stage or on another client: the request it waits on could queue forever
	 * behind the slot the stage holds (task 4 review I-1).
	 * A PerAttempt stage that answers while its call still runs gets that call canceled and its limiter slot released, when the
	 * attempt's outcome returns through the attempt boundary (M4a final review A1); it cannot keep the call downloading.
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
 * One logical request moving through a client's stages (spec 4). Game thread, except where marked. A stage reads the
 * request through the accessors below. The one change it may make is to GetAttemptHeaders(), from a PerAttempt stage
 * (request signing). Replacing the body is not supported in 2.0: its length and Content-Type were fixed when the request
 * was built, and the transport, the progress totals and the trace rely on them (final review I1). A stage that needs it,
 * to compress or encrypt, is an M4 design question. The rest of an exchange is VaRestHttp's own state, in a Private header.
 */
class VARESTHTTP_API FVaRestExchange : public TSharedFromThis<FVaRestExchange, ESPMode::ThreadSafe>
{
public:
	/** Out of line (VaRestPipeline.cpp), with the accessors: the state it makes is complete only in VaRestHttp */
	FVaRestExchange();
	/**
	 * Out of line (VaRestPipeline.cpp). Off the game thread it sends its State and Config there to die (review I-1
	 * backstop): with them go the client state, custom stages, the transport and the clock, whose destructors may clear
	 * game-thread timers.
	 */
	~FVaRestExchange();
	FVaRestExchange(const FVaRestExchange&) = delete;
	FVaRestExchange& operator=(const FVaRestExchange&) = delete;

	// ---- for stages
	uint64 GetRequestId() const;
	uint64 GetClientId() const;
	const FString& GetVerb() const;
	/** Base URL + expanded path + query */
	const FString& GetUrl() const;
	/** The current attempt's URL: GetUrl() plus an auth's query pairs (an API key), from the auth stage on (plan decision 19) */
	const FString& GetAttemptUrl() const;
	/** The path before {param} substitution, without the query (Inspector IN-1) */
	const FString& GetPathTemplate() const;
	FName GetTag() const;
	EVaRestPriority GetPriority() const;
	bool IsIdempotent() const;
	/** Client defaults, then the request's own */
	const FVaRestHeaders& GetRequestHeaders() const;
	/** A fresh copy of GetRequestHeaders() for each attempt; PerAttempt stages may edit it */
	FVaRestHeaders& GetAttemptHeaders();
	const FVaRestHeaders& GetAttemptHeaders() const;
	const FVaRestRequestBody& GetBody() const;
	/** 0 before the attempt boundary, then 1, 2... */
	int32 GetAttempt() const;
	/** Any thread */
	bool IsCancelRequested() const;
	/**
	 * Any thread: the callbacks have been delivered (or dropped). With OnHttpThread(), true once the result is handed to the
	 * worker; the callbacks may still be running.
	 */
	bool IsDone() const;

private:
	/** Defined in VaRestExchangeState.h (Private): only VaRestHttp's own code reaches Impl */
	friend struct VaRest::Http::FExchangeAccess;

	/** Never null between the constructor and the destructor */
	TUniquePtr<VaRest::Http::FExchangeState> Impl;
};
