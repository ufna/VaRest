// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestBackoff.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBackoffFormulaTest, "VaRest.Http.Backoff.Formula",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBackoffFormulaTest::RunTest(const FString& Parameters)
{
	// Spec 4.1: 0.5 s, x2, at most 30 s
	const FVaRestBackoff Default;
	const double Expected[] = { 0.0, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 30.0, 30.0 };
	for (int32 Step = 0; Step < UE_ARRAY_COUNT(Expected); ++Step)
	{
		TestEqual(FString::Printf(TEXT("step %d"), Step), Default.GetCeiling(Step), Expected[Step]);
	}
	TestEqual(TEXT("a negative step waits nothing"), Default.GetCeiling(-3), 0.0);
	TestEqual(TEXT("a huge step stays at the cap (no overflow)"), Default.GetCeiling(100000), 30.0);
	FVaRestBackoff Odd;
	Odd.Multiplier = 0.5f;
	TestEqual(TEXT("a multiplier below 1 counts as 1"), Odd.GetCeiling(5), 0.5);
	Odd = FVaRestBackoff();
	Odd.BaseDelay = 1e30f;
	TestEqual(TEXT("a huge base stays at the cap"), Odd.GetCeiling(1), 30.0);
	Odd = FVaRestBackoff();
	Odd.BaseDelay = std::numeric_limits<float>::quiet_NaN();
	TestEqual(TEXT("a NaN base counts as 0"), Odd.GetCeiling(3), 0.0);
	Odd = FVaRestBackoff();
	Odd.MaxDelay = -1.f;
	TestEqual(TEXT("a negative cap counts as 0"), Odd.GetCeiling(3), 0.0);
	Odd = FVaRestBackoff();
	Odd.BaseDelay = 0.f;
	Odd.Multiplier = 1e30f;
	// Spec review M-2: a finite multiplier whose power overflows -- 0 x 1e30^39 is 0 x inf = NaN, which the overflow guard
	// would turn into the cap; only the zero-base early return keeps it 0
	TestEqual(TEXT("a zero base with a huge multiplier is 0, not the cap"), Odd.GetCeiling(40), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBackoffJitterTest, "VaRest.Http.Backoff.Jitter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBackoffJitterTest::RunTest(const FString& Parameters)
{
	FVaRestBackoff Full;
	FRandomStream Random(1234);
	double Min = 1e9;
	double Max = -1.0;
	double Sum = 0.0;
	constexpr int32 Draws = 4000;
	for (int32 Index = 0; Index < Draws; ++Index)
	{
		const double Delay = Full.GetDelay(3, Random);
		Min = FMath::Min(Min, Delay);
		Max = FMath::Max(Max, Delay);
		Sum += Delay;
	}
	TestTrue(TEXT("full jitter: within [0, d)"), Min >= 0.0 && Max < 2.0);
	TestTrue(TEXT("... spread over it (mean near d/2)"), FMath::Abs(Sum / Draws - 1.0) < 0.1);

	FVaRestBackoff Ratio;
	Ratio.Jitter = EVaRestJitter::Ratio;
	Ratio.JitterRatio = 0.5f;
	Min = 1e9;
	Max = -1.0;
	for (int32 Index = 0; Index < Draws; ++Index)
	{
		const double Delay = Ratio.GetDelay(3, Random);
		Min = FMath::Min(Min, Delay);
		Max = FMath::Max(Max, Delay);
	}
	TestTrue(TEXT("ratio jitter: within d * (1 +/- r)"), Min >= 1.0 && Max <= 3.0 && Max > 2.0 && Min < 2.0);
	// Carry 1: one draw can land under the cap by luck; many make removing the clamp fail deterministically (d = 30 here,
	// so the unclamped upper half of d * (1 +/- 0.5) would pass 30 in about half the draws)
	bool bUnderCap = true;
	for (int32 Index = 0; Index < 1000; ++Index)
	{
		bUnderCap &= Ratio.GetDelay(20, Random) <= 30.0;
	}
	TestTrue(TEXT("ratio jitter never passes the cap (1,000 draws)"), bUnderCap);

	FVaRestBackoff Exact;
	Exact.Jitter = EVaRestJitter::None;
	TestEqual(TEXT("no jitter: exactly d"), Exact.GetDelay(3, Random), 2.0);

	// Plan decision 26: the caller's stream decides, so a seeded test knows the delay
	FRandomStream A(42);
	FRandomStream B(42);
	bool bSame = true;
	for (int32 Index = 0; Index < 16; ++Index)
	{
		bSame &= Full.GetDelay(Index + 1, A) == Full.GetDelay(Index + 1, B);
	}
	TestTrue(TEXT("one seed, one sequence"), bSame);
	FRandomStream C(42);
	const int32 Before = C.GetCurrentSeed();
	Full.GetDelay(0, C);
	TestEqual(TEXT("step 0 draws nothing"), C.GetCurrentSeed(), Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBackoffRetryAfterTest, "VaRest.Http.Backoff.RetryAfter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBackoffRetryAfterTest::RunTest(const FString& Parameters)
{
	using VaRest::Http::ParseRetryAfter;
	const FDateTime Now(2026, 10, 21, 12, 0, 0);
	auto Seconds = [&Now](const TCHAR* Value) { return ParseRetryAfter(Value, Now); };
	TestTrue(TEXT("delay-seconds"), Seconds(TEXT("120")) == TOptional<double>(120.0));
	TestTrue(TEXT("... trimmed"), Seconds(TEXT("  7 ")) == TOptional<double>(7.0));
	TestTrue(TEXT("... zero"), Seconds(TEXT("0")) == TOptional<double>(0.0));
	TestTrue(TEXT("... huge, bounded (no overflow)"), Seconds(TEXT("99999999999999999999999")) == TOptional<double>(1e9));
	TestFalse(TEXT("a negative number is not delay-seconds"), Seconds(TEXT("-1")).IsSet());
	TestFalse(TEXT("nor a fraction"), Seconds(TEXT("1.5")).IsSet());
	TestFalse(TEXT("nor garbage"), Seconds(TEXT("soon")).IsSet());
	TestFalse(TEXT("nor nothing"), Seconds(TEXT("")).IsSet());
	TestTrue(TEXT("an HTTP-date 30 s ahead"), Seconds(TEXT("Wed, 21 Oct 2026 12:00:30 GMT")) == TOptional<double>(30.0));
	TestTrue(TEXT("an HTTP-date in the past is 0"), Seconds(TEXT("Wed, 21 Oct 2026 11:00:00 GMT")) == TOptional<double>(0.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBackoffHttpDateQuirksTest, "VaRest.Http.Backoff.HttpDateQuirks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBackoffHttpDateQuirksTest::RunTest(const FString& Parameters)
{
	using VaRest::Http::ParseRetryAfter;
	const FDateTime Now(2026, 10, 21, 12, 0, 0);
	// Misc/DateTime.cpp:611-612: an RFC 850 date's two-digit year gets "Year += 1900" ("Horrible assumption"), so 26 is
	// 1926: parsed, in the past, so "retry now"
	const TOptional<double> Rfc850 = ParseRetryAfter(TEXT("Wednesday, 21-Oct-26 12:00:30 GMT"), Now);
	TestTrue(TEXT("RFC 850, two-digit year: parsed into the 1900s, so 0"), Rfc850 == TOptional<double>(0.0));
	// Misc/DateTime.cpp:663 and :676 drop ParseTime's result, so a time part that does not parse (here two fields, not
	// three; ParseTime returns false before it writes anything, :430-440) leaves Hour/Minute/Second at 0: midnight. A
	// strict parser would reject this; the engine's reads 22 Oct 00:00, 12 hours ahead
	const TOptional<double> NoSeconds = ParseRetryAfter(TEXT("Thu, 22 Oct 2026 12:00 GMT"), Now);
	TestTrue(TEXT("a malformed time part is ignored: midnight, 12 h ahead"), NoSeconds == TOptional<double>(43200.0));
	return true;
}

#endif
