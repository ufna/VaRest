// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tasks/Task.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestTracePoints.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceM4PayloadsTest, "VaRest.Http.TraceSurface.M4Payloads",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceM4PayloadsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	const uint64 Built = FVaRestTrace::Counters().EventsBuilt.load();
	{
		// IN-11: no sink, nothing built
		FVaRestScopedSinkIsolation Quiet;
		TracePoints::Marker(*FClientAccess::State(Rig.Client), EVaRestTraceMarkerKind::SessionRefreshed, 0, TEXT("x"));
		TestEqual(TEXT("a marker with no sink builds nothing"), FVaRestTrace::Counters().EventsBuilt.load(), Built);
	}
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	FTraceRecorder& Recorder = *Scoped.Recorder;

	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/a")), Outcome).Send();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the recorder sees one request's kinds"), Recorder.KindsOf(Handle.GetId()), FString(TEXT("Started,Dispatched,Ended,Completed")));

	// IN-9: a marker's text is value-scrubbed with the client's current secrets
	Rig.Client.SetSecretValue(TEXT("S"), TEXT("marker-secret-12345"));
	TracePoints::Marker(*FClientAccess::State(Rig.Client), EVaRestTraceMarkerKind::SessionExpired, Handle.GetId(), TEXT("refresh said marker-secret-12345 is bad"));
	const TArray<FVaRestTraceMarker> Markers = Recorder.MarkersOf(Rig.Client.GetId());
	if (!TestEqual(TEXT("one marker"), Markers.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("... its kind, client and cause"), Markers[0].Kind == EVaRestTraceMarkerKind::SessionExpired && Markers[0].ClientId == Rig.Client.GetId()
		&& Markers[0].CauseRequestId == Handle.GetId());
	TestTrue(TEXT("... its text scrubbed"), !Markers[0].Text.Contains(TEXT("marker-secret-12345")) && Markers[0].Text.Contains(TEXT("***")));

	// IN-6: an excerpt may come from a background thread; it is recorded and counted as such. Waited for with a flag, not
	// Wait(): Wait() retracts an unstarted task and runs it here (Task.h:65-80, TaskPrivate.cpp:246-251), and the excerpt
	// would then come from the game thread (concurrency review M-11). PumpUntil runs game-thread tasks only.
	const uint64 Id = Handle.GetId();
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bEmitted = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	UE::Tasks::Launch(TEXT("VaRest.Test.Excerpt"), [Id, bEmitted]
	{
		FVaRestTraceExcerptEvent Payload;
		Payload.Owner = EVaRestTraceExcerptOwner::Response;
		Payload.RequestId = Id;
		Payload.Attempt = 1;
		Payload.Excerpt.Kind = EVaRestExcerptKind::Skipped;
		Payload.Excerpt.SkipReason = EVaRestExcerptSkip::Backlog;
		FVaRestTrace::Emit(EVaRestTraceKind::Excerpt, 0.0, FVaRestTracePayload(TInPlaceType<FVaRestTraceExcerptEvent>(), MoveTemp(Payload)));
		bEmitted->store(true);
	});
	if (!TestTrue(TEXT("the excerpt task ran"), PumpUntil([bEmitted] { return bEmitted->load(); })))
	{
		return false;
	}
	const TArray<FVaRestTraceExcerptEvent> Excerpts = Recorder.ExcerptsOf(Id);
	TestTrue(TEXT("the excerpt arrives, keyed by request"), Excerpts.Num() == 1 && Excerpts[0].Excerpt.SkipReason == EVaRestExcerptSkip::Backlog);
	TestTrue(TEXT("only excerpts came from another thread"), Recorder.OnlyExcerptsOffGameThread());
	TestEqualSensitive(TEXT("KindsOf leaves excerpts out (they race the game thread)"), Recorder.KindsOf(Id), FString(TEXT("Started,Dispatched,Ended,Completed")));

	// IN-4 on the game thread through TracePoints::Note, which stamps the request and attempt
	FVaRestTraceNote Note;
	Note.Kind = EVaRestTraceNoteKind::RetryScheduled;
	Note.Reason = TEXT("Status503");
	Note.Delay = 0.5;
	FVaRestTrace::Emit(EVaRestTraceKind::Note, 0.0, FVaRestTracePayload(TInPlaceType<FVaRestTraceNote>(), MoveTemp(Note)));
	TestTrue(TEXT("a note with no request id is recorded under 0"), Recorder.For(0).ContainsByPredicate([](const FVaRestTraceEvent& Event)
	{
		return Event.Kind == EVaRestTraceKind::Note && Event.Payload.Get<FVaRestTraceNote>().Reason == FName(TEXT("Status503"));
	}));

	// IN-14's admission cap is one of the merged caps
	class FCapSink final : public IVaRestTraceSink
	{
	public:
		virtual FVaRestTraceDetailSettings GetDetail() const override
		{
			FVaRestTraceDetailSettings Settings;
			Settings.Flags = EVaRestTraceDetail::Metadata;
			Settings.MaxPendingExcerptBytes = 1234;
			return Settings;
		}
		virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
		virtual void OnEvent(const FVaRestTraceEvent&) override {}
	};
	const TSharedRef<FCapSink, ESPMode::ThreadSafe> Caps = MakeShared<FCapSink, ESPMode::ThreadSafe>();
	FVaRestTrace::AddSink(Caps);
	TestEqual(TEXT("MaxPendingExcerptBytes merges as the largest"), FVaRestTrace::GetDetailSettings().MaxPendingExcerptBytes, static_cast<int64>(1234));
	FVaRestTrace::RemoveSink(Caps);
	return true;
}

#endif
