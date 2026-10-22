// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "VaRestAuth.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestExchangeState.h"
#include "VaRestHttpSettings.h"
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
			if ((!OnlyTag.IsNone() && Exchange->GetTag() != OnlyTag) || ++Passes > HoldPasses)
			{
				Next.Proceed(Exchange, MoveTemp(OnOutcome));
				return;
			}
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
		/** Set: only requests with this tag are held; the others pass through */
		FName OnlyTag;
		/** How many passes are held (in the PerAttempt slot: attempts); later ones pass through, their outcomes going up */
		int32 HoldPasses = MAX_int32;
		int32 Passes = 0;
		FVaRestOutcomeCallback Held;
		TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe> Kept;
		/** What reached this stage from behind it after it had answered */
		TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> LateAnswers = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	};

	/** Carry C1: a custom auth that signs each attempt into the query (try=1, try=2, ...), as a nonce or timestamp would */
	class FAttemptQueryAuth final : public IVaRestAuth
	{
	public:
		virtual FName GetAuthName() const override { return TEXT("AttemptQuery"); }
		virtual bool Apply(FVaRestAuthTarget& Target) const override
		{
			Target.Query.Emplace(TEXT("try"), FString::FromInt(++Applied));
			return true;
		}

		mutable int32 Applied = 0;
	};

	/** A PerAttempt stage that answers its first attempt itself, inside Process, with Code; later attempts go through */
	class FAnswerFirstAttempt final : public IVaRestStage
	{
	public:
		explicit FAnswerFirstAttempt(int32 InCode)
			: Code(InCode)
		{
		}

		virtual FName GetStageName() const override { return TEXT("AnswerFirstAttempt"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			if (++*Calls == 1)
			{
				FVaRestAttemptOutcome Early;
				Early.Status = Code;
				OnOutcome(MoveTemp(Early));
				return;
			}
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}

		const int32 Code;
		TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Calls = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	};

	/**
	 * An Outer stage that answers its request itself inside Process (so inside a game-thread Send(), where Complete defers,
	 * decision 3), after proceeding or not. It keeps what a misbehaving stage would: its Next, to proceed again after it
	 * answered, and its outcome callback, to report twice (review C-3). Both hold the exchange or the chain: Release() at
	 * the end of a test.
	 */
	class FEagerOuter final : public IVaRestStage
	{
	public:
		FEagerOuter(bool bInProceed, int32 InCode)
			: bProceed(bInProceed)
			, Code(InCode)
		{
		}

		virtual FName GetStageName() const override { return TEXT("EagerOuter"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Kept = Exchange;
			KeptNext.Emplace(Next);
			Done = MakeShared<FVaRestOutcomeCallback, ESPMode::ThreadSafe>(MoveTemp(OnOutcome));
			if (bProceed)
			{
				Next.Proceed(Exchange, [Late = LateAnswers](FVaRestAttemptOutcome&&) { ++*Late; });
			}
			FVaRestAttemptOutcome Own;
			Own.Status = Code;
			if (!JsonBody.IsEmpty())
			{
				// An answer the pipeline decodes on a task: the request is in Phase Decoding until it comes back
				Own.ContentType = TEXT("application/json");
				Own.Body.Append(reinterpret_cast<const uint8*>(JsonBody.GetData()), JsonBody.Len());
			}
			(*Done)(MoveTemp(Own));
		}

		/** Proceeds again after answering; what comes back Canceled is counted in AgainCanceled */
		void ProceedAgain()
		{
			const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Pinned = Kept.Pin();
			if (!Pinned.IsValid() || !KeptNext.IsSet())
			{
				return;
			}
			KeptNext.GetValue().Proceed(Pinned.ToSharedRef(), [Again = AgainCanceled](FVaRestAttemptOutcome&& Back)
			{
				*Again += Back.Failure == EVaRestTransportFailure::Canceled ? 1 : 0;
			});
		}

		/** Reports a second outcome, Canceled */
		void ReportAgain()
		{
			if (Done.IsValid())
			{
				(*Done)(FVaRestAttemptOutcome::Canceled());
			}
		}

		void Release()
		{
			Done.Reset();
			KeptNext.Reset();
		}

		const bool bProceed;
		const int32 Code;
		/** Set (to a literal): the answer carries this JSON body */
		FUtf8StringView JsonBody;
		TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe> Kept;
		TOptional<FVaRestStageNext> KeptNext;
		TSharedPtr<FVaRestOutcomeCallback, ESPMode::ThreadSafe> Done;
		TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> LateAnswers = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
		TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> AgainCanceled = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
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
	// configuration refused, Task 17) are Kind=Config and never retried. M4a final review A9 (review A M-5): every other
	// Config and Auth detail ClassifyAttempt knows (VaRestClassify.cpp:27-28, :38) is listed too.
	const TCHAR* Never[] = { TEXT("Cancelled"), TEXT("InvalidHeader"), TEXT("TransportDropped"), TEXT("AuthRejected"), TEXT("HttpDisabled"), TEXT("ResponseTooLarge"),
		TEXT("AuthInvalid"), TEXT("AuthRefreshConfig"), TEXT("DomainNotAllowed"), TEXT("BodyFileMissing"), TEXT("UploadReadFailed"), TEXT("BodyRejected"),
		TEXT("NoCredentials"), TEXT("SessionCleared") };
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
	if (!TestTrue(TEXT("at it"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	TestEqual(TEXT("... the second call"), Rig.Fake->NumCalls(), 2);
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
	if (!TestTrue(TEXT("a client without WithRetry: the request completes"), Plain.Wait(NoPolicy)))
	{
		Abandon(Plain);
		return false;
	}
	TestEqual(TEXT("plan decision 1: a client without WithRetry sends once"), Plain.Fake->NumCalls(), 1);

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
	if (!TestTrue(TEXT("Cancel() still finishes it"), Rig.Wait(Outcome)))
	{
		Outer->Held.Reset();
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... with one OnError(Canceled)"), Outcome->Error.Kind == EVaRestErrorKind::Canceled && Outcome->ErrorCalls.load() == 1);
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

	// Task 8 review minor 4: the first Retry-After line is read (FVaRestHeaders::Find); a list folded into one line --
	// NSURLSession joins repeated lines on Mac and iOS (AppleHttp.cpp:55) -- is no delay, so the backoff applies
	FVaRestAttemptOutcome Twice = Status(503);
	Twice.Headers.Add(TEXT("Retry-After"), TEXT("2"));
	Twice.Headers.Add(TEXT("Retry-After"), TEXT("5"));
	const FRetryDecision FirstLine = Decide(Policy, Twice, 0, 0.0, 0.0);
	TestTrue(TEXT("two Retry-After lines: the first is read"), FirstLine.bRetry && FirstLine.Delay == 2.0 && FirstLine.RetryAfter == 2.0);
	const FRetryDecision Folded = Decide(Policy, Status(503, TEXT("2, 5")), 0, 0.0, 0.0);
	TestTrue(TEXT("... one folded line \"2, 5\" is not a delay: the backoff's 0.5 s"), Folded.bRetry && Folded.Delay == 0.5 && Folded.RetryAfter < 0.0);

	FVaRestRetryPolicy Odd = Policy;
	Odd.RetryStatuses.Add(200);
	TestFalse(TEXT("a success status listed in RetryStatuses has no effect: a 200 is never retried"), Decide(Odd, Status(200), 0, 0.0, 0.0).bRetry);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryCancelEarlyTest, "VaRest.Http.Retry.CancelAfterEarlyAnswer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryCancelEarlyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Task 8 review I-1: a PerAttempt stage answers its attempt itself while the transport call still runs (its own timeout,
	// VaRestClient.h WithMaxConcurrent), Retry backs off, and the request still holds that stale call. Cancel() and
	// shutdown must end the wait anyway: the timer goes and every Outer stage gets its one Canceled, inside the call.
	FTestRig Rig;
	const TSharedRef<FOutcomeProbe, ESPMode::ThreadSafe> Probe = MakeShared<FOutcomeProbe, ESPMode::ThreadSafe>();
	const TSharedRef<FHoldingOuter, ESPMode::ThreadSafe> Early = MakeShared<FHoldingOuter, ESPMode::ThreadSafe>(true);
	Rig.Client.Use(Probe, EVaRestStageSlot::Outer).Use(Early, EVaRestStageSlot::PerAttempt).WithRetry(Exact(3));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });

	auto Canceled = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/early")), Canceled).Send();
	if (!TestEqual(TEXT("the call is on the wire"), Rig.Fake->NumCalls(), 1))
	{
		Early->Held.Reset();
		Abandon(Rig);
		return false;
	}
	Early->AnswerNow(503);
	if (!TestEqual(TEXT("the stage's own 503: Retry backs off, the call still running"), Rig.Clock->NumPendingTimers(), 1))
	{
		Abandon(Rig);
		return false;
	}
	Handle.Cancel();
	// Inside Cancel(), before any pump
	TestEqual(TEXT("Cancel() ends the wait at once: no timer"), Rig.Clock->NumPendingTimers(), 0);
	TestEqual(TEXT("... and the Outer stage got its one Canceled inside Cancel()"), Probe->Count(Handle.GetId(), EVaRestTransportFailure::Canceled), 1);
	if (!TestTrue(TEXT("the request ends without the clock moving"), Rig.Wait(Canceled)))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... one OnError(Canceled), one OnComplete"), Canceled->ErrorCalls.load() == 1 && Canceled->CompleteCalls.load() == 1
		&& Canceled->Error.Kind == EVaRestErrorKind::Canceled);

	auto Silent = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Second = Observe(Rig.Client.Get(TEXT("/exit")), Silent).Send();
	if (!TestEqual(TEXT("a second call on the wire"), Rig.Fake->NumCalls(), 2))
	{
		Early->Held.Reset();
		Abandon(Rig);
		return false;
	}
	Early->AnswerNow(503);
	if (!TestEqual(TEXT("... it backs off too"), Rig.Clock->NumPendingTimers(), 1))
	{
		Abandon(Rig);
		return false;
	}
	VaRest::Http::FClientAccess::State(Rig.Client)->CancelAll(NAME_None, EVaRestCancelReason::Shutdown);
	TestEqual(TEXT("CancelAll(Shutdown) leaves no armed timer"), Rig.Clock->NumPendingTimers(), 0);
	TestEqual(TEXT("... and its Outer stage got one Canceled"), Probe->Count(Second.GetId(), EVaRestTransportFailure::Canceled), 1);
	if (!TestTrue(TEXT("the shut-down request ends"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestEqual(TEXT("shutdown: no callbacks (decision 2)"), Silent->CompleteCalls.load() + Silent->ErrorCalls.load() + Silent->SuccessCalls.load(), 0);
	TestTrue(TEXT("each request: one outcome for the Outer stage"), Probe->Total(Handle.GetId()) == 1 && Probe->Total(Second.GetId()) == 1);
	TestEqual(TEXT("the stale calls were canceled; their answers reached only the stage that had answered"), Early->LateAnswers->load(), 2);
	TestEqual(TEXT("no attempt after the stops"), Rig.Fake->NumCalls(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryQueuedCompletionTest, "VaRest.Http.Retry.BackoffBeforeQueuedCompletion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryQueuedCompletionTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Task 8 review minor 1: inside a game-thread Send(), a PerAttempt stage answers 503 itself, Retry arms a backoff, and
	// the Outer stage in front answers the request itself with no body. Complete defers its delivery (decision 3), so the
	// request is neither finished nor decoding; a backoff that fired before that delivery would start a network attempt
	// for an answered request. The deferred answer ends the wait at once, and nothing proceeds after it.
	FTestRig Rig;
	const TSharedRef<FEagerOuter, ESPMode::ThreadSafe> Outer = MakeShared<FEagerOuter, ESPMode::ThreadSafe>(true, 204);
	const TSharedRef<FAnswerFirstAttempt, ESPMode::ThreadSafe> Breaker = MakeShared<FAnswerFirstAttempt, ESPMode::ThreadSafe>(503);
	Rig.Client.Use(Outer, EVaRestStageSlot::Outer).Use(Breaker, EVaRestStageSlot::PerAttempt).WithRetry(Exact(3));
	auto Outcome = MakeOutcome<void>();
	Observe(Rig.Client.Get<void>(TEXT("/eager")), Outcome).Send();
	// Before any pump: the completion is queued, not run
	TestFalse(TEXT("the answer is delivered later (decision 3)"), Outcome->IsDone());
	TestEqual(TEXT("the queued answer ended the backoff at once"), Rig.Clock->NumPendingTimers(), 0);
	TestEqual(TEXT("... and Retry answered Canceled behind the stage, once"), Outer->LateAnswers->load(), 1);
	// The virtual clock moves before the queued delivery runs: what a backoff timer would have done, it does now
	Rig.Clock->Advance(1.0);
	if (!TestTrue(TEXT("the stage's answer is delivered"), Rig.Wait(Outcome)))
	{
		Outer->Release();
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("... as the stage's success"), Outcome->SuccessCalls.load() == 1 && Outcome->ErrorCalls.load() == 0);
	TestEqual(TEXT("the PerAttempt stage ran once"), Breaker->Calls->load(), 1);
	TestEqual(TEXT("no network attempt"), Rig.Fake->NumCalls(), 0);
	Outer->Release();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryQueuedRefusesTest, "VaRest.Http.Retry.QueuedCompletionRefusesMore",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryQueuedRefusesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Task 8 review minor 1: a completion that Complete queued inside Send() is an accepted answer. Until it runs, a stage
	// that proceeds again starts no attempt (Proceed), and a second outcome is ignored (OnChainDone, review C-3), so a
	// Canceled cannot be delivered ahead of the answer.
	FTestRig Rig;
	const TSharedRef<FEagerOuter, ESPMode::ThreadSafe> Outer = MakeShared<FEagerOuter, ESPMode::ThreadSafe>(false, 204);
	Rig.Client.Use(Outer, EVaRestStageSlot::Outer);
	auto Outcome = MakeOutcome<void>();
	Observe(Rig.Client.Get<void>(TEXT("/again")), Outcome).Send();
	if (!TestFalse(TEXT("the answer is queued, not delivered"), Outcome->IsDone()))
	{
		Outer->Release();
		Abandon(Rig);
		return false;
	}
	Outer->ProceedAgain();
	TestEqual(TEXT("a stage proceeding after its answer: Canceled at once"), Outer->AgainCanceled->load(), 1);
	TestEqual(TEXT("... and no network attempt"), Rig.Fake->NumCalls(), 0);
	Outer->ReportAgain();
	TestEqual(TEXT("a second outcome before the queued answer runs: nothing delivered for it"), Outcome->CompleteCalls.load(), 0);
	if (!TestTrue(TEXT("the queued answer is delivered"), Rig.Wait(Outcome)))
	{
		Outer->Release();
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("... the stage's success, once, and no error"), Outcome->SuccessCalls.load() == 1 && Outcome->ErrorCalls.load() == 0
		&& Outcome->CompleteCalls.load() == 1);
	TestEqual(TEXT("no network attempt at all"), Rig.Fake->NumCalls(), 0);
	Outer->Release();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryQueuedWhileDecodingTest, "VaRest.Http.Retry.QueuedRetryWhileDecoding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryQueuedWhileDecodingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Task 8 review minor 3 (re-review N-6's family): Retry's re-issue waits in the limiter's queue when a custom Outer
	// stage answers the request with a body. A slot freed while that answer is decoded must not dispatch the queued
	// attempt: the answer takes it out of the queue.
	FTestRig Rig;
	// Inline completions: the blocker's answer is queued for the game thread below, ahead of the decode task's delivery
	Rig.Fake->bCompleteOnWorker = false;
	const TSharedRef<FHoldingOuter, ESPMode::ThreadSafe> Outer = MakeShared<FHoldingOuter, ESPMode::ThreadSafe>(true);
	Outer->OnlyTag = TEXT("Held");
	Rig.Client.Use(Outer, EVaRestStageSlot::Outer).WithRetry(Exact(3)).WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		return Request.Url.EndsWith(TEXT("/blocker")) ? FVaRestFakeResponse::Manual() : Json(503);
	});
	VaRest::Http::FLimiter& Limiter = VaRest::Http::FClientAccess::State(Rig.Client)->GetLimiter();
	auto Answered = MakeOutcome<FVaRestJson>();
	auto Blocker = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/held")).Tag(TEXT("Held")), Answered).Send();
	if (!TestTrue(TEXT("the first attempt, then a backoff"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Outer->Held.Reset();
		Abandon(Rig);
		return false;
	}
	Observe(Rig.Client.Get(TEXT("/blocker")), Blocker).Send();
	if (!TestEqual(TEXT("the blocker takes the one slot"), Rig.Fake->NumCalls(), 2))
	{
		Outer->Held.Reset();
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.5);
	if (!TestEqual(TEXT("the retry waits in the limiter's queue"), Limiter.NumQueued(), 1))
	{
		Outer->Held.Reset();
		Abandon(Rig);
		return false;
	}
	Rig.Fake->Complete(1, Json(200));
	Outer->AnswerNow(200, FUtf8StringView(UTF8TEXT(R"({"from":"stage"})")));
	TestEqual(TEXT("the stage's answer took the retry out of the queue"), Limiter.NumQueued(), 0);
	TestEqual(TEXT("... which answered Canceled behind the stage, once"), Outer->LateAnswers->load(), 1);
	if (!TestTrue(TEXT("both delivered"), PumpUntil([Answered, Blocker] { return Answered->IsDone() && Blocker->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestEqual(TEXT("the freed slot dispatched nothing for the answered request"), Rig.Fake->NumCalls(), 2);
	TestTrue(TEXT("the stage's answer, decoded"), Answered->SuccessCalls.load() == 1 && Answered->Value.IsSet()
		&& Answered->Value->Get(TEXT("from")).AsString() == TEXT("stage"));
	TestTrue(TEXT("the blocker's own answer"), Blocker->SuccessCalls.load() == 1);
	TestEqual(TEXT("nothing more behind the stage"), Outer->LateAnswers->load(), 1);
	return true;
}

namespace VaRest::Http::RetryTestPrivate
{
	/** A PerAttempt stage that holds every attempt Delay seconds on Clock before passing it on (re-review N-5) */
	class FDeferringStage final : public IVaRestStage
	{
	public:
		FDeferringStage(TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> InClock, double InDelay)
			: Clock(MoveTemp(InClock))
			, Delay(InDelay)
		{
		}

		virtual FName GetStageName() const override { return TEXT("Deferring"); }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			// The timer owns the continuation until the test advances the clock
			Clock->SetTimer(Delay, [Exchange, Next, Done = MoveTemp(OnOutcome)]() mutable
			{
				Next.Proceed(Exchange, MoveTemp(Done));
			});
		}

	private:
		TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock;
		double Delay = 0.0;
	};

	/**
	 * Virtual time whose timers fire early: a core ticker fed game-time deltas that run faster than Now(), as a fixed
	 * timestep can (FVaRestRealClock; Task 9 review minor 2). A timer fires after Ratio of its delay, but at least MinStep
	 * later -- a frame.
	 */
	class FEarlyClock final : public IVaRestClock
	{
	public:
		FEarlyClock(TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> InTime, double InRatio, double InMinStep)
			: Time(MoveTemp(InTime))
			, Ratio(InRatio)
			, MinStep(InMinStep)
		{
		}

		virtual double Now() const override { return Time->Now(); }
		virtual FVaRestTimerHandle SetTimer(double DelaySeconds, TUniqueFunction<void()>&& Callback) override
		{
			return Time->SetTimer(FMath::Max(DelaySeconds * Ratio, MinStep), MoveTemp(Callback));
		}
		virtual void ClearTimer(FVaRestTimerHandle& Handle) override { Time->ClearTimer(Handle); }

	private:
		TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Time;
		double Ratio = 1.0;
		double MinStep = 0.0;
	};

	/**
	 * A PerAttempt stage that breaks its contract (IVaRestStage::Process: "Call OnOutcome exactly once"): it answers each
	 * attempt twice, inside Process, with Code (Task 8 re-review, out of scope 2)
	 */
	class FReportsTwice final : public IVaRestStage
	{
	public:
		explicit FReportsTwice(int32 InCode)
			: Code(InCode)
		{
		}

		virtual FName GetStageName() const override { return TEXT("ReportsTwice"); }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			++*Calls;
			for (int32 Report = 0; Report < 2; ++Report)
			{
				FVaRestAttemptOutcome Own;
				Own.Status = Code;
				OnOutcome(MoveTemp(Own));
			}
		}

		const int32 Code;
		TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Calls = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryDeadlineTest, "VaRest.Http.Retry.Deadline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryDeadlineTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Plan decision 10: t=0 attempt 1 (503), wait 1 s; t=1 attempt 2 (503), a 2 s wait would end at 3 >= 2.5: no retry
	FTestRig Rig;
	FVaRestRetryPolicy Policy = Exact(5).WithDeadline(2.5f);
	Policy.Backoff.BaseDelay = 1.f;
	Rig.Client.WithRetry(Policy).WithTimeout(10.f);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return Json(503); });
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/d")), Outcome).Send();
	if (!TestTrue(TEXT("first backoff armed"), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(1.0);
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)) || !TestEqual(TEXT("two attempts: the third would start past the deadline"), Rig.Fake->NumCalls(), 2))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("the last answer is the error"), Outcome->Error.HttpStatus == 503 && Outcome->Error.Attempts == 2);
	TestTrue(TEXT("each attempt's total timeout is cut to the time left: 2.5 s, then 1.5 s"),
		FMath::IsNearlyEqual(Rig.Fake->GetCall(0).Request.Timeout, 2.5f, 1e-4f) && FMath::IsNearlyEqual(Rig.Fake->GetCall(1).Request.Timeout, 1.5f, 1e-4f));

	// Waiting in the limiter counts (spec 4.7 "across attempts" from the request's start)
	FTestRig Queue;
	Queue.Client.WithMaxConcurrent(1).WithRetry(Exact(3).WithDeadline(1.f));
	Queue.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Url.EndsWith(TEXT("/hold")) ? FVaRestFakeResponse::Manual() : Json(200); });
	auto Hold = MakeOutcome<FVaRestJson>();
	auto Late = MakeOutcome<FVaRestJson>();
	Observe(Queue.Client.Get(TEXT("/hold")).NoRetry(), Hold).Send();
	Observe(Queue.Client.Get(TEXT("/late")), Late).Send();
	if (!TestTrue(TEXT("one holds the slot"), PumpUntil([&Queue] { return Queue.Fake->NumCalls() == 1; })))
	{
		Abandon(Queue);
		return false;
	}
	Queue.Clock->Advance(1.5);
	// Concurrency review M-1: armed in the queue, not only checked at the grant -- it ends at 1 s while the slot is held
	if (!TestTrue(TEXT("the late one ends at its deadline, while the slot is still held"), PumpUntil([Late] { return Late->IsDone(); })))
	{
		Abandon(Queue);
		return false;
	}
	Queue.Fake->Complete(0, Json(200));
	if (!TestTrue(TEXT("both complete"), PumpUntil([Hold, Late] { return Hold->IsDone() && Late->IsDone(); })))
	{
		Abandon(Queue);
		return false;
	}
	Queue.Drain();
	TestEqual(TEXT("the late one never reached the transport"), Queue.Fake->NumCalls(), 1);
	TestEqual(TEXT("... and its deadline timer is gone"), Queue.Clock->NumPendingTimers(), 0);
	TestTrue(TEXT("... it is a Timeout, not retryable, saying why"), Late->Error.Kind == EVaRestErrorKind::Timeout && !Late->Error.bRetryable
		&& Late->Error.Message.Contains(TEXT("Deadline")));

	// Re-review N-5: an attempt a PerAttempt stage holds past the deadline (2 s on a 1 s deadline) is refused by the
	// transport stage's check at dispatch -- the one path to it now that the queue's own timer ends a queued wait
	FTestRig Held;
	Held.Client.WithRetry(Exact(3).WithDeadline(1.f)).Use(MakeShared<FDeferringStage, ESPMode::ThreadSafe>(Held.Clock, 2.0), EVaRestStageSlot::PerAttempt);
	auto Deferred = MakeOutcome<FVaRestJson>();
	Observe(Held.Client.Get(TEXT("/deferred")), Deferred).Send();
	if (!TestTrue(TEXT("the stage holds the attempt"), PumpUntil([&Held] { return Held.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Held);
		return false;
	}
	Held.Clock->Advance(2.0);
	if (!TestTrue(TEXT("the deferred attempt completes"), Held.Wait(Deferred)))
	{
		Abandon(Held);
		return false;
	}
	TestTrue(TEXT("... as Timeout, not retryable, saying Deadline"), Deferred->Error.Kind == EVaRestErrorKind::Timeout && !Deferred->Error.bRetryable
		&& Deferred->Error.Message.Contains(TEXT("Deadline")));
	TestEqual(TEXT("... and never sent"), Held.Fake->NumCalls(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryQueueTest, "VaRest.Http.Retry.QueuesBehindWaiting",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryQueueTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Carry (c), spec 4.6: "queued retries wait their turn too". A's retry has no delay; B and C wait in the limiter
	// when A fails, so the retry goes behind them: a, b, c, a.
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	FVaRestRetryPolicy Instant = Exact(3);
	Instant.Backoff.BaseDelay = 0.f;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto A = MakeOutcome<FVaRestJson>();
	auto B = MakeOutcome<FVaRestJson>();
	auto C = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")).Retry(Instant), A).Send();
	Observe(Rig.Client.Get(TEXT("/b")), B).Send();
	Observe(Rig.Client.Get(TEXT("/c")), C).Send();
	if (!TestTrue(TEXT("A holds the slot"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Fake->Complete(0, Json(503));
	if (!TestTrue(TEXT("B gets the slot A freed; A's retry is armed"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.0);
	Rig.Drain();
	TestEqual(TEXT("the retry waits behind C"), Rig.Fake->NumCalls(), 2);
	for (int32 Index = 1; Index < 4; ++Index)
	{
		Rig.Fake->Complete(Index, Json(200));
		if (Index < 3 && !TestTrue(FString::Printf(TEXT("call %d"), Index + 2), PumpUntil([&Rig, Index] { return Rig.Fake->NumCalls() == Index + 2; })))
		{
			Abandon(Rig);
			return false;
		}
	}
	if (!TestTrue(TEXT("all complete"), PumpUntil([A, B, C] { return A->IsDone() && B->IsDone() && C->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	TArray<FString> Order;
	for (int32 Index = 0; Index < Rig.Fake->NumCalls(); ++Index)
	{
		Order.Add(Rig.Fake->GetCall(Index).Request.Url.RightChop(FString(TEXT("https://api.test/")).Len()));
	}
	TestEqualSensitive(TEXT("dispatch order"), FString::Join(Order, TEXT(",")), FString(TEXT("a,b,c,a")));
	TestTrue(TEXT("A succeeds on its retry"), A->SuccessCalls.load() == 1 && A->Response.Attempts == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryTimeoutTest, "VaRest.Http.Retry.TimeoutPerAttempt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Spec 4.7: the timeout is per attempt; without a deadline the request lasts 1 + 0.5 + 1 + 1 + 1 = 4.5 s
	FTestRig Rig;
	Rig.Client.WithTimeout(1.f).WithRetry(Exact(2));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/slow")), Outcome).Send();
	const double Waits[] = { 0.5, 1.0 };
	for (int32 Step = 0; Step < 3; ++Step)
	{
		if (!TestTrue(FString::Printf(TEXT("attempt %d starts"), Step + 1), PumpUntil([&Rig, Step] { return Rig.Fake->NumCalls() == Step + 1; })))
		{
			Abandon(Rig);
			return false;
		}
		Rig.Clock->Advance(1.0);
		if (Step < 2)
		{
			if (!TestTrue(FString::Printf(TEXT("attempt %d timed out; backoff armed"), Step + 1), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
			{
				Abandon(Rig);
				return false;
			}
			Rig.Clock->Advance(Waits[Step]);
		}
	}
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("a Timeout after three attempts"), Outcome->Error.Kind == EVaRestErrorKind::Timeout && Outcome->Error.Attempts == 3);
	bool bEachOne = true;
	for (int32 Index = 0; Index < Rig.Fake->NumCalls(); ++Index)
	{
		bEachOne &= Rig.Fake->GetCall(Index).Request.Timeout == 1.f;
	}
	TestTrue(TEXT("every attempt has the whole per-attempt timeout"), bEachOne);
	TestTrue(TEXT("elapsed: three timeouts and two waits"), FMath::IsNearlyEqual(Outcome->Response.Elapsed, 4.5, 1e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryStopTest, "VaRest.Http.Retry.StopWhileBackingOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryStopTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Plan decision 7: Cancel(), shutdown and the owner's destruction reach a request that waits on a backoff timer, and
	// an Outer stage in front of Retry gets exactly one outcome for each (concurrency review I-5: the probe)
	FTestRig Rig;
	const TSharedRef<FOutcomeProbe, ESPMode::ThreadSafe> Probe = MakeShared<FOutcomeProbe, ESPMode::ThreadSafe>();
	Rig.Client.Use(Probe, EVaRestStageSlot::Outer).WithRetry(Exact(3));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return Json(503); });
	UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	auto Canceled = MakeOutcome<FVaRestJson>();
	auto Owned = MakeOutcome<FVaRestJson>();
	auto ShutDown = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/c")), Canceled).Send();
	const FVaRestRequestHandle OwnedHandle = Observe(Rig.Client.Get(TEXT("/o")).Owner(Owner), Owned).Send();
	const FVaRestRequestHandle ShutDownHandle = Observe(Rig.Client.Get(TEXT("/s")).Tag(TEXT("Exit")), ShutDown).Send();
	if (!TestTrue(TEXT("all three back off"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3 && Rig.Clock->NumPendingTimers() == 3; })))
	{
		Abandon(Rig);
		return false;
	}

	Handle.Cancel();
	// Inside Cancel(), before any pump: the interrupt answered through the chain and cleared the timer (plan decision 7)
	TestEqual(TEXT("Cancel() clears the timer at once"), Rig.Clock->NumPendingTimers(), 2);
	TestEqual(TEXT("... and the Outer stage got its one Canceled inside Cancel()"), Probe->Count(Handle.GetId(), EVaRestTransportFailure::Canceled), 1);
	if (!TestTrue(TEXT("Cancel() ends the wait"), PumpUntil([Canceled] { return Canceled->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... one OnError(Canceled), one OnComplete"), Canceled->ErrorCalls.load() == 1 && Canceled->CompleteCalls.load() == 1
		&& Canceled->Error.Kind == EVaRestErrorKind::Canceled);

	VaRest::Http::FClientAccess::State(Rig.Client)->CancelAll(TEXT("Exit"), EVaRestCancelReason::Shutdown);
	Rig.Drain();
	TestEqual(TEXT("shutdown: its timer is gone too"), Rig.Clock->NumPendingTimers(), 1);

	Owner->MarkAsGarbage();
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("the owner's request ends when its timer fires"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestEqual(TEXT("no attempt after any stop"), Rig.Fake->NumCalls(), 3);
	TestEqual(TEXT("shutdown and owner gone: no callbacks (spec 3.2, decision 2)"), ShutDown->CompleteCalls.load() + ShutDown->ErrorCalls.load()
		+ Owned->CompleteCalls.load() + Owned->ErrorCalls.load(), 0);
	TestEqual(TEXT("nothing left armed"), Rig.Clock->NumPendingTimers(), 0);
	TestTrue(TEXT("each stop: one outcome for the Outer stage, a Canceled"), Probe->Total(Handle.GetId()) == 1 && Probe->Total(OwnedHandle.GetId()) == 1
		&& Probe->Total(ShutDownHandle.GetId()) == 1 && Probe->Count(OwnedHandle.GetId(), EVaRestTransportFailure::Canceled) == 1
		&& Probe->Count(ShutDownHandle.GetId(), EVaRestTransportFailure::Canceled) == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryBodiesTest, "VaRest.Http.Retry.BodiesResent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryBodiesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Plan decisions 8 and 24 (carry (e)): every re-issue is a new transport request with the whole body -- a JSON body,
	// a multipart body with a file part, a BodyFile -- and a SaveTo keeps only the final attempt's file
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaRestRetryTest") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Dir, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Dir, false, true); };
	TArray<uint8> Pattern;
	for (int32 Index = 0; Index < 1000; ++Index)
	{
		Pattern.Add(static_cast<uint8>(Index * 7 + 3));
	}
	const FString PartFile = Dir / TEXT("part.bin");
	const FString UploadFile = Dir / TEXT("upload.bin");
	const FString Destination = Dir / TEXT("download.bin");
	if (!TestTrue(TEXT("fixtures written"), FFileHelper::SaveArrayToFile(Pattern, *PartFile) && FFileHelper::SaveArrayToFile(Pattern, *UploadFile)))
	{
		return false;
	}
	FTestRig Rig;
	Rig.Client.WithRetry(Exact(1));
	TSharedRef<TMap<FString, int32>, ESPMode::ThreadSafe> Seen = MakeShared<TMap<FString, int32>, ESPMode::ThreadSafe>();
	Rig.Fake->SetResponder([Seen](const FVaRestTransportRequest& Request)
	{
		const bool bFirst = ++Seen->FindOrAdd(Request.Url) == 1;
		if (Request.Url.EndsWith(TEXT("/download")))
		{
			return bFirst ? FVaRestFakeResponse::Text(503, TEXT("busy")) : FVaRestFakeResponse::Bytes(200, TArray<uint8>{ 'o', 'k' });
		}
		return bFirst ? Json(503) : Json(200);
	});
	auto Json1 = MakeOutcome<FVaRestJson>();
	auto Parts = MakeOutcome<FVaRestJson>();
	auto File = MakeOutcome<FVaRestJson>();
	auto Saved = MakeOutcome<void>();
	Observe(Rig.Client.Put(TEXT("/json")).Body(FVaRestJson::Object().Set(TEXT("a"), 1)), Json1).Send();
	Observe(Rig.Client.Put(TEXT("/multipart")).Body(FVaRestMultipart().AddField(TEXT("f"), TEXT("v")).AddFile(TEXT("file"), PartFile)), Parts).Send();
	Observe(Rig.Client.Put(TEXT("/file")).BodyFile(UploadFile), File).Send();
	Observe(Rig.Client.Get<void>(TEXT("/download")).SaveTo(Destination), Saved).Send();
	if (!TestTrue(TEXT("all four back off"), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 4; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("all complete"), PumpUntil([&] { return Json1->IsDone() && Parts->IsDone() && File->IsDone() && Saved->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	if (!TestEqual(TEXT("two calls each"), Rig.Fake->NumCalls(), 8))
	{
		return false;
	}
	for (const TCHAR* Path : { TEXT("/json"), TEXT("/multipart"), TEXT("/file") })
	{
		TArray<int32> Calls;
		for (int32 Index = 0; Index < Rig.Fake->NumCalls(); ++Index)
		{
			if (Rig.Fake->GetCall(Index).Request.Url.EndsWith(Path))
			{
				Calls.Add(Index);
			}
		}
		if (!TestEqual(FString::Printf(TEXT("%s: two attempts"), Path), Calls.Num(), 2))
		{
			continue;
		}
		const TArray<uint8> First = Rig.Fake->ReadBody(Calls[0]);
		TestTrue(FString::Printf(TEXT("%s: the retry sends the whole body again"), Path), First.Num() > 0 && First == Rig.Fake->ReadBody(Calls[1]));
	}
	TArray<uint8> Downloaded;
	TestTrue(TEXT("SaveTo: the final attempt's body"), Saved->SuccessCalls.load() == 1 && FFileHelper::LoadFileToArray(Downloaded, *Destination)
		&& Downloaded == TArray<uint8>{ 'o', 'k' });
	// Spec review I-7: the lambda's local is not "Parts" (the test's own Parts above; MSVC C4456)
	const auto Leftovers = [&Dir]
	{
		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *(Dir / TEXT("*.part")), true, false);
		return Found.Num();
	};
	// The first attempt's temp file is deleted by a UE::Tasks task (FTransportStage): give it the time it needs
	TestTrue(TEXT("... and no attempt's temp file is left"), PumpUntil([&Leftovers] { return Leftovers() == 0; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryQueueEndLoggedTest, "VaRest.Http.Retry.QueueEndLogged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryQueueEndLoggedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Task 8 re-review minor 1: an attempt the request itself lost in the limiter's queue writes its Basic line (the
	// deadline's here; a Cancel()'s is VaRest.Http.Log's queued-time test), and one dropped because the request's answer
	// was accepted (FClientState::EndWaits) ends silently, as a finished request's dead entry does at its grant
	TGuardConsoleVariable<int32> Level(TEXT("VaRest.LogLevel"), -1);
	FLogCapture Capture;
	FTestRig Queue;
	Queue.Client.WithMaxConcurrent(1).WithLogLevel(EVaRestLogLevel::Basic).WithRetry(Exact(3).WithDeadline(1.f));
	Queue.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Url.EndsWith(TEXT("/hold")) ? FVaRestFakeResponse::Manual() : Json(200); });
	AddExpectedMessagePlain(TEXT("GET https://api.test/late -> Timeout Deadline (0 ms, 1000 ms queued)"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	auto Hold = MakeOutcome<FVaRestJson>();
	auto Late = MakeOutcome<FVaRestJson>();
	Observe(Queue.Client.Get(TEXT("/hold")).NoRetry(), Hold).Send();
	const FVaRestRequestHandle LateHandle = Observe(Queue.Client.Get(TEXT("/late")), Late).Send();
	if (!TestTrue(TEXT("one holds the slot"), PumpUntil([&Queue] { return Queue.Fake->NumCalls() == 1; })))
	{
		Abandon(Queue);
		return false;
	}
	Queue.Clock->Advance(1.5);
	if (!TestTrue(TEXT("the late one ends at its deadline"), PumpUntil([Late] { return Late->IsDone(); })))
	{
		Abandon(Queue);
		return false;
	}
	Queue.Fake->Complete(0, Json(200));
	if (!TestTrue(TEXT("both complete"), PumpUntil([Hold, Late] { return Hold->IsDone() && Late->IsDone(); })))
	{
		Abandon(Queue);
		return false;
	}
	Queue.Drain();
	const FString DeadlineLine = FString::Printf(TEXT("#%llu GET https://api.test/late -> Timeout Deadline (0 ms, 1000 ms queued)"), LateHandle.GetId());
	TestEqual(TEXT("the deadline's abort writes the attempt's Basic line, once"), Capture.Count(DeadlineLine), 1);

	// The decode window of VaRest.Http.Retry.QueuedRetryWhileDecoding, at Basic: the stage's answer takes the queued retry
	// out of the queue, and that abort is not the request's own end
	FTestRig Rig;
	Rig.Fake->bCompleteOnWorker = false;
	const TSharedRef<FHoldingOuter, ESPMode::ThreadSafe> Outer = MakeShared<FHoldingOuter, ESPMode::ThreadSafe>(true);
	Outer->OnlyTag = TEXT("Held");
	Rig.Client.Use(Outer, EVaRestStageSlot::Outer).WithRetry(Exact(3)).WithMaxConcurrent(1).WithLogLevel(EVaRestLogLevel::Basic);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		return Request.Url.EndsWith(TEXT("/blocker")) ? FVaRestFakeResponse::Manual() : Json(503);
	});
	AddExpectedMessagePlain(TEXT("GET https://api.test/held -> 503"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	VaRest::Http::FLimiter& Limiter = VaRest::Http::FClientAccess::State(Rig.Client)->GetLimiter();
	auto Answered = MakeOutcome<FVaRestJson>();
	auto Blocker = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle HeldHandle = Observe(Rig.Client.Get(TEXT("/held")).Tag(TEXT("Held")), Answered).Send();
	if (!TestTrue(TEXT("the first attempt, then a backoff"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Outer->Held.Reset();
		Abandon(Rig);
		return false;
	}
	Observe(Rig.Client.Get(TEXT("/blocker")), Blocker).Send();
	if (!TestEqual(TEXT("the blocker takes the one slot"), Rig.Fake->NumCalls(), 2))
	{
		Outer->Held.Reset();
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.5);
	if (!TestEqual(TEXT("the retry waits in the limiter's queue"), Limiter.NumQueued(), 1))
	{
		Outer->Held.Reset();
		Abandon(Rig);
		return false;
	}
	Rig.Fake->Complete(1, Json(200));
	Outer->AnswerNow(200, FUtf8StringView(UTF8TEXT(R"({"from":"stage"})")));
	TestEqual(TEXT("the stage's answer took the retry out of the queue"), Limiter.NumQueued(), 0);
	if (!TestTrue(TEXT("both delivered"), PumpUntil([Answered, Blocker] { return Answered->IsDone() && Blocker->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("the stage's answer is the request's"), Answered->SuccessCalls.load() == 1);
	TestEqual(TEXT("the freed slot dispatched nothing for it"), Rig.Fake->NumCalls(), 2);
	const FString Dropped = FString::Printf(TEXT("#%llu GET https://api.test/held -> canceled"), HeldHandle.GetId());
	TestEqual(TEXT("... and the retry it took out of the queue wrote no canceled line"), Capture.Count(Dropped), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryProceedWhileDecodingTest, "VaRest.Http.Retry.ProceedRefusedWhileDecoding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryProceedWhileDecodingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Task 8 re-review, out of scope 1 (re-review N-6's family, closed centrally): once an answer is accepted and handed to
	// its decode task, a stage that proceeds again starts no attempt -- Proceed refuses outside Phase InChain
	FTestRig Rig;
	const TSharedRef<FEagerOuter, ESPMode::ThreadSafe> Outer = MakeShared<FEagerOuter, ESPMode::ThreadSafe>(false, 200);
	Outer->JsonBody = FUtf8StringView(UTF8TEXT(R"({"from":"stage"})"));
	Rig.Client.Use(Outer, EVaRestStageSlot::Outer);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/decoding")), Outcome).Send();
	// Before any pump: the decode task's result comes back through a game-thread task, so the request is still decoding
	{
		const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Pinned = Outer->Kept.Pin();
		if (!TestTrue(TEXT("the stage's answer is being decoded"), Pinned.IsValid() && !Outcome->IsDone()
			&& VaRest::Http::Internals(*Pinned).Phase == EVaRestExchangePhase::Decoding))
		{
			Outer->Release();
			Abandon(Rig);
			return false;
		}
	}
	Outer->ProceedAgain();
	TestEqual(TEXT("a stage proceeding while its answer decodes: Canceled at once"), Outer->AgainCanceled->load(), 1);
	TestEqual(TEXT("... and no network attempt"), Rig.Fake->NumCalls(), 0);
	if (!TestTrue(TEXT("the decoded answer is delivered"), Rig.Wait(Outcome)))
	{
		Outer->Release();
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("... the stage's, once"), Outcome->SuccessCalls.load() == 1 && Outcome->ErrorCalls.load() == 0 && Outcome->Value.IsSet()
		&& Outcome->Value->Get(TEXT("from")).AsString() == TEXT("stage"));
	TestEqual(TEXT("no network attempt at all"), Rig.Fake->NumCalls(), 0);
	Outer->Release();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryReportsTwiceTest, "VaRest.Http.Retry.StageReportsTwice",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryReportsTwiceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Task 8 re-review, out of scope 2: a PerAttempt stage that reports twice (review C-3: the second report is ignored)
	// must not arm a second backoff. A second timer would overwrite the one the interrupt hook clears, so Cancel() could
	// not reach the first, and the two would start two attempts at once.
	FTestRig Rig;
	const TSharedRef<FOutcomeProbe, ESPMode::ThreadSafe> Probe = MakeShared<FOutcomeProbe, ESPMode::ThreadSafe>();
	const TSharedRef<FReportsTwice, ESPMode::ThreadSafe> Twice = MakeShared<FReportsTwice, ESPMode::ThreadSafe>(503);
	Rig.Client.Use(Probe, EVaRestStageSlot::Outer).Use(Twice, EVaRestStageSlot::PerAttempt).WithRetry(Exact(3));
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/twice")), Outcome).Send();
	// Inside Send(): the stage answered the first attempt twice
	TestEqual(TEXT("one backoff for the two reports"), Rig.Clock->NumPendingTimers(), 1);
	Handle.Cancel();
	TestEqual(TEXT("Cancel() reaches the one wait: nothing left armed"), Rig.Clock->NumPendingTimers(), 0);
	TestEqual(TEXT("... and the Outer stage got its one Canceled inside Cancel()"), Probe->Count(Handle.GetId(), EVaRestTransportFailure::Canceled), 1);
	if (!TestTrue(TEXT("Cancel() ends it"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	// A timer the hook could not reach would fire here
	Rig.Clock->Advance(10.0);
	Rig.Drain();
	TestTrue(TEXT("one OnError(Canceled), one OnComplete"), Outcome->ErrorCalls.load() == 1 && Outcome->CompleteCalls.load() == 1
		&& Outcome->Error.Kind == EVaRestErrorKind::Canceled);
	TestEqual(TEXT("the stage ran for the first attempt only"), Twice->Calls->load(), 1);
	TestEqual(TEXT("the Outer stage got one outcome"), Probe->Total(Handle.GetId()), 1);
	TestEqual(TEXT("nothing reached the network"), Rig.Fake->NumCalls(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryDeadlineIdleTest, "VaRest.Http.Retry.DeadlineKeepsIdleDefault",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryDeadlineIdleTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// The controller's Task 9 ruling: the deadline only shortens timeouts. No timeout set and Deadline 300: the time left
	// caps the total, but the activity timeout stays the engine's idle default (30 s, HttpModule.cpp:98), not 300 -- so a
	// stalled first attempt fails at that default, as a connection error, and is retried within the deadline.
	FTestRig Rig;
	Rig.Fake->EngineActivityTimeout = 30.f;
	Rig.Client.WithRetry(Exact(3).WithDeadline(300.f));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Attempt == 1 ? FVaRestFakeResponse::Manual() : Json(200); });
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/stall")), Outcome).Send();
	if (!TestTrue(TEXT("the first attempt is on the wire"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	const float FirstTotal = Rig.Fake->GetCall(0).Request.Timeout;
	// Final values (IVaRestTransport::Start): the transport sets what it is given, and 0 leaves the engine's default
	const float FirstActivity = Rig.Fake->GetCall(0).Request.ActivityTimeout;
	TestTrue(TEXT("no timeout set: the time left is the total cap, 300 s"), FMath::IsNearlyEqual(FirstTotal, 300.f, 1e-3f));
	TestEqual(TEXT("... and the activity timeout handed over is 0, the engine's default, not 300 s"), FirstActivity, 0.f);
	Rig.Clock->Advance(30.0);
	if (!TestTrue(TEXT("the stall ends at the idle default; a retry is armed"), PumpUntil([&Rig]
		{
			return Rig.Fake->NumInFlight() == 0 && Rig.Clock->NumPendingTimers() == 1;
		})))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)) || !TestEqual(TEXT("two attempts"), Rig.Fake->NumCalls(), 2))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("retried: the second attempt succeeds"), Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 2);
	TestTrue(TEXT("... its total cut to the time left, 300 - 30.5 s"), FMath::IsNearlyEqual(Rig.Fake->GetCall(1).Request.Timeout, 269.5f, 1e-3f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryDeadlineActivityTest, "VaRest.Http.Retry.DeadlineCutsActivity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryDeadlineActivityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// The controller's Task 9 ruling: a set activity timeout is never longer than the time left, and one that is shorter is
	// kept. Deadline 20 s with only an activity timeout of 50 s: both timeouts are 20 s. With 15 s: attempt 1 keeps 15 s
	// (its silence ends it at 15 s), attempt 2 starts at 15.5 s and gets 4.5 s for both.
	FTestRig Rig;
	Rig.Client.WithRetry(Exact(3).WithDeadline(20.f));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto Long = MakeOutcome<FVaRestJson>();
	auto OwnTotal = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/long")).Timeout(0.f, 50.f), Long).Send();
	Observe(Rig.Client.Get(TEXT("/total")).Timeout(100.f), OwnTotal).Send();
	if (!TestTrue(TEXT("both are on the wire"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; })))
	{
		Abandon(Rig);
		return false;
	}
	const FVaRestTransportRequest& LongRequest = Rig.Fake->GetCall(0).Request;
	TestTrue(TEXT("activity 50 s, no total: both are the time left, 20 s"), FMath::IsNearlyEqual(LongRequest.Timeout, 20.f, 1e-4f)
		&& FMath::IsNearlyEqual(LongRequest.ActivityTimeout, 20.f, 1e-4f));
	const FVaRestTransportRequest& TotalRequest = Rig.Fake->GetCall(1).Request;
	TestTrue(TEXT("a total of 100 s and no activity: the total cut to 20 s, and the activity from the request's own total (decision 30), cut to 20 s"),
		FMath::IsNearlyEqual(TotalRequest.Timeout, 20.f, 1e-4f) && FMath::IsNearlyEqual(TotalRequest.ActivityTimeout, 20.f, 1e-4f));
	Rig.Client.CancelAll();
	if (!TestTrue(TEXT("both end"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; })))
	{
		return false;
	}

	FTestRig Short;
	Short.Client.WithRetry(Exact(3).WithDeadline(20.f)).WithTimeout(0.f, 15.f);
	Short.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto Silent = MakeOutcome<FVaRestJson>();
	Observe(Short.Client.Get(TEXT("/silent")), Silent).Send();
	if (!TestTrue(TEXT("attempt 1 is on the wire"), PumpUntil([&Short] { return Short.Fake->NumCalls() == 1; })))
	{
		Abandon(Short);
		return false;
	}
	TestTrue(TEXT("attempt 1: the shorter activity timeout is kept, 15 s; the total is the time left, 20 s"),
		FMath::IsNearlyEqual(Short.Fake->GetCall(0).Request.ActivityTimeout, 15.f, 1e-4f) && FMath::IsNearlyEqual(Short.Fake->GetCall(0).Request.Timeout, 20.f, 1e-4f));
	Short.Clock->Advance(15.0);
	if (!TestTrue(TEXT("its silence ends it at 15 s; a retry is armed"), PumpUntil([&Short]
		{
			return Short.Fake->NumInFlight() == 0 && Short.Clock->NumPendingTimers() == 1;
		})))
	{
		Abandon(Short);
		return false;
	}
	Short.Clock->Advance(0.5);
	if (!TestTrue(TEXT("attempt 2 is on the wire"), PumpUntil([&Short] { return Short.Fake->NumCalls() == 2; })))
	{
		Abandon(Short);
		return false;
	}
	TestTrue(TEXT("attempt 2 at 15.5 s: the 15 s activity timeout is cut to the 4.5 s left, and so is the total"),
		FMath::IsNearlyEqual(Short.Fake->GetCall(1).Request.ActivityTimeout, 4.5f, 1e-4f) && FMath::IsNearlyEqual(Short.Fake->GetCall(1).Request.Timeout, 4.5f, 1e-4f));
	Short.Clock->Advance(4.5);
	if (!TestTrue(TEXT("it ends at the deadline"), Short.Wait(Silent)))
	{
		Abandon(Short);
		return false;
	}
	TestTrue(TEXT("... after two attempts: a third would start past it"), Silent->ErrorCalls.load() == 1 && Silent->Error.Attempts == 2
		&& Short.Fake->NumCalls() == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryQueueDeadlineClearedTest, "VaRest.Http.Retry.QueueDeadlineCleared",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryQueueDeadlineClearedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Task 9 review minor 1: the deadline's queue timer goes with every exit from the queue before the deadline -- the
	// grant and the abort (a Cancel()) -- not only when it fires
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1).WithRetry(Exact(3).WithDeadline(10.f));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Url.EndsWith(TEXT("/hold")) ? FVaRestFakeResponse::Manual() : Json(200); });
	auto Hold = MakeOutcome<FVaRestJson>();
	auto Granted = MakeOutcome<FVaRestJson>();
	auto Canceled = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/hold")).NoRetry(), Hold).Send();
	Observe(Rig.Client.Get(TEXT("/granted")), Granted).Send();
	const FVaRestRequestHandle CanceledHandle = Observe(Rig.Client.Get(TEXT("/canceled")), Canceled).Send();
	// The held call has no timeout (the rig sets none) and arms nothing: the two queue timers are all there is
	if (!TestTrue(TEXT("one holds the slot; two wait, each with its deadline's timer"), PumpUntil([&Rig]
		{
			return Rig.Fake->NumCalls() == 1 && Rig.Clock->NumPendingTimers() == 2;
		})))
	{
		Abandon(Rig);
		return false;
	}
	CanceledHandle.Cancel();
	TestEqual(TEXT("canceled in the queue: its timer goes with it"), Rig.Clock->NumPendingTimers(), 1);
	if (!TestTrue(TEXT("... and it ends Canceled"), PumpUntil([Canceled] { return Canceled->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Fake->Complete(0, Json(200));
	if (!TestTrue(TEXT("the slot frees: the other is granted and completes"), PumpUntil([Hold, Granted] { return Hold->IsDone() && Granted->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestEqual(TEXT("granted from the queue: its timer went with the grant"), Rig.Clock->NumPendingTimers(), 0);
	TestTrue(TEXT("... it succeeded; the canceled one is Canceled"), Granted->SuccessCalls.load() == 1 && Canceled->Error.Kind == EVaRestErrorKind::Canceled);
	TestEqual(TEXT("two dispatched"), Rig.Fake->NumCalls(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryQueueDeadlineEarlyTest, "VaRest.Http.Retry.QueueDeadlineNotEarly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryQueueDeadlineEarlyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Task 9 review minor 2: on a clock whose timers fire early (half the delay, at least 0.1 s), the queue's deadline
	// timer fires at 0.5, 0.75, 0.875 and 0.975 s, before the 1 s deadline on Now(): each fire re-arms for the rest, and
	// only the one at 1.075 s ends the wait -- an early Deadline is not retryable
	FTestRig Rig;
	Rig.Client.WithClock(MakeShared<FEarlyClock, ESPMode::ThreadSafe>(Rig.Clock, 0.5, 0.1)).WithMaxConcurrent(1).WithRetry(Exact(3).WithDeadline(1.f));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Url.EndsWith(TEXT("/hold")) ? FVaRestFakeResponse::Manual() : Json(200); });
	auto Hold = MakeOutcome<FVaRestJson>();
	auto Late = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/hold")).NoRetry(), Hold).Send();
	Observe(Rig.Client.Get(TEXT("/late")), Late).Send();
	if (!TestTrue(TEXT("one holds the slot"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.9);
	Rig.Drain();
	TestFalse(TEXT("early fires do not end the wait: still queued at 0.9 s"), Late->IsDone());
	TestEqual(TEXT("... with its timer re-armed"), Rig.Clock->NumPendingTimers(), 1);
	Rig.Clock->Advance(0.3);
	if (!TestTrue(TEXT("past the deadline on Now(), the wait ends"), PumpUntil([Late] { return Late->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... as Timeout, not retryable, saying Deadline"), Late->Error.Kind == EVaRestErrorKind::Timeout && !Late->Error.bRetryable
		&& Late->Error.Message.Contains(TEXT("Deadline")));
	Rig.Fake->Complete(0, Json(200));
	if (!TestTrue(TEXT("the holder completes"), PumpUntil([Hold] { return Hold->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestEqual(TEXT("the late one never reached the transport"), Rig.Fake->NumCalls(), 1);
	TestEqual(TEXT("nothing left armed"), Rig.Clock->NumPendingTimers(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryStaleEndLiveCallTest, "VaRest.Http.Retry.StaleEndKeepsLiveCall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryStaleEndLiveCallTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// M4a final review A1, review A I-1 row R1 (VaRest-tqt): a PerAttempt stage answers attempt 1 itself while its call runs
	// (a user's own timeout), Retry re-issues, and attempt 1's call reports back while attempt 2's runs. That end is not the
	// live dispatch's: Cancel() must still reach attempt 2's call, whose Canceled end gives its limiter slot back.
	FTestRig Rig;
	Rig.Fake->bCompleteOnWorker = false;
	const TSharedRef<FHoldingOuter, ESPMode::ThreadSafe> Early = MakeShared<FHoldingOuter, ESPMode::ThreadSafe>(true);
	Early->HoldPasses = 1;
	Rig.Client.Use(Early, EVaRestStageSlot::PerAttempt).WithRetry(Exact(3));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	const VaRest::Http::FLimiter& Slots = VaRest::Http::FClientAccess::State(Rig.Client)->GetLimiter();
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/stale")), Outcome).Send();
	if (!TestEqual(TEXT("attempt 1 is on the wire"), Rig.Fake->NumCalls(), 1))
	{
		Early->Held.Reset();
		Abandon(Rig);
		return false;
	}
	Early->AnswerNow(503);
	// Before any pump: attempt 1's end waits in the game thread's queue while the backoff dispatches attempt 2
	Rig.Clock->Advance(0.5);
	if (!TestEqual(TEXT("attempt 2 is on the wire"), Rig.Fake->NumCalls(), 2))
	{
		Abandon(Rig);
		return false;
	}
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Late = Early->LateAnswers;
	if (!TestTrue(TEXT("attempt 1's end arrives while attempt 2 runs"), PumpUntil([Late] { return Late->load() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... attempt 2's call runs on, holding the one slot"), !Rig.Fake->GetCall(1).bCompleted && Slots.NumActive() == 1);
	Handle.Cancel();
	TestTrue(TEXT("Cancel() reaches attempt 2's call"), Rig.Fake->GetCall(1).bCanceled);
	const bool bEnded = TestTrue(TEXT("the request ends"), Rig.Wait(Outcome));
	Rig.Drain();
	TestTrue(TEXT("... with one OnError(Canceled)"), bEnded && Outcome->ErrorCalls.load() == 1 && Outcome->CompleteCalls.load() == 1
		&& Outcome->Error.Kind == EVaRestErrorKind::Canceled);
	TestEqual(TEXT("its slot came back, attempt 2's call completed by nobody but the cancel"), Slots.NumActive(), 0);
	TestEqual(TEXT("... and no call runs on"), Rig.Fake->NumInFlight(), 0);
	if (!Rig.Fake->GetCall(1).bCompleted)
	{
		// Only on the failure path: the call the cancel missed is finished here, so nothing outlives the test
		Rig.Fake->Complete(1, Json(200));
		Abandon(Rig);
		Rig.Drain();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryStaleEndProgressTest, "VaRest.Http.Retry.StaleEndKeepsProgress",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryStaleEndProgressTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// M4a final review A1, review A I-1 row R2: as StaleEndKeepsLiveCall, with OnProgress bound. Attempt 1's late end must
	// not clear attempt 2's progress timer: the live download keeps reporting (and its owner-gone check keeps running).
	FTestRig Rig;
	Rig.Fake->bCompleteOnWorker = false;
	const TSharedRef<FHoldingOuter, ESPMode::ThreadSafe> Early = MakeShared<FHoldingOuter, ESPMode::ThreadSafe>(true);
	Early->HoldPasses = 1;
	Rig.Client.Use(Early, EVaRestStageSlot::PerAttempt).WithRetry(Exact(3));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Calls = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/stale")).OnProgress([Calls](const FVaRestProgress&) { ++*Calls; }), Outcome).Send();
	if (!TestEqual(TEXT("attempt 1 is on the wire"), Rig.Fake->NumCalls(), 1))
	{
		Early->Held.Reset();
		Abandon(Rig);
		return false;
	}
	Early->AnswerNow(503);
	Rig.Clock->Advance(0.5);
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Late = Early->LateAnswers;
	if (!TestTrue(TEXT("attempt 2 runs when attempt 1's end arrives"), Rig.Fake->NumCalls() == 2 && PumpUntil([Late] { return Late->load() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	const int32 Before = Calls->load();
	Rig.Fake->ReportProgress(1, 0, 10, 100);
	Rig.Clock->Advance(0.1);
	TestEqual(TEXT("attempt 2's progress still reaches OnProgress after attempt 1's late end"), Calls->load(), Before + 1);
	Rig.Fake->Complete(1, Json(200));
	TestTrue(TEXT("attempt 2 succeeds"), Rig.Wait(Outcome) && Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 2);
	Rig.Drain();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryOrphanCanceledTest, "VaRest.Http.Retry.OrphanEndsWithItsAttempt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryOrphanCanceledTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// M4a final review A1 (c), review A I-1 row R5: when a PerAttempt stage's own answer for an attempt passes the attempt
	// boundary while that attempt's call still runs, the call is canceled there, with its progress timer -- inside the
	// answer, before Retry's backoff. Only the stage that answered hears its Canceled end.
	FTestRig Rig;
	const TSharedRef<FHoldingOuter, ESPMode::ThreadSafe> Early = MakeShared<FHoldingOuter, ESPMode::ThreadSafe>(true);
	Early->HoldPasses = 1;
	Rig.Client.Use(Early, EVaRestStageSlot::PerAttempt).WithRetry(Exact(3));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Attempt == 1 ? FVaRestFakeResponse::Manual() : Json(200); });
	auto Outcome = MakeOutcome<FVaRestJson>();
	const int32 TimersBefore = Rig.Clock->NumPendingTimers();
	Observe(Rig.Client.Get(TEXT("/orphan")).OnProgress([](const FVaRestProgress&) {}), Outcome).Send();
	if (!TestTrue(TEXT("attempt 1 on the wire, its progress tick armed"), Rig.Fake->NumCalls() == 1 && Rig.Clock->NumPendingTimers() == TimersBefore + 1))
	{
		Early->Held.Reset();
		Abandon(Rig);
		return false;
	}
	Early->AnswerNow(503);
	// Inside the answer, before any pump
	TestTrue(TEXT("attempt 1's call is canceled at its attempt's end"), Rig.Fake->GetCall(0).bCanceled && Rig.Fake->NumInFlight() == 0);
	TestEqual(TEXT("... its progress tick gone: only the backoff is armed"), Rig.Clock->NumPendingTimers(), TimersBefore + 1);
	Rig.Clock->Advance(0.5);
	TestTrue(TEXT("attempt 2 succeeds"), Rig.Wait(Outcome) && Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 2);
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Late = Early->LateAnswers;
	TestTrue(TEXT("the canceled call's end reached only the stage that answered"), PumpUntil([Late] { return Late->load() == 1; }));
	Rig.Drain();
	TestEqual(TEXT("one delivery"), Outcome->CompleteCalls.load(), 1);
	if (!Rig.Fake->GetCall(0).bCompleted)
	{
		// Only on the failure path: the call nothing canceled is finished here, so nothing outlives the test
		Rig.Fake->Complete(0, Json(200));
		Rig.Drain();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryOuterAnswerFreesSlotTest, "VaRest.Http.Retry.OuterAnswerFreesTheSlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryOuterAnswerFreesSlotTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// M4a final review A1 (d), review A M-1 row R7: an Outer stage answers its request (504) while the attempt is on the
	// wire. Accepting that answer cancels the call, so its limiter slot comes back with its Canceled end: with one slot, the
	// next request reaches the transport without anyone completing the orphaned call.
	FTestRig Rig;
	const TSharedRef<FHoldingOuter, ESPMode::ThreadSafe> Outer = MakeShared<FHoldingOuter, ESPMode::ThreadSafe>(true);
	Outer->OnlyTag = TEXT("Held");
	Rig.Client.Use(Outer, EVaRestStageSlot::Outer).WithRetry(Exact(3)).WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Url.EndsWith(TEXT("/held")) ? FVaRestFakeResponse::Manual() : Json(200); });
	const VaRest::Http::FLimiter& Slots = VaRest::Http::FClientAccess::State(Rig.Client)->GetLimiter();
	auto Answered = MakeOutcome<FVaRestJson>();
	auto Next = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/held")).Tag(TEXT("Held")), Answered).Send();
	Observe(Rig.Client.Get(TEXT("/next")), Next).Send();
	if (!TestTrue(TEXT("the held request's call holds the one slot; the next one waits"), Rig.Fake->NumCalls() == 1 && Slots.NumQueued() == 1))
	{
		Outer->Held.Reset();
		Abandon(Rig);
		return false;
	}
	Outer->AnswerNow(504);
	TestTrue(TEXT("the Outer stage's answer is delivered"), Rig.Wait(Answered) && Answered->Error.HttpStatus == 504);
	TestTrue(TEXT("... and the call it left is canceled"), Rig.Fake->GetCall(0).bCanceled);
	const bool bNext = TestTrue(TEXT("the freed slot sends the next request"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }));
	TestTrue(TEXT("... which succeeds"), bNext && Rig.Wait(Next) && Next->SuccessCalls.load() == 1);
	if (!bNext)
	{
		// Only on the failure path: the orphan that kept the slot is finished here
		Rig.Fake->Complete(0, Json(200));
		Abandon(Rig);
	}
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Late = Outer->LateAnswers;
	TestTrue(TEXT("the orphan's Canceled end reached only the stage that answered"), PumpUntil([Late] { return Late->load() == 1; }));
	Rig.Drain();
	TestTrue(TEXT("every slot came back"), Slots.NumActive() == 0 && Slots.NumQueued() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogStaleEndLineTest, "VaRest.Http.Log.StaleEndLineIsItsOwn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogStaleEndLineTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// M4a final review carry C1 (wave A concern 1, review A I-1's log bullet): a stale transport end -- attempt 1's call,
	// which a PerAttempt stage answered for and the boundary canceled (A1), ending while attempt 2 runs -- writes a Basic
	// line about its own dispatch: its URL (a custom auth signs each attempt into the query) and its own wait in the queue,
	// not the live attempt's. Attempt 1 queues 250 ms behind a blocker; attempt 2 queues for none.
	TGuardConsoleVariable<int32> Level(TEXT("VaRest.LogLevel"), -1);
	FTestRig Rig;
	Rig.Fake->bCompleteOnWorker = false;
	const TSharedRef<FHoldingOuter, ESPMode::ThreadSafe> Early = MakeShared<FHoldingOuter, ESPMode::ThreadSafe>(true);
	Early->HoldPasses = 1;
	Early->OnlyTag = TEXT("Stale");
	Rig.Client.WithAuth(MakeShared<FAttemptQueryAuth, ESPMode::ThreadSafe>()).Use(Early, EVaRestStageSlot::PerAttempt).WithRetry(Exact(3))
		.WithMaxConcurrent(1).WithLogLevel(EVaRestLogLevel::Basic);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	FLogCapture Capture;
	auto Blocker = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/blocker")).NoAuth(), Blocker).Send();
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/stale")).Tag(TEXT("Stale")), Outcome).Send();
	Rig.Clock->Advance(0.25);
	Rig.Fake->Complete(0, Json(200));
	if (!TestTrue(TEXT("attempt 1 leaves the queue after 250 ms, and is on the wire"), Rig.Wait(Blocker) && PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; })))
	{
		Early->Held.Reset();
		Abandon(Rig);
		return false;
	}
	Early->AnswerNow(503);
	Rig.Clock->Advance(0.5);
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Late = Early->LateAnswers;
	if (!TestTrue(TEXT("attempt 2 is on the wire when attempt 1's end arrives"), Rig.Fake->NumCalls() == 3 && PumpUntil([Late] { return Late->load() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Fake->Complete(2, Json(200));
	TestTrue(TEXT("attempt 2 succeeds"), Rig.Wait(Outcome) && Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 2);
	Rig.Drain();
	const FString Id = FString::Printf(TEXT("#%llu "), Handle.GetId());
	const auto Count = [&Capture, &Id](const TCHAR* Text)
	{
		int32 Found = 0;
		for (const FString& Line : Capture.GetLines())
		{
			Found += Line.Contains(Id) && Line.Contains(Text) ? 1 : 0;
		}
		return Found;
	};
	TestEqual(TEXT("attempt 1's stale end: its own URL, its 500 ms, its 250 ms in the queue"),
		Count(TEXT("GET https://api.test/stale?try=1 -> canceled (500 ms, 250 ms queued)")), 1);
	TestEqual(TEXT("... no canceled line shows attempt 2's URL"), Count(TEXT("try=2 -> canceled")), 0);
	TestEqual(TEXT("attempt 2's own line: its URL, no queue wait"), Count(TEXT("GET https://api.test/stale?try=2 -> 200 (0 ms, ")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryInertOverrideTest, "VaRest.Http.Retry.InertOverrideLogged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryInertOverrideTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// M4a final review A4 (review A M-3): a request's own Retry() on a POST without Idempotent() is never retried (spec
	// 4.1), so one Log line says so. A client-wide WithRetry() writes none for every POST, nor does an Idempotent() POST, a
	// GET, or a request whose override is NoRetry().
	TGuardConsoleVariable<int32> Level(TEXT("VaRest.LogLevel"), -1);
	FLogCapture Capture;
	FTestRig Rig;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Basic).WithRetry(Exact(3));
	auto Inert = MakeOutcome<FVaRestJson>();
	auto ClientWide = MakeOutcome<FVaRestJson>();
	auto Marked = MakeOutcome<FVaRestJson>();
	auto Fetched = MakeOutcome<FVaRestJson>();
	auto Opted = MakeOutcome<FVaRestJson>();
	const uint64 InertId = Observe(Rig.Client.Post(TEXT("/inert")).Body(TEXT("o")).Retry(Exact(3)), Inert).Send().GetId();
	const uint64 ClientWideId = Observe(Rig.Client.Post(TEXT("/client-wide")).Body(TEXT("o")), ClientWide).Send().GetId();
	const uint64 MarkedId = Observe(Rig.Client.Post(TEXT("/marked")).Body(TEXT("o")).Idempotent().Retry(Exact(3)), Marked).Send().GetId();
	const uint64 FetchedId = Observe(Rig.Client.Get(TEXT("/fetched")).Retry(Exact(3)), Fetched).Send().GetId();
	const uint64 OptedId = Observe(Rig.Client.Patch(TEXT("/opted")).Body(TEXT("o")).NoRetry(), Opted).Send().GetId();
	if (!TestTrue(TEXT("all complete"), PumpUntil([&] { return Inert->IsDone() && ClientWide->IsDone() && Marked->IsDone() && Fetched->IsDone() && Opted->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	const auto Line = [](uint64 Id, const TCHAR* Verb)
	{
		return FString::Printf(TEXT("#%llu VaRest: %s is not retried without Idempotent() (spec 4.1)"), Id, Verb);
	};
	TestEqual(TEXT("the POST's own Retry(): one line"), Capture.Count(Line(InertId, TEXT("POST"))), 1);
	TestEqual(TEXT("a client-wide WithRetry(): none"), Capture.Count(Line(ClientWideId, TEXT("POST"))), 0);
	TestEqual(TEXT("an Idempotent() POST: none"), Capture.Count(Line(MarkedId, TEXT("POST"))), 0);
	TestEqual(TEXT("a GET: none"), Capture.Count(Line(FetchedId, TEXT("GET"))), 0);
	TestEqual(TEXT("NoRetry(): none"), Capture.Count(Line(OptedId, TEXT("PATCH"))), 0);
	TestEqual(TEXT("... and no other request wrote it"), Capture.Count(TEXT("is not retried without Idempotent()")), 1);
	return true;
}

#endif
