// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

#include "Misc/OutputDevice.h"
#include "Serialization/Archive.h"
#include "VaRestJsonDocument.h"
#include "VaRestJsonLog.h"
#include "VaRestJsonReader.h"
#include "VaRestJsonWriter.h"

namespace VaRest::Json::StructOpsPrivate
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
// a known cost for large T3D arrays of handles. The resulting document keeps only the value's bytes.
bool FVaRestJson::ImportTextItem(const TCHAR*& Buffer, int32 PortFlags, UObject* Parent, FOutputDevice* ErrorText)
{
	using namespace VaRest::Json;
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
	// (No parameter is named Num: it would hide FVaRestJson::Num, an MSVC C4458 error.)
	const auto MakeInput = [](FDocument& Target, const UTF8CHAR* Bytes, int64 Count)
	{
		Target.Input.Reserve(static_cast<int32>(Count) + 1); // the terminator fits without a reallocation
		Target.Input.Append(reinterpret_cast<const uint8*>(Bytes), static_cast<int32>(Count));
		Target.Input.Add(0);
	};
	FDocumentRef NewDoc = MakeShared<FDocument, ESPMode::ThreadSafe>();
	MakeInput(*NewDoc, Utf8.Get(), Utf8.Length());

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

	if (Consumed < Utf8.Length())
	{
		// The document would otherwise own the whole remaining text (a T3D blob, the rest of a parent
		// struct's text) for as long as the handle lives. Re-parse exactly the value's bytes into a
		// document of their own: the in-situ parse rewrote NewDoc's copy, never the Utf8 conversion.
		FDocumentRef Exact = MakeShared<FDocument, ESPMode::ThreadSafe>();
		MakeInput(*Exact, Utf8.Get(), Consumed);
		if (!ParseInSitu(*Exact, 0, Options, FUtf8StringView(Utf8.Get(), static_cast<int32>(Consumed)), Error))
		{
			// Unreachable for a value that just parsed as a prefix; fail rather than keep the big buffer
			if (ErrorText)
			{
				ErrorText->Logf(ELogVerbosity::Warning, TEXT("FVaRestJson::ImportTextItem: %s"), *Error.ToString());
			}
			return false;
		}
		NewDoc = MoveTemp(Exact);
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
	using namespace VaRest::Json::StructOpsPrivate;
	// Reference collectors that neither load nor save (FArchiveReplaceObjectRef, FArchiveReferenceMarker:
	// ArchiveReplaceObjectRef.h:196, ArchiveReferenceMarker.h:25) walk every property looking for UObject
	// pointers. A handle holds none, so writing its JSON text there is pure cost. A collector that also
	// loads or saves (FImportExportCollector sets IsSaving + IsPersistent, ImportExportCollector.cpp:11-14)
	// still gets the full format, so its byte stream stays consistent. Counting archives are not skipped:
	// FArchiveCountMem only sums CountBytes (ArchiveCountMem.h:35-39), but other counting archives may
	// measure the serialized bytes.
	if (Ar.IsObjectReferenceCollector() && !Ar.IsLoading() && !Ar.IsSaving())
	{
		return true;
	}

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
		Bytes.Reserve(static_cast<int32>(Length) + 1); // slack for the terminator: ParseOwned stays zero-copy
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
	// Write the text first: a value nested deeper than MaxDepth cannot be written, and a load would reject it anyway
	// (spec 2.2), so it is saved as an invalid handle plus an error instead of a payload nothing reads back.
	TArray<uint8> Bytes;
	if (ValidByte != 0 && !VaRest::Json::Write(*Node, EVaRestJsonFormat::Condensed, Bytes))
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::Serialize: cannot save a value nested deeper than %d levels; saved as an invalid handle"), VaRest::Json::MaxDepth);
		ValidByte = 0;
	}
	Ar << ValidByte;
	if (ValidByte != 0)
	{
		int64 Length = Bytes.Num();
		Ar << Length;
		if (Length > 0)
		{
			Ar.Serialize(Bytes.GetData(), Length);
		}
	}
	return true;
}
