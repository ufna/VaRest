// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestExchangeState.h"
#include "VaRestHttpTesting.h"
#include "VaRestRetry.h"
#include "VaRestRetryInternal.h"
#include "VaRestTrace.h"

#include <atomic>

namespace VaRest::Http::RetryTestPrivate
{
	FVaRestAttemptOutcome Status(int32 Code, const TCHAR* RetryAfter = nullptr)
	{
		FVaRestAttemptOutcome Out;
		Out.Status = Code;
		if (RetryAfter)
		{
			Out.Headers.Set(TEXT("Retry-After"), RetryAfter);
		}
		return Out;
	}

	FVaRestAttemptOutcome Failed(EVaRestTransportFailure Failure, const TCHAR* Detail)
	{
		FVaRestAttemptOutcome Out;
		Out.Failure = Failure;
		Out.FailureDetail = Detail;
		return Out;
	}

	/** A policy whose delays a test can read off: no jitter, 0.5 s doubling */
	FVaRestRetryPolicy Exact(int32 MaxRetries = 3)
	{
		FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(MaxRetries);
		Policy.Backoff.Jitter = EVaRestJitter::None;
		return Policy;
	}

	FVaRestFakeResponse Json(int32 Code, const char* Text = "{}")
	{
		return FVaRestFakeResponse::Json(Code, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	int32 CallsTo(const VaRest::Http::Testing::FTestRig& Rig, const TCHAR* Path)
	{
		int32 Count = 0;
		for (int32 Index = 0; Index < Rig.Fake->NumCalls(); ++Index)
		{
			Count += Rig.Fake->GetCall(Index).Request.Url.EndsWith(Path) ? 1 : 0;
		}
		return Count;
	}

	/** Cancels everything the rig has in flight and pumps until it is all done: the drain of a test that stops early */
	void Abandon(VaRest::Http::Testing::FTestRig& Rig)
	{
		Rig.Client.CancelAll();
		VaRest::Http::Testing::PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	}

	/** Virtual time whose timers fire Lateness seconds after they are due: a real-clock ticker a frame late (spec review M-10) */
	class FLateClock final : public IVaRestClock
	{
	public:
		FLateClock(TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> InTime, double InLateness)
			: Time(MoveTemp(InTime))
			, Lateness(InLateness)
		{
		}

		virtual double Now() const override { return Time->Now(); }
		virtual FVaRestTimerHandle SetTimer(double DelaySeconds, TUniqueFunction<void()>&& Callback) override
		{
			return Time->SetTimer(DelaySeconds + Lateness, MoveTemp(Callback));
		}
		virtual void ClearTimer(FVaRestTimerHandle& Handle) override { Time->ClearTimer(Handle); }

	private:
		TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Time;
		double Lateness = 0.0;
	};

	/**
	 * An Outer stage (so in front of Retry) that keeps its request's answer: it proceeds or not, and AnswerNow() answers
	 * the request itself while what is behind it may still wait -- a user's own timeout stage (concurrency review M-3, M-4).
	 * The exchange is kept weakly; Held captures it, so a test that never calls AnswerNow() resets Held at its end.
	 */
	class FHoldingOuter final : public IVaRestStage
	{
	public:
		explicit FHoldingOuter(bool bInProceed)
			: bProceed(bInProceed)
		{
		}

		virtual FName GetStageName() const override { return TEXT("HoldingOuter"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Held = MoveTemp(OnOutcome);
			Kept = Exchange;
			if (bProceed)
			{
				Next.Proceed(Exchange, [Late = LateAnswers](FVaRestAttemptOutcome&&) { ++*Late; });
			}
		}

		/** JsonBody, when given, makes an answer the pipeline decodes on a task (re-review N-6) */
		void AnswerNow(int32 Code, FUtf8StringView JsonBody = FUtf8StringView())
		{
			if (!Held)
			{
				return;
			}
			FVaRestAttemptOutcome Early;
			Early.Status = Code;
			if (!JsonBody.IsEmpty())
			{
				Early.ContentType = TEXT("application/json");
				Early.Body.Append(reinterpret_cast<const uint8*>(JsonBody.GetData()), JsonBody.Len());
			}
			FVaRestOutcomeCallback Answer = MoveTemp(Held);
			Held.Reset();
			Answer(MoveTemp(Early));
		}

		const bool bProceed;
		FVaRestOutcomeCallback Held;
		TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe> Kept;
		/** What reached this stage from behind it after it had answered */
		TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> LateAnswers = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryDecideTest, "VaRest.Http.Retry.Decide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryDecideTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::RetryTestPrivate;
	const FDateTime NowUtc(2026, 10, 21, 12, 0, 0);
	const FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(3);
	auto Decide = [&](const FVaRestRetryPolicy& With, const FVaRestAttemptOutcome& Outcome, int32 Done = 0, double Now = 0.0, double DeadlineAt = 0.0)
	{
		FRandomStream Random(7);
		return DecideRetry(With, Outcome, Done, Now, DeadlineAt, NowUtc, Random);
	};

	FRandomStream Same(7);
	const FRetryDecision First = Decide(Policy, Status(503));
	TestTrue(TEXT("503: retried, after the backoff's first delay from the caller's stream"), First.bRetry && First.Delay == Policy.Backoff.GetDelay(1, Same));
	TestTrue(TEXT("... the reason names the status, no Retry-After"), First.Reason == FName(TEXT("Status503")) && First.RetryAfter < 0.0);
	TestFalse(TEXT("after MaxRetries: no more"), Decide(Policy, Status(503), 3).bRetry);
	TestFalse(TEXT("404: not a retried status"), Decide(Policy, Status(404)).bRetry);
	FVaRestRetryPolicy Conflict = Policy;
	Conflict.RetryStatuses.Add(409);
	TestTrue(TEXT("a status the policy adds is retried"), Decide(Conflict, Status(409)).bRetry);
	TestTrue(TEXT("a connection error"), Decide(Policy, Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError"))).bRetry);
	TestTrue(TEXT("the engine's Other (a reset, an empty reply on curl)"), Decide(Policy, Failed(EVaRestTransportFailure::Other, TEXT("Other"))).bRetry);
	FVaRestRetryPolicy NoConnection = Policy;
	NoConnection.bRetryConnectionErrors = false;
	TestFalse(TEXT("... unless the policy says no"), Decide(NoConnection, Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError"))).bRetry);
	TestTrue(TEXT("a timeout"), Decide(Policy, Failed(EVaRestTransportFailure::Timeout, TEXT("TimedOut"))).bRetry);
	FVaRestRetryPolicy NoTimeout = Policy;
	NoTimeout.bRetryTimeouts = false;
	TestFalse(TEXT("... unless the policy says no"), Decide(NoTimeout, Failed(EVaRestTransportFailure::Timeout, TEXT("TimedOut"))).bRetry);
	// Rulings Q7 and I-7: AuthInvalid (a misconfigured built-in auth, Task 14) and AuthRefreshConfig (a refresh the
	// configuration refused, Task 17) are Kind=Config and never retried
	const TCHAR* Never[] = { TEXT("Cancelled"), TEXT("InvalidHeader"), TEXT("TransportDropped"), TEXT("AuthRejected"), TEXT("HttpDisabled"), TEXT("ResponseTooLarge"),
		TEXT("AuthInvalid"), TEXT("AuthRefreshConfig") };
	for (const TCHAR* Detail : Never)
	{
		const EVaRestTransportFailure Failure = FCString::Strcmp(Detail, TEXT("Cancelled")) == 0 ? EVaRestTransportFailure::Canceled : EVaRestTransportFailure::Other;
		TestFalse(FString::Printf(TEXT("never retried: %s"), Detail), Decide(Policy, Failed(Failure, Detail)).bRetry);
	}
	TestFalse(TEXT("never retried: a deadline"), Decide(Policy, Failed(EVaRestTransportFailure::Timeout, TEXT("Deadline"))).bRetry);
	TestFalse(TEXT("a disabled policy"), Decide(FVaRestRetryPolicy::Disabled(), Status(503)).bRetry);

	// Retry-After replaces the backoff (plan decision 11), capped (spec 4.1: 60 s)
	const FRetryDecision Asked = Decide(Policy, Status(503, TEXT("5")));
	TestTrue(TEXT("Retry-After: 5"), Asked.bRetry && Asked.Delay == 5.0 && Asked.RetryAfter == 5.0);
	const FRetryDecision Long = Decide(Policy, Status(429, TEXT("120")));
	TestTrue(TEXT("Retry-After: 120 waits the cap, and says what was asked"), Long.bRetry && Long.Delay == 60.0 && Long.RetryAfter == 120.0);
	const FRetryDecision Dated = Decide(Policy, Status(503, TEXT("Wed, 21 Oct 2026 12:00:30 GMT")));
	TestTrue(TEXT("an HTTP-date"), Dated.bRetry && Dated.Delay == 30.0);
	FRandomStream Again(7);
	const FRetryDecision Garbage = Decide(Policy, Status(503, TEXT("soon")));
	TestTrue(TEXT("garbage: the backoff"), Garbage.bRetry && Garbage.Delay == Policy.Backoff.GetDelay(1, Again) && Garbage.RetryAfter < 0.0);
	FVaRestRetryPolicy Deaf = Exact();
	Deaf.bRespectRetryAfter = false;
	TestTrue(TEXT("a policy that ignores Retry-After"), Decide(Deaf, Status(503, TEXT("5"))).Delay == 0.5);

	// The deadline (plan decision 10): a retry that would start at or past it does not happen
	const FRetryDecision Late = Decide(Exact(), Status(503), 0, 10.0, 10.4);
	TestTrue(TEXT("a retry past the deadline: none, saying why"), !Late.bRetry && Late.Reason == FName(TEXT("Deadline")));
	TestTrue(TEXT("one before it: yes"), Decide(Exact(), Status(503), 0, 10.0, 11.0).bRetry);

	// Spec 4.1's verbs
	for (const TCHAR* Verb : { TEXT("GET"), TEXT("HEAD"), TEXT("OPTIONS"), TEXT("PUT"), TEXT("DELETE"), TEXT("get") })
	{
		TestTrue(FString::Printf(TEXT("%s is idempotent"), Verb), IsIdempotentVerb(Verb));
	}
	for (const TCHAR* Verb : { TEXT("POST"), TEXT("PATCH"), TEXT("QUERY"), TEXT("CONNECT") })
	{
		TestFalse(FString::Printf(TEXT("%s is not (spec 4.1 retries it only with Idempotent())"), Verb), IsIdempotentVerb(Verb));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetrySeedTest, "VaRest.Http.Retry.RandomSeed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetrySeedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	// Carry 2 (plan decision 26): WithRandomSeed makes a client's jitter exact, and only the seed decides it
	FTestRig RigA;
	FTestRig RigB;
	FTestRig RigC;
	RigA.Client.WithRandomSeed(1234);
	RigB.Client.WithRandomSeed(1234);
	RigC.Client.WithRandomSeed(4321);
	FRandomStream& A = FClientAccess::State(RigA.Client)->GetRandom();
	FRandomStream& B = FClientAccess::State(RigB.Client)->GetRandom();
	FRandomStream& C = FClientAccess::State(RigC.Client)->GetRandom();
	bool bSame = true;
	bool bDiffer = false;
	for (int32 Index = 0; Index < 16; ++Index)
	{
		const float FromA = A.GetFraction();
		bSame &= FromA == B.GetFraction();
		bDiffer |= FromA != C.GetFraction();
	}
	TestTrue(TEXT("the same seed draws the same sequence"), bSame);
	TestTrue(TEXT("a different seed draws a different one"), bDiffer);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryRecoversTest, "VaRest.Http.Retry.Recovers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryRecoversTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	FTestRig Rig;
	Rig.Client.WithRetry(Exact(3));
	Rig.Fake->Enqueue(Json(503));
	Rig.Fake->Enqueue(Json(503));
	Rig.Fake->Enqueue(Json(200, R"({"ok":1})"));
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/r")), Outcome).Send();
	if (!TestTrue(TEXT("the first attempt, then a backoff timer"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	TestFalse(TEXT("not done while it backs off"), Outcome->IsDone());
	TestEqual(TEXT("... and still in flight"), Rig.Client.GetInFlightCount(), 1);
	Rig.Clock->Advance(0.499);
	Rig.Drain();
	TestEqual(TEXT("no attempt before the delay"), Rig.Fake->NumCalls(), 1);
	Rig.Clock->Advance(0.001);
	if (!TestTrue(TEXT("the second at 0.5 s, then a 1 s backoff"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(1.0);
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	if (!TestEqual(TEXT("three calls"), Rig.Fake->NumCalls(), 3))
	{
		return false;
	}
	TestTrue(TEXT("a success after three attempts"), Outcome->SuccessCalls.load() == 1 && Outcome->ErrorCalls.load() == 0 && Outcome->Response.Attempts == 3);
	TestTrue(TEXT("... with the last attempt's body"), Outcome->Value.IsSet() && Outcome->Value->Get(TEXT("ok")).AsInt() == 1);
	TestEqual(TEXT("each attempt numbered"), Rig.Fake->GetCall(2).Request.Attempt, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryNonIdempotentTest, "VaRest.Http.Retry.NonIdempotentSentOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryNonIdempotentTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Spec 4.1: "Only idempotent verbs (GET, HEAD, OPTIONS, PUT, DELETE) retry by default; POST/PATCH only with .Idempotent()"
	FTestRig Rig;
	Rig.Client.WithRetry(Exact(3));
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> KeyedCalls = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	Rig.Fake->SetResponder([KeyedCalls](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.EndsWith(TEXT("/drop")))
		{
			return FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError"));
		}
		if (Request.Url.EndsWith(TEXT("/keyed")))
		{
			return ++*KeyedCalls == 1 ? Json(503) : Json(200);
		}
		return Json(503);
	});
	auto Order = MakeOutcome<FVaRestJson>();
	auto Dropped = MakeOutcome<FVaRestJson>();
	auto Patched = MakeOutcome<FVaRestJson>();
	auto Keyed = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Post(TEXT("/orders")).Body(TEXT("o")), Order).Send();
	Observe(Rig.Client.Post(TEXT("/drop")).Body(TEXT("o")), Dropped).Send();
	Observe(Rig.Client.Patch(TEXT("/patch")).Body(TEXT("o")), Patched).Send();
	Observe(Rig.Client.Post(TEXT("/keyed")).Body(TEXT("o")).Idempotent(), Keyed).Send();
	if (!TestTrue(TEXT("the three non-idempotent ones finish; the keyed one backs off"),
		PumpUntil([&] { return Order->IsDone() && Dropped->IsDone() && Patched->IsDone() && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	TestEqual(TEXT("POST 503: sent once"), CallsTo(Rig, TEXT("/orders")), 1);
	TestTrue(TEXT("... its error after one attempt (bRetryable states spec 4.1's class, not what was done)"), Order->Error.Kind == EVaRestErrorKind::Http
		&& Order->Error.HttpStatus == 503 && Order->Error.Attempts == 1 && Order->Error.bRetryable);
	TestEqual(TEXT("POST, connection error: sent once"), CallsTo(Rig, TEXT("/drop")), 1);
	TestEqual(TEXT("PATCH 503: sent once"), CallsTo(Rig, TEXT("/patch")), 1);
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("an Idempotent() POST is retried"), Rig.Wait(Keyed)) || !TestEqual(TEXT("... twice in all"), CallsTo(Rig, TEXT("/keyed")), 2))
	{
		Abandon(Rig);
		return false;
	}
	FString Keys[2];
	int32 Found = 0;
	for (int32 Index = 0; Index < Rig.Fake->NumCalls() && Found < 2; ++Index)
	{
		if (Rig.Fake->GetCall(Index).Request.Url.EndsWith(TEXT("/keyed")))
		{
			Keys[Found++] = Rig.Fake->GetCall(Index).Request.Headers.Get(TEXT("Idempotency-Key"));
		}
	}
	TestTrue(TEXT("... with the same Idempotency-Key on both attempts (spec 3.2)"), !Keys[0].IsEmpty() && Keys[0].Equals(Keys[1], ESearchCase::CaseSensitive));
	TestTrue(TEXT("... and succeeds"), Keyed->SuccessCalls.load() == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryGivesUpTest, "VaRest.Http.Retry.GivesUp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryGivesUpTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	FTestRig Rig;
	Rig.Client.WithRetry(Exact(3));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return Json(503); });
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/down")), Outcome).Send();
	for (int32 Step = 0; Step < 3; ++Step)
	{
		if (!TestTrue(FString::Printf(TEXT("backoff %d armed"), Step + 1), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
		{
			Abandon(Rig);
			return false;
		}
		Rig.Clock->Advance(2.0);
	}
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	TestEqual(TEXT("one attempt and three retries"), Rig.Fake->NumCalls(), 4);
	TestTrue(TEXT("the last error, after four attempts"), Outcome->Error.Kind == EVaRestErrorKind::Http && Outcome->Error.HttpStatus == 503 && Outcome->Error.Attempts == 4);
	TestEqual(TEXT("no timer left"), Rig.Clock->NumPendingTimers(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryAfterTraceLogTest, "VaRest.Http.Retry.RetryAfterTraceAndLog",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryAfterTraceLogTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	TGuardConsoleVariable<int32> Level(TEXT("VaRest.LogLevel"), -1);
	FTestRig Rig;
	Rig.Client.WithRetry(Exact(3)).WithLogLevel(EVaRestLogLevel::Basic);
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("-> 503"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FVaRestFakeResponse Busy = Json(503);
	Busy.Headers.Set(TEXT("Retry-After"), TEXT("2"));
	Rig.Fake->Enqueue(Busy);
	Rig.Fake->Enqueue(Json(200));
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/busy")), Outcome).Send();
	if (!TestTrue(TEXT("backoff armed"), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(1.999);
	Rig.Drain();
	TestEqual(TEXT("Retry-After: 2 -- nothing at 1.999 s"), Rig.Fake->NumCalls(), 1);
	Rig.Clock->Advance(0.001);
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("a success on the second attempt"), Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 2);
	const uint64 Id = Handle.GetId();
	TestEqualSensitive(TEXT("IN-4: a Note between the attempts"), Scoped.Recorder->KindsOf(Id), FString(TEXT("Started,Dispatched,Ended,Note,Dispatched,Ended,Completed")));
	TArray<FVaRestTraceAttemptDispatched> Dispatched;
	TArray<FVaRestTraceNote> Notes;
	for (const FVaRestTraceEvent& Event : Scoped.Recorder->For(Id))
	{
		if (Event.Kind == EVaRestTraceKind::AttemptDispatched)
		{
			Dispatched.Add(Event.Payload.Get<FVaRestTraceAttemptDispatched>());
		}
		else if (Event.Kind == EVaRestTraceKind::Note)
		{
			Notes.Add(Event.Payload.Get<FVaRestTraceNote>());
		}
	}
	if (!TestTrue(TEXT("two dispatches and one note"), Dispatched.Num() == 2 && Notes.Num() == 1))
	{
		return false;
	}
	TestTrue(TEXT("the note: RetryScheduled, why, how long, what was asked, for attempt 1"), Notes[0].Kind == EVaRestTraceNoteKind::RetryScheduled
		&& Notes[0].Reason == FName(TEXT("Status503")) && Notes[0].Delay == 2.0 && Notes[0].RetryAfter == 2.0 && Notes[0].Attempt == 1);
	TestTrue(TEXT("IN-2: the first attempt has no gap"), Dispatched[0].GapKind == EVaRestTraceGap::None && Dispatched[0].GapSeconds == 0.0);
	TestTrue(TEXT("... the second a 2 s backoff"), Dispatched[1].GapKind == EVaRestTraceGap::Backoff && Dispatched[1].GapSeconds == 2.0 && Dispatched[1].Attempt == 2);
	TestTrue(TEXT("the log says so, with the request id (IN-16)"),
		Capture.Contains(FString::Printf(TEXT("#%llu VaRest: Status503; retry 1 of 3 in 2000 ms (Retry-After)"), Id)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetrySeededTest, "VaRest.Http.Retry.SeededJitter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetrySeededTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Plan decision 26: the client's stream, seeded, gives the delay a test can compute
	const FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(1);
	FRandomStream Expected(42);
	const double Delay = Policy.Backoff.GetDelay(1, Expected);
	FTestRig Rig;
	Rig.Client.WithRetry(Policy).WithRandomSeed(42);
	Rig.Fake->Enqueue(Json(503));
	Rig.Fake->Enqueue(Json(200));
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/j")), Outcome).Send();
	if (!TestTrue(TEXT("backoff armed"), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	if (Delay > 1e-5)
	{
		Rig.Clock->Advance(Delay - 1e-6);
		Rig.Drain();
		TestEqual(TEXT("not before the seeded delay"), Rig.Fake->NumCalls(), 1);
	}
	Rig.Clock->Advance(2e-5);
	TestTrue(TEXT("at it"), Rig.Wait(Outcome) && Rig.Fake->NumCalls() == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryNotRetriedTest, "VaRest.Http.Retry.NotRetried",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryNotRetriedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	FTestRig Plain;
	Plain.Fake->Enqueue(Json(503));
	auto NoPolicy = MakeOutcome<FVaRestJson>();
	Observe(Plain.Client.Get(TEXT("/p")), NoPolicy).Send();
	TestTrue(TEXT("plan decision 1: a client without WithRetry sends once"), Plain.Wait(NoPolicy) && Plain.Fake->NumCalls() == 1);

	FTestRig Rig;
	Rig.Client.WithRetry(Exact(3));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.EndsWith(TEXT("/text")))
		{
			return FVaRestFakeResponse::Text(200, TEXT("not json"));
		}
		if (Request.Url.EndsWith(TEXT("/manual")))
		{
			return FVaRestFakeResponse::Manual();
		}
		if (Request.Url.EndsWith(TEXT("/opted")))
		{
			// Spec review M-4 (I-4): a status the policy retries, so NoRetry() is what stops it
			return Json(503);
		}
		return Json(404);
	});
	auto Missing = MakeOutcome<FVaRestJson>();
	auto NotJson = MakeOutcome<FVaRestJson>();
	auto Canceled = MakeOutcome<FVaRestJson>();
	auto Opted = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/missing")), Missing).Send();
	Observe(Rig.Client.Get(TEXT("/text")), NotJson).Send();
	const FVaRestRequestHandle Pending = Observe(Rig.Client.Get(TEXT("/manual")), Canceled).Send();
	Observe(Rig.Client.Get(TEXT("/opted")).NoRetry(), Opted).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 4; });
	Pending.Cancel();
	if (!TestTrue(TEXT("all complete"), PumpUntil([&] { return Missing->IsDone() && NotJson->IsDone() && Canceled->IsDone() && Opted->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestEqual(TEXT("404, a decode error, a cancel and a NoRetry() request: one call each"), Rig.Fake->NumCalls(), 4);
	TestEqual(TEXT("... and no backoff armed"), Rig.Clock->NumPendingTimers(), 0);
	TestTrue(TEXT("the kinds are the attempts' own"), Missing->Error.Kind == EVaRestErrorKind::Http && NotJson->Error.Kind == EVaRestErrorKind::Decode
		&& Canceled->Error.Kind == EVaRestErrorKind::Canceled);
	TestTrue(TEXT("NoRetry(): a retryable 503, after one attempt"), Opted->Error.HttpStatus == 503 && Opted->Error.Attempts == 1 && Opted->Error.bRetryable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryGapTest, "VaRest.Http.Retry.GapIsMeasured",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryGapTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Spec review M-10 (Inspector 3.3: a gap runs from the previous attempt's end to this attempt's start): IN-2 reports the
	// time that passed, not the delay that was asked for. Timers 0.25 s late stand for a real-clock ticker a frame late.
	FTestRig Rig;
	Rig.Client.WithRetry(Exact(1)).WithClock(MakeShared<FLateClock, ESPMode::ThreadSafe>(Rig.Clock, 0.25));
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	Rig.Fake->Enqueue(Json(503));
	Rig.Fake->Enqueue(Json(200));
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/late")), Outcome).Send();
	if (!TestTrue(TEXT("the first attempt, then a backoff timer"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(2.0);
	if (!TestTrue(TEXT("completes after one retry"), Rig.Wait(Outcome)) || !TestEqual(TEXT("two calls"), Rig.Fake->NumCalls(), 2))
	{
		Abandon(Rig);
		return false;
	}
	TArray<FVaRestTraceAttemptDispatched> Dispatched;
	double AskedDelay = -1.0;
	for (const FVaRestTraceEvent& Event : Scoped.Recorder->For(Handle.GetId()))
	{
		if (Event.Kind == EVaRestTraceKind::AttemptDispatched)
		{
			Dispatched.Add(Event.Payload.Get<FVaRestTraceAttemptDispatched>());
		}
		else if (Event.Kind == EVaRestTraceKind::Note)
		{
			AskedDelay = Event.Payload.Get<FVaRestTraceNote>().Delay;
		}
	}
	if (!TestEqual(TEXT("two dispatches"), Dispatched.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("the note says what was asked: 0.5 s"), AskedDelay, 0.5);
	TestTrue(TEXT("IN-2: the gap is what passed: 0.75 s"), Dispatched[1].GapKind == EVaRestTraceGap::Backoff && FMath::IsNearlyEqual(Dispatched[1].GapSeconds, 0.75, 1e-9));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryFinishedWaitingTest, "VaRest.Http.Retry.FinishedWhileBackingOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryFinishedWaitingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Concurrency review M-3: a custom Outer stage answers its request while Retry backs off. Complete runs Retry's
	// interrupt, so the backoff timer goes with the request instead of holding it for the whole delay.
	FTestRig Rig;
	const TSharedRef<FHoldingOuter, ESPMode::ThreadSafe> Outer = MakeShared<FHoldingOuter, ESPMode::ThreadSafe>(true);
	Rig.Client.Use(Outer, EVaRestStageSlot::Outer).WithRetry(Exact(3));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return Json(503); });
	auto Outcome = MakeOutcome<void>();
	Observe(Rig.Client.Get<void>(TEXT("/held")), Outcome).Send();
	if (!TestTrue(TEXT("Retry backs off"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Outer->Held.Reset();
		Abandon(Rig);
		return false;
	}
	Outer->AnswerNow(204);
	if (!TestTrue(TEXT("the stage's answer completes the request"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("... as the stage's success"), Outcome->SuccessCalls.load() == 1 && Outcome->ErrorCalls.load() == 0);
	TestEqual(TEXT("the backoff timer went with it"), Rig.Clock->NumPendingTimers(), 0);
	TestEqual(TEXT("... and Retry answered Canceled behind the stage, once"), Outer->LateAnswers->load(), 1);
	Rig.Clock->Advance(5.0);
	Rig.Drain();
	TestEqual(TEXT("no attempt after it"), Rig.Fake->NumCalls(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryIntoDecodeTest, "VaRest.Http.Retry.FinishedIntoDecodeWhileBackingOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryIntoDecodeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Re-review N-6: the same with an answer that is decoded on a task. OnChainDone sets Phase = Decoding long before
	// Complete runs, and a backoff timer firing in that window would start a network attempt for a request already being
	// delivered (Proceed checks only bFinished and the cancel flag); OnChainDone runs the interrupt before the decode starts.
	FTestRig Rig;
	const TSharedRef<FHoldingOuter, ESPMode::ThreadSafe> Outer = MakeShared<FHoldingOuter, ESPMode::ThreadSafe>(true);
	Rig.Client.Use(Outer, EVaRestStageSlot::Outer).WithRetry(Exact(3));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return Json(503); });
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/held")), Outcome).Send();
	if (!TestTrue(TEXT("Retry backs off"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Outer->Held.Reset();
		Abandon(Rig);
		return false;
	}
	Outer->AnswerNow(200, FUtf8StringView(UTF8TEXT(R"({"from":"stage"})")));
	// Synchronously, before any pump: the decode task is launched and the request is not finished, yet the timer is gone
	TestEqual(TEXT("the backoff timer went before the decode started"), Rig.Clock->NumPendingTimers(), 0);
	TestEqual(TEXT("... and Retry answered Canceled behind the stage, once"), Outer->LateAnswers->load(), 1);
	if (!TestTrue(TEXT("the stage's answer is delivered"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... decoded"), Outcome->SuccessCalls.load() == 1 && Outcome->Value.IsSet() && Outcome->Value->Get(TEXT("from")).AsString() == TEXT("stage"));
	Rig.Clock->Advance(5.0);
	Rig.Drain();
	TestEqual(TEXT("no attempt after it"), Rig.Fake->NumCalls(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryLateAttemptTest, "VaRest.Http.Retry.AttemptEndsWhileDecoding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryLateAttemptTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Re-review N-6's window from the other side: a custom Outer stage answers while Retry's attempt is still on the wire,
	// so nothing waits to be interrupted. That attempt's 503 reaches Retry while the stage's answer is decoded; a backoff
	// armed then could start a network attempt for a request already being delivered, so Retry passes the 503 up instead.
	FTestRig Rig;
	// Inline completions: the attempt's answer is queued for the game thread by Complete() below, so it runs ahead of the
	// decode task's delivery, which is queued only after the decode (the game thread's queue is first in, first out)
	Rig.Fake->bCompleteOnWorker = false;
	const TSharedRef<FHoldingOuter, ESPMode::ThreadSafe> Outer = MakeShared<FHoldingOuter, ESPMode::ThreadSafe>(true);
	Rig.Client.Use(Outer, EVaRestStageSlot::Outer).WithRetry(Exact(3));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/late")), Outcome).Send();
	if (!TestEqual(TEXT("the attempt is on the wire"), Rig.Fake->NumCalls(), 1))
	{
		Outer->Held.Reset();
		Abandon(Rig);
		return false;
	}
	Rig.Fake->Complete(0, Json(503));
	Outer->AnswerNow(200, FUtf8StringView(UTF8TEXT(R"({"from":"stage"})")));
	if (!TestTrue(TEXT("the stage's answer is delivered"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("... decoded"), Outcome->SuccessCalls.load() == 1 && Outcome->Value.IsSet() && Outcome->Value->Get(TEXT("from")).AsString() == TEXT("stage"));
	TestEqual(TEXT("the late 503 went up to the stage, once"), Outer->LateAnswers->load(), 1);
	TestEqualSensitive(TEXT("... while the answer was decoded, with no retry scheduled for it"), Scoped.Recorder->KindsOf(Handle.GetId()),
		FString(TEXT("Started,Dispatched,Ended,Completed")));
	TestEqual(TEXT("no backoff armed"), Rig.Clock->NumPendingTimers(), 0);
	Rig.Clock->Advance(5.0);
	Rig.Drain();
	TestEqual(TEXT("no attempt after it"), Rig.Fake->NumCalls(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryDeadHookTest, "VaRest.Http.Retry.InterruptThatCannotAnswer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryDeadHookTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Concurrency review M-4: an interrupt whose stage state is gone answers nothing; Cancel() then finishes the request
	// itself. The stage holds the request without proceeding, and the hook is set by hand: no M4a path frees a waiting
	// stage's state, which is why the fallback exists.
	FTestRig Rig;
	const TSharedRef<FHoldingOuter, ESPMode::ThreadSafe> Outer = MakeShared<FHoldingOuter, ESPMode::ThreadSafe>(false);
	Rig.Client.Use(Outer, EVaRestStageSlot::Outer);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/stuck")), Outcome).Send();
	const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Held = Outer->Kept.Pin();
	if (!TestTrue(TEXT("the stage holds it"), Held.IsValid()))
	{
		Outer->Held.Reset();
		Abandon(Rig);
		return false;
	}
	VaRest::Http::Internals(*Held).Interrupt = [] { return false; };
	Handle.Cancel();
	TestTrue(TEXT("Cancel() still finishes it"), Rig.Wait(Outcome) && Outcome->Error.Kind == EVaRestErrorKind::Canceled && Outcome->ErrorCalls.load() == 1);
	TestEqual(TEXT("... and nothing was sent"), Rig.Fake->NumCalls(), 0);
	// The held answer captures the exchange, whose configuration holds this stage: break the cycle
	Outer->Held.Reset();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryNoteStampTest, "VaRest.Http.Retry.NoteStamped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryNoteStampTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Task 5 review minor 1, carried to the first trace point that emits a Note: TracePoints::Note stamps the exchange's
	// request id, its current attempt and its clock's time. Late in virtual time and on a second attempt, so no stamp can
	// pass as a default.
	FTestRig Rig;
	Rig.Client.WithRetry(Exact(2));
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	Rig.Fake->Enqueue(Json(503));
	Rig.Fake->Enqueue(Json(502));
	Rig.Fake->Enqueue(Json(200));
	Rig.Clock->Advance(3.25);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/stamped")), Outcome).Send();
	if (!TestTrue(TEXT("the first attempt, then a backoff timer"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("the second, then another"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(1.0);
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	const uint64 Id = Handle.GetId();
	TArray<FVaRestTraceNote> Notes;
	TArray<double> Times;
	for (const FVaRestTraceEvent& Event : Scoped.Recorder->For(Id))
	{
		if (Event.Kind == EVaRestTraceKind::Note)
		{
			Notes.Add(Event.Payload.Get<FVaRestTraceNote>());
			Times.Add(Event.Time);
		}
	}
	// For() finds an event by the request id in its payload: a Note without the stamp is not found at all
	if (!TestEqual(TEXT("two notes recorded for the request's id"), Notes.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("the second note: the request's id"), Notes[1].RequestId == Id);
	TestEqual(TEXT("... the attempt that ended, 2"), Notes[1].Attempt, 2);
	TestTrue(TEXT("... the client's clock when it ended: 3.75 s"), FMath::IsNearlyEqual(Times[1], 3.75, 1e-9));
	TestTrue(TEXT("... why and how long"), Notes[1].Reason == FName(TEXT("Status502")) && Notes[1].Delay == 1.0 && Notes[1].RetryAfter < 0.0);
	TestEqual(TEXT("the first: attempt 1"), Notes[0].Attempt, 1);
	TestTrue(TEXT("... at 3.25 s"), FMath::IsNearlyEqual(Times[0], 3.25, 1e-9));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryDefaultsTest, "VaRest.Http.Retry.PolicyDefaults",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	// Task 7 review, carried (ruling Q2): a policy as constructed is spec 4.1's, word for word; a client has none until
	// WithRetry (plan decision 1)
	const FVaRestRetryPolicy Policy;
	TestEqual(TEXT("MaxRetries 3"), Policy.MaxRetries, 3);
	TestEqual(TEXT("BaseDelay 0.5 s"), Policy.Backoff.BaseDelay, 0.5f);
	TestEqual(TEXT("Multiplier 2"), Policy.Backoff.Multiplier, 2.f);
	TestEqual(TEXT("MaxDelay 30 s"), Policy.Backoff.MaxDelay, 30.f);
	TestTrue(TEXT("full jitter"), Policy.Backoff.Jitter == EVaRestJitter::Full);
	TestTrue(TEXT("statuses {408, 425, 429, 500, 502, 503, 504}"), Policy.RetryStatuses == TArray<int32>({ 408, 425, 429, 500, 502, 503, 504 }));
	TestTrue(TEXT("connection errors and timeouts retried"), Policy.bRetryConnectionErrors && Policy.bRetryTimeouts);
	TestTrue(TEXT("Retry-After respected"), Policy.bRespectRetryAfter);
	TestEqual(TEXT("... capped at 60 s"), Policy.MaxRetryAfter, 60.f);
	TestEqual(TEXT("no deadline"), Policy.Deadline, 0.f);
	TestTrue(TEXT("enabled"), Policy.IsEnabled());
	const FVaRestRetryPolicy Made = FVaRestRetryPolicy::Exponential();
	TestTrue(TEXT("Exponential(): the same, three retries"), Made.MaxRetries == 3 && Made.RetryStatuses == Policy.RetryStatuses && Made.Backoff.BaseDelay == 0.5f);
	TestFalse(TEXT("Disabled(): none"), FVaRestRetryPolicy::Disabled().IsEnabled());
	Testing::FTestRig Rig;
	TestFalse(TEXT("a client's own policy: disabled until WithRetry (Q2)"), FClientAccess::State(Rig.Client)->GetConfig()->Retry.IsEnabled());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryDecideEdgesTest, "VaRest.Http.Retry.DecideEdges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryDecideEdgesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::RetryTestPrivate;
	// Task 7 review, carried: the rows DecideRetry's first test left out
	const FDateTime NowUtc(2026, 10, 21, 12, 0, 0);
	auto Decide = [NowUtc](const FVaRestRetryPolicy& With, const FVaRestAttemptOutcome& Outcome, int32 Done, double Now, double DeadlineAt)
	{
		FRandomStream Random(7);
		return DecideRetry(With, Outcome, Done, Now, DeadlineAt, NowUtc, Random);
	};
	const FVaRestRetryPolicy Policy = Exact(3);

	// Plan decision 10 holds for a Retry-After too: what the server asked must also end before the deadline
	const FRetryDecision AskedPast = Decide(Policy, Status(503, TEXT("5")), 0, 10.0, 12.0);
	TestTrue(TEXT("a Retry-After that ends past the deadline: refused, saying why"), !AskedPast.bRetry && AskedPast.Reason == FName(TEXT("Deadline")));
	const FRetryDecision AskedBefore = Decide(Policy, Status(503, TEXT("5")), 0, 10.0, 15.5);
	TestTrue(TEXT("... one that ends before it: waited"), AskedBefore.bRetry && AskedBefore.Delay == 5.0);
	// The boundary is refused: the next attempt would start at the deadline, and decision 10 starts none "at or past" it
	const FRetryDecision AtDeadline = Decide(Policy, Status(503), 0, 10.0, 10.5);
	TestTrue(TEXT("Now + Delay == DeadlineAt: refused"), !AtDeadline.bRetry && AtDeadline.Reason == FName(TEXT("Deadline")));
	TestTrue(TEXT("... a nanosecond more and it is retried"), Decide(Policy, Status(503), 0, 10.0, 10.500000001).bRetry);
	const FRetryDecision NoDeadline = Decide(Policy, Status(429, TEXT("60")), 0, 1e6, 0.0);
	TestTrue(TEXT("DeadlineAt 0: no deadline, however late"), NoDeadline.bRetry && NoDeadline.Delay == 60.0);

	// Retry-After belongs to a retried HTTP status only
	const FRetryDecision Missing = Decide(Policy, Status(404, TEXT("5")), 0, 0.0, 0.0);
	TestTrue(TEXT("Retry-After on a status not retried: no retry, and the header is not read"),
		!Missing.bRetry && Missing.RetryAfter < 0.0 && Missing.Reason == FName(TEXT("Status404")));
	FVaRestAttemptOutcome Dropped = Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError"));
	Dropped.Headers.Set(TEXT("Retry-After"), TEXT("5"));
	const FRetryDecision Reconnect = Decide(Policy, Dropped, 0, 0.0, 0.0);
	TestTrue(TEXT("... nor on a connection failure: the backoff's 0.5 s"), Reconnect.bRetry && Reconnect.Delay == 0.5 && Reconnect.RetryAfter < 0.0);

	FVaRestRetryPolicy Short = Policy;
	Short.MaxRetryAfter = 10.f;
	const FRetryDecision Capped = Decide(Short, Status(503, TEXT("30")), 0, 0.0, 0.0);
	TestTrue(TEXT("MaxRetryAfter 10: a Retry-After of 30 waits 10, and says 30 was asked"), Capped.bRetry && Capped.Delay == 10.0 && Capped.RetryAfter == 30.0);
	TestEqual(TEXT("... one under that cap waits what it asked"), Decide(Short, Status(503, TEXT("7")), 0, 0.0, 0.0).Delay, 7.0);

	const FRetryDecision Last = Decide(Policy, Status(503), 2, 0.0, 0.0);
	TestTrue(TEXT("RetriesDone == MaxRetries - 1: the last retry, after the third step's 2 s"), Last.bRetry && Last.Delay == 2.0);
	TestFalse(TEXT("RetriesDone == MaxRetries: none"), Decide(Policy, Status(503), 3, 0.0, 0.0).bRetry);

	FVaRestRetryPolicy Odd = Policy;
	Odd.RetryStatuses.Add(200);
	TestFalse(TEXT("a success status listed in RetryStatuses has no effect: a 200 is never retried"), Decide(Odd, Status(200), 0, 0.0, 0.0).bRetry);
	return true;
}

#endif
