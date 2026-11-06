// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

#include "VaRestJsonLog.h"
#include "VaRestJsonReader.h"
#include "VaRestJsonUtf8.h"
#include "VaRestJsonWriter.h"

namespace VaRest::Json::TextPrivate
{
	FVaRestJsonError TooDeepToWrite()
	{
		FVaRestJsonError Error;
		Error.Message = FString::Printf(TEXT("cannot write a value nested deeper than %d levels"), MaxDepth);
		return Error;
	}

	/**
	 * First arena chunk for a parse: the input size clamped to [FArena::MinChunkSize, 32 KiB] (VaRest-awm). A small
	 * response fits its first chunk; a large one starts at 32 KiB and doubles to FArena::MaxChunkSize, so a string-heavy
	 * input (strings stay in the input buffer, in situ) never reserves a chunk the size of the input for a few nodes.
	 */
	SIZE_T ArenaHintFor(int64 InputBytes)
	{
		return static_cast<SIZE_T>(FMath::Clamp<int64>(InputBytes, FArena::MinChunkSize, 32 * 1024));
	}

	/** Rejects oversize input before anything is allocated */
	bool ExceedsLimit(int64 InputBytes, const FVaRestJsonParseOptions& Options, FVaRestJsonError& OutError)
	{
		if (Options.MaxInputBytes < 0 || Options.MaxValues < 0 || Options.MaxWorkUnits < 0)
		{
			OutError = FVaRestJsonError();
			OutError.Message = TEXT("JSON parse limits must be nonnegative");
			return true;
		}
		if (InputBytes >= MAX_int32 || (Options.MaxWorkUnits > 0 && InputBytes > Options.MaxWorkUnits))
		{
			OutError = FVaRestJsonError();
			OutError.Message = InputBytes >= MAX_int32 ? TEXT("JSON input cannot fit its terminated buffer") : TEXT("JSON logical work limit exceeded by input bytes");
			return true;
		}
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
	if (Node && !VaRest::Json::Write(*Node, Format, Out))
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::ToUtf8: cannot write a value nested deeper than %d levels"), VaRest::Json::MaxDepth);
	}
}

FString FVaRestJson::ToString(EVaRestJsonFormat Format) const
{
	if (!Node)
	{
		return FString();
	}
	TArray<uint8> Bytes;
	if (!VaRest::Json::Write(*Node, Format, Bytes))
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::ToString: cannot write a value nested deeper than %d levels"), VaRest::Json::MaxDepth);
		return FString();
	}
	return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
}

TValueOrError<FString, FVaRestJsonError> FVaRestJson::TryToString(EVaRestJsonFormat Format) const
{
	TArray<uint8> Bytes;
	if (Node && !VaRest::Json::Write(*Node, Format, Bytes))
	{
		FVaRestJsonError Error = VaRest::Json::TextPrivate::TooDeepToWrite();
		return MakeError(MoveTemp(Error));
	}
	FString Text = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
	return MakeValue(MoveTemp(Text));
}

TValueOrError<void, FVaRestJsonError> FVaRestJson::TryToUtf8(TArray<uint8>& Out, EVaRestJsonFormat Format) const
{
	if (Node && !VaRest::Json::Write(*Node, Format, Out))
	{
		FVaRestJsonError Error = VaRest::Json::TextPrivate::TooDeepToWrite();
		return MakeError(MoveTemp(Error));
	}
	return MakeValue();
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
	// One conversion for the parse and for its diagnostics, so line/column always describe the bytes that were parsed
	// (M1 converted with FPlatformString::Convert here and StringCast on the error path; spec 2.2 wants U+FFFD for an
	// unpaired surrogate, the engine writes '?': GenericPlatformString.cpp:791-797).
	const int64 Utf8Len = Utf8Length(Text);
	FVaRestJsonError Error;
	if (ExceedsLimit(Utf8Len, Options, Error))
	{
		return MakeError(MoveTemp(Error));
	}
	if (Utf8Len >= MAX_int32)
	{
		Error.Message = FString::Printf(TEXT("Input of %lld UTF-8 bytes is larger than 2 GiB"), Utf8Len);
		return MakeError(MoveTemp(Error));
	}
	TArray<uint8> Bytes;
	Bytes.Reserve(static_cast<int32>(Utf8Len) + 1); // the in-situ terminator fits without a reallocation
	Bytes.AddUninitialized(static_cast<int32>(Utf8Len));
	int32 Replaced = 0;
	const int32 Written = WriteUtf8(Text, TArrayView<UTF8CHAR>(reinterpret_cast<UTF8CHAR*>(Bytes.GetData()), Bytes.Num()), Replaced);
	check(Written == Bytes.Num()); // sized by Utf8Length, the same rules
	if (Replaced > 0)
	{
		UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJson::Parse: %d unpaired UTF-16 surrogate(s) replaced with U+FFFD"), Replaced);
	}

	TValueOrError<FVaRestJson, FVaRestJsonError> Result = ParseIntoDocument(MoveTemp(Bytes), Options, FUtf8StringView());
	if (!Result.HasValue() && Result.GetError().Offset >= 0)
	{
		// The in-situ parse rewrote Bytes; convert again for the diagnostics, with the very same converter
		FVaRestJsonError Diagnosed = Result.GetError();
		TArray<UTF8CHAR> Original;
		Original.SetNumUninitialized(static_cast<int32>(Utf8Len));
		int32 Unused = 0;
		WriteUtf8(Text, TArrayView<UTF8CHAR>(Original.GetData(), Original.Num()), Unused);
		FillLineColumn(Diagnosed, FUtf8StringView(Original.GetData(), Original.Num()));
		return MakeError(MoveTemp(Diagnosed));
	}
	return Result;
}
