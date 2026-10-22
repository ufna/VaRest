// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestRedactor.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tasks/Pipe.h"
#include "VaRestJsonUtf8.h"
#include "VaRestRedactionPipe.h"
#include "VaRestSha256.h"
#include "VaRestUrl.h"

namespace VaRest::Http::RedactorPrivate
{
	/** The module's gate (plan decision 16) */
	FRedactionGate Gate;
	static_assert(std::is_trivially_destructible_v<FRedactionGate>, "a static gate must not run a destructor at exit (review I-4)");

	TArray<uint8> ToUtf8(FStringView Text)
	{
		VaRest::Json::FUtf8Scratch Scratch;
		const FUtf8StringView Utf8 = VaRest::Json::ConvertForDocument(Text, Scratch, /*bWarn*/ false);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.GetData()), Utf8.Len());
	}

	FString ToText(TConstArrayView<uint8> Bytes)
	{
		VaRest::Json::FUtf8Scratch Repaired;
		VaRest::Json::AppendRepairedUtf8(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num()), Repaired);
		return FString::ConstructFromPtrSize(Repaired.GetData(), Repaired.Num());
	}

	/** The media type: before ';', trimmed */
	FStringView MediaType(FStringView ContentType)
	{
		int32 Semicolon = INDEX_NONE;
		return (ContentType.FindChar(TEXT(';'), Semicolon) ? ContentType.Left(Semicolon) : ContentType).TrimStartAndEnd();
	}

	uint8 FirstNonSpace(TConstArrayView<uint8> Bytes)
	{
		// Review M5: a UTF-8 BOM before the JSON is skipped, as the parser does
		if (Bytes.Num() >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF)
		{
			Bytes = Bytes.RightChop(3);
		}
		for (const uint8 Byte : Bytes)
		{
			if (Byte != ' ' && Byte != '\t' && Byte != '\r' && Byte != '\n')
			{
				return Byte;
			}
		}
		return 0;
	}

	bool IsTextual(FStringView Type)
	{
		return Type.StartsWith(TEXTVIEW("text/"), ESearchCase::IgnoreCase) || Type.Equals(TEXTVIEW("application/xml"), ESearchCase::IgnoreCase)
			|| Type.EndsWith(TEXTVIEW("+xml"), ESearchCase::IgnoreCase) || Type.Equals(TEXTVIEW("application/javascript"), ESearchCase::IgnoreCase);
	}

	/** Bytes up to the last whole UTF-8 character (a cut excerpt never ends in half a character) */
	int32 WholeCharacters(TConstArrayView<uint8> Bytes)
	{
		int32 End = Bytes.Num();
		int32 Back = 0;
		while (End - Back > 0 && Back < 3 && (Bytes[End - Back - 1] & 0xC0) == 0x80)
		{
			++Back;
		}
		if (End - Back <= 0)
		{
			return End;
		}
		const uint8 Lead = Bytes[End - Back - 1];
		const int32 Need = Lead >= 0xF0 ? 3 : Lead >= 0xE0 ? 2 : Lead >= 0xC0 ? 1 : 0;
		return Need > Back ? End - Back - 1 : End;
	}

	/** %XX with the hex digits lower-case: RFC 3986 2.1 makes %2b and %2B the same octet, and a server may echo either */
	FString LowerHexEscapes(const FString& Escaped)
	{
		FString Out = Escaped;
		for (int32 i = 0; i + 2 < Out.Len(); ++i)
		{
			if (Out[i] == TEXT('%'))
			{
				Out[i + 1] = FChar::ToLower(Out[i + 1]);
				Out[i + 2] = FChar::ToLower(Out[i + 2]);
				i += 2;
			}
		}
		return Out;
	}

	TConstArrayView<TCHAR> NeedleView(const FString& Needle)
	{
		return TConstArrayView<TCHAR>(*Needle, Needle.Len());
	}

	TConstArrayView<uint8> NeedleView(const TArray<uint8>& Needle)
	{
		return TConstArrayView<uint8>(Needle);
	}

	/**
	 * Replaces every occurrence of every needle by "***". The match ranges of all needles are collected first and merged,
	 * then the text is written once (review M5): replacing needle after needle would leave the tail of a second secret
	 * that overlaps the first (AAAABBBB then BBBBCCCC over AAAABBBBCCCC leaves CCCC), and would rebuild the buffer
	 * for needles that never hit. Out is untouched when nothing matched.
	 */
	template <typename ElementType, typename NeedleArrayType>
	bool ScrubRanges(TArray<ElementType>& Text, const NeedleArrayType& Needles)
	{
		TArray<TPair<int32, int32>> Ranges;
		for (const auto& Needle : Needles)
		{
			const TConstArrayView<ElementType> View = NeedleView(Needle);
			if (View.Num() == 0 || View.Num() > Text.Num())
			{
				continue;
			}
			for (int32 i = 0; i + View.Num() <= Text.Num(); ++i)
			{
				if (Text[i] == View[0] && FMemory::Memcmp(Text.GetData() + i, View.GetData(), View.Num() * sizeof(ElementType)) == 0)
				{
					Ranges.Emplace(i, i + View.Num());
				}
			}
		}
		if (Ranges.Num() == 0)
		{
			return false;
		}
		Ranges.Sort([](const TPair<int32, int32>& A, const TPair<int32, int32>& B) { return A.Key < B.Key; });
		TArray<ElementType> Scrubbed;
		Scrubbed.Reserve(Text.Num());
		int32 Written = 0;
		int32 RangeEnd = 0;
		for (int32 Index = 0; Index < Ranges.Num();)
		{
			const int32 RangeStart = Ranges[Index].Key;
			RangeEnd = Ranges[Index].Value;
			++Index;
			while (Index < Ranges.Num() && Ranges[Index].Key < RangeEnd)
			{
				RangeEnd = FMath::Max(RangeEnd, Ranges[Index].Value);
				++Index;
			}
			Scrubbed.Append(Text.GetData() + Written, RangeStart - Written);
			Scrubbed.Add(static_cast<ElementType>('*'));
			Scrubbed.Add(static_cast<ElementType>('*'));
			Scrubbed.Add(static_cast<ElementType>('*'));
			Written = RangeEnd;
		}
		Scrubbed.Append(Text.GetData() + Written, Text.Num() - Written);
		Text = MoveTemp(Scrubbed);
		return true;
	}

	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Share(TArray<uint8>&& Bytes)
	{
		return MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(MoveTemp(Bytes));
	}
}

bool VaRest::Http::FRedactionGate::Launch(const TCHAR* DebugName, TUniqueFunction<void()>&& Body)
{
	// Read first without counting, so a stop under steady launching drains at once instead of waiting for a gap
	if (bStopping.load())
	{
		return false;
	}
#if WITH_DEV_AUTOMATION_TESTS
	Reach(EHookPoint::AfterUncountedRead);
#endif
	Launching.fetch_add(1);
	ON_SCOPE_EXIT { Launching.fetch_sub(1); };
	// The read that carries the safety (see the atomics): a stop that began since the first read is seen here
	UE::Tasks::FPipe* Current = bStopping.load() ? nullptr : Pipe.load();
	if (!Current)
	{
		return false;
	}
#if WITH_DEV_AUTOMATION_TESTS
	Reach(EHookPoint::HoldingPipe);
#endif
	// FPipe::Launch takes any thread and reaches the pipe by atomics only (Pipe.h:62-80). The body is scheduled
	// (TaskPrivate.h:584-630), except with no task workers (-nothreading), where the scheduler runs it inline on this
	// thread (Scheduler.cpp:533, :622-632): then a game-thread launch redacts on the game thread, still inside the counted
	// window, and no body calls Stop
	Current->Launch(DebugName, MoveTemp(Body));
	return true;
}

void VaRest::Http::FRedactionGate::Start()
{
	check(IsInGameThread());
	check(!Pipe.load());
	Pipe.store(new UE::Tasks::FPipe(TEXT("VaRest.Redaction")));
	bStopping.store(false);
}

void VaRest::Http::FRedactionGate::Stop()
{
	check(IsInGameThread());
	bStopping.store(true);
	while (Launching.load() != 0)
	{
#if WITH_DEV_AUTOMATION_TESTS
		Reach(EHookPoint::StopWaitingForCount);
#endif
		// An OS yield, not a CPU pause (Yield is _mm_pause or isb, GenericPlatformProcess.h:964-978; YieldThread gives up
		// the time slice, :754-755): a launcher preempted inside the counted window may share this core
		FPlatformProcess::YieldThread();
	}
#if WITH_DEV_AUTOMATION_TESTS
	Reach(EHookPoint::StopAfterDrain);
#endif
	if (UE::Tasks::FPipe* Existing = Pipe.exchange(nullptr))
	{
		Existing->WaitUntilEmpty();
		delete Existing;
	}
}

bool VaRest::Http::FRedactionGate::Wait()
{
	check(IsInGameThread()); // the thread Stop deletes it on: the pipe read here outlives the wait
	UE::Tasks::FPipe* Current = Pipe.load();
	if (!Current)
	{
		return false;
	}
	Current->WaitUntilEmpty();
	return true;
}

bool VaRest::Http::LaunchRedaction(const TCHAR* DebugName, TUniqueFunction<void()>&& Body)
{
	return RedactorPrivate::Gate.Launch(DebugName, MoveTemp(Body));
}

void VaRest::Http::StartRedactionPipe()
{
	RedactorPrivate::Gate.Start();
}

void VaRest::Http::StopRedactionPipe()
{
	// ShutdownModule does not run in an `Automation RunTests ...; Quit` session (CLAUDE.md); a pipe left alive then is
	// leaked on purpose, and its destructor's idle check never runs
	RedactorPrivate::Gate.Stop();
}

bool VaRest::Http::WaitForRedactionPipe()
{
	return RedactorPrivate::Gate.Wait();
}

void FVaRestSecretSet::Set(FName Slot, const FString& Value, double Now)
{
	if (const FString* Old = Current.Find(Slot))
	{
		if (Old->Equals(Value, ESearchCase::CaseSensitive))
		{
			return;
		}
		Previous.Add({ *Old, Now + PreviousLifetime, Slot });
		while (Previous.Num() > MaxPrevious)
		{
			Previous.RemoveAt(0);
		}
	}
	if (Value.IsEmpty())
	{
		Current.Remove(Slot);
	}
	else
	{
		Current.Add(Slot, Value);
	}
}

void FVaRestSecretSet::ClearAll()
{
	Current.Reset();
	Previous.Reset();
}

void FVaRestSecretSet::ClearSlots(TFunctionRef<bool(FName Slot)> Matches)
{
	for (auto It = Current.CreateIterator(); It; ++It)
	{
		if (Matches(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
	Previous.RemoveAll([&Matches](const FPrevious& Old) { return Matches(Old.Slot); });
}

TArray<FString> FVaRestSecretSet::GetActive(double Now) const
{
	TArray<FString> Out;
	auto Consider = [&Out](const FString& Value)
	{
		if (VaRest::Json::Utf8Length(Value) >= MinBytes)
		{
			// FString's operator== ignores case: two tokens that differ only in case are two secrets
			if (!Out.ContainsByPredicate([&Value](const FString& Known) { return Known.Equals(Value, ESearchCase::CaseSensitive); }))
			{
				Out.Add(Value);
			}
		}
	};
	for (const TPair<FName, FString>& Pair : Current)
	{
		Consider(Pair.Value);
	}
	for (const FPrevious& Old : Previous)
	{
		if (Old.ExpiresAt > Now)
		{
			Consider(Old.Value);
		}
	}
	Out.Sort([](const FString& A, const FString& B) { return A.Len() > B.Len(); });
	return Out;
}

double FVaRestSecretSet::NextExpiry(double Now) const
{
	double Next = -1.0;
	for (const FPrevious& Old : Previous)
	{
		if (Old.ExpiresAt > Now + ExpirySlack)
		{
			Next = Next < 0.0 ? Old.ExpiresAt : FMath::Min(Next, Old.ExpiresAt);
		}
	}
	return Next;
}

void FVaRestSecretSet::Prune(double Now)
{
	Previous.RemoveAll([Now](const FPrevious& Old) { return Old.ExpiresAt <= Now + ExpirySlack; });
}

void FVaRestSecretSet::Rebase(double Delta)
{
	for (FPrevious& Old : Previous)
	{
		Old.ExpiresAt += Delta;
	}
}

FVaRestRedactor::FVaRestRedactor(FVaRestRedactRules InRules, TArray<FString> InSecrets)
	: Rules(MoveTemp(InRules))
	, InputSecrets(InSecrets)
{
	using namespace VaRest::Http::RedactorPrivate;
	// FString's operator== ignores case, so AddUnique would drop the lower-case hex forms as duplicates of the upper-case ones
	auto AddNeedle = [this](const FString& Needle)
	{
		if (!Secrets.ContainsByPredicate([&Needle](const FString& Known) { return Known.Equals(Needle, ESearchCase::CaseSensitive); }))
		{
			Secrets.Add(Needle);
		}
	};
	for (const FString& Secret : InSecrets)
	{
		if (VaRest::Json::Utf8Length(Secret) >= FVaRestSecretSet::MinBytes)
		{
			// Review M6: the forms a secret takes on the wire are needles too, when they differ: percent-encoded by
			// Path()/Query() (base64's + / =), form-encoded by FVaRestForm (space as '+', '~' as %7E, '*' kept: FormEncode
			// and PercentEncode differ on those three, VaRestUrl.cpp), either with lower-case hex (RFC 3986 2.1), and
			// JSON-escaped in a body (quote and backslash)
			const FString Percent = VaRest::Http::PercentEncode(Secret);
			const FString Form = VaRest::Http::FormEncode(Secret);
			AddNeedle(Secret);
			AddNeedle(Percent);
			AddNeedle(Form);
			AddNeedle(LowerHexEscapes(Percent));
			AddNeedle(LowerHexEscapes(Form));
			AddNeedle(Secret.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\"")));
		}
	}
	Secrets.Sort([](const FString& A, const FString& B) { return A.Len() > B.Len(); });
	for (const FString& Needle : Secrets)
	{
		SecretsUtf8.Add(ToUtf8(Needle));
		MaxNeedleBytes = FMath::Max(MaxNeedleBytes, SecretsUtf8.Last().Num());
	}
}

bool FVaRestRedactor::ScrubInPlace(FString& Text) const
{
	if (Secrets.Num() == 0)
	{
		return false;
	}
	TArray<TCHAR> Chars(Text.GetCharArray());
	if (!VaRest::Http::RedactorPrivate::ScrubRanges(Chars, Secrets))
	{
		return false;
	}
	Text = FString(Chars.GetData());
	return true;
}

bool FVaRestRedactor::ScrubUtf8(TArray<uint8>& Utf8) const
{
	// "a memmem of a few needles over at most the cap" (Inspector 2.2 layer 3)
	return VaRest::Http::RedactorPrivate::ScrubRanges(Utf8, SecretsUtf8);
}

bool FVaRestRedactor::TrimNeedleTail(TArray<uint8>& Utf8) const
{
	// Review I1: a cap can cut a known secret in two, and the half before the cut matches no needle. The caller copied
	// exactly the cap, so nothing past it can be scrubbed; instead drop the longest tail that begins a needle.
	int32 Drop = 0;
	for (const TArray<uint8>& Needle : SecretsUtf8)
	{
		for (int32 Length = FMath::Min(Needle.Num() - 1, Utf8.Num()); Length > Drop; --Length)
		{
			if (FMemory::Memcmp(Utf8.GetData() + Utf8.Num() - Length, Needle.GetData(), Length) == 0)
			{
				Drop = Length;
				break;
			}
		}
	}
	Utf8.SetNum(Utf8.Num() - Drop);
	return Drop > 0;
}

FString FVaRestRedactor::ScrubText(FStringView Text) const
{
	++FVaRestTrace::Counters().RedactorCalls;
	FString Out(Text);
	ScrubInPlace(Out);
	return Out;
}

int32 FVaRestRedactor::SecretValueStart(FStringView Piece, EVaRestRedactKeyKind Kind) const
{
	// One rule for query, fragment, form bodies and path parameters: a key can start at the front of the piece or after any
	// '?' (a query inside a query value or a hash-router fragment), ';' (a pair separator for some servers, a path
	// parameter's start) or '#' (a second fragment). The first candidate that is a secret key wins, and its value runs to
	// the end of the piece: these characters only ever START a key, they never end a value, so "password=ab;cd" cannot
	// leak "cd". Over-redaction is accepted; leaking a tail is not.
	//
	// Linear (review round 3): every candidate whose key reaches the same '=' is one group, and each '=' is found once.
	// A group is judged by its longest candidate and its shortest. Every candidate between them holds a separator, so no
	// exact key matches it (FVaRestRedactRules::AddKey refuses separators) and the idempotencykey/tokentype allowance does
	// not apply; the substring rules that could match it match the longest, which holds the same text. A candidate after the
	// front is a query key whatever the piece is (a "?code=" inside a form value is an OAuth code).
	auto Judge = [this, &Piece](int32 KeyStart, int32 KeyEnd, EVaRestRedactKeyKind KeyKind)
	{
		return Rules.ShouldRedact(FStringView(VaRest::Http::PercentDecode(Piece.Mid(KeyStart, KeyEnd - KeyStart), true)), KeyKind);
	};
	int32 Cursor = 0;
	while (Cursor <= Piece.Len())
	{
		int32 Equals = INDEX_NONE;
		if (!Piece.RightChop(Cursor).FindChar(TEXT('='), Equals))
		{
			return INDEX_NONE;
		}
		Equals += Cursor;
		int32 Longest = INDEX_NONE;
		int32 Shortest = INDEX_NONE;
		for (int32 At = FMath::Max(Cursor, 1); At < Equals; ++At)
		{
			const TCHAR Before = Piece[At - 1];
			if (Before == TEXT('?') || Before == TEXT(';') || Before == TEXT('#'))
			{
				Longest = Longest == INDEX_NONE ? At : Longest;
				Shortest = At;
			}
		}
		if ((Cursor == 0 && Equals > 0 && Judge(0, Equals, Kind))
			|| (Longest != INDEX_NONE && (Judge(Longest, Equals, EVaRestRedactKeyKind::Query) || (Shortest != Longest && Judge(Shortest, Equals, EVaRestRedactKeyKind::Query)))))
		{
			return Equals;
		}
		Cursor = Equals + 1;
	}
	return INDEX_NONE;
}

void FVaRestRedactor::AppendRedactedPairs(FStringView Pairs, EVaRestRedactKeyKind Kind, FString& Out) const
{
	// Only '&' separates pairs, so a secret value always ends at the next '&' or at the end
	int32 Start = 0;
	while (Start <= Pairs.Len())
	{
		int32 End = Start;
		while (End < Pairs.Len() && Pairs[End] != TEXT('&'))
		{
			++End;
		}
		const FStringView Piece = Pairs.Mid(Start, End - Start);
		int32 Equals = SecretValueStart(Piece, Kind);
		int32 FirstEquals = INDEX_NONE;
		int32 Percent = INDEX_NONE;
		if (Equals == INDEX_NONE && Piece.FindChar(TEXT('='), FirstEquals) && Piece.RightChop(FirstEquals + 1).FindChar(TEXT('%'), Percent))
		{
			// An encoded nested query ("next=%2Fx%3Fcode%3DX"): to the server it is the value "/x?code=X". VaRest's own
			// Query() encodes everything but the unreserved set, so a URL built through it always looks like this. The value
			// is decoded once; the text after its first '?' is a query of its own, judged with query keys, and a hit hides the
			// value.
			const FString Decoded = VaRest::Http::PercentDecode(Piece.RightChop(FirstEquals + 1), true);
			int32 Question = INDEX_NONE;
			if (Decoded.FindChar(TEXT('?'), Question))
			{
				// Review round 4: Query() encodes '&' as %26 too, so the decoded tail has pairs of its own, and the server
				// splits them as the outer loop splits the raw query. SecretValueStart starts no candidate after '&', so the
				// tail is cut at every '&' and each pair scanned: "state=abc&code=X" otherwise hid the code behind state.
				// One walk over the tail; the pairs do not overlap and each scan is linear in its pair, so this stays linear.
				const FStringView Tail = FStringView(Decoded).RightChop(Question + 1);
				int32 TailStart = 0;
				while (Equals == INDEX_NONE && TailStart <= Tail.Len())
				{
					int32 TailEnd = TailStart;
					while (TailEnd < Tail.Len() && Tail[TailEnd] != TEXT('&'))
					{
						++TailEnd;
					}
					if (SecretValueStart(Tail.Mid(TailStart, TailEnd - TailStart), EVaRestRedactKeyKind::Query) != INDEX_NONE)
					{
						Equals = FirstEquals;
					}
					TailStart = TailEnd + 1;
				}
			}
		}
		if (Equals != INDEX_NONE)
		{
			Out += Piece.Left(Equals);
			Out += TEXT("=***");
		}
		else
		{
			Out += Piece;
		}
		if (End < Pairs.Len())
		{
			Out.AppendChar(TEXT('&'));
		}
		Start = End + 1;
	}
}

void FVaRestRedactor::AppendRedactedPath(FStringView Path, FString& Out) const
{
	// Path parameters (";jsessionid=X"): a parameter run goes to the next '/', and a secret key's value to the end of that run
	int32 Start = 0;
	while (Start < Path.Len())
	{
		int32 End = Start;
		if (Path[Start] != TEXT(';'))
		{
			while (End < Path.Len() && Path[End] != TEXT(';'))
			{
				++End;
			}
			Out += Path.Mid(Start, End - Start);
			Start = End;
			continue;
		}
		++End;
		while (End < Path.Len() && Path[End] != TEXT('/'))
		{
			++End;
		}
		const FStringView Run = Path.Mid(Start + 1, End - Start - 1);
		const int32 Equals = SecretValueStart(Run, EVaRestRedactKeyKind::Query);
		Out.AppendChar(TEXT(';'));
		if (Equals != INDEX_NONE)
		{
			Out += Run.Left(Equals);
			Out += TEXT("=***");
		}
		else
		{
			Out += Run;
		}
		Start = End;
	}
}

FString FVaRestRedactor::RedactUrlUncounted(FStringView Whole) const
{
	FString Out;
	Out.Reserve(Whole.Len());
	// Review M3: leading controls and spaces are skipped, as the WHATWG URL parser does, so " https://u:p@h" is still a URL
	int32 Lead = 0;
	while (Lead < Whole.Len() && Whole[Lead] <= TEXT(' '))
	{
		++Lead;
	}
	Out += Whole.Left(Lead);
	const FStringView Url = Whole.RightChop(Lead);
	int32 Pos = 0;
	// Review C2: "://" ends a scheme only when everything before it is one (RFC 3986: ALPHA *( ALPHA / DIGIT / "+" /
	// "-" / "." )). Otherwise "/cb?code=SECRET&redirect_uri=https://app/done" would take its whole query for a scheme
	// and copy it verbatim. A leading "//" (protocol-relative) starts an authority too.
	int32 SchemeEnd = INDEX_NONE;
	for (int32 i = 0; i < Url.Len(); ++i)
	{
		const TCHAR C = Url[i];
		if (C == TEXT(':'))
		{
			SchemeEnd = (i > 0 && Url.Mid(i).StartsWith(TEXTVIEW("://"))) ? i : INDEX_NONE;
			break;
		}
		if (!(FChar::IsAlpha(C) || (i > 0 && (FChar::IsDigit(C) || C == TEXT('+') || C == TEXT('-') || C == TEXT('.')))))
		{
			break;
		}
	}
	const int32 AuthorityStart = SchemeEnd != INDEX_NONE ? SchemeEnd + 3 : (Url.StartsWith(TEXTVIEW("//")) ? 2 : INDEX_NONE);
	if (AuthorityStart != INDEX_NONE)
	{
		int32 AuthorityEnd = Url.Len();
		for (int32 i = AuthorityStart; i < Url.Len(); ++i)
		{
			if (Url[i] == TEXT('/') || Url[i] == TEXT('?') || Url[i] == TEXT('#'))
			{
				AuthorityEnd = i;
				break;
			}
		}
		Out += Url.Left(AuthorityStart);
		const FStringView Authority = Url.Mid(AuthorityStart, AuthorityEnd - AuthorityStart);
		int32 At = INDEX_NONE;
		if (Authority.FindLastChar(TEXT('@'), At))
		{
			// Layer 5: userinfo (user:pass@) is redacted whole
			Out += TEXT("***@");
			Out += Authority.RightChop(At + 1);
		}
		else
		{
			Out += Authority;
		}
		Pos = AuthorityEnd;
	}
	else
	{
		// Review M3: no scheme and no "//": a "user:pass@host/x" still carries userinfo, which ends at the last '@' before
		// the first '/', '?' or '#'
		int32 Boundary = Url.Len();
		for (int32 i = 0; i < Url.Len(); ++i)
		{
			if (Url[i] == TEXT('/') || Url[i] == TEXT('?') || Url[i] == TEXT('#'))
			{
				Boundary = i;
				break;
			}
		}
		int32 At = INDEX_NONE;
		if (Url.Left(Boundary).FindLastChar(TEXT('@'), At))
		{
			Out += TEXT("***@");
			Pos = At + 1;
		}
	}
	int32 QueryStart = INDEX_NONE;
	int32 FragmentStart = INDEX_NONE;
	for (int32 i = Pos; i < Url.Len(); ++i)
	{
		if (Url[i] == TEXT('#'))
		{
			FragmentStart = i;
			break;
		}
		if (Url[i] == TEXT('?') && QueryStart == INDEX_NONE)
		{
			QueryStart = i;
		}
	}
	const int32 PathEnd = QueryStart != INDEX_NONE ? QueryStart : FragmentStart != INDEX_NONE ? FragmentStart : Url.Len();
	AppendRedactedPath(Url.Mid(Pos, PathEnd - Pos), Out);
	if (QueryStart != INDEX_NONE)
	{
		const int32 QueryEnd = FragmentStart != INDEX_NONE ? FragmentStart : Url.Len();
		Out.AppendChar(TEXT('?'));
		AppendRedactedPairs(Url.Mid(QueryStart + 1, QueryEnd - QueryStart - 1), EVaRestRedactKeyKind::Query, Out);
	}
	if (FragmentStart != INDEX_NONE)
	{
		// OAuth's implicit flow returns #access_token=...: a fragment is redacted like a query
		Out.AppendChar(TEXT('#'));
		AppendRedactedPairs(Url.RightChop(FragmentStart + 1), EVaRestRedactKeyKind::Query, Out);
	}
	ScrubInPlace(Out);
	return Out;
}

FString FVaRestRedactor::RedactUrl(FStringView Url) const
{
	++FVaRestTrace::Counters().RedactorCalls;
	return RedactUrlUncounted(Url);
}

FString FVaRestRedactor::RedactHeaderValue(FStringView Name, FStringView Value) const
{
	++FVaRestTrace::Counters().RedactorCalls;
	if (Rules.ShouldRedact(Name, EVaRestRedactKeyKind::Header))
	{
		return TEXT("***");
	}
	if (Name.Equals(TEXTVIEW("Location"), ESearchCase::IgnoreCase) || Name.Equals(TEXTVIEW("Content-Location"), ESearchCase::IgnoreCase)
		|| Name.Equals(TEXTVIEW("Referer"), ESearchCase::IgnoreCase))
	{
		return RedactUrlUncounted(Value);
	}
	FString Out;
	if (Name.Equals(TEXTVIEW("Refresh"), ESearchCase::IgnoreCase))
	{
		// "5; url=https://app/cb?code=X": the part after url= is a URL
		const int32 UrlAt = FString(Value).Find(TEXT("url="), ESearchCase::IgnoreCase);
		if (UrlAt != INDEX_NONE)
		{
			Out += Value.Left(UrlAt + 4);
			Out += RedactUrlUncounted(Value.RightChop(UrlAt + 4));
			ScrubInPlace(Out);
			return Out;
		}
	}
	else if (Name.Equals(TEXTVIEW("Link"), ESearchCase::IgnoreCase))
	{
		// RFC 8288: "<target>; rel=next, <target>; ...": every target is a URL
		int32 Pos = 0;
		while (Pos < Value.Len())
		{
			int32 Open = INDEX_NONE;
			int32 Close = INDEX_NONE;
			if (!Value.RightChop(Pos).FindChar(TEXT('<'), Open) || !Value.RightChop(Pos + Open + 1).FindChar(TEXT('>'), Close))
			{
				break;
			}
			Out += Value.Mid(Pos, Open + 1);
			Out += RedactUrlUncounted(Value.Mid(Pos + Open + 1, Close));
			Pos += Open + 1 + Close;
		}
		Out += Value.RightChop(Pos);
		ScrubInPlace(Out);
		return Out;
	}
	Out = FString(Value);
	ScrubInPlace(Out);
	return Out;
}

FVaRestHeaders FVaRestRedactor::RedactHeaders(const FVaRestHeaders& In) const
{
	FVaRestHeaders Out;
	for (const FVaRestHeaders::FPair& Pair : In)
	{
		Out.Add(Pair.Key, RedactHeaderValue(Pair.Key, Pair.Value));
	}
	return Out;
}

FVaRestTraceExcerpt FVaRestRedactor::RedactBody(FStringView ContentType, TConstArrayView<uint8> Prefix, int64 TotalBytes, int32 Cap) const
{
	using namespace VaRest::Http::RedactorPrivate;
	++FVaRestTrace::Counters().RedactorCalls;
	FVaRestTraceExcerpt Excerpt;
	Excerpt.TotalBytes = TotalBytes;
	const int32 Seen = FMath::Min(Prefix.Num(), FMath::Max(Cap, 0));
	const TConstArrayView<uint8> Head = Prefix.Left(Seen);
	const bool bWhole = Seen == TotalBytes;
	const FStringView Type = MediaType(ContentType);
	const uint8 First = FirstNonSpace(Head);
	const bool bSniffJson = (Type.IsEmpty() || Type.StartsWith(TEXTVIEW("text/"), ESearchCase::IgnoreCase)) && (First == '{' || First == '[');
	TArray<uint8> Out;

	if (VaRest::Http::IsJsonContentType(Type) || bSniffJson)
	{
		Excerpt.Kind = EVaRestExcerptKind::Json;
		const FVaRestRedactResult Result = VaRest::Json::RedactPrefix(
			FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Head.GetData()), Head.Num()), Rules, EVaRestJsonFormat::Condensed, Out, bWhole);
		Excerpt.bRedacted = Result.bRedacted;
		Excerpt.bTruncated = !bWhole || Result.StopReason != EVaRestRedactStop::Complete;
		Excerpt.StopReason = Result.StopReason == EVaRestRedactStop::Complete ? FString()
			: Result.StopReason == EVaRestRedactStop::Cap ? FString(TEXT("cap"))
			: FString::Printf(TEXT("parse error at byte %lld: %s"), Result.ErrorOffset, *Result.Error);
	}
	else if (Type.Equals(TEXTVIEW("application/x-www-form-urlencoded"), ESearchCase::IgnoreCase))
	{
		Excerpt.Kind = EVaRestExcerptKind::Form;
		FString Text = ToText(Head);
		if (!bWhole)
		{
			// The last pair may be cut by the cap: a cut token is never written (Inspector 2.2)
			int32 LastAmp = INDEX_NONE;
			if (Text.FindLastChar(TEXT('&'), LastAmp))
			{
				Text.LeftInline(LastAmp);
			}
			else
			{
				Text.Reset();
			}
			Excerpt.bTruncated = true;
			Excerpt.StopReason = TEXT("cap");
		}
		FString Redacted;
		AppendRedactedPairs(Text, EVaRestRedactKeyKind::Form, Redacted);
		Excerpt.bRedacted = !Redacted.Equals(Text, ESearchCase::CaseSensitive);
		Out = ToUtf8(Redacted);
	}
	else if (Type.StartsWith(TEXTVIEW("multipart/"), ESearchCase::IgnoreCase))
	{
		Excerpt.Kind = EVaRestExcerptKind::Multipart;
		Out = ToUtf8(FString::Printf(TEXT("<multipart body, %lld bytes>"), TotalBytes));
	}
	else if (IsTextual(Type))
	{
		Excerpt.Kind = EVaRestExcerptKind::Text;
		const TConstArrayView<uint8> Whole = bWhole ? Head : Head.Left(WholeCharacters(Head));
		Out = ToUtf8(ToText(Whole));
		Excerpt.bTruncated = !bWhole;
		Excerpt.StopReason = bWhole ? FString() : FString(TEXT("cap"));
	}
	else
	{
		// Layer 4: binary is a descriptor. Protobuf and FlatBuffers store strings verbatim, so no byte is copied.
		Excerpt.Kind = EVaRestExcerptKind::Binary;
		if (Prefix.Num() == TotalBytes)
		{
			Excerpt.Sha256Prefix = VaRest::Http::FSha256::Hash(Prefix).ToHex().Left(8);
		}
		Out = ToUtf8(FString::Printf(TEXT("<binary: %lld bytes, %s%s%s>"), TotalBytes,
			Type.IsEmpty() ? TEXT("no content type") : *FString(Type),
			Excerpt.Sha256Prefix.IsEmpty() ? TEXT("") : TEXT(", sha256 "), *Excerpt.Sha256Prefix));
		// The Content-Type is caller text: it goes through the scrub too, like every other byte a sink receives
		Excerpt.bRedacted |= ScrubUtf8(Out);
		Excerpt.Bytes = Share(MoveTemp(Out));
		return Excerpt;
	}
	Excerpt.bRedacted |= ScrubUtf8(Out);
	if (!bWhole)
	{
		// Review I1: the cap may have cut a known secret in two
		Excerpt.bRedacted |= TrimNeedleTail(Out);
	}
	Excerpt.Bytes = Share(MoveTemp(Out));
	return Excerpt;
}

FVaRestTraceExcerpt FVaRestRedactor::DescribeMultipart(TConstArrayView<FVaRestMultipartPart> Parts, int64 TotalBytes, int32 Cap) const
{
	using namespace VaRest::Http::RedactorPrivate;
	++FVaRestTrace::Counters().RedactorCalls;
	FVaRestTraceExcerpt Excerpt;
	Excerpt.Kind = EVaRestExcerptKind::Multipart;
	Excerpt.TotalBytes = TotalBytes;
	const int32 CapBytes = FMath::Max(Cap, 0);
	// Review M6: nothing past the cap plus the longest needle can reach the excerpt, so a large text part is read no further
	const int32 Reach = CapBytes + MaxNeedleBytes;
	FString Text;
	for (const FVaRestMultipartPart& Part : Parts)
	{
		if (Text.Len() > Reach)
		{
			break;
		}
		if (!Text.IsEmpty())
		{
			Text.AppendChar(TEXT('\n'));
		}
		if (Part.bField)
		{
			const bool bSecret = Rules.ShouldRedact(FStringView(Part.Name), EVaRestRedactKeyKind::Part);
			Excerpt.bRedacted |= bSecret;
			FString Value(TEXT("***"));
			if (!bSecret)
			{
				TArray<uint8> Head(Part.Bytes.GetData(), FMath::Min(Part.Bytes.Num(), Reach));
				if (Part.Bytes.Num() > Head.Num() || Head.Num() < Part.Size)
				{
					// Task 14 fix round 1: the part is cut, here or by the caller (the log stage copies at most the cap plus the
					// longest needle). A known secret the cut split in two matches no needle, and the scrubs before it shorten
					// the text, so its front could land inside the cap: scrubbed first, then the tail that begins a needle is
					// dropped, as RedactBody does for a cut body (review I1).
					Excerpt.bRedacted |= ScrubUtf8(Head);
					Excerpt.bRedacted |= TrimNeedleTail(Head);
					Excerpt.bTruncated = true;
					Excerpt.StopReason = TEXT("cap");
				}
				Value = ToText(Head);
			}
			Text += FString::Printf(TEXT("part \"%s\": %s"), *Part.Name, *Value);
		}
		else
		{
			Text += FString::Printf(TEXT("part \"%s\" filename=\"%s\" %s %lld B"), *Part.Name, *Part.FileName, *Part.ContentType, Part.Size);
		}
	}
	Excerpt.bRedacted |= ScrubInPlace(Text);
	TArray<uint8> Out = ToUtf8(Text);
	if (Out.Num() > CapBytes)
	{
		Out.SetNum(WholeCharacters(TConstArrayView<uint8>(Out).Left(CapBytes)));
		Excerpt.bTruncated = true;
		Excerpt.StopReason = TEXT("cap");
	}
	Excerpt.Bytes = Share(MoveTemp(Out));
	return Excerpt;
}

FVaRestTraceExcerpt FVaRestRedactor::DescribeStream(FStringView FilePath, FStringView ContentType, int64 TotalBytes) const
{
	using namespace VaRest::Http::RedactorPrivate;
	++FVaRestTrace::Counters().RedactorCalls;
	FString Path = FPaths::ConvertRelativePathToFull(FString(FilePath));
	const FString Project = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	if (FPaths::IsUnderDirectory(Path, Project))
	{
		// Inspector 2.2: "path relative to the project when inside it" -- a user's home directory is not in the excerpt
		FPaths::MakePathRelativeTo(Path, *Project);
	}
	FString Text = FString::Printf(TEXT("<file upload: %s, %lld bytes, %s>"), *Path, TotalBytes, *FString(ContentType));
	FVaRestTraceExcerpt Excerpt;
	Excerpt.Kind = EVaRestExcerptKind::Stream;
	Excerpt.TotalBytes = TotalBytes;
	Excerpt.bRedacted = ScrubInPlace(Text);
	Excerpt.Bytes = Share(ToUtf8(Text));
	return Excerpt;
}
