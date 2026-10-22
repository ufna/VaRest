// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "Misc/DateTime.h"

#include "VaRestBackoff.generated.h"

/** How a backoff step's delay is spread (spec 4.1 "full jitter"; WebSocket spec 3.4's Ratio) */
UENUM(BlueprintType)
enum class EVaRestJitter : uint8
{
	/** Uniform in [0, d): spec 4.1's default */
	Full,
	/** d * (1 +/- JitterRatio), uniform, then capped: socket.io-client's style (WebSocket spec 3.4, 8) */
	Ratio,
	/** Exactly d */
	None
};

/**
 * WebSocket spec 19 item 1: the core's backoff formula, for HTTP retry and the socket reconnect policy alike. Step 1 is
 * the wait before the first retry: d = min(MaxDelay, BaseDelay * Multiplier^(Step - 1)), then jittered. BlueprintType
 * because M5's asset and the socket policy embed it: its field names freeze with M5 (plan decision 27).
 */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestBackoff
{
	GENERATED_BODY()

	/** Seconds before the first retry, before jitter (spec 4.1: 0.5) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http", meta = (ClampMin = "0"))
	float BaseDelay = 0.5f;

	/** Each step multiplies the delay (spec 4.1: 2); below 1 counts as 1 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http", meta = (ClampMin = "1"))
	float Multiplier = 2.f;

	/** No step waits longer, jitter included (spec 4.1: 30) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http", meta = (ClampMin = "0"))
	float MaxDelay = 30.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	EVaRestJitter Jitter = EVaRestJitter::Full;

	/** Ratio jitter only: the spread, 0 to 1 (socket.io-client: 0.5) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http", meta = (ClampMin = "0", ClampMax = "1"))
	float JitterRatio = 0.5f;

	/** The un-jittered delay of Step; 0 below step 1. A non-finite or negative field counts as 0 (Multiplier as 1). */
	double GetCeiling(int32 Step) const;

	/**
	 * GetCeiling(Step), jittered with Random -- the caller's stream (plan decision 26): a client's, a socket's, a test's
	 * seeded one. One draw for Full and Ratio, none for None or below step 1. Never below 0, never above MaxDelay.
	 */
	double GetDelay(int32 Step, FRandomStream& Random) const;
};

namespace VaRest::Http
{
	/**
	 * RFC 9110 10.2.3 Retry-After: delay-seconds (digits only, surrounding spaces ignored) or an HTTP-date
	 * (FDateTime::ParseHttpDate, DateTime.h:695-713), as seconds from NowUtc; a date in the past is 0, and seconds stop
	 * growing at 1e9. Unset for anything else: a negative number, a fraction, text, a list. Not capped: the retry policy is
	 * (spec 4.1: 60 s). One value: of a response with several Retry-After lines the retry stage passes the first. On Mac
	 * and iOS NSURLSession folds repeated lines into one value (allHeaderFields, AppleHttp.cpp:55), and a folded "2, 5"
	 * is unset here, so the policy's backoff applies.
	 *
	 * ParseHttpDate is lenient (Misc/DateTime.cpp:612, :663, :676) and its leniency means "retry now": an RFC 850 date's
	 * two-digit year lands in the 1900s, a past date, so 0; a date whose time part does not parse is read as midnight, so 0
	 * once that midnight has passed. The wait is then the policy's bounded one, never longer than the header asked.
	 */
	VARESTHTTP_API TOptional<double> ParseRetryAfter(FStringView Value, const FDateTime& NowUtc);
}
