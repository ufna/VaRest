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
	if (UE::Tasks::FPipe* Pipe = VaRest::Http::GetRedactionPipe())
	{
		Pipe->WaitUntilEmpty();
	}
}

FString VaRest::Http::Testing::GetTestServerUrl()
{
	FString Url;
	FParse::Value(FCommandLine::Get(), TEXT("VaRestTestServer="), Url);
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
#endif // WITH_DEV_AUTOMATION_TESTS
