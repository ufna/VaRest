// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestHttpTesting.h"

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Parse.h"
#include "Misc/ScopeLock.h"
#include "String/Find.h"
#include "VaRestRedactionPipe.h"

#if !UE_BUILD_SHIPPING
VaRest::Http::Testing::FTestCounters& VaRest::Http::Testing::Counters()
{
	static FTestCounters Instance;
	return Instance;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
bool VaRest::Http::Testing::PumpUntil(TFunctionRef<bool()> Done, double TimeoutSeconds, bool bTickCoreTicker)
{
	check(IsInGameThread());
	// Review M-17, M-9: from inside a game-thread task the engine's own guard is a bare verify (TaskGraph.cpp:705); name the misuse instead
	checkf(!FTaskGraphInterface::Get().IsThreadProcessingTasks(ENamedThreads::GameThread),
		TEXT("Testing::PumpUntil called from inside a game-thread task: call it from a test body"));
	// Nested in itself (from a callback it runs): a second Tick would see slots the outer Tick moved out (Ticker.cpp:117, :128)
	static bool bPumping = false;
	checkf(!bPumping, TEXT("Testing::PumpUntil is not re-entrant: call it from a test body, never from a callback, a task or a ticker it runs"));
	TGuardValue<bool> Pumping(bPumping, true);
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	double LastTick = FPlatformTime::Seconds();
	for (;;)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		if (bTickCoreTicker)
		{
			const double Now = FPlatformTime::Seconds();
			FTSTicker::GetCoreTicker().Tick(static_cast<float>(Now - LastTick));
			LastTick = Now;
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		}
		if (Done())
		{
			return true;
		}
		if (FPlatformTime::Seconds() >= Deadline)
		{
			return false;
		}
		FPlatformProcess::Sleep(0.001f);
	}
}

void VaRest::Http::Testing::FlushRedactionPipe()
{
	VaRest::Http::WaitForRedactionPipe();
}

FString VaRest::Http::Testing::GetTestServerUrl()
{
	FString Url;
	FParse::Value(FCommandLine::Get(), TEXT("VaRestTestServer="), Url);
	return Url;
}

FString VaRest::Http::Testing::GetTestServerTlsUrl()
{
	// "VaRestTestServerTls=" and "VaRestTestServer=" are not substrings of each other, so neither lookup sees the other
	FString Url;
	FParse::Value(FCommandLine::Get(), TEXT("VaRestTestServerTls="), Url);
	return Url;
}

VaRest::Http::Testing::FLogCapture::FLogCapture()
{
	GLog->AddOutputDevice(this);
}

VaRest::Http::Testing::FLogCapture::~FLogCapture()
{
	GLog->RemoveOutputDevice(this);
}

void VaRest::Http::Testing::FLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	FScopeLock ScopeLock(&Lock);
	Lines.Add(FString::Printf(TEXT("%s: %s"), *Category.ToString(), V));
}

bool VaRest::Http::Testing::FLogCapture::Contains(FStringView Text) const
{
	return Count(Text) > 0;
}

int32 VaRest::Http::Testing::FLogCapture::Count(FStringView Text) const
{
	FScopeLock ScopeLock(&Lock);
	int32 Found = 0;
	for (const FString& Line : Lines)
	{
		Found += UE::String::FindFirst(Line, Text, ESearchCase::CaseSensitive) != INDEX_NONE ? 1 : 0;
	}
	return Found;
}

TArray<FString> VaRest::Http::Testing::FLogCapture::GetLines() const
{
	FScopeLock ScopeLock(&Lock);
	return Lines;
}

VaRest::Http::Testing::FTestRig::FTestRig(FStringView BaseUrl)
	: Clock(MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>())
	, Fake(MakeShared<FVaRestFakeTransport, ESPMode::ThreadSafe>(Clock))
	, Client(FVaRestClient::Make(BaseUrl))
{
	// Logging is a test subject of its own (VaRest.Http.Log.*); elsewhere it would only add noise and warnings
	Client.WithTransport(Fake).WithClock(Clock).WithLogLevel(EVaRestLogLevel::None);
}

bool VaRest::Http::Testing::FTestRig::Drain(double TimeoutSeconds) const
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	// Monotonic counts, not "in flight": a task that starts and ends while the queue runs would leave an in-flight count at
	// zero with its result still queued
	const auto Started = [this]
	{
		return static_cast<int64>(Fake->NumCompletionsQueued()) + Counters().DecodeTasksLaunched.load();
	};
	const auto Settled = [this]
	{
		return Fake->NumCompletionsRun() == Fake->NumCompletionsQueued() && Counters().DecodeTasksHandedBack.load() == Counters().DecodeTasksLaunched.load();
	};
	for (;;)
	{
		// Each worker counts itself done after it queued its game-thread task, so once settled, all of that is queued
		if (!PumpUntil(Settled, FMath::Max(0.0, Deadline - FPlatformTime::Seconds())))
		{
			return false;
		}
		const int64 Before = Started();
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		if (Started() == Before)
		{
			return true;
		}
	}
}

uint64 VaRest::Http::Testing::RequestIdOf(const FVaRestTraceEvent& Event)
{
	switch (Event.Kind)
	{
	case EVaRestTraceKind::Started: return Event.Payload.Get<FVaRestTraceStarted>().RequestId;
	case EVaRestTraceKind::AttemptDispatched: return Event.Payload.Get<FVaRestTraceAttemptDispatched>().RequestId;
	case EVaRestTraceKind::AttemptEnded: return Event.Payload.Get<FVaRestTraceAttemptEnded>().RequestId;
	case EVaRestTraceKind::Note: return Event.Payload.Get<FVaRestTraceNote>().RequestId;
	case EVaRestTraceKind::Completed: return Event.Payload.Get<FVaRestTraceCompleted>().RequestId;
	case EVaRestTraceKind::Excerpt:
	{
		const FVaRestTraceExcerptEvent& Excerpt = Event.Payload.Get<FVaRestTraceExcerptEvent>();
		return Excerpt.Owner == EVaRestTraceExcerptOwner::Frame ? 0 : Excerpt.RequestId;
	}
	default: return 0;
	}
}

VaRest::Http::Testing::FTraceRecorder::FTraceRecorder(EVaRestTraceDetail InFlags)
{
	Detail.Flags = InFlags;
}

void VaRest::Http::Testing::FTraceRecorder::OnAttached(const FVaRestTraceSnapshot& Snapshot)
{
	FScopeLock Lock(&Mutex);
	Snapshots.Add(Snapshot);
}

void VaRest::Http::Testing::FTraceRecorder::OnEvent(const FVaRestTraceEvent& Event)
{
	if (Event.Kind != EVaRestTraceKind::Excerpt && !IsInGameThread())
	{
		bNonExcerptOffGameThread = true;
	}
	FScopeLock Lock(&Mutex);
	Events.Add(Event);
}

TArray<FVaRestTraceEvent> VaRest::Http::Testing::FTraceRecorder::For(uint64 RequestId) const
{
	FScopeLock Lock(&Mutex);
	return Events.FilterByPredicate([RequestId](const FVaRestTraceEvent& Event) { return RequestIdOf(Event) == RequestId; });
}

FString VaRest::Http::Testing::FTraceRecorder::KindsOf(uint64 RequestId) const
{
	TArray<FString> Names;
	for (const FVaRestTraceEvent& Event : For(RequestId))
	{
		switch (Event.Kind)
		{
		case EVaRestTraceKind::Started: Names.Add(TEXT("Started")); break;
		case EVaRestTraceKind::AttemptDispatched: Names.Add(TEXT("Dispatched")); break;
		case EVaRestTraceKind::AttemptEnded: Names.Add(TEXT("Ended")); break;
		case EVaRestTraceKind::Note: Names.Add(TEXT("Note")); break;
		case EVaRestTraceKind::Completed: Names.Add(TEXT("Completed")); break;
		default: break;
		}
	}
	return FString::Join(Names, TEXT(","));
}

TArray<FVaRestTraceExcerptEvent> VaRest::Http::Testing::FTraceRecorder::ExcerptsOf(uint64 RequestId) const
{
	TArray<FVaRestTraceExcerptEvent> Out;
	for (const FVaRestTraceEvent& Event : For(RequestId))
	{
		if (Event.Kind == EVaRestTraceKind::Excerpt)
		{
			Out.Add(Event.Payload.Get<FVaRestTraceExcerptEvent>());
		}
	}
	return Out;
}

TArray<FVaRestTraceMarker> VaRest::Http::Testing::FTraceRecorder::MarkersOf(uint64 ClientId) const
{
	FScopeLock Lock(&Mutex);
	TArray<FVaRestTraceMarker> Out;
	for (const FVaRestTraceEvent& Event : Events)
	{
		if (Event.Kind == EVaRestTraceKind::Marker && Event.Payload.Get<FVaRestTraceMarker>().ClientId == ClientId)
		{
			Out.Add(Event.Payload.Get<FVaRestTraceMarker>());
		}
	}
	return Out;
}

TArray<FVaRestTraceSnapshot> VaRest::Http::Testing::FTraceRecorder::GetSnapshots() const
{
	FScopeLock Lock(&Mutex);
	return Snapshots;
}

VaRest::Http::Testing::FScopedTraceRecorder::FScopedTraceRecorder(EVaRestTraceDetail Flags, int32 BodyCap)
	: Recorder(MakeShared<FTraceRecorder, ESPMode::ThreadSafe>(Flags))
{
	Recorder->Detail.BodyCap = BodyCap;
	FVaRestTrace::AddSink(Recorder);
}

VaRest::Http::Testing::FScopedTraceRecorder::~FScopedTraceRecorder()
{
	// Before Isolation's destructor restores the outer list (members die in reverse order)
	FVaRestTrace::RemoveSink(Recorder);
}
#endif // WITH_DEV_AUTOMATION_TESTS
