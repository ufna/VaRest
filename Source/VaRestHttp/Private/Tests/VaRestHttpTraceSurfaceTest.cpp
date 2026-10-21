// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/ParallelFor.h"
#include "Misc/AutomationTest.h"
#include "VaRestTrace.h"

namespace VaRest::Http::TraceSurfaceTestPrivate
{
	class FSink final : public IVaRestTraceSink
	{
	public:
		explicit FSink(EVaRestTraceDetail InFlags, int32 InBodyCap = 0)
		{
			Detail.Flags = InFlags;
			Detail.BodyCap = InBodyCap;
		}
		virtual FVaRestTraceDetailSettings GetDetail() const override { return Detail; }
		virtual void OnAttached(const FVaRestTraceSnapshot& Snapshot) override
		{
			++Attached;
			LastSnapshot = Snapshot;
		}
		virtual void OnEvent(const FVaRestTraceEvent& Event) override { Events.Add(Event); }

		FVaRestTraceDetailSettings Detail;
		int32 Attached = 0;
		FVaRestTraceSnapshot LastSnapshot;
		TArray<FVaRestTraceEvent> Events;
	};

	/** Calls back into the registry from every hook the registry runs, which a lock held across them would deadlock on */
	class FReentrantSink final : public IVaRestTraceSink
	{
	public:
		virtual ~FReentrantSink() override { Touch(); }
		virtual FVaRestTraceDetailSettings GetDetail() const override
		{
			Touch();
			FVaRestTraceDetailSettings Result;
			Result.Flags = EVaRestTraceDetail::Metadata;
			return Result;
		}
		virtual void OnAttached(const FVaRestTraceSnapshot&) override
		{
			Touch();
			++Attached;
		}
		virtual void OnEvent(const FVaRestTraceEvent& Event) override
		{
			Events.Add(Event.Seq);
			if (OnEventHook)
			{
				OnEventHook();
			}
		}

		static void Touch()
		{
			(void)FVaRestTrace::GetSinks();
			(void)FVaRestTrace::GetDetailSettings();
		}

		int32 Attached = 0;
		TArray<uint64> Events;
		TFunction<void()> OnEventHook;
	};

	FVaRestTracePayload Started(uint64 RequestId)
	{
		FVaRestTraceStarted Payload;
		Payload.RequestId = RequestId;
		return FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), MoveTemp(Payload));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceMaskTest, "VaRest.Http.TraceSurface.MaskAndSinks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceMaskTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TraceSurfaceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	TestTrue(TEXT("isolated: no sink, mask 0"), FVaRestTrace::Detail() == EVaRestTraceDetail::None);

	const uint64 BuiltBefore = FVaRestTrace::Counters().EventsBuilt;
	FVaRestTrace::Emit(EVaRestTraceKind::Started, 1.0, Started(1));
	TestEqual(TEXT("no sink: nothing is built"), FVaRestTrace::Counters().EventsBuilt.load(), BuiltBefore);

	// A asks for the larger body cap and B for the larger binary and frame caps, so "last wins" and a mixed-up field both fail
	TSharedRef<FSink, ESPMode::ThreadSafe> A = MakeShared<FSink, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata, 2048);
	FVaRestTrace::AddSink(A);
	TestEqual(TEXT("OnAttached runs once, from AddSink"), A->Attached, 1);
	// Presence, not counts: later tasks add snapshot contributors that this test does not isolate (review M-4)
	TestTrue(TEXT("the snapshot has scope 0 \"Process\" first (IN-7)"), A->LastSnapshot.Scopes.Num() >= 1
		&& A->LastSnapshot.Scopes[0].ScopeId == FVaRestTrace::ProcessScopeId && A->LastSnapshot.Scopes[0].Label == TEXT("Process"));
	TestTrue(TEXT("mask = the sink's detail"), FVaRestTrace::Detail() == EVaRestTraceDetail::Metadata);

	FVaRestTrace::Emit(EVaRestTraceKind::Started, 2.0, Started(42));
	FVaRestTrace::Emit(EVaRestTraceKind::Started, 3.0, Started(43));
	TestEqual(TEXT("events reach the sink"), A->Events.Num(), 2);
	TestTrue(TEXT("payload kept"), A->Events.Num() == 2 && A->Events[0].Payload.Get<FVaRestTraceStarted>().RequestId == 42);
	TestTrue(TEXT("Seq increases"), A->Events.Num() == 2 && A->Events[1].Seq > A->Events[0].Seq && A->Events[0].Seq > 0);
	TestTrue(TEXT("Time is the caller's clock reading"), A->Events.Num() == 2 && A->Events[1].Time == 3.0);
	TestEqual(TEXT("built events are counted"), FVaRestTrace::Counters().EventsBuilt.load(), BuiltBefore + 2);

	TSharedRef<FSink, ESPMode::ThreadSafe> B = MakeShared<FSink, ESPMode::ThreadSafe>(EVaRestTraceDetail::Headers | EVaRestTraceDetail::Bodies, 1024);
	B->Detail.BinaryCap = 512;
	B->Detail.FrameCap = 256;
	FVaRestTrace::AddSink(B);
	TestTrue(TEXT("mask is the union of every sink's flags"), FVaRestTrace::Detail() == (EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Headers | EVaRestTraceDetail::Bodies));
	TestEqual(TEXT("body cap is the largest asked for"), FVaRestTrace::GetDetailSettings().BodyCap, 2048);
	TestEqual(TEXT("binary cap is the largest asked for"), FVaRestTrace::GetDetailSettings().BinaryCap, 512);
	TestEqual(TEXT("frame cap is the largest asked for"), FVaRestTrace::GetDetailSettings().FrameCap, 256);

	FVaRestTrace::RemoveSink(A);
	TestTrue(TEXT("removing a sink recomputes the mask"), FVaRestTrace::Detail() == (EVaRestTraceDetail::Headers | EVaRestTraceDetail::Bodies));
	TestEqual(TEXT("... and the body cap"), FVaRestTrace::GetDetailSettings().BodyCap, 1024);
	TestEqual(TEXT("... the binary cap"), FVaRestTrace::GetDetailSettings().BinaryCap, 512);
	TestEqual(TEXT("... the frame cap"), FVaRestTrace::GetDetailSettings().FrameCap, 256);
	FVaRestTrace::Emit(EVaRestTraceKind::Started, 4.0, Started(44));
	TestEqual(TEXT("a removed sink gets nothing more"), A->Events.Num(), 2);
	TestEqual(TEXT("the other one does"), B->Events.Num(), 1);

	B->Detail.Flags = EVaRestTraceDetail::Metadata;
	FVaRestTrace::RefreshDetail();
	TestTrue(TEXT("RefreshDetail re-reads GetDetail"), FVaRestTrace::Detail() == EVaRestTraceDetail::Metadata);

	FVaRestTrace::RemoveSink(B);
	TestTrue(TEXT("no sinks, mask 0"), FVaRestTrace::Detail() == EVaRestTraceDetail::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceIsolationTest, "VaRest.Http.TraceSurface.Isolation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceIsolationTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TraceSurfaceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Outer; // stands in for whatever the editor had registered
	TSharedRef<FSink, ESPMode::ThreadSafe> EditorSink = MakeShared<FSink, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(EditorSink);
	{
		FVaRestTrace::FScopedSinkIsolation Inner;
		TestTrue(TEXT("inside an isolation the outer sink is not active"), FVaRestTrace::Detail() == EVaRestTraceDetail::None);
		FVaRestTrace::Emit(EVaRestTraceKind::Started, 1.0, Started(1));
		TSharedRef<FSink, ESPMode::ThreadSafe> Local = MakeShared<FSink, ESPMode::ThreadSafe>(EVaRestTraceDetail::Headers);
		FVaRestTrace::AddSink(Local);
		FVaRestTrace::Emit(EVaRestTraceKind::Started, 2.0, Started(2));
		TestEqual(TEXT("a sink added inside sees events"), Local->Events.Num(), 1);
	}
	TestEqual(TEXT("the outer sink saw nothing from inside"), EditorSink->Events.Num(), 0);
	TestTrue(TEXT("leaving restores the outer list and its mask"), FVaRestTrace::Detail() == EVaRestTraceDetail::Metadata);
	FVaRestTrace::Emit(EVaRestTraceKind::Started, 3.0, Started(3));
	TestEqual(TEXT("... and the outer sink receives again"), EditorSink->Events.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceSnapshotTest, "VaRest.Http.TraceSurface.Snapshot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceSnapshotTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TraceSurfaceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const FDelegateHandle Contributor = FVaRestTrace::OnBuildSnapshot().AddLambda([](FVaRestTraceSnapshot& Snapshot)
	{
		FVaRestTraceClient& Client = Snapshot.Clients.AddDefaulted_GetRef();
		Client.ClientId = 77;
		Client.DisplayName = TEXT("https://api.test");
		FVaRestTraceStarted& InFlight = Snapshot.InFlight.AddDefaulted_GetRef();
		InFlight.RequestId = 9;
		InFlight.Flags = EVaRestTraceRequestFlags::Partial;
	});
	TSharedRef<FSink, ESPMode::ThreadSafe> Sink = MakeShared<FSink, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Sink);
	FVaRestTrace::OnBuildSnapshot().Remove(Contributor);
	// Presence, not counts or indexes: other contributors may be registered by then (review M-4)
	TestTrue(TEXT("contributors fill the snapshot (IN-12)"), Sink->LastSnapshot.Clients.ContainsByPredicate(
		[](const FVaRestTraceClient& Client) { return Client.ClientId == 77; }));
	TestTrue(TEXT("in-flight requests arrive as Started{Partial}"), Sink->LastSnapshot.InFlight.ContainsByPredicate(
		[](const FVaRestTraceStarted& Request) { return Request.RequestId == 9 && EnumHasAnyFlags(Request.Flags, EVaRestTraceRequestFlags::Partial); }));
	TestTrue(TEXT("scope 0 stays first"), Sink->LastSnapshot.Scopes.Num() >= 1 && Sink->LastSnapshot.Scopes[0].ScopeId == FVaRestTrace::ProcessScopeId);
	FVaRestTrace::RemoveSink(Sink);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceIdsTest, "VaRest.Http.TraceSurface.Ids",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceIdsTest::RunTest(const FString& Parameters)
{
	TArray<uint64> Ids;
	Ids.SetNumZeroed(1000);
	ParallelFor(Ids.Num(), [&Ids](int32 i) { Ids[i] = FVaRestTrace::NewRequestId(); });
	TSet<uint64> Unique;
	Unique.Append(Ids);
	TestEqual(TEXT("ids from many threads are unique (IN-10: cheap counters)"), Unique.Num(), Ids.Num());
	TestFalse(TEXT("0 is never an id"), Unique.Contains(0));
	TestTrue(TEXT("client ids too"), FVaRestTrace::NewClientId() != FVaRestTrace::NewClientId());

	FVaRestTraceExcerpt Excerpt;
	TestTrue(TEXT("an excerpt without bytes is empty text"), Excerpt.ToText().IsEmpty());
	Excerpt.Bytes = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(TArray<uint8>{ 'h', 'i' });
	TestEqualSensitive(TEXT("excerpt text"), Excerpt.ToText(), FString(TEXT("hi")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceReentrancyTest, "VaRest.Http.TraceSurface.Reentrancy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceReentrancyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TraceSurfaceTestPrivate;
	typedef TSharedRef<FReentrantSink, ESPMode::ThreadSafe> FReentrantRef;
	FVaRestTrace::FScopedSinkIsolation Isolation;

	// GetDetail (from Merge) and OnAttached call GetSinks and GetDetailSettings: they must not run under the write lock
	FReentrantRef A = MakeShared<FReentrantSink, ESPMode::ThreadSafe>();
	FReentrantRef B = MakeShared<FReentrantSink, ESPMode::ThreadSafe>();
	FReentrantRef C = MakeShared<FReentrantSink, ESPMode::ThreadSafe>();
	FVaRestTrace::AddSink(A);
	FVaRestTrace::AddSink(B);
	FVaRestTrace::RefreshDetail();
	TestEqual(TEXT("A attached once"), A->Attached, 1);

	// A removes B and adds C while an event is being delivered
	A->OnEventHook = [B, C]()
	{
		FVaRestTrace::RemoveSink(B);
		FVaRestTrace::AddSink(C);
		FVaRestTrace::OnBuildSnapshot(); // touches the delegate too
	};
	FVaRestTrace::Emit(EVaRestTraceKind::Started, 1.0, Started(1));
	A->OnEventHook = nullptr;
	TestEqual(TEXT("A saw the first event"), A->Events.Num(), 1);
	TestEqual(TEXT("B, removed mid-delivery, still got the event in progress (Emit iterates its snapshot)"), B->Events.Num(), 1);
	TestEqual(TEXT("C, added mid-delivery, did not get the event in progress"), C->Events.Num(), 0);

	FVaRestTrace::Emit(EVaRestTraceKind::Started, 2.0, Started(2));
	TestEqual(TEXT("B gets nothing after"), B->Events.Num(), 1);
	TestEqual(TEXT("C gets the next event"), C->Events.Num(), 1);
	TestEqual(TEXT("A gets it too"), A->Events.Num(), 2);

	// Drop the only handles inside an inner isolation: its destructor releases the list, which runs the sinks' destructors,
	// which call GetSinks. That must happen outside the lock.
	{
		FVaRestTrace::FScopedSinkIsolation Inner;
		{
			FReentrantRef Doomed = MakeShared<FReentrantSink, ESPMode::ThreadSafe>();
			FVaRestTrace::AddSink(Doomed);
		}
	}
	// ... and removing the last handle of a published sink releases it inside RemoveSink/Swap
	{
		FReentrantRef Last = MakeShared<FReentrantSink, ESPMode::ThreadSafe>();
		FVaRestTrace::AddSink(Last);
		FVaRestTrace::RemoveSink(Last);
	}
	FVaRestTrace::RemoveSink(A);
	FVaRestTrace::RemoveSink(C);
	TestTrue(TEXT("no sinks, mask 0"), FVaRestTrace::Detail() == EVaRestTraceDetail::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceReAddTest, "VaRest.Http.TraceSurface.ReAdd",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceReAddTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TraceSurfaceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	TSharedRef<FSink, ESPMode::ThreadSafe> Sink = MakeShared<FSink, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Sink);
	FVaRestTrace::AddSink(Sink);
	TestEqual(TEXT("a sink added twice is attached once (review M-8)"), Sink->Attached, 1);
	FVaRestTrace::Emit(EVaRestTraceKind::Started, 1.0, Started(1));
	TestEqual(TEXT("... and receives each event once"), Sink->Events.Num(), 1);

	// A Pause inside an isolation must survive the restore (review M-3)
	{
		FVaRestTrace::FScopedSinkIsolation Inner;
		FVaRestTrace::RemoveSink(Sink);
	}
	TestTrue(TEXT("a sink removed inside an isolation stays removed after it"), FVaRestTrace::Detail() == EVaRestTraceDetail::None);
	return true;
}

#endif
