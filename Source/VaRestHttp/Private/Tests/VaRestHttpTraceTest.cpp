// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Tasks/Task.h"
#include "UObject/Package.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTesting.h"
#include "VaRestTrace.h"

namespace VaRest::Http::TraceTestPrivate
{
	uint64 RequestIdOf(const FVaRestTraceEvent& Event)
	{
		switch (Event.Kind)
		{
		case EVaRestTraceKind::Started: return Event.Payload.Get<FVaRestTraceStarted>().RequestId;
		case EVaRestTraceKind::AttemptDispatched: return Event.Payload.Get<FVaRestTraceAttemptDispatched>().RequestId;
		case EVaRestTraceKind::AttemptEnded: return Event.Payload.Get<FVaRestTraceAttemptEnded>().RequestId;
		case EVaRestTraceKind::Completed: return Event.Payload.Get<FVaRestTraceCompleted>().RequestId;
		default: return 0;
		}
	}

	FString KindsOf(const TArray<FVaRestTraceEvent>& Events)
	{
		FString Out;
		for (const FVaRestTraceEvent& Event : Events)
		{
			if (!Out.IsEmpty())
			{
				Out += TEXT(",");
			}
			switch (Event.Kind)
			{
			case EVaRestTraceKind::Started: Out += TEXT("Started"); break;
			case EVaRestTraceKind::AttemptDispatched: Out += TEXT("Dispatched"); break;
			case EVaRestTraceKind::AttemptEnded: Out += TEXT("Ended"); break;
			case EVaRestTraceKind::Completed: Out += TEXT("Completed"); break;
			default: Out += TEXT("Other"); break;
			}
		}
		return Out;
	}

	class FRecordingSink final : public IVaRestTraceSink
	{
	public:
		explicit FRecordingSink(EVaRestTraceDetail InFlags)
		{
			Detail.Flags = InFlags;
		}

		virtual FVaRestTraceDetailSettings GetDetail() const override { return Detail; }
		virtual void OnAttached(const FVaRestTraceSnapshot& Snapshot) override
		{
			Snapshots.Add(Snapshot);
			if (AttachHook)
			{
				AttachHook(Snapshot);
			}
		}
		virtual void OnEvent(const FVaRestTraceEvent& Event) override
		{
			if (!IsInGameThread())
			{
				bAllOnGameThread = false;
			}
			if (Hook)
			{
				Hook(Event);
			}
			FScopeLock Lock(&Mutex);
			Events.Add(Event);
		}

		/** One request's events, in arrival (= Seq) order */
		TArray<FVaRestTraceEvent> For(uint64 RequestId) const
		{
			FScopeLock Lock(&Mutex);
			return Events.FilterByPredicate([RequestId](const FVaRestTraceEvent& Event) { return RequestIdOf(Event) == RequestId; });
		}

		TArray<FVaRestTraceClient> ClientEvents(uint64 ClientId) const
		{
			FScopeLock Lock(&Mutex);
			TArray<FVaRestTraceClient> Out;
			for (const FVaRestTraceEvent& Event : Events)
			{
				if (Event.Kind == EVaRestTraceKind::Client && Event.Payload.Get<FVaRestTraceClient>().ClientId == ClientId)
				{
					Out.Add(Event.Payload.Get<FVaRestTraceClient>());
				}
			}
			return Out;
		}

		FVaRestTraceDetailSettings Detail;
		TArray<FVaRestTraceSnapshot> Snapshots;
		/** Runs inside OnEvent, before the event is stored */
		TFunction<void(const FVaRestTraceEvent&)> Hook;
		/** Runs inside OnAttached: after AddSink built the snapshot, before it adds the sink (review M-9's window) */
		TFunction<void(const FVaRestTraceSnapshot&)> AttachHook;
		std::atomic<bool> bAllOnGameThread{ true };
		mutable FCriticalSection Mutex;
		TArray<FVaRestTraceEvent> Events;
	};

	/**
	 * Task 15 review m3: a PerAttempt stage that proceeds and keeps its attempt's answer until AnswerNow() answers 204
	 * itself, while the transport still runs; what the transport reports afterwards is counted in LateAnswers and goes no
	 * further. A held answer captures the exchange: answer it before the test ends.
	 */
	class FHeldAnswerStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("HeldAnswer"); }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Held = MoveTemp(OnOutcome);
			Next.Proceed(Exchange, [Swallowed = LateAnswers](FVaRestAttemptOutcome&&) { ++*Swallowed; });
		}

		void AnswerNow()
		{
			if (!Held)
			{
				return;
			}
			FVaRestOutcomeCallback Answer = MoveTemp(Held);
			Held.Reset();
			FVaRestAttemptOutcome Early;
			Early.Status = 204;
			Answer(MoveTemp(Early));
		}

		FVaRestOutcomeCallback Held;
		TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> LateAnswers = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	};

	using FSinkRef = TSharedRef<FRecordingSink, ESPMode::ThreadSafe>;

	FSinkRef MakeSink(EVaRestTraceDetail Flags)
	{
		return MakeShared<FRecordingSink, ESPMode::ThreadSafe>(Flags);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceSuccessTest, "VaRest.Http.Trace.Success",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceSuccessTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TraceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Headers);
	FVaRestTrace::AddSink(Sink);
	FTestRig Rig;
	UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	Rig.Clock->Advance(5.0);
	// Task 15 review M2: the response's own secrets -- a cookie, an echoed credential, a redirect carrying a code
	FVaRestFakeResponse Answer = FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{}")));
	Answer.Headers.Set(TEXT("Set-Cookie"), TEXT("s=abc"));
	Answer.Headers.Set(TEXT("Authorization"), TEXT("Bearer abc"));
	Answer.Headers.Set(TEXT("Location"), TEXT("https://api.test/next?code=abc&page=2"));
	Rig.Fake->Enqueue(MoveTemp(Answer));

	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Post(TEXT("/items/{id}?token=abc"))
		.Path(TEXT("id"), TEXT("7"))
		.Header(TEXT("Authorization"), TEXT("Bearer abc"))
		.Body(FVaRestJson::Object().Set(TEXT("n"), TEXT("v")))
		.Tag(TEXT("inventory"))
		.Priority(EVaRestPriority::High)
		.Idempotent()
		.Owner(Owner), Outcome).Send();
	Rig.Wait(Outcome);
	FVaRestTrace::RemoveSink(Sink);

	const TArray<FVaRestTraceEvent> Events = Sink->For(Handle.GetId());
	TestEqualSensitive(TEXT("a typical request is four events (Inspector 2.1)"), KindsOf(Events), FString(TEXT("Started,Dispatched,Ended,Completed")));
	if (Events.Num() != 4)
	{
		return false;
	}
	TestTrue(TEXT("Seq increases"), Events[0].Seq < Events[1].Seq && Events[1].Seq < Events[2].Seq && Events[2].Seq < Events[3].Seq);
	TestTrue(TEXT("every event on the game thread"), Sink->bAllOnGameThread.load());

	const FVaRestTraceStarted& Started = Events[0].Payload.Get<FVaRestTraceStarted>();
	TestEqual(TEXT("IN-1: the client"), Started.ClientId, Rig.Client.GetId());
	TestEqual(TEXT("... scope 0"), Started.ScopeId, FVaRestTrace::ProcessScopeId);
	TestEqualSensitive(TEXT("... the verb"), Started.Verb, FString(TEXT("POST")));
	TestEqualSensitive(TEXT("... the URL, redacted"), Started.Url, FString(TEXT("https://api.test/items/7?token=***")));
	TestEqualSensitive(TEXT("... the template before substitution, without the query"), Started.PathTemplate, FString(TEXT("/items/{id}")));
	TestTrue(TEXT("... tag, priority, the idempotent flag"), Started.Tag == FName(TEXT("inventory")) && Started.Priority == EVaRestPriority::High
		&& EnumHasAnyFlags(Started.Flags, EVaRestTraceRequestFlags::Idempotent));
	TestEqualSensitive(TEXT("... the headers as built, redacted"), Started.Headers.Get(TEXT("Authorization")), FString(TEXT("***")));
	TestTrue(TEXT("... the body's type, size and kind"), Started.Body.Kind == EVaRestBodyKind::Json && Started.Body.Bytes == 9
		&& Started.Body.ContentType.StartsWith(TEXT("application/json"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("IN-15: the C++ call site"), Started.Initiator.SourceFile.Contains(TEXT("VaRestHttpTraceTest.cpp"), ESearchCase::CaseSensitive) && Started.Initiator.SourceLine > 0);
	TestEqualSensitive(TEXT("IN-15: the owner's path"), Started.Initiator.OwnerPath, Owner->GetPathName());
	TestTrue(TEXT("... no Blueprint frame for a C++ caller"), Started.Initiator.ScriptFunction.IsEmpty() && Started.Initiator.ScriptCodeOffset == INDEX_NONE);
	TestTrue(TEXT("... StartUtc is set"), Started.StartUtc.GetTicks() > 0);
	TestEqual(TEXT("Time is the client's clock"), Events[0].Time, 5.0);

	const FVaRestTraceAttemptDispatched& Dispatched = Events[1].Payload.Get<FVaRestTraceAttemptDispatched>();
	TestTrue(TEXT("IN-2: attempt 1, its start, no limiter wait"), Dispatched.Attempt == 1 && Dispatched.AttemptStart == 5.0 && Dispatched.LimiterWait == 0.0);
	TestEqualSensitive(TEXT("... the final headers, redacted"), Dispatched.Headers.Get(TEXT("Authorization")), FString(TEXT("***")));
	TestTrue(TEXT("... with the idempotency key, which is not a secret"), Dispatched.Headers.Contains(TEXT("Idempotency-Key"))
		&& !Dispatched.Headers.Get(TEXT("Idempotency-Key")).Equals(TEXT("***"), ESearchCase::CaseSensitive));

	const FVaRestTraceAttemptEnded& Ended = Events[2].Payload.Get<FVaRestTraceAttemptEnded>();
	TestTrue(TEXT("IN-3: status, sizes, no failure"), Ended.Attempt == 1 && Ended.Status == 200 && Ended.BytesDown == 2 && Ended.Failure == EVaRestTransportFailure::None);
	TestEqualSensitive(TEXT("... the effective URL, redacted"), Ended.EffectiveUrl, FString(TEXT("https://api.test/items/7?token=***")));
	TestEqualSensitive(TEXT("... the response headers"), Ended.Headers.Get(TEXT("Content-Type")), FString(TEXT("application/json")));
	TestEqualSensitive(TEXT("... redacted: a cookie"), Ended.Headers.Get(TEXT("Set-Cookie")), FString(TEXT("***")));
	TestEqualSensitive(TEXT("... a credential"), Ended.Headers.Get(TEXT("Authorization")), FString(TEXT("***")));
	TestEqualSensitive(TEXT("... a Location as a URL (Inspector 2.2 layer 5)"), Ended.Headers.Get(TEXT("Location")), FString(TEXT("https://api.test/next?code=***&page=2")));
	TestTrue(TEXT("... the transport's timestamps (delegates bound under IN-11)"), Ended.UploadDoneAt >= 0.0 && Ended.StatusAt >= 0.0 && Ended.TransportDoneAt >= 0.0);

	const FVaRestTraceCompleted& Completed = Events[3].Payload.Get<FVaRestTraceCompleted>();
	TestTrue(TEXT("IN-5: success"), Completed.Outcome == EVaRestTraceOutcome::Success && Completed.Error.Kind == EVaRestErrorKind::None);
	TestEqual(TEXT("... the result type"), Completed.ResultType, FName(TEXT("Json")));
	TestTrue(TEXT("... decode times, read because a sink is attached"), Completed.DecodeStartAt >= 0.0 && Completed.DecodeEndAt >= Completed.DecodeStartAt);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceFailuresTest, "VaRest.Http.Trace.Failures",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceFailuresTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TraceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Sink);
	FTestRig Rig;
	Rig.Client.SetSecretValue(TEXT("bearer"), TEXT("secret-value-123456"));

	Rig.Fake->Enqueue(FVaRestFakeResponse::Json(404, FUtf8StringView(UTF8TEXT("{\"error\":\"nope\"}"))));
	auto NotFound = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Missing = Observe(Rig.Client.Get(TEXT("/missing")), NotFound).Send();
	Rig.Wait(NotFound);

	auto Broken = MakeOutcome<FVaRestJson>();
	// An owner whose name holds the known value: the initiator is value-scrubbed like every captured string (Inspector 2.2 layer 3)
	UObject* Named = NewObject<UVaRestHttpSettings>(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UVaRestHttpSettings::StaticClass(), FName(TEXT("Owner_secret-value-123456"))), RF_Transient);
	const FVaRestRequestHandle Bad = Observe(Rig.Client.Get(TEXT("/u/secret-value-123456/{id")).Owner(Named), Broken).Send();
	Rig.Wait(Broken);

	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	auto Stopped = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Canceled = Observe(Rig.Client.Get(TEXT("/slow")), Stopped).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumInFlight() == 1; });
	Canceled.Cancel();
	Rig.Wait(Stopped);
	FVaRestTrace::RemoveSink(Sink);

	const TArray<FVaRestTraceEvent> Http = Sink->For(Missing.GetId());
	TestEqualSensitive(TEXT("an HTTP error is four events"), KindsOf(Http), FString(TEXT("Started,Dispatched,Ended,Completed")));
	if (Http.Num() == 4)
	{
		const FVaRestTraceCompleted& Done = Http[3].Payload.Get<FVaRestTraceCompleted>();
		TestTrue(TEXT("... Completed{Error, Http 404}"), Http[2].Payload.Get<FVaRestTraceAttemptEnded>().Status == 404
			&& Done.Outcome == EVaRestTraceOutcome::Error && Done.Error.Kind == EVaRestErrorKind::Http && Done.Error.HttpStatus == 404);
	}

	const TArray<FVaRestTraceEvent> Config = Sink->For(Bad.GetId());
	TestEqualSensitive(TEXT("a request that never makes an attempt: Started, Completed"), KindsOf(Config), FString(TEXT("Started,Completed")));
	if (Config.Num() == 2)
	{
		const FVaRestTraceCompleted& Done = Config[1].Payload.Get<FVaRestTraceCompleted>();
		TestTrue(TEXT("... Completed{Error, Config}"), Done.Outcome == EVaRestTraceOutcome::Error && Done.Error.Kind == EVaRestErrorKind::Config);
		TestTrue(TEXT("... its message value-scrubbed (Inspector 2.2: FVaRestError Message)"), Done.Error.Message.Contains(TEXT("***"), ESearchCase::CaseSensitive)
			&& !Done.Error.Message.Contains(TEXT("secret-value-123456"), ESearchCase::CaseSensitive));
		const FVaRestTraceStarted& Refused = Config[0].Payload.Get<FVaRestTraceStarted>();
		TestFalse(TEXT("... and the URL of Started too"), Refused.Url.Contains(TEXT("secret-value-123456"), ESearchCase::CaseSensitive));
		TestTrue(TEXT("... and the owner's path"), Refused.Initiator.OwnerPath.Contains(TEXT("Owner_***"), ESearchCase::CaseSensitive)
			&& !Refused.Initiator.OwnerPath.Contains(TEXT("secret-value-123456"), ESearchCase::CaseSensitive));
	}

	const TArray<FVaRestTraceEvent> Cancel = Sink->For(Canceled.GetId());
	TestEqualSensitive(TEXT("a cancel in the transport is four events"), KindsOf(Cancel), FString(TEXT("Started,Dispatched,Ended,Completed")));
	if (Cancel.Num() == 4)
	{
		const FVaRestTraceCompleted& Done = Cancel[3].Payload.Get<FVaRestTraceCompleted>();
		TestTrue(TEXT("... AttemptEnded{Canceled}"), Cancel[2].Payload.Get<FVaRestTraceAttemptEnded>().Failure == EVaRestTransportFailure::Canceled);
		TestTrue(TEXT("... Completed{Canceled, User}"), Done.Outcome == EVaRestTraceOutcome::Canceled && Done.CancelReason == EVaRestCancelReason::User);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceZeroCostTest, "VaRest.Http.Trace.ZeroCostWhenOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceZeroCostTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TraceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	// Task 15 review M4: a sink that asks for nothing. The mask stays 0, but the list is not empty, so a trace point past a
	// missing gate would deliver to it, and Emit counts EventsBuilt only for a delivered event (VaRestTrace.cpp, Emit).
	const FSinkRef Deaf = MakeSink(EVaRestTraceDetail::None);
	FVaRestTrace::AddSink(Deaf);
	FTestRig Rig;
	// "TraceCounters", not "Counters": Testing::Counters() is in scope (review M18)
	const FVaRestTraceCounters& TraceCounters = FVaRestTrace::Counters();
	// Review M9: the counters are process-wide -- let an earlier test's late tasks (body log lines on the redaction pipe,
	// decode tasks) land before the baseline
	FlushRedactionPipe();
	PumpUntil([] { return false; }, 0.05);
	FVaRestTrace::Counters().Reset();
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> Outcomes;
	TArray<FVaRestRequestHandle> Handles;
	// Inspector 3.1 item 6 / R3: "over 1000 requests" (review M10); cheap on the fake
	for (int32 i = 0; i < 1000; ++i)
	{
		Outcomes.Add(MakeOutcome<FVaRestJson>());
		Handles.Add(Observe(Rig.Client.Get(TEXT("/n?token=x")).Header(TEXT("Authorization"), TEXT("Bearer x")), Outcomes.Last()).Send());
	}
	TestTrue(TEXT("all complete"), PumpUntil([&Outcomes] { return !Outcomes.ContainsByPredicate([](const TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>& Each) { return !Each->IsDone(); }); }, 30.0));
	{
		// The client trace point too: made, renamed, re-based and destroyed with the mask at 0 (review M4)
		FVaRestClient Passing = FVaRestClient::Make(TEXT("https://user:pw@passing.test"));
		Passing.WithBaseUrl(TEXT("https://other.test")).WithName(TEXT("Passing"));
	}
	PumpUntil([] { return false; }, 0.05);
	FVaRestTrace::RemoveSink(Deaf);
	// Inspector 3.1 item 6: counted, not timed
	TestEqual(TEXT("IN-11: no event is built without a sink that asks for one"), TraceCounters.EventsBuilt.load(), static_cast<uint64>(0));
	TestEqual(TEXT("... none reaches a sink that asks for nothing"), Deaf->Events.Num(), 0);
	TestEqual(TEXT("... no redactor call"), TraceCounters.RedactorCalls.load(), static_cast<uint64>(0));
	TestEqual(TEXT("... no transport delegate bound"), TraceCounters.DelegateBinds.load(), static_cast<uint64>(0));
	TestEqual(TEXT("... no trace-point cycles"), TraceCounters.TraceCycles.load(), static_cast<uint64>(0));

	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Sink);
	FVaRestTrace::Counters().Reset();
	auto One = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Traced = Observe(Rig.Client.Get(TEXT("/n")).Header(TEXT("Authorization"), TEXT("Bearer x")), One).Send();
	Rig.Wait(One);
	FVaRestTrace::RemoveSink(Sink);
	// Review M9: only this request's events -- a late Client{Destroyed} from an earlier rig may count in EventsBuilt
	const TArray<FVaRestTraceEvent> Events = Sink->For(Traced.GetId());
	TestEqual(TEXT("with a sink: four events for the request"), Events.Num(), 4);
	TestEqual(TEXT("... the two timing delegates bound for the one attempt (IN-11)"), TraceCounters.DelegateBinds.load(), static_cast<uint64>(2));
	TestTrue(TEXT("Metadata alone builds no headers"), Events.Num() == 4 && Events[0].Payload.Get<FVaRestTraceStarted>().Headers.IsEmpty()
		&& Events[1].Payload.Get<FVaRestTraceAttemptDispatched>().Headers.IsEmpty() && Events[2].Payload.Get<FVaRestTraceAttemptEnded>().Headers.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceLateAttachTest, "VaRest.Http.Trace.LateAttach",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceLateAttachTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TraceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/slow?token=abc")), Outcome).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumInFlight() == 1; });

	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Sink);
	TestEqual(TEXT("one snapshot, at AddSink"), Sink->Snapshots.Num(), 1);
	if (Sink->Snapshots.Num() == 1)
	{
		const FVaRestTraceSnapshot& Snapshot = Sink->Snapshots[0];
		const uint64 ClientId = Rig.Client.GetId();
		const uint64 RequestId = Handle.GetId();
		const FVaRestTraceClient* Client = Snapshot.Clients.FindByPredicate([ClientId](const FVaRestTraceClient& Each) { return Each.ClientId == ClientId; });
		TestTrue(TEXT("IN-12: the live client"), Client && Client->DisplayName.Equals(TEXT("https://api.test"), ESearchCase::CaseSensitive) && Client->ScopeId == FVaRestTrace::ProcessScopeId);
		const FVaRestTraceStarted* Entry = Snapshot.InFlight.FindByPredicate([RequestId](const FVaRestTraceStarted& Each) { return Each.RequestId == RequestId; });
		TestTrue(TEXT("... the in-flight request as Started{Partial}"), Entry && EnumHasAnyFlags(Entry->Flags, EVaRestTraceRequestFlags::Partial));
		TestTrue(TEXT("... with its verb, redacted URL, template, attempt and start time (Inspector 2.3)"), Entry && Entry->Verb.Equals(TEXT("GET"), ESearchCase::CaseSensitive)
			&& Entry->Url.Equals(TEXT("https://api.test/slow?token=***"), ESearchCase::CaseSensitive) && Entry->PathTemplate.Equals(TEXT("/slow"), ESearchCase::CaseSensitive)
			&& Entry->CurrentAttempt == 1 && Entry->StartedAt >= 0.0);
	}
	Rig.Fake->Complete(0, FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{}"))));
	Rig.Wait(Outcome);
	FVaRestTrace::RemoveSink(Sink);
	TestEqualSensitive(TEXT("after the attach, the rest of its events"), KindsOf(Sink->For(Handle.GetId())), FString(TEXT("Ended,Completed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceThreadTest, "VaRest.Http.Trace.GameThreadOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceThreadTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TraceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	FTestRig Rig;
	Rig.Fake->bCompleteOnWorker = true; // as FHttpModule does with CompleteOnHttpThread
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Callbacks = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> CompletedFirst = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	Sink->Hook = [Callbacks, CompletedFirst](const FVaRestTraceEvent& Event)
	{
		if (Event.Kind == EVaRestTraceKind::Completed && Callbacks->load() == 0)
		{
			++*CompletedFirst;
		}
	};
	FVaRestTrace::AddSink(Sink);

	const FVaRestRequestHandle OnGame = Rig.Client.Get(TEXT("/a")).OnComplete([Callbacks](const TVaRestResult<FVaRestJson>&) { ++*Callbacks; }).Send();
	TestTrue(TEXT("the game-thread request finished"), PumpUntil([Callbacks] { return Callbacks->load() == 1; }));
	Callbacks->store(0);
	const FVaRestRequestHandle OffGame = Rig.Client.Get(TEXT("/b")).OnHttpThread().OnComplete([Callbacks](const TVaRestResult<FVaRestJson>&) { ++*Callbacks; }).Send();
	TestTrue(TEXT("the OnHttpThread request finished"), PumpUntil([Callbacks] { return Callbacks->load() == 1; }));
	FVaRestTrace::RemoveSink(Sink);

	TestEqual(TEXT("IN-5: Completed before any callback, OnHttpThread ones included"), CompletedFirst->load(), 2);
	TestTrue(TEXT("Inspector 9: nothing is emitted off the game thread, though the transport completes on a worker"), Sink->bAllOnGameThread.load());
	TestEqual(TEXT("four events each"), Sink->For(OnGame.GetId()).Num() + Sink->For(OffGame.GetId()).Num(), 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceClientsTest, "VaRest.Http.Trace.Clients",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceClientsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TraceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Sink);
	uint64 Id = 0;
	{
		FVaRestClient Client = FVaRestClient::Make(TEXT("https://user:pw@temp.test"));
		Id = Client.GetId();
		Client.WithBaseUrl(TEXT("https://user:pw@moved.test")); // unnamed: the new base renames it (task 15 review M5)
		Client.WithName(TEXT("Temp"));
		Client.WithName(TEXT("Temp")); // unchanged: no event
		Client.WithBaseUrl(TEXT("https://third.test")); // named: the name stays, no event
	}
	FVaRestTrace::RemoveSink(Sink);
	const TArray<FVaRestTraceClient> Seen = Sink->ClientEvents(Id);
	TestEqual(TEXT("created, re-based, renamed, destroyed"), Seen.Num(), 4);
	if (Seen.Num() != 4)
	{
		return false;
	}
	TestTrue(TEXT("IN-8: Created, named by the base URL, redacted"), Seen[0].bCreated && Seen[0].DisplayName.Equals(TEXT("https://***@temp.test"), ESearchCase::CaseSensitive)
		&& Seen[0].ScopeId == FVaRestTrace::ProcessScopeId);
	TestTrue(TEXT("decision 23: an unnamed client's new base URL is Created again, redacted"), Seen[1].bCreated
		&& Seen[1].DisplayName.Equals(TEXT("https://***@moved.test"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("decision 23: a rename is Created again for the same id"), Seen[2].bCreated && Seen[2].DisplayName.Equals(TEXT("Temp"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("Destroyed when the last reference goes"), !Seen[3].bCreated);
	TestTrue(TEXT("on the game thread"), Sink->bAllOnGameThread.load());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceClientNamesTest, "VaRest.Http.Trace.ClientNames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceClientNamesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TraceTestPrivate;
	// Task 15 review I1: an unnamed client is named by its base URL, which Make does not validate. Whatever its shape,
	// it is URL-redacted -- in Client{Created} and in a late sink's snapshot alike.
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Sink);
	const TCHAR* const Bases[] = { TEXT("user:pw@host/x"), TEXT("host/v1?api_key=SECRET"), TEXT("ftp://u:p@h/x"), TEXT("https://u:p@h/x?token=T") };
	const TCHAR* const Leaks[] = { TEXT("pw"), TEXT("SECRET"), TEXT("p@"), TEXT("T") };
	TArray<FVaRestClient> Clients;
	for (const TCHAR* Base : Bases)
	{
		Clients.Add(FVaRestClient::Make(Base));
	}
	const FSinkRef Late = MakeSink(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Late);
	FVaRestTrace::RemoveSink(Late);
	FVaRestTrace::RemoveSink(Sink);

	const auto Clean = [Leaks](const FString& Name)
	{
		for (const TCHAR* Leak : Leaks)
		{
			if (Name.Contains(Leak, ESearchCase::CaseSensitive))
			{
				return false;
			}
		}
		return Name.Contains(TEXT("***"), ESearchCase::CaseSensitive);
	};
	TestEqual(TEXT("one snapshot for the late sink"), Late->Snapshots.Num(), 1);
	for (int32 Index = 0; Index < Clients.Num(); ++Index)
	{
		const uint64 ClientId = Clients[Index].GetId();
		const TArray<FVaRestTraceClient> Seen = Sink->ClientEvents(ClientId);
		const FString Created = Seen.Num() == 1 ? Seen[0].DisplayName : FString();
		TestTrue(FString::Printf(TEXT("Client{Created} for \"%s\" is redacted: \"%s\""), Bases[Index], *Created), Seen.Num() == 1 && Clean(Created));
		const FVaRestTraceClient* Listed = Late->Snapshots.Num() == 1
			? Late->Snapshots[0].Clients.FindByPredicate([ClientId](const FVaRestTraceClient& Each) { return Each.ClientId == ClientId; })
			: nullptr;
		const FString InSnapshot = Listed ? Listed->DisplayName : FString();
		TestTrue(FString::Printf(TEXT("... and its snapshot entry: \"%s\""), *InSnapshot), Listed != nullptr && Clean(InSnapshot));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceClientOffThreadTest, "VaRest.Http.Trace.ClientDestroyedOffGameThread",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceClientOffThreadTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TraceTestPrivate;
	// Task 15 review m1: a game's own worker drops the last FVaRestClient. The state dies there; its Client{Destroyed} is
	// emitted from a game-thread task (Inspector 9: nothing is emitted off the game thread).
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Sink); // before Make: the snapshot's walk holds no pin on the client below
	FVaRestClient Doomed = FVaRestClient::Make(TEXT("https://worker.test"));
	const uint64 Id = Doomed.GetId();
	const TWeakPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> Weak = VaRest::Http::FClientAccess::State(Doomed);
	const int32 OffBefore = Counters().ClientStatesDestroyedOffGameThread.load();
	const UE::Tasks::FTask Dropping = UE::Tasks::Launch(TEXT("VaRest.Test.DropClient"), [Last = MoveTemp(Doomed)]() mutable
	{
		Last = FVaRestClient();
	});
	// Not Wait(): it "tries to retract the task and execute it in-place" (Task.h:66-67), which would run it here, on the game thread
	const double Deadline = FPlatformTime::Seconds() + 5.0;
	while (!Dropping.IsCompleted() && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(0.001f);
	}
	TestTrue(TEXT("the worker dropped the last handle"), Dropping.IsCompleted());
	TestEqual(TEXT("... and the state died there"), Counters().ClientStatesDestroyedOffGameThread.load() - OffBefore, 1);
	TestTrue(TEXT("Client{Destroyed} arrives"), PumpUntil([Sink, Id] { return Sink->ClientEvents(Id).ContainsByPredicate([](const FVaRestTraceClient& Each) { return !Each.bCreated; }); }));
	FVaRestTrace::RemoveSink(Sink);
	TestFalse(TEXT("... for a state that is gone"), Weak.IsValid());
	TestTrue(TEXT("... on the game thread (Inspector 9)"), Sink->bAllOnGameThread.load());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceGhostTest, "VaRest.Http.Trace.SnapshotNoGhost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceGhostTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TraceTestPrivate;
	// Review M-9: the snapshot's walk pins every live state. When a pin is a state's last reference, the state must die
	// after AddSink added the sink, so its Client{Destroyed} reaches it (else the sink keeps a ghost client)
	FVaRestTrace::FScopedSinkIsolation Isolation;
	FVaRestClient Doomed = FVaRestClient::Make(TEXT("https://ghost.test"));
	const uint64 Id = Doomed.GetId();
	TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> Held = VaRest::Http::FClientAccess::State(Doomed);
	const TWeakPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> Weak = Held;
	Doomed = FVaRestClient();
	VaRest::Http::FClientState::ForEachLive([&Held, Id](VaRest::Http::FClientState& State)
	{
		if (State.GetClientId() == Id)
		{
			Held.Reset(); // the walk's pin is now the last reference
		}
	});
	TestTrue(TEXT("the walk's pin outlives the walk"), Weak.IsValid());
	PumpUntil([&Weak] { return !Weak.IsValid(); }, 1.0);
	TestFalse(TEXT("... and is dropped from a later game-thread task"), Weak.IsValid());

	FVaRestClient Second = FVaRestClient::Make(TEXT("https://ghost2.test"));
	const uint64 SecondId = Second.GetId();
	// Shared, not a stack local the hook captures by reference (the standing test rule)
	using FStateHolder = TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe>;
	const TSharedRef<FStateHolder, ESPMode::ThreadSafe> SecondHeld = MakeShared<FStateHolder, ESPMode::ThreadSafe>(VaRest::Http::FClientAccess::State(Second));
	const TWeakPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> SecondWeak = *SecondHeld;
	Second = FVaRestClient();
	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	// Review N-M3: drop the last outside reference in M-9's window -- after the snapshot listed the client, before AddSink
	// added the sink. Only the walk's deferred pin keeps the state alive past AddSink, so its Client{Destroyed} reaches
	// the sink. (An OnBuildSnapshot contributor would run before the core's walk, MulticastDelegateBase.h:299-300, and
	// the state would die unlisted: that path never tests the window.)
	Sink->AttachHook = [SecondHeld](const FVaRestTraceSnapshot&) { SecondHeld->Reset(); };
	FVaRestTrace::AddSink(Sink);
	PumpUntil([] { return false; }, 0.05);
	FVaRestTrace::RemoveSink(Sink);
	const bool bInSnapshot = Sink->Snapshots.Num() == 1 && Sink->Snapshots[0].Clients.ContainsByPredicate([SecondId](const FVaRestTraceClient& Each) { return Each.ClientId == SecondId; });
	const bool bDestroyedSeen = Sink->ClientEvents(SecondId).ContainsByPredicate([](const FVaRestTraceClient& Each) { return !Each.bCreated; });
	TestTrue(TEXT("IN-12: the snapshot lists the client alive at AddSink"), bInSnapshot);
	TestTrue(TEXT("a client the snapshot listed is destroyed for the sink too: no ghost"), bDestroyedSeen);
	TestFalse(TEXT("... and the state is gone once the pins are released"), SecondWeak.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceSilentTest, "VaRest.Http.Trace.SilentIsSealed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceSilentTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TraceTestPrivate;
	// Spec 3.2: an owner gone, or the engine exiting, means no callbacks; Inspector 2.3: the record is still sealed.
	// Completed goes out before the silent return (IN-5: "before any callback", and also when none runs).
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Sink);
	FTestRig Rig;

	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	UObject* InFlightOwner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	auto Owned = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Running = Observe(Rig.Client.Get(TEXT("/owned")).Owner(InFlightOwner), Owned).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumInFlight() == 1; });
	InFlightOwner->MarkAsGarbage(); // TWeakObjectPtr::IsValid is false for garbage (WeakObjectPtr.h:290)
	Rig.Fake->Complete(0, FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{}"))));
	TestTrue(TEXT("the request whose owner went away in flight finishes"), PumpUntil([&Running] { return !Running.IsActive(); }));

	UObject* GoneOwner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	GoneOwner->MarkAsGarbage();
	auto Never = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle AtEntry = Observe(Rig.Client.Get(TEXT("/gone")).Owner(GoneOwner), Never).Send();
	TestTrue(TEXT("... and so does one whose owner was gone before entry"), PumpUntil([&AtEntry] { return !AtEntry.IsActive(); }));

	// Task 15 review m7: engine exit. CancelForShutdown() is process-wide, as the OnEnginePreExit hook calls it: in a headless
	// run nothing else is in flight; until the reset, every Send() anywhere completes silently (task 11 review, minor 6).
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	auto Exiting = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle AtExit = Observe(Rig.Client.Get(TEXT("/exiting")), Exiting).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumInFlight() == 1; });
	VaRest::Http::FClientState::CancelForShutdown();
	// Review N-6: the editor goes on, so every later test must send again -- also when this test returns early
	ON_SCOPE_EXIT { VaRest::Http::FClientState::ResetShutdownInternal(); };
	TestTrue(TEXT("... and so does one the engine exit canceled"), PumpUntil([&AtExit] { return !AtExit.IsActive(); }));
	PumpUntil([] { return false; }, 0.05);
	FVaRestTrace::RemoveSink(Sink);
	TestEqual(TEXT("spec 3.2: no callbacks"), Owned->CompleteCalls.load() + Never->CompleteCalls.load() + Exiting->CompleteCalls.load(), 0);
	TestEqual(TEXT("... and nothing is left in flight"), Rig.Client.GetInFlightCount(), 0);

	const TArray<FVaRestTraceEvent> InTransport = Sink->For(Running.GetId());
	TestEqualSensitive(TEXT("owner gone mid-attempt: the record is sealed all the same"), KindsOf(InTransport), FString(TEXT("Started,Dispatched,Ended,Completed")));
	if (InTransport.Num() == 4)
	{
		const FVaRestTraceCompleted& Done = InTransport[3].Payload.Get<FVaRestTraceCompleted>();
		TestTrue(TEXT("... Completed{Canceled, OwnerGone}"), Done.Outcome == EVaRestTraceOutcome::Canceled && Done.CancelReason == EVaRestCancelReason::OwnerGone);
	}
	const TArray<FVaRestTraceEvent> BeforeEntry = Sink->For(AtEntry.GetId());
	TestEqualSensitive(TEXT("owner gone before entry: Started, Completed"), KindsOf(BeforeEntry), FString(TEXT("Started,Completed")));
	if (BeforeEntry.Num() == 2)
	{
		const FVaRestTraceCompleted& Done = BeforeEntry[1].Payload.Get<FVaRestTraceCompleted>();
		TestTrue(TEXT("... Completed{Canceled, OwnerGone} too"), Done.Outcome == EVaRestTraceOutcome::Canceled && Done.CancelReason == EVaRestCancelReason::OwnerGone);
	}
	const TArray<FVaRestTraceEvent> Shutdown = Sink->For(AtExit.GetId());
	TestEqualSensitive(TEXT("engine exit mid-attempt: sealed too"), KindsOf(Shutdown), FString(TEXT("Started,Dispatched,Ended,Completed")));
	if (Shutdown.Num() == 4)
	{
		const FVaRestTraceCompleted& Done = Shutdown[3].Payload.Get<FVaRestTraceCompleted>();
		TestTrue(TEXT("... Completed{Canceled, Shutdown}"), Done.Outcome == EVaRestTraceOutcome::Canceled && Done.CancelReason == EVaRestCancelReason::Shutdown);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceLateAttemptTest, "VaRest.Http.Trace.LateAttemptSealed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceLateAttemptTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TraceTestPrivate;
	// Decision 24 (task 15 review m3): an attempt whose answer comes back after Completed is not reported -- the record is
	// sealed (Inspector 2.3). A stage answers the attempt itself while the transport still runs; the transport answers later.
	// M4a final review A1 (c): the answer cancels the call at the attempt's end, and the request completes inside the same
	// call (the stage answers from the test body, not inside Send(), so Complete does not defer), so the call's Canceled end
	// always comes after Completed.
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Sink);
	FTestRig Rig;
	const TSharedRef<FHeldAnswerStage, ESPMode::ThreadSafe> Stage = MakeShared<FHeldAnswerStage, ESPMode::ThreadSafe>();
	Rig.Client.Use(Stage, EVaRestStageSlot::PerAttempt);
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/early")), Outcome).Send();
	Stage->AnswerNow();
	TestTrue(TEXT("the stage's answer completes the request"), Rig.Wait(Outcome));
	TestTrue(TEXT("... and cancels the transport call it left"), Rig.Fake->NumCalls() == 1 && Rig.Fake->GetCall(0).bCanceled);
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Late = Stage->LateAnswers;
	TestTrue(TEXT("the transport's answer comes back after Completed"), PumpUntil([Late] { return Late->load() == 1; }));
	FVaRestTrace::RemoveSink(Sink);
	TestEqualSensitive(TEXT("decision 24: no AttemptEnded after Completed"), KindsOf(Sink->For(Handle.GetId())), FString(TEXT("Started,Dispatched,Completed")));
	TestEqual(TEXT("... and the success was delivered once"), Outcome->SuccessCalls.load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTracePathTemplateTest, "VaRest.Http.Trace.PathTemplateErrorRedacted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTracePathTemplateTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TraceTestPrivate;
	// Final review M8: a path-template error's text reaches Error.Message, the Basic Warning (escaped, not redacted) and the
	// trace (value-scrubbed only); an absolute Path with userinfo must not carry its password into any of them
	TGuardConsoleVariable<int32> Level(TEXT("VaRest.LogLevel"), -1);
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Sink);
	FTestRig Rig;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Basic);
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("-> Config error: unclosed '{'"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("-> Config error: unmatched '}'"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("-> Config error: empty {}"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("-> Config error: path parameter \"x\" is not in"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> Outcomes;
	TArray<FVaRestRequestHandle> Handles;
	auto SendOne = [&Outcomes, &Handles](TVaRestRequest<FVaRestJson>&& Request)
	{
		Outcomes.Add(MakeOutcome<FVaRestJson>());
		Handles.Add(Observe(MoveTemp(Request), Outcomes.Last()).Send());
	};
	SendOne(Rig.Client.Get(TEXT("https://u:canary-pw@h.test/{x")));
	SendOne(Rig.Client.Get(TEXT("https://u:canary-pw@h.test/x}")));
	SendOne(Rig.Client.Get(TEXT("https://u:canary-pw@h.test/{}")));
	SendOne(Rig.Client.Get(TEXT("https://u:canary-pw@h.test/a")).Path(TEXT("x"), TEXT("1")));
	TestTrue(TEXT("all complete"), PumpUntil([&Outcomes] { for (const auto& O : Outcomes) { if (!O->IsDone()) { return false; } } return true; }));
	FVaRestTrace::RemoveSink(Sink);
	for (int32 Index = 0; Index < Outcomes.Num(); ++Index)
	{
		const FVaRestError& Error = Outcomes[Index]->Error;
		TestTrue(FString::Printf(TEXT("#%d: Kind Config"), Index), Error.Kind == EVaRestErrorKind::Config);
		TestFalse(FString::Printf(TEXT("#%d: no password in Error.Message"), Index), Error.Message.Contains(TEXT("canary-pw")));
		TestTrue(FString::Printf(TEXT("#%d: the template, redacted, in the message"), Index), Error.Message.Contains(TEXT("(path template \"https://***@h.test/")));
		bool bCompleted = false;
		for (const FVaRestTraceEvent& Event : Sink->For(Handles[Index].GetId()))
		{
			if (Event.Kind == EVaRestTraceKind::Started)
			{
				const FVaRestTraceStarted& Started = Event.Payload.Get<FVaRestTraceStarted>();
				TestFalse(FString::Printf(TEXT("#%d: no password in the trace's URL or template"), Index), Started.Url.Contains(TEXT("canary-pw"))
					|| Started.PathTemplate.Contains(TEXT("canary-pw")));
			}
			else if (Event.Kind == EVaRestTraceKind::Completed)
			{
				const FString& Traced = Event.Payload.Get<FVaRestTraceCompleted>().Error.Message;
				bCompleted = true;
				TestTrue(FString::Printf(TEXT("#%d: the trace's error message, without the password"), Index), Traced.Contains(TEXT("path template"))
					&& !Traced.Contains(TEXT("canary-pw")));
			}
		}
		TestTrue(FString::Printf(TEXT("#%d: Completed was traced"), Index), bCompleted);
	}
	TestEqual(TEXT("four Basic lines, one per request"), Capture.Count(TEXT("-> Config error: ")), 4);
	TestFalse(TEXT("no password in any log line"), Capture.Contains(TEXT("canary-pw")));
	TestEqual(TEXT("never sent"), Rig.Fake->NumCalls(), 0);
	return true;
}

#endif
