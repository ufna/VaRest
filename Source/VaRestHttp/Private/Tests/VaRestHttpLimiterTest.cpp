// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTesting.h"
#include "VaRestLimiter.h"

namespace VaRest::Http::LimiterTestPrivate
{
	FVaRestFakeResponse Ok()
	{
		return FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{}")));
	}

	/** Completes the oldest call still open; false when none is */
	bool CompleteOldest(VaRest::Http::Testing::FTestRig& Rig)
	{
		for (int32 i = 0; i < Rig.Fake->NumCalls(); ++i)
		{
			if (!Rig.Fake->GetCall(i).bCompleted)
			{
				Rig.Fake->Complete(i, Ok());
				return true;
			}
		}
		return false;
	}

	/** The path of the newest transport call; empty when there is none */
	FString NewestPath(const VaRest::Http::Testing::FTestRig& Rig)
	{
		const int32 Num = Rig.Fake->NumCalls();
		return Num > 0 ? Rig.Fake->GetCall(Num - 1).Request.Url.RightChop(FString(TEXT("https://api.test")).Len()) : FString();
	}

	/** True when the transport saw a call whose URL ends in Path */
	bool SawPath(const VaRest::Http::Testing::FTestRig& Rig, const TCHAR* Path)
	{
		for (int32 i = 0; i < Rig.Fake->NumCalls(); ++i)
		{
			if (Rig.Fake->GetCall(i).Request.Url.EndsWith(Path))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Records every exchange it sees, weakly, for the test to read or flag. As an Outer stage it sees a request before the
	 * limiter (ruling I-3: the limiter opens the attempt), so a queued request is found too. As a PerAttempt stage it runs
	 * after the grant and gives a request to .../bad a header value with CR LF, which the transport stage refuses (review I3).
	 */
	class FProbeStage final : public IVaRestStage
	{
	public:
		explicit FProbeStage(EVaRestStageSlot InSlot)
			: Slot(InSlot)
		{
		}

		virtual FName GetStageName() const override { return Slot == EVaRestStageSlot::Outer ? TEXT("Probe") : TEXT("ProbeAttempt"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			if (Slot == EVaRestStageSlot::PerAttempt && Exchange->GetUrl().EndsWith(TEXT("/bad")))
			{
				Exchange->GetAttemptHeaders().Set(TEXT("X-Limiter-Bad"), TEXT("a\r\nb"));
			}
			Seen.Add(Exchange);
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}

		/** The exchange whose URL ends in Path, while it lives */
		TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Find(const TCHAR* Path) const
		{
			for (const TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe>& Weak : Seen)
			{
				const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Pinned = Weak.Pin();
				if (Pinned.IsValid() && Pinned->GetUrl().EndsWith(Path))
				{
					return Pinned;
				}
			}
			return nullptr;
		}

		TArray<TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe>> Seen;
		const EVaRestStageSlot Slot;
	};

	/** A PerAttempt stage that reports its outcome twice (a buggy stage, review C-3) */
	class FTwiceStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("Twice"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Next.Proceed(Exchange, [Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
			{
				FVaRestAttemptOutcome Again = Outcome;
				Done(MoveTemp(Outcome));
				Done(MoveTemp(Again));
			});
		}
	};

	/**
	 * A PerAttempt stage that holds a request to .../held until the test lets it go (an asynchronous signer); every other
	 * request passes. ProceedHeld sends the held attempt to the transport with a callback that only counts its answer;
	 * AnswerHeld answers the attempt itself (an early answer) and lets go of everything: the held exchange holds the
	 * configuration, which holds this stage.
	 */
	class FHoldingStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("Hold"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			if (!Exchange->GetUrl().EndsWith(TEXT("/held")))
			{
				Next.Proceed(Exchange, MoveTemp(OnOutcome));
				return;
			}
			HeldExchange = Exchange;
			HeldNext.Emplace(Next);
			HeldOutcome = MoveTemp(OnOutcome);
		}

		bool IsHolding() const { return HeldExchange.IsValid(); }

		void ProceedHeld()
		{
			if (HeldExchange.IsValid() && HeldNext.IsSet())
			{
				HeldNext->Proceed(HeldExchange.ToSharedRef(), [Late = LateAnswers](FVaRestAttemptOutcome&&) { ++*Late; });
			}
		}

		/** Nothing when nothing is held */
		void AnswerHeld()
		{
			// Moved out first, so the stage keeps nothing once the answer runs
			FVaRestOutcomeCallback Done = MoveTemp(HeldOutcome);
			const bool bWasHolding = HeldExchange.IsValid();
			HeldExchange.Reset();
			HeldNext.Reset();
			if (bWasHolding && Done)
			{
				FVaRestAttemptOutcome Early;
				Early.Status = 204;
				Done(MoveTemp(Early));
			}
		}

		TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> HeldExchange;
		TOptional<FVaRestStageNext> HeldNext;
		FVaRestOutcomeCallback HeldOutcome;
		TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> LateAnswers = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	};

	/** A limiter under test and what its grants saw */
	struct FReentryProbe
	{
		VaRest::Http::FLimiter Tested;
		int32 Depth = 0;
		int32 MaxDepth = 0;
		TArray<uint64> Granted;
	};

	FString JoinIds(const TArray<uint64>& Ids)
	{
		FString Out;
		for (const uint64 Id : Ids)
		{
			Out += FString::Printf(TEXT("%s%llu"), Out.IsEmpty() ? TEXT("") : TEXT(","), Id);
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterMaxTest, "VaRest.Http.Limiter.MaxConcurrent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterMaxTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(2);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> O;
	for (int32 i = 0; i < 5; ++i)
	{
		O.Add(MakeOutcome<FVaRestJson>());
		Observe(Rig.Client.Get(FString::Printf(TEXT("/q/%d"), i)), O.Last()).Send();
	}
	PumpUntil([] { return false; }, 0.05);
	TestEqual(TEXT("spec 4.6: at most MaxConcurrent reach the transport"), Rig.Fake->NumCalls(), 2);
	CompleteOldest(Rig);
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3; });
	TestEqual(TEXT("a finished attempt frees a slot for the next"), Rig.Fake->NumCalls(), 3);
	auto AllDone = [&O] { for (const auto& X : O) { if (!X->IsDone()) { return false; } } return true; };
	int32 MaxInFlight = 0;
	for (int32 Guard = 0; Guard < 200 && !AllDone(); ++Guard)
	{
		MaxInFlight = FMath::Max(MaxInFlight, Rig.Fake->NumInFlight());
		CompleteOldest(Rig);
		PumpUntil([] { return false; }, 0.01);
	}
	TestTrue(TEXT("all five finish"), AllDone());
	TestEqual(TEXT("each reached the transport once"), Rig.Fake->NumCalls(), 5);
	TestTrue(TEXT("never more than two at once"), MaxInFlight <= 2);
	if (!AllDone())
	{
		Rig.Client.CancelAll();
	}
	TestTrue(TEXT("nothing is left in flight"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterOrderTest, "VaRest.Http.Limiter.PriorityOrder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterOrderTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	struct FSend
	{
		const TCHAR* Name;
		EVaRestPriority Priority;
	};
	const FSend Sends[] = { { TEXT("A"), EVaRestPriority::Normal }, { TEXT("B"), EVaRestPriority::Low }, { TEXT("C"), EVaRestPriority::Normal },
		{ TEXT("D"), EVaRestPriority::Highest }, { TEXT("E"), EVaRestPriority::High }, { TEXT("F"), EVaRestPriority::Normal } };
	for (const FSend& Send : Sends)
	{
		Rig.Client.Get(FString::Printf(TEXT("/%s"), Send.Name)).Priority(Send.Priority).Send();
		PumpUntil([] { return false; }, 0.01); // A is granted first; the rest queue in this order
	}
	TArray<FString> Order;
	for (int32 Step = 0; Step < 6; ++Step)
	{
		if (!TestTrue(TEXT("the next one is dispatched"), PumpUntil([&Rig, Step] { return Rig.Fake->NumCalls() > Step; })))
		{
			break;
		}
		Order.Add(Rig.Fake->GetCall(Step).Request.Url.RightChop(FString(TEXT("https://api.test/")).Len()));
		Rig.Fake->Complete(Step, Ok());
	}
	// Highest -> Low, FIFO within a priority (spec 4.6)
	TestEqualSensitive(TEXT("dispatch order"), FString::Join(Order, TEXT(",")), FString(TEXT("A,D,E,C,F,B")));
	if (Order.Num() < 6)
	{
		Rig.Client.CancelAll();
	}
	TestTrue(TEXT("all six finish"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterCancelTest, "VaRest.Http.Limiter.CancelQueued",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterCancelTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	const VaRest::Http::FLimiter& Slots = VaRest::Http::FClientAccess::State(Rig.Client)->GetLimiter();
	auto A = MakeOutcome<FVaRestJson>();
	auto B = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), A).Send();
	const FVaRestRequestHandle BHandle = Observe(Rig.Client.Get(TEXT("/b")), B).Send();
	PumpUntil([] { return false; }, 0.02);
	TestEqual(TEXT("B waits in the queue"), Slots.NumQueued(), 1);
	BHandle.Cancel();
	// Review Minor 5: without the Abort, Cancel()'s fallback still finishes B at once, and its stale entry is granted and
	// dropped only when A's slot frees -- the queue is the one place that tells the two apart
	TestEqual(TEXT("Cancel() takes it out of the queue at once"), Slots.NumQueued(), 0);
	TestTrue(TEXT("a queued request cancels at once"), Rig.Wait(B));
	TestTrue(TEXT("... as Canceled"), B->Error.Kind == EVaRestErrorKind::Canceled);
	CompleteOldest(Rig);
	TestTrue(TEXT("A finishes"), Rig.Wait(A));
	PumpUntil([] { return false; }, 0.02);
	TestEqual(TEXT("the canceled one is never dispatched"), Rig.Fake->NumCalls(), 1);
	auto C = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/c")), C).Send();
	if (TestTrue(TEXT("and the slot is free for the next"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; })))
	{
		Rig.Fake->Complete(1, Ok());
		TestTrue(TEXT("C finishes"), Rig.Wait(C));
	}
	Rig.Client.CancelAll(); // only on the failure path is anything left
	TestTrue(TEXT("nothing is left in flight"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }));
	TestFalse(TEXT("the transport never saw the canceled request"), SawPath(Rig, TEXT("/b")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterOwnerTest, "VaRest.Http.Limiter.OwnerGoneWhileQueued",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterOwnerTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	auto A = MakeOutcome<FVaRestJson>();
	auto Queued = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), A).Send();
	const FVaRestRequestHandle QueuedHandle = Observe(Rig.Client.Get(TEXT("/owned")).Owner(Owner), Queued).Send();
	PumpUntil([] { return false; }, 0.02);
	Owner->MarkAsGarbage();
	CompleteOldest(Rig);
	TestTrue(TEXT("A finishes"), Rig.Wait(A));
	TestTrue(TEXT("the queued one finishes, silently"), PumpUntil([&QueuedHandle] { return !QueuedHandle.IsActive(); }));
	TestEqual(TEXT("an owner gone while queued: never dispatched"), Rig.Fake->NumCalls(), 1);
	TestEqual(TEXT("... no callbacks"), Queued->CompleteCalls.load(), 0);
	auto C = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/c")), C).Send();
	if (TestTrue(TEXT("... and its slot was not leaked"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; })))
	{
		Rig.Fake->Complete(1, Ok());
		TestTrue(TEXT("C finishes"), Rig.Wait(C));
	}
	Rig.Client.CancelAll(); // only on the failure path is anything left
	TestTrue(TEXT("nothing is left in flight"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterCapacityTest, "VaRest.Http.Limiter.CapacityChange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterCapacityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	for (int32 i = 0; i < 3; ++i)
	{
		Rig.Client.Get(FString::Printf(TEXT("/c/%d"), i)).Send();
	}
	PumpUntil([] { return false; }, 0.02);
	TestEqual(TEXT("one at a time"), Rig.Fake->NumCalls(), 1);
	Rig.Client.WithMaxConcurrent(3);
	TestEqual(TEXT("review M-2: nothing is granted inside WithMaxConcurrent()"), Rig.Fake->NumCalls(), 1);
	TestTrue(TEXT("raising MaxConcurrent dispatches the queue on the next game-thread task"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3; }));

	// Review Minor 7: lowering it below the attempts in flight takes none back, and grants nothing until fewer than the
	// new limit remain
	Rig.Client.Get(TEXT("/c/3")).Send(); // queued: three slots, three in flight
	Rig.Client.WithMaxConcurrent(1);
	PumpUntil([] { return false; }, 0.02);
	TestTrue(TEXT("lowering MaxConcurrent cancels nothing"), Rig.Fake->NumCanceled() == 0 && Rig.Fake->NumInFlight() == 3);
	TestEqual(TEXT("... and grants nothing"), Rig.Fake->NumCalls(), 3);
	CompleteOldest(Rig);
	PumpUntil([] { return false; }, 0.02);
	TestEqual(TEXT("two in flight against one slot: nothing granted"), Rig.Fake->NumCalls(), 3);
	CompleteOldest(Rig);
	PumpUntil([] { return false; }, 0.02);
	TestEqual(TEXT("one in flight against one slot: nothing granted"), Rig.Fake->NumCalls(), 3);
	CompleteOldest(Rig);
	TestTrue(TEXT("none in flight: the queued one is granted"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 4; }));
	Rig.Client.CancelAll();
	TestTrue(TEXT("all four finish"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterDroppedTest, "VaRest.Http.Limiter.DroppedCompletionFreesSlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterDroppedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Review I-3: a completion the transport drops still finishes the request, and its slot comes back
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto First = MakeOutcome<FVaRestJson>();
	auto Second = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/first")), First).Send();
	Observe(Rig.Client.Get(TEXT("/second")), Second).Send();
	PumpUntil([] { return false; }, 0.02);
	if (TestEqual(TEXT("one slot"), Rig.Fake->NumCalls(), 1))
	{
		Rig.Fake->Drop(0);
		TestTrue(TEXT("the dropped one finishes"), Rig.Wait(First));
		TestTrue(TEXT("... once, with OnError and OnComplete"), First->ErrorCalls == 1 && First->CompleteCalls == 1 && First->Error.Message.Contains(TEXT("TransportDropped")));
		TestTrue(TEXT("the slot is released: the queued one is dispatched"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }));
	}
	Rig.Client.CancelAll();
	TestTrue(TEXT("both finish"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterExitsTest, "VaRest.Http.Limiter.EveryExitFreesSlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterExitsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	// One slot: the request queued behind each way an attempt ends is dispatched only if that way gave the slot back
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	// Ruling I-3: the probe that finds a queued exchange is an Outer stage; the one that sets the bad header runs after the grant
	TSharedRef<FProbeStage, ESPMode::ThreadSafe> Probe = MakeShared<FProbeStage, ESPMode::ThreadSafe>(EVaRestStageSlot::Outer);
	Rig.Client.Use(Probe, EVaRestStageSlot::Outer);
	Rig.Client.Use(MakeShared<FProbeStage, ESPMode::ThreadSafe>(EVaRestStageSlot::PerAttempt), EVaRestStageSlot::PerAttempt);
	// Completions post their game-thread task inside Complete(), Cancel() and Advance(), so the order below is fixed
	Rig.Fake->bCompleteOnWorker = false;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	const VaRest::Http::FLimiter& Slots = VaRest::Http::FClientAccess::State(Rig.Client)->GetLimiter();
	auto Drain = [this, &Rig]
	{
		Rig.Client.CancelAll();
		TestTrue(TEXT("drained after a failure"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }));
		return true;
	};

	// Canceled at the transport
	auto CancelMe = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle CancelMeHandle = Observe(Rig.Client.Get(TEXT("/cancel-me")), CancelMe).Send();
	auto AfterCancel = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/after-cancel")), AfterCancel).Send();
	PumpUntil([] { return false; }, 0.02);
	TestEqual(TEXT("cancel: the second waits for the slot"), Rig.Fake->NumCalls(), 1);
	CancelMeHandle.Cancel();
	TestTrue(TEXT("cancel: finishes as Canceled"), Rig.Wait(CancelMe) && CancelMe->Error.Kind == EVaRestErrorKind::Canceled);
	if (!TestTrue(TEXT("a cancel at the transport frees the slot"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; })))
	{
		return Drain();
	}
	CompleteOldest(Rig);
	TestTrue(TEXT("cancel: the next one finishes"), Rig.Wait(AfterCancel));

	// Timed out at the transport (the fake's total timeout on the virtual clock, as the HTTP transport's)
	VaRest::Http::FClientAccess::State(Rig.Client)->EditConfig([](VaRest::Http::FClientConfig& Edited) { Edited.Timeout = 1.f; });
	auto TimedOut = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/time-out")), TimedOut).Send();
	VaRest::Http::FClientAccess::State(Rig.Client)->EditConfig([](VaRest::Http::FClientConfig& Edited) { Edited.Timeout = 0.f; });
	auto AfterTimeout = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/after-timeout")), AfterTimeout).Send();
	PumpUntil([] { return false; }, 0.02);
	TestEqual(TEXT("timeout: the second waits for the slot"), Rig.Fake->NumCalls(), 3);
	Rig.Clock->Advance(1.5);
	TestTrue(TEXT("timeout: finishes as Timeout"), Rig.Wait(TimedOut) && TimedOut->Error.Kind == EVaRestErrorKind::Timeout);
	if (!TestTrue(TEXT("a timeout frees the slot"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 4; })))
	{
		return Drain();
	}
	CompleteOldest(Rig);
	TestTrue(TEXT("timeout: the next one finishes"), Rig.Wait(AfterTimeout));

	// Refused before dispatch: granted, then failed by the transport stage (a header a stage set, review I3)
	AddExpectedErrorPlain(TEXT("header \"X-Limiter-Bad\""), EAutomationExpectedErrorFlags::Contains, 1);
	auto Holder = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/hold")), Holder).Send();
	auto Refused = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/bad")), Refused).Send();
	auto AfterRefused = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/after-bad")), AfterRefused).Send();
	PumpUntil([] { return false; }, 0.02);
	TestEqual(TEXT("refused: both wait behind the holder"), Rig.Fake->NumCalls(), 5);
	CompleteOldest(Rig);
	TestTrue(TEXT("refused: finishes as Config"), Rig.Wait(Refused) && Refused->Error.Kind == EVaRestErrorKind::Config);
	if (!TestTrue(TEXT("a refusal before dispatch frees the slot"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 6; })))
	{
		return Drain();
	}
	TestEqual(TEXT("... and the refused one never reached the transport"), NewestPath(Rig), FString(TEXT("/after-bad")));
	CompleteOldest(Rig);
	TestTrue(TEXT("refused: the holder and the next one finish"), Rig.Wait(Holder) && Rig.Wait(AfterRefused));

	// Flagged while queued, with no game-thread Cancel() behind it -- the state a worker's Cancel() leaves until its hop
	// runs (VaRestClient.cpp:254-255). Only the grant can see it: Proceed answers Canceled there, and no Abort can, since
	// only FClientState::Cancel calls one. Nothing here depends on the order of game-thread tasks (review Minor 6).
	auto Holder2 = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/hold-2")), Holder2).Send();
	auto Flagged = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/flagged")), Flagged).Send();
	auto AfterFlagged = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/after-flagged")), AfterFlagged).Send();
	PumpUntil([] { return false; }, 0.02);
	TestEqual(TEXT("flagged: both wait behind the holder"), Rig.Fake->NumCalls(), 7);
	const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> FlaggedExchange = Probe->Find(TEXT("/flagged"));
	if (!TestTrue(TEXT("flagged: the probe holds it"), FlaggedExchange.IsValid()))
	{
		return Drain();
	}
	VaRest::Http::Internals(*FlaggedExchange).bCancelRequested = true;
	TestEqual(TEXT("flagged: setting the flag takes nothing out of the queue"), Slots.NumQueued(), 2);
	CompleteOldest(Rig);
	TestTrue(TEXT("flagged: finishes as Canceled"), Rig.Wait(Flagged) && Flagged->Error.Kind == EVaRestErrorKind::Canceled);
	if (!TestTrue(TEXT("a cancel seen at the grant frees the slot"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 8; })))
	{
		return Drain();
	}
	TestEqual(TEXT("... and the flagged one never reached the transport"), NewestPath(Rig), FString(TEXT("/after-flagged")));
	TestTrue(TEXT("... it was granted: out of the queue, past the attempt boundary"), !VaRest::Http::Internals(*FlaggedExchange).bInLimiterQueue && FlaggedExchange->GetAttempt() == 1);
	CompleteOldest(Rig);
	TestTrue(TEXT("flagged: the holder and the next one finish"), Rig.Wait(Holder2) && Rig.Wait(AfterFlagged));

	// Canceled while queued: out of the queue at once, holding no slot
	auto Holder3 = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/hold-3")), Holder3).Send();
	auto Queued = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle QueuedHandle = Observe(Rig.Client.Get(TEXT("/queued")), Queued).Send();
	PumpUntil([] { return false; }, 0.02);
	TestTrue(TEXT("queued: one granted, one waiting"), Slots.NumActive() == 1 && Slots.NumQueued() == 1);
	QueuedHandle.Cancel();
	TestTrue(TEXT("Cancel() takes a queued request out of the queue at once"), Slots.NumQueued() == 0 && Slots.NumActive() == 1);
	TestTrue(TEXT("queued: finishes as Canceled"), Rig.Wait(Queued) && Queued->Error.Kind == EVaRestErrorKind::Canceled);
	CompleteOldest(Rig);
	TestTrue(TEXT("queued: the holder finishes"), Rig.Wait(Holder3));
	PumpUntil([] { return false; }, 0.02);
	TestEqual(TEXT("... and the canceled one never reached the transport"), Rig.Fake->NumCalls(), 9);

	TestTrue(TEXT("nothing is left in flight"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }));
	TestTrue(TEXT("every slot came back"), Slots.NumActive() == 0 && Slots.NumQueued() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterShutdownTest, "VaRest.Http.Limiter.ShutdownDrainsQueue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterShutdownTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	// Review Minor 7: engine exit aborts what is queued, silently, and the queues drain. CancelForShutdown() is
	// process-wide: like VaRest.Http.Cancel's shutdown part (task 11 review, minor 6), run this headless only.
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	const VaRest::Http::FLimiter& Slots = VaRest::Http::FClientAccess::State(Rig.Client)->GetLimiter();
	auto A = MakeOutcome<FVaRestJson>();
	auto High = MakeOutcome<FVaRestJson>();
	auto Low = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), A).Send();
	Observe(Rig.Client.Get(TEXT("/high")).Priority(EVaRestPriority::High), High).Send();
	Observe(Rig.Client.Get(TEXT("/low")).Priority(EVaRestPriority::Low), Low).Send();
	PumpUntil([] { return false; }, 0.02);
	TestTrue(TEXT("one granted, two queued"), Slots.NumActive() == 1 && Slots.NumQueued() == 2);
	VaRest::Http::FClientState::CancelForShutdown();
	// Review N-6: the editor goes on, so every later test must send again -- also when this test returns early
	ON_SCOPE_EXIT { VaRest::Http::FClientState::ResetShutdownInternal(); };
	TestEqual(TEXT("engine exit empties the queue at once"), Slots.NumQueued(), 0);
	TestTrue(TEXT("every request finishes"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }));
	PumpUntil([] { return false; }, 0.05);
	TestTrue(TEXT("... silently: no callbacks"), A->CompleteCalls.load() == 0 && High->CompleteCalls.load() == 0 && Low->CompleteCalls.load() == 0);
	TestEqual(TEXT("the queued ones never reached the transport"), Rig.Fake->NumCalls(), 1);
	TestTrue(TEXT("... and the granted one's slot came back"), Slots.NumActive() == 0 && Slots.NumQueued() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterWaitTest, "VaRest.Http.Limiter.WaitOnOwnClock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterWaitTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	// Inspector IN-2, review Minor 3: LimiterWait is measured on the waiting exchange's own clock at both ends, whichever
	// exchange's Release grants it and whatever clock the client holds by then
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	// Ruling I-3: an Outer stage, so it holds B while B waits in the queue
	TSharedRef<FProbeStage, ESPMode::ThreadSafe> Probe = MakeShared<FProbeStage, ESPMode::ThreadSafe>(EVaRestStageSlot::Outer);
	Rig.Client.Use(Probe, EVaRestStageSlot::Outer);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto A = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), A).Send(); // on the rig's clock, granted at once
	TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Second = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
	Second->Advance(100.0);
	Rig.Client.WithClock(Second);
	auto B = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/b")), B).Send(); // queued, on the second clock
	TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Third = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
	Third->Advance(1000.0);
	Rig.Client.WithClock(Third); // the client moves on before B is granted
	// Kept for the reads below: a finished exchange may be gone by then
	const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> AExchange = Probe->Find(TEXT("/a"));
	const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> BExchange = Probe->Find(TEXT("/b"));
	PumpUntil([] { return false; }, 0.02);
	Second->Advance(3.0);
	CompleteOldest(Rig); // A's Release, on A's clock, grants B
	if (TestTrue(TEXT("B is granted"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }))
		&& TestTrue(TEXT("the probe saw both"), AExchange.IsValid() && BExchange.IsValid()))
	{
		TestEqual(TEXT("a grant at once waited nothing"), VaRest::Http::Internals(*AExchange).LimiterWait, 0.0);
		TestEqual(TEXT("B waited 3 s on its own clock"), VaRest::Http::Internals(*BExchange).LimiterWait, 3.0);
	}
	CompleteOldest(Rig);
	TestTrue(TEXT("both finish"), Rig.Wait(A) && Rig.Wait(B));
	Rig.Client.CancelAll(); // only on the failure path is anything left
	TestTrue(TEXT("nothing is left in flight"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterReentryTest, "VaRest.Http.Limiter.ReleaseInsideGrant",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterReentryTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::LimiterTestPrivate;
	// Review M-1: an attempt that ends inside its own grant (canceled at the grant, refused by the transport stage) only
	// frees its slot; the pump already running grants the next. A Release that pumped again would nest one level deeper
	// for every such entry in the queue.
	TSharedRef<FReentryProbe> Probe = MakeShared<FReentryProbe>();
	const TWeakPtr<FReentryProbe> WeakProbe = Probe;
	Probe->Tested.SetCapacity(1);
	Probe->Tested.Acquire(1, EVaRestPriority::Normal, [] {}, [] {});
	for (uint64 Id = 2; Id <= 6; ++Id)
	{
		Probe->Tested.Acquire(Id, EVaRestPriority::Normal, [WeakProbe, Id]
		{
			if (const TSharedPtr<FReentryProbe> Pinned = WeakProbe.Pin())
			{
				++Pinned->Depth;
				Pinned->MaxDepth = FMath::Max(Pinned->MaxDepth, Pinned->Depth);
				Pinned->Granted.Add(Id);
				Pinned->Tested.Release(); // it ends at once
				--Pinned->Depth;
			}
		}, [] {});
	}
	TestEqual(TEXT("one granted, five queued"), Probe->Tested.NumQueued(), 5);
	Probe->Tested.Release();
	TestEqualSensitive(TEXT("every queued attempt is granted, in order"), JoinIds(Probe->Granted), FString(TEXT("2,3,4,5,6")));
	TestEqual(TEXT("review M-1: a Release inside a grant does not pump again"), Probe->MaxDepth, 1);
	TestEqual(TEXT("... and every slot came back"), Probe->Tested.NumActive(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterBadPriorityTest, "VaRest.Http.Limiter.OutOfRangePriority",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterBadPriorityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::LimiterTestPrivate;
	// Review Minor 2: EVaRestPriority is a uint8 BlueprintType enum; a value outside it queues as Low, with a warning,
	// instead of indexing past the queues
	AddExpectedMessagePlain(TEXT("priority 200 is not an EVaRestPriority"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TSharedRef<FReentryProbe> Probe = MakeShared<FReentryProbe>();
	const TWeakPtr<FReentryProbe> WeakProbe = Probe;
	auto EndsAtOnce = [WeakProbe](uint64 Id)
	{
		return [WeakProbe, Id]
		{
			if (const TSharedPtr<FReentryProbe> Pinned = WeakProbe.Pin())
			{
				Pinned->Granted.Add(Id);
				Pinned->Tested.Release();
			}
		};
	};
	Probe->Tested.SetCapacity(1);
	Probe->Tested.Acquire(1, EVaRestPriority::Normal, [] {}, [] {});
	Probe->Tested.Acquire(2, static_cast<EVaRestPriority>(200), EndsAtOnce(2), [] {});
	Probe->Tested.Acquire(3, EVaRestPriority::Low, EndsAtOnce(3), [] {});
	Probe->Tested.Acquire(4, EVaRestPriority::Normal, EndsAtOnce(4), [] {});
	TestEqual(TEXT("three queued"), Probe->Tested.NumQueued(), 3);
	Probe->Tested.Release();
	TestEqualSensitive(TEXT("the out-of-range one waits with Low, first come first served"), JoinIds(Probe->Granted), FString(TEXT("4,2,3")));
	TestTrue(TEXT("... and every slot came back"), Probe->Tested.NumActive() == 0 && Probe->Tested.NumQueued() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterDoubleReleaseTest, "VaRest.Http.Limiter.DoubleReleaseEnsuresEachTime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterDoubleReleaseTest::RunTest(const FString& Parameters)
{
#if DO_ENSURE
	// Bead VaRest-3dj (4): ensureAlways reports every time only while core.EnsureAlwaysEnabled is on (AssertionMacros.cpp:136-140,
	// :948) and every ensure is handled: [Core.System] HandleEnsurePercent, else -handleensurepercent=, at 100
	// (GenericPlatformMisc.cpp:2058-2068, :2070-2094). Under other settings the count is chance: say so instead of failing.
	{
		IConsoleVariable* Always = IConsoleManager::Get().FindConsoleVariable(TEXT("core.EnsureAlwaysEnabled"));
		float Percent = 100.f;
		if (!GConfig || !GConfig->GetFloat(TEXT("Core.System"), TEXT("HandleEnsurePercent"), Percent, GEngineIni))
		{
			FParse::Value(FCommandLine::Get(), TEXT("handleensurepercent="), Percent);
		}
		if (!Always || !Always->GetBool() || Percent < 100.f)
		{
			AddInfo(TEXT("core.EnsureAlwaysEnabled is off or HandleEnsurePercent is below 100: ensureAlways does not report every failure here; nothing to check"));
			return true;
		}
	}
	// Task 12 review minor 1 (P12.1): every double release reports, not only the first of the session (ensureMsgf's rule,
	// AssertionMacros.cpp:970-975). The thread's ensure handler (AssertionMacros.h:55-61) takes this one ensure, so the run
	// is deterministic and leaves no callstack or crash report; FDebug::EnsureFailed asks it first (AssertionMacros.cpp:499).
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Fired = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	const TFunction<bool(const FEnsureHandlerArgs&)> Outer = GetEnsureHandler();
	SetEnsureHandler([Fired, Outer](const FEnsureHandlerArgs& Args)
	{
		if (Args.Message && FCString::Strstr(Args.Message, TEXT("FLimiter: Release() without a matching grant")))
		{
			++*Fired;
			return true;
		}
		return Outer ? Outer(Args) : false;
	});
	ON_SCOPE_EXIT { SetEnsureHandler(Outer); };
	VaRest::Http::FLimiter Limiter;
	Limiter.Release();
	Limiter.Release();
	TestEqual(TEXT("each double release ensures (ensureAlways)"), Fired->load(), 2);
	TestEqual(TEXT("... and the count never goes negative"), Limiter.NumActive(), 0);
#else
	AddInfo(TEXT("ensures are compiled out (DO_ENSURE 0): nothing to check"));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterFirstTest, "VaRest.Http.Limiter.FirstInTheAttempt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterFirstTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	// Ruling I-3 (spec 4 and 4.6 as amended): the limiter opens every attempt, so what comes after it -- a PerAttempt stage
	// (a signer; M4a's AuthApply) and the log stage -- meets a queued request only when it leaves the queue
	TGuardConsoleVariable<int32> Level(TEXT("VaRest.LogLevel"), -1);
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1).WithLogLevel(EVaRestLogLevel::Headers);
	const TSharedRef<FProbeStage, ESPMode::ThreadSafe> Signer = MakeShared<FProbeStage, ESPMode::ThreadSafe>(EVaRestStageSlot::PerAttempt);
	Rig.Client.Use(Signer, EVaRestStageSlot::PerAttempt);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	const VaRest::Http::FLimiter& Slots = VaRest::Http::FClientAccess::State(Rig.Client)->GetLimiter();
	FLogCapture Capture;
	auto First = MakeOutcome<FVaRestJson>();
	auto Second = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/first")), First).Send();
	const FVaRestRequestHandle SecondHandle = Observe(Rig.Client.Get(TEXT("/second")), Second).Send();
	if (!TestTrue(TEXT("the first holds the slot, the second queues"), PumpUntil([&Rig, &Slots] { return Rig.Fake->NumCalls() == 1 && Slots.NumQueued() == 1; })))
	{
		Rig.Client.CancelAll();
		PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
		return false;
	}
	const FString SecondLine = FString::Printf(TEXT("#%llu > GET https://api.test/second (attempt 1)"), SecondHandle.GetId());
	TestFalse(TEXT("a PerAttempt stage has not met the queued request"), Signer->Find(TEXT("/second")).IsValid());
	TestFalse(TEXT("... nor has the log stage written its request line"), Capture.Contains(SecondLine));
	CompleteOldest(Rig);
	if (!TestTrue(TEXT("the second is granted"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; })))
	{
		// Re-review N-8: drained before the early return
		Rig.Client.CancelAll();
		PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
		return false;
	}
	TestTrue(TEXT("... and only now meets the PerAttempt stage"), Signer->Find(TEXT("/second")).IsValid());
	TestTrue(TEXT("... and the log stage"), Capture.Contains(SecondLine));
	CompleteOldest(Rig);
	TestTrue(TEXT("both finish"), Rig.Wait(First) && Rig.Wait(Second));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterCanceledAtGrantTest, "VaRest.Http.Limiter.CanceledAtGrantLogged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterCanceledAtGrantTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	// Ruling I-3: a request flagged as canceled while it waits, with no game-thread Cancel() behind it (a worker's Cancel()
	// until its hop runs, VaRestClient.cpp:254-255), is answered Canceled at its grant, so it never reaches the log stage,
	// which is after the limiter now. Its Basic line is written at the grant, as for one aborted in the queue: 0 ms of its
	// own, the wait apart.
	TGuardConsoleVariable<int32> Level(TEXT("VaRest.LogLevel"), -1);
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1).WithLogLevel(EVaRestLogLevel::Basic);
	const TSharedRef<FProbeStage, ESPMode::ThreadSafe> Probe = MakeShared<FProbeStage, ESPMode::ThreadSafe>(EVaRestStageSlot::Outer);
	Rig.Client.Use(Probe, EVaRestStageSlot::Outer);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	const VaRest::Http::FLimiter& Slots = VaRest::Http::FClientAccess::State(Rig.Client)->GetLimiter();
	FLogCapture Capture;
	auto Holder = MakeOutcome<FVaRestJson>();
	auto Flagged = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/holder")), Holder).Send();
	const FVaRestRequestHandle FlaggedHandle = Observe(Rig.Client.Get(TEXT("/flagged")), Flagged).Send();
	const bool bQueued = TestTrue(TEXT("the holder has the slot, the other queues"), PumpUntil([&Rig, &Slots] { return Rig.Fake->NumCalls() == 1 && Slots.NumQueued() == 1; }));
	const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> FlaggedExchange = Probe->Find(TEXT("/flagged"));
	if (!bQueued || !TestTrue(TEXT("the probe holds the queued one"), FlaggedExchange.IsValid()))
	{
		Rig.Client.CancelAll();
		PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
		return false;
	}
	VaRest::Http::Internals(*FlaggedExchange).bCancelRequested = true;
	Rig.Clock->Advance(1.0);
	CompleteOldest(Rig); // the holder's Release grants the flagged one, and Proceed answers it Canceled
	TestTrue(TEXT("the flagged one finishes as Canceled"), Rig.Wait(Flagged) && Flagged->Error.Kind == EVaRestErrorKind::Canceled);
	TestTrue(TEXT("the holder finishes"), Rig.Wait(Holder));
	TestTrue(TEXT("drained"), Rig.Drain());
	TestEqual(TEXT("the flagged one was never dispatched"), Rig.Fake->NumCalls(), 1);
	const FString Line = FString::Printf(TEXT("#%llu GET https://api.test/flagged -> canceled (0 ms, 1000 ms queued)"), FlaggedHandle.GetId());
	TestEqual(TEXT("its Basic line, once: 0 ms of its own, the 1000 ms queued apart"), Capture.Count(Line), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterReportsTwiceTest, "VaRest.Http.Limiter.PerAttemptReportsTwice",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterReportsTwiceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	// Ruling I-3: a custom PerAttempt stage now runs inside the grant, so the limiter's release is in the callback it is
	// handed, no longer only in the transport stage's exactly-once completion. A buggy stage that reports twice (review
	// C-3: the second report is ignored) must release once: a second release frees another attempt's slot, and the client
	// runs more than MaxConcurrent at once. A double release is also an ensureAlways (FLimiter::Release), counted here.
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Ensured = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
#if DO_ENSURE
	// Task 4 review minor 3: SetEnsureHandler and GetEnsureHandler are declared always (AssertionMacros.h:60-61) but defined
	// only under DO_CHECK || DO_GUARD_SLOW || DO_ENSURE (AssertionMacros.cpp:450, :475-485, :799), so a target with
	// automation tests and checks off would not link. Without ensures the call counts below catch the bug on their own.
	const TFunction<bool(const FEnsureHandlerArgs&)> Previous = GetEnsureHandler();
	SetEnsureHandler([Ensured, Previous](const FEnsureHandlerArgs& Args)
	{
		if (Args.Message && FCString::Strstr(Args.Message, TEXT("FLimiter: Release() without a matching grant")))
		{
			++*Ensured;
			return true;
		}
		return Previous ? Previous(Args) : false;
	});
	ON_SCOPE_EXIT { SetEnsureHandler(Previous); };
#endif
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	Rig.Client.Use(MakeShared<FTwiceStage, ESPMode::ThreadSafe>(), EVaRestStageSlot::PerAttempt);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	const VaRest::Http::FLimiter& Slots = VaRest::Http::FClientAccess::State(Rig.Client)->GetLimiter();
	auto A = MakeOutcome<FVaRestJson>();
	auto B = MakeOutcome<FVaRestJson>();
	auto C = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), A).Send();
	Observe(Rig.Client.Get(TEXT("/b")), B).Send();
	Observe(Rig.Client.Get(TEXT("/c")), C).Send();
	if (!TestTrue(TEXT("one slot: A holds it, B and C queue"), PumpUntil([&Rig, &Slots] { return Rig.Fake->NumCalls() == 1 && Slots.NumQueued() == 2; })))
	{
		Rig.Client.CancelAll();
		PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
		return false;
	}
	CompleteOldest(Rig); // A's one answer reaches the limiter's callback twice
	TestTrue(TEXT("A finishes"), Rig.Wait(A));
	TestTrue(TEXT("drained"), Rig.Drain());
	TestEqual(TEXT("one release for two reports: B alone is granted"), Rig.Fake->NumCalls(), 2);
	TestTrue(TEXT("... C still waits behind the one slot B holds"), Slots.NumQueued() == 1 && Slots.NumActive() == 1);
	TestEqual(TEXT("... and A's callbacks ran once"), A->CompleteCalls.load(), 1);
	CompleteOldest(Rig);
	TestTrue(TEXT("B finishes"), Rig.Wait(B));
	if (TestTrue(TEXT("C is granted"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3; })))
	{
		CompleteOldest(Rig);
		TestTrue(TEXT("C finishes"), Rig.Wait(C));
	}
	Rig.Client.CancelAll(); // only on the failure path is anything left
	TestTrue(TEXT("nothing is left in flight"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }));
	TestTrue(TEXT("every slot came back"), Slots.NumActive() == 0 && Slots.NumQueued() == 0);
	TestEqual(TEXT("no release without a grant"), Ensured->load(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterHoldsSlotTest, "VaRest.Http.Limiter.PerAttemptHoldsSlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterHoldsSlotTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	// Ruling I-3, task 4 review I-1 and minor 2 (VaRestClient.h WithMaxConcurrent, EVaRestStageSlot::PerAttempt): a
	// PerAttempt stage holds a slot from the limiter's grant until it proceeds or answers; one that answers its attempt
	// itself while the transport call still runs gives the slot back with its answer, and the call runs on, uncounted
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	const TSharedRef<FHoldingStage, ESPMode::ThreadSafe> Stage = MakeShared<FHoldingStage, ESPMode::ThreadSafe>();
	Rig.Client.Use(Stage, EVaRestStageSlot::PerAttempt);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	const VaRest::Http::FLimiter& Slots = VaRest::Http::FClientAccess::State(Rig.Client)->GetLimiter();
	auto Bail = [this, &Rig, &Stage]
	{
		Rig.Client.CancelAll();
		Stage->AnswerHeld();
		while (CompleteOldest(Rig))
		{
		}
		TestTrue(TEXT("drained after a failure"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }) && Rig.Drain());
		return false;
	};

	auto A = MakeOutcome<FVaRestJson>();
	auto B = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/held")), A).Send();
	Observe(Rig.Client.Get(TEXT("/b")), B).Send();
	TestTrue(TEXT("drained"), Rig.Drain());
	if (!TestTrue(TEXT("the stage holds A and the one slot; B waits"),
		Stage->IsHolding() && Slots.NumActive() == 1 && Slots.NumQueued() == 1 && Rig.Fake->NumCalls() == 0))
	{
		return Bail();
	}
	Stage->ProceedHeld(); // A's transport call starts; the stage keeps A's answer
	TestTrue(TEXT("drained"), Rig.Drain());
	if (!TestTrue(TEXT("proceeding into a call is not answering: A's call runs, B still waits"), Rig.Fake->NumCalls() == 1 && Slots.NumQueued() == 1))
	{
		return Bail();
	}
	Stage->AnswerHeld(); // the early answer, while A's transport call still runs
	TestTrue(TEXT("A finishes with the stage's answer"), Rig.Wait(A) && A->SuccessCalls.load() == 1 && A->Response.Status == 204);
	if (!TestTrue(TEXT("the early answer gave the slot back: B is granted"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; })))
	{
		return Bail();
	}
	TestEqual(TEXT("... while A's call runs on, uncounted"), Rig.Fake->NumInFlight(), 2);
	Rig.Fake->Complete(0, Ok()); // A's late answer: only counted by the stage
	Rig.Fake->Complete(1, Ok());
	TestTrue(TEXT("B finishes"), Rig.Wait(B) && B->SuccessCalls.load() == 1);
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Late = Stage->LateAnswers;
	TestTrue(TEXT("A's late answer came back to the stage"), PumpUntil([Late] { return Late->load() == 1; }));
	TestTrue(TEXT("drained"), Rig.Drain());
	TestEqual(TEXT("A's callbacks ran once"), A->CompleteCalls.load(), 1);
	TestTrue(TEXT("every slot came back"), Slots.NumActive() == 0 && Slots.NumQueued() == 0);
	return true;
}

#endif
