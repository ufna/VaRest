// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketLog.h"
#include "VaRestWebSocketLog.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeLock.h"

namespace VaRest::WebSocket
{
EVaRestLogLevel SocketLogLevel(EVaRestLogLevel Configured)
{
	const auto* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("VaRest.LogLevel"));
	const int32 Forced = Variable ? Variable->GetInt() : -1;
	int32 Effective = FMath::Clamp(Forced < 0 ? static_cast<int32>(Configured) : Forced, 0, 3);
#if UE_BUILD_SHIPPING
	Effective = FMath::Min(Effective, static_cast<int32>(EVaRestLogLevel::Headers));
#endif
	return static_cast<EVaRestLogLevel>(Effective);
}
FString SocketLogText(const FVaRestRedactor& Redactor, FStringView Text)
{
	FString Safe = Redactor.ScrubText(Text);
	Safe.LeftInline(2048);
	for (TCHAR& Character : Safe) if (Character < TEXT(' ') || Character == 127) Character = TEXT(' ');
	return Safe;
}

struct FSocketLogEntry
{
	TSharedPtr<FSocketFrameLog, ESPMode::ThreadSafe> Owner;
	TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> Redactor;
	TArray<uint8> Prefix;
	FString Channel, Route;
	FName ProtocolTag;
	uint64 SocketId = 0, AttemptId = 0;
	int64 TotalBytes = 0;
	int64 BodyBytes = 0;
	int32 Charge = 0;
	bool bOutbound = false, bBinary = false;
	bool bJson = false;
	~FSocketLogEntry() { if (Owner) Owner->Release(Charge); }
	void Write() const
	{
#if !UE_BUILD_SHIPPING
		check(!IsInGameThread());
		FVaRestTraceExcerpt Excerpt;
		if (bBinary)
		{
			// Hex is opt-in Body output. Feed it through the core's hex-value scrub and capped-tail guard too.
			static const ANSICHAR Hex[] = "0123456789abcdef";
			TArray<uint8> Encoded; Encoded.Reserve(Prefix.Num() * 2);
			for (uint8 Byte : Prefix) { Encoded.Add(Hex[Byte >> 4]); Encoded.Add(Hex[Byte & 15]); }
			Excerpt = Redactor->RedactBody(TEXTVIEW("text/plain"), Encoded, TotalBytes * 2, 128);
		}
		else Excerpt = Redactor->RedactBody(bJson ? FStringView(TEXTVIEW("application/json")) : FStringView(TEXTVIEW("text/plain")), Prefix, BodyBytes, 4096);
		FString Body = Excerpt.ToText();
		for (TCHAR& Character : Body) if (Character < TEXT(' ') || Character == 127) Character = TEXT(' ');
		UE_LOG(LogVaRestSocket, Log, TEXT("Socket %llu attempt %llu %s %s %lld bytes channel=%s route=%s tag=%s%s %s"),
			static_cast<unsigned long long>(SocketId), static_cast<unsigned long long>(AttemptId), bOutbound ? TEXT(">") : TEXT("<"),
			bBinary ? TEXT("binary") : TEXT("text"), static_cast<long long>(TotalBytes), *SocketLogText(*Redactor, Channel),
			*SocketLogText(*Redactor, Route), *SocketLogText(*Redactor, ProtocolTag.ToString()), Excerpt.bTruncated ? TEXT(" [capped]") : TEXT(""), *Body);
#endif
	}
};
FSocketFrameLogCapture::FSocketFrameLogCapture() = default;
FSocketFrameLogCapture::~FSocketFrameLogCapture() = default;
FSocketFrameLogCapture::FSocketFrameLogCapture(FSocketFrameLogCapture&& Other) = default;
FSocketFrameLogCapture& FSocketFrameLogCapture::operator=(FSocketFrameLogCapture&& Other) = default;
void FSocketFrameLogCapture::SetMetadata(FStringView Channel, FStringView Route, FName ProtocolTag)
{
	if (!Entry) return;
	Entry->Channel = SocketLogText(*Entry->Redactor, Channel); Entry->Route = SocketLogText(*Entry->Redactor, Route); Entry->ProtocolTag = ProtocolTag;
}
void FSocketFrameLogCapture::Publish()
{
	if (!Entry) return;
	const auto Owner = Entry->Owner;
	Owner->Publish(MoveTemp(Entry));
}
FSocketFrameLog::~FSocketFrameLog() { check(bStopped && Pending == 0 && PendingBytes == 0); }
void FSocketFrameLog::SetLevelOnGT(EVaRestLogLevel InLevel)
{
	check(IsInGameThread()); Level.store(static_cast<uint8>(InLevel), std::memory_order_release);
}
FSocketFrameLogCapture FSocketFrameLog::Prepare(uint64 SocketId, uint64 AttemptId, bool bOutbound, EVaRestSocketFrameKind Kind,
	TConstArrayView<uint8> Bytes, TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Redactor, int32 JsonOffset, bool bRedactAllJsonValues)
{
	FSocketFrameLogCapture Out;
#if !UE_BUILD_SHIPPING
	if (SocketLogLevel(static_cast<EVaRestLogLevel>(Level.load(std::memory_order_acquire))) != EVaRestLogLevel::Body || !UE_LOG_ACTIVE(LogVaRestSocket, Log)) return Out;
	if (bRedactAllJsonValues)
	{
		auto Rules = Redactor->GetRules(); Rules.RedactAllJsonValues();
		Redactor = MakeShared<const FVaRestRedactor, ESPMode::ThreadSafe>(MoveTemp(Rules), Redactor->GetSecrets());
	}
	const bool bBinary = Kind == EVaRestSocketFrameKind::Binary;
	const int32 WireBytes = Bytes.Num(); const bool bJson = !bBinary && JsonOffset >= 0;
	if (bJson) Bytes = Bytes.RightChop(FMath::Min(JsonOffset, Bytes.Num()));
	const int64 Needed = static_cast<int64>(bBinary ? 64 : 4096) + Redactor->GetMaxNeedleBytes();
	const int32 Take = static_cast<int32>(FMath::Min<int64>(Bytes.Num(), Needed));
	{
		FScopeLock Lock(&Admission);
		// Admission precedes allocation; both undecoded attachment groups and queued log tasks hold their slots.
		if (bStopped || Pending >= MaxPending || Take > MaxBytes - PendingBytes) return Out;
		++Pending; PendingBytes += Take;
	}
	Out.Entry = MakeUnique<FSocketLogEntry>();
	Out.Entry->Owner = AsShared(); Out.Entry->Charge = Take; Out.Entry->Redactor = MoveTemp(Redactor);
	Out.Entry->SocketId = SocketId; Out.Entry->AttemptId = AttemptId; Out.Entry->bOutbound = bOutbound; Out.Entry->bBinary = bBinary;
	Out.Entry->TotalBytes = WireBytes; Out.Entry->BodyBytes = Bytes.Num(); Out.Entry->bJson = bJson;
	if (Take) Out.Entry->Prefix.Append(Bytes.GetData(), Take);
#endif
	return Out;
}
void FSocketFrameLog::Publish(TUniquePtr<FSocketLogEntry> Entry)
{
	{
		FScopeLock Lock(&Admission);
		if (!bStopped)
		{
			Pipe.Launch(UE_SOURCE_LOCATION, [Payload = MoveTemp(Entry)] { Payload->Write(); });
			return;
		}
	}
	// Entry returns its charge outside Admission, including when a transport callback retired the socket.
}
void FSocketFrameLog::Release(int32 Bytes)
{
	FScopeLock Lock(&Admission); --Pending; PendingBytes -= Bytes; check(Pending >= 0 && PendingBytes >= 0);
}
void FSocketFrameLog::StopAndWaitOnGT()
{
	check(IsInGameThread());
	{ FScopeLock Lock(&Admission); bStopped = true; }
	Pipe.WaitUntilEmpty(); // never parse/redact bodies or pump unrelated GTK work here
}
}
