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
}

bool FVaRestJson::ExportTextItem(FString& ValueStr, const FVaRestJson& DefaultValue, UObject* Parent, int32 PortFlags, UObject* ExportRootScope) const
{
	// An invalid handle exports as nothing; ImportTextItem reads "nothing" back as invalid.
	ValueStr += ToString(EVaRestJsonFormat::Condensed);
	return true;
}

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
	uint8 Version = SerializeVersion;
	Ar << Version;
	bool bValid = IsValid();
	Ar << bValid;

	if (Ar.IsLoading())
	{
		*this = FVaRestJson();
		if (bValid)
		{
			TArray<uint8> Bytes;
			Ar << Bytes;
			TValueOrError<FVaRestJson, FVaRestJsonError> Result = ParseOwned(MoveTemp(Bytes));
			if (Result.HasValue())
			{
				*this = Result.StealValue();
			}
			else
			{
				UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::Serialize: stored JSON does not parse: %s"), *Result.GetError().ToString());
			}
		}
	}
	else if (bValid)
	{
		TArray<uint8> Bytes;
		ToUtf8(Bytes);
		Ar << Bytes;
	}
	return true;
}
