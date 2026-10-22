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
	 * Retry-After is read only for an HTTP status the policy retries, never for a connection failure or a timeout, and only
	 * its first line (FVaRestHeaders::Find; RFC 9110 10.2.3 makes it a single value). On Mac and iOS NSURLSession folds
	 * repeated lines into one (allHeaderFields, AppleHttp.cpp:55): "2, 5" is no delay, so the backoff applies. A retry
	 * whose next attempt would start at or past the deadline is refused, the boundary included: Now + delay == DeadlineAt
	 * is refused (plan decision 10: no attempt starts at or past it). Eligibility by verb is the caller's
	 * (FExchangeState::bRetryEligible, which the retry stage reads).
	 */
	FRetryDecision DecideRetry(const FVaRestRetryPolicy& Policy, const FVaRestAttemptOutcome& Outcome, int32 RetriesDone, double Now, double DeadlineAt,
		const FDateTime& NowUtc, FRandomStream& Random);

	/** Spec 4's retry stage (Task 8): the first built-in Outer stage */
	FVaRestStageRef MakeRetryStage();

	/** Plan decision 10: what an attempt that would start past the retry deadline answers -- Kind=Timeout, not retryable */
	FVaRestAttemptOutcome MakeDeadlineOutcome();

	/**
	 * A deadline timer that fires this close to DeadlineAt counts as the deadline. The virtual clock keeps whole nanoseconds,
	 * so at a timer's due time Now() can read one ULP below a double DeadlineAt, and a re-arm for that rest rounds to 0 ns
	 * and fires again at the same instant, forever (the secret-expiry loop of M3 plan re-review N-I3,
	 * m3-plan-rereview-spec.md). Any rest above it is re-armed, never answered early: the real clock's core ticker counts
	 * game-time deltas, which a fixed timestep can run faster than Now() (FVaRestRealClock, Task 9 review minor 2). Used by
	 * the limiter queue's deadline timer (VaRestPipeline.cpp) and the auth gate's park (VaRestAuthGate.cpp).
	 */
	inline constexpr double DeadlineTimerSlack = 1e-3;
}
