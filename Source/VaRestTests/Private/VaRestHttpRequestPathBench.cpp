// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "VaRestClient.h"
#include "VaRestHttpTesting.h"
#include "VaRestJsonObject.h"
#include "VaRestRequestJSON.h"
#include "VaRestTypes.h"

#include <atomic>

namespace VaRestTestsRequestPathBenchPrivate
{
	/** What one request cost, measured from outside: v1's paths are frozen, so nothing is hooked into it */
	struct FRun
	{
		/** From the start of the send step to the callback */
		double LatencyMs = 0.0;
		/**
		 * Game-thread work: the send step (building and sending the request), then the pump's core ticker (the HTTP manager,
		 * so v1's completion and parse) and game-thread tasks (v2's hop and callbacks)
		 */
		double GameThreadMs = 0.0;
		/** The longest single step (the send step or one pump step): the hitch a player would see in that frame */
		double LongestStepMs = 0.0;
		bool bOk = false;
	};

	/** Adds one step of game-thread work that started at StepStart */
	void AddStep(FRun& Run, double StepStart)
	{
		const double WorkMs = (FPlatformTime::Seconds() - StepStart) * 1000.0;
		Run.GameThreadMs += WorkMs;
		Run.LongestStepMs = FMath::Max(Run.LongestStepMs, WorkMs);
	}

	/** VaRest::Http::Testing::PumpUntil's loop, timing each step's work and leaving the sleeps out */
	void Pump(TFunctionRef<bool()> Done, FRun& Run, double TimeoutSeconds)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		double LastTick = FPlatformTime::Seconds();
		while (!Done() && FPlatformTime::Seconds() < Deadline)
		{
			const double StepStart = FPlatformTime::Seconds();
			FTSTicker::GetCoreTicker().Tick(static_cast<float>(StepStart - LastTick));
			LastTick = StepStart;
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			AddStep(Run, StepStart);
			FPlatformProcess::Sleep(0.0002f);
		}
	}

	/** v1 as a game uses it: UVaRestRequestJSON, completed on the game thread by the engine's default policy, parsed there */
	FRun RunV1(const FString& FullUrl, int64 ExpectedCount)
	{
		FRun Run;
		TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Finished = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
		const double Start = FPlatformTime::Seconds();
		UVaRestRequestJSON* Request = NewObject<UVaRestRequestJSON>();
		Request->SetVerb(EVaRestRequestVerb::GET);
		Request->SetContentType(EVaRestRequestContentType::json);
		Request->OnStaticRequestComplete.AddLambda([Finished](UVaRestRequestJSON*) { *Finished = 1; });
		Request->OnStaticRequestFail.AddLambda([Finished](UVaRestRequestJSON*) { *Finished = 2; });
		Request->ProcessURL(FullUrl);
		AddStep(Run, Start);
		Pump([Finished] { return Finished->load() != 0; }, Run, 120.0);
		Run.LatencyMs = (FPlatformTime::Seconds() - Start) * 1000.0;
		if (Finished->load() == 0)
		{
			// Every test drains what it sends: the cancel completes it through the same game-thread tick
			Request->Cancel();
			FRun Ignored;
			Pump([Finished] { return Finished->load() != 0; }, Ignored, 10.0);
			return Run;
		}
		// Checked after the clock stops: GetObjectField makes UObjects
		UVaRestJsonObject* Data = Finished->load() == 1 && Request->GetResponseObject() ? Request->GetResponseObject()->GetObjectField(TEXT("data")) : nullptr;
		Run.bOk = Data && Data->GetIntegerField(TEXT("count")) == ExpectedCount;
		return Run;
	}

	/** v2 as shipped: an FVaRestClient at its default log level, the body parsed zero-copy off the game thread */
	FRun RunV2(const FVaRestClient& Client, const FString& Path, int64 ExpectedCount)
	{
		FRun Run;
		auto Outcome = VaRest::Http::Testing::MakeOutcome<FVaRestJson>();
		const double Start = FPlatformTime::Seconds();
		const FVaRestRequestHandle Handle = VaRest::Http::Testing::Observe(Client.Get(Path), Outcome).Send();
		AddStep(Run, Start);
		Pump([Outcome] { return Outcome->IsDone(); }, Run, 120.0);
		Run.LatencyMs = (FPlatformTime::Seconds() - Start) * 1000.0;
		if (!Outcome->IsDone())
		{
			// Every test drains what it sends
			Handle.Cancel();
			FRun Ignored;
			Pump([Outcome] { return Outcome->IsDone(); }, Ignored, 10.0);
			return Run;
		}
		Run.bOk = Outcome->Value.IsSet() && Outcome->Value.GetValue().Path(TEXT("data.count")).AsInt64() == ExpectedCount;
		return Run;
	}

	double Median(TArray<double> Values)
	{
		Values.Sort();
		return Values.Num() == 0 ? 0.0 : Values[Values.Num() / 2];
	}

	struct FRow
	{
		FString Fixture;
		FString Label;
		TArray<FRun> Runs;

		double MedianOf(double FRun::*Field) const
		{
			TArray<double> Values;
			for (const FRun& Each : Runs)
			{
				Values.Add(Each.*Field);
			}
			return Median(MoveTemp(Values));
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRequestPathBench, "Bench.VaRest.Http.RequestPath",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::PerfFilter)

bool FVaRestHttpRequestPathBench::RunTest(const FString& Parameters)
{
	using namespace VaRestTestsRequestPathBenchPrivate;
	const FString BaseUrl = VaRest::Http::Testing::GetTestServerUrl();
	if (BaseUrl.IsEmpty() || !GEngine)
	{
		AddError(TEXT("no test server: run Tools/run_tests.sh Bench.VaRest.Http, which starts Tools/testserver/server.py with the fixtures"));
		return false;
	}
	const FVaRestClient Client = FVaRestClient::Make(BaseUrl);
	TArray<FRow> Rows;
	struct FFixture
	{
		const TCHAR* Name;
		int32 Runs;
	};
	const FFixture Fixtures[] = { { TEXT("bench_1mb.json"), 9 }, { TEXT("bench_10mb.json"), 5 } };
	for (const FFixture& Fixture : Fixtures)
	{
		const FString LocalPath = FPaths::ProjectSavedDir() / TEXT("VaRestBench") / Fixture.Name;
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *LocalPath))
		{
			AddError(FString::Printf(TEXT("missing %s -- run: python3 Tools/gen_bench_json.py <Host>/Saved/VaRestBench"), *LocalPath));
			return false;
		}
		// The count the parse must reproduce, read once outside any timing
		TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(TConstArrayView<uint8>(Bytes));
		const int64 ExpectedCount = Parsed.HasValue() ? Parsed.GetValue().Path(TEXT("data.count")).AsInt64(-1) : -1;
		if (!TestTrue(FString::Printf(TEXT("%s has data.count"), Fixture.Name), ExpectedCount > 0))
		{
			return false;
		}
		const FString Path = FString::Printf(TEXT("/fixture/%s"), Fixture.Name);
		const FString FullUrl = BaseUrl + Path;

		// Warm-up, untimed: connections, the server's file cache, first-use allocations
		RunV1(FullUrl, ExpectedCount);
		RunV2(Client, Path, ExpectedCount);

		FRow V1Shipped;
		V1Shipped.Fixture = Fixture.Name;
		V1Shipped.Label = TEXT("v1 UVaRestRequestJSON, as shipped (logs the body on desktop)");
		FRow V1Quiet;
		V1Quiet.Fixture = Fixture.Name;
		V1Quiet.Label = TEXT("v1 UVaRestRequestJSON, LogVaRest off");
		FRow V2;
		V2.Fixture = Fixture.Name;
		V2.Label = TEXT("v2 FVaRestClient -> FVaRestJson (Basic log)");
		for (int32 Index = 0; Index < Fixture.Runs; ++Index)
		{
			// Interleaved, so drift in the machine's state spreads over all three
			V1Shipped.Runs.Add(RunV1(FullUrl, ExpectedCount));
			// The "Log" console command (LogSuppressionInterface.cpp:589-591), reached through UEngine::Exec (UnrealEngine.cpp:5533)
			GEngine->Exec(nullptr, TEXT("Log LogVaRest Off"));
			V1Quiet.Runs.Add(RunV1(FullUrl, ExpectedCount));
			GEngine->Exec(nullptr, TEXT("Log LogVaRest Log"));
			V2.Runs.Add(RunV2(Client, Path, ExpectedCount));
		}
		Rows.Add(MoveTemp(V1Shipped));
		Rows.Add(MoveTemp(V1Quiet));
		Rows.Add(MoveTemp(V2));
	}

	FString Report = FString::Printf(TEXT("# Request path: v1 vs v2 (%s)\n\nMedian of the runs; ms of wall time.\n\n"), *FDateTime::UtcNow().ToIso8601());
	Report += TEXT("| fixture | path | runs | latency ms | game-thread ms | longest step ms |\n|---|---|---|---|---|---|\n");
	bool bAllOk = true;
	for (const FRow& Row : Rows)
	{
		for (const FRun& Each : Row.Runs)
		{
			bAllOk &= Each.bOk;
		}
		Report += FString::Printf(TEXT("| %s | %s | %d | %.2f | %.2f | %.2f |\n"), *Row.Fixture, *Row.Label, Row.Runs.Num(),
			Row.MedianOf(&FRun::LatencyMs), Row.MedianOf(&FRun::GameThreadMs), Row.MedianOf(&FRun::LongestStepMs));
	}
	AddInfo(Report);
	FFileHelper::SaveStringToFile(Report, *(FPaths::ProjectSavedDir() / TEXT("VaRestBench") / TEXT("request-path-results.md")));
	TestTrue(TEXT("every run parsed the fixture whole (data.count matches)"), bAllOk);
	// The threading rule's point (spec 1): v1 parses on the game thread, v2 only moves the result there
	const FRow* V1Big = Rows.FindByPredicate([](const FRow& Row) { return Row.Fixture == TEXT("bench_10mb.json") && Row.Label.Contains(TEXT("LogVaRest off")); });
	const FRow* V2Big = Rows.FindByPredicate([](const FRow& Row) { return Row.Fixture == TEXT("bench_10mb.json") && Row.Label.StartsWith(TEXT("v2")); });
	TestTrue(TEXT("10 MiB: v2 costs the game thread less than v1 even with v1's logging off"), V1Big && V2Big
		&& V2Big->MedianOf(&FRun::GameThreadMs) < V1Big->MedianOf(&FRun::GameThreadMs));
	return true;
}

#endif
