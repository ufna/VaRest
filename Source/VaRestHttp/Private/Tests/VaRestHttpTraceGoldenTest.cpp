// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestClient.h"
#include "VaRestEnvelope.h"
#include "VaRestHttpTesting.h"
#include "VaRestRetry.h"

/**
 * Inspector IN-17: the core-owned goldens M4a owns. Each pins a request's event kinds in order and the phase values on the
 * virtual clock, so a change in either is a deliberate one; a golden that fails names the task whose trace point moved
 * (M4a Task 5: the Note payload; Task 8: the retry stage, its note and gap; Task 11: the envelope on the decode task; M3:
 * cancel and the attach snapshot). Every golden also checks Inspector 3.3's invariant (PhasesHold, spec review I-1). The
 * auth goldens are in VaRestHttpAuthTest.cpp, beside FHandAuth; the decode-warning one is in VaRestTests (it needs a USTRUCT);
 * the stale-dispatch ones (M4a final review A1) are in VaRestHttpExcerptTest.cpp, beside the excerpt helpers they use.
 */
namespace VaRest::Http::GoldenTestPrivate
{
	FVaRestFakeResponse Json(int32 Code, const char* Text = "{}")
	{
		return FVaRestFakeResponse::Json(Code, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	FVaRestRetryPolicy Exact()
	{
		FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(3);
		Policy.Backoff.Jitter = EVaRestJitter::None;
		return Policy;
	}

	/** Every event of Kind for request Id, with its time, payloads copied out (For() returns a copy of the list) */
	template <typename PayloadType>
	TArray<TPair<double, PayloadType>> All(const VaRest::Http::Testing::FTraceRecorder& Recorder, uint64 Id, EVaRestTraceKind Kind)
	{
		TArray<TPair<double, PayloadType>> Out;
		for (const FVaRestTraceEvent& Event : Recorder.For(Id))
		{
			if (Event.Kind == Kind)
			{
				Out.Emplace(Event.Time, Event.Payload.Get<PayloadType>());
			}
		}
		return Out;
	}

	/** The rig's work settled and the redaction pipe idle: nothing of this golden reaches the next test */
	void Settle(VaRest::Http::Testing::FTestRig& Rig)
	{
		Rig.Drain();
		VaRest::Http::Testing::FlushRedactionPipe();
	}

	/** Cancels what the rig has in flight and pumps until it is done: the drain of a test that stops early (spec review M-5) */
	void Abandon(VaRest::Http::Testing::FTestRig& Rig)
	{
		Rig.Client.CancelAll();
		VaRest::Http::Testing::PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
		Settle(Rig);
	}

	bool SeqIncreases(const VaRest::Http::Testing::FTraceRecorder& Recorder, uint64 Id)
	{
		uint64 Last = 0;
		for (const FVaRestTraceEvent& Event : Recorder.For(Id))
		{
			if (Event.Seq <= Last)
			{
				return false;
			}
			Last = Event.Seq;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenRetryAfterTest, "VaRest.Http.Trace.Golden.RetryAfter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenRetryAfterTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::GoldenTestPrivate;
	// IN-17 retry with Retry-After (Task 8): the note says why and how long, the next attempt's GapBefore is the backoff
	FTestRig Rig;
	Rig.Client.WithRetry(Exact());
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	FVaRestFakeResponse Busy = Json(503);
	Busy.Headers.Set(TEXT("Retry-After"), TEXT("2"));
	Rig.Fake->Enqueue(Busy);
	Rig.Fake->Enqueue(Json(200));
	auto Outcome = MakeOutcome<FVaRestJson>();
	const uint64 Id = Observe(Rig.Client.Get(TEXT("/busy")), Outcome).Send().GetId();
	if (!TestTrue(TEXT("backoff armed"), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(2.0);
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	Settle(Rig);
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	TestEqualSensitive(TEXT("the sequence"), Recorder.KindsOf(Id), FString(TEXT("Started,Dispatched,Ended,Note,Dispatched,Ended,Completed")));
	TestTrue(TEXT("Seq increases"), SeqIncreases(Recorder, Id));
	FString Why;
	TestTrue(TEXT("Inspector 3.3: the phases hold"), PhasesHold(Recorder, Id, Why));
	TestEqualSensitive(TEXT("... (why not)"), Why, FString());
	const auto Dispatched = All<FVaRestTraceAttemptDispatched>(Recorder, Id, EVaRestTraceKind::AttemptDispatched);
	const auto Ended = All<FVaRestTraceAttemptEnded>(Recorder, Id, EVaRestTraceKind::AttemptEnded);
	const auto Notes = All<FVaRestTraceNote>(Recorder, Id, EVaRestTraceKind::Note);
	const auto Completed = All<FVaRestTraceCompleted>(Recorder, Id, EVaRestTraceKind::Completed);
	if (!TestTrue(TEXT("the counts"), Dispatched.Num() == 2 && Ended.Num() == 2 && Notes.Num() == 1 && Completed.Num() == 1))
	{
		return false;
	}
	TestTrue(TEXT("attempt 1 at 0 s, no gap"), Dispatched[0].Value.Attempt == 1 && Dispatched[0].Value.AttemptStart == 0.0 && Dispatched[0].Value.GapKind == EVaRestTraceGap::None);
	TestTrue(TEXT("... ends 503"), Ended[0].Value.Attempt == 1 && Ended[0].Value.Status == 503);
	TestTrue(TEXT("the note at 0 s: RetryScheduled, Status503, 2 s, Retry-After 2"), Notes[0].Key == 0.0 && Notes[0].Value.Kind == EVaRestTraceNoteKind::RetryScheduled
		&& Notes[0].Value.Reason == FName(TEXT("Status503")) && Notes[0].Value.Delay == 2.0 && Notes[0].Value.RetryAfter == 2.0);
	TestTrue(TEXT("attempt 2 at 2 s after a 2 s backoff"), Dispatched[1].Value.Attempt == 2 && Dispatched[1].Value.AttemptStart == 2.0
		&& Dispatched[1].Value.GapKind == EVaRestTraceGap::Backoff && Dispatched[1].Value.GapSeconds == 2.0);
	TestTrue(TEXT("... ends 200"), Ended[1].Value.Status == 200);
	TestTrue(TEXT("Completed at 2 s: a success"), Completed[0].Key == 2.0 && Completed[0].Value.Outcome == EVaRestTraceOutcome::Success);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenCancelTest, "VaRest.Http.Trace.Golden.Cancel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenCancelTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::GoldenTestPrivate;
	// IN-17 cancel: one request canceled in the transport, one canceled while it backs off (Task 20: no retry for it)
	FTestRig Rig;
	Rig.Client.WithRetry(Exact());
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Url.EndsWith(TEXT("/wait")) ? FVaRestFakeResponse::Manual() : Json(503); });
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto InTransport = MakeOutcome<FVaRestJson>();
	auto BackingOff = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle A = Observe(Rig.Client.Get(TEXT("/wait")), InTransport).Send();
	const FVaRestRequestHandle B = Observe(Rig.Client.Get(TEXT("/busy")), BackingOff).Send();
	if (!TestTrue(TEXT("one in the transport, one backing off"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	A.Cancel();
	B.Cancel();
	if (!TestTrue(TEXT("both complete"), PumpUntil([InTransport, BackingOff] { return InTransport->IsDone() && BackingOff->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Settle(Rig);
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	for (const uint64 Each : { A.GetId(), B.GetId() })
	{
		FString Why;
		const bool bHold = PhasesHold(Recorder, Each, Why);
		TestTrue(FString::Printf(TEXT("#%llu: Inspector 3.3's phases hold (%s)"), Each, *Why), bHold);
		TestTrue(FString::Printf(TEXT("#%llu: Seq increases"), Each), SeqIncreases(Recorder, Each));
	}
	TestEqualSensitive(TEXT("canceled in the transport"), Recorder.KindsOf(A.GetId()), FString(TEXT("Started,Dispatched,Ended,Completed")));
	TestEqualSensitive(TEXT("canceled while backing off: the note, then Completed"), Recorder.KindsOf(B.GetId()), FString(TEXT("Started,Dispatched,Ended,Note,Completed")));
	const auto EndedA = All<FVaRestTraceAttemptEnded>(Recorder, A.GetId(), EVaRestTraceKind::AttemptEnded);
	TestTrue(TEXT("IN-3: the transport's attempt ended Canceled"), EndedA.Num() == 1 && EndedA[0].Value.Failure == EVaRestTransportFailure::Canceled);
	const auto EndedB = All<FVaRestTraceAttemptEnded>(Recorder, B.GetId(), EVaRestTraceKind::AttemptEnded);
	TestTrue(TEXT("... the backing-off one's only attempt ended 503"), EndedB.Num() == 1 && EndedB[0].Value.Status == 503);
	for (const uint64 Id : { A.GetId(), B.GetId() })
	{
		const auto Starts = All<FVaRestTraceAttemptDispatched>(Recorder, Id, EVaRestTraceKind::AttemptDispatched);
		const auto Ends = All<FVaRestTraceAttemptEnded>(Recorder, Id, EVaRestTraceKind::AttemptEnded);
		TestTrue(TEXT("cancellation preserves physical identity"), Starts.Num() == 1 && Ends.Num() == 1
			&& Starts[0].Value.DispatchId != 0 && Ends[0].Value.DispatchId == Starts[0].Value.DispatchId);
		const auto Completed = All<FVaRestTraceCompleted>(Recorder, Id, EVaRestTraceKind::Completed);
		TestTrue(FString::Printf(TEXT("#%llu: Completed Canceled, reason User"), Id), Completed.Num() == 1 && Completed[0].Value.Outcome == EVaRestTraceOutcome::Canceled
			&& Completed[0].Value.CancelReason == EVaRestCancelReason::User && Completed[0].Key == 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenEnvelopeTest, "VaRest.Http.Trace.Golden.EnvelopeFailure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenEnvelopeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::GoldenTestPrivate;
	// IN-17 envelope failure (Task 11): judged on the decode task, Kind=Server at decode, never retried -- one attempt even
	// with a retry policy
	FTestRig Rig;
	Rig.Client.WithRetry(Exact());
	Rig.Client.WithEnvelope(FVaRestEnvelope::DataAt(TEXT("data")).SuccessWhen(TEXT("ok"), { TEXT("true") }).ErrorCodeAt(TEXT("code")).ErrorMessageAt(TEXT("message")));
	Rig.Fake->Enqueue(Json(200, R"({"ok":false,"code":"E42","message":"the envelope says no"})"));
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const uint64 Id = Observe(Rig.Client.Get(TEXT("/e")), Outcome).Send().GetId();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	Settle(Rig);
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	TestEqualSensitive(TEXT("the sequence: one attempt, no retry note"), Recorder.KindsOf(Id), FString(TEXT("Started,Dispatched,Ended,Completed")));
	TestTrue(TEXT("Seq increases"), SeqIncreases(Recorder, Id));
	FString Why;
	TestTrue(TEXT("Inspector 3.3: the phases hold"), PhasesHold(Recorder, Id, Why));
	TestEqualSensitive(TEXT("... (why not)"), Why, FString());
	const auto Ended = All<FVaRestTraceAttemptEnded>(Recorder, Id, EVaRestTraceKind::AttemptEnded);
	TestTrue(TEXT("the attempt ended 200: the failure is the envelope's, not the transport's"), Ended.Num() == 1 && Ended[0].Value.Status == 200
		&& Ended[0].Value.Failure == EVaRestTransportFailure::None);
	const auto Completed = All<FVaRestTraceCompleted>(Recorder, Id, EVaRestTraceKind::Completed);
	if (!TestEqual(TEXT("one Completed"), Completed.Num(), 1))
	{
		return false;
	}
	const FVaRestTraceCompleted& Done = Completed[0].Value;
	TestTrue(TEXT("an error, Kind Server, the envelope's code"), Done.Outcome == EVaRestTraceOutcome::Error && Done.Error.Kind == EVaRestErrorKind::Server
		&& Done.Error.HttpStatus == 200 && Done.Error.Code == TEXT("E42"));
	TestTrue(TEXT("IN-5 Envelope: the failed predicate"), Done.Envelope.bApplied && Done.Envelope.DataPath == TEXT("data") && Done.Envelope.PredicatePath == TEXT("ok")
		&& Done.Envelope.Expected == TEXT("true") && Done.Envelope.Actual == TEXT("false"));
	TestTrue(TEXT("... and the envelope's code and message"), Done.Envelope.Code == TEXT("E42") && Done.Envelope.Message == TEXT("the envelope says no"));
	TestTrue(TEXT("decode timed (the mask was on)"), Done.DecodeStartAt >= 0.0 && Done.DecodeEndAt >= Done.DecodeStartAt);
	TestTrue(TEXT("Completed at 0 s"), Completed[0].Key == 0.0);
	TestEqual(TEXT("the callback saw one attempt"), Outcome->Error.Attempts, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenEnvelopeConfigRedactionTest, "VaRest.Http.Trace.Golden.EnvelopeConfigRedaction",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenEnvelopeConfigRedactionTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::GoldenTestPrivate;
	// Configured envelope strings obey the same known-secret contract as server text; the unregistered case proves
	// ordinary nonempty configuration and the existing Actual/Code/Message still reach the real Metadata sink.
	for (const bool bKnownSecret : { false, true })
	{
		AddInfo(bKnownSecret ? TEXT("EnvelopeConfigRedaction: registered-secret configuration") : TEXT("EnvelopeConfigRedaction: benign configuration control"));
		FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
		FTestRig Rig;
		Rig.Client.WithRetry(Exact());
		if (bKnownSecret)
		{
			Rig.Client.SetSecretValue(TEXT("EnvelopeData"), TEXT("EnvelopeDataCanary57391"));
			Rig.Client.SetSecretValue(TEXT("EnvelopePredicate"), TEXT("EnvelopePredicateCanary68402"));
			Rig.Client.SetSecretValue(TEXT("EnvelopeExpected"), TEXT("EnvelopeExpectedCanary79513"));
		}
		Rig.Client.WithEnvelope(FVaRestEnvelope::DataAt(TEXT("data.EnvelopeDataCanary57391"))
			.SuccessWhen(TEXT("checks.EnvelopePredicateCanary68402"), { TEXT("accepted-EnvelopeExpectedCanary79513") })
			.ErrorCodeAt(TEXT("code")).ErrorMessageAt(TEXT("message")));
		Rig.Fake->Enqueue(Json(200, R"({"data":{"EnvelopeDataCanary57391":{"value":7}},"checks":{"EnvelopePredicateCanary68402":"ordinary-denied"},"code":"E_CONFIG","message":"ordinary configuration refused"})"));
		auto Outcome = MakeOutcome<FVaRestJson>();
		const uint64 Id = Observe(Rig.Client.Get(TEXT("/envelope-config")), Outcome).Send().GetId();
		if (!TestTrue(TEXT("envelope configuration case completes"), Rig.Wait(Outcome)))
		{
			Abandon(Rig);
			return false;
		}
		Settle(Rig);
		const FTraceRecorder& Recorder = *Scoped.Recorder;
		TestEqualSensitive(TEXT("envelope configuration sequence has one attempt and no retry note"), Recorder.KindsOf(Id), FString(TEXT("Started,Dispatched,Ended,Completed")));
		TestTrue(TEXT("envelope configuration Seq increases"), SeqIncreases(Recorder, Id));
		FString Why;
		TestTrue(TEXT("envelope configuration phases hold"), PhasesHold(Recorder, Id, Why));
		TestEqualSensitive(TEXT("envelope configuration phase explanation empty"), Why, FString());
		const auto Ended = All<FVaRestTraceAttemptEnded>(Recorder, Id, EVaRestTraceKind::AttemptEnded);
		TestTrue(TEXT("envelope configuration transport really ended 200"), Ended.Num() == 1 && Ended[0].Value.Status == 200
			&& Ended[0].Value.Failure == EVaRestTransportFailure::None);
		const auto Completed = All<FVaRestTraceCompleted>(Recorder, Id, EVaRestTraceKind::Completed);
		if (!TestEqual(TEXT("envelope configuration has one Completed"), Completed.Num(), 1)) return false;
		const FVaRestTraceCompleted& Done = Completed[0].Value;
		TestTrue(TEXT("envelope configuration is a real applied Server predicate failure"), Done.RequestId == Id && Done.Envelope.bApplied
			&& Done.Outcome == EVaRestTraceOutcome::Error && Done.Error.Kind == EVaRestErrorKind::Server
			&& Done.Error.HttpStatus == 200 && Done.Response.Status == 200 && Outcome->Error.Kind == EVaRestErrorKind::Server);
		TestTrue(TEXT("envelope configuration fields remain nonempty"), !Done.Envelope.DataPath.IsEmpty() && !Done.Envelope.PredicatePath.IsEmpty() && !Done.Envelope.Expected.IsEmpty());
		if (bKnownSecret)
		{
			TestEqualSensitive(TEXT("IN-5 Envelope.DataPath configured secret value-scrubbed"), Done.Envelope.DataPath, FString(TEXT("data.***")));
			TestEqualSensitive(TEXT("IN-5 Envelope.PredicatePath configured secret value-scrubbed"), Done.Envelope.PredicatePath, FString(TEXT("checks.***")));
			TestEqualSensitive(TEXT("IN-5 Envelope.Expected configured secret value-scrubbed"), Done.Envelope.Expected, FString(TEXT("accepted-***")));
		}
		else
		{
			TestEqualSensitive(TEXT("benign configured DataPath preserved"), Done.Envelope.DataPath, FString(TEXT("data.EnvelopeDataCanary57391")));
			TestEqualSensitive(TEXT("benign configured PredicatePath preserved"), Done.Envelope.PredicatePath, FString(TEXT("checks.EnvelopePredicateCanary68402")));
			TestEqualSensitive(TEXT("benign configured Expected preserved"), Done.Envelope.Expected, FString(TEXT("accepted-EnvelopeExpectedCanary79513")));
		}
		TestEqualSensitive(TEXT("envelope configuration benign Actual preserved"), Done.Envelope.Actual, FString(TEXT("ordinary-denied")));
		TestEqualSensitive(TEXT("envelope configuration benign Code preserved"), Done.Envelope.Code, FString(TEXT("E_CONFIG")));
		TestEqualSensitive(TEXT("envelope configuration benign Message preserved"), Done.Envelope.Message, FString(TEXT("ordinary configuration refused")));
		TestTrue(TEXT("envelope configuration Error code and message preserved"), Done.Error.Code == TEXT("E_CONFIG") && Done.Error.Message == TEXT("ordinary configuration refused"));
		TestTrue(TEXT("envelope configuration decode timed and receipt unchanged"), Done.DecodeStartAt >= 0.0 && Done.DecodeEndAt >= Done.DecodeStartAt && Completed[0].Key == 0.0);
		TestEqual(TEXT("envelope configuration callback sees one attempt"), Outcome->Error.Attempts, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenAttachTest, "VaRest.Http.Trace.Golden.AttachMidFlight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenAttachTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::GoldenTestPrivate;
	// IN-12 + IN-17: a sink that attaches while one request is in the transport and one backs off gets one coherent
	// record for each: the snapshot's Started{Partial}, then the rest
	FTestRig Rig;
	Rig.Client.WithRetry(Exact());
	TSharedRef<int32, ESPMode::ThreadSafe> BusyCalls = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Rig.Fake->SetResponder([BusyCalls](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.EndsWith(TEXT("/wait")))
		{
			return FVaRestFakeResponse::Manual();
		}
		return ++*BusyCalls == 1 ? Json(503) : Json(200);
	});
	auto InTransport = MakeOutcome<FVaRestJson>();
	auto BackingOff = MakeOutcome<FVaRestJson>();
	const uint64 A = Observe(Rig.Client.Get(TEXT("/wait")), InTransport).Send().GetId();
	const uint64 B = Observe(Rig.Client.Get(TEXT("/busy")), BackingOff).Send().GetId();
	if (!TestTrue(TEXT("one in the transport, one backing off"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	const TArray<FVaRestTraceSnapshot> Snapshots = Scoped.Recorder->GetSnapshots();
	if (!TestEqual(TEXT("one snapshot"), Snapshots.Num(), 1))
	{
		Abandon(Rig);
		return false;
	}
	const auto Partial = [&Snapshots](uint64 Id) { return Snapshots[0].InFlight.FindByPredicate([Id](const FVaRestTraceStarted& Entry) { return Entry.RequestId == Id; }); };
	const FVaRestTraceStarted* PartialA = Partial(A);
	const FVaRestTraceStarted* PartialB = Partial(B);
	TestTrue(TEXT("both in the snapshot as Partial, at attempt 1, started at 0 s"), PartialA && PartialB && EnumHasAnyFlags(PartialA->Flags, EVaRestTraceRequestFlags::Partial)
		&& EnumHasAnyFlags(PartialB->Flags, EVaRestTraceRequestFlags::Partial) && PartialA->CurrentAttempt == 1 && PartialB->CurrentAttempt == 1
		&& PartialA->StartedAt == 0.0 && PartialB->StartedAt == 0.0);
	Rig.Fake->Complete(0, Json(200));
	// A's answer reaches the game thread before the clock moves, so its times are exact: the fake's completion is queued for
	// a worker, and an Advance before it lands would stamp A's AttemptEnded and Completed at 0.5 s
	if (!TestTrue(TEXT("A completes"), PumpUntil([InTransport] { return InTransport->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("both complete"), PumpUntil([InTransport, BackingOff] { return InTransport->IsDone() && BackingOff->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Settle(Rig);
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	FString Why;
	TestTrue(TEXT("Inspector 3.3: B's retry's phases hold"), PhasesHold(Recorder, B, Why));
	TestEqualSensitive(TEXT("... (why not)"), Why, FString());
	TestTrue(TEXT("Seq increases for both"), SeqIncreases(Recorder, A) && SeqIncreases(Recorder, B));
	TestEqualSensitive(TEXT("A: the rest of its record"), Recorder.KindsOf(A), FString(TEXT("Ended,Completed")));
	TestEqualSensitive(TEXT("B: its retry and the end"), Recorder.KindsOf(B), FString(TEXT("Dispatched,Ended,Completed")));
	const auto EndedA = All<FVaRestTraceAttemptEnded>(Recorder, A, EVaRestTraceKind::AttemptEnded);
	TestTrue(TEXT("A: attempt 1 ended 200 at 0 s"), EndedA.Num() == 1 && EndedA[0].Value.Attempt == 1 && EndedA[0].Value.Status == 200 && EndedA[0].Key == 0.0);
	const auto Dispatched = All<FVaRestTraceAttemptDispatched>(Recorder, B, EVaRestTraceKind::AttemptDispatched);
	TestTrue(TEXT("B: attempt 2 at 0.5 s after a 0.5 s backoff"), Dispatched.Num() == 1 && Dispatched[0].Value.Attempt == 2 && Dispatched[0].Value.GapKind == EVaRestTraceGap::Backoff
		&& Dispatched[0].Value.GapSeconds == 0.5 && Dispatched[0].Value.AttemptStart == 0.5 && Dispatched[0].Key == 0.5);
	for (const uint64 Id : { A, B })
	{
		const auto Completed = All<FVaRestTraceCompleted>(Recorder, Id, EVaRestTraceKind::Completed);
		TestTrue(FString::Printf(TEXT("#%llu: Completed, a success, at %.1f s"), Id, Id == A ? 0.0 : 0.5), Completed.Num() == 1
			&& Completed[0].Value.Outcome == EVaRestTraceOutcome::Success && Completed[0].Key == (Id == A ? 0.0 : 0.5));
	}
	return true;
}

#endif
