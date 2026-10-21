// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "VaRestClient.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::TimeoutProgressTestPrivate
{
	/**
	 * Task 13 review minor 5: a PerAttempt stage that answers its attempt itself while the transport call still runs, so the
	 * request finishes with that call live (IVaRestStage::Process does not forbid it). It counts the late answers it swallows.
	 */
	class FEarlyAnswerStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("EarlyAnswer"); }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Next.Proceed(Exchange, [Swallowed = LateAnswers](FVaRestAttemptOutcome&&) { ++*Swallowed; });
			FVaRestAttemptOutcome Early;
			Early.Status = 204;
			OnOutcome(MoveTemp(Early));
		}

		TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> LateAnswers = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTimeoutAppliedTest, "VaRest.Http.Timeout.Applied",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTimeoutAppliedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Plain;
	Plain.Client.Get(TEXT("/p")).Send();
	FTestRig Rig;
	Rig.Client.WithTimeout(10.f, 3.f);
	Rig.Client.Get(TEXT("/a")).Send();
	Rig.Client.Get(TEXT("/b")).Timeout(2.f).Send();
	Rig.Client.Get(TEXT("/c")).Timeout(2.f, 1.f).Send();
	TestTrue(TEXT("all reach the transport"), PumpUntil([&] { return Plain.Fake->NumCalls() == 1 && Rig.Fake->NumCalls() == 3; }));
	auto Of = [&Rig](int32 i) -> const FVaRestTransportRequest& { return Rig.Fake->GetCall(i).Request; };
	TestTrue(TEXT("no setting: 0, the engine's default (HttpModule.cpp:96-98)"), Plain.Fake->GetCall(0).Request.Timeout == 0.f && Plain.Fake->GetCall(0).Request.ActivityTimeout == 0.f);
	TestTrue(TEXT("the client's timeouts (spec 3.1)"), Of(0).Timeout == 10.f && Of(0).ActivityTimeout == 3.f);
	TestTrue(TEXT("a request's total timeout replaces the client's; its activity timeout stays"), Of(1).Timeout == 2.f && Of(1).ActivityTimeout == 3.f);
	TestTrue(TEXT("... and both"), Of(2).Timeout == 2.f && Of(2).ActivityTimeout == 1.f);
	PumpUntil([&] { return Plain.Client.GetInFlightCount() == 0 && Rig.Client.GetInFlightCount() == 0; });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTimeoutFiresTest, "VaRest.Http.Timeout.Fires",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTimeoutFiresTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto Total = MakeOutcome<FVaRestJson>();
	auto Idle = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/slow")).Timeout(1.f), Total).Send();
	Observe(Rig.Client.Get(TEXT("/silent")).Timeout(0.f, 2.f), Idle).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; });
	Rig.Clock->Advance(1.0);
	TestTrue(TEXT("the total timeout"), Rig.Wait(Total));
	TestTrue(TEXT("... is Kind Timeout, retryable (spec 4.1)"), Total->Error.Kind == EVaRestErrorKind::Timeout && Total->Error.bRetryable);
	TestFalse(TEXT("the other is still waiting"), Idle->IsDone());
	Rig.Clock->Advance(1.0);
	TestTrue(TEXT("the activity timeout"), Rig.Wait(Idle));
	TestTrue(TEXT("... is Kind Connection, as the engine reports it (decision 19)"), Idle->Error.Kind == EVaRestErrorKind::Connection
		&& Idle->Error.Message.Contains(TEXT("ActivityTimeout")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpProgressTest, "VaRest.Http.Progress.Throttled",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpProgressTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	struct FSeen
	{
		TArray<FVaRestProgress> Calls;
		bool bGameThread = true;
	};
	TSharedRef<FSeen, ESPMode::ThreadSafe> Seen = MakeShared<FSeen, ESPMode::ThreadSafe>();
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Post(TEXT("/up")).Body(TArray<uint8>{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }).OnProgress([Seen](const FVaRestProgress& Progress)
	{
		Seen->bGameThread &= IsInGameThread();
		Seen->Calls.Add(Progress);
	}), Outcome).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; });
	TestTrue(TEXT("OnProgress binds progress reporting (IN-11)"), Rig.Fake->GetCall(0).Request.bReportProgress);
	TestTrue(TEXT("... as a callback, so curl counts the upload (decision 29, review N-9)"), Rig.Fake->GetCall(0).Request.bProgressCallback);

	Rig.Fake->ReportProgress(0, 4, 0);
	Rig.Fake->ReportProgress(0, 10, 0);
	TestEqual(TEXT("throttled: nothing between ticks"), Seen->Calls.Num(), 0);
	Rig.Clock->Advance(0.1);
	TestEqual(TEXT("one call per tick, with the latest counts"), Seen->Calls.Num(), 1);
	TestTrue(TEXT("... sent and the upload total"), Seen->Calls.Num() == 1 && Seen->Calls[0].BytesSent == 10 && Seen->Calls[0].UploadTotal == 10);
	Rig.Clock->Advance(0.1);
	TestEqual(TEXT("no change, no call"), Seen->Calls.Num(), 1);
	Rig.Fake->ReportProgress(0, 10, 50, 120);
	Rig.Clock->Advance(0.05);
	TestEqual(TEXT("not before the next tick"), Seen->Calls.Num(), 1);
	Rig.Clock->Advance(0.05);
	TestTrue(TEXT("received and the download total"), Seen->Calls.Num() == 2 && Seen->Calls[1].BytesReceived == 50 && Seen->Calls[1].DownloadTotal == 120);

	FVaRestFakeResponse Body = FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT(R"({"k":"0123456789"})")));
	Rig.Fake->Complete(0, Body);
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	TestEqual(TEXT("a last call when the attempt ends"), Seen->Calls.Num(), 3);
	TestTrue(TEXT("... with every byte"), Seen->Calls.Num() == 3 && Seen->Calls[2].BytesReceived == 18);
	TestTrue(TEXT("always on the game thread (spec 3.2)"), Seen->bGameThread);

	Rig.Client.Get(TEXT("/quiet")).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; });
	TestFalse(TEXT("no OnProgress, no progress binding (IN-11)"), Rig.Fake->GetCall(1).Request.bReportProgress);
	TestFalse(TEXT("... and no callback"), Rig.Fake->GetCall(1).Request.bProgressCallback);
	PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });

	// Review M-16: a compressed body's Content-Length counts compressed bytes; past it the total is unknown, never > 100%
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	TSharedRef<FSeen, ESPMode::ThreadSafe> Encoded = MakeShared<FSeen, ESPMode::ThreadSafe>();
	auto Gzip = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/gz")).OnProgress([Encoded](const FVaRestProgress& Progress) { Encoded->Calls.Add(Progress); }), Gzip).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3; });
	Rig.Fake->ReportProgress(2, 0, 300, 100);
	Rig.Clock->Advance(0.1);
	TestTrue(TEXT("received past the reported total: the total is unknown (-1)"), Encoded->Calls.Num() == 1 && Encoded->Calls[0].DownloadTotal == -1);
	Rig.Client.CancelAll();
	PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpProgressOwnerGoneTest, "VaRest.Http.Progress.NotAfterOwnerGone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpProgressOwnerGoneTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Review C-1: the usual .Owner(this).OnProgress([this]...) -- the last progress call must not run into a dead owner
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Calls = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/owned")).Owner(Owner).OnProgress([Calls](const FVaRestProgress&) { ++*Calls; }), Outcome).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; });
	Rig.Fake->ReportProgress(0, 0, 10, 100);
	Owner->MarkAsGarbage();
	const int32 AtMark = Calls->load();
	Rig.Fake->Complete(0, FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT(R"({"k":1})"))));
	TestTrue(TEXT("finishes"), PumpUntil([&Handle] { return !Handle.IsActive(); }));
	PumpUntil([] { return false; }, 0.05);
	TestEqual(TEXT("spec 3.2: owner gone -> no callbacks, progress included"), Calls->load() - AtMark, 0);
	TestEqual(TEXT("... and no result callbacks"), Outcome->CompleteCalls.load(), 0);

	// After an explicit Cancel() too (review M-3)
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> AfterCancel = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	auto Canceled = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle CancelHandle = Observe(Rig.Client.Get(TEXT("/canceled")).OnProgress([AfterCancel](const FVaRestProgress&) { ++*AfterCancel; }), Canceled).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; });
	CancelHandle.Cancel();
	// The fake's Manual() call arms no timers of its own: the progress tick is the only one there was
	TestEqual(TEXT("Cancel() stops the progress timer (review M-3)"), Rig.Clock->NumPendingTimers(), 0);
	const int32 AtCancel = AfterCancel->load();
	Rig.Fake->ReportProgress(1, 0, 10, 100);
	Rig.Clock->Advance(0.1);
	TestTrue(TEXT("completes"), Rig.Wait(Canceled));
	TestEqual(TEXT("no progress after Cancel() returned"), AfterCancel->load() - AtCancel, 0);

	// Cancel() from inside OnProgress: the tick that ran it arms no other (review M-3)
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	TSharedRef<FVaRestRequestHandle, ESPMode::ThreadSafe> SelfHandle = MakeShared<FVaRestRequestHandle, ESPMode::ThreadSafe>();
	auto SelfCanceled = MakeOutcome<FVaRestJson>();
	*SelfHandle = Observe(Rig.Client.Get(TEXT("/self")).OnProgress([SelfHandle](const FVaRestProgress&) { SelfHandle->Cancel(); }), SelfCanceled).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3; });
	Rig.Fake->ReportProgress(2, 0, 10, 100);
	Rig.Clock->Advance(0.1);
	TestEqual(TEXT("a Cancel() inside OnProgress stops the progress timer (review M-3)"), Rig.Clock->NumPendingTimers(), 0);
	TestTrue(TEXT("... and the request completes"), Rig.Wait(SelfCanceled));
	TestTrue(TEXT("... as Canceled"), SelfCanceled->Error.Kind == EVaRestErrorKind::Canceled);

	// Spec 3.2: an owner gone mid-download stops the transfer at the next tick, not at its end
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	UObject* Leaving = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> LeftCalls = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	auto Left = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle LeftHandle = Observe(Rig.Client.Get(TEXT("/leaving")).Owner(Leaving).OnProgress([LeftCalls](const FVaRestProgress&) { ++*LeftCalls; }), Left).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 4; });
	Rig.Fake->ReportProgress(3, 0, 10, 100);
	Leaving->MarkAsGarbage();
	Rig.Clock->Advance(0.1);
	TestTrue(TEXT("an owner gone mid-download cancels the transfer at the next tick"), Rig.Fake->NumCalls() == 4 && Rig.Fake->GetCall(3).bCanceled);
	TestTrue(TEXT("... the request finishes"), PumpUntil([&LeftHandle] { return !LeftHandle.IsActive(); }));
	TestEqual(TEXT("... silently, progress included"), LeftCalls->load() + Left->CompleteCalls.load(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpProgressAfterFinishTest, "VaRest.Http.Progress.NoTickAfterFinish",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpProgressAfterFinishTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TimeoutProgressTestPrivate;
	// Task 13 review minor 5 (P13.5): a request finished while its transport call is still live must not keep its progress
	// tick re-arming until that call ends. Timers are counted against the clock's count before the Send (P13.6).
	FTestRig Rig;
	TSharedRef<FEarlyAnswerStage, ESPMode::ThreadSafe> Stage = MakeShared<FEarlyAnswerStage, ESPMode::ThreadSafe>();
	Rig.Client.Use(Stage, EVaRestStageSlot::PerAttempt);
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual()); // arms no timer of its own
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Calls = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const int32 TimersBefore = Rig.Clock->NumPendingTimers();
	Observe(Rig.Client.Get(TEXT("/early")).OnProgress([Calls](const FVaRestProgress&) { ++*Calls; }), Outcome).Send();
	TestTrue(TEXT("the stage's answer completes the request"), Rig.Wait(Outcome));
	TestEqual(TEXT("... while the transport call still runs"), Rig.Fake->NumInFlight(), 1);
	TestEqual(TEXT("the progress tick was armed at dispatch"), Rig.Clock->NumPendingTimers(), TimersBefore + 1);
	Rig.Fake->ReportProgress(0, 0, 10, 100);
	Rig.Clock->Advance(0.1);
	TestEqual(TEXT("the tick of a finished request does not re-arm"), Rig.Clock->NumPendingTimers(), TimersBefore);
	TestEqual(TEXT("... and delivers nothing (Complete took the handler)"), Calls->load(), 0);
	Rig.Fake->Complete(0, FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{}"))));
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Late = Stage->LateAnswers;
	TestTrue(TEXT("the transport's late answer is drained"), PumpUntil([Late] { return Late->load() == 1; }));
	TestEqual(TEXT("... and the request was delivered once"), Outcome->CompleteCalls.load(), 1);
	return true;
}

#endif
