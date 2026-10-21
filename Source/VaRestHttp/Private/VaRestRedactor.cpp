// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestRedactor.h"

#include "Misc/Paths.h"
#include "VaRestJsonUtf8.h"
#include "VaRestRedactionPipe.h"
#include "VaRestSha256.h"
#include "VaRestUrl.h"

namespace VaRest::Http::RedactorPrivate
{
	UE::Tasks::FPipe* Pipe = nullptr;

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

	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Share(TArray<uint8>&& Bytes)
	{
		return MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(MoveTemp(Bytes));
	}
}

UE::Tasks::FPipe* VaRest::Http::GetRedactionPipe()
{
	check(IsInGameThread()); // review M-10: only the game thread launches on it
	return RedactorPrivate::Pipe;
}

void VaRest::Http::StartRedactionPipe()
{
	check(!RedactorPrivate::Pipe);
	RedactorPrivate::Pipe = new UE::Tasks::FPipe(TEXT("VaRest.Redaction"));
}

void VaRest::Http::StopRedactionPipe()
{
	// ShutdownModule does not run in an `Automation RunTests ...; Quit` session (CLAUDE.md); a pipe left alive then is
	// leaked on purpose, and its destructor's idle check never runs
	if (UE::Tasks::FPipe* Existing = RedactorPrivate::Pipe)
	{
		RedactorPrivate::Pipe = nullptr;
		Existing->WaitUntilEmpty();
		delete Existing;
	}
}

void FVaRestSecretSet::Set(FName Slot, const FString& Value, double Now)
{
	if (const FString* Old = Current.Find(Slot))
	{
		if (Old->Equals(Value, ESearchCase::CaseSensitive))
		{
			return;
		}
		Previous.Add({ *Old, Now + PreviousLifetime });
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

TArray<FString> FVaRestSecretSet::GetActive(double Now) const
{
	TArray<FString> Out;
	auto Consider = [&Out](const FString& Value)
	{
		if (VaRest::Json::Utf8Length(Value) >= MinBytes)
		{
			Out.AddUnique(Value);
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

FVaRestRedactor::FVaRestRedactor(FVaRestRedactRules InRules, TArray<FString> InSecrets)
	: Rules(MoveTemp(InRules))
{
	for (FString& Secret : InSecrets)
	{
		if (VaRest::Json::Utf8Length(Secret) >= FVaRestSecretSet::MinBytes)
		{
			// Review M6: the forms a secret takes on the wire -- percent-encoded by Path()/Query() (base64's + / =) and
			// JSON-escaped in a body ('"', '\') -- are needles too, when they differ
			const FString Percent = VaRest::Http::PercentEncode(Secret);
			FString Escaped = Secret.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
			Secrets.AddUnique(Percent);
			Secrets.AddUnique(MoveTemp(Escaped));
			Secrets.AddUnique(MoveTemp(Secret));
		}
	}
	Secrets.Sort([](const FString& A, const FString& B) { return A.Len() > B.Len(); });
	for (const FString& Secret : Secrets)
	{
		SecretsUtf8.Add(VaRest::Http::RedactorPrivate::ToUtf8(Secret));
	}
}

bool FVaRestRedactor::ScrubInPlace(FString& Text) const
{
	bool bAny = false;
	for (const FString& Secret : Secrets)
	{
		bAny |= Text.ReplaceInline(*Secret, TEXT("***"), ESearchCase::CaseSensitive) > 0;
	}
	return bAny;
}

bool FVaRestRedactor::ScrubUtf8(TArray<uint8>& Utf8) const
{
	// "a memmem of a few needles over at most the cap" (Inspector 2.2 layer 3)
	bool bAny = false;
	for (const TArray<uint8>& Needle : SecretsUtf8)
	{
		if (Needle.Num() == 0 || Needle.Num() > Utf8.Num())
		{
			continue;
		}
		TArray<uint8> Out;
		bool bHit = false;
		int32 i = 0;
		while (i < Utf8.Num())
		{
			if (i + Needle.Num() <= Utf8.Num() && FMemory::Memcmp(Utf8.GetData() + i, Needle.GetData(), Needle.Num()) == 0)
			{
				Out.Append(reinterpret_cast<const uint8*>("***"), 3);
				i += Needle.Num();
				bHit = true;
			}
			else
			{
				Out.Add(Utf8[i++]);
			}
		}
		if (bHit)
		{
			Utf8 = MoveTemp(Out);
			bAny = true;
		}
	}
	return bAny;
}

FString FVaRestRedactor::ScrubText(FStringView Text) const
{
	++FVaRestTrace::Counters().RedactorCalls;
	FString Out(Text);
	ScrubInPlace(Out);
	return Out;
}

void FVaRestRedactor::AppendRedactedPairs(FStringView Pairs, EVaRestRedactKeyKind Kind, FString& Out) const
{
	int32 Start = 0;
	bool bFirst = true;
	while (Start <= Pairs.Len())
	{
		int32 End = Start;
		while (End < Pairs.Len() && Pairs[End] != TEXT('&'))
		{
			++End;
		}
		const FStringView Piece = Pairs.Mid(Start, End - Start);
		if (!bFirst)
		{
			Out.AppendChar(TEXT('&'));
		}
		bFirst = false;
		int32 Equals = INDEX_NONE;
		if (Piece.FindChar(TEXT('='), Equals) && Rules.ShouldRedact(FStringView(VaRest::Http::PercentDecode(Piece.Left(Equals), true)), Kind))
		{
			Out += Piece.Left(Equals);
			Out += TEXT("=***");
		}
		else
		{
			Out += Piece;
		}
		Start = End + 1;
	}
}

FString FVaRestRedactor::RedactUrlUncounted(FStringView Url) const
{
	FString Out;
	Out.Reserve(Url.Len());
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
	Out += Url.Mid(Pos, PathEnd - Pos);
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
	if (Name.Equals(TEXTVIEW("Location"), ESearchCase::IgnoreCase) || Name.Equals(TEXTVIEW("Content-Location"), ESearchCase::IgnoreCase))
	{
		return RedactUrlUncounted(Value);
	}
	FString Out(Value);
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
	FString Text;
	for (const FVaRestMultipartPart& Part : Parts)
	{
		if (!Text.IsEmpty())
		{
			Text.AppendChar(TEXT('\n'));
		}
		if (Part.bField)
		{
			const bool bSecret = Rules.ShouldRedact(FStringView(Part.Name), EVaRestRedactKeyKind::Part);
			Excerpt.bRedacted |= bSecret;
			Text += FString::Printf(TEXT("part \"%s\": %s"), *Part.Name, bSecret ? TEXT("***") : *ToText(Part.Bytes));
		}
		else
		{
			Text += FString::Printf(TEXT("part \"%s\" filename=\"%s\" %s %lld B"), *Part.Name, *Part.FileName, *Part.ContentType, Part.Size);
		}
	}
	Excerpt.bRedacted |= ScrubInPlace(Text);
	TArray<uint8> Out = ToUtf8(Text);
	if (Cap >= 0 && Out.Num() > Cap)
	{
		Out.SetNum(WholeCharacters(TConstArrayView<uint8>(Out).Left(Cap)));
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
