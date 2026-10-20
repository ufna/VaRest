// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

#include "VaRestJsonReader.h"
#include "VaRestJsonWriter.h"

namespace VaRest::Json::TextPrivate
{
	/**
	 * First arena chunk for a parse: input / 32, clamped to [FArena::MinChunkSize, FArena::MaxChunkSize].
	 * Parsed strings stay in the input buffer (in situ), so the DOM can be far smaller than the input: a
	 * 1 MiB single-string document holds one node. Sizing the first chunk like the input reserved that
	 * whole size for nothing; at 1/32 the worst case (all strings) wastes ~3% of the input the document
	 * already owns. Node-heavy input (the DOM is ~4.2x the input, docs/passport/2026-10-19-json-bench.md)
	 * reaches MaxChunkSize after a few doublings; the passport's addendum measures this sizing.
	 */
	SIZE_T ArenaHintFor(int64 InputBytes)
	{
		return static_cast<SIZE_T>(FMath::Clamp<int64>(InputBytes / 32, FArena::MinChunkSize, FArena::MaxChunkSize));
	}

	/** Rejects oversize input before anything is allocated */
	bool ExceedsLimit(int64 InputBytes, const FVaRestJsonParseOptions& Options, FVaRestJsonError& OutError)
	{
		if (Options.MaxInputBytes > 0 && InputBytes > Options.MaxInputBytes)
		{
			OutError = FVaRestJsonError();
			OutError.Offset = -1;
			OutError.Message = FString::Printf(TEXT("Input of %lld bytes exceeds MaxInputBytes (%lld)"), InputBytes, Options.MaxInputBytes);
			return true;
		}
		return false;
	}

	TValueOrError<FVaRestJson, FVaRestJsonError> ParseIntoDocument(TArray<uint8>&& Bytes, const FVaRestJsonParseOptions& Options, FUtf8StringView Original)
	{
		FVaRestJsonError Error;
		if (ExceedsLimit(Bytes.Num(), Options, Error))
		{
			return MakeError(MoveTemp(Error));
		}
		const int64 Bom = BomLength(Bytes.GetData(), Bytes.Num());
		const int64 InputBytes = Bytes.Num();
		TSharedPtr<FDocument, ESPMode::ThreadSafe> Doc = MakeShared<FDocument, ESPMode::ThreadSafe>(ArenaHintFor(InputBytes));
		Doc->Input = MoveTemp(Bytes);
		Doc->Input.Add(0); // no reallocation when the caller left a byte of slack

		if (!ParseInSitu(*Doc, Bom, Options, Original, Error))
		{
			return MakeError(MoveTemp(Error));
		}
		FNode* Root = Doc->Root;
		return MakeValue(FVaRestJson::MakeInternal(MoveTemp(Doc), Root));
	}
}

void FVaRestJson::ToUtf8(TArray<uint8>& Out, EVaRestJsonFormat Format) const
{
	if (Node)
	{
		VaRest::Json::Write(*Node, Format, Out);
	}
}

FString FVaRestJson::ToString(EVaRestJsonFormat Format) const
{
	if (!Node)
	{
		return FString();
	}
	TArray<uint8> Bytes;
	VaRest::Json::Write(*Node, Format, Bytes);
	return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
}

TValueOrError<FVaRestJson, FVaRestJsonError> FVaRestJson::ParseOwned(TArray<uint8>&& Utf8Bytes, const FVaRestJsonParseOptions& Options)
{
	return VaRest::Json::TextPrivate::ParseIntoDocument(MoveTemp(Utf8Bytes), Options, FUtf8StringView());
}

TValueOrError<FVaRestJson, FVaRestJsonError> FVaRestJson::Parse(TConstArrayView<uint8> Utf8Bytes, const FVaRestJsonParseOptions& Options)
{
	using namespace VaRest::Json::TextPrivate;
	FVaRestJsonError Error;
	if (ExceedsLimit(Utf8Bytes.Num(), Options, Error))
	{
		return MakeError(MoveTemp(Error));
	}
	TArray<uint8> Copy;
	Copy.Reserve(Utf8Bytes.Num() + 1);
	Copy.Append(Utf8Bytes.GetData(), Utf8Bytes.Num());
	return ParseIntoDocument(MoveTemp(Copy), Options, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Utf8Bytes.GetData()), Utf8Bytes.Num()));
}

TValueOrError<FVaRestJson, FVaRestJsonError> FVaRestJson::Parse(FUtf8StringView Text, const FVaRestJsonParseOptions& Options)
{
	return Parse(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Text.GetData()), Text.Len()), Options);
}

TValueOrError<FVaRestJson, FVaRestJsonError> FVaRestJson::Parse(FStringView Text, const FVaRestJsonParseOptions& Options)
{
	using namespace VaRest::Json;
	using namespace VaRest::Json::TextPrivate;
	// One copy: convert straight into the owned buffer (with a byte of slack for the terminator). The in-situ
	// parse rewrites that buffer, so line/column are computed on the error path only, by converting again.
	const int32 Utf8Len = FPlatformString::ConvertedLength<UTF8CHAR>(Text.GetData(), Text.Len());
	FVaRestJsonError Error;
	if (ExceedsLimit(Utf8Len, Options, Error))
	{
		return MakeError(MoveTemp(Error));
	}
	TArray<uint8> Bytes;
	Bytes.Reserve(Utf8Len + 1);
	Bytes.AddUninitialized(Utf8Len);
	FPlatformString::Convert(reinterpret_cast<UTF8CHAR*>(Bytes.GetData()), Utf8Len, Text.GetData(), Text.Len());

	TValueOrError<FVaRestJson, FVaRestJsonError> Result = ParseIntoDocument(MoveTemp(Bytes), Options, FUtf8StringView());
	if (!Result.HasValue() && Result.GetError().Offset >= 0)
	{
		FVaRestJsonError Diagnosed = Result.GetError();
		const auto Original = StringCast<UTF8CHAR>(Text.GetData(), Text.Len());
		FillLineColumn(Diagnosed, FUtf8StringView(Original.Get(), Original.Length()));
		return MakeError(MoveTemp(Diagnosed));
	}
	return Result;
}
