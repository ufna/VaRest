// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestCache.h"
#include "VaRestHttpTesting.h"
#include "VaRestInspector.h"
#include "VaRestTrace.h"
#if VAREST_WITH_INSPECTOR
#include "VaRestInspectorTestUtil.h"
#include "VaRestInspectorPacket.h"
#include "VaRestInspectorQueue.h"
#endif

namespace VaRest::Inspector::Private::CostsTesting
{
using VaRest::Http::Testing::FTestRig;
using VaRest::Http::Testing::FTraceRecorder;
using VaRest::Http::Testing::FlushRedactionPipe;
using VaRest::Http::Testing::MakeOutcome;
using VaRest::Http::Testing::Observe;
#if !UE_BUILD_SHIPPING
using VaRest::Http::Testing::Counters;
#endif

struct FTraceImage
{
	uint64 Events = 0, Redactions = 0, Binds = 0, Cycles = 0;
	int64 Copies = 0;
};
FTraceImage TraceImage()
{
	const auto& C = FVaRestTrace::Counters();
	FTraceImage I;
	I.Events = C.EventsBuilt.load(); I.Redactions = C.RedactorCalls.load();
	I.Binds = C.DelegateBinds.load(); I.Cycles = C.TraceCycles.load();
#if !UE_BUILD_SHIPPING
	I.Copies = Counters().TraceExcerptBytesCopied.load();
#endif
	return I;
}
bool SameTrace(const FTraceImage& A, const FTraceImage& B)
{
	return A.Events == B.Events && A.Redactions == B.Redactions && A.Binds == B.Binds
		&& A.Cycles == B.Cycles && A.Copies == B.Copies;
}
bool Drain(FAutomationTestBase& Test, const FTestRig& Rig)
{
	const bool Good = Test.TestTrue(TEXT("Task14 owned fake HTTP work drains"), Rig.Drain());
	FlushRedactionPipe();
	return Good;
}
uint64 SendBody(FAutomationTestBase& Test, FTestRig& Rig, const TCHAR* Path,
	const FString& Body = TEXT("safe-request-body"), const FString& Response = TEXT("safe-response-body"))
{
	auto Answer = FVaRestFakeResponse::Text(200, Response);
	Answer.Headers.Set(TEXT("X-Task14-Reply"), TEXT("visible-reply"));
	Rig.Fake->Enqueue(MoveTemp(Answer));
	const auto Outcome = MakeOutcome<void>();
	const auto Handle = Observe(Rig.Client.Post<void>(Path).Header(TEXT("X-Task14-Request"), TEXT("visible-request"))
		.Body(Body).NoRetry(), Outcome).Send();
	const bool Done = Rig.Wait(Outcome);
	const bool Good = Test.TestTrue(TEXT("Task14 real request completes successfully"),
		Done && Outcome->SuccessCalls.load() == 1 && Outcome->ErrorCalls.load() == 0
		&& Outcome->CompleteCalls.load() == 1 && Outcome->bAllOnGameThread.load());
	return Good ? Handle.GetId() : 0;
}
bool Batch(FAutomationTestBase& Test, FTestRig& Rig, int32 Count, TArray<uint64>* SeenIds = nullptr)
{
	const int32 BeforeCalls = Rig.Fake->NumCalls();
	TSet<uint64> Ids;
	for (int32 N = 0; N < Count; ++N)
	{
		const uint64 Id = SendBody(Test, Rig, TEXT("/task14/costs"));
		if (Id == 0) break;
		Ids.Add(Id);
		if (SeenIds) SeenIds->Add(Id);
	}
	bool Good = Drain(Test, Rig);
	Good = Test.TestEqual(TEXT("Task14 all requested fake calls actually ran"), Rig.Fake->NumCalls() - BeforeCalls, Count) && Good;
	Good = Test.TestEqual(TEXT("Task14 real request ids are nonzero and unique"), Ids.Num(), Count) && Good;
	return Good;
}
bool HasBody(const FTraceRecorder& Recorder, uint64 Id, int32 Cap = MAX_int32)
{
	for (const auto& E : Recorder.ExcerptsOf(Id))
		if (E.Excerpt.SkipReason == EVaRestExcerptSkip::None && E.Excerpt.Bytes.IsValid()
			&& E.Excerpt.Bytes->Num() > 0 && E.Excerpt.Bytes->Num() <= Cap)
			return true;
	return false;
}
bool SourceMetadata(FAutomationTestBase& Test, const FTraceRecorder& Recorder, uint64 Id)
{
	const auto Events = Recorder.For(Id);
	uint64 Dispatch = 0; int32 Starts = 0, Dispatched = 0, Ends = 0, Completed = 0;
	bool Good = Id != 0;
	for (const auto& E : Events)
	{
		switch (E.Kind)
		{
		case EVaRestTraceKind::Started:
			++Starts;
#if !VAREST_WITH_TRACE_EXCERPTS
			Good = !E.Payload.Get<FVaRestTraceStarted>().bExcerptPending && Good;
#endif
			break;
		case EVaRestTraceKind::AttemptDispatched:
			++Dispatched; Dispatch = E.Payload.Get<FVaRestTraceAttemptDispatched>().DispatchId;
			Good = Dispatch != 0 && Good;
			break;
		case EVaRestTraceKind::AttemptEnded:
			++Ends;
			Good = Dispatch != 0 && E.Payload.Get<FVaRestTraceAttemptEnded>().DispatchId == Dispatch && Good;
#if !VAREST_WITH_TRACE_EXCERPTS
			Good = !E.Payload.Get<FVaRestTraceAttemptEnded>().bExcerptPending && Good;
#endif
			break;
		case EVaRestTraceKind::Completed:
		{
			++Completed; const auto& C = E.Payload.Get<FVaRestTraceCompleted>();
			Good = C.RequestId == Id && C.Response.Status == 200 && C.Response.BodyBytes == 18
				&& C.Response.Storage == EVaRestTraceResponseStorage::Memory
				&& !C.Response.ContentType.IsEmpty() && !C.Response.EffectiveUrl.IsEmpty() && Good;
			break;
		}
		default: break;
		}
	}
	return Test.TestTrue(TEXT("Task14 real source metadata preserves Response and DispatchId"),
		Good && Starts == 1 && Dispatched == 1 && Ends == 1 && Completed == 1);
}
uint64 CacheHit(FAutomationTestBase& Test, FTestRig& Rig, FTraceRecorder& Recorder)
{
	auto Policy = FVaRestCachePolicy::Memory(); Policy.DefaultTtlSeconds = 60;
	Rig.Client.WithCache(Policy);
	auto Answer = FVaRestFakeResponse::Text(200, TEXT("cached-body"));
	Answer.Headers.Set(TEXT("Cache-Control"), TEXT("max-age=60"));
	Rig.Fake->Enqueue(MoveTemp(Answer));
	const auto Seed = MakeOutcome<FString>();
	Observe(Rig.Client.Get<FString>(TEXT("/task14/cache")).NoRetry(), Seed).Send();
	if (!Test.TestTrue(TEXT("Task14 real memory cache seed completes"), Rig.Wait(Seed)) || !Drain(Test, Rig)) return 0;
	const int32 BeforeCalls = Rig.Fake->NumCalls();
	const auto Hit = MakeOutcome<FString>();
	const auto Handle = Observe(Rig.Client.Get<FString>(TEXT("/task14/cache")).NoRetry(), Hit).Send();
	if (!Test.TestTrue(TEXT("Task14 real cache hit completes"), Rig.Wait(Hit)) || !Drain(Test, Rig)) return 0;
	bool Good = Hit->SuccessCalls.load() == 1 && Hit->Value.IsSet() && Hit->Value.GetValue() == TEXT("cached-body");
	int32 Completed = 0, Attempts = 0;
	for (const auto& E : Recorder.For(Handle.GetId()))
	{
		if (E.Kind == EVaRestTraceKind::AttemptDispatched) ++Attempts;
		if (E.Kind == EVaRestTraceKind::Completed)
		{
			++Completed; const auto& C = E.Payload.Get<FVaRestTraceCompleted>();
			Good = C.bFromCache && C.CacheLookup == EVaRestTraceCacheLookup::HitMemory
				&& C.Response.Status == 200 && C.Response.BodyBytes == 11 && Good;
		}
	}
	Good = Test.TestTrue(TEXT("Task14 cached metadata has actual delivered provenance without an attempt"),
		Good && Completed == 1 && Attempts == 0 && Rig.Fake->NumCalls() == BeforeCalls) && Good;
	return Good ? Handle.GetId() : 0;
}

bool MergeMatrix(FAutomationTestBase& Test)
{
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const auto A = MakeShared<FTraceRecorder, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies);
	const auto B = MakeShared<FTraceRecorder, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Headers | EVaRestTraceDetail::Bodies);
	A->Detail.BodyCap = 37; A->Detail.BinaryCap = 3; A->Detail.FrameCap = 5;
	B->Detail.BodyCap = 91; B->Detail.BinaryCap = 19; B->Detail.FrameCap = 17;
	A->Detail.MaxPendingExcerptBytes = 0; B->Detail.MaxPendingExcerptBytes = 1024 * 1024;
	FVaRestTrace::AddSink(A); FVaRestTrace::AddSink(B);
	ON_SCOPE_EXIT { FVaRestTrace::RemoveSink(B); FVaRestTrace::RemoveSink(A); };
	auto D = FVaRestTrace::GetDetailSettings();
	bool Good = Test.TestTrue(TEXT("Task14 independent sink flags and numerical caps merge by maximum"),
		D.Flags == (EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Headers | EVaRestTraceDetail::Bodies)
		&& D.BodyCap == 91 && D.BinaryCap == 19 && D.FrameCap == 17);
	Good = Test.TestEqual(TEXT("Task14 Bodies default0 is normalized before maximum"),
		D.MaxPendingExcerptBytes, int64(16 * 1024 * 1024)) && Good;
	A->Detail.MaxPendingExcerptBytes = 1024 * 1024; B->Detail.MaxPendingExcerptBytes = 2 * 1024 * 1024;
	FVaRestTrace::RefreshDetail();
	Good = Test.TestEqual(TEXT("Task14 explicit Bodies pending allowances merge independently"),
		FVaRestTrace::GetDetailSettings().MaxPendingExcerptBytes, int64(2 * 1024 * 1024)) && Good;
	A->Detail.MaxPendingExcerptBytes = 0; B->Detail.MaxPendingExcerptBytes = 32 * 1024 * 1024;
	FVaRestTrace::RefreshDetail();
	Good = Test.TestEqual(TEXT("Task14 explicit larger allowance exceeds default"),
		FVaRestTrace::GetDetailSettings().MaxPendingExcerptBytes, int64(32 * 1024 * 1024)) && Good;
	A->Detail.Flags = EVaRestTraceDetail::Metadata; A->Detail.BodyCap = 123;
	B->Detail.MaxPendingExcerptBytes = 1024 * 1024;
	FVaRestTrace::RefreshDetail(); D = FVaRestTrace::GetDetailSettings();
	Good = Test.TestTrue(TEXT("Task14 metadata default does not raise Bodies pending allowance but caps still merge"),
		D.MaxPendingExcerptBytes == 1024 * 1024 && D.BodyCap == 123) && Good;
	Good = Test.TestTrue(TEXT("Task14 merge does not mutate either consumer request"),
		A->Detail.MaxPendingExcerptBytes == 0 && B->Detail.BodyCap == 91 && B->Detail.FrameCap == 17) && Good;
	FVaRestTrace::RemoveSink(B); D = FVaRestTrace::GetDetailSettings();
	Good = Test.TestTrue(TEXT("Task14 removing body consumer removes its flags and pending demand"),
		D.Flags == EVaRestTraceDetail::Metadata && D.BodyCap == 123 && D.MaxPendingExcerptBytes == 0) && Good;
	return Good;
}

#if VAREST_WITH_INSPECTOR
using CaptureTesting::FCaptureFixture;

FCapturePolicy Policy()
{
	FCapturePolicy P;
	P.Limits.StoreBytes = 16ll * 1024 * 1024; P.Limits.MaxRecords = 512;
	P.Limits.PendingBytes = 4ll * 1024 * 1024; P.Limits.PendingEvents = 4096;
	P.Limits.BodyBytes = 64; P.Limits.BinaryBytes = 0;
	return P;
}
FAllocationCounterImage ProbeImage(const FAllocationCounters& P)
{
	FAllocationCounterImage I;
	I.LiveBacking = P.LiveBacking.load(); I.PeakBacking = P.PeakBacking.load();
	I.TextBytesCopied = P.TextBytesCopied.load(); I.ExcerptRefsAcquired = P.ExcerptRefsAcquired.load();
	I.LiveControllers = P.LiveControllers.load(); I.DestroyedControllers = P.DestroyedControllers.load();
	I.UnsupportedAllocator = P.UnsupportedAllocator.load(); I.BoundFailures = P.BoundFailures.load();
	return I;
}
FAllocationCounterImage ArmProbe(FAllocationCounters& P)
{
	// Only this quiescent fixture's observation highwater; never a ledger peak.
	P.PeakBacking.store(P.LiveBacking.load());
	return ProbeImage(P);
}
bool NoAcquisition(const FAllocationCounterImage& A, const FAllocationCounterImage& B)
{
	return B.LiveBacking == A.LiveBacking && B.PeakBacking == A.LiveBacking
		&& B.TextBytesCopied == A.TextBytesCopied && B.ExcerptRefsAcquired == A.ExcerptRefsAcquired
		&& B.LiveControllers == A.LiveControllers && B.DestroyedControllers == A.DestroyedControllers
		&& B.UnsupportedAllocator == A.UnsupportedAllocator && B.BoundFailures == A.BoundFailures;
}
bool OffWindow(FAutomationTestBase& Test, FCaptureFixture& F, int32 Count, const TCHAR* Label, TArray<uint64>* SeenIds = nullptr)
{
	if (!F.Settle()) return false;
	auto Root = F.Service->RootForTest().Pin();
	if (!Test.TestTrue(TEXT("Task14 cost probe belongs to actual production root"), Root.IsValid())) return false;
	auto& D = *Root->Domain.Get();
	const auto I = F.Service->TestImage();
	if (!Test.TestTrue(TEXT("Task14 cost baseline has no outstanding packet, entry or snapshot"),
		D.Pending.Count.load() == 0 && I.Entries == 0 && I.SnapshotControllers == 0)) return false;
	const auto ContextBefore = ArmProbe(D.ContextProbe), StoreBefore = ArmProbe(D.StoreProbe);
	const auto TraceBefore = TraceImage();
	const bool Requests = Batch(Test, F.Rig, Count, SeenIds);
	const auto TraceAfter = TraceImage();
	const auto ContextAfter = ProbeImage(D.ContextProbe), StoreAfter = ProbeImage(D.StoreProbe);
	return Test.TestTrue(Label, SameTrace(TraceBefore, TraceAfter)
		&& NoAcquisition(ContextBefore, ContextAfter) && NoAcquisition(StoreBefore, StoreAfter)) && Requests;
}
bool OffCosts(FAutomationTestBase& Test)
{
	const auto P = Policy(); FCaptureFixture F(Test, false, false, false, &P);
	if (!F.Ready()) return false;
	F.Service->SetCapture(false); F.Settle();
	auto Empty = FVaRestTrace::GetSinks();
	// Deliberately nonfatal: a registration mutant must still run all1000 requests.
	bool Good = Test.TestTrue(TEXT("Task14 disabled service registers no sink"),
		!F.Service->IsCapturing() && (!Empty || Empty->IsEmpty()));
	Empty.Reset(); // Do not keep an unexpected mutant sink alive into later controls.
	Good = OffWindow(Test, F, 1000, TEXT("Task14 disabled requests add no trace or Inspector acquisition work")) && Good;
	F.Service->Pause(); F.Settle();
	const auto Deaf = MakeShared<FTraceRecorder, ESPMode::ThreadSafe>(EVaRestTraceDetail::None);
	FVaRestTrace::AddSink(Deaf);
	TArray<uint64> DeafIds;
	Good = OffWindow(Test, F, 8, TEXT("Task14 None-detail mask adds no trace or Inspector work"), &DeafIds) && Good;
	bool NoDeafEvents = Deaf->GetSnapshots().Num() == 1 && Deaf->For(0).IsEmpty();
	for (uint64 Id : DeafIds) NoDeafEvents = Deaf->For(Id).IsEmpty() && NoDeafEvents;
	Good = Test.TestTrue(TEXT("Task14 deaf recorder receives no request event"), NoDeafEvents) && Good;
	FVaRestTrace::RemoveSink(Deaf);
	F.Service->Clear(); F.Settle();
	if (!Test.TestTrue(TEXT("Task14 on-control starts the real capture service"), F.Service->Start(P)) || !F.Settle()) return false;
	auto Root = F.Service->RootForTest().Pin();
	if (!Test.TestTrue(TEXT("Task14 on-control retains actual probe root"), Root.IsValid())) return false;
	auto& D = *Root->Domain.Get();
	const auto TraceBefore = TraceImage();
	const auto ContextBefore = ArmProbe(D.ContextProbe), StoreBefore = ArmProbe(D.StoreProbe);
	const uint64 OnId = SendBody(Test, F.Rig, TEXT("/task14/on"));
	if (OnId == 0 || !F.Settle()) return false;
	const auto TraceAfter = TraceImage();
	Good = Test.TestTrue(TEXT("Task14 on-control really builds metadata, redacts and binds the two trace delegates"),
		TraceAfter.Events > TraceBefore.Events && TraceAfter.Redactions > TraceBefore.Redactions
		&& TraceAfter.Binds == TraceBefore.Binds + 2) && Good;
#if !UE_BUILD_SHIPPING
	Good = Test.TestTrue(TEXT("Task14 Development on-control accounts trace-point cycles"),
		TraceAfter.Cycles > TraceBefore.Cycles) && Good;
#endif
	Good = Test.TestTrue(TEXT("Task14 on-control really copies Inspector metadata and acquires backing"),
		D.ContextProbe.TextBytesCopied.load() > ContextBefore.TextBytesCopied
		&& (D.ContextProbe.PeakBacking.load() > ContextBefore.LiveBacking || D.StoreProbe.PeakBacking.load() > StoreBefore.LiveBacking)) && Good;
#if VAREST_WITH_TRACE_EXCERPTS
	Good = Test.TestTrue(TEXT("Task14 on-control really copies and acquires a body excerpt"),
		D.ContextProbe.ExcerptRefsAcquired.load() > ContextBefore.ExcerptRefsAcquired) && Good;
#if !UE_BUILD_SHIPPING
	Good = Test.TestTrue(TEXT("Task14 on-control producer body copies are observed"), TraceAfter.Copies > TraceBefore.Copies) && Good;
#endif
#endif
	Good = F.Row(OnId, [&](const FVaRestInspectorRecordView& R) {
		Test.TestTrue(TEXT("Task14 on-control is a real completed row"), R.bActualCompletionObserved && R.FinalStatus == 200);
	}) && Good;
	F.Service->Pause(); F.Settle();
	Root.Reset(); // No copied old-generation owner crosses the next fixed baseline.
	const auto Paused = FVaRestTrace::GetSinks();
	Good = Test.TestTrue(TEXT("Task14 real Pause removes its sink"), !F.Service->IsCapturing() && (!Paused || Paused->IsEmpty())) && Good;
	Good = OffWindow(Test, F, 16, TEXT("Task14 paused requests add no trace or Inspector acquisition work")) && Good;
	Good = F.Row(OnId, [&](const FVaRestInspectorRecordView& R) {
		Test.TestTrue(TEXT("Task14 Pause preserves the captured completed row"), R.bActualCompletionObserved && R.FinalStatus == 200);
	}) && Good;
	return Good;
}
bool MaskIsolation(FAutomationTestBase& Test)
{
	auto P = Policy(); P.bHeaders = false; P.bBodies = false;
	FCaptureFixture F(Test, false, true, false, &P);
	if (!F.Ready() || !F.Service->IsCapturing()) return false;
	const auto Recorder = MakeShared<FTraceRecorder, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Headers | EVaRestTraceDetail::Bodies);
	Recorder->Detail.BodyCap = 128; Recorder->Detail.BinaryCap = 0;
	FVaRestTrace::AddSink(Recorder);
	ON_SCOPE_EXIT { FlushRedactionPipe(); FVaRestTrace::RemoveSink(Recorder); };
	if (!F.Settle()) return false;
	auto Root = F.Service->RootForTest().Pin();
	if (!Test.TestTrue(TEXT("Task14 metadata policy uses an actual root"), Root.IsValid())) return false;
	auto& D = *Root->Domain.Get();
	const int64 BeforeRefs = D.ContextProbe.ExcerptRefsAcquired.load();
	const uint64 Id = SendBody(Test, F.Rig, TEXT("/task14/metadata"));
	if (Id == 0 || !F.Settle()) return false;
	bool NoOwners = true;
	bool Good = F.Row(Id, [&](const FVaRestInspectorRecordView& R) {
		auto Check = [&](const FVaRestInspectorPacketView& V) {
			if (V.Fixed) NoOwners = V.Fixed->Headers.Count == 0 && !V.Fixed->Scalars.bExcerptPending && V.Excerpt.IsEmpty() && NoOwners;
		};
		Check(R.Started); Check(R.Completed);
		for (const auto& Dispatch : R.Dispatches) { Check(Dispatch.Dispatched); Check(Dispatch.Ended); Check(Dispatch.ResponseExcerpt); }
		for (const auto& Excerpt : R.Excerpts) Check(Excerpt);
		NoOwners = R.Excerpts.IsEmpty() && R.RequestExcerptState == EVaRestInspectorDetailState::Absent && NoOwners;
	});
	Good = Test.TestTrue(TEXT("Task14 metadata Inspector acquires no excerpt reference"),
		NoOwners && D.ContextProbe.ExcerptRefsAcquired.load() == BeforeRefs) && Good;
	Good = SourceMetadata(Test, *Recorder, Id) && Good;
	bool HasHeaders = false;
	for (const auto& E : Recorder->For(Id))
		if (E.Kind == EVaRestTraceKind::Started) HasHeaders = !E.Payload.Get<FVaRestTraceStarted>().Headers.IsEmpty();
	Good = Test.TestTrue(TEXT("Task14 independent recorder requests real headers"), HasHeaders) && Good;
#if VAREST_WITH_TRACE_EXCERPTS
	Good = Test.TestTrue(TEXT("Task14 independent recorder receives an actual nonempty body"), HasBody(*Recorder, Id)) && Good;
#else
	Good = Test.TestTrue(TEXT("Task14 excerpt-disabled source emits no body"), Recorder->ExcerptsOf(Id).IsEmpty()) && Good;
#endif
	// A Bodies-enabled consumer shares the merged immutable owner; no local numeric clamp is invented.
	F.Service->Pause(); P.bHeaders = true; P.bBodies = true; P.Limits.BodyBytes = 32;
	if (!Test.TestTrue(TEXT("Task14 reverse policy starts the real service"), F.Service->Start(P))) return false;
	Recorder->Detail.Flags = EVaRestTraceDetail::Metadata;
	FVaRestTrace::RefreshDetail();
	const uint64 ReverseId = SendBody(Test, F.Rig, TEXT("/task14/reverse"), FString::ChrN(96, TEXT('r')));
	if (ReverseId == 0 || !F.Settle()) return false;
	const auto Merged = FVaRestTrace::GetDetailSettings();
	Good = Test.TestEqual(TEXT("Task14 metadata recorder cap participates in producer maximum"), Merged.BodyCap, 128) && Good;
	Good = F.Row(ReverseId, [&](const FVaRestInspectorRecordView& R) {
		Test.TestTrue(TEXT("Task14 reversed Inspector actually owns requested headers"), R.Started.Fixed && R.Started.Fixed->Headers.Count > 0);
#if VAREST_WITH_TRACE_EXCERPTS
		bool SharedMerged = false;
		for (const auto& E : R.Excerpts)
			if (E.Fixed && E.Fixed->Scalars.Owner == EVaRestTraceExcerptOwner::Request)
				SharedMerged = E.Excerpt.Num() > P.Limits.BodyBytes && E.Excerpt.Num() <= Merged.BodyCap;
		Test.TestTrue(TEXT("Task14 Bodies Inspector retains admitted shared merged excerpt without a second clip"), SharedMerged);
#endif
	}) && Good;
	FVaRestTrace::RemoveSink(Recorder);
	const uint64 InspectorOnly = SendBody(Test, F.Rig, TEXT("/task14/inspector-only"));
	if (InspectorOnly == 0 || !F.Settle()) return false;
	Good = F.Row(InspectorOnly, [&](const FVaRestInspectorRecordView& R) {
		Test.TestTrue(TEXT("Task14 removing recorder preserves Inspector completion"), R.bActualCompletionObserved);
	}) && Good;
	FVaRestTrace::AddSink(Recorder);
	F.Service->Pause(); F.Settle();
	const uint64 RecorderOnly = SendBody(Test, F.Rig, TEXT("/task14/recorder-only"));
	if (RecorderOnly == 0 || !Drain(Test, F.Rig)) return false;
	Good = SourceMetadata(Test, *Recorder, RecorderOnly) && Good;
	const auto Left = FVaRestTrace::GetSinks();
	Good = Test.TestTrue(TEXT("Task14 removing Inspector leaves independent recorder"), Left && Left->Num() == 1 && Left->Contains(Recorder)) && Good;
	Root.Reset();
	return Good;
}
bool SavedListRemoval(FAutomationTestBase& Test)
{
	const auto P = Policy(); FCaptureFixture F(Test, false, true, false, &P);
	if (!F.Ready() || !F.Service->IsCapturing()) return false;
	const auto Recorder = MakeShared<FTraceRecorder, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Recorder);
	ON_SCOPE_EXIT { FlushRedactionPipe(); FVaRestTrace::RemoveSink(Recorder); };
	{
		// Deliberately save the actual two-consumer list, then remove only Inspector.
		FVaRestTrace::FScopedSinkIsolation Inner;
		F.Service->Pause();
	}
	const auto Restored = FVaRestTrace::GetSinks();
	bool Good = Test.TestTrue(TEXT("Task14 saved-list restoration does not resurrect paused Inspector"),
		!F.Service->IsCapturing() && Restored && Restored->Num() == 1 && Restored->Contains(Recorder));
	const uint64 Id = SendBody(Test, F.Rig, TEXT("/task14/saved-list"));
	if (Id == 0 || !Drain(Test, F.Rig)) return false;
	return SourceMetadata(Test, *Recorder, Id) && Good;
}
bool LocalPending(FAutomationTestBase& Test)
{
	auto P = Policy(); P.Limits.PendingEvents = 1;
	FCaptureFixture F(Test, false, true, false, &P);
	if (!F.Ready() || !F.Service->IsCapturing() || !F.Settle()) return false;
	const auto Recorder = MakeShared<FTraceRecorder, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies);
	Recorder->Detail.BodyCap = 64; Recorder->Detail.MaxPendingExcerptBytes = 16 * 1024 * 1024;
	FVaRestTrace::AddSink(Recorder);
	ON_SCOPE_EXIT { FlushRedactionPipe(); FVaRestTrace::RemoveSink(Recorder); };
	// These real owners are declared after F and die before its physical lifetime audit.
	auto Root = F.Service->RootForTest().Pin(); auto Queue = F.Service->QueueForTest();
	if (!Test.TestTrue(TEXT("Task14 pressure retains real root and generation queue"), Root.IsValid() && Queue.IsValid())) return false;
	auto& D = *Root->Domain.Get();
	const uint64 First = SendBody(Test, F.Rig, TEXT("/task14/held"));
	if (First == 0) return false;
	auto Held = Queue->TryPopMetadata();
	if (!Test.TestTrue(TEXT("Task14 pressure holds an actual HTTP packet and its pending ticket"),
		Held.IsSet() && Held->Fixed().Scalars.RequestId == First && D.Pending.Count.load() == 1)) return false;
	const uint64 BeforeDropped = D.Pending.DroppedCount.load();
	const uint64 Second = SendBody(Test, F.Rig, TEXT("/task14/pressure"));
	if (Second == 0 || !Drain(Test, F.Rig)) return false;
	bool Good = Test.TestTrue(TEXT("Task14 local pending count refuses without changing another consumer"),
		D.Identity.Effective.PendingEvents == 1 && D.Pending.Count.load() == 1
		&& D.Pending.DroppedCount.load() > BeforeDropped
		&& D.Pending.Bytes.load() <= D.Identity.Effective.PendingBytes);
	Good = SourceMetadata(Test, *Recorder, Second) && Good;
#if VAREST_WITH_TRACE_EXCERPTS
	Good = Test.TestTrue(TEXT("Task14 local refusal still serves the recorder body"), HasBody(*Recorder, Second)) && Good;
	for (const auto& E : Recorder->ExcerptsOf(Second))
		Good = Test.TestTrue(TEXT("Task14 local count pressure did not consume source raw-copy budget"),
			E.Excerpt.SkipReason != EVaRestExcerptSkip::Backlog) && Good;
#endif
	Held.Reset();
	Good = Test.TestEqual(TEXT("Task14 resetting held actual packet returns its count ticket"), D.Pending.Count.load(), int64(0)) && Good;
	if (!F.Settle()) return false;
	const uint64 Third = SendBody(Test, F.Rig, TEXT("/task14/readmitted"));
	if (Third == 0) return false;
	auto Readmitted = Queue->TryPopMetadata();
	Good = Test.TestTrue(TEXT("Task14 local admission resumes after real ticket release"),
		Readmitted.IsSet() && Readmitted->Fixed().Scalars.RequestId == Third) && Good;
	Readmitted.Reset();
	if (!F.Settle()) return false;
	Queue.Reset(); Root.Reset();
	return Good;
}
bool NumericDefaults(FAutomationTestBase& Test)
{
	auto P = Policy(); P.Limits.BodyBytes = 32; P.Limits.BinaryBytes = 0;
	FCaptureFixture F(Test, false, true, true, &P);
	if (!F.Ready() || !F.Service->IsCapturing()) return false;
	// Observe broadcasts without masking the Inspector's own numerical demand.
	F.Recorder->Detail.Flags = EVaRestTraceDetail::None;
	F.Recorder->Detail.BodyCap = 0; F.Recorder->Detail.BinaryCap = 0; F.Recorder->Detail.FrameCap = 0;
	FVaRestTrace::RefreshDetail();
	const auto D = FVaRestTrace::GetDetailSettings();
	bool Good = Test.TestTrue(TEXT("Task14 sole Inspector advertises actual body, binary0 and no socket caps"),
		D.BodyCap == 32 && D.BinaryCap == 0 && D.FrameCap == 0);
	const uint64 TextId = SendBody(Test, F.Rig, TEXT("/task14/sole-cap"), FString::ChrN(128, TEXT('x')));
	if (TextId == 0 || !Drain(Test, F.Rig)) return false;
#if VAREST_WITH_TRACE_EXCERPTS
	{
		// Borrow a REAL admitted packet after all producers finish, not a synthetic ticket.
		auto ProofRoot = F.Service->RootForTest().Pin();
		auto ProofQueue = F.Service->QueueForTest();
		if (!Test.TestTrue(TEXT("Task14 shared capacity proof retains its actual generation"), ProofRoot.IsValid() && ProofQueue.IsValid())) return false;
		auto Packet = ProofQueue->TryPopExcerpt();
		if (!Test.TestTrue(TEXT("Task14 shared capacity proof uses a real excerpt packet"),
			Packet.IsSet() && Packet->Excerpt().IsValid() && !Packet->Fixed().Header.bSnapshot)) return false;
		const int64 FullCapacity = Packet->RetainedCapacity();
		const int64 NodeCharge = int64(ProofRoot->Lifetime.Get()->Allocator.QueueNodeBound());
		const int64 PendingBefore = ProofRoot->Domain.Get()->Pending.Bytes.load();
		bool Shared = false;
		for (const auto& E : F.Recorder->ExcerptsOf(TextId))
			Shared = (E.Excerpt.Bytes.Get() == Packet->Excerpt().Get()) || Shared;
		Good = Test.TestTrue(TEXT("Task14 Inspector packet shares the actual immutable source owner"), Shared) && Good;
		Good = Test.TestTrue(TEXT("Task14 retained capacity includes complete allocated excerpt owner"),
			FullCapacity >= int64(Packet->Excerpt()->GetAllocatedSize()) && Packet->Excerpt()->GetAllocatedSize() >= SIZE_T(Packet->Excerpt()->Num())) && Good;
		Packet.Reset();
		Good = Test.TestEqual(TEXT("Task14 pending admission charged full retained capacity plus real queue node"),
			PendingBefore - ProofRoot->Domain.Get()->Pending.Bytes.load(), FullCapacity + NodeCharge) && Good;
	}
	bool Cut = false;
	for (const auto& E : F.Recorder->ExcerptsOf(TextId))
		if (E.Owner == EVaRestTraceExcerptOwner::Request)
			Cut = E.Excerpt.Bytes.IsValid() && E.Excerpt.Bytes->Num() > 0 && E.Excerpt.Bytes->Num() <= 32 && E.Excerpt.bTruncated;
	Good = Test.TestTrue(TEXT("Task14 sole Inspector numeric cap bounds actual producer prefix"), Cut) && Good;
#endif
	if (!F.Settle()) return false;
	TArray<uint8> Binary; Binary.Add(0); Binary.Add(0xff); Binary.Add(0x42);
	F.Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(200, MoveTemp(Binary)));
	const auto Outcome = MakeOutcome<void>();
	const auto H = F.Track(Observe(F.Rig.Client.Get<void>(TEXT("/task14/binary")).NoRetry(), Outcome).Send());
	if (!Test.TestTrue(TEXT("Task14 binary0 request completes"), F.Rig.Wait(Outcome)) || !F.Settle()) return false;
#if VAREST_WITH_TRACE_EXCERPTS
	bool Descriptor = false;
	for (const auto& E : F.Recorder->ExcerptsOf(H.GetId()))
		if (E.Owner == EVaRestTraceExcerptOwner::Response)
			Descriptor = E.Excerpt.Kind == EVaRestExcerptKind::Binary && E.Excerpt.TotalBytes == 3 && !E.Excerpt.Sha256Prefix.IsEmpty();
	Good = Test.TestTrue(TEXT("Task14 binary0 still emits actual binary descriptor"), Descriptor) && Good;
#else
	Good = Test.TestTrue(TEXT("Task14 excerpt0 keeps binary metadata without raw owner"), F.Recorder->ExcerptsOf(H.GetId()).IsEmpty()) && Good;
#endif
	return Good;
}
bool MetadataMode(FAutomationTestBase& Test)
{
	const auto P = Policy(); FCaptureFixture F(Test, false, true, true, &P);
	if (!F.Ready() || !F.Service->IsCapturing() || !F.Settle()) return false;
	auto Root = F.Service->RootForTest().Pin();
	if (!Test.TestTrue(TEXT("Task14 macro metadata uses actual production root"), Root.IsValid())) return false;
#if !VAREST_WITH_TRACE_EXCERPTS
	auto& D = *Root->Domain.Get();
	const auto Before = TraceImage(); const int64 BeforeRefs = D.ContextProbe.ExcerptRefsAcquired.load();
#endif
	const uint64 Network = SendBody(Test, F.Rig, TEXT("/task14/metadata-mode"));
	if (Network == 0 || !F.Settle()) return false;
	bool Good = SourceMetadata(Test, *F.Recorder, Network);
	Good = F.Row(Network, [&](const FVaRestInspectorRecordView& R) {
		Test.TestTrue(TEXT("Task14 Inspector retains real network response and physical dispatch identity"),
			R.FinalStatus == 200 && R.FinalBodyBytes == 18 && R.Dispatches.Num() == 1
			&& R.Dispatches[0].DispatchId != 0 && R.Completed.Fixed && R.Completed.Fixed->Scalars.Status == 200);
#if !VAREST_WITH_TRACE_EXCERPTS
		bool Empty = R.Excerpts.IsEmpty() && R.RequestExcerptState == EVaRestInspectorDetailState::Absent;
		if (R.Started.Fixed) Empty = !R.Started.Fixed->Scalars.bExcerptPending && Empty;
		for (const auto& A : R.Dispatches)
			Empty = A.ResponseExcerpt.Excerpt.IsEmpty() && (!A.Ended.Fixed || !A.Ended.Fixed->Scalars.bExcerptPending) && Empty;
		Test.TestTrue(TEXT("Task14 excerpt0 Inspector has no pending or raw body owner"), Empty);
#endif
	}) && Good;
	const uint64 Cached = CacheHit(Test, F.Rig, *F.Recorder);
	if (Cached == 0 || !F.Settle()) return false;
	Good = F.Row(Cached, [&](const FVaRestInspectorRecordView& R) {
		Test.TestTrue(TEXT("Task14 real cache has NoProducer with delivered response and no wire attempt"),
			R.FinalPreviewState == EVaRestInspectorDetailState::NoProducer && R.FinalStatus == 200
			&& R.FinalBodyBytes == 11 && R.Dispatches.IsEmpty() && R.Completed.Fixed && R.Completed.Fixed->Scalars.bFromCache);
	}) && Good;
#if !VAREST_WITH_TRACE_EXCERPTS
	Good = Test.TestTrue(TEXT("Task14 excerpt0 source and Inspector copy no body"),
		TraceImage().Copies == Before.Copies && D.ContextProbe.ExcerptRefsAcquired.load() == BeforeRefs
		&& F.Recorder->ExcerptsOf(Network).IsEmpty() && F.Recorder->ExcerptsOf(Cached).IsEmpty()) && Good;
#endif
	Root.Reset();
	return Good;
}
#else
bool OffCosts(FAutomationTestBase& Test)
{
	FVaRestTrace::FScopedSinkIsolation Isolation;
	FTestRig Rig;
	auto& Inspector = IVaRestInspector::Get();
	Inspector.SetCapture(true); Inspector.Clear(); Inspector.SetCapture(false);
	if (!Drain(Test, Rig)) return false;
	const auto Before = TraceImage();
	bool Good = Batch(Test, Rig, 1000);
	const auto Sinks = FVaRestTrace::GetSinks();
	Good = Test.TestTrue(TEXT("Task14 compiled-out facade performs no capture or source work"),
		!Inspector.IsCapturing() && Inspector.GetRevision() == 0 && (!Sinks || Sinks->IsEmpty()) && SameTrace(Before, TraceImage())) && Good;
	const auto Recorder = MakeShared<FTraceRecorder, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Recorder);
	ON_SCOPE_EXIT { FlushRedactionPipe(); FVaRestTrace::RemoveSink(Recorder); };
	const uint64 Id = SendBody(Test, Rig, TEXT("/task14/compiled-out-control"));
	if (Id == 0 || !Drain(Test, Rig)) return false;
	return SourceMetadata(Test, *Recorder, Id) && Good;
}
bool MetadataMode(FAutomationTestBase& Test)
{
	FVaRestTrace::FScopedSinkIsolation Isolation;
	FTestRig Rig;
	const auto Recorder = MakeShared<FTraceRecorder, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Headers | EVaRestTraceDetail::Bodies);
	Recorder->Detail.BodyCap = 64;
	FVaRestTrace::AddSink(Recorder);
	ON_SCOPE_EXIT { FlushRedactionPipe(); FVaRestTrace::RemoveSink(Recorder); };
	auto& Inspector = IVaRestInspector::Get();
	Inspector.SetCapture(true); Inspector.Clear(); Inspector.SetCapture(false);
#if !VAREST_WITH_TRACE_EXCERPTS
	const auto Before = TraceImage();
#endif
	const uint64 Network = SendBody(Test, Rig, TEXT("/task14/compiled-out-metadata"));
	if (Network == 0 || !Drain(Test, Rig)) return false;
	bool Good = SourceMetadata(Test, *Recorder, Network);
	const uint64 Cached = CacheHit(Test, Rig, *Recorder);
	if (Cached == 0) return false;
	TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe> Snapshot;
	Good = Test.TestTrue(TEXT("Task14 compiled-out facade returns a valid empty snapshot"), Inspector.TrySnapshot(Snapshot) && Snapshot.IsValid()) && Good;
	int32 Records = 0;
	if (Snapshot) Snapshot->ForEachRecord([&Records](const FVaRestInspectorRecordView&) { ++Records; });
	const auto Sinks = FVaRestTrace::GetSinks();
	Good = Test.TestTrue(TEXT("Task14 compiled-out controls preserve the independent recorder"),
		Records == 0 && !Inspector.IsCapturing() && Inspector.GetRevision() == 0
		&& Sinks && Sinks->Num() == 1 && Sinks->Contains(Recorder)) && Good;
#if !VAREST_WITH_TRACE_EXCERPTS
	Good = Test.TestTrue(TEXT("Task14 excerpt0 public metadata route has no copied raw body"),
		Before.Copies == TraceImage().Copies && Recorder->ExcerptsOf(Network).IsEmpty() && Recorder->ExcerptsOf(Cached).IsEmpty()) && Good;
#else
	Good = Test.TestTrue(TEXT("Task14 compiled-out Inspector still permits independent recorder bodies"), HasBody(*Recorder, Network)) && Good;
#endif
	return Good;
}
#endif
} // namespace VaRest::Inspector::Private::CostsTesting

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorOffCostsTest, "VaRest.Inspector.Capture.OffCosts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorOffCostsTest::RunTest(const FString& Parameters)
{
	return VaRest::Inspector::Private::CostsTesting::OffCosts(*this);
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorDetailIsolationTest, "VaRest.Inspector.Capture.DetailIsolation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorDetailIsolationTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private::CostsTesting;
	bool Good = MergeMatrix(*this);
#if VAREST_WITH_INSPECTOR
	Good = MaskIsolation(*this) && Good;
	Good = SavedListRemoval(*this) && Good;
	Good = LocalPending(*this) && Good;
	Good = NumericDefaults(*this) && Good;
#endif
	return MetadataMode(*this) && Good;
}
#endif
