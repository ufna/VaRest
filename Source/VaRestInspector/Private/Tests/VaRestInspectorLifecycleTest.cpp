// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspector.h"

#if WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR

#include "Async/Async.h"
#include "CoreGlobals.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Templates/UniquePtr.h"
#include "VaRestInspectorPacket.h"
#include "VaRestInspectorService.h"
#include "VaRestInspectorSink.h"
#include "VaRestTrace.h"

namespace VaRest::Inspector::Private
{
// This friend only observes real state and invokes the actual private thunks.
// It does not change a ledger, a capability, an ordinal or a row.
struct FServiceTestAccess
{
	static TSharedRef<FPendingAccount, ESPMode::ThreadSafe> Pending(FService& S) { return S.Pending.ToSharedRef(); }
	static FAllocationAdapter& Allocator(FService& S) { return S.Lifetime->Allocator; }
	static FBothLedgerImage Image(const FService& S) { return S.Domain->Export(); }
 static void TeardownHooks(FService& S,FAccountTeardownTestHooks* H) { S.Domain->TeardownTestHooks.store(H,std::memory_order_release); }
	static FVaRestInspectorDiagnostics Diagnostics(const FService& S) { return S.Store.Diagnostics(); }
	static FOverflowRecovery Recovery(const FService& S) { return S.Recovery; }
	static void ArmObservedLoss(FService& S)
	{
		S.Recovery.Observe(S.Pending->LossEpoch.load(), S.Recovery.HandledOrphan, S.ActiveQueue->LastPublishedMetadata());
	}
	static bool FrameBound(const FService& S) { return S.CallbackState->EndFrame.IsValid(); }
	static bool ExitBound(const FService& S) { return S.CallbackState->PreExit.IsValid(); }
	static bool CallbacksClosed(const FService& S) { return S.CallbackState->Closed; }
	static uint64 LastFrame(const FService& S) { return S.CallbackState->LastFrame; }
	static int32 FrameDepth(const FService& S) { return S.CallbackState->FrameDepth; }
	static FCallbackArgument Argument(FService& S) { return {S.Self, S.CallbackState}; }
	static void EndFrame(FCallbackArgument A) { FService::EndFrame(MoveTemp(A)); }
	static void PreExit(FCallbackArgument A) { FService::PreExit(MoveTemp(A)); }
};

namespace LifecycleTesting
{
using FServicePtr = TSharedPtr<FService, ESPMode::ThreadSafe>;
using FSnapshotPtr = TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>;

struct FSourceState
{
	TArray<FVaRestTraceStarted> Live;
	uint64 SnapshotWalks = 0;
	TOptional<FVaRestTraceEvent> DuringNextWalk;
	FVaRestTraceStarted Make(uint64 Id) const
	{
		FVaRestTraceStarted S;
		S.RequestId = Id; S.ClientId = 88001; S.Verb = TEXT("GET");
		S.Url = TEXT("https://capture.invalid/lifecycle"); S.PathTemplate = TEXT("/lifecycle");
		S.StartedAt = 10; S.StartUtc = FDateTime(2026, 10, 25);
		return S;
	}
	void Add(uint64 Id) { Live.Add(Make(Id)); }
	void Remove(uint64 Id) { Live.RemoveAll([Id](const auto& S) { return S.RequestId == Id; }); }
};

struct FRowImage
{
	bool Found = false, Pinned = false, Partial = false, Terminal = false, Sealed = false, Actual = false;
	uint64 Birth = 0, Arrival = 0, Revision = 0, StartedSeq = 0;
	int32 Timeline = 0, Status = 0;
	double CompletedAt = -1;
	EVaRestInspectorEndState End = EVaRestInspectorEndState::InFlight;
	EVaRestInspectorDetailState RequestExcerpt = EVaRestInspectorDetailState::Absent;
};
FRowImage Row(FService& S, uint64 Id)
{
	FRowImage O;
	S.ForEachRecord([&](const FVaRestInspectorRecordView& R) {
		if (R.Kind != EVaRestInspectorRecordKind::Http || R.RequestId != Id) return;
		O.Found = true; O.Pinned = R.bPinned; O.Partial = R.bPartial; O.Terminal = R.bTerminalKnown;
		O.Sealed = R.bSealed; O.Actual = R.bActualCompletionObserved; O.Birth = R.BirthMetadataOrdinal;
		O.Arrival = R.ArrivalOrdinal; O.Revision = R.Revision; O.Timeline = R.Timeline.Num();
		O.Status = R.FinalStatus; O.CompletedAt = R.CompletedAt; O.End = R.EndState;
		O.RequestExcerpt = R.RequestExcerptState;
		if (R.Started.Fixed) O.StartedSeq = R.Started.Fixed->Header.Seq;
	});
	return O;
}
void UnknownTerminal(FAutomationTestBase& T, FService& S, uint64 Id, const TCHAR* Label)
{
	const FRowImage R = Row(S, Id);
	T.TestTrue(FString::Printf(TEXT("%s row remains visible"), Label), R.Found);
	T.TestFalse(FString::Printf(TEXT("%s absent row unpinned"), Label), R.Pinned);
	T.TestFalse(FString::Printf(TEXT("%s no invented Completed"), Label), R.Actual || R.Terminal);
	T.TestTrue(FString::Printf(TEXT("%s interrupted without transport result"), Label), R.End == EVaRestInspectorEndState::CaptureInterrupted);
	T.TestEqual(FString::Printf(TEXT("%s no invented completion time"), Label), R.CompletedAt, -1.0);
	T.TestEqual(FString::Printf(TEXT("%s no invented status"), Label), R.Status, 0);
}
FVaRestTraceEvent Event(const FVaRestTraceStarted& S)
{
	FVaRestTraceEvent E; E.Kind = EVaRestTraceKind::Started; E.Time = S.StartedAt;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), S); return E;
}
FVaRestTraceEvent Completed(uint64 Id, bool bScalar = false)
{
	FVaRestTraceCompleted C; C.RequestId = Id; C.Outcome = EVaRestTraceOutcome::Success;
	C.Response.Status = 207; C.Response.BodyBytes = 19; C.Response.Storage = EVaRestTraceResponseStorage::Memory;
	if (bScalar) C.Error.Message = FString::ChrN(FNormalizedLimits::MaxStringChars + 1, TCHAR('x'));
	FVaRestTraceEvent E; E.Kind = EVaRestTraceKind::Completed; E.Time = 17;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceCompleted>(), MoveTemp(C)); return E;
}
FVaRestTraceEvent Excerpt(uint64 Id, int32 Capacity = 64)
{
	auto Bytes = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
	Bytes->Reserve(Capacity); Bytes->Add(uint8('x'));
	FVaRestTraceExcerptEvent X; X.RequestId = Id; X.Owner = EVaRestTraceExcerptOwner::Request;
	X.Excerpt.Bytes = Bytes; X.Excerpt.TotalBytes = 1;
	FVaRestTraceEvent E; E.Kind = EVaRestTraceKind::Excerpt; E.Time = 11;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceExcerptEvent>(), MoveTemp(X)); return E;
}
void Emit(FVaRestTraceEvent E)
{
	FVaRestTrace::Emit(E.Kind, E.Time, MoveTemp(E.Payload));
}

class FFixture
{
public:
	explicit FFixture(FAutomationTestBase& InTest) : Test(InTest), Source(MakeShared<FSourceState, ESPMode::ThreadSafe>())
	{
		// Registry is genuinely isolated; contributors and callbacks own their state.
		SourceHandle = FVaRestTrace::OnBuildSnapshot().AddLambda([State = Source](FVaRestTraceSnapshot& Out) {
			++State->SnapshotWalks;
			if (State->DuringNextWalk.IsSet())
			{
				FVaRestTraceEvent E = MoveTemp(State->DuringNextWalk.GetValue()); State->DuringNextWalk.Reset();
				FVaRestTrace::Emit(E.Kind, E.Time, MoveTemp(E.Payload));
			}
			for (auto S : State->Live) { S.Flags |= EVaRestTraceRequestFlags::Partial; Out.InFlight.Add(MoveTemp(S)); }
		});
		Policy.Limits.StoreBytes = 16ll * 1024 * 1024; Policy.Limits.MaxRecords = 512;
		Policy.Limits.PendingBytes = 4ll * 1024 * 1024; Policy.Limits.PendingEvents = 4096;
	}
	~FFixture()
	{
		if (Service) Service->Shutdown();
		Clock.Reset(); Service.Reset();
		FVaRestTrace::OnBuildSnapshot().Remove(SourceHandle);
	}
	bool Create(EFactoryFailure Failure = EFactoryFailure::None)
	{
		FServiceTestOptions O; O.RefuseBefore = Failure;
		if (!Test.TestTrue(TEXT("real admitted factory succeeds; no capability override or skip"), FService::TryCreateForTest(Policy, O, Service))) return false;
		Clock = Service->ClockForTest();
		return Test.TestTrue(TEXT("genuinely admitted deterministic clock"), Clock.IsValid());
	}
	bool Start() { return Test.TestTrue(TEXT("real generation starts"), Service->Start(Policy)); }
	uint64 AddLive(bool bEmit = true)
	{
		const uint64 Id = FVaRestTrace::NewRequestId(); Source->Add(Id);
		if (bEmit) Emit(Event(Source->Live.Last()));
		return Id;
	}
	void Finish(uint64 Id, bool bScalar = false) { Source->Remove(Id); Emit(Completed(Id, bScalar)); }
	void Tick(int32 Count = 1) { for (int32 N = 0; N < Count; ++N) Service->Tick(); }
	void Advance(int64 Nanos) { Clock->AdvanceNanos(Nanos); }
	FAutomationTestBase& Test;
	// First constructed, last destroyed: strict LIFO restoration.
	FVaRestTrace::FScopedSinkIsolation Isolation;
	TSharedRef<FSourceState, ESPMode::ThreadSafe> Source;
	FDelegateHandle SourceHandle;
	FCapturePolicy Policy;
	FServicePtr Service;
	TSharedPtr<FDeterministicReceiptClock, ESPMode::ThreadSafe> Clock;
};

// The event proves admission and the before-Enqueue boundary. The timeout is
// failure containment only. Every exit releases BEFORE joining and draining.
class FExcerptHold
{
public:
	explicit FExcerptHold(TSharedPtr<FCaptureQueue, ESPMode::ThreadSafe> Q) : Queue(MoveTemp(Q))
	{
		Ready = FPlatformProcess::GetSynchEventFromPool(true);
		Release = FPlatformProcess::GetSynchEventFromPool(true);
		Hooks.BeforeEnqueue = Ready; Hooks.ReleaseEnqueue = Release;
	}
	~FExcerptHold()
	{
		Finish();
		FPlatformProcess::ReturnSynchEventToPool(Ready); FPlatformProcess::ReturnSynchEventToPool(Release);
	}
	bool Start(FAutomationTestBase& T, FVaRestTraceEvent E)
	{
		Hooks.BeforeEnqueueRequestId = E.Payload.Get<FVaRestTraceExcerptEvent>().RequestId;
		Queue->SetTestHooks(&Hooks);
		Work = Async(EAsyncExecution::ThreadPool, [Q = Queue, E = MoveTemp(E)]() mutable {
   const EPush Result=Q->TryPush(E); Q.Reset(); return Result;
  });
		return T.TestTrue(TEXT("owned worker reached admitted before-Enqueue barrier"), Ready->Wait(10000));
	}
	void Finish()
	{
		Release->Trigger();
		if (!bJoined && Work.IsValid()) { Result = Work.Get(); bJoined = true; Queue->SetTestHooks(nullptr); Queue.Reset(); }
	}
	EPush Result = EPush::Unsupported;
private:
	TSharedPtr<FCaptureQueue, ESPMode::ThreadSafe> Queue;
	FEvent* Ready = nullptr; FEvent* Release = nullptr;
	FQueueTestHooks Hooks;
	TFuture<EPush> Work;
	bool bJoined = false;
};

// Choose an actual upstream excerpt capacity using the real measured layout.
// It leaves less than one independently measured scalar Completed charge.
// No reservation, limit, ledger or calibration is rewritten by this helper.
bool ExhaustWithHeldExcerpt(FFixture& F, FExcerptHold& Hold, uint64 HoldRequest = 0, int64* OutHeldCharge = nullptr)
{
	auto Pending = FServiceTestAccess::Pending(*F.Service);
	FAllocationAdapter& A = FServiceTestAccess::Allocator(*F.Service);
	FPacketLayout Scalar;
	if (!F.Test.TestTrue(TEXT("independent scalar Completed layout"), FPacketCodec::Measure(Completed(1, true), F.Policy, A, Scalar) == EMeasure::ScalarOnly)) return false;
	const int64 Slack = F.Service->TestImage().EffectivePending - F.Service->TestImage().PendingTotal;
	const uint64 Id = HoldRequest ? HoldRequest : FVaRestTrace::NewRequestId();
	FPacketLayout Empty;
	FVaRestTraceEvent E = Excerpt(Id, 0);
	if (FPacketCodec::Measure(E, F.Policy, A, Empty) != EMeasure::Full) return false;
	const int64 Target = Slack - Empty.PendingCharge;
	if (!F.Test.TestTrue(TEXT("real payload fits with count slack"), Target > 0 && Target <= MAX_int32 && Pending->Count.load() + 2 < Pending->CountLimit)) return false;
	bool Found = false; int64 SelectedCharge = 0;
	for (int32 Shrink = 0; Shrink < 256 && Target > Shrink; ++Shrink)
	{
		E = Excerpt(Id, int32(Target - Shrink)); FPacketLayout L;
		if (FPacketCodec::Measure(E, F.Policy, A, L) == EMeasure::Full && L.PendingCharge <= Slack && Slack - L.PendingCharge < Scalar.PendingCharge)
		{ Found = true; SelectedCharge = L.PendingCharge; break; }
	}
	if (!F.Test.TestTrue(TEXT("actual retained capacity exhausts byte slack, not count"), Found)) return false;
	if (!Hold.Start(F.Test, MoveTemp(E))) return false;
	if (OutHeldCharge) *OutHeldCharge = SelectedCharge;
	F.Test.TestTrue(TEXT("real excerpt reservation remains outstanding"), Pending->ExcerptReservations.load() > 0);
	F.Test.TestTrue(TEXT("byte-only exhaustion keeps count slack"), Pending->Count.load() + 1 < Pending->CountLimit);
	F.Test.TestTrue(TEXT("actual admission respects common total"), F.Service->TestImage().PendingTotal <= F.Policy.Limits.PendingBytes);
	return true;
}
TOptional<int64> IndependentGenerationBytes(FService& S)
{
	FAllocationAdapter& A = FServiceTestAccess::Allocator(S);
	const auto Queue = A.Bound(sizeof(FCaptureQueue), alignof(FCaptureQueue));
	const auto Sink = A.Bound(sizeof(FSink), alignof(FSink));
	const auto Internal = FCaptureQueue::InternalBaselineCharge(A);
	if (!Queue.IsSet() || !Sink.IsSet() || !Internal.IsSet()) return {};
	int64 Sum = 2 * 4096; // Actual separate Queue and Sink stored controllers.
	if (!AddChecked(Sum, int64(Queue.GetValue()), MAX_int64) ||
		!AddChecked(Sum, int64(Sink.GetValue()), MAX_int64) || !AddChecked(Sum, Internal.GetValue(), MAX_int64)) return {};
	return Sum;
}

void DroppedCompletion(FFixture& F, uint64 Id)
{
	const auto Q = F.Service->QueueForTest();
	const uint64 Published = Q->LastPublishedMetadata(), BeforeLoss = Q->LossEpoch();
	F.Source->Remove(Id);
	F.Test.TestTrue(TEXT("real normal Completed dropped"), Q->TryPush(Completed(Id)) == EPush::Dropped);
	F.Test.TestTrue(TEXT("independently scalar Completed also dropped"), Q->TryPush(Completed(Id, true)) == EPush::Dropped);
	F.Test.TestEqual(TEXT("drops create no successful metadata ordinal"), Q->LastPublishedMetadata(), Published);
	F.Test.TestTrue(TEXT("real loss epoch advances"), Q->LossEpoch() > BeforeLoss);
}

bool PolicyCases(FAutomationTestBase& T)
{
	T.TestTrue(TEXT("interactive editor default on"), FService::InitialCapturePolicy(true, false, true, false, TEXT("")));
	T.TestFalse(TEXT("unattended editor default off"), FService::InitialCapturePolicy(true, true, true, false, TEXT("")));
	T.TestFalse(TEXT("editor setting honored"), FService::InitialCapturePolicy(true, false, false, true, TEXT("")));
	T.TestFalse(TEXT("packaged default off"), FService::InitialCapturePolicy(false, false, true, false, TEXT("")));
	T.TestTrue(TEXT("packaged explicit setting on"), FService::InitialCapturePolicy(false, false, false, true, TEXT("")));
	T.TestFalse(TEXT("explicit zero overrides interactive on"), FService::InitialCapturePolicy(true, false, true, true, TEXT("-VaRestInspector=0")));
	T.TestTrue(TEXT("bare override enables unattended"), FService::InitialCapturePolicy(true, true, false, false, TEXT("-VaRestInspector")));
	T.TestTrue(TEXT("explicit one enables packaged"), FService::InitialCapturePolicy(false, false, false, false, TEXT("-VaRestInspector=1")));
	return true;
}

bool AttachmentAndControls(FAutomationTestBase& T)
{
	FFixture F(T); const uint64 Id = F.AddLive(false);
	if (!F.Create()) return false;
	struct FNotifications { int32 Count = 0, Depth = 0, MaxDepth = 0; bool Consistent = true; };
	auto N = MakeShared<FNotifications, ESPMode::ThreadSafe>();
	FVaRestInspectorChanged Changed;
	Changed.AddLambda([N, Weak = TWeakPtr<FService, ESPMode::ThreadSafe>(F.Service)](uint64) {
		++N->Depth; N->MaxDepth = FMath::Max(N->MaxDepth, N->Depth); ++N->Count;
		if (auto S = Weak.Pin()) { FSnapshotPtr Snapshot; N->Consistent &= S->TrySnapshot(Snapshot); }
		--N->Depth;
	});
	F.Service->AttachFacade(Changed, nullptr);
	ON_SCOPE_EXIT { F.Service->DetachFacade(); };
	if (!F.Start()) return false;
	T.TestTrue(TEXT("OnAttached is synchronous Partial/pinned"), Row(*F.Service, Id).Partial && Row(*F.Service, Id).Pinned);
	T.TestEqual(TEXT("OnChanged is deferred until batch"), N->Count, 0);
	F.Tick(); T.TestEqual(TEXT("one consistent attach notification"), N->Count, 1);
	const uint64 G = F.Service->TestImage().Generation;
	T.TestTrue(TEXT("double Start is safe"), F.Service->Start(F.Policy));
	T.TestEqual(TEXT("double Start does not rotate"), F.Service->TestImage().Generation, G);
	T.TestEqual(TEXT("exactly one active sink"), FVaRestTrace::GetSinks()->Num(), 1);
	F.Service->Pause(); F.Service->Pause(); F.Finish(Id); F.Service->Start(F.Policy);
	UnknownTerminal(T, *F.Service, Id, TEXT("paused completion"));
	const uint64 Live = F.AddLive(); F.Tick();
	auto OldList = FVaRestTrace::GetSinks(); auto OldSink = F.Service->SinkForTest();
	F.Service->Clear();
	T.TestTrue(TEXT("Clear reattaches actually live request"), Row(*F.Service, Live).Partial && Row(*F.Service, Live).Pinned);
	const auto Before = F.Service->TestImage();
	const uint64 LateId = FVaRestTrace::NewRequestId();
	OldSink->OnEvent(Event(F.Source->Make(LateId)));
	OldSink->OnEvent(Excerpt(Live));
	T.TestEqual(TEXT("M1 closed old sink cannot mutate a still-owned Store"), F.Service->TestImage().Revision, Before.Revision);
	T.TestFalse(TEXT("M1 closed old sink cannot repopulate a row"), Row(*F.Service, LateId).Found);
	const auto After = F.Service->TestImage();
	T.TestEqual(TEXT("closed callback copies no borrowed text"), After.TextCopies, Before.TextCopies);
	T.TestEqual(TEXT("closed callback acquires no excerpt"), After.ExcerptRefs, Before.ExcerptRefs);
	F.Tick();
	T.TestTrue(TEXT("closed list still owns its original sink"), OldList.IsValid() && OldList->Num() == 1);
	OldSink.Reset(); OldList.Reset();
	T.TestTrue(TEXT("observers see complete snapshot transactions"), N->Consistent);
	T.TestEqual(TEXT("notifications never recurse"), N->MaxDepth, 1);
	F.Service->Clear(); F.Service->Clear(); F.Service->Pause();
	F.Service->Shutdown(); F.Service->Shutdown();
	T.TestFalse(TEXT("idempotent shutdown remains inactive"), F.Service->IsCapturing());
	T.TestFalse(TEXT("shutdown admits no Start"), F.Service->Start(F.Policy));
	return true;
}

bool PauseStatsClosure(FAutomationTestBase& T)
{
	FFixture F(T); if (!F.Create() || !F.Start()) return false;
	// Establish ordinary context through the actual registry before Started.
	FVaRestTraceScope Scope; Scope.ScopeId = 0; Scope.bOpened = true; Scope.Label = TEXT("Process");
	FVaRestTrace::Emit(EVaRestTraceKind::Scope, 9, FVaRestTracePayload(TInPlaceType<FVaRestTraceScope>(), Scope));
	FVaRestTraceClient Client; Client.ClientId = 88001; Client.ScopeId = Scope.ScopeId;
	Client.bCreated = true; Client.DisplayName = TEXT("Lifecycle");
	FVaRestTrace::Emit(EVaRestTraceKind::Client, 9, FVaRestTracePayload(TInPlaceType<FVaRestTraceClient>(), Client));
	const uint64 Id = F.AddLive(); F.Tick();
	FVaRestTraceAttemptDispatched Dispatch; Dispatch.RequestId = Id; Dispatch.Attempt = 1;
	Dispatch.DispatchId = 7; Dispatch.GapKind = EVaRestTraceGap::None;
	FVaRestTrace::Emit(EVaRestTraceKind::AttemptDispatched, 12,
		FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptDispatched>(), Dispatch));
	F.Tick();
	FVaRestInspectorHttpStats OriginalGroup;
	uint64 OriginalArrival = 0, OriginalStartedSeq = 0;
	const auto CheckSnapshot = [&](const TCHAR* Stage, bool bPartial, bool bPinned, bool bInterrupted, bool bSave) {
		FSnapshotPtr Snapshot;
		const auto Label = [Stage](const TCHAR* What) { return FString::Printf(TEXT("Pause stats %s: %s"), Stage, What); };
		const bool bSnapshot = T.TestTrue(Label(TEXT("immutable snapshot admitted")), F.Service->TrySnapshot(Snapshot));
		const bool bValid = T.TestTrue(Label(TEXT("immutable snapshot valid")), Snapshot.IsValid());
		if (!bSnapshot || !bValid) return false;
		int32 Rows = 0;
		Snapshot->ForEachRecord([&](const FVaRestInspectorRecordView& R) {
			if (R.Kind != EVaRestInspectorRecordKind::Http || R.RequestId != Id) return;
			++Rows;
			T.TestEqual(Label(TEXT("partial truth")), R.bPartial, bPartial);
			T.TestEqual(Label(TEXT("pin truth")), R.bPinned, bPinned);
			T.TestTrue(Label(TEXT("end state truth")), R.EndState == (bInterrupted ? EVaRestInspectorEndState::CaptureInterrupted : EVaRestInspectorEndState::InFlight));
			T.TestFalse(Label(TEXT("no terminal result invented")), R.bTerminalKnown || R.bActualCompletionObserved || R.bSealed);
			T.TestEqual(Label(TEXT("no completion status invented")), R.FinalStatus, 0);
			T.TestEqual(Label(TEXT("no completion time invented")), R.CompletedAt, -1.0);
			T.TestNull(Label(TEXT("no Completed descriptor invented")), R.Completed.Fixed);
			T.TestTrue(Label(TEXT("real context retained")), R.bContextKnown && R.ScopeId == Scope.ScopeId && R.ClientId == Client.ClientId);
			if (T.TestNotNull(Label(TEXT("real original Started retained")), R.Started.Fixed))
			{
				T.TestTrue(Label(TEXT("descriptor is ordinary Started")), R.Started.Fixed->Header.Kind == EVaRestTraceKind::Started && !R.Started.Fixed->Header.bSnapshot);
				T.TestEqual(Label(TEXT("Started belongs to original request")), R.Started.Fixed->Scalars.RequestId, Id);
				if (bSave) { OriginalArrival = R.ArrivalOrdinal; OriginalStartedSeq = R.Started.Fixed->Header.Seq; }
				T.TestEqual(Label(TEXT("original Started identity retained")), R.Started.Fixed->Header.Seq, OriginalStartedSeq);
				T.TestTrue(Label(TEXT("Started was actually emitted")), OriginalStartedSeq != 0);
			}
			T.TestEqual(Label(TEXT("original arrival retained")), R.ArrivalOrdinal, OriginalArrival);
		});
		T.TestEqual(Label(TEXT("exactly one original row")), Rows, 1);
		int32 Groups = 0;
		Snapshot->ForEachStats([&](const FVaRestInspectorHttpStats& G) {
			if (bSave ? (G.Started != 1 || G.Dispatches != 1) :
				(G.bOverflow != OriginalGroup.bOverflow || G.ScopeId != OriginalGroup.ScopeId || G.ClientId != OriginalGroup.ClientId ||
				 G.Endpoint != OriginalGroup.Endpoint || G.Key != OriginalGroup.Key)) return;
			++Groups;
			if (bSave) OriginalGroup = G; // Own the complete identity; no visitor view escapes.
			T.TestFalse(Label(TEXT("actual group is not overflow")), G.bOverflow);
			T.TestFalse(Label(TEXT("actual group key is nonempty")), G.Key.IsEmpty());
			T.TestEqual(Label(TEXT("actual group scope")), G.ScopeId, Scope.ScopeId);
			T.TestEqual(Label(TEXT("actual group client")), G.ClientId, Client.ClientId);
			T.TestEqual(Label(TEXT("one genuine Started")), G.Started, uint64(1));
			T.TestEqual(Label(TEXT("one genuine Dispatch")), G.Dispatches, uint64(1));
			T.TestEqual(Label(TEXT("Partial observed exactly once")), G.Partial, uint64(bPartial ? 1 : 0));
			T.TestEqual(Label(TEXT("Incomplete observed exactly once")), G.Incomplete, uint64(bPartial ? 1 : 0));
			T.TestEqual(Label(TEXT("unfinished wire truth")), G.bWireBytesUnknown, bPartial);
			T.TestEqual(Label(TEXT("no Completed invented")), G.Completed, uint64(0));
			T.TestEqual(Label(TEXT("no Success invented")), G.Success, uint64(0));
			T.TestEqual(Label(TEXT("no Error invented")), G.Error, uint64(0));
			T.TestEqual(Label(TEXT("no Queued invented")), G.Queued, uint64(0));
			T.TestEqual(Label(TEXT("no Canceled invented")), G.Canceled, uint64(0));
			T.TestEqual(Label(TEXT("no upstream wire bytes invented")), G.WireBytesUp, uint64(0));
			T.TestEqual(Label(TEXT("no downstream wire bytes invented")), G.WireBytesDown, uint64(0));
			T.TestEqual(Label(TEXT("known None gap remains classified")), G.UnknownDispatchClassification, uint64(0));
		});
		T.TestEqual(Label(TEXT("exactly one saved group identity")), Groups, 1);
		Snapshot.Reset(); // Release each admitted reader before the next control/snapshot.
		return Rows == 1 && Groups == 1;
	};
	if (!CheckSnapshot(TEXT("before Pause"), false, true, false, true)) return false;
	F.Service->Pause();
	bool Good = CheckSnapshot(TEXT("immediate Pause"), true, false, true, false);
	F.Service->Pause();
	Good = CheckSnapshot(TEXT("repeated Pause"), true, false, true, false) && Good;
	if (!F.Start()) return false; // The original Id is still present in the real source.
	Good = CheckSnapshot(TEXT("Resume original row"), true, true, false, false) && Good;
	F.Service->Pause();
	Good = CheckSnapshot(TEXT("Pause after Resume"), true, false, true, false) && Good;
	return Good;
}

bool ReentrantControls(FAutomationTestBase& T)
{
	FFixture F(T); if (!F.Create() || !F.Start()) return false;
	const uint64 Id = F.AddLive(); F.Tick();
	const uint64 Before = F.Service->TestImage().Generation;
	struct FObserver { int32 Calls = 0; bool Atomic = true; FDelegateHandle Handle; };
	auto O = MakeShared<FObserver, ESPMode::ThreadSafe>();
	auto Changed = MakeShared<FVaRestInspectorChanged, ESPMode::ThreadSafe>();
	O->Handle = Changed->AddLambda([O, Changed, Weak = TWeakPtr<FService, ESPMode::ThreadSafe>(F.Service)](uint64) {
		if (auto S = Weak.Pin())
		{
			++O->Calls; const uint64 G = S->TestImage().Generation;
			FSnapshotPtr Snapshot; O->Atomic &= S->TrySnapshot(Snapshot);
			S->Clear(); S->SetCapture(true);
			O->Atomic &= S->TestImage().Generation == G;
			Changed->Remove(O->Handle);
		}
	});
	F.Service->AttachFacade(*Changed, nullptr);
	ON_SCOPE_EXIT { F.Service->DetachFacade(); Changed->Clear(); };
	F.Finish(Id); F.Tick();
	T.TestEqual(TEXT("self-removing observer executes once"), O->Calls, 1);
	T.TestTrue(TEXT("observer controls defer until complete batch"), O->Atomic);
	T.TestTrue(TEXT("Clear then last Start applies outside observer"), F.Service->TestImage().Generation > Before && F.Service->IsCapturing());
	F.Tick(); T.TestEqual(TEXT("deferred notification does not recurse removed observer"), O->Calls, 1);
	const uint64 V = F.AddLive(); F.Tick();
	struct FVisitState { int32 Visits = 0; bool Stable = true; };
	auto VS = MakeShared<FVisitState, ESPMode::ThreadSafe>();
	T.TestTrue(TEXT("visitor accepts consistent enumeration"), F.Service->ForEachRecord([S = F.Service, VS](const auto&) {
		++VS->Visits; const auto G = S->TestImage().Generation;
		S->Clear(); S->SetCapture(false); S->SetCapture(true); S->Shutdown();
		VS->Stable &= !S->IsShutdown() && S->TestImage().Generation == G;
	}));
	T.TestTrue(TEXT("visitor ran on actual row"), VS->Visits > 0 && V != 0);
	T.TestTrue(TEXT("Shutdown deferred through visitor"), VS->Stable);
	T.TestTrue(TEXT("Shutdown wins over Clear/last Start"), F.Service->IsShutdown());
	return true;
}

bool WeakAndCallbacks(FAutomationTestBase& T)
{
	FFixture F(T); if (!F.Create() || !F.Start()) return false;
	const uint64 Id = F.AddLive();
	auto Argument = FServiceTestAccess::Argument(*F.Service);
	FServiceTestAccess::EndFrame(Argument);
	T.TestTrue(TEXT("real EndFrame thunk consumes first batch"), Row(*F.Service, Id).Found);
	const uint64 Next = F.AddLive();
	FServiceTestAccess::EndFrame(Argument);
	T.TestFalse(TEXT("same-frame recursive thunk refuses a second batch"), Row(*F.Service, Next).Found);
	F.Tick(); T.TestTrue(TEXT("direct owning next batch observes queued event"), Row(*F.Service, Next).Found);
	const auto Before = FServiceTestAccess::Image(*F.Service);
	T.TestTrue(TEXT("both actual callback claims live"), Before.Claims[6].State == EClaimState::BackingLive && Before.Claims[7].State == EClaimState::BackingLive);
	TWeakPtr<FProductionRootBlock, ESPMode::ThreadSafe> Root = F.Service->RootForTest();
	FRootRetireReceipt* Receipt = F.Service->ReceiptForTest();
	TWeakPtr<FService, ESPMode::ThreadSafe> Weak = F.Service;
	FServiceTestAccess::PreExit(Argument);
	T.TestTrue(TEXT("actual PreExit thunk shuts down"), F.Service->IsShutdown());
	const auto Closed = FServiceTestAccess::Image(*F.Service);
	T.TestTrue(TEXT("Remove returned before callback claims retired"), Closed.Claims[6].IsEmpty() && Closed.Claims[7].IsEmpty());
	T.TestTrue(TEXT("physical callback frees reduce backing"), Closed.ContextProbe.LiveBacking < Before.ContextProbe.LiveBacking);
	F.Clock.Reset(); F.Service.Reset();
	T.TestFalse(TEXT("Service last strong ended"), Weak.IsValid());
	FBothLedgerImage Freed;
	T.TestFalse(TEXT("expired Service controller still retains root"), Receipt->ReadFreed(Freed));
	FServiceTestAccess::EndFrame(Argument); // Failed Pin: copied weak still protects State.
	T.TestFalse(TEXT("failed Pin did not free protected State early"), Receipt->ReadFreed(Freed));
	Argument.Service.Reset(); Weak.Reset();
	T.TestFalse(TEXT("root payload is actually gone"), Root.IsValid());
	T.TestTrue(TEXT("owned root weak reads only post-free receipt"), Receipt->ReadFreed(Freed));
	T.TestEqual(TEXT("actual root free leaves controller-only pending charge"), Freed.PendingTotal, int64(4096));
	T.TestEqual(TEXT("actual root free leaves controller-only store charge"), Freed.StoreTotal, int64(4096));
	T.TestEqual(TEXT("root physical backing is zero before receipt publication"), Freed.ContextProbe.LiveBacking, int64(0));
	T.TestTrue(TEXT("only final weak controller claim remains"), Freed.Claims[0].IsEmpty() && Freed.Claims[1].Reserved == 4096);
	Receipt = nullptr; Root.Reset(); // Final stored deleter retires controller, then excluded engine free tail.
	return true;
}

struct FFrameRemovalState
{
	explicit FFrameRemovalState(FAutomationTestBase& T) : Fixture(MakeShared<FFixture, ESPMode::ThreadSafe>(T)) {}
	TSharedPtr<FFixture, ESPMode::ThreadSafe> Fixture;
	FVaRestInspectorChanged Changed;
	TWeakPtr<FProductionRootBlock, ESPMode::ThreadSafe> Root;
	FRootRetireReceipt* Receipt = nullptr;
	uint64 InitialFrame = 0; uint32 Polls = 0;
	bool Observed = false, Deferred = false;
};
class FCheckFrameRemoval final : public IAutomationLatentCommand
{
public:
	explicit FCheckFrameRemoval(TSharedRef<FFrameRemovalState, ESPMode::ThreadSafe> S) : State(MoveTemp(S)) {}
	bool Update() override
	{
		auto& F = *State->Fixture; auto& T = F.Test;
		if (!F.Service->IsShutdown() && ++State->Polls < 128) return false;
		T.TestTrue(TEXT("actual normal engine EndFrame invoked observer"), State->Observed && GFrameCounter > State->InitialFrame);
		T.TestTrue(TEXT("actual callback observer Shutdown was deferred"), State->Deferred);
		T.TestTrue(TEXT("actual self-removal ends service"), F.Service->IsShutdown());
		F.Service->Shutdown(); // Failure containment if a frame was unavailable.
		const auto Closed = FServiceTestAccess::Image(*F.Service);
		T.TestTrue(TEXT("actual executing binding Remove returned before charge retirement"), Closed.Claims[6].IsEmpty() && Closed.Claims[7].IsEmpty());
		T.TestEqual(TEXT("actual callback frame leases returned"), Closed.RootRoles[uint8(ERootRole::CallbackFrame)], uint32(0));
		T.TestEqual(TEXT("actual copied delegate arguments returned"), Closed.RootRoles[uint8(ERootRole::DelegateArgument)], uint32(0));
		State->Changed.Clear(); F.Clock.Reset(); F.Service.Reset();
		FBothLedgerImage Freed;
		T.TestFalse(TEXT("self-removed callback root payload actually freed"), State->Root.IsValid());
		T.TestTrue(TEXT("actual self-removal root receipt after free"), State->Receipt->ReadFreed(Freed));
		T.TestEqual(TEXT("actual self-removal leaves controller-only charge"), Freed.PendingTotal, int64(4096));
		State->Receipt = nullptr; State->Root.Reset(); State->Fixture.Reset(); return true;
	}
private:
	TSharedRef<FFrameRemovalState, ESPMode::ThreadSafe> State;
};
bool QueueActualFrameRemoval(FAutomationTestBase& T)
{
	auto State = MakeShared<FFrameRemovalState, ESPMode::ThreadSafe>(T);
	auto& F = *State->Fixture;
	if (!F.Create() || !F.Start()) return false;
	State->Root = F.Service->RootForTest(); State->Receipt = F.Service->ReceiptForTest(); State->InitialFrame = GFrameCounter;
	TWeakPtr<FFrameRemovalState, ESPMode::ThreadSafe> Weak = State;
	State->Changed.AddLambda([Weak] (uint64) {
		if (auto S = Weak.Pin())
		{
			S->Observed = true; S->Fixture->Service->Shutdown();
			S->Deferred = !S->Fixture->Service->IsShutdown();
		}
	});
	F.Service->AttachFacade(State->Changed, nullptr); F.AddLive();
	// No global Broadcast or direct thunk invocation: the engine owns this frame.
	T.AddCommand(new FCheckFrameRemoval(State)); return true;
}

// Actual engine frames own this test's notifications and idle transitions.
// The first behavioral failure exits through real shutdown, so accepted13
// production has one specific idle-unregister RED rather than a cascade.
struct FIdlePauseState
{
 explicit FIdlePauseState(FAutomationTestBase& T) : Fixture(MakeShared<FFixture,ESPMode::ThreadSafe>(T)) {}
 ~FIdlePauseState() { Cleanup(); }
 void Cleanup()
 {
  if (Unstarted) { if (Unstarted->Service) Unstarted->Service->Shutdown(); Unstarted->Clock.Reset(); Unstarted->Service.Reset(); Unstarted.Reset(); }
  if (Fixture && Fixture->Service) { Fixture->Service->DetachFacade(); Fixture->Service->Shutdown(); }
  if (Held)
  {
   Held->Finish();
   if (HeldQueue) { while (HeldQueue->TryPopExcerpt().IsSet()) {} }
  }
  Held.Reset(); HeldSink.Reset(); HeldQueue.Reset(); HeldList.Reset();
  Changed.Clear();
  if (Fixture) { Fixture->Clock.Reset(); Fixture->Service.Reset(); }
  Argument.Reset(); WeakService.Reset();
  Receipt=nullptr; Root.Reset(); Fixture.Reset();
 }
 TSharedPtr<FFixture,ESPMode::ThreadSafe> Fixture,Unstarted;
 FVaRestInspectorChanged Changed;
 TUniquePtr<FExcerptHold> Held;
 TSharedPtr<FCaptureQueue,ESPMode::ThreadSafe> HeldQueue;
 TSharedPtr<FSink,ESPMode::ThreadSafe> HeldSink;
 TSharedPtr<const FVaRestTrace::FSinkList,ESPMode::ThreadSafe> HeldList;
 TOptional<FCallbackArgument> Argument;
 TWeakPtr<FService,ESPMode::ThreadSafe> WeakService;
 TWeakPtr<FProductionRootBlock,ESPMode::ThreadSafe> Root;
 FRootRetireReceipt* Receipt=nullptr;
 FBothLedgerImage BeforeIdle,Idle,PressureIdle;
 uint64 RequestId=0,Frame=0,IdleLastFrame=0;
 int64 HeldCharge=0,OriginalPending=0;
 int32 Phase=0,Calls=0,BeforeCalls=0,Depth=0,MaxDepth=0;
 uint32 Polls=0;
 bool BeforeIdleSampled=false,Reenter=false,ObserverStable=false,FacadeClosed=false;
};
bool SameFrameAdmission(const FRootClaim& A,const FRootClaim& B)
{
 return A.Serial==B.Serial && A.Serial!=0 && A.Reserved==B.Reserved && A.Request==B.Request &&
  A.Alignment==B.Alignment && A.Ledgers==B.Ledgers && A.Kind==B.Kind;
}
bool SameIdleCertificates(const FBothLedgerImage& A,const FBothLedgerImage& B)
{
 for (uint8 N=0;N<uint8(EControllerShape::Count);++N)
 {
  const auto& X=A.Identity.Certificates[N]; const auto& Y=B.Identity.Certificates[N];
  if (X.Request!=Y.Request || X.Capacity!=Y.Capacity || X.CertifiedBound!=Y.CertifiedBound ||
   X.TypeAlignment!=Y.TypeAlignment || X.OrdinaryAlignment!=Y.OrdinaryAlignment ||
   X.Support.Family!=Y.Support.Family || X.Support.bVerifiedModuleNew!=Y.Support.bVerifiedModuleNew ||
   X.Support.bVerifiedNoInflatingProxy!=Y.Support.bVerifiedNoInflatingProxy ||
   X.Support.bCalibrationPassed!=Y.Support.bCalibrationPassed || X.Attempted!=Y.Attempted || X.Failed!=Y.Failed) return false;
 }
 return true;
}
class FCheckIdlePause final : public IAutomationLatentCommand
{
public:
 explicit FCheckIdlePause(TSharedRef<FIdlePauseState,ESPMode::ThreadSafe> S) : State(MoveTemp(S)) {}
 bool Update() override
 {
  if (Done) return true;
  auto& F=*State->Fixture; auto& T=F.Test;
  if (State->Phase==0)
  {
   if (!F.Create()) return Finish();
   State->RequestId=F.AddLive(false);
   TWeakPtr<FIdlePauseState,ESPMode::ThreadSafe> Weak=State;
   State->Changed.AddLambda([Weak](uint64) {
    if (auto S=Weak.Pin())
    {
     ++S->Depth; S->MaxDepth=FMath::Max(S->MaxDepth,S->Depth); ++S->Calls;
     auto& Service=*S->Fixture->Service;
     if (S->Phase==2) { S->BeforeIdle=FServiceTestAccess::Image(Service); S->BeforeIdleSampled=true; }
     if (S->Reenter)
     {
      S->Reenter=false;
      const uint64 BeforeGeneration=Service.TestImage().Generation;
      Service.Clear(); Service.SetCapture(true);
      S->ObserverStable=!Service.IsCapturing() && Service.TestImage().Generation==BeforeGeneration;
     }
     --S->Depth;
    }
   });
   F.Service->AttachFacade(State->Changed,&State->FacadeClosed);
   if (!F.Start()) return Finish();
   T.TestEqual(TEXT("IdlePause Start never notifies synchronously"),State->Calls,0);
   Next(1); return false;
  }
  if (State->Phase==1)
  {
   if (!Await(State->Calls>0 && GFrameCounter>State->Frame,TEXT("IdlePause actual Start notification across a normal frame"))) return Done;
   T.TestTrue(TEXT("IdlePause capturing frame is actually bound"),FServiceTestAccess::FrameBound(*F.Service));
   State->BeforeCalls=State->Calls; Next(2);
   F.Service->Pause();
   T.TestEqual(TEXT("IdlePause Pause never notifies synchronously"),State->Calls,State->BeforeCalls);
   return false;
  }
  if (State->Phase==2)
  {
   if (!Await(State->Calls>State->BeforeCalls && GFrameCounter>State->Frame,TEXT("IdlePause genuine Pause notification arrives before idle assertion"))) return Done;
   if (!T.TestFalse(TEXT("Task14 paused idle EndFrame is unregistered after notification"),FServiceTestAccess::FrameBound(*F.Service))) return Finish();
   // This point is unreachable for the original always-bound production pump.
   if (!T.TestTrue(TEXT("IdlePause sampled a genuine notification frame"),State->BeforeIdleSampled)) return Finish();
   State->Idle=FServiceTestAccess::Image(*F.Service);
   const auto& Before=State->BeforeIdle; const auto& After=State->Idle;
   T.TestTrue(TEXT("IdlePause keeps PreExit alive and service callback state open"),FServiceTestAccess::ExitBound(*F.Service) && !FServiceTestAccess::CallbacksClosed(*F.Service));
   T.TestTrue(TEXT("IdlePause demotes actual live frame to reserved with no observed backing"),
    Before.Claims[6].State==EClaimState::BackingLive && Before.Claims[6].ObservedCapacity>0 &&
    After.Claims[6].State==EClaimState::Reserved && After.Claims[6].ObservedCapacity==0);
   T.TestTrue(TEXT("IdlePause preserves exact admitted frame identity"),SameFrameAdmission(Before.Claims[6],After.Claims[6]));
   T.TestEqual(TEXT("IdlePause retains BOTH Pending reservation"),After.PendingTotal,Before.PendingTotal);
   T.TestEqual(TEXT("IdlePause retains BOTH Store reservation"),After.StoreTotal,Before.StoreTotal);
   T.TestEqual(TEXT("IdlePause actual callback free reduces live backing only"),After.ContextProbe.LiveBacking,Before.ContextProbe.LiveBacking-Before.Claims[6].ObservedCapacity);
   T.TestEqual(TEXT("IdlePause callback frame has unwound"),FServiceTestAccess::FrameDepth(*F.Service),0);
   State->IdleLastFrame=FServiceTestAccess::LastFrame(*F.Service);
   Next(3); return false;
  }
  if (State->Phase==3)
  {
   if (!Await(GFrameCounter>State->Frame && GFrameCounter-State->Frame>=2,TEXT("IdlePause two actual quiet engine frames advance"))) return Done;
   T.TestFalse(TEXT("IdlePause quiet frames keep EndFrame absent"),FServiceTestAccess::FrameBound(*F.Service));
   T.TestEqual(TEXT("IdlePause no hidden paused callback updates LastFrame"),FServiceTestAccess::LastFrame(*F.Service),State->IdleLastFrame);
   T.TestTrue(TEXT("IdlePause quiet fixed owner baseline leaves full authority unchanged"),EqualAuthorityImages(State->Idle,FServiceTestAccess::Image(*F.Service)));
   State->Unstarted=MakeShared<FFixture,ESPMode::ThreadSafe>(T);
   if (!State->Unstarted->Create()) return Finish();
   State->Unstarted->Service->SetCapture(false);
   T.TestTrue(TEXT("IdlePause genuine never-started factory is still unpublished"),
    FServiceTestAccess::Image(*State->Unstarted->Service).Identity.DomainPhase==EDomainPhase::Unpublished);
   Next(30); return false;
  }
  if (State->Phase==30)
  {
   if (!Await(GFrameCounter>State->Frame && GFrameCounter-State->Frame>=2,TEXT("IdlePause never-started service settles through normal frames"))) return Done;
   auto& Unstarted=*State->Unstarted->Service;
   T.TestFalse(TEXT("IdlePause never-started paused service unregisters its frame"),FServiceTestAccess::FrameBound(Unstarted));
   const auto InitialIdle=FServiceTestAccess::Image(Unstarted);
   T.TestTrue(TEXT("IdlePause unpublished idle claim remains genuinely reserved"),
    InitialIdle.Identity.DomainPhase==EDomainPhase::Unpublished && InitialIdle.Claims[6].State==EClaimState::Reserved &&
    InitialIdle.Claims[6].ObservedCapacity==0 && FServiceTestAccess::ExitBound(Unstarted) && !FServiceTestAccess::CallbacksClosed(Unstarted));
   State->Unstarted->Service->Shutdown(); State->Unstarted->Clock.Reset(); State->Unstarted->Service.Reset(); State->Unstarted.Reset();
   State->BeforeCalls=State->Calls; State->Reenter=true;
   F.Service->Clear();
   T.TestEqual(TEXT("IdlePause paused Clear remains asynchronous"),State->Calls,State->BeforeCalls);
   T.TestTrue(TEXT("IdlePause real Clear rebinds an actual frame"),FServiceTestAccess::FrameBound(*F.Service));
   Next(4); return false;
  }
  if (State->Phase==4)
  {
   if (!Await(State->Calls>=State->BeforeCalls+2 && F.Service->IsCapturing() && GFrameCounter>State->Frame,TEXT("IdlePause reentrant Clear then Start produces later normal-frame notification"))) return Done;
   const auto Current=FServiceTestAccess::Image(*F.Service);
   T.TestTrue(TEXT("IdlePause rearm retains frame claim and calibration certificates"),SameFrameAdmission(State->Idle.Claims[6],Current.Claims[6]) && SameIdleCertificates(State->Idle,Current));
   T.TestEqual(TEXT("IdlePause frame rearm allocates no claim serial"),Current.Identity.NextClaimSerial,State->Idle.Identity.NextClaimSerial);
   T.TestEqual(TEXT("IdlePause frame rearm allocates no binding serial"),Current.Identity.LastBindingSerial,State->Idle.Identity.LastBindingSerial);
   T.TestTrue(TEXT("IdlePause observer controls deferred until after consistent batch"),State->ObserverStable);
   T.TestEqual(TEXT("IdlePause Changed never recurses"),State->MaxDepth,1);
   T.TestTrue(TEXT("IdlePause restarted source identity is genuinely attached"),Row(*F.Service,State->RequestId).Found && Row(*F.Service,State->RequestId).Pinned);
   State->HeldQueue=F.Service->QueueForTest(); State->HeldSink=F.Service->SinkForTest(); State->HeldList=FVaRestTrace::GetSinks();
   State->Held=MakeUnique<FExcerptHold>(State->HeldQueue);
   if (!ExhaustWithHeldExcerpt(F,*State->Held,State->RequestId,&State->HeldCharge)) return Finish();
   State->OriginalPending=F.Service->TestImage().EffectivePending;
   FNormalizedLimits Requested=F.Policy.Limits; Requested.PendingBytes/=2;
   F.Service->RequestLimitsForTest(Requested);
   bool Visited=false,Stable=true;
   const bool Enumerated=F.Service->ForEachRecord([&](const FVaRestInspectorRecordView& R) {
    if (R.RequestId!=State->RequestId) return;
    Visited=true; const uint64 BeforeGeneration=F.Service->TestImage().Generation;
    F.Service->Clear(); F.Service->SetCapture(false);
    Stable=Stable && F.Service->IsCapturing() && F.Service->TestImage().Generation==BeforeGeneration;
   });
   if (!T.TestTrue(TEXT("IdlePause actual visitor defers controls while row is borrowed"),Enumerated && Visited && Stable)) return Finish();
   T.TestFalse(TEXT("IdlePause visitor's final Pause wins"),F.Service->IsCapturing());
   State->BeforeCalls=State->Calls; Next(5); return false;
  }
  if (State->Phase==5)
  {
   if (!Await(State->Calls>State->BeforeCalls && GFrameCounter>State->Frame,TEXT("IdlePause pressured Pause notification completes on normal frame"))) return Done;
   if (!T.TestFalse(TEXT("IdlePause pressure does not keep an empty paused pump"),FServiceTestAccess::FrameBound(*F.Service))) return Finish();
   State->PressureIdle=FServiceTestAccess::Image(*F.Service);
   T.TestTrue(TEXT("IdlePause real old publisher remains admitted while frame is idle"),F.Service->TestImage().Entries>0 && F.Service->TestImage().PendingDynamic>=State->HeldCharge);
   State->BeforeCalls=State->Calls;
   F.Service->Clear();
   T.TestEqual(TEXT("IdlePause pressured paused Clear never notifies synchronously"),State->Calls,State->BeforeCalls);
   T.TestTrue(TEXT("IdlePause retained reservation rebinds under actual byte pressure"),FServiceTestAccess::FrameBound(*F.Service));
   Next(6); return false;
  }
  if (State->Phase==6)
  {
   if (!Await(State->Calls>State->BeforeCalls && GFrameCounter>State->Frame,TEXT("IdlePause pressured Clear gets actual asynchronous Changed"))) return Done;
   const auto Pressure=FServiceTestAccess::Image(*F.Service);
   T.TestFalse(TEXT("IdlePause pressured Clear returns to idle"),FServiceTestAccess::FrameBound(*F.Service));
   T.TestTrue(TEXT("IdlePause pressure rearm preserves frame admission"),SameFrameAdmission(State->PressureIdle.Claims[6],Pressure.Claims[6]));
   T.TestEqual(TEXT("IdlePause pressure rearm consumes no serial"),Pressure.Identity.NextClaimSerial,State->PressureIdle.Identity.NextClaimSerial);
   T.TestTrue(TEXT("IdlePause real retained old publisher blocks account recreation"),F.Service->TestImage().bPendingLimits);
   T.TestEqual(TEXT("IdlePause pressure preserves old effective Pending cap"),F.Service->TestImage().EffectivePending,State->OriginalPending);
   T.TestTrue(TEXT("IdlePause actual held packet charge is never reset"),F.Service->TestImage().PendingDynamic>=State->HeldCharge);
   State->Held->Finish();
   T.TestTrue(TEXT("IdlePause already admitted old publisher completes safely"),State->Held->Result==EPush::Accepted);
   while (State->HeldQueue->TryPopExcerpt().IsSet()) {}
   State->Held.Reset(); State->HeldSink.Reset(); State->HeldQueue.Reset(); State->HeldList.Reset();
   T.TestTrue(TEXT("IdlePause final old-owner release does not auto-apply limits"),F.Service->TestImage().bPendingLimits);
   State->BeforeCalls=State->Calls;
   F.Service->Clear();
   T.TestFalse(TEXT("IdlePause explicit Clear commits only after genuine old-owner quiescence"),F.Service->TestImage().bPendingLimits);
   T.TestEqual(TEXT("IdlePause committed Pending cap is the real request"),F.Service->TestImage().EffectivePending,State->OriginalPending/2);
   Next(7); return false;
  }
  if (State->Phase==7)
  {
   if (!Await(State->Calls>State->BeforeCalls && GFrameCounter>State->Frame,TEXT("IdlePause final Clear notification settles before PreExit"))) return Done;
   if (!T.TestFalse(TEXT("IdlePause final normal frame suspends again"),FServiceTestAccess::FrameBound(*F.Service))) return Finish();
   State->Argument.Emplace(FServiceTestAccess::Argument(*F.Service));
   State->WeakService=F.Service; State->Root=F.Service->RootForTest(); State->Receipt=F.Service->ReceiptForTest();
   if (!T.TestNotNull(TEXT("IdlePause actual stored root receipt exists"),State->Receipt)) return Finish();
   FServiceTestAccess::PreExit(State->Argument.GetValue());
   T.TestTrue(TEXT("IdlePause real PreExit closes facade and paused service"),State->FacadeClosed && F.Service->IsShutdown());
   const auto Closed=FServiceTestAccess::Image(*F.Service);
   T.TestTrue(TEXT("IdlePause shutdown retires idle Reserved6 and actual PreExit7"),Closed.Claims[6].IsEmpty() && Closed.Claims[7].IsEmpty());
   T.TestTrue(TEXT("IdlePause shutdown leaves neither callback handle"),!FServiceTestAccess::FrameBound(*F.Service) && !FServiceTestAccess::ExitBound(*F.Service));
   F.Clock.Reset(); F.Service.Reset();
   T.TestFalse(TEXT("IdlePause actual service last strong ended"),State->WeakService.IsValid());
   FBothLedgerImage Freed;
   T.TestFalse(TEXT("IdlePause copied weak keeps callback State before actual root free"),State->Receipt->ReadFreed(Freed));
   FServiceTestAccess::EndFrame(State->Argument.GetValue());
   T.TestFalse(TEXT("IdlePause failed Pin callback still protects State"),State->Receipt->ReadFreed(Freed));
   State->Argument.Reset(); State->WeakService.Reset();
   T.TestFalse(TEXT("IdlePause root payload actually freed after callback weak release"),State->Root.IsValid());
   if (T.TestTrue(TEXT("IdlePause owned root weak reads post-free receipt"),State->Receipt->ReadFreed(Freed)))
   {
    T.TestEqual(TEXT("IdlePause final Pending retains only root controller"),Freed.PendingTotal,int64(4096));
    T.TestEqual(TEXT("IdlePause final Store retains only root controller"),Freed.StoreTotal,int64(4096));
    T.TestEqual(TEXT("IdlePause physical root backing is zero before receipt"),Freed.ContextProbe.LiveBacking,int64(0));
   }
   State->Receipt=nullptr; State->Root.Reset();
   return Finish();
  }
  return Finish();
 }
private:
 void Next(int32 Phase) { State->Phase=Phase; State->Frame=GFrameCounter; State->Polls=0; }
 bool Await(bool Ready,const TCHAR* Label)
 {
  if (Ready) { State->Polls=0; return true; }
  if (++State->Polls<128) return false;
  State->Fixture->Test.TestTrue(Label,false); Finish(); return false;
 }
 bool Finish() { Done=true; State->Cleanup(); return true; }
 TSharedRef<FIdlePauseState,ESPMode::ThreadSafe> State;
 bool Done=false;
};
bool QueueIdlePause(FAutomationTestBase& T)
{
 T.AddCommand(new FCheckIdlePause(MakeShared<FIdlePauseState,ESPMode::ThreadSafe>(T)));
 return true;
}

bool ReadersAndLimits(FAutomationTestBase& T)
{
	FFixture F(T); if (!F.Create() || !F.Start()) return false;
	const uint64 Id = F.AddLive(); F.Finish(Id); F.Tick();
	// Real retained sealed payloads put the requested shrink BELOW held bytes.
	for (int32 N = 0; N < 96; ++N)
	{
		const uint64 Large = F.AddLive(false);
		F.Source->Live.Last().Headers.Add(TEXT("X-Lifecycle"), FString::ChrN(16384, TCHAR('s')));
		Emit(Event(F.Source->Live.Last())); F.Finish(Large); F.Tick();
	}
	FSnapshotPtr Readers[4];
	for (auto& S : Readers) if (!T.TestTrue(TEXT("four real strong snapshots admitted"), F.Service->TrySnapshot(S))) return false;
	T.TestEqual(TEXT("four distinct snapshot controller slots"), F.Service->TestImage().SnapshotControllers, int64(4));
	FSnapshotPtr Refused; T.TestFalse(TEXT("fifth snapshot refused"), F.Service->TrySnapshot(Refused));
	FNormalizedLimits New = F.Policy.Limits; New.StoreBytes = 1024ll * 1024; New.PendingBytes /= 2;
	F.Service->RequestLimitsForTest(New);
	const auto Before = F.Service->TestImage();
	F.Service->Clear(); F.Service->Pause(); F.Service->SetCapture(true); F.Service->Clear();
	T.TestTrue(TEXT("requested lower store budget is below actual held retained bytes"), F.Service->TestImage().StoreTotal > New.StoreBytes);
	T.TestTrue(TEXT("retained readers keep new limits visibly pending"), F.Service->TestImage().bPendingLimits);
	T.TestEqual(TEXT("held readers preserve old effective cap"), F.Service->TestImage().EffectiveStore, Before.EffectiveStore);
	for (const auto& S : Readers) { int32 Count = 0; S->ForEachRecord([&](const auto& R) { if (R.RequestId == Id && R.bActualCompletionObserved) ++Count; }); T.TestEqual(TEXT("old snapshot immutable across Clear/attach/stop/resume"), Count, 1); }
	TWeakPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe> Expired = Readers[0];
	for (auto& S : Readers) S.Reset();
	T.TestFalse(TEXT("snapshot last strong released"), Expired.IsValid());
	F.Service->Clear();
	T.TestTrue(TEXT("expired weak controller blocks limit recreation"), F.Service->TestImage().bPendingLimits);
	Expired.Reset();
	T.TestTrue(TEXT("reader release never auto-applies limits"), F.Service->TestImage().bPendingLimits);
	F.Service->Clear();
	T.TestFalse(TEXT("explicit Clear applies limits after actual quiescence"), F.Service->TestImage().bPendingLimits);
	T.TestEqual(TEXT("new cap visible only after commit"), F.Service->TestImage().EffectiveStore, New.StoreBytes);
	T.TestTrue(TEXT("new live store total respects committed min-cap transaction"), F.Service->TestImage().StoreTotal <= New.StoreBytes);
	T.TestTrue(TEXT("new live pending total respects committed min-cap transaction"), F.Service->TestImage().PendingTotal <= New.PendingBytes);
	T.TestTrue(TEXT("cumulative high water is never reset"), F.Service->TestImage().PendingPeak >= Before.PendingPeak && F.Service->TestImage().StorePeak >= Before.StorePeak);
	return true;
}

bool OldGenerationAndPublisher(FAutomationTestBase& T)
{
	FFixture F(T); if (!F.Create() || !F.Start()) return false;
	const uint64 Id = F.AddLive(); F.Tick();
	auto OldQueue = F.Service->QueueForTest(); auto OldSink = F.Service->SinkForTest();
	auto OldList = FVaRestTrace::GetSinks();
	FExcerptHold Held(OldQueue);
	FVaRestTraceEvent Borrowed = Excerpt(Id, 4096); FPacketLayout HeldLayout;
	const auto GenerationBytes = IndependentGenerationBytes(*F.Service);
	if (!T.TestTrue(TEXT("independent generation component bounds available"), GenerationBytes.IsSet()) ||
		!T.TestTrue(TEXT("independent actual held packet layout available"), FPacketCodec::Measure(Borrowed, F.Policy, FServiceTestAccess::Allocator(*F.Service), HeldLayout) == EMeasure::Full)) return false;
	const int64 IndependentHeld = GenerationBytes.GetValue() + HeldLayout.PendingCharge;
	if (!Held.Start(T, MoveTemp(Borrowed))) return false;
	const auto Reserved = F.Service->TestImage();
	T.TestEqual(TEXT("actual old generation equals independent wrapper/controller/Impl/node/packet capacity sum"), Reserved.PendingDynamic, IndependentHeld);
	T.TestTrue(TEXT("admitted worker owns real dynamic bytes"), IndependentHeld > 4096 && Reserved.Entries > 0);
	F.Service->Pause();
	T.TestTrue(TEXT("closed admission retains admitted publisher charge"), F.Service->TestImage().PendingDynamic >= IndependentHeld);
	F.Service->Start(F.Policy);
	const auto Resumed = F.Service->TestImage();
	// M2: resets cannot satisfy an independently retained old baseline+packet sum.
	T.TestTrue(TEXT("resume includes independent old-generation actual bytes"), Resumed.PendingDynamic >= IndependentHeld + GenerationBytes.GetValue());
	T.TestTrue(TEXT("resume never resets common total below dynamic owners"), Resumed.PendingTotal >= Resumed.PendingDynamic && Resumed.PendingPeak >= Reserved.PendingPeak);
	const auto BeforeClosedCallback = F.Service->TestImage();
	OldSink->OnEvent(Excerpt(Id, 4096));
	T.TestEqual(TEXT("late old sink callback does not copy into new store stream"), F.Service->TestImage().TextCopies, BeforeClosedCallback.TextCopies);
	T.TestEqual(TEXT("late old sink callback does not acquire excerpt"), F.Service->TestImage().ExcerptRefs, BeforeClosedCallback.ExcerptRefs);
	FNormalizedLimits New = F.Policy.Limits; New.PendingBytes /= 2; F.Service->RequestLimitsForTest(New);
	F.Service->Clear(); T.TestTrue(TEXT("old list and publisher block account recreation"), F.Service->TestImage().bPendingLimits);
	Held.Finish(); // release BEFORE any drain, including assertion failures.
	T.TestTrue(TEXT("already admitted old publisher completes into old lane only"), Held.Result == EPush::Accepted);
	while (OldQueue->TryPopExcerpt().IsSet()) {}
	OldSink.Reset(); OldQueue.Reset(); OldList.Reset();
	T.TestTrue(TEXT("old owner release does not automatically apply new limits"), F.Service->TestImage().bPendingLimits);
	F.Service->Clear(); T.TestFalse(TEXT("explicit Clear after old generation final release applies limits"), F.Service->TestImage().bPendingLimits);
	return true;
}

bool FinalWeakTeardownExclusion(FAutomationTestBase& T)
{
 FFixture F(T); if (!F.Create() || !F.Start()) return false;
 TWeakPtr<FCaptureQueue,ESPMode::ThreadSafe> Queue=F.Service->QueueForTest();
 F.Service->Pause();
 T.TestFalse(TEXT("teardown control observes actual expired Queue payload"),Queue.IsValid());
 FNormalizedLimits New=F.Policy.Limits; New.StoreBytes/=2;
 F.Service->RequestLimitsForTest(New);
 FAccountTeardownTestHooks Hooks;
 Hooks.Ready=FPlatformProcess::GetSynchEventFromPool(true);
 Hooks.Release=FPlatformProcess::GetSynchEventFromPool(true);
 Hooks.Bank=FServiceTestAccess::Image(*F.Service).Identity.ActiveBankSlot;
 FServiceTestAccess::TeardownHooks(*F.Service,&Hooks); Hooks.Armed.store(true,std::memory_order_release);
 auto Worker=Async(EAsyncExecution::ThreadPool,[Q=MoveTemp(Queue)]() mutable { Q.Reset(); });
 bool Joined=false;
 // A borrowed barrier never outlives its real final-weak worker, including failures.
 ON_SCOPE_EXIT {
  Hooks.Release->Trigger(); if (!Joined) Worker.Get();
  FServiceTestAccess::TeardownHooks(*F.Service,nullptr);
  FPlatformProcess::ReturnSynchEventToPool(Hooks.Ready);
  FPlatformProcess::ReturnSynchEventToPool(Hooks.Release);
 };
 if (!T.TestTrue(TEXT("actual final weak reached borrowed teardown boundary"),Hooks.Ready->Wait(10000))) return false;
 const auto During=FServiceTestAccess::Image(*F.Service);
 const auto* Roles=During.Identity.Banks[Hooks.Bank].Roles;
 T.TestEqual(TEXT("ordinary Queue controller retired before barrier"),Roles[uint8(EBankRole::QueueController)],uint32(0));
 T.TestEqual(TEXT("ordinary Sink controller retired before barrier"),Roles[uint8(EBankRole::SinkController)],uint32(0));
 T.TestEqual(TEXT("all ordinary pending baselines released before barrier"),Roles[uint8(EBankRole::PendingBaseline)],uint32(0));
 T.TestEqual(TEXT("sole independent foreign guard spans final Account member destruction"),Roles[uint8(EBankRole::ForeignAccount)],uint32(1));
 F.Service->Clear();
 T.TestTrue(TEXT("Clear cannot recycle bank while genuine teardown guard remains"),F.Service->TestImage().bPendingLimits);
 Hooks.Release->Trigger(); Worker.Get(); Joined=true;
 FServiceTestAccess::TeardownHooks(*F.Service,nullptr);
 T.TestTrue(TEXT("actual final guard release never auto-applies limits"),F.Service->TestImage().bPendingLimits);
 F.Service->Clear();
 T.TestFalse(TEXT("later explicit Clear commits after final teardown"),F.Service->TestImage().bPendingLimits);
 T.TestEqual(TEXT("post-teardown explicit commit publishes requested cap"),F.Service->TestImage().EffectiveStore,New.StoreBytes);
 return true;
}

bool PendingCapacityAcrossResume(FAutomationTestBase& T)
{
	FFixture F(T); if (!F.Create() || !F.Start()) return false;
	const uint64 Id = F.AddLive(); F.Tick();
	auto Queue = F.Service->QueueForTest(); auto Sink = F.Service->SinkForTest(); auto List = FVaRestTrace::GetSinks();
	FExcerptHold Held(Queue); int64 HeldCharge = 0;
	const auto GenerationBytes = IndependentGenerationBytes(*F.Service);
	if (!T.TestTrue(TEXT("capacity control independently measures generation components"), GenerationBytes.IsSet()) || !ExhaustWithHeldExcerpt(F, Held, Id, &HeldCharge)) return false;
	const auto Occupied = F.Service->TestImage();
	T.TestEqual(TEXT("capacity control real owner sum independently calculated"), Occupied.PendingDynamic, GenerationBytes.GetValue() + HeldCharge);
	F.Service->Pause();
	T.TestFalse(TEXT("M2 real old generation leaves insufficient common byte capacity for Resume"), F.Service->Start(F.Policy));
	const auto Refused = F.Service->TestImage();
	T.TestTrue(TEXT("M2 independent retained packet plus old baseline survives refused Resume"), Refused.PendingDynamic >= Occupied.PendingDynamic);
	T.TestTrue(TEXT("M2 no temporary fresh full budget can admit overlapping generation"), Refused.PendingTotal <= Refused.EffectivePending);
	T.TestTrue(TEXT("M2 refusal preserves common high water"), Refused.PendingPeak >= Occupied.PendingPeak);
	// This cleanup is also used by a controlled admission-reset mutant. A
	// literal persistent-zero mutant needs Root's separate safe-unwind ruling.
	F.Service->Pause(); Held.Finish(); while (Queue->TryPopExcerpt().IsSet()) {}
	Sink.Reset(); Queue.Reset(); List.Reset();
	T.TestTrue(TEXT("Resume succeeds after actual old payload and baseline free"), F.Service->Start(F.Policy));
	return true;
}

bool ClosedOwnersAfterFacade(FAutomationTestBase& T)
{
	FFixture F(T); if (!F.Create() || !F.Start()) return false;
	const uint64 Id = F.AddLive(); F.Tick();
	auto Queue = F.Service->QueueForTest(); auto Sink = F.Service->SinkForTest();
	auto List = FVaRestTrace::GetSinks();
	FExcerptHold Held(Queue); if (!Held.Start(T, Excerpt(Id, 4096))) return false;
	TWeakPtr<FProductionRootBlock, ESPMode::ThreadSafe> Root = F.Service->RootForTest();
	FRootRetireReceipt* Receipt = F.Service->ReceiptForTest();
	F.Service->Shutdown(); F.Clock.Reset(); F.Service.Reset();
	FBothLedgerImage Before;
	{ auto Owned = Root.Pin(); if (!T.TestTrue(TEXT("old list and admitted worker genuinely retain root after facade destruction"), Owned.IsValid())) return false; Before = Owned->Domain.Get()->Export(); }
	const auto ClosedEvent = Excerpt(Id, 8192);
	{ auto Late = Async(EAsyncExecution::ThreadPool, [S = Sink, ClosedEvent]() mutable { S->OnEvent(ClosedEvent); S.Reset(); }); Late.Get(); }
	{ auto Owned = Root.Pin(); if (!T.TestTrue(TEXT("late closed worker keeps owned context valid"), Owned.IsValid())) return false;
		const auto After = Owned->Domain.Get()->Export();
		T.TestEqual(TEXT("facade-destroyed callback copies no text"), After.ContextProbe.TextBytesCopied, Before.ContextProbe.TextBytesCopied);
		T.TestEqual(TEXT("facade-destroyed callback acquires no excerpt"), After.ContextProbe.ExcerptRefsAcquired, Before.ContextProbe.ExcerptRefsAcquired);
		T.TestEqual(TEXT("facade-destroyed callback has no new store charge"), After.StoreTotal, Before.StoreTotal);
	}
	FBothLedgerImage Freed; T.TestFalse(TEXT("root cannot free while admitted old publisher retained"), Receipt->ReadFreed(Freed));
	Held.Finish(); while (Queue->TryPopExcerpt().IsSet()) {}
	Sink.Reset(); Queue.Reset(); List.Reset();
	T.TestFalse(TEXT("all late owners release root payload"), Root.IsValid());
	T.TestTrue(TEXT("late final owner actual free precedes receipt"), Receipt->ReadFreed(Freed));
	T.TestEqual(TEXT("late owner pending dynamic is zero"), Freed.Pending.Bytes, int64(0));
	T.TestEqual(TEXT("late owner pending events are zero"), Freed.Pending.Count, int64(0));
	T.TestEqual(TEXT("late owner pending ledger retains only root controller"), Freed.PendingTotal, int64(4096));
	T.TestEqual(TEXT("late owner store ledger retains only root controller"), Freed.StoreTotal, int64(4096));
	Receipt = nullptr; Root.Reset(); return true;
}

bool FailureCases(FAutomationTestBase& T)
{
	FFixture F(T); if (!F.Create()) return false;
	const EFactoryFailure Failures[] = {EFactoryFailure::BootstrapCalibration, EFactoryFailure::RootBacking,
		EFactoryFailure::RootController, EFactoryFailure::Lifetime, EFactoryFailure::PendingController,
		EFactoryFailure::ClockController, EFactoryFailure::ServiceController, EFactoryFailure::FixedStorage,
		EFactoryFailure::FrameBinding, EFactoryFailure::PreExitBinding};
	for (const auto Failure : Failures)
	{
		FServiceFactoryWitness Witness; FServiceTestOptions O; O.RefuseBefore = Failure; O.Witness = &Witness;
		auto Sentinel = F.Service;
		T.TestFalse(FString::Printf(TEXT("real pre-operation refusal stage %u"), uint32(Failure)), FService::TryCreateForTest(F.Policy, O, Sentinel));
		T.TestTrue(TEXT("failed factory preserves caller Out"), Sentinel.Get() == F.Service.Get());
		FBothLedgerImage Final;
		if (Witness.bRootCreated)
		{
			T.TestFalse(TEXT("failed factory root has no remaining strong owner"), Witness.Root.IsValid());
			if (T.TestNotNull(TEXT("failed factory exposes actual stored receipt"), Witness.Receipt))
			{
				T.TestTrue(TEXT("failure receipt publishes only after raw root free"), Witness.Receipt->ReadFreed(Final));
				T.TestEqual(TEXT("failed rooted factory retains top weak controller only"), Final.PendingTotal, int64(4096));
				T.TestEqual(TEXT("failed rooted factory BOTH store credit"), Final.StoreTotal, int64(4096));
				T.TestEqual(TEXT("failed rooted factory physical backing really freed"), Final.ContextProbe.LiveBacking, int64(0));
			}
			Witness.Receipt = nullptr; Witness.Root.Reset(); // Actual stored-deleter final weak performs zero checks.
		}
		else
		{
			T.TestTrue(TEXT("pre-root actual unwind captured"), Witness.bBootstrapCaptured);
			Final = Witness.BootstrapFinal;
			T.TestEqual(TEXT("pre-root pending credit returns after physical free"), Final.PendingTotal, int64(0));
			T.TestEqual(TEXT("pre-root BOTH store credit returns"), Final.StoreTotal, int64(0));
			T.TestEqual(TEXT("pre-root physical backing is zero"), Final.ContextProbe.LiveBacking, int64(0));
		}
		const auto List = FVaRestTrace::GetSinks();
		T.TestEqual(TEXT("failed start leaves no registered generation"), List ? List->Num() : 0, 0);
	}
	{
		FServicePtr Attach; FServiceTestOptions O; O.RefuseBefore = EFactoryFailure::Attach;
		if (!T.TestTrue(TEXT("attach refusal still uses real admitted factory"), FService::TryCreateForTest(F.Policy, O, Attach))) return false;
		T.TestFalse(TEXT("real attach refuses before active publication completes"), Attach->Start(F.Policy));
		T.TestFalse(TEXT("attach refusal leaves paused coherent service"), Attach->IsCapturing()); Attach->Shutdown();
	}
	{
		FServicePtr Stage; FServiceTestOptions O; O.RefuseBefore = EFactoryFailure::Staging;
		if (!T.TestTrue(TEXT("staging refusal creates genuine initial service"), FService::TryCreateForTest(F.Policy, O, Stage))) return false;
		const auto Before = Stage->TestImage(); FNormalizedLimits New = F.Policy.Limits; New.StoreBytes /= 2;
		Stage->RequestLimitsForTest(New); Stage->Clear();
		T.TestTrue(TEXT("staging refusal remains visibly pending"), Stage->TestImage().bPendingLimits);
		T.TestEqual(TEXT("staging failure never publishes requested live cap"), Stage->TestImage().EffectiveStore, Before.EffectiveStore);
		T.TestTrue(TEXT("staging failure preserves cumulative highwater"), Stage->TestImage().StorePeak >= Before.StorePeak);
		Stage->Shutdown();
	}
	return true;
}

bool BothLedgerRefusalCases(FAutomationTestBase& T)
{
	FFixture F(T); if (!F.Create()) return false; // Genuine positive factory and nonnull Out sentinel.
	using FRootController = SharedPointerInternals::TReferenceControllerWithDeleter<
		FProductionRootBlock, FRootStoredDeleter, ESPMode::ThreadSafe>;
	struct FCase { const TCHAR* Label; int64 PendingCap, StoreCap, PendingPeak; };
	// These are deliberately below the PUBLIC normalized minima. They exercise
	// the real PRIVATE factory's earliest budget-refusal boundary, with every
	// allocator/type/capability predicate intact, not a reachable public setting.
	const FCase Cases[] = {
		{TEXT("BOTH refusal: first Pending cap"), 4095, F.Policy.Limits.StoreBytes, 0},
		{TEXT("BOTH refusal: second Store cap rolls Pending back"), F.Policy.Limits.PendingBytes, 4095, 4096}};
	bool Good = true;
	for (const auto& Case : Cases)
	{
		T.AddInfo(Case.Label);
		FCapturePolicy Policy = F.Policy;
		Policy.Limits.PendingBytes = Case.PendingCap; Policy.Limits.StoreBytes = Case.StoreCap;
		FServiceFactoryWitness Witness; FServiceTestOptions Options;
		Options.RefuseBefore = EFactoryFailure::None; Options.Witness = &Witness;
		auto Out = F.Service;
		Good = T.TestFalse(TEXT("BOTH actual budget refuses without stage injection"), FService::TryCreateForTest(Policy, Options, Out)) && Good;
		Good = T.TestTrue(TEXT("BOTH refusal preserves real nonnull caller Out"), Out.Get() == F.Service.Get()) && Good;
		if (!T.TestTrue(TEXT("BOTH refusal exposes actual pre-root bootstrap witness"), Witness.bBootstrapCaptured)) return false;
		const FBothLedgerImage& I = Witness.BootstrapFinal;
		const auto& Root = I.Identity.Certificates[uint8(EControllerShape::Root)];
		Good = T.TestTrue(TEXT("BOTH actual Root calibration reached real supported reservation"), Root.Attempted && Root.Failed &&
			Root.Support.Family != FSupportedAllocator::EFamily::Unsupported && Root.Support.bVerifiedModuleNew && Root.Support.bVerifiedNoInflatingProxy) && Good;
		Good = T.TestEqual(TEXT("BOTH refusal records exact compiler Root controller request"), Root.Request, SIZE_T(sizeof(FRootController))) && Good;
		Good = T.TestEqual(TEXT("BOTH refusal records exact compiler Root controller alignment"), Root.TypeAlignment, uint32(alignof(FRootController))) && Good;
		Good = T.TestEqual(TEXT("BOTH refused scratch has no observed capacity"), Root.Capacity, SIZE_T(0)) && Good;
		Good = T.TestEqual(TEXT("BOTH refused scratch has no live certified claim"), Root.CertifiedBound, SIZE_T(0)) && Good;
		Good = T.TestFalse(TEXT("BOTH refusal creates no root owner or receipt"), Witness.bRootCreated || Witness.Root.IsValid() || Witness.Receipt != nullptr) && Good;
		Good = T.TestEqual(TEXT("BOTH Pending total returns to zero"), I.PendingTotal, int64(0)) && Good;
		Good = T.TestEqual(TEXT("BOTH Store total remains zero"), I.StoreTotal, int64(0)) && Good;
		Good = T.TestEqual(TEXT("BOTH Pending peak distinguishes first refusal from second rollback"), I.PendingPeak, Case.PendingPeak) && Good;
		Good = T.TestEqual(TEXT("BOTH Store peak records no admitted Store charge"), I.StorePeak, int64(0)) && Good;
		Good = T.TestEqual(TEXT("BOTH refusal preserves actual Pending cap"), I.Identity.Effective.PendingBytes, Case.PendingCap) && Good;
		Good = T.TestEqual(TEXT("BOTH refusal preserves actual Store cap"), I.Identity.Effective.StoreBytes, Case.StoreCap) && Good;
		Good = T.TestTrue(TEXT("BOTH refusal creates no dynamic account ownership"), I.Pending.Count == 0 && I.Pending.Bytes == 0 && I.Pending.ExcerptReservations == 0 && I.SnapshotControllers == 0) && Good;
		Good = T.TestTrue(TEXT("BOTH reservation failure precedes actual physical allocation"), I.ContextProbe.CalibrationRequest == 0 && I.ContextProbe.CalibrationAlignment == 0 && I.ContextProbe.CalibrationCapacity == 0 &&
			I.ContextProbe.LiveBacking == 0 && I.ContextProbe.PeakBacking == 0 && I.ContextProbe.LiveControllers == 0 && I.ContextProbe.DestroyedControllers == 0 &&
			I.StoreProbe.LiveBacking == 0 && I.StoreProbe.PeakBacking == 0 && I.StoreProbe.LiveControllers == 0) && Good;
		Good = T.TestTrue(TEXT("BOTH failure retains uncalibrated attempted bootstrap"), I.BootstrapAdapter.CalibrationAttempted && !I.BootstrapAdapter.Support.bCalibrationPassed && !I.LifetimeAdapter.CalibrationAttempted) && Good;
		bool Empty = !I.ScratchLive && I.Scratch.IsEmpty() && I.Identity.NextClaimSerial == 0 && I.ConstructedSlotMask == 0 && I.GateWord == FReservationDomain::ClosedBit;
		for (const auto& Claim : I.Claims) Empty = Empty && Claim.IsEmpty();
		for (const auto& Bank : I.Identity.Banks) for (uint32 Role : Bank.Roles) Empty = Empty && Role == 0;
		for (uint32 Role : I.RootRoles) Empty = Empty && Role == 0;
		Good = T.TestTrue(TEXT("BOTH refused reserve leaves no scratch claim slot or owner"), Empty) && Good;
		Good = T.TestTrue(TEXT("BOTH failed factory preserves one ledger identity"), I.Identity.DomainSerial != 0 && I.Identity.PendingLedgerSerial == I.Identity.DomainSerial && I.Identity.StoreLedgerSerial == I.Identity.DomainSerial) && Good;
	}
	return Good;
}

bool BatchBudgets(FAutomationTestBase& T)
{
	FFixture F(T); if (!F.Create() || !F.Start()) return false;
	auto Q = F.Service->QueueForTest();
	for (int32 N = 0; N < 300; ++N) { FVaRestTraceNote Note; Note.RequestId = FVaRestTrace::NewRequestId(); FVaRestTraceEvent E; E.Kind = EVaRestTraceKind::Note; E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceNote>(), Note); if (!T.TestTrue(TEXT("metadata actually queued"), Q->TryPush(E) == EPush::Accepted)) return false; }
	for (int32 N = 0; N < 100; ++N) if (!T.TestTrue(TEXT("excerpt actually queued"), Q->TryPush(Excerpt(FVaRestTrace::NewRequestId())) == EPush::Accepted)) return false;
	auto A = FServiceTestAccess::Pending(*F.Service);
	T.TestEqual(TEXT("both lanes share real count400"), A->Count.load(), int64(400));
	F.Tick(); T.TestEqual(TEXT("one batch never exceeds256 applications"), A->Count.load(), int64(144));
	// Actual remaining lanes discriminate 192 metadata first, then64 excerpts.
	int32 M = 0, X = 0; while (Q->TryPopMetadata().IsSet()) ++M; while (Q->TryPopExcerpt().IsSet()) ++X;
	T.TestEqual(TEXT("metadata receives192 first"), M, 108); T.TestEqual(TEXT("excerpt receives64"), X, 36);
	for (int32 N = 0; N < 300; ++N) if (Q->TryPush(Excerpt(FVaRestTrace::NewRequestId())) != EPush::Accepted) return false;
	F.Tick(); T.TestEqual(TEXT("excerpt borrows unused metadata to256"), A->Count.load(), int64(44)); F.Tick();
	for (int32 N = 0; N < 300; ++N) { FVaRestTraceNote Note; Note.RequestId = FVaRestTrace::NewRequestId(); FVaRestTraceEvent E; E.Kind = EVaRestTraceKind::Note; E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceNote>(), Note); if (Q->TryPush(E) != EPush::Accepted) return false; }
	F.Tick(); T.TestEqual(TEXT("metadata borrows unused excerpt to256"), A->Count.load(), int64(44));
	return true;
}

bool CrossGeneration(FAutomationTestBase& T, bool bOrdinalRotation)
{
	FFixture F(T); if (!F.Create() || !F.Start()) return false;
	for (int32 N = 0; N < 99; ++N) { FVaRestTraceClient C; C.ClientId = 88001; C.DisplayName = TEXT("Lifecycle"); FVaRestTrace::Emit(EVaRestTraceKind::Client, 9, FVaRestTracePayload(TInPlaceType<FVaRestTraceClient>(), C)); }
	F.Tick(); const uint64 R = F.AddLive(); F.Tick(); const auto A = Row(*F.Service, R);
	T.TestEqual(TEXT("A actual Started birth is ordinal100"), A.Birth, uint64(100)); T.TestTrue(TEXT("A Started retains actual nonzero public-emission wire Seq"), A.StartedSeq != 0); T.TestTrue(TEXT("A is live/pinned"), A.Pinned);
	const uint64 L = F.AddLive(); F.Tick();
	if (bOrdinalRotation)
	{
		auto Q = F.Service->QueueForTest(); Q->SetPublishedOrdinalForTest(MAX_uint64);
		FVaRestTraceNote Note; Note.RequestId = L; FVaRestTraceEvent E; E.Kind = EVaRestTraceKind::Note; E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceNote>(), Note);
		T.TestTrue(TEXT("nearMAX actual next publication fails closed"), Q->TryPush(E) == EPush::Dropped); F.Tick();
	}
	else { F.Service->Pause(); F.Service->Start(F.Policy); }
	const auto B = Row(*F.Service, R);
	T.TestEqual(TEXT("B publishes no metadata on attach"), F.Service->QueueForTest()->LastPublishedMetadata(), uint64(0));
	T.TestEqual(TEXT("retained mutable row rebased to B zero"), B.Birth, uint64(0));
	T.TestEqual(TEXT("rebase preserves original arrival"), B.Arrival, A.Arrival);
	T.TestEqual(TEXT("rebase preserves original Started identity"), B.StartedSeq, A.StartedSeq);
	T.TestTrue(TEXT("resume attaches live Partial/pinned"), B.Partial && B.Pinned && Row(*F.Service, L).Pinned);
	FExcerptHold Held(F.Service->QueueForTest()); if (!ExhaustWithHeldExcerpt(F, Held)) return false;
	DroppedCompletion(F, R); const auto Before = F.Service->TestImage();
	F.Tick(); const auto Recovered = F.Service->TestImage();
	T.TestEqual(TEXT("M5 recovery advances BEFORE held publisher release"), Recovered.RecoveryCount, Before.RecoveryCount + 1);
	T.TestEqual(TEXT("A100->B captured recovery watermark is zero"), Recovered.LastRecoveryWatermark, uint64(0));
	UnknownTerminal(T, *F.Service, R, TEXT("A100->B W0"));
	T.TestTrue(TEXT("still-live L remains pinned"), Row(*F.Service, L).Pinned);
	F.Tick(); T.TestFalse(TEXT("quiet tick never repins absent R"), Row(*F.Service, R).Pinned);
	Held.Finish(); T.TestTrue(TEXT("held publisher eventually returns"), Held.Result == EPush::Accepted); F.Tick();
	return true;
}

bool CrossGenerationQueuedEnrichment(FAutomationTestBase& T)
{
 FFixture F(T); if (!F.Create() || !F.Start()) return false;
 for (int32 N=0;N<99;++N)
 {
  FVaRestTraceClient C; C.ClientId=88001; C.DisplayName=TEXT("Lifecycle");
  FVaRestTrace::Emit(EVaRestTraceKind::Client,9,FVaRestTracePayload(TInPlaceType<FVaRestTraceClient>(),C));
 }
 F.Tick(); const uint64 R=F.AddLive(); F.Tick(); const auto Original=Row(*F.Service,R);
 T.TestEqual(TEXT("B-enrichment original A Started birth100"),Original.Birth,uint64(100));
 T.TestTrue(TEXT("B-enrichment original Started Seq is real"),Original.StartedSeq!=0);
 const uint64 L=F.AddLive(); F.Tick();
 F.Service->Pause(); if (!T.TestTrue(TEXT("B-enrichment actual generation restart"),F.Service->Start(F.Policy))) return false;
 const auto Attached=Row(*F.Service,R);
 T.TestEqual(TEXT("B-enrichment attach rebase birth zero"),Attached.Birth,uint64(0));
 T.TestEqual(TEXT("B-enrichment attach publishes no metadata"),F.Service->QueueForTest()->LastPublishedMetadata(),uint64(0));
 FExcerptHold Held(F.Service->QueueForTest()); if (!ExhaustWithHeldExcerpt(F,Held)) return false;
 // Loss is genuine but belongs to another identity: R's actual Completed is
 // successfully queued later, rather than substituted by this dropped event.
 DroppedCompletion(F,FVaRestTrace::NewRequestId()); F.Source->Remove(R);
 FServiceTestAccess::ArmObservedLoss(*F.Service);
 T.TestEqual(TEXT("B-enrichment freezes real loss watermark zero"),FServiceTestAccess::Recovery(*F.Service).Watermark,uint64(0));
 Held.Finish(); auto Packet=F.Service->QueueForTest()->TryPopExcerpt();
 if (!T.TestTrue(TEXT("B-enrichment releases actual held packet before metadata admission"),Packet.IsSet())) return false;
 Packet.Reset();
 for (int32 N=0;N<300;++N)
 {
  FVaRestTraceNote Note; Note.RequestId=L;
  FVaRestTrace::Emit(EVaRestTraceKind::Note,18,FVaRestTracePayload(TInPlaceType<FVaRestTraceNote>(),Note));
 }
 const uint64 BeforeCompleted=F.Service->QueueForTest()->LastPublishedMetadata();
 Emit(Completed(R));
 T.TestEqual(TEXT("B-enrichment actual Completed successfully publishes behind300 metadata"),F.Service->QueueForTest()->LastPublishedMetadata(),BeforeCompleted+1);
 F.Tick();
 T.TestEqual(TEXT("B-enrichment absence recovery uses fixed W0"),F.Service->TestImage().LastRecoveryWatermark,uint64(0));
 UnknownTerminal(T,*F.Service,R,TEXT("A100->B/W0 before queued207"));
 T.TestTrue(TEXT("B-enrichment independent live L stays pinned"),Row(*F.Service,L).Pinned);
 F.Tick(); const auto Actual=Row(*F.Service,R);
 T.TestTrue(TEXT("B-enrichment real queued207 enriches terminal truth"),Actual.Actual && Actual.Terminal && Actual.End==EVaRestInspectorEndState::Success && Actual.Status==207);
 T.TestEqual(TEXT("B-enrichment preserves original Arrival"),Actual.Arrival,Original.Arrival);
 T.TestEqual(TEXT("B-enrichment preserves original Started Seq"),Actual.StartedSeq,Original.StartedSeq);
 int32 Matches=0; F.Service->ForEachRecord([&](const auto& V){ if (V.RequestId==R) ++Matches; });
 T.TestEqual(TEXT("B-enrichment updates exactly one original row"),Matches,1);
 // The genuine Started had no outstanding body promise. Real completion may
 // seal normally; it must not require another synthetic event or completion.
 T.TestTrue(TEXT("B-enrichment normal promise closure seals actual completion"),Actual.Sealed);
 return true;
}

bool FixedWatermarkCases(FFixture& F, uint64 R, TCHAR Case)
{
	auto& T = F.Test;
	const uint64 L = F.AddLive(); F.Tick();
	FExcerptHold FirstHeld(F.Service->QueueForTest());
	if (!ExhaustWithHeldExcerpt(F, FirstHeld)) return false;
	DroppedCompletion(F, Case == TCHAR('d') ? FVaRestTrace::NewRequestId() : R);
	FServiceTestAccess::ArmObservedLoss(*F.Service);
	const uint64 W = FServiceTestAccess::Recovery(*F.Service).Watermark;
	FirstHeld.Finish();
	// A real dequeue/destruction releases the deliberately held upstream test
	// payload before appending metadata. It is not a ledger/counter repair.
	auto OldExcerpt = F.Service->QueueForTest()->TryPopExcerpt();
	if (!T.TestTrue(TEXT("fixed-W real held excerpt is drainable"), OldExcerpt.IsSet())) return false;
	OldExcerpt.Reset();
	if (Case == TCHAR('b'))
	{
		const uint64 Post = F.AddLive();
		FExcerptHold SecondHeld(F.Service->QueueForTest());
		if (!ExhaustWithHeldExcerpt(F, SecondHeld)) return false;
		DroppedCompletion(F, Post); F.Tick();
		T.TestEqual(TEXT("b immutable old W completed before newer loss target"), F.Service->TestImage().LastRecoveryWatermark, W);
		UnknownTerminal(T, *F.Service, R, TEXT("b pre-W absence"));
		const auto Later = Row(*F.Service, Post);
		T.TestTrue(TEXT("b metadata-born post-W row is outside first absence pass"), Later.Found && Later.Birth > W && Later.Pinned && !Later.Actual);
		F.Advance(249000000); F.Tick(); T.TestTrue(TEXT("b249ms newer absent row still awaits own epoch"), Row(*F.Service, Post).Pinned);
		F.Advance(1000000); F.Tick(); UnknownTerminal(T, *F.Service, Post, TEXT("b newer W absence"));
		T.TestTrue(TEXT("b live presence control remains pinned"), Row(*F.Service, L).Pinned);
		SecondHeld.Finish(); F.Tick(); return true;
	}
	const auto Append = [&F, L](int32 Count) {
		for (int32 N = 0; N < Count; ++N)
		{
			FVaRestTraceNote Note; Note.RequestId = L;
			FVaRestTrace::Emit(EVaRestTraceKind::Note, 18, FVaRestTracePayload(TInPlaceType<FVaRestTraceNote>(), Note));
		}
	};
	const auto Identity = Row(*F.Service, R);
	Append(Case == TCHAR('d') ? 300 : 600);
	if (Case == TCHAR('d')) F.Finish(R); // One real successful Completed behind W and >256 queued metadata.
	const uint64 Walks = F.Source->SnapshotWalks;
	F.Tick();
	T.TestEqual(TEXT("M3 fixed-W progress despite successful post-arm append"), F.Service->TestImage().RecoveryCount, uint64(1));
	T.TestEqual(TEXT("M3 actual recovery used original W"), F.Service->TestImage().LastRecoveryWatermark, W);
	T.TestEqual(TEXT("one source snapshot per eligible tick"), F.Source->SnapshotWalks, Walks + 1);
	UnknownTerminal(T, *F.Service, R, TEXT("fixed-W absence before queued completion"));
	T.TestTrue(TEXT("fixed-W live L stays pinned"), Row(*F.Service, L).Pinned);
	if (Case == TCHAR('d'))
	{
		F.Tick(); const auto Actual = Row(*F.Service, R);
		T.TestTrue(TEXT("d genuine queued Completed enriches existing unsealed row"), Actual.Actual && Actual.Terminal && Actual.End == EVaRestInspectorEndState::Success && Actual.Status == 207);
		T.TestEqual(TEXT("d enrichment preserves row arrival identity"), Actual.Arrival, Identity.Arrival);
		T.TestEqual(TEXT("d enrichment preserves original source Started Seq"), Actual.StartedSeq, Identity.StartedSeq);
	}
	else
	{
		for (int32 N = 0; N < 3; ++N) { Append(300); F.Advance(250000000); F.Tick(); }
		T.TestEqual(TEXT("c producers cannot indefinitely extend old watermark"), F.Service->TestImage().LastRecoveryWatermark, W);
		T.TestEqual(TEXT("c successful append alone does not invent another loss recovery"), F.Service->TestImage().RecoveryCount, uint64(1));
	}
	return true;
}

bool OverflowCase(FAutomationTestBase& T, TCHAR Case)
{
	FFixture F(T); if (!F.Create() || !F.Start()) return false;
	const uint64 R = F.AddLive(Case != TCHAR('j') && Case != TCHAR('k') && Case != TCHAR('l') && Case != TCHAR('m'));
	if (Case == TCHAR('j')) { F.Service->Clear(); T.TestTrue(TEXT("j snapshot-born row is pinned"), Row(*F.Service, R).Pinned); }
	if (Case == TCHAR('k'))
	{
		FExcerptHold LostStart(F.Service->QueueForTest());
		if (!ExhaustWithHeldExcerpt(F, LostStart)) return false;
		T.TestTrue(TEXT("k actual Started is dropped before any row exists"), F.Service->QueueForTest()->TryPush(Event(F.Source->Live.Last())) == EPush::Dropped);
		LostStart.Finish(); auto Discarded = F.Service->QueueForTest()->TryPopExcerpt();
		T.TestTrue(TEXT("k actual held payload drains before Ended admission"), Discarded.IsSet()); Discarded.Reset();
	}
	if (Case == TCHAR('k') || Case == TCHAR('m'))
	{
		FVaRestTraceAttemptEnded End; End.RequestId = R; End.Attempt = 1; End.DispatchId = 7; End.Status = 200;
		FVaRestTrace::Emit(EVaRestTraceKind::AttemptEnded, 12, FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptEnded>(), End));
		F.Tick(); T.TestTrue(TEXT("orphan Ended admits real Partial"), Row(*F.Service, R).Found && Row(*F.Service, R).Partial);
	}
	if (Case != TCHAR('a')) F.Tick();
	if (Case == TCHAR('b') || Case == TCHAR('c') || Case == TCHAR('d')) return FixedWatermarkCases(F, R, Case);
	if (Case == TCHAR('h'))
	{
		// Independently bounded row-count pressure: all actual sources remain live.
		for (int32 N = 0; N < F.Policy.Limits.MaxRecords + 4; ++N) F.AddLive();
		F.Tick(4); const auto D = FServiceTestAccess::Diagnostics(*F.Service);
		T.TestTrue(TEXT("h pinned-row pressure visibly refuses Started"), D.RefusedRows > 0);
		T.TestTrue(TEXT("h original live row remains pinned"), Row(*F.Service, R).Pinned);
		T.TestTrue(TEXT("h finite domain remains within admitted store cap"), F.Service->TestImage().StoreTotal <= F.Policy.Limits.StoreBytes);
		return true;
	}
	if (Case == TCHAR('i'))
	{
		F.Source->Remove(R); const auto C = Completed(R, true); FPacketLayout Layout;
		T.TestTrue(TEXT("i actual oversized metadata measures scalar-only"), FPacketCodec::Measure(C, F.Policy, FServiceTestAccess::Allocator(*F.Service), Layout) == EMeasure::ScalarOnly);
		T.TestTrue(TEXT("i real scalar Completed published"), F.Service->QueueForTest()->TryPush(C) == EPush::ScalarOnly);
		F.Tick(); const auto V = Row(*F.Service, R);
		T.TestTrue(TEXT("i scalar fallback resolves correct row with true outcome"), V.Actual && V.Terminal && V.End == EVaRestInspectorEndState::Success && V.Status == 207);
		return true;
	}
	if (Case == TCHAR('m'))
	{
		T.TestTrue(TEXT("m genuine live orphan promoted by source snapshot"), Row(*F.Service, R).Pinned);
		F.Finish(R); F.Tick(); T.TestTrue(TEXT("m later real completion resolves row"), Row(*F.Service, R).Actual);
		return true;
	}
	if (Case == TCHAR('f'))
	{
		FExcerptHold Held(F.Service->QueueForTest()); if (!Held.Start(T, Excerpt(R, 128))) return false;
		T.TestTrue(TEXT("f outstanding admitted boundary reservation"), F.Service->QueueForTest()->OutstandingExcerptReservations() > 0);
		T.TestTrue(TEXT("f another excerpt publishes while first worker held"), F.Service->QueueForTest()->TryPush(Excerpt(FVaRestTrace::NewRequestId())) == EPush::Accepted);
		// Real scalar metadata omission is a new loss, with successful ordinal.
		F.Source->Remove(R); T.TestTrue(TEXT("f actual loss descriptor publishes"), F.Service->QueueForTest()->TryPush(Completed(R, true)) == EPush::ScalarOnly);
		const auto Before = F.Service->TestImage(); F.Tick();
		T.TestEqual(TEXT("f metadata reaches watermark despite held excerpt"), F.Service->TestImage().RecoveryCount, Before.RecoveryCount + 1);
		T.TestTrue(TEXT("f actual completion retained through held excerpt"), Row(*F.Service, R).Actual);
		Held.Finish(); F.Tick(); return true;
	}
	FExcerptHold Held(F.Service->QueueForTest()); if (!ExhaustWithHeldExcerpt(F, Held, Case == TCHAR('l') ? R : 0)) return false;
	if (Case == TCHAR('l')) T.TestTrue(TEXT("l actual Started and every terminal metadata item are lost"), F.Service->QueueForTest()->TryPush(Event(F.Source->Live.Last())) == EPush::Dropped);
	DroppedCompletion(F, R);
	if (Case == TCHAR('g'))
	{
		const uint64 G = F.Service->TestImage().Generation;
		F.Service->Pause(); F.Service->Clear(); F.Service->Start(F.Policy);
		T.TestTrue(TEXT("g control rotation clears armed generation"), F.Service->TestImage().Generation > G);
		Held.Finish(); F.Tick(); T.TestFalse(TEXT("g obsolete row cannot repopulate"), Row(*F.Service, R).Found); return true;
	}
	if (Case == TCHAR('l'))
	{
		F.Source->Remove(R); F.Tick(); const auto Before = F.Service->TestImage().RecoveryCount;
		Held.Finish(); F.Tick(); F.Advance(250000000); F.Tick();
		T.TestTrue(TEXT("l late held orphan rearms without new producer events"), F.Service->TestImage().RecoveryCount > Before);
		UnknownTerminal(T, *F.Service, R, TEXT("l late held excerpt")); return true;
	}
	const uint64 PriorRecoveries = F.Service->TestImage().RecoveryCount;
	if (PriorRecoveries) F.Advance(250000000);
	if (Case == TCHAR('e')) F.Source->DuringNextWalk.Emplace(Completed(FVaRestTrace::NewRequestId()));
	F.Tick();
	UnknownTerminal(T, *F.Service, R, TEXT("overflow absence"));
	const auto First = F.Service->TestImage();
	T.TestEqual(TEXT("at most one source recovery snapshot per tick"), First.RecoveryCount, PriorRecoveries + 1);
	if (Case == TCHAR('e'))
	{
		// The snapshot contributor emitted a second genuine dropped Completed
		// during the first walk. No later producer event is needed to arm it.
		const auto Armed = FServiceTestAccess::Recovery(*F.Service);
		T.TestTrue(TEXT("M4 newer loss is rearmed on the completion tick without another event"), Armed.bPending && Armed.TargetLoss > Armed.HandledLoss);
		F.Tick(); T.TestEqual(TEXT("e immediate quiet tick rate limited"), F.Service->TestImage().RecoveryCount, First.RecoveryCount);
		F.Advance(249000000); F.Tick(); T.TestEqual(TEXT("e249ms no early recovery"), F.Service->TestImage().RecoveryCount, First.RecoveryCount);
		F.Advance(1000000); F.Tick(); T.TestEqual(TEXT("e250ms automatic quiet rearm progresses"), F.Service->TestImage().RecoveryCount, First.RecoveryCount + 1);
	}
	Held.Finish();
	F.Tick();
	return true;
}
} // namespace LifecycleTesting
} // namespace VaRest::Inspector::Private

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorLifecycleTest, "VaRest.Inspector.Capture.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private::LifecycleTesting;
	bool Good = PolicyCases(*this);
	Good = AttachmentAndControls(*this) && Good;
	Good = PauseStatsClosure(*this) && Good;
	Good = ReentrantControls(*this) && Good;
	Good = WeakAndCallbacks(*this) && Good;
	Good = ReadersAndLimits(*this) && Good;
	Good = OldGenerationAndPublisher(*this) && Good;
	Good = PendingCapacityAcrossResume(*this) && Good;
 Good = FinalWeakTeardownExclusion(*this) && Good;
	Good = ClosedOwnersAfterFacade(*this) && Good;
	Good = FailureCases(*this) && Good;
	Good = BothLedgerRefusalCases(*this) && Good;
	Good = QueueActualFrameRemoval(*this) && Good;
	return Good;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorIdlePauseTest, "VaRest.Inspector.Capture.IdlePause",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorIdlePauseTest::RunTest(const FString& Parameters)
{
 return VaRest::Inspector::Private::LifecycleTesting::QueueIdlePause(*this);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorOverflowRecoveryTest, "VaRest.Inspector.Store.OverflowRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorOverflowRecoveryTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private::LifecycleTesting;
	bool Good = BatchBudgets(*this);
	for (TCHAR Case = TCHAR('a'); Case <= TCHAR('m'); ++Case)
	{
		AddInfo(FString::Printf(TEXT("OverflowRecovery owning case %c"), Case));
		Good = OverflowCase(*this, Case) && Good;
	}
	Good = CrossGeneration(*this, false) && Good;
	Good = CrossGeneration(*this, true) && Good;
 Good = CrossGenerationQueuedEnrichment(*this) && Good;
	return Good;
}
#endif
