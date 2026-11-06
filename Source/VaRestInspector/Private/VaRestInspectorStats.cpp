// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspectorStats.h"
#include "VaRestHttpTypes.h"
#include "VaRestInspectorStore.h"

#include "HAL/UnrealMemory.h"
#include "Math/UnrealMathUtility.h"
#include <cmath>
#include <limits>

namespace
{
// These rounded binary64 boundaries define the implemented intervals. A
// comparison search keeps exact boundaries out of logarithm rounding errors.
struct FStatsDurationBoundaries
{
	double Values[511] = {};
	constexpr FStatsDurationBoundaries()
	{
		Values[0] = 1e-6;
		for (int32 Index = 1; Index <= 510; ++Index)
			Values[Index] = Values[Index - 1] * 1.1;
	}
	constexpr bool IsValid() const
	{
		for (int32 Index = 1; Index <= 510; ++Index)
			if (!(Values[Index] > Values[Index - 1]) || Values[Index] > std::numeric_limits<double>::max())
				return false;
		return Values[0] > 0;
	}
};
constexpr FStatsDurationBoundaries StatsDurationBoundaries;
static_assert(StatsDurationBoundaries.IsValid(), "Duration intervals must be finite and strictly increasing");

void StatsAdd(uint64& Counter, uint64 Amount, bool& bSaturated)
{
	if (Amount > MAX_uint64 - Counter)
	{
		Counter = MAX_uint64;
		bSaturated = true;
	}
	else
		Counter += Amount;
}
// Ordered categories: zero, underflow, ordinary bins1..510, overflow.
int32 StatsDurationCategory(double Seconds)
{
	if (Seconds == 0)
		return 0;
	if (Seconds < StatsDurationBoundaries.Values[0])
		return 1;
	if (Seconds >= StatsDurationBoundaries.Values[510])
		return 512;
	int32 Low = 1, High = 510;
	while (Low < High)
	{
		const int32 Middle = Low + (High - Low) / 2;
		if (Seconds < StatsDurationBoundaries.Values[Middle])
			High = Middle;
		else
			Low = Middle + 1;
	}
	return Low + 1;
}
#if VAREST_WITH_INSPECTOR
bool StatsHex(TCHAR C)
{
	return (C >= TCHAR('0') && C <= TCHAR('9')) || (C >= TCHAR('a') && C <= TCHAR('f')) || (C >= TCHAR('A') && C <= TCHAR('F'));
}
bool StatsIpv6(FStringView Address)
{
	if (Address.IsEmpty()) return false;
	int32 Position = 0, Groups = 0;
	bool bCompressed = false;
	if (Address[0] == TCHAR(':'))
	{
		if (Address.Len() < 2 || Address[1] != TCHAR(':')) return false;
		Position = 2; bCompressed = true;
	}
	while (Position < Address.Len())
	{
		const int32 Begin = Position;
		bool bDotted = false;
		while (Position < Address.Len() && Address[Position] != TCHAR(':'))
		{
			bDotted |= Address[Position] == TCHAR('.');
			++Position;
		}
		if (Position == Begin) return false;
		if (bDotted)
		{
			if (Position != Address.Len()) return false;
			int32 At = Begin, Octets = 0;
			while (At < Position)
			{
				int32 Digits = 0, Value = 0;
				while (At < Position && Address[At] >= TCHAR('0') && Address[At] <= TCHAR('9'))
				{
					Value = Value * 10 + int32(Address[At++] - TCHAR('0'));
					if (++Digits > 3) return false;
				}
				if (Digits == 0 || Value > 255 || ++Octets > 4) return false;
				if (At == Position) break;
				if (Address[At++] != TCHAR('.') || At == Position) return false;
			}
			if (Octets != 4) return false;
			Groups += 2;
			break;
		}
		if (Position - Begin > 4) return false;
		for (int32 At = Begin; At < Position; ++At) if (!StatsHex(Address[At])) return false;
		if (++Groups > 8) return false;
		if (Position == Address.Len()) break;
		if (++Position == Address.Len()) return false;
		if (Address[Position] == TCHAR(':'))
		{
			if (bCompressed) return false;
			bCompressed = true;
			++Position;
		}
	}
	return bCompressed ? Groups < 8 : Groups == 8;
}
#endif
}

void FVaRestInspectorErrorCounts::Add(EVaRestErrorKind Kind)
{
	uint64* Counter = &Unknown;
	switch (Kind)
	{
	case EVaRestErrorKind::Canceled: Counter = &Canceled; break;
	case EVaRestErrorKind::Connection: Counter = &Connection; break;
	case EVaRestErrorKind::Timeout: Counter = &Timeout; break;
	case EVaRestErrorKind::Http: Counter = &Http; break;
	case EVaRestErrorKind::Server: Counter = &Server; break;
	case EVaRestErrorKind::Decode: Counter = &Decode; break;
	case EVaRestErrorKind::Auth: Counter = &Auth; break;
	case EVaRestErrorKind::Offline: Counter = &Offline; break;
	case EVaRestErrorKind::Config: Counter = &Config; break;
	case EVaRestErrorKind::None: break;
	default: break;
	}
	StatsAdd(*Counter, 1, bSaturated);
}

FVaRestInspectorQuantile FVaRestInspectorHttpStats::Quantile(uint32 Numerator, uint32 Denominator) const
{
	FVaRestInspectorQuantile Result;
	if (bDurationCountsSaturated)
	{
		Result.Kind = EVaRestInspectorQuantileKind::Saturated;
		return Result;
	}
	if (Denominator == 0 || Numerator > Denominator || DurationCount == 0 ||
		!FMath::IsFinite(DurationMin) || !FMath::IsFinite(DurationMax) || DurationMin < 0 || DurationMax < DurationMin ||
		Histogram[511] != DurationOverflow)
		return Result;
	uint64 Count = 0;
	bool bCountOverflow = false;
	StatsAdd(Count, Histogram[0], bCountOverflow);
	StatsAdd(Count, DurationUnderflow, bCountOverflow);
	for (int32 Bin = 1; Bin <= 511; ++Bin)
		StatsAdd(Count, Histogram[Bin], bCountOverflow);
	if (bCountOverflow || Count != DurationCount)
		return Result;
	int32 FirstCategory = INDEX_NONE, LastCategory = INDEX_NONE;
	for (int32 Category = 0; Category <= 512; ++Category)
	{
		const uint64 InCategory = Category == 0 ? Histogram[0] : Category == 1 ? DurationUnderflow : Histogram[Category - 1];
		if (InCategory)
		{
			if (FirstCategory == INDEX_NONE)
				FirstCategory = Category;
			LastCategory = Category;
		}
	}
	// Marginal consistency, including exact zero: extrema must occupy the
	// actual lowest/highest populated categories, not an unrelated empty bin.
	if (FirstCategory == INDEX_NONE || StatsDurationCategory(DurationMin) != FirstCategory ||
		StatsDurationCategory(DurationMax) != LastCategory)
		return Result;
	const uint64 Quotient = DurationCount / Denominator, Remainder = DurationCount % Denominator;
	const uint64 TailProduct = Remainder * uint64(Numerator);
	const uint64 Base = Quotient * uint64(Numerator);
	const uint64 Tail = TailProduct / Denominator + (TailProduct % Denominator != 0 ? 1 : 0);
	if (Tail > MAX_uint64 - Base)
		return Result;
	uint64 Rank = Base + Tail;
	if (Rank == 0)
		Rank = 1;
	if (Rank > DurationCount)
		Rank = DurationCount;
	int32 Selected = INDEX_NONE;
	for (int32 Category = 0; Category <= 512; ++Category)
	{
		const uint64 InCategory = Category == 0 ? Histogram[0] : Category == 1 ? DurationUnderflow : Histogram[Category - 1];
		if (Rank <= InCategory)
		{
			Selected = Category;
			break;
		}
		Rank -= InCategory;
	}
	if (Selected == INDEX_NONE)
		return Result;
	if (Selected == 0)
	{
		if (DurationMin != 0)
			return Result;
		Result.Kind = EVaRestInspectorQuantileKind::Zero;
		Result.bHasValue = Result.bLowerInclusive = Result.bUpperInclusive = true;
		return Result;
	}
	double Lower = 0, Upper = 0;
	if (Selected == 1)
	{
		Lower = 0;
		Upper = StatsDurationBoundaries.Values[0];
		Result.Kind = EVaRestInspectorQuantileKind::Underflow;
		Result.bLowerInclusive = false;
	}
	else if (Selected == 512)
	{
		Lower = StatsDurationBoundaries.Values[510];
		Upper = std::numeric_limits<double>::max();
		Result.Kind = EVaRestInspectorQuantileKind::Overflow;
		Result.bUpperInclusive = true;
	}
	else
	{
		Lower = StatsDurationBoundaries.Values[Selected - 2];
		Upper = StatsDurationBoundaries.Values[Selected - 1];
		Result.Kind = EVaRestInspectorQuantileKind::LogInterval;
		// H3's sole midpoint anchor; bins/rank/validation/clamping are separate.
		Result.Value = std::sqrt(Lower * Upper);
		if (Result.Value < DurationMin) Result.Value = DurationMin;
		if (Result.Value > DurationMax) Result.Value = DurationMax;
		Result.bHasValue = true;
	}
	Result.Lower = Lower;
	Result.Upper = Upper;
	if (DurationMin > Result.Lower)
	{
		Result.Lower = DurationMin;
		Result.bLowerInclusive = true;
	}
	if (DurationMax < Result.Upper)
	{
		Result.Upper = DurationMax;
		Result.bUpperInclusive = true;
	}
	if (Result.Lower > Result.Upper || (Result.Lower == Result.Upper && !(Result.bLowerInclusive && Result.bUpperInclusive)) ||
		(Result.bHasValue && (!FMath::IsFinite(Result.Value) || Result.Value < Result.Lower || Result.Value > Result.Upper ||
			(Result.Value == Result.Lower && !Result.bLowerInclusive) || (Result.Value == Result.Upper && !Result.bUpperInclusive))))
		return FVaRestInspectorQuantile{};
	return Result;
}

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
bool BuildStatsKey(const FStatsKeyParts& Parts, TArrayView<TCHAR> Scratch, FStatsKeyView& Out)
{
	Out = {};
	if (!Parts.bContextKnown)
		return false;
	Out.ScopeId = Parts.ScopeId;
	Out.ClientId = Parts.ClientId;
	Out.Endpoint = Parts.Endpoint;
	if (!Parts.Endpoint.IsNone())
		return true;
	const auto ValidText = [](FStringView Text) {
		return Text.Len() >= 0 && Text.Len() <= FNormalizedLimits::MaxStringChars && (Text.IsEmpty() || Text.GetData());
	};
	if (!ValidText(Parts.Verb) || Parts.Verb.IsEmpty() || !ValidText(Parts.PathTemplate) || !ValidText(Parts.Url))
		return false;
	FStringView Suffix = Parts.PathTemplate;
	bool bHost = Suffix.IsEmpty();
	if (bHost)
	{
		const FStringView Url = Parts.Url;
		const auto LowerAscii = [](TCHAR C) { return C >= TCHAR('A') && C <= TCHAR('Z') ? TCHAR(C + ('a' - 'A')) : C; };
		const bool bHttp = Url.Len() >= 7 && LowerAscii(Url[0]) == TCHAR('h') && LowerAscii(Url[1]) == TCHAR('t') &&
			LowerAscii(Url[2]) == TCHAR('t') && LowerAscii(Url[3]) == TCHAR('p');
		int32 Begin = 4;
		if (!bHttp)
			return false;
		if (Begin < Url.Len() && LowerAscii(Url[Begin]) == TCHAR('s')) ++Begin;
		if (Begin + 2 >= Url.Len() || Url[Begin] != TCHAR(':') || Url[Begin + 1] != TCHAR('/') || Url[Begin + 2] != TCHAR('/'))
			return false;
		Begin += 3;
		int32 End = Begin;
		while (End < Url.Len() && Url[End] != TCHAR('/') && Url[End] != TCHAR('?') && Url[End] != TCHAR('#'))
		{
			if (Url[End] <= TCHAR(' ') || Url[End] == TCHAR('\\')) return false;
			++End;
		}
		for (int32 Index = Begin; Index < End; ++Index)
			if (Url[Index] == TCHAR('@')) Begin = Index + 1;
		if (Begin >= End)
			return false;
		int32 HostEnd = End;
		if (Url[Begin] == TCHAR('['))
		{
			HostEnd = Begin + 1;
			while (HostEnd < End && Url[HostEnd] != TCHAR(']')) ++HostEnd;
			if (HostEnd >= End || HostEnd == Begin + 1) return false;
			if (!StatsIpv6(Url.Mid(Begin + 1, HostEnd - Begin - 1))) return false;
			++HostEnd;
			if (HostEnd < End && Url[HostEnd] != TCHAR(':')) return false;
		}
		else
		{
			for (int32 Index = Begin; Index < End; ++Index)
			{
				if (Url[Index] == TCHAR(':')) { HostEnd = Index; break; }
				const TCHAR C = Url[Index];
				if ((C >= TCHAR('a') && C <= TCHAR('z')) || (C >= TCHAR('A') && C <= TCHAR('Z')) ||
					(C >= TCHAR('0') && C <= TCHAR('9')) || C == TCHAR('-') || C == TCHAR('.') || C == TCHAR('_') || C == TCHAR('~') ||
					C == TCHAR('!') || C == TCHAR('$') || C == TCHAR('&') || C == TCHAR('\'') || C == TCHAR('(') || C == TCHAR(')') ||
					C == TCHAR('*') || C == TCHAR('+') || C == TCHAR(',') || C == TCHAR(';') || C == TCHAR('=')) continue;
				if (C != TCHAR('%') || Index + 2 >= End || !StatsHex(Url[Index + 1]) || !StatsHex(Url[Index + 2])) return false;
				Index += 2;
			}
		}
		if (HostEnd == Begin) return false;
		if (HostEnd < End)
		{
			if (HostEnd + 1 == End) return false;
			for (int32 Index = HostEnd + 1; Index < End; ++Index)
				if (Url[Index] < TCHAR('0') || Url[Index] > TCHAR('9')) return false;
		}
		Suffix = Url.Mid(Begin, HostEnd - Begin);
	}
	const int64 Chars = 2ll + Parts.Verb.Len() + 1 + Suffix.Len();
	if (Chars > FNormalizedLimits::MaxStringChars || Chars + 1 > Scratch.Num())
		return false;
	int32 Written = 0;
	Scratch[Written++] = bHost ? TCHAR('H') : TCHAR('T');
	Scratch[Written++] = TCHAR(':');
	for (TCHAR C : Parts.Verb) Scratch[Written++] = C;
	Scratch[Written++] = TCHAR(' ');
	for (TCHAR C : Suffix)
		Scratch[Written++] = bHost && C >= TCHAR('A') && C <= TCHAR('Z') ? TCHAR(C + ('a' - 'A')) : C;
	Scratch[Written] = TCHAR(0);
	Out.Key = FStringView(Scratch.GetData(), Written);
	return true;
}

void AccumulateStatsStarted(FStatsGroup& Group)
{
	StatsAdd(Group.Started, 1, Group.bSaturated);
}
void AccumulateStatsCompleted(FStatsGroup& Group, const FPacketScalars& Scalars, const FStatsDurationSample& Duration)
{
	StatsAdd(Group.Completed, 1, Group.bSaturated);
	switch (Scalars.Outcome)
	{
	case EVaRestTraceOutcome::Success: StatsAdd(Group.Success, 1, Group.bSaturated); break;
	case EVaRestTraceOutcome::Error:
		StatsAdd(Group.Error, 1, Group.bSaturated);
		Group.ErrorsByKind.Add(Scalars.ErrorKind);
		Group.bSaturated |= Group.ErrorsByKind.bSaturated;
		break;
	case EVaRestTraceOutcome::Queued: StatsAdd(Group.Queued, 1, Group.bSaturated); break;
	case EVaRestTraceOutcome::Canceled: StatsAdd(Group.Canceled, 1, Group.bSaturated); break;
	default: break;
	}
	if (Scalars.bFromCache)
	{
		StatsAdd(Group.CacheHit, 1, Group.bSaturated);
		if (Scalars.CacheLookup == EVaRestTraceCacheLookup::HitDisk)
			StatsAdd(Group.CacheDisk, 1, Group.bSaturated);
	}
	if (Duration.Kind == FStatsDurationSample::EKind::ExcludedQueued)
		return;
	if (Duration.Kind != FStatsDurationSample::EKind::Finite)
	{
		StatsAdd(Duration.Kind == FStatsDurationSample::EKind::NonFinite ? Group.DurationNonFinite : Group.DurationUnknown, 1, Group.bSaturated);
		return;
	}
	const double Seconds = Duration.Seconds;
	const bool bFirst = Group.DurationCount == 0;
	StatsAdd(Group.DurationCount, 1, Group.bDurationCountsSaturated);
	const int32 Category = StatsDurationCategory(Seconds);
	if (Category == 1)
		StatsAdd(Group.DurationUnderflow, 1, Group.bDurationCountsSaturated);
	else
	{
		const int32 Bin = Category == 0 ? 0 : Category - 1;
		StatsAdd(Group.Histogram[Bin], 1, Group.bDurationCountsSaturated);
		if (Bin == 511) StatsAdd(Group.DurationOverflow, 1, Group.bDurationCountsSaturated);
	}
	Group.bSaturated |= Group.bDurationCountsSaturated;
	if (bFirst)
		Group.DurationMin = Group.DurationMax = Seconds;
	else
	{
		if (Seconds < Group.DurationMin) Group.DurationMin = Seconds;
		if (Seconds > Group.DurationMax) Group.DurationMax = Seconds;
	}
	if (Seconds > std::numeric_limits<double>::max() - Group.DurationSum)
	{
		Group.DurationSum = std::numeric_limits<double>::max();
		Group.bSaturated = true;
	}
	else
		Group.DurationSum += Seconds;
}
void AccumulateStatsDispatched(FStatsGroup& Group, const FPacketScalars& Scalars, bool bNewPhysical, bool bNewClassification)
{
	if (bNewPhysical) StatsAdd(Group.Dispatches, 1, Group.bSaturated);
	if (!bNewClassification) return;
	switch (Scalars.GapKind)
	{
	case EVaRestTraceGap::SessionRefresh: StatsAdd(Group.AuthReplays, 1, Group.bSaturated); break;
	case EVaRestTraceGap::Backoff: StatsAdd(Group.Retries, 1, Group.bSaturated); break;
	case EVaRestTraceGap::None: break;
	default: StatsAdd(Group.UnknownDispatchClassification, 1, Group.bSaturated); break;
	}
}
void AccumulateStatsEnded(FStatsGroup& Group, const FPacketScalars& Scalars, bool bNewPhysical)
{
	if (bNewPhysical) StatsAdd(Group.Dispatches, 1, Group.bSaturated);
	if (Scalars.BytesUp < 0) Group.bWireBytesUnknown = true;
	else StatsAdd(Group.WireBytesUp, uint64(Scalars.BytesUp), Group.bSaturated);
	if (Scalars.BytesDown < 0) Group.bWireBytesUnknown = true;
	else StatsAdd(Group.WireBytesDown, uint64(Scalars.BytesDown), Group.bSaturated);
}
void CopyStatsScalars(const FStatsGroup& Source, FVaRestInspectorHttpStats& Out)
{
#define COPY_STATS_FIELD(Field) Out.Field = Source.Field
	COPY_STATS_FIELD(ScopeId); COPY_STATS_FIELD(ClientId); COPY_STATS_FIELD(Endpoint);
	COPY_STATS_FIELD(bOverflow); COPY_STATS_FIELD(bSaturated); COPY_STATS_FIELD(bWireBytesUnknown); COPY_STATS_FIELD(bDurationCountsSaturated);
	COPY_STATS_FIELD(Started); COPY_STATS_FIELD(Completed); COPY_STATS_FIELD(Success); COPY_STATS_FIELD(Error); COPY_STATS_FIELD(Queued); COPY_STATS_FIELD(Canceled);
	COPY_STATS_FIELD(Dispatches); COPY_STATS_FIELD(Retries); COPY_STATS_FIELD(AuthReplays); COPY_STATS_FIELD(RetryScheduled);
	COPY_STATS_FIELD(UnknownDispatchClassification); COPY_STATS_FIELD(CacheHit); COPY_STATS_FIELD(CacheDisk);
	COPY_STATS_FIELD(Revalidated); COPY_STATS_FIELD(Stale); COPY_STATS_FIELD(Replayed);
	COPY_STATS_FIELD(WireBytesUp); COPY_STATS_FIELD(WireBytesDown); COPY_STATS_FIELD(Partial); COPY_STATS_FIELD(Incomplete); COPY_STATS_FIELD(Dropped);
	COPY_STATS_FIELD(ErrorsByKind); COPY_STATS_FIELD(DurationCount); COPY_STATS_FIELD(DurationUnderflow); COPY_STATS_FIELD(DurationOverflow);
	COPY_STATS_FIELD(DurationUnknown); COPY_STATS_FIELD(DurationNonFinite); COPY_STATS_FIELD(DurationSum); COPY_STATS_FIELD(DurationMin); COPY_STATS_FIELD(DurationMax);
#undef COPY_STATS_FIELD
	for (int32 Bin = 0; Bin < 512; ++Bin)
		Out.Histogram[Bin] = Source.Histogram[Bin];
}
} // namespace VaRest::Inspector::Private
#endif
