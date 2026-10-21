// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"
#include "Templates/Function.h"

#include <atomic>

#if !UE_BUILD_SHIPPING
namespace VaRest::Http::Testing
{
	/** Where decode steps ran (the decode-off-the-game-thread proof, Task 10). Not in Shipping (review M15). */
	struct FTestCounters
	{
		std::atomic<int32> DecodesOnGameThread{ 0 };
		std::atomic<int32> DecodesOffGameThread{ 0 };
		/** ~FClientState off the game thread (review I-1: it must not happen) */
		std::atomic<int32> ClientStatesDestroyedOffGameThread{ 0 };
	};
	VARESTHTTP_API FTestCounters& Counters();
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
/**
 * Helpers for VaRest's automation tests, and for other modules' (Inspector, WebSocket). Not for game code: compiled with
 * WITH_DEV_AUTOMATION_TESTS only (review M15).
 */
namespace VaRest::Http::Testing
{
	/**
	 * Game thread, from a test body. Runs game-thread tasks until Done() is true or TimeoutSeconds of real time pass;
	 * true when Done() became true. Automation runs tests from FEngineLoop::Tick outside any task
	 * (LaunchEngineLoop.cpp:6036-6045), so ProcessThreadUntilIdle's recursion guard holds (TaskGraph.cpp:700-708); never
	 * call this from inside a game-thread task or a ticker: it checks, because a nested core-ticker Tick trips
	 * Ticker.cpp:102 with a message that does not name the cause (review M-17). bTickCoreTicker also ticks
	 * FTSTicker::GetCoreTicker() (real clock timers, the engine's HTTP manager).
	 */
	VARESTHTTP_API bool PumpUntil(TFunctionRef<bool()> Done, double TimeoutSeconds = 5.0, bool bTickCoreTicker = false);

	/** Waits until the core redaction pipe is idle: body log lines are written there */
	VARESTHTTP_API void FlushRedactionPipe();

	/** -VaRestTestServer=<url>, which Tools/run_tests.sh passes; empty when absent */
	VARESTHTTP_API FString GetTestServerUrl();

	/** Collects every log line written while it lives, from any thread, as "Category: text" */
	class VARESTHTTP_API FLogCapture final : public FOutputDevice
	{
	public:
		FLogCapture();
		virtual ~FLogCapture() override;

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;
		// Without these the redirector buffers other threads' lines until the game thread flushes them (OutputDevice.h:201-212)
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		bool Contains(FStringView Text) const;
		int32 Count(FStringView Text) const;
		TArray<FString> GetLines() const;

	private:
		mutable FCriticalSection Lock;
		TArray<FString> Lines;
	};

	// Task 8 appends TTestOutcome, MakeOutcome, Observe and FTestRig here, inside this block
}
#endif // WITH_DEV_AUTOMATION_TESTS
