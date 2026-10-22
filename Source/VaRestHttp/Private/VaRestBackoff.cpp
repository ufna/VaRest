// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestBackoff.h"

namespace VaRest::Http::BackoffPrivate
{
	/** A field as a usable number: NaN, an infinity or anything below Floor is Floor */
	double Sane(float Value, double Floor)
	{
		return FMath::IsFinite(Value) ? FMath::Max(static_cast<double>(Value), Floor) : Floor;
	}
}

double FVaRestBackoff::GetCeiling(int32 Step) const
{
	using namespace VaRest::Http::BackoffPrivate;
	const double Base = Sane(BaseDelay, 0.0);
	if (Step < 1 || Base <= 0.0)
	{
		return 0.0;
	}
	const double Cap = Sane(MaxDelay, 0.0);
	const double Factor = FMath::IsFinite(Multiplier) ? FMath::Max(static_cast<double>(Multiplier), 1.0) : 1.0;
	// The exponent is bounded so pow stays defined; any step that large is at the cap long before
	const double Grown = Base * FMath::Pow(Factor, static_cast<double>(FMath::Min(Step - 1, 1023)));
	return FMath::IsFinite(Grown) ? FMath::Min(Grown, Cap) : Cap;
}

double FVaRestBackoff::GetDelay(int32 Step, FRandomStream& Random) const
{
	using namespace VaRest::Http::BackoffPrivate;
	if (Step < 1)
	{
		return 0.0;
	}
	const double Ceiling = GetCeiling(Step);
	switch (Jitter)
	{
	case EVaRestJitter::None:
		return Ceiling;
	case EVaRestJitter::Ratio:
	{
		const double Spread = FMath::IsFinite(JitterRatio) ? FMath::Clamp(static_cast<double>(JitterRatio), 0.0, 1.0) : 0.0;
		const double Scaled = Ceiling * (1.0 + Spread * (2.0 * Random.GetFraction() - 1.0));
		return FMath::Clamp(Scaled, 0.0, Sane(MaxDelay, 0.0));
	}
	default:
		// GetFraction is [0, 1) (RandomStream.h:110-122)
		return Ceiling * Random.GetFraction();
	}
}

TOptional<double> VaRest::Http::ParseRetryAfter(FStringView Value, const FDateTime& NowUtc)
{
	const FStringView Trimmed = Value.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return TOptional<double>();
	}
	bool bDigits = true;
	for (const TCHAR Char : Trimmed)
	{
		bDigits &= FChar::IsDigit(Char);
	}
	if (bDigits)
	{
		double Seconds = 0.0;
		for (const TCHAR Char : Trimmed)
		{
			// 31 years: every cap has long taken over, and the sum cannot overflow
			Seconds = FMath::Min(Seconds * 10.0 + static_cast<double>(Char - TEXT('0')), 1e9);
		}
		return Seconds;
	}
	FDateTime When;
	if (FDateTime::ParseHttpDate(FString(Trimmed), When))
	{
		return FMath::Max(0.0, (When - NowUtc).GetTotalSeconds());
	}
	return TOptional<double>();
}
