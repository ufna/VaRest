// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestBackoff.h"

#include "VaRestRetry.generated.h"

/**
 * Spec 4.1. A request is retried only when it is idempotent -- GET, HEAD, OPTIONS, PUT or DELETE, or marked Idempotent()
 * -- and its client or the request itself has an enabled policy: a client without WithRetry() does not retry (plan
 * decision 1). BlueprintType: M5's asset embeds it, and its field names freeze then (plan decision 27).
 */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestRetryPolicy
{
	GENERATED_BODY()

	/** Attempts after the first (spec 4.1: 3); 0 = no retry. A session replay after a refresh is not a retry (plan decision 2). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http", meta = (ClampMin = "0"))
	int32 MaxRetries = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FVaRestBackoff Backoff;

	/**
	 * The statuses retried (spec 4.1). Only error statuses are read: a success (2xx) listed here has no effect, since a
	 * success is never retried.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	TArray<int32> RetryStatuses = { 408, 425, 429, 500, 502, 503, 504 };

	/** No usable response: refused, reset, empty, or no data for the activity timeout */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	bool bRetryConnectionErrors = true;

	/** The attempt's total timeout passed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	bool bRetryTimeouts = true;

	/**
	 * Wait what a Retry-After header asks (seconds or an HTTP-date) instead of the backoff, at most MaxRetryAfter (plan
	 * decision 11). Its first line is read; one the platform folded from several lines ("2, 5", Mac and iOS) is not a
	 * delay, and the backoff applies.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	bool bRespectRetryAfter = true;

	/** Seconds (spec 4.1: 60) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http", meta = (ClampMin = "0"))
	float MaxRetryAfter = 60.f;

	/**
	 * Seconds from the request's start; 0 = none (spec 4.1, 4.7; plan decision 10). No retry starts at or past it, each
	 * attempt's total timeout is cut to the time left, and an attempt that would start past it fails Kind=Timeout without
	 * connecting. Waiting in the limiter's queue counts.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http", meta = (ClampMin = "0"))
	float Deadline = 0.f;

	/** No retries */
	static FVaRestRetryPolicy Disabled();
	/** Spec 4.1's defaults with InMaxRetries retries */
	static FVaRestRetryPolicy Exponential(int32 InMaxRetries = 3);
	FVaRestRetryPolicy WithDeadline(float Seconds) const;
	FVaRestRetryPolicy WithBackoff(const FVaRestBackoff& InBackoff) const;

	bool IsEnabled() const { return MaxRetries > 0; }
};

namespace VaRest::Http
{
	/** Spec 4.1 (RFC 9110 9.2.2): GET, HEAD, OPTIONS, PUT, DELETE, any case. POST and PATCH retry only with Idempotent(). */
	VARESTHTTP_API bool IsIdempotentVerb(FStringView Verb);
}
