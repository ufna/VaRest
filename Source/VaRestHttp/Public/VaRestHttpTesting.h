// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"
#include "Templates/Function.h"
#include "VaRestClient.h"
#include "VaRestFakeTransport.h"

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
		/**
		 * ~FVaRestExchange off the game thread: the pipeline moves its references to the game thread (review I-1), so only a
		 * pin taken on another thread (IsActive, Cancel) may make this count; a request sent and completed without one never does
		 */
		std::atomic<int32> ExchangesDestroyedOffGameThread{ 0 };
		/** Bytes body logging copied on the game thread (task 14 review M7: at most the cap per body, Inspector 7) */
		std::atomic<int64> LogBodyBytesCopied{ 0 };
		/**
		 * Decode tasks started (game thread, as the task is launched) and those that have queued their result for the game
		 * thread (counted last, on the worker): equal when no decode is in flight (FTestRig::Drain, final review M10)
		 */
		std::atomic<int32> DecodeTasksLaunched{ 0 };
		std::atomic<int32> DecodeTasksHandedBack{ 0 };
		/** OnHttpThread() deliveries launched on a worker (game thread, at the launch): a silent request launches none (final review M11) */
		std::atomic<int32> OffGameThreadDeliveries{ 0 };
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
	 * call this from inside a game-thread task or from a callback it runs: it checks both. It cannot see that it is inside
	 * a core-ticker callback the engine ticked; there a nested Tick dereferences slots the outer Tick moved out
	 * (Ticker.cpp:117, :128), so do not (review M-17, M-9). bTickCoreTicker also ticks
	 * FTSTicker::GetCoreTicker() (real clock timers, the engine's HTTP manager).
	 */
	VARESTHTTP_API bool PumpUntil(TFunctionRef<bool()> Done, double TimeoutSeconds = 5.0, bool bTickCoreTicker = false);

	/** Waits until the core redaction pipe is idle: body log lines are written there */
	VARESTHTTP_API void FlushRedactionPipe();

	/** -VaRestTestServer=<url>, which Tools/run_tests.sh passes; empty when absent */
	VARESTHTTP_API FString GetTestServerUrl();

	/** -VaRestTestServerTls=<url>, the test server's TLS listener (WebSocket W0), which Tools/run_tests.sh passes; empty when absent */
	VARESTHTTP_API FString GetTestServerTlsUrl();

	/** Collects every log line written while it lives, from any thread, as "Category: text" */
	class VARESTHTTP_API FLogCapture final : public FOutputDevice
	{
	public:
		FLogCapture();
		virtual ~FLogCapture() override;

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;
		// Without CanBeUsedOnMultipleThreads the redirector buffers other threads' lines until the game thread flushes them (OutputDeviceRedirector.cpp:440)
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		bool Contains(FStringView Text) const;
		int32 Count(FStringView Text) const;
		TArray<FString> GetLines() const;

	private:
		mutable FCriticalSection Lock;
		TArray<FString> Lines;
	};

	/** What a request's callbacks did; shared, so a test that times out leaves no dangling reference */
	template <typename T>
	struct TTestOutcome
	{
		std::atomic<int32> SuccessCalls{ 0 };
		std::atomic<int32> ErrorCalls{ 0 };
		std::atomic<int32> CompleteCalls{ 0 };
		std::atomic<bool> bAllOnGameThread{ true };
		TOptional<T> Value;
		FVaRestError Error;
		FVaRestResponse Response;

		bool IsDone() const { return CompleteCalls.load() > 0; }
	};

	template <>
	struct TTestOutcome<void>
	{
		std::atomic<int32> SuccessCalls{ 0 };
		std::atomic<int32> ErrorCalls{ 0 };
		std::atomic<int32> CompleteCalls{ 0 };
		std::atomic<bool> bAllOnGameThread{ true };
		FVaRestError Error;
		FVaRestResponse Response;

		bool IsDone() const { return CompleteCalls.load() > 0; }
	};

	template <typename T>
	TSharedRef<TTestOutcome<T>, ESPMode::ThreadSafe> MakeOutcome()
	{
		return MakeShared<TTestOutcome<T>, ESPMode::ThreadSafe>();
	}

	/** Wires OnSuccess, OnError and OnComplete into Outcome. OnComplete is counted last, after the value and error are stored. */
	template <typename T>
	TVaRestRequest<T> Observe(TVaRestRequest<T>&& Request, const TSharedRef<TTestOutcome<T>, ESPMode::ThreadSafe>& Outcome)
	{
		auto Tracked = [Outcome](const FVaRestError& Error)
		{
			Outcome->bAllOnGameThread = Outcome->bAllOnGameThread.load() && IsInGameThread();
			Outcome->Error = Error;
			++Outcome->ErrorCalls;
		};
		auto Completed = [Outcome](const TVaRestResult<T>& Result)
		{
			Outcome->bAllOnGameThread = Outcome->bAllOnGameThread.load() && IsInGameThread();
			Outcome->Response = Result.Response;
			++Outcome->CompleteCalls;
		};
		if constexpr (std::is_void_v<T>)
		{
			return MoveTemp(Request)
				.OnSuccess([Outcome](const FVaRestResponse&)
				{
					Outcome->bAllOnGameThread = Outcome->bAllOnGameThread.load() && IsInGameThread();
					++Outcome->SuccessCalls;
				})
				.OnError(MoveTemp(Tracked))
				.OnComplete(MoveTemp(Completed));
		}
		else
		{
			return MoveTemp(Request)
				.OnSuccess([Outcome](const T& Result, const FVaRestResponse&)
				{
					Outcome->bAllOnGameThread = Outcome->bAllOnGameThread.load() && IsInGameThread();
					Outcome->Value = Result;
					++Outcome->SuccessCalls;
				})
				.OnError(MoveTemp(Tracked))
				.OnComplete(MoveTemp(Completed));
		}
	}

	/** A client on the fake transport and the virtual clock (spec 7.1). Game thread. */
	struct VARESTHTTP_API FTestRig
	{
		explicit FTestRig(FStringView BaseUrl = TEXTVIEW("https://api.test"));

		TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock;
		TSharedRef<FVaRestFakeTransport, ESPMode::ThreadSafe> Fake;
		FVaRestClient Client;

		template <typename T>
		bool Wait(const TSharedRef<TTestOutcome<T>, ESPMode::ThreadSafe>& Outcome, double TimeoutSeconds = 5.0) const
		{
			return PumpUntil([&Outcome] { return Outcome->IsDone(); }, TimeoutSeconds);
		}

		/**
		 * Game thread, from a test body (final review M10): pumps until this rig's work has settled -- every completion the
		 * fake queued for a worker has run there, every decode task has queued its result for the game thread, and the
		 * game-thread tasks those queued have run without starting more. Deterministic, unlike a fixed wait: what a "nothing
		 * else arrives" check needs. It does not see OnHttpThread() callbacks on a worker, timers on the virtual clock, or a
		 * request sent from another thread whose Enter task is not queued yet. False after TimeoutSeconds.
		 */
		bool Drain(double TimeoutSeconds = 5.0) const;
	};
}
#endif // WITH_DEV_AUTOMATION_TESTS
