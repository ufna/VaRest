// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestDecode.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "VaRestJsonUtf8.h"

namespace VaRest::Http::DecodePrivate
{
	FString ToText(TConstArrayView<uint8> Bytes)
	{
		VaRest::Json::FUtf8Scratch Repaired;
		VaRest::Json::AppendRepairedUtf8(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num()), Repaired);
		return FString::ConstructFromPtrSize(Repaired.GetData(), Repaired.Num());
	}

	bool LooksLikeJson(TConstArrayView<uint8> Bytes)
	{
		for (const uint8 Byte : Bytes)
		{
			if (Byte == ' ' || Byte == '\t' || Byte == '\r' || Byte == '\n')
			{
				continue;
			}
			return Byte == '{' || Byte == '[';
		}
		return false;
	}
}

FString VaRest::Http::RawBodyText(TConstArrayView<uint8> Bytes)
{
	return DecodePrivate::ToText(Bytes.Left(FMath::Min(Bytes.Num(), RawBodyCap)));
}

void VaRest::Http::ReleaseBody(FFinish& Finish)
{
	if (Finish.bKeepBody)
	{
		Finish.Response.Body = MoveTemp(Finish.Body);
	}
	else
	{
		Finish.Body.Empty();
	}
}

bool VaRest::Http::DecodeJson(FFinish& Finish, FVaRestJson& Out)
{
	Out = FVaRestJson();
	if (Finish.Body.Num() == 0 || Finish.Response.Status == 204)
	{
		ReleaseBody(Finish);
		return true;
	}
	const FString& ContentType = Finish.Response.ContentType;
	if (!ContentType.IsEmpty() && !IsJsonContentType(ContentType))
	{
		Finish.Error.Kind = EVaRestErrorKind::Decode;
		Finish.Error.Message = FString::Printf(TEXT("expected a JSON response, got Content-Type \"%s\""), *ContentType);
		Finish.Error.RawBody = RawBodyText(Finish.Body);
		Finish.Response.Body = MoveTemp(Finish.Body); // kept on error (spec 3.2)
		return false;
	}
	const int64 Cap = Finish.ParseOptions.MaxInputBytes;
	if (Cap > 0 && Finish.Body.Num() > Cap)
	{
		// Decision 20: the arena DOM is about 4.2x its input; refused before anything is parsed, the bytes kept (spec 3.2)
		Finish.Error.Kind = EVaRestErrorKind::Decode;
		Finish.Error.Message = FString::Printf(TEXT("the JSON response is %d bytes, over the MaxJsonBytes limit of %lld"), Finish.Body.Num(), Cap);
		Finish.Error.RawBody = RawBodyText(Finish.Body);
		Finish.Response.Body = MoveTemp(Finish.Body);
		return false;
	}
	if (Finish.bKeepBody)
	{
		// A copy: the in-situ parse unescapes strings inside the buffer (spec 2.1)
		Finish.Response.Body = Finish.Body;
	}
	// Decision 26: the diagnostic prefix is copied before the parse rewrites the buffer -- at most RawBodyCap bytes, held
	// inline (no allocation per response); KeepBody()'s copy above already holds the whole body unparsed, so none then
	TArray<uint8, TInlineAllocator<RawBodyCap>> Diagnostic;
	if (!Finish.bKeepBody)
	{
		Diagnostic.Append(Finish.Body.GetData(), FMath::Min(Finish.Body.Num(), RawBodyCap));
	}
	TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::ParseOwned(MoveTemp(Finish.Body), Finish.ParseOptions);
	if (Parsed.HasError())
	{
		const FVaRestJsonError& JsonError = Parsed.GetError();
		Finish.Error.Kind = EVaRestErrorKind::Decode;
		Finish.Error.Message = JsonError.Offset >= 0
			? FString::Printf(TEXT("the response is not valid JSON: %s (byte %lld)"), *JsonError.Message, JsonError.Offset)
			: FString::Printf(TEXT("the response is not valid JSON: %s"), *JsonError.Message);
		Finish.Error.RawBody = RawBodyText(Finish.bKeepBody ? TConstArrayView<uint8>(Finish.Response.Body) : TConstArrayView<uint8>(Diagnostic));
		return false;
	}
	Out = Parsed.StealValue();
	return true;
}

bool VaRest::Http::DecodeStruct(FFinish& Finish, const UScriptStruct* Struct, void* Data)
{
	FVaRestJson Json;
	if (!DecodeJson(Finish, Json))
	{
		return false;
	}
	if (!Json.IsValid())
	{
		return true; // 204 or empty: the struct keeps its defaults (spec 3.2)
	}
	FVaRestJsonDecodeReport Report;
	const bool bDecoded = FVaRestJsonConvert::JsonToStruct(Json, Struct, Data, Finish.DecodeOptions, &Report);
	Finish.Response.DecodeWarnings = MoveTemp(Report.Warnings);
	if (bDecoded)
	{
		return true;
	}
	const FVaRestJsonError* First = Report.Errors.Num() > 0 ? &Report.Errors[0] : nullptr;
	Finish.Error.Kind = EVaRestErrorKind::Decode;
	Finish.Error.Message = First ? First->Message : FString::Printf(TEXT("the response is not a JSON object for %s"), *Struct->GetName());
	Finish.Error.Path = First ? First->Path : FString();
	// Valid JSON that does not fit: the parsed body is the diagnostic
	Finish.Error.Body = Json;
	return false;
}

void VaRest::Http::DecodeText(FFinish& Finish, FString& Out)
{
	Out = DecodePrivate::ToText(Finish.Body);
	ReleaseBody(Finish);
}

void VaRest::Http::ProcessErrorBody(FFinish& Finish)
{
	if (Finish.Body.Num() == 0)
	{
		return;
	}
	const FString& ContentType = Finish.Response.ContentType;
	const bool bJson = IsJsonContentType(ContentType) || (ContentType.IsEmpty() && DecodePrivate::LooksLikeJson(Finish.Body));
	if (bJson)
	{
		TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(TConstArrayView<uint8>(Finish.Body), Finish.ParseOptions);
		if (Parsed.HasValue())
		{
			Finish.Error.Body = Parsed.StealValue();
		}
	}
	if (!Finish.Error.Body.IsValid())
	{
		Finish.Error.RawBody = RawBodyText(Finish.Body);
	}
	Finish.Response.Body = MoveTemp(Finish.Body);
}

FString VaRest::Http::MakeTempFileName(const FString& Destination, uint64 RequestId)
{
	return FString::Printf(TEXT("%s.varest-%llu-%s.part"), *Destination, RequestId, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

VaRest::Http::FFileOps VaRest::Http::PlatformFileOps()
{
	FFileOps Ops;
	Ops.Move = [](const FString& To, const FString& From) { return FPlatformFileManager::Get().GetPlatformFile().MoveFile(*To, *From); };
	Ops.Delete = [](const FString& Path) { return FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*Path); };
	Ops.IsFile = [](const FString& Path) { return FPlatformFileManager::Get().GetPlatformFile().FileExists(*Path); };
	return Ops;
}

bool VaRest::Http::MoveIntoPlace(const FString& To, const FString& From, bool bMoveRefusesTarget, const FFileOps& Ops, FString& OutStranded)
{
	OutStranded.Reset();
	if (Ops.Move(To, From))
	{
		return true;
	}
	if (!bMoveRefusesTarget || !Ops.IsFile(To))
	{
		// rename() replaces atomically, so its failure is real; with no file to move aside, so is MoveFileW's
		return false;
	}
	const FString Backup = FString::Printf(TEXT("%s.varest-%s.bak"), *To, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!Ops.Move(Backup, To))
	{
		return false; // To untouched
	}
	if (Ops.Move(To, From))
	{
		Ops.Delete(Backup);
		return true;
	}
	if (!Ops.Move(To, Backup))
	{
		OutStranded = Backup;
	}
	return false;
}

bool VaRest::Http::FinalizeSaveTo(FFinish& Finish, TFunctionRef<bool()> IsCanceled)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	auto Fail = [&Finish, &PlatformFile](FString&& Problem)
	{
		Finish.Error.Kind = EVaRestErrorKind::Decode;
		Finish.Error.Message = MoveTemp(Problem);
		PlatformFile.DeleteFile(*Finish.TempFile);
		return false;
	};
	// Creates the temp file (OpenWrite truncates) and writes Bytes into it
	auto WriteTemp = [&Finish, &PlatformFile](TConstArrayView<uint8> Bytes)
	{
		PlatformFile.CreateDirectoryTree(*FPaths::GetPath(Finish.SaveTo));
		TUniquePtr<IFileHandle> Created(PlatformFile.OpenWrite(*Finish.TempFile));
		return Created.IsValid() && (Bytes.Num() == 0 || Created->Write(Bytes.GetData(), Bytes.Num()));
	};
	if (!Finish.bBodyInFile)
	{
		// Review M1: a stage answered, so this outcome's bytes are in memory, and a file at TempFile is an earlier attempt's
		if (!Finish.TempFile.IsEmpty())
		{
			PlatformFile.DeleteFile(*Finish.TempFile);
		}
		Finish.TempFile = MakeTempFileName(Finish.SaveTo, Finish.Response.RequestId);
		if (!WriteTemp(Finish.Body))
		{
			return Fail(FString::Printf(TEXT("could not write the download to %s"), *Finish.TempFile));
		}
		Finish.Body.Empty(); // on disk now, as a transport's download is
	}
	else if (!PlatformFile.FileExists(*Finish.TempFile))
	{
		if (Finish.Response.BytesDown > 0)
		{
			// Review I1: bytes arrived, so the sink made this file; without it the download is lost, never an empty success
			return Fail(FString::Printf(TEXT("%lld bytes were received for %s, but their temp file %s is gone"),
				Finish.Response.BytesDown, *Finish.SaveTo, *Finish.TempFile));
		}
		// No byte arrived, so the sink never opened the file: an empty body is an empty file
		if (!WriteTemp(TConstArrayView<uint8>()))
		{
			return Fail(FString::Printf(TEXT("could not create the download at %s"), *Finish.TempFile));
		}
	}
	if (IsCanceled())
	{
		// Review M-4: "a canceled download never touches the destination" -- read as late as it can be
		PlatformFile.DeleteFile(*Finish.TempFile);
		return false;
	}
	FString Stranded;
	if (!MoveIntoPlace(Finish.SaveTo, Finish.TempFile, PLATFORM_WINDOWS != 0, PlatformFileOps(), Stranded))
	{
		return Fail(Stranded.IsEmpty()
			? FString::Printf(TEXT("could not move the download onto %s; what was there is untouched"), *Finish.SaveTo)
			: FString::Printf(TEXT("could not move the download onto %s, nor put the previous file back: it is at %s"), *Finish.SaveTo, *Stranded));
	}
	Finish.Response.SavedFile = Finish.SaveTo;
	return true;
}

void VaRest::Http::LoadFilePrefix(const FString& Path, TArray<uint8>& Out)
{
	TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
	if (!File.IsValid())
	{
		return;
	}
	const int64 Count = FMath::Min<int64>(File->Size(), RawBodyCap);
	Out.SetNumUninitialized(static_cast<int32>(Count));
	if (Count > 0 && !File->Read(Out.GetData(), Count))
	{
		Out.Reset();
	}
}
