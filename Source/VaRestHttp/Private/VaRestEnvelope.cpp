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
		// Integral doubles convert too (VaRestJsonNode.h:108-157, NodeToInt64 and NodeToUInt64): 200, 200.0 and 2e2 all render "200"
		if (const TOptional<int64> Signed = Value.TryGet<int64>())
		{
			return LexToString(Signed.GetValue());
		}
		if (const TOptional<uint64> Unsigned = Value.TryGet<uint64>())
		{
			return LexToString(Unsigned.GetValue());
		}
		const double Number = Value.AsDouble();
		if (FMath::IsFinite(Number) && FMath::TruncToDouble(Number) == Number)
		{
			// Integral past 64 bits: still no fraction and no exponent
			return FString::Printf(TEXT("%.0f"), Number);
		}
		// Shortest round-trip text: the writer's (RapidJSON's Writer::Double, VaRestJsonSax.h:49)
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
	OutMessage = ErrorMessagePath.IsEmpty() ? FString() : FieldText(Body.Path(ErrorMessagePath));
}

FVaRestEnvelopeResult FVaRestEnvelope::Evaluate(const FVaRestJson& Body) const
{
	using namespace VaRest::Http::EnvelopePrivate;
	FVaRestEnvelopeResult Out;
	if (HasPredicate())
	{
		const FVaRestJson Found = Body.Path(SuccessPath);
		// Ruling Q3: compared as MatchText renders it, case-sensitive; null, objects and arrays never match
		const TOptional<FString> Text = MatchText(Found);
		const bool bPass = Text.IsSet() && SuccessValues.ContainsByPredicate([&Text](const FString& Value) { return Value.Equals(Text.GetValue(), ESearchCase::CaseSensitive); });
		if (!bPass)
		{
			Out.bSuccess = false;
			Out.bDataFound = false;
			Out.FailedPath = SuccessPath;
			Out.Expected = FString::Join(SuccessValues, TEXT("|"));
			// The body's own text for display (IN-5): what the server sent, not what was compared
			Out.Actual = Found.IsValid() ? Shown(ValueText(Found)) : FString(TEXT("(missing)"));
			ReadErrorFields(Body, Out.Code, Out.Message);
			return Out;
		}
	}
	if (DataPath.IsEmpty())
	{
		Out.Data = Body;
		Out.bDataFound = Body.IsValid();
		return Out;
	}
	Out.Data = Body.Path(DataPath);
	Out.bDataFound = Out.Data.IsValid();
	return Out;
}
