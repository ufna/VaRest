// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestRetryInternal.h"

#include "VaRestClassify.h"

FVaRestRetryPolicy FVaRestRetryPolicy::Disabled()
{
	FVaRestRetryPolicy Policy;
	Policy.MaxRetries = 0;
	return Policy;
}

FVaRestRetryPolicy FVaRestRetryPolicy::Exponential(int32 InMaxRetries)
{
	FVaRestRetryPolicy Policy;
	Policy.MaxRetries = FMath::Max(0, InMaxRetries);
	return Policy;
}

FVaRestRetryPolicy FVaRestRetryPolicy::WithDeadline(float Seconds) const
{
	FVaRestRetryPolicy Copy = *this;
	Copy.Deadline = FMath::IsFinite(Seconds) ? FMath::Max(0.f, Seconds) : 0.f;
	return Copy;
}

FVaRestRetryPolicy FVaRestRetryPolicy::WithBackoff(const FVaRestBackoff& InBackoff) const
{
	FVaRestRetryPolicy Copy = *this;
	Copy.Backoff = InBackoff;
	return Copy;
}

bool VaRest::Http::IsIdempotentVerb(FStringView Verb)
{
	return Verb.Equals(TEXTVIEW("GET"), ESearchCase::IgnoreCase) || Verb.Equals(TEXTVIEW("HEAD"), ESearchCase::IgnoreCase)
		|| Verb.Equals(TEXTVIEW("OPTIONS"), ESearchCase::IgnoreCase) || Verb.Equals(TEXTVIEW("PUT"), ESearchCase::IgnoreCase)
		|| Verb.Equals(TEXTVIEW("DELETE"), ESearchCase::IgnoreCase);
}

VaRest::Http::FRetryDecision VaRest::Http::DecideRetry(const FVaRestRetryPolicy& Policy, const FVaRestAttemptOutcome& Outcome, int32 RetriesDone,
	double Now, double DeadlineAt, const FDateTime& NowUtc, FRandomStream& Random)
{
	FRetryDecision Out;
	if (!Policy.IsEnabled() || RetriesDone >= Policy.MaxRetries)
	{
		Out.Reason = TEXT("Exhausted");
		return Out;
	}
	// Plan decision 12: the same classification the request's FVaRestError gets
	const FAttemptClass Class = ClassifyAttempt(Outcome);
	bool bEligible = false;
	switch (Class.Kind)
	{
	case EVaRestErrorKind::Connection:
		bEligible = Class.bRetryable && Policy.bRetryConnectionErrors;
		Out.Reason = TEXT("Connection");
		break;
	case EVaRestErrorKind::Timeout:
		bEligible = Class.bRetryable && Policy.bRetryTimeouts;
		Out.Reason = TEXT("Timeout");
		break;
	case EVaRestErrorKind::Http:
		// The policy's list, not FVaRestError::IsRetryableStatus: a game may add 409 or drop 500
		bEligible = Policy.RetryStatuses.Contains(Outcome.Status);
		Out.Reason = FName(*FString::Printf(TEXT("Status%d"), Outcome.Status));
		break;
	default:
		Out.Reason = TEXT("NotRetryable");
		break;
	}
	if (!bEligible)
	{
		return Out;
	}
	TOptional<double> Asked;
	if (Class.Kind == EVaRestErrorKind::Http && Policy.bRespectRetryAfter)
	{
		if (const FString* Header = Outcome.Headers.Find(TEXT("Retry-After")))
		{
			Asked = ParseRetryAfter(*Header, NowUtc);
		}
	}
	double Delay = 0.0;
	if (Asked.IsSet())
	{
		// Plan decision 11: what the server asked, capped (spec 4.1: 60 s)
		Out.RetryAfter = Asked.GetValue();
		const double Cap = FMath::IsFinite(Policy.MaxRetryAfter) ? FMath::Max(0.0, static_cast<double>(Policy.MaxRetryAfter)) : 0.0;
		Delay = FMath::Min(Asked.GetValue(), Cap);
	}
	else
	{
		Delay = Policy.Backoff.GetDelay(RetriesDone + 1, Random);
	}
	if (DeadlineAt > 0.0 && Now + Delay >= DeadlineAt)
	{
		// Plan decision 10: the next attempt would start at or past the deadline
		Out.Reason = TEXT("Deadline");
		return Out;
	}
	Out.bRetry = true;
	Out.Delay = Delay;
	return Out;
}
