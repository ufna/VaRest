// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestEnvelope.h"

namespace VaRest::Http::EnvelopePrivate
{
	/** A server's value in a message or a trace field: bounded, so a whole object in "ok" does not become a page of text */
	constexpr int32 MaxShownChars = 256;

	FString Shown(FString Text)
	{
		return Text.Len() > MaxShownChars ? Text.Left(MaxShownChars) + TEXT("...") : MoveTemp(Text);
	}

	/** An error field as text: MatchText's rendering, so a numeric code 42.0 reads "42"; else condensed JSON */
	FString FieldText(const FVaRestJson& Value)
	{
		TOptional<FString> Text = FVaRestEnvelope::MatchText(Value);
		return Text.IsSet() ? MoveTemp(Text.GetValue()) : FVaRestEnvelope::ValueText(Value);
	}
}

FVaRestError FVaRestEnvelopeResult::ToError(const FVaRestJson& Body, int32 HttpStatus) const
{
	FVaRestError Error;
	Error.Kind = EVaRestErrorKind::Server;
	Error.HttpStatus = HttpStatus;
	Error.Code = Code;
	Error.Message = !Message.IsEmpty() ? Message
		: FString::Printf(TEXT("%s (%s is %s, not %s)"), *FVaRestError::DefaultMessage(EVaRestErrorKind::Server, HttpStatus), *FailedPath, *Actual, *Expected);
	Error.Body = Body;
	Error.bRetryable = false;
	return Error;
}

FVaRestEnvelope FVaRestEnvelope::DataAt(FStringView Path)
{
	FVaRestEnvelope Envelope;
	Envelope.DataPath = FString(Path);
	return Envelope;
}

FVaRestEnvelope FVaRestEnvelope::SuccessWhen(FStringView Path, TArray<FString> Values) const
{
	FVaRestEnvelope Copy = *this;
	Copy.SuccessPath = FString(Path);
	Copy.SuccessValues = MoveTemp(Values);
	return Copy;
}

FVaRestEnvelope FVaRestEnvelope::ErrorMessageAt(FStringView Path) const
{
	FVaRestEnvelope Copy = *this;
	Copy.ErrorMessagePath = FString(Path);
	return Copy;
}

FVaRestEnvelope FVaRestEnvelope::ErrorCodeAt(FStringView Path) const
{
	FVaRestEnvelope Copy = *this;
	Copy.ErrorCodePath = FString(Path);
	return Copy;
}

FVaRestEnvelope FVaRestEnvelope::ParseErrorBodies(bool bParse) const
{
	FVaRestEnvelope Copy = *this;
	Copy.bParseErrorBodies = bParse;
	return Copy;
}

TOptional<FString> FVaRestEnvelope::MatchText(const FVaRestJson& Value)
{
	switch (Value.GetType())
	{
	case EVaRestJsonType::String:
		return Value.AsString();
	case EVaRestJsonType::Bool:
		return FString(Value.AsBool() ? TEXT("true") : TEXT("false"));
	case EVaRestJsonType::Number:
	{
		// Integral doubles in [-2^63, 2^64) convert too (VaRestJsonNode.h:108-163, NodeToInt64 and NodeToUInt64): 200, 200.0
		// and 2e2 all render "200"
		if (const TOptional<int64> Signed = Value.TryGet<int64>())
		{
			return LexToString(Signed.GetValue());
		}
		if (const TOptional<uint64> Unsigned = Value.TryGet<uint64>())
		{
			return LexToString(Unsigned.GetValue());
		}
		// Any other number -- a fraction, or an integral double past 64 bits -- is the writer's shortest round-trip text
		// (RapidJSON's Writer::Double, VaRestJsonSax.h:49): 1e300 is "1e300". Not %.0f (Task 10 review minor 1): that prints
		// the double's exact binary expansion, 301 digits that no server and no SuccessValues entry ever spells.
		return Value.ToString(EVaRestJsonFormat::Condensed);
	}
	default:
		// Ruling Q3: null, an object and an array never match, nor does an invalid handle
		return TOptional<FString>();
	}
}

FString FVaRestEnvelope::ValueText(const FVaRestJson& Value)
{
	if (!Value.IsValid())
	{
		return FString();
	}
	return Value.IsString() ? Value.AsString() : Value.ToString(EVaRestJsonFormat::Condensed);
}

void FVaRestEnvelope::ReadErrorFields(const FVaRestJson& Body, FString& OutCode, FString& OutMessage) const
{
	using namespace VaRest::Http::EnvelopePrivate;
	OutCode = ErrorCodePath.IsEmpty() ? FString() : Shown(FieldText(Body.Path(ErrorCodePath)));
	if (ErrorMessagePath.IsEmpty())
	{
		OutMessage.Reset();
		return;
	}
	// A string (or a scalar) is the server's own user-facing text and stays whole; an object or an array is only a fallback
	// rendering of what was there, capped like every shown value (Task 10 review minor 2)
	const FVaRestJson Found = Body.Path(ErrorMessagePath);
	TOptional<FString> Text = MatchText(Found);
	OutMessage = Text.IsSet() ? MoveTemp(Text.GetValue()) : Shown(ValueText(Found));
}

FVaRestEnvelopeResult FVaRestEnvelope::EvaluatePredicate(const FVaRestJson& Body) const
{
	using namespace VaRest::Http::EnvelopePrivate;
	FVaRestEnvelopeResult Out;
	// Never looked up here: a void result keeps no data (plan decision 9), and Evaluate fills both on a success
	Out.bDataFound = false;
	if (!HasPredicate())
	{
		return Out;
	}
	const FVaRestJson Found = Body.Path(SuccessPath);
	// Ruling Q3: compared as MatchText renders it, case-sensitive; null, objects and arrays never match
	const TOptional<FString> Text = MatchText(Found);
	const bool bPass = Text.IsSet() && SuccessValues.ContainsByPredicate([&Text](const FString& Value) { return Value.Equals(Text.GetValue(), ESearchCase::CaseSensitive); });
	if (!bPass)
	{
		Out.bSuccess = false;
		Out.FailedPath = SuccessPath;
		// An empty allow-list allows nothing (Task 10 review minor 4): it says so, not "..., not )"
		Out.Expected = SuccessValues.Num() > 0 ? FString::Join(SuccessValues, TEXT("|")) : FString(TEXT("(none)"));
		// The body's own text for display (IN-5): what the server sent, not what was compared
		Out.Actual = Found.IsValid() ? Shown(ValueText(Found)) : FString(TEXT("(missing)"));
		ReadErrorFields(Body, Out.Code, Out.Message);
	}
	return Out;
}

FVaRestEnvelopeResult FVaRestEnvelope::Evaluate(const FVaRestJson& Body) const
{
	FVaRestEnvelopeResult Out = EvaluatePredicate(Body);
	if (!Out.bSuccess)
	{
		return Out;
	}
	Out.Data = DataPath.IsEmpty() ? Body : Body.Path(DataPath);
	Out.bDataFound = Out.Data.IsValid();
	return Out;
}
