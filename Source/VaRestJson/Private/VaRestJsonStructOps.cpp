// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

#include "Misc/OutputDevice.h"
#include "Serialization/Archive.h"
#include "VaRestJsonDocument.h"
#include "VaRestJsonLog.h"
#include "VaRestJsonReader.h"

using namespace VaRest::Json;

namespace
{
	constexpr uint8 SerializeVersion = 1;
	constexpr int64 MaxSerializedBytes = int64(1) << 30;
}

bool FVaRestJson::ExportTextItem(FString& ValueStr, const FVaRestJson& DefaultValue, UObject* Parent, int32 PortFlags, UObject* ExportRootScope) const
{
	// An invalid handle exports as nothing; ImportTextItem reads "nothing" back as invalid.
	ValueStr += ToString(EVaRestJsonFormat::Condensed);
	return true;
}

// Each call converts the whole remaining buffer tail to UTF-8 (O(tail) per field): fine for pin defaults,
// a known cost for large T3D arrays of handles.
bool FVaRestJson::ImportTextItem(const TCHAR*& Buffer, int32 PortFlags, UObject* Parent, FOutputDevice* ErrorText)
{
	const TCHAR* Cursor = Buffer;
	while (FChar::IsWhitespace(*Cursor))
	{
		++Cursor;
	}
	if (*Cursor == TEXT('\0') || *Cursor == TEXT(',') || *Cursor == TEXT(')'))
	{
		*this = FVaRestJson(); // an empty field inside a parent struct's text
		Buffer = Cursor;
		return true;
	}

	// Parse a prefix of the rest of the buffer: the JSON value ends where RapidJSON stops
	// (kParseStopWhenDoneFlag), and the parent's parser continues from there.
	const auto Utf8 = StringCast<UTF8CHAR>(Cursor);
	FDocumentRef NewDoc = MakeShared<FDocument, ESPMode::ThreadSafe>();
	NewDoc->Input.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	NewDoc->Input.Add(0);

	FVaRestJsonParseOptions Options;
	FVaRestJsonError Error;
	int64 Consumed = 0;
	if (!ParseInSitu(*NewDoc, 0, Options, FUtf8StringView(Utf8.Get(), Utf8.Length()), Error, &Consumed))
	{
		if (ErrorText)
		{
			ErrorText->Logf(ELogVerbosity::Warning, TEXT("FVaRestJson::ImportTextItem: %s"), *Error.ToString());
		}
		return false;
	}

	// Map consumed UTF-8 bytes back to TCHARs of the original buffer
	const auto Back = StringCast<TCHAR>(Utf8.Get(), static_cast<int32>(Consumed));
	Buffer = Cursor + Back.Length();

	FNode* Root = NewDoc->Root;
	*this = FVaRestJson(MoveTemp(NewDoc), Root);
	return true;
}

bool FVaRestJson::Identical(const FVaRestJson* Other, uint32 PortFlags) const
{
	return Other ? Equals(*Other) : !IsValid();
}

bool FVaRestJson::Serialize(FArchive& Ar)
{
	// Format v1: uint8 Version, uint8 bValid, and when valid: int64 Length + Length raw UTF-8 bytes.
	// The archive may be corrupt or hostile (this runs on every asset load), so nothing read is trusted
	// before it is bounded, and the payload is never read through TArray's count-then-allocate path.
	uint8 Version = SerializeVersion;
	uint8 ValidByte = IsValid() ? 1 : 0;

	if (Ar.IsLoading())
	{
		*this = FVaRestJson();
		Ar << Version;
		if (Ar.IsError())
		{
			return true;
		}
		if (Version != SerializeVersion)
		{
			UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: unsupported serialized version %d"), static_cast<int32>(Version));
			Ar.SetError();
			return true;
		}
		Ar << ValidByte;
		if (Ar.IsError() || ValidByte == 0)
		{
			return true;
		}

		int64 Length = 0;
		Ar << Length;
		if (Ar.IsError())
		{
			return true;
		}
		const int64 TotalSize = Ar.TotalSize();
		if (Length < 0 || Length > MaxSerializedBytes || (TotalSize > 0 && Length > TotalSize - Ar.Tell()))
		{
			UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::Serialize: implausible payload length %lld"), Length);
			Ar.SetError();
			return true;
		}

		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(static_cast<int32>(Length));
		if (Length > 0)
		{
			Ar.Serialize(Bytes.GetData(), Length);
		}
		if (Ar.IsError())
		{
			return true;
		}

		TValueOrError<FVaRestJson, FVaRestJsonError> Result = ParseOwned(MoveTemp(Bytes));
		if (Result.HasValue())
		{
			*this = Result.StealValue();
		}
		else
		{
			// The archive itself is intact: do not fail the whole load for one bad value.
			UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::Serialize: stored JSON does not parse: %s"), *Result.GetError().ToString());
		}
		return true;
	}

	Ar << Version;
	Ar << ValidByte;
	if (ValidByte != 0)
	{
		TArray<uint8> Bytes;
		ToUtf8(Bytes);
		int64 Length = Bytes.Num();
		Ar << Length;
		if (Length > 0)
		{
			Ar.Serialize(Bytes.GetData(), Length);
		}
	}
	return true;
}
