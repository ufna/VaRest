// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestDecode.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "VaRestHttpLog.h"
#include "VaRestHttpTesting.h"
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
	// Spec 3.4, plan decision 9: on this task, next to the parse, so the envelope costs no second parse
	return ApplyEnvelope(Finish, Out);
}

bool VaRest::Http::ApplyEnvelope(FFinish& Finish, FVaRestJson& InOut)
{
	if (!Finish.Envelope.IsValid())
	{
		return true;
	}
	const FVaRestEnvelope& Applied = *Finish.Envelope;
	const FVaRestEnvelopeResult Result = Applied.Evaluate(InOut);
	// IN-11 (spec review M-14): the trace's strings are built only when the mask was set at the finish
	if (Finish.bTimeDecode)
	{
		FVaRestTraceEnvelope& Trace = Finish.EnvelopeTrace;
		Trace.bApplied = true;
		Trace.DataPath = Applied.DataPath;
		if (!Result.bSuccess)
		{
			Trace.PredicatePath = Result.FailedPath;
			Trace.Expected = Result.Expected;
			Trace.Actual = Result.Actual;
			Trace.Code = Result.Code;
			Trace.Message = Result.Message;
		}
	}
	if (!Result.bSuccess)
	{
		// Spec 3.4: "A 2xx response whose predicate fails becomes Kind=Server" -- made here, after the retry stage decided on
		// this attempt's status, so it is never retried (Error.bRetryable false)
		Finish.Error = Result.ToError(InOut, Finish.Response.Status);
		InOut = FVaRestJson();
		return false;
	}
	if (!Result.bDataFound)
	{
		Finish.bEnvelopeDataMissing = true;
		Finish.EnvelopeBody = InOut;
	}
	InOut = Result.Data;
	return true;
}

void VaRest::Http::CheckEnvelopePredicate(FFinish& Finish)
{
	if (!Finish.Envelope.IsValid() || !Finish.Envelope->HasPredicate() || Finish.Body.Num() == 0 || Finish.Response.Status == 204 || Finish.bBodyInFile)
	{
		return;
	}
	const FString& ContentType = Finish.Response.ContentType;
	if (!IsJsonContentType(ContentType) && !(ContentType.IsEmpty() && DecodePrivate::LooksLikeJson(Finish.Body)))
	{
		return;
	}
	// A void result decodes nothing: a broken body, or one over MaxJsonBytes (refused before parsing, as DecodeJson does), is
	// no envelope failure -- but the predicate never ran, which the request's Basic line says (spec review M-13)
	FString Unchecked;
	const int64 Cap = Finish.ParseOptions.MaxInputBytes;
	if (Cap > 0 && Finish.Body.Num() > Cap)
	{
		Unchecked = FString::Printf(TEXT("the body is %d bytes, over the MaxJsonBytes limit of %lld"), Finish.Body.Num(), Cap);
	}
	else
	{
#if !UE_BUILD_SHIPPING
		// Concurrency review I-6: counted where the parse happens -- OnChainDone sends a void result with a predicate to a task
		if (IsInGameThread())
		{
			++Testing::Counters().EnvelopeParsesOnGameThread;
		}
#endif
		// A copy, not ParseOwned: the bytes stay whole for Response.Body (KeepBody(), or an error's diagnostics)
		TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(TConstArrayView<uint8>(Finish.Body), Finish.ParseOptions);
		if (Parsed.HasValue())
		{
			FVaRestJson ParsedBody = Parsed.StealValue();
			ApplyEnvelope(Finish, ParsedBody);
			return;
		}
		// The parser's own text: a reason and a position, never the body's bytes (VaRestJsonReader.cpp:96-98, :108)
		const FVaRestJsonError& JsonError = Parsed.GetError();
		Unchecked = JsonError.Offset >= 0
			? FString::Printf(TEXT("the body is not valid JSON: %s (byte %lld)"), *JsonError.Message, JsonError.Offset)
			: FString::Printf(TEXT("the body is not valid JSON: %s"), *JsonError.Message);
	}
	if (Finish.LogLevel != EVaRestLogLevel::None && UE_LOG_ACTIVE(LogVaRestHttp, Warning))
	{
		UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu VaRest: the envelope's predicate could not be checked (%s)"), Finish.Response.RequestId, *EscapeForLog(Unchecked));
	}
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
		if (Finish.bEnvelopeDataMissing)
		{
			// Plan decision 9: a struct names the missing path instead of decoding to its defaults
			Finish.Error.Kind = EVaRestErrorKind::Decode;
			Finish.Error.Message = FString::Printf(TEXT("the envelope's data path \"%s\" is not in the response"), *Finish.Envelope->DataPath);
			Finish.Error.Path = Finish.Envelope->DataPath;
			Finish.Error.Body = Finish.EnvelopeBody;
			return false;
		}
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
	if (Finish.Envelope.IsValid() && Finish.Envelope->bParseErrorBodies && Finish.Error.Body.IsValid())
	{
		// Spec 3.4: "bParseErrorBodies (read message/code from 4xx/5xx bodies)"
		FString Code;
		FString Message;
		Finish.Envelope->ReadErrorFields(Finish.Error.Body, Code, Message);
		if (!Code.IsEmpty())
		{
			Finish.Error.Code = Code;
		}
		if (!Message.IsEmpty())
		{
			Finish.Error.Message = Message;
		}
		// IN-11 (spec review M-14): only when the trace mask was set at the finish
		if (Finish.bTimeDecode)
		{
			Finish.EnvelopeTrace.bApplied = true;
			Finish.EnvelopeTrace.DataPath = Finish.Envelope->DataPath;
			Finish.EnvelopeTrace.Code = MoveTemp(Code);
			Finish.EnvelopeTrace.Message = MoveTemp(Message);
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
