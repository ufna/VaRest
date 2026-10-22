// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestRetry.h"
#include "VaRestStage.h"

namespace VaRest::Http
{
	/** What the retry stage does with one outcome */
	struct FRetryDecision
	{
		bool bRetry = false;
		/** Seconds until the next attempt */
		double Delay = 0.0;
		/** What a Retry-After header asked for, in seconds, before the cap; -1 without one */
		double RetryAfter = -1.0;
		/** For the trace (IN-4 Reason) and the log line: "Status503", "Connection", "Timeout"; why not, when bRetry is false */
		FName Reason;
	};

	/**
	 * Spec 4.1, for one outcome. RetriesDone: retries already made. Now and DeadlineAt are the request's clock (DeadlineAt 0
	 * = none); NowUtc dates a Retry-After HTTP-date. Draws once from Random when it retries by backoff, never otherwise.
	 * Eligibility by verb is the caller's (FExchangeState::bRetryEligible).
	 */
	FRetryDecision DecideRetry(const FVaRestRetryPolicy& Policy, const FVaRestAttemptOutcome& Outcome, int32 RetriesDone, double Now, double DeadlineAt,
		const FDateTime& NowUtc, FRandomStream& Random);

	/** Spec 4's retry stage (Task 8): the first built-in Outer stage */
	FVaRestStageRef MakeRetryStage();
}
