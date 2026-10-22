// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Tasks/Task.h"
#include "UObject/Package.h"
#include "VaRestAuth.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpStatsInternal.h"
#include "VaRestHttpTesting.h"
#include "VaRestRetry.h"

// A tag for the code that sends: the callbacks must run under it, not under VaRest's (VaRest.Http.Stats.LlmTags)
LLM_DEFINE_TAG(VaRestTests_Sender);

namespace VaRest::Http::StatsTestPrivate
{
	FVaRestFakeResponse Json(int32 Code, const char* Text = "{}")
	{
		return FVaRestFakeResponse::Json(Code, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	/** Drains a rig whose test stopped early */
	void Abandon(VaRest::Http::Testing::FTestRig& Rig)
	{
		Rig.Client.CancelAll();
		VaRest::Http::Testing::PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	}

	/** Spec 4.1's policy with InMaxRetries retries and no jitter: each delay exact */
	FVaRestRetryPolicy ExactRetry(int32 InMaxRetries)
	{
		FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(InMaxRetries);
		Policy.Backoff.Jitter = EVaRestJitter::None;
		return Policy;
	}

	/** The rig's client's own counters: exact, unlike the process-wide ones every test shares (concurrency review M-13) */
	const VaRest::Http::FHttpCounters& CountersOf(const VaRest::Http::Testing::FTestRig& Rig)
	{
		return VaRest::Http::FClientAccess::State(Rig.Client)->GetCounters();
	}

	/** An auth whose refresh waits until the test answers it: a request parks behind it */
	class FHeldRefreshAuth final : public IVaRestAuth
	{
	public:
		virtual FName GetAuthName() const override { return TEXT("HeldRefresh"); }
		virtual bool Apply(FVaRestAuthTarget& Target) const override { return Target.Session.HasAccessToken(); }
		virtual bool CanRefresh(const FVaRestSessionData&) const override { return true; }
		virtual bool NeedsRefresh(const FVaRestSessionData& Session, double) const override { return !Session.HasAccessToken(); }
		virtual void Refresh(const FVaRestAuthRefreshContext&, TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done) override
		{
			Pending = MoveTemp(Done);
		}

		bool IsPending() const { return static_cast<bool>(Pending); }

		void Answer(FVaRestAuthRefreshResult&& Result)
		{
			if (Pending)
			{
				TUniqueFunction<void(FVaRestAuthRefreshResult&&)> Done = MoveTemp(Pending);
				Pending.Reset();
				Done(MoveTemp(Result));
			}
		}

	private:
		TUniqueFunction<void(FVaRestAuthRefreshResult&&)> Pending;
	};

	/** Records every exchange its PerAttempt slot sees */
	class FExchangeGrab final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("ExchangeGrab"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Seen.Add(Exchange);
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}

		TArray<TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe>> Seen;
	};

#if ENABLE_LOW_LEVEL_MEM_TRACKER
	/** The calling thread's active LLM tag (the default tag set) */
	FName ActiveLlmTag()
	{
		const UE::LLMPrivate::FTagData* Data = FLowLevelMemTracker::Get().GetActiveTagData(ELLMTracker::Default);
		return Data ? FLowLevelMemTracker::Get().GetTagUniqueName(Data) : NAME_None;
	}

	/** The tag each named place ran under; any thread (an OnHttpThread() callback runs on a worker) */
	struct FTagLog
	{
		void Note(const TCHAR* Where)
		{
			const FName Tag = ActiveLlmTag();
			FScopeLock Locked(&Lock);
			Seen.Add(FString(Where), Tag);
		}

		bool Has(const TCHAR* Where) const
		{
			FScopeLock Locked(&Lock);
			return Seen.Contains(FString(Where));
		}

		FName Get(const TCHAR* Where) const
		{
			FScopeLock Locked(&Lock);
			const FName* Found = Seen.Find(FString(Where));
			return Found ? *Found : NAME_None;
		}

	private:
		mutable FCriticalSection Lock;
		TMap<FString, FName> Seen;
	};
	using FTagLogRef = TSharedRef<FTagLog, ESPMode::ThreadSafe>;

	/** Notes the tag a PerAttempt stage runs under: VaRest's pipeline */
	class FTagStage final : public IVaRestStage
	{
	public:
		explicit FTagStage(FTagLogRef InLog)
			: Log(MoveTemp(InLog))
		{
		}
		virtual FName GetStageName() const override { return TEXT("TagStage"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Log->Note(TEXT("stage"));
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}

	private:
		FTagLogRef Log;
	};

	/** Notes the tag a Started event is emitted under: the trace's */
	class FTagSink final : public IVaRestTraceSink
	{
	public:
		explicit FTagSink(FTagLogRef InLog)
			: Log(MoveTemp(InLog))
		{
		}
		virtual FVaRestTraceDetailSettings GetDetail() const override
		{
			FVaRestTraceDetailSettings Out;
			Out.Flags = EVaRestTraceDetail::Metadata;
			return Out;
		}
		virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
		virtual void OnEvent(const FVaRestTraceEvent& Event) override
		{
			if (Event.Kind == EVaRestTraceKind::Started)
			{
				Log->Note(TEXT("trace"));
			}
		}

	private:
		FTagLogRef Log;
	};
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpStatsCountersTest, "VaRest.Http.Stats.Counters",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpStatsCountersTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(1);
	Policy.Backoff.Jitter = EVaRestJitter::None;
	Rig.Client.WithRetry(Policy);
	Rig.Fake->Enqueue(FVaRestFakeResponse::Json(503, FUtf8StringView(UTF8TEXT("{}"))));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{\"a\":1}"))));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{}"))));
	// Concurrency review M-13: this client's own counters, exact -- a request of an earlier test that ends during this one
	// moves only the process-wide ones, which are checked to grow, not compared
	const VaRest::Http::FHttpCounters& Mine = VaRest::Http::FClientAccess::State(Rig.Client)->GetCounters();
	const VaRest::Http::FHttpCounters& Global = VaRest::Http::FHttpStats::Global();
	const int64 GlobalRequests = Global.Requests.load();
	const int64 GlobalDown = Global.BytesDown.load();
	auto Retried = MakeOutcome<FVaRestJson>();
	auto Posted = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/r")), Retried).Send();
	if (!TestTrue(TEXT("backing off"), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
	{
		Rig.Client.CancelAll();
		PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
		return false;
	}
	TestEqual(TEXT("in flight while it waits"), Mine.InFlight.load(), static_cast<int64>(1));
	Observe(Rig.Client.Post(TEXT("/p")).Body(TEXT("12345")), Posted).Send();
	// A silent request (its owner is gone at entry): counted in and out like any other
	UObject* Gone = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	Gone->MarkAsGarbage();
	auto Silent = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/gone")).Owner(Gone), Silent).Send();
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("both complete"), PumpUntil([Retried, Posted] { return Retried->IsDone() && Posted->IsDone(); })))
	{
		// Spec review M-5: drained before the early return
		Rig.Client.CancelAll();
		PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
		return false;
	}
	PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	Rig.Drain();
	TestEqual(TEXT("three requests (a retry is not a request)"), Mine.Requests.load(), static_cast<int64>(3));
	TestEqual(TEXT("one retry"), Mine.Retries.load(), static_cast<int64>(1));
	TestEqual(TEXT("none in flight after"), Mine.InFlight.load(), static_cast<int64>(0));
	TestEqual(TEXT("bytes down: every attempt's body (2 + 7 + 2)"), Mine.BytesDown.load(), static_cast<int64>(11));
	TestEqual(TEXT("bytes up: the POST body"), Mine.BytesUp.load(), static_cast<int64>(5));
	TestTrue(TEXT("stat VaRest's process-wide counters grew by at least as much"), Global.Requests.load() - GlobalRequests >= 3 && Global.BytesDown.load() - GlobalDown >= 11);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpStatsEveryEndTest, "VaRest.Http.Stats.EveryEndCountsOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpStatsEveryEndTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::StatsTestPrivate;
	// Spec 4.8: InFlight goes up once at Enter and down once at the request's end, whatever ends it. One slot, so a second
	// request waits in the limiter's queue.
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.Contains(TEXT("/held/")))
		{
			return FVaRestFakeResponse::Manual();
		}
		return Request.Url.EndsWith(TEXT("/busy")) ? Json(503) : Json(200);
	});
	const VaRest::Http::FHttpCounters& Mine = CountersOf(Rig);
	int64 Sent = 0;

	// A cancel while the attempt is at the transport
	{
		auto Outcome = MakeOutcome<FVaRestJson>();
		const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/held/cancel")), Outcome).Send();
		++Sent;
		if (!TestTrue(TEXT("held at the transport"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
		{
			Abandon(Rig);
			return false;
		}
		TestEqual(TEXT("in flight at the transport"), Mine.InFlight.load(), static_cast<int64>(1));
		Handle.Cancel();
		TestTrue(TEXT("Cancel ends it"), Rig.Wait(Outcome) && Outcome->Error.Kind == EVaRestErrorKind::Canceled);
		TestEqual(TEXT("... and counts it out"), Mine.InFlight.load(), static_cast<int64>(0));
	}

	// CancelAll: one at the transport, one in the limiter's queue (its abort)
	{
		const int32 CallsBefore = Rig.Fake->NumCalls();
		auto First = MakeOutcome<FVaRestJson>();
		auto Queued = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/held/first")), First).Send();
		Observe(Rig.Client.Get(TEXT("/held/queued")), Queued).Send();
		Sent += 2;
		if (!TestTrue(TEXT("the first at the transport"), PumpUntil([&Rig, CallsBefore] { return Rig.Fake->NumCalls() == CallsBefore + 1; })))
		{
			Abandon(Rig);
			return false;
		}
		TestEqual(TEXT("both in flight, one of them queued"), Mine.InFlight.load(), static_cast<int64>(2));
		Rig.Client.CancelAll();
		TestTrue(TEXT("CancelAll ends both"), PumpUntil([First, Queued] { return First->IsDone() && Queued->IsDone(); })
			&& First->Error.Kind == EVaRestErrorKind::Canceled && Queued->Error.Kind == EVaRestErrorKind::Canceled);
		TestEqual(TEXT("... the queued one never reached the transport"), Rig.Fake->NumCalls(), CallsBefore + 1);
		TestEqual(TEXT("... and counts both out"), Mine.InFlight.load(), static_cast<int64>(0));
	}

	// A configuration error before the chain
	{
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/{unclosed")), Outcome).Send();
		++Sent;
		TestTrue(TEXT("a configuration error ends it"), Rig.Wait(Outcome) && Outcome->Error.Kind == EVaRestErrorKind::Config);
		TestEqual(TEXT("... and counts it out"), Mine.InFlight.load(), static_cast<int64>(0));
	}

	// Canceled on another thread before its Enter task ran: Enter finishes it at once
	{
		const int32 CallsBefore = Rig.Fake->NumCalls();
		auto Outcome = MakeOutcome<FVaRestJson>();
		const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bSent = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
		FVaRestClient Client = Rig.Client;
		UE::Tasks::Launch(TEXT("VaRestTests.SendAndCancel"), [Client, Outcome, bSent]
		{
			const FVaRestRequestHandle Handle = Observe(Client.Get(TEXT("/early")), Outcome).Send();
			Handle.Cancel();
			*bSent = true;
		});
		// Not FTask::Wait, which may run the task here, on the game thread; and no game-thread task runs meanwhile, so its
		// Enter task runs after the cancel flag is set
		const double Until = FPlatformTime::Seconds() + 5.0;
		while (!bSent->load() && FPlatformTime::Seconds() < Until)
		{
			FPlatformProcess::Sleep(0.001f);
		}
		++Sent;
		TestTrue(TEXT("sent and canceled on a worker"), bSent->load());
		TestTrue(TEXT("canceled before the hop"), Rig.Wait(Outcome) && Outcome->Error.Kind == EVaRestErrorKind::Canceled);
		Rig.Drain();
		TestEqual(TEXT("... never sent"), Rig.Fake->NumCalls(), CallsBefore);
		TestEqual(TEXT("... and counted out"), Mine.InFlight.load(), static_cast<int64>(0));
	}

	// The owner gone while the attempt is at the transport: a silent end
	{
		const int32 CallsBefore = Rig.Fake->NumCalls();
		UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/held/owner")).Owner(Owner), Outcome).Send();
		++Sent;
		if (!TestTrue(TEXT("the owned one at the transport"), PumpUntil([&Rig, CallsBefore] { return Rig.Fake->NumCalls() == CallsBefore + 1; })))
		{
			Abandon(Rig);
			return false;
		}
		Owner->MarkAsGarbage();
		Rig.Fake->Complete(CallsBefore, Json(200));
		TestTrue(TEXT("its owner gone, it ends"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }) && Rig.Drain());
		TestEqual(TEXT("... silently"), Outcome->CompleteCalls.load(), 0);
		TestEqual(TEXT("... and is counted out"), Mine.InFlight.load(), static_cast<int64>(0));
	}

	// A cancel during a backoff: the retry was scheduled, never re-issued
	{
		const int32 CallsBefore = Rig.Fake->NumCalls();
		auto Outcome = MakeOutcome<FVaRestJson>();
		const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/busy")).Retry(ExactRetry(1)), Outcome).Send();
		++Sent;
		if (!TestTrue(TEXT("backing off after a 503"), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
		{
			Abandon(Rig);
			return false;
		}
		TestEqual(TEXT("in flight while it backs off"), Mine.InFlight.load(), static_cast<int64>(1));
		Handle.Cancel();
		TestTrue(TEXT("Cancel ends the backoff"), Rig.Wait(Outcome) && Outcome->Error.Kind == EVaRestErrorKind::Canceled);
		Rig.Clock->Advance(5.0);
		Rig.Drain();
		TestEqual(TEXT("... counted out"), Mine.InFlight.load(), static_cast<int64>(0));
		TestEqual(TEXT("... with no retry: none was re-issued"), Mine.Retries.load(), static_cast<int64>(0));
		TestEqual(TEXT("... and no second attempt"), Rig.Fake->NumCalls(), CallsBefore + 1);
	}

	// The retry deadline while queued behind the slot: the limiter's abort answers Deadline
	{
		const int32 CallsBefore = Rig.Fake->NumCalls();
		auto Holder = MakeOutcome<FVaRestJson>();
		auto Late = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/held/slot")), Holder).Send();
		Observe(Rig.Client.Get(TEXT("/held/late")).Retry(ExactRetry(1).WithDeadline(1.f)), Late).Send();
		Sent += 2;
		if (!TestTrue(TEXT("the holder at the transport"), PumpUntil([&Rig, CallsBefore] { return Rig.Fake->NumCalls() == CallsBefore + 1; })))
		{
			Abandon(Rig);
			return false;
		}
		Rig.Clock->Advance(1.0);
		TestTrue(TEXT("the queued one ends at its deadline"), Rig.Wait(Late) && Late->Error.Kind == EVaRestErrorKind::Timeout);
		TestEqual(TEXT("... and is counted out"), Mine.InFlight.load(), static_cast<int64>(1));
		Rig.Fake->Complete(CallsBefore, Json(200));
		TestTrue(TEXT("the holder completes"), Rig.Wait(Holder) && Holder->SuccessCalls.load() == 1);
		TestEqual(TEXT("... and is counted out"), Mine.InFlight.load(), static_cast<int64>(0));
	}

	// A park behind a refresh, canceled: the park's end
	{
		const TSharedRef<FHeldRefreshAuth, ESPMode::ThreadSafe> Auth = MakeShared<FHeldRefreshAuth, ESPMode::ThreadSafe>();
		Rig.Client.WithAuth(Auth);
		const int32 CallsBefore = Rig.Fake->NumCalls();
		auto Outcome = MakeOutcome<FVaRestJson>();
		const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/parked")), Outcome).Send();
		++Sent;
		if (!TestTrue(TEXT("parked behind the refresh"), PumpUntil([&Auth] { return Auth->IsPending(); })))
		{
			Abandon(Rig);
			return false;
		}
		TestEqual(TEXT("in flight while parked"), Mine.InFlight.load(), static_cast<int64>(1));
		Handle.Cancel();
		TestTrue(TEXT("Cancel ends the park"), Rig.Wait(Outcome) && Outcome->Error.Kind == EVaRestErrorKind::Canceled);
		TestEqual(TEXT("... and counts it out"), Mine.InFlight.load(), static_cast<int64>(0));
		TestEqual(TEXT("... never sent"), Rig.Fake->NumCalls(), CallsBefore);
		Auth->Answer(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Connection, TEXT("the test ends the refresh")));
		Rig.Drain();
	}

	TestEqual(TEXT("one request per Send()"), Mine.Requests.load(), Sent);
	TestEqual(TEXT("none left in flight"), Mine.InFlight.load(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpStatsRetriesTest, "VaRest.Http.Stats.RetriesAndBytesPerAttempt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpStatsRetriesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::StatsTestPrivate;
	// A request retried three times is one request and three retries; each attempt's bytes count, the body each time it goes
	FTestRig Rig;
	Rig.Client.WithRetry(ExactRetry(3));
	Rig.Fake->Enqueue(Json(503));
	Rig.Fake->Enqueue(Json(503));
	Rig.Fake->Enqueue(Json(503));
	Rig.Fake->Enqueue(Json(200, R"({"a":1})"));
	const VaRest::Http::FHttpCounters& Mine = CountersOf(Rig);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Put(TEXT("/put")).Body(TEXT("12345")), Outcome).Send();
	for (int32 Backoff = 1; Backoff <= 3; ++Backoff)
	{
		if (!TestTrue(*FString::Printf(TEXT("backoff %d"), Backoff), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
		{
			Abandon(Rig);
			return false;
		}
		TestEqual(*FString::Printf(TEXT("retries before backoff %d fires"), Backoff), Mine.Retries.load(), static_cast<int64>(Backoff - 1));
		Rig.Clock->Advance(4.0);
	}
	if (!TestTrue(TEXT("the fourth attempt succeeds"), Rig.Wait(Outcome) && Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 4))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestEqual(TEXT("one request"), Mine.Requests.load(), static_cast<int64>(1));
	TestEqual(TEXT("three retries"), Mine.Retries.load(), static_cast<int64>(3));
	TestEqual(TEXT("bytes up: the body, once per attempt"), Mine.BytesUp.load(), static_cast<int64>(4 * 5));
	TestEqual(TEXT("bytes down: every attempt's body (3 x 2 + 7)"), Mine.BytesDown.load(), static_cast<int64>(13));
	TestEqual(TEXT("none in flight"), Mine.InFlight.load(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpStatsRefreshTest, "VaRest.Http.Stats.RefreshIsARequest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpStatsRefreshTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::StatsTestPrivate;
	// A 401, the refresh request, the replay: two requests -- the refresh is one -- and no retry (plan decision 2: only
	// Retry's re-issues are retries), though the client retries
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig())).WithRetry(ExactRetry(2));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.Contains(TEXT("/auth/")))
		{
			return Json(200, R"({"accessToken":"new-access-123456","refreshToken":"new-refresh-123456","expiresIn":60})");
		}
		return Request.Headers.Get(TEXT("Authorization")) == TEXT("Bearer new-access-123456") ? Json(200, R"({"ok":true})") : Json(401);
	});
	const VaRest::Http::FHttpCounters& Mine = CountersOf(Rig);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
	if (!TestTrue(TEXT("replayed after the refresh"), Rig.Wait(Outcome) && Outcome->SuccessCalls.load() == 1) || !TestEqual(TEXT("three calls"), Rig.Fake->NumCalls(), 3))
	{
		Abandon(Rig);
		return false;
	}
	PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	Rig.Drain();
	TestEqual(TEXT("two requests: the refresh request is one"), Mine.Requests.load(), static_cast<int64>(2));
	TestEqual(TEXT("a replay is not a retry"), Mine.Retries.load(), static_cast<int64>(0));
	TestEqual(TEXT("none in flight"), Mine.InFlight.load(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpStatsEmptyClientTest, "VaRest.Http.Stats.EmptyClientCountsNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpStatsEmptyClientTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Final review M7's builder: no client state, so no pipeline and no request in stat VaRest -- nothing to count out
	AddExpectedErrorPlain(TEXT("Send() on an empty FVaRestClient"), EAutomationExpectedErrorFlags::Contains, 1);
	const VaRest::Http::FHttpCounters& Global = VaRest::Http::FHttpStats::Global();
	FVaRestClient Empty;
	auto Outcome = MakeOutcome<FVaRestJson>();
	// Exact across this span: only the game thread writes these two (Enter, Complete), and no game-thread task runs in it
	const int64 Requests = Global.Requests.load();
	const int64 InFlight = Global.InFlight.load();
	Observe(Empty.Get(TEXT("/nowhere")), Outcome).Send();
	TestEqual(TEXT("no request counted"), Global.Requests.load() - Requests, static_cast<int64>(0));
	TestEqual(TEXT("none in flight"), Global.InFlight.load() - InFlight, static_cast<int64>(0));
	TestTrue(TEXT("its callbacks still get Kind=Config"), PumpUntil([Outcome] { return Outcome->IsDone(); }) && Outcome->Error.Kind == EVaRestErrorKind::Config);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpStatsSameIdTest, "VaRest.Http.Stats.SameIdCountsEach",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpStatsSameIdTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::StatsTestPrivate;
#if DO_ENSURE
	// As VaRest.Http.Auth.RefreshIdInUseEnsures: ensureAlways reports every time only while core.EnsureAlwaysEnabled is on
	// (AssertionMacros.cpp:136-140, :948) and every ensure is handled at HandleEnsurePercent 100
	// (GenericPlatformMisc.cpp:2058-2068, :2070-2094)
	{
		IConsoleVariable* Always = IConsoleManager::Get().FindConsoleVariable(TEXT("core.EnsureAlwaysEnabled"));
		float Percent = 100.f;
		if (!GConfig || !GConfig->GetFloat(TEXT("Core.System"), TEXT("HandleEnsurePercent"), Percent, GEngineIni))
		{
			FParse::Value(FCommandLine::Get(), TEXT("handleensurepercent="), Percent);
		}
		if (!Always || !Always->GetBool() || Percent < 100.f)
		{
			AddInfo(TEXT("core.EnsureAlwaysEnabled is off or HandleEnsurePercent is below 100: the ensure cannot be taken here; nothing to check"));
			return true;
		}
	}
	// Two exchanges under one id (Enter ensures; Task 16 review M-7): the second replaces the first's map entry, and both
	// are requests in flight -- the first counts out though its entry is not its own any more
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Fired = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	const TFunction<bool(const FEnsureHandlerArgs&)> Outer = GetEnsureHandler();
	SetEnsureHandler([Fired, Outer](const FEnsureHandlerArgs& Args)
	{
		if (Args.Message && FCString::Strstr(Args.Message, TEXT("is already in flight on this client")))
		{
			++*Fired;
			return true;
		}
		return Outer ? Outer(Args) : false;
	});
	ON_SCOPE_EXIT { SetEnsureHandler(Outer); };
	FTestRig Rig;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Url.EndsWith(TEXT("/auth/first")) ? FVaRestFakeResponse::Manual() : Json(200); });
	const VaRest::Http::FHttpCounters& Mine = CountersOf(Rig);
	const uint64 RefreshId = FVaRestTrace::NewRequestId();
	const FVaRestAuthRefreshContext First = VaRest::Http::FClientAccess::MakeRefreshContext(Rig.Client, FVaRestSessionData(), RefreshId);
	const FVaRestAuthRefreshContext Second = VaRest::Http::FClientAccess::MakeRefreshContext(Rig.Client, FVaRestSessionData(), RefreshId);
	auto FirstOutcome = MakeOutcome<FVaRestJson>();
	auto SecondOutcome = MakeOutcome<FVaRestJson>();
	Observe(First.Request(TEXT("POST"), TEXT("/auth/first")).Body(TEXT("{}")), FirstOutcome).Send();
	if (!TestTrue(TEXT("the first is held at the transport"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Observe(Second.Request(TEXT("POST"), TEXT("/auth/second")).Body(TEXT("{}")), SecondOutcome).Send();
	if (!TestTrue(TEXT("the second completes"), Rig.Wait(SecondOutcome)))
	{
		Abandon(Rig);
		return false;
	}
	TestEqual(TEXT("the second exchange under the id ensured"), Fired->load(), 1);
	TestEqual(TEXT("the first is still in flight"), Mine.InFlight.load(), static_cast<int64>(1));
	Rig.Fake->Complete(0, Json(200));
	TestTrue(TEXT("the first ends"), Rig.Wait(FirstOutcome));
	Rig.Drain();
	TestEqual(TEXT("two requests"), Mine.Requests.load(), static_cast<int64>(2));
	TestEqual(TEXT("both counted out"), Mine.InFlight.load(), static_cast<int64>(0));
#else
	AddInfo(TEXT("ensures are compiled out: nothing to check"));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpStatsEnteredTwiceTest, "VaRest.Http.Stats.EnteredTwiceIsOneRequest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpStatsEnteredTwiceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::StatsTestPrivate;
	// As VaRest.Http.Auth.InFlightEntryIsTheExchange: one exchange entered again while still in the chain is one request
	FTestRig Rig;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	const TSharedRef<FExchangeGrab, ESPMode::ThreadSafe> Grab = MakeShared<FExchangeGrab, ESPMode::ThreadSafe>();
	Rig.Client.Use(Grab, EVaRestStageSlot::PerAttempt);
	const VaRest::Http::FHttpCounters& Mine = CountersOf(Rig);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/again")), Outcome).Send();
	if (!TestTrue(TEXT("held at the transport"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Exchange;
	for (const TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe>& Seen : Grab->Seen)
	{
		const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Pinned = Seen.Pin();
		if (Pinned.IsValid() && Pinned->GetRequestId() == Handle.GetId())
		{
			Exchange = Pinned;
		}
	}
	if (!TestTrue(TEXT("its exchange"), Exchange.IsValid()))
	{
		Abandon(Rig);
		return false;
	}
	// Flagged and its transport call canceled; the Canceled comes back only through a game-thread task, so it is still in
	// the chain when it enters again, and Enter finishes it Canceled
	Handle.Cancel();
	VaRest::Http::FClientAccess::State(Rig.Client)->Enter(Exchange.ToSharedRef());
	Exchange.Reset();
	TestTrue(TEXT("it completes once, Canceled"), Rig.Wait(Outcome) && Outcome->Error.Kind == EVaRestErrorKind::Canceled);
	PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	Rig.Drain();
	TestEqual(TEXT("one request"), Mine.Requests.load(), static_cast<int64>(1));
	TestEqual(TEXT("counted out once"), Mine.InFlight.load(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpStatsShutdownTest, "VaRest.Http.Stats.ShutdownCountsOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpStatsShutdownTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::StatsTestPrivate;
	// Engine exit: a request in flight and one sent after it both end silently, and both are counted out. Process-wide, as
	// VaRest.Http.Cancel.AllAndShutdown: it cancels every live client's requests, so run it where no other VaRest traffic
	// matters (a headless test run).
	FTestRig Rig;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	const VaRest::Http::FHttpCounters& Mine = CountersOf(Rig);
	auto Held = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/held")), Held).Send();
	if (!TestTrue(TEXT("held at the transport"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	VaRest::Http::FClientState::CancelForShutdown();
	// Review N-6: the editor goes on, so every later test must send again -- also when this test returns early
	ON_SCOPE_EXIT { VaRest::Http::FClientState::ResetShutdownInternal(); };
	TestTrue(TEXT("shutdown ends it"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }));
	auto Late = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle LateHandle = Observe(Rig.Client.Get(TEXT("/late")), Late).Send();
	TestTrue(TEXT("a request sent during shutdown ends"), PumpUntil([&LateHandle] { return !LateHandle.IsActive(); }));
	Rig.Drain();
	TestEqual(TEXT("... silently, both"), Held->CompleteCalls.load() + Late->CompleteCalls.load(), 0);
	TestEqual(TEXT("two requests"), Mine.Requests.load(), static_cast<int64>(2));
	TestEqual(TEXT("both counted out"), Mine.InFlight.load(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpStatsLlmTagsTest, "VaRest.Http.Stats.LlmTags",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpStatsLlmTagsTest::RunTest(const FString& Parameters)
{
#if ENABLE_LOW_LEVEL_MEM_TRACKER
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::StatsTestPrivate;
	if (!FLowLevelMemTracker::IsEnabled())
	{
		AddInfo(TEXT("LLM is off in this run (start with -llm): nothing to check"));
		return true;
	}
	// Spec 4.8, Inspector IN-14: the pipeline runs under VaRest, a trace point under VaRest_Trace, and the game's own
	// callbacks under the tag they were sent with -- every delivery path: a decoded body (the decode task's VaRest.Deliver),
	// no decode (delivered from OnAttemptDone), an error, OnHttpThread() (a worker task), OnProgress, and an empty client's
	// builder (no pipeline: its delivery task is launched from Send())
	FTestRig Rig;
	const FTagLogRef Log = MakeShared<FTagLog, ESPMode::ThreadSafe>();
	Rig.Client.Use(MakeShared<FTagStage, ESPMode::ThreadSafe>(Log), EVaRestStageSlot::PerAttempt);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Url.EndsWith(TEXT("/fail")) ? Json(500) : Json(200, R"({"a":1})"); });
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	const TSharedRef<FTagSink, ESPMode::ThreadSafe> Sink = MakeShared<FTagSink, ESPMode::ThreadSafe>(Log);
	FVaRestTrace::AddSink(Sink);
	ON_SCOPE_EXIT { FVaRestTrace::RemoveSink(Sink); };
	{
		LLM_SCOPE_BYTAG(VaRestTests_Sender);
		Rig.Client.Get(TEXT("/json"))
			.OnSuccess([Log](const FVaRestJson&) { Log->Note(TEXT("decoded")); })
			.OnProgress([Log](const FVaRestProgress&) { Log->Note(TEXT("progress")); })
			.Send();
		Rig.Client.Get<void>(TEXT("/void")).OnSuccess([Log]() { Log->Note(TEXT("void")); }).Send();
		Rig.Client.Get(TEXT("/fail")).OnError([Log](const FVaRestError&) { Log->Note(TEXT("error")); }).Send();
		Rig.Client.Get(TEXT("/worker")).OnHttpThread().OnComplete([Log](const TVaRestResult<FVaRestJson>&) { Log->Note(TEXT("worker")); }).Send();
		AddExpectedErrorPlain(TEXT("Send() on an empty FVaRestClient"), EAutomationExpectedErrorFlags::Contains, 1);
		FVaRestClient Empty;
		Empty.Get(TEXT("/nowhere")).OnError([Log](const FVaRestError&) { Log->Note(TEXT("empty")); }).Send();
	}
	if (!TestTrue(TEXT("every callback ran"), PumpUntil([Log]
		{
			return Log->Has(TEXT("decoded")) && Log->Has(TEXT("void")) && Log->Has(TEXT("error")) && Log->Has(TEXT("worker")) && Log->Has(TEXT("empty"));
		})))
	{
		Abandon(Rig);
		return false;
	}
	PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	Rig.Drain();
	const FName Sender = LLM_TAG_NAME(VaRestTests_Sender);
	TestEqual(TEXT("a stage runs under VaRest"), Log->Get(TEXT("stage")), LLM_TAG_NAME(VaRest));
	TestEqual(TEXT("a trace point under VaRest_Trace"), Log->Get(TEXT("trace")), LLM_TAG_NAME(VaRest_Trace));
	TestEqual(TEXT("a decoded result's callback under the sender's tag"), Log->Get(TEXT("decoded")), Sender);
	TestEqual(TEXT("a void result's callback under the sender's tag"), Log->Get(TEXT("void")), Sender);
	TestEqual(TEXT("an error's callback under the sender's tag"), Log->Get(TEXT("error")), Sender);
	TestEqual(TEXT("an OnHttpThread() callback under the sender's tag"), Log->Get(TEXT("worker")), Sender);
	TestEqual(TEXT("OnProgress under the sender's tag"), Log->Get(TEXT("progress")), Sender);
	TestEqual(TEXT("an empty client's callback under the sender's tag"), Log->Get(TEXT("empty")), Sender);
#else
	AddInfo(TEXT("LLM is compiled out: nothing to check"));
#endif
	return true;
}

#endif
