// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspector.h"

#if WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR

#include "HAL/UnrealMemory.h"
#include "Misc/AutomationTest.h"
#include "Templates/SharedPointerInternals.h"
#include "VaRestClock.h"
#include "VaRestInspectorAllocation.h"
#include "VaRestInspectorBudget.h"
#include "VaRestInspectorPacket.h"
#include "VaRestInspectorQueue.h"
#include "VaRestInspectorStore.h"

namespace VaRest::Inspector::Private::Testing
{
// The accepted QueueTest owns this read-only actual-profile check. It performs
// no calibration or Inspector allocation and never overrides support flags.
bool ReadLiveProfile(FAutomationTestBase& Test);
} // namespace VaRest::Inspector::Private::Testing

namespace VaRest::Inspector::Private::StoreTesting
{
// Bounded fixture exclusion, not the Task11 production root factory. At most
// one context + four pending roots + four budget roots remain alive. One extra
// 4096 allowance covers calibration scratch. Clock/source/framework ownership
// is upstream. No fixture weak root escapes; children drain before these roots.
class FFixtureRoots
{
public:
	explicit FFixtureRoots(FAutomationTestBase& InTest)
		: Test(InTest)
	{
	}
	~FFixtureRoots()
	{
		for (auto& Pending : PendingRoots)
		{
			if (!Pending)
				continue;
			Test.TestEqual(TEXT("fixture pending count drained"), Pending->Count.load(), int64(0));
			Test.TestEqual(TEXT("fixture pending bytes drained"), Pending->Bytes.load(), int64(0));
			Test.TestEqual(TEXT("fixture pending root exclusively retained"), Pending.GetSharedReferenceCount(), 1);
			Pending.Reset();
			StartupUsed.fetch_sub(4096);
		}
		for (auto& Budget : BudgetRoots)
		{
			if (!Budget)
				continue;
			Test.TestEqual(TEXT("fixture budget child credits drained"), Budget->UsedBytes.load(), Budget->BootstrapBytes);
			Test.TestEqual(TEXT("fixture row controllers drained"), Budget->Probe.LiveControllers.load(), int64(0));
			Budget.SafeRelease();
			StartupUsed.fetch_sub(4096);
		}
		if (Lifetime)
		{
			Test.TestEqual(TEXT("fixture all actual backing freed"), Lifetime->Probe.LiveBacking.load(), int64(0));
			Lifetime.SafeRelease();
			StartupUsed.fetch_sub(4096);
		}
		Test.TestEqual(TEXT("fixture root allowances released after free"), StartupUsed.load(), int64(0));
	}
	bool Initialize(bool bCalibrateRows = true)
	{
		if (!Testing::ReadLiveProfile(Test) || !AdmitRoot(sizeof(FAllocationLifetime), alignof(FAllocationLifetime)))
			return false;
		Lifetime = new FAllocationLifetime(GetSupportedAllocator());
		if (!Test.TestTrue(TEXT("visible context allocation within root allowance"), FMemory::GetAllocSize(Lifetime.GetReference()) <= 4096))
			return false;
		if (!bCalibrateRows)
			return true;
		const int64 Before = StartupUsed.load();
		const bool bReady = FStore::CalibrateRows(*Lifetime, StartupUsed, StartupLimit);
		Test.AddInfo(FString::Printf(TEXT("StoreRowCalibration actual: Request=%lld Alignment=%lld Capacity=%lld StartupLimit=%lld"),
			Lifetime->Probe.CalibrationRequest.load(), Lifetime->Probe.CalibrationAlignment.load(),
			Lifetime->Probe.CalibrationCapacity.load(), StartupLimit));
		Test.TestEqual(TEXT("row calibration returns transient allowance after free"), StartupUsed.load(), Before);
		Test.TestEqual(TEXT("row calibration has no surviving backing"), Lifetime->Probe.LiveBacking.load(), int64(0));
		return Test.TestTrue(TEXT("actual row controller calibrated"), bReady);
	}
	TSharedPtr<FPendingAccount, ESPMode::ThreadSafe> NewPending(int64 Count, int64 Bytes)
	{
		if (!Lifetime || PendingCount == 4)
			return {};
		using C = SharedPointerInternals::TIntrusiveReferenceController<FPendingAccount, ESPMode::ThreadSafe>;
		if (!AdmitRoot(sizeof(C), alignof(C)))
			return {};
		auto Root = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(Count, Bytes);
		Root->Lifetime = Lifetime;
		PendingRoots[PendingCount++] = Root;
		return Root;
	}
	TRefCountPtr<FBudgetAccount> NewBudget(int64 Limit)
	{
		if (!Lifetime || BudgetCount == 4 || Limit < 4096)
			return {};
		if (!AdmitRoot(sizeof(FBudgetAccount), alignof(FBudgetAccount)))
			return {};
		TRefCountPtr<FBudgetAccount> Root = new FBudgetAccount(Limit, 4096, Lifetime);
		BudgetRoots[BudgetCount++] = Root;
		if (!Test.TestTrue(TEXT("visible budget allocation within root allowance"), FMemory::GetAllocSize(Root.GetReference()) <= 4096))
			return {};
		return Root;
	}
	FAllocationLifetime& Context() const { return *Lifetime; }

private:
	bool AdmitRoot(SIZE_T Request, uint32 TypeAlignment)
	{
		if (!Test.TestTrue(TEXT("exact fixture root fits audited source path"), Request > 0 && Request <= 1024 && TypeAlignment > 0 && TypeAlignment <= 16))
			return false;
		if (!Test.TestTrue(TEXT("fixture root admitted before allocation"), TryReserve(StartupUsed, StartupLimit, 4096)))
			return false;
		FAllocationCounters ScratchProbe;
		FAllocationAdapter Scratch(ScratchProbe, GetSupportedAllocator());
		const int64 Before = StartupUsed.load();
		const bool bReady = Scratch.Calibrate(StartupUsed, StartupLimit, Request, TypeAlignment);
		const int64 Alignment = Request <= 8 ? 8 : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
		Test.AddInfo(FString::Printf(TEXT("StoreFixtureRootCalibration actual: Request=%lld Alignment=%lld Capacity=%lld StartupUsed=%lld StartupLimit=%lld"),
			ScratchProbe.CalibrationRequest.load(), ScratchProbe.CalibrationAlignment.load(), ScratchProbe.CalibrationCapacity.load(), StartupUsed.load(), StartupLimit));
		Test.TestEqual(TEXT("root calibration uses exact request"), ScratchProbe.CalibrationRequest.load(), int64(Request));
		Test.TestEqual(TEXT("root calibration uses ordinary-new alignment"), ScratchProbe.CalibrationAlignment.load(), Alignment);
		Test.TestEqual(TEXT("root calibration returns scratch credit"), StartupUsed.load(), Before);
		Test.TestEqual(TEXT("root calibration frees scratch backing"), ScratchProbe.LiveBacking.load(), int64(0));
		if (!Test.TestTrue(TEXT("exact root calibration succeeds"), bReady))
		{
			StartupUsed.fetch_sub(4096);
			return false;
		}
		return true;
	}
	static constexpr int64 StartupLimit = 10 * 4096;
	FAutomationTestBase& Test;
	std::atomic<int64> StartupUsed{0};
	TRefCountPtr<FAllocationLifetime> Lifetime;
	TSharedPtr<FPendingAccount, ESPMode::ThreadSafe> PendingRoots[4];
	TRefCountPtr<FBudgetAccount> BudgetRoots[4];
	int32 PendingCount = 0, BudgetCount = 0;
};

static TRefCountPtr<FBudgetAccount> MakeBudgetForTest(FFixtureRoots& Roots, int64 Limit)
{
	return Roots.NewBudget(Limit);
}
static FVaRestTraceEvent Started(uint64 Id)
{
	FVaRestTraceStarted B;
	B.RequestId = Id;
	B.ScopeId = 0;
	B.ClientId = 9;
	B.Verb = TEXT("GET");
	B.Url = TEXT("https://example.test/ordinary");
	B.StartedAt = 10;
	B.StartUtc = FDateTime(2026, 10, 24);
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::Started;
	E.Time = 10;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), MoveTemp(B));
	return E;
}
static bool PushApply(FAutomationTestBase& Test, FCaptureQueue& Queue, FStore& Store, const FVaRestTraceEvent& Event)
{
	if (!Test.TestTrue(TEXT("metadata packet admitted"), Queue.TryPush(Event) == EPush::Accepted))
		return false;
	auto Packet = Queue.TryPopMetadata();
	if (!Test.TestTrue(TEXT("metadata packet dequeued"), Packet.IsSet()))
		return false;
	return Store.Apply(MoveTemp(Packet.GetValue()));
}
static int32 CountRows(const FStore& Store)
{
	int32 Count = 0;
	Store.ForEachRecord([&](const FVaRestInspectorRecordView&) { ++Count; });
	return Count;
}
} // namespace VaRest::Inspector::Private::StoreTesting

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorIdentityAndAttachTest,
	"VaRest.Inspector.Store.IdentityAndAttach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorIdentityAndAttachTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	using namespace VaRest::Inspector::Private::StoreTesting;
	// Clock and source snapshot allocations are upstream test objects.
	auto Clock = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
	FCapturePolicy Policy;
	Policy.Limits.MaxRecords = 8;
	{
		FFixtureRoots Unsupported(*this);
		if (!Unsupported.Initialize(false))
			return false;
		auto Account = MakeBudgetForTest(Unsupported, Policy.Limits.StoreBytes);
		if (!TestTrue(TEXT("uncalibrated account exists"), bool(Account)))
			return false;
		FStore Inert(Policy.Limits, Clock);
		TestFalse(TEXT("unqualified row shape refuses initialization"), Inert.Initialize(Account));
		TestFalse(TEXT("null account refuses initialization"), Inert.Initialize({}));
		int32 Visits = 0;
		TestFalse(TEXT("inert enumeration visibly unavailable"), Inert.ForEachRecord([&](const FVaRestInspectorRecordView&) { ++Visits; }));
		TestEqual(TEXT("inert enumeration invokes no visitor"), Visits, 0);
		FPacket Invalid;
		TestFalse(TEXT("inert apply safely drops invalid packet"), Inert.Apply(MoveTemp(Invalid)));
		TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe> Output = MakeShared<FVaRestInspectorSnapshot, ESPMode::ThreadSafe>();
		const auto Before = Output;
		TestFalse(TEXT("inert snapshot refuses"), Inert.TrySnapshot(Output));
		TestTrue(TEXT("inert snapshot output unchanged"), Output == Before);
		Inert.Clear();
	}
	FFixtureRoots Roots(*this);
	if (!Roots.Initialize())
		return false;
	auto& Allocator = Roots.Context().Allocator;
	auto& Probe = Roots.Context().Probe;
	auto Budget = MakeBudgetForTest(Roots, Policy.Limits.StoreBytes);
	auto Pending = Roots.NewPending(32, 1024 * 1024);
	if (!TestTrue(TEXT("admitted common roots"), bool(Budget) && Pending.IsValid()))
		return false;
	FStore Store(Policy.Limits, Clock);
	if (!TestTrue(TEXT("fixed store initialized"), Store.Initialize(Budget)))
		return false;
	const int64 FixedBytes = Budget->UsedBytes.load();
	const int64 FixedBacking = Probe.LiveBacking.load();
	TestTrue(TEXT("startup fixed allowance bounded"), FixedBytes <= Policy.Limits.StoreBytes / 4);
	TestTrue(TEXT("same-account initialization idempotent"), Store.Initialize(Budget));
	TestEqual(TEXT("idempotent initialization allocates nothing"), Probe.LiveBacking.load(), FixedBacking);
	Store.BeginGeneration(1, 0);
	{
		FCaptureQueue Queue(Policy, 1, Pending.ToSharedRef(), Allocator);
		if (!TestTrue(TEXT("identity queue initialized"), Queue.IsSupported()))
			return false;
		auto Event41 = Started(41);
		if (!TestTrue(TEXT("real Started applied"), PushApply(*this, Queue, Store, Event41)))
			return false;
		uint64 Arrival41 = 0;
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			if (Row.RequestId != 41)
				return;
			Arrival41 = Row.ArrivalOrdinal;
			TestTrue(TEXT("real Started pins"), Row.bPinned);
			TestFalse(TEXT("real Started initially complete history"), Row.bPartial);
			TestEqual(TEXT("Started birth uses consumed ordinal"), Row.BirthMetadataOrdinal, uint64(1));
			TestEqual(TEXT("numeric process scope retained"), Row.ScopeId, uint64(0));
			TestEqual(TEXT("scope zero is Process"), FString(Row.ScopeLabel), FString(TEXT("Process")));
		});
		FVaRestTraceSnapshot Snapshot;
		Snapshot.InFlight.Add(Event41.Payload.Get<FVaRestTraceStarted>());
		const int64 CopiedBefore = Probe.TextBytesCopied.load();
		Store.ApplyAttached(Snapshot, Policy, Pending.ToSharedRef(), Allocator);
		TestEqual(TEXT("snapshot duplicate exactly one row"), CountRows(Store), 1);
		TestEqual(TEXT("snapshot duplicate does not recopy Started"), Probe.TextBytesCopied.load(), CopiedBefore);
		FVaRestTraceEvent Note;
		Note.Kind = EVaRestTraceKind::Note;
		FVaRestTraceNote N;
		N.RequestId = 41;
		N.Attempt = 0;
		Note.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceNote>(), N);
		TestTrue(TEXT("later real metadata uses existing row"), PushApply(*this, Queue, Store, Note));
		TestEqual(TEXT("real event after attach still one row"), CountRows(Store), 1);
		FVaRestTraceStarted Snapshot42;
		Snapshot42.RequestId = 42;
		Snapshot42.ClientId = 10;
		Snapshot42.CurrentAttempt = 3;
		Snapshot42.StartedAt = 100;
		Snapshot.InFlight.Add(Snapshot42);
		Store.ApplyAttached(Snapshot, Policy, Pending.ToSharedRef(), Allocator);
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			if (Row.RequestId == 41)
				TestEqual(TEXT("arrival identity survives duplicate and event"), Row.ArrivalOrdinal, Arrival41);
			if (Row.RequestId != 42)
				return;
			TestTrue(TEXT("snapshot-only row Partial and live"), Row.bPartial && Row.bPinned);
			TestEqual(TEXT("source current attempt preserved"), Row.CurrentAttempt, 3);
			TestTrue(TEXT("source StartedAt known"), Row.bStartedAtKnown);
			TestEqual(TEXT("source StartedAt preserved"), Row.StartedAt, 100.0);
			TestFalse(TEXT("missing historical UTC unknown"), Row.bStartUtcKnown);
			TestFalse(TEXT("missing historical initiator unknown"), Row.bInitiatorKnown);
			TestEqual(TEXT("no prior dispatch invented"), Row.DispatchEvents.Num(), 0);
			TestEqual(TEXT("no prior excerpt invented"), Row.Excerpts.Num(), 0);
			if (TestNotNull(TEXT("snapshot Started retained"), Row.Started.Fixed))
			{
				TestEqual(TEXT("no historical headers invented"), Row.Started.Pairs(Row.Started.Fixed->Headers).Num(), 0);
				TestFalse(TEXT("no historical promise invented"), Row.Started.Fixed->Scalars.bExcerptPending);
			}
		});
		FVaRestTraceEvent Ended;
		Ended.Kind = EVaRestTraceKind::AttemptEnded;
		FVaRestTraceAttemptEnded End;
		End.RequestId = 43;
		End.DispatchId = 7;
		Ended.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptEnded>(), End);
		TestTrue(TEXT("unknown Ended admits Partial base"), PushApply(*this, Queue, Store, Ended));
		FVaRestTraceEvent Completed;
		Completed.Kind = EVaRestTraceKind::Completed;
		FVaRestTraceCompleted C;
		C.RequestId = 44;
		C.Outcome = EVaRestTraceOutcome::Queued;
		Completed.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceCompleted>(), C);
		TestTrue(TEXT("unknown Completed admits terminal Partial"), PushApply(*this, Queue, Store, Completed));
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			if (Row.RequestId == 43)
			{
				TestTrue(TEXT("orphan is partial unpinned without terminal truth"), Row.bPartial && !Row.bPinned && !Row.bTerminalKnown);
				TestTrue(TEXT("orphan outcome unavailable"), Row.EndState == EVaRestInspectorEndState::CaptureInterrupted);
			}
			if (Row.RequestId == 44)
				TestTrue(TEXT("actual Completed immediately terminal"), Row.bPartial && Row.bTerminalKnown && !Row.bPinned && Row.EndState == EVaRestInspectorEndState::Queued);
		});
		const int32 BeforeInvalid = CountRows(Store);
		auto Zero = Started(0);
		TestFalse(TEXT("zero request cannot create row"), PushApply(*this, Queue, Store, Zero));
		auto Unsupported = Started(45);
		Unsupported.Source = EVaRestTraceSource::Socket;
		TestTrue(TEXT("socket rejected before variant access"), Queue.TryPush(Unsupported) == EPush::Unsupported);
		Unsupported.Source = EVaRestTraceSource::Http;
		Unsupported.Kind = EVaRestTraceKind::SocketFrame;
		TestTrue(TEXT("unsupported kind rejected before variant access"), Queue.TryPush(Unsupported) == EPush::Unsupported);
		TestEqual(TEXT("invalid identities do not create rows"), CountRows(Store), BeforeInvalid);
		Store.BeginGeneration(2, 0);
		Store.ApplyAttached(Snapshot, Policy, Pending.ToSharedRef(), Allocator);
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			if (Row.RequestId == 41)
			{
				TestEqual(TEXT("new generation rebases mutable birth"), Row.BirthMetadataOrdinal, uint64(0));
				TestEqual(TEXT("new generation preserves arrival"), Row.ArrivalOrdinal, Arrival41);
				TestTrue(TEXT("source presence remains live"), Row.bPinned);
			}
			if (Row.RequestId == 43)
				TestFalse(TEXT("absent orphan remains unpinned"), Row.bPinned);
			if (Row.RequestId == 44)
				TestTrue(TEXT("terminal truth never revived"), Row.bTerminalKnown && !Row.bPinned);
		});
		Queue.Close();
	}
	Store.Clear();
	TestEqual(TEXT("Clear returns rows to fixed store baseline"), Budget->UsedBytes.load(), FixedBytes);
	TestEqual(TEXT("Clear frees transferred metadata"), Probe.LiveBacking.load(), FixedBacking);
	// Direct borrowed attachment: no owning trace event, queue publication or copy
	// before scalar base/count admission and common pending admission.
	FVaRestTraceSnapshot Direct;
	FVaRestTraceStarted& B = Direct.InFlight.AddDefaulted_GetRef();
	B.RequestId = 77;
	B.Url = TEXT("https://example.test/attached");
	B.StartedAt = 100;
	B.Headers.Set(TEXT("X-Test"), TEXT("visible"));
	FPacketHeader Header;
	Header.bSnapshot = true;
	Header.Time = 100;
	FPacketLayout Layout;
	const int64 BeforeMeasure = Probe.TextBytesCopied.load();
	const int64 BeforeRefs = Probe.ExcerptRefsAcquired.load();
	if (!TestTrue(TEXT("borrowed Started measures full"), FPacketCodec::MeasureStarted(B, Header, Policy, Allocator, Layout) == EMeasure::Full))
		return false;
	TestEqual(TEXT("Started measurement copies zero text"), Probe.TextBytesCopied.load(), BeforeMeasure);
	TestEqual(TEXT("Started measurement takes zero refs"), Probe.ExcerptRefsAcquired.load(), BeforeRefs);
	TestEqual(TEXT("direct charge excludes queue node"), Layout.PendingCharge, int64(Layout.BackingBound));
	auto Baseline = FCaptureQueue::BaselineCharge(Allocator);
	if (!TestTrue(TEXT("direct control baseline available"), Baseline.IsSet()))
		return false;
	auto ShortPending = Roots.NewPending(8, Baseline.GetValue() + Layout.PendingCharge - 1);
	auto ExactPending = Roots.NewPending(8, Baseline.GetValue() + Layout.PendingCharge);
	if (!TestTrue(TEXT("direct pending roots admitted"), ShortPending.IsValid() && ExactPending.IsValid()))
		return false;
	{
		FCaptureQueue ShortQueue(Policy, 2, ShortPending.ToSharedRef(), Allocator);
		FCaptureQueue ExactQueue(Policy, 2, ExactPending.ToSharedRef(), Allocator);
		if (!TestTrue(TEXT("direct control queues available"), ShortQueue.IsSupported() && ExactQueue.IsSupported()))
			return false;
		const uint64 LossBefore = Store.Diagnostics().AttachLosses;
		Store.ApplyAttached(Direct, Policy, ShortPending.ToSharedRef(), Allocator);
		TestEqual(TEXT("byte-short attach keeps scalar base"), CountRows(Store), 1);
		TestEqual(TEXT("byte-short attach records one loss"), Store.Diagnostics().AttachLosses, LossBefore + 1);
		TestEqual(TEXT("byte-short attach copies no text or headers"), Probe.TextBytesCopied.load(), BeforeMeasure);
		TestEqual(TEXT("byte-short attach acquires no excerpt"), Probe.ExcerptRefsAcquired.load(), BeforeRefs);
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			TestTrue(TEXT("byte-short scalar base remains Partial and source-present"), Row.RequestId == 77 && Row.bPartial && Row.bPinned);
			TestNull(TEXT("byte-short scalar base has no owning Started"), Row.Started.Fixed);
		});
		Store.ApplyAttached(Direct, Policy, ExactPending.ToSharedRef(), Allocator);
		TestEqual(TEXT("exact pending capacity fills same base"), CountRows(Store), 1);
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			if (!TestNotNull(TEXT("exact attach retained Started"), Row.Started.Fixed))
				return;
			TestEqual(TEXT("direct URL unchanged"), FString(Row.Started.Text(EText::Url)), FString(TEXT("https://example.test/attached")));
			const auto Pairs = Row.Started.Pairs(Row.Started.Fixed->Headers);
			if (TestEqual(TEXT("exact direct header count"), Pairs.Num(), 1))
			{
				TestEqual(TEXT("direct header key"), FString(Row.Started.Read(Pairs[0].First)), FString(TEXT("X-Test")));
				TestEqual(TEXT("direct header value"), FString(Row.Started.Read(Pairs[0].Second)), FString(TEXT("visible")));
			}
			TestTrue(TEXT("direct snapshot bit"), Row.Started.Fixed->Header.bSnapshot);
			TestEqual(TEXT("direct has no fabricated Seq"), Row.Started.Fixed->Header.Seq, uint64(0));
			TestEqual(TEXT("direct has no publication ordinal"), Row.Started.Fixed->Header.MetadataOrdinal, uint64(0));
			TestEqual(TEXT("public backing is initialized extent"), Row.Started.Backing.Num(), int32(Layout.BackingRequest));
		});
		const int64 AfterExact = Probe.TextBytesCopied.load();
		const int64 BytesAfterExact = Budget->UsedBytes.load();
		Store.ApplyAttached(Direct, Policy, ExactPending.ToSharedRef(), Allocator);
		TestEqual(TEXT("duplicate direct attachment creates no row"), CountRows(Store), 1);
		TestEqual(TEXT("duplicate direct attachment makes no copy"), Probe.TextBytesCopied.load(), AfterExact);
		TestEqual(TEXT("duplicate direct attachment takes no second owner"), Budget->UsedBytes.load(), BytesAfterExact);
		TestEqual(TEXT("attach leaves published ordinal unchanged"), ExactQueue.LastPublishedMetadata(), uint64(0));
		TestFalse(TEXT("attach leaves metadata lane empty"), ExactQueue.TryPopMetadata().IsSet());
		TestFalse(TEXT("attach leaves excerpt lane empty"), ExactQueue.TryPopExcerpt().IsSet());
		Store.Clear();
		TestEqual(TEXT("Clear keeps only exact queue baseline"), ExactPending->Bytes.load(), Baseline.GetValue());
		TestEqual(TEXT("Clear keeps no direct pending count"), ExactPending->Count.load(), int64(0));
		ShortQueue.Close();
		ExactQueue.Close();
	}
	TestEqual(TEXT("source URL unchanged throughout"), B.Url, FString(TEXT("https://example.test/attached")));
	TestEqual(TEXT("source header count unchanged"), B.Headers.Num(), 1);
	TestEqual(TEXT("source header value unchanged"), B.Headers.Get(TEXT("X-Test")), FString(TEXT("visible")));
	TestEqual(TEXT("direct owners freed after Clear"), Probe.LiveBacking.load(), FixedBacking);
	// Count saturation is independent of bytes and has spare fixed slots for the
	// approved count-admission regression to be observed without a crash.
	FCapturePolicy SmallPolicy = Policy;
	SmallPolicy.Limits.MaxRecords = 2;
	auto SmallBudget = MakeBudgetForTest(Roots, SmallPolicy.Limits.StoreBytes);
	if (!TestTrue(TEXT("small-count account admitted"), bool(SmallBudget)))
		return false;
	{
		FStore Small(SmallPolicy.Limits, Clock);
		if (!TestTrue(TEXT("small-count store initialized"), Small.Initialize(SmallBudget)))
			return false;
		Small.BeginGeneration(3, 0);
		FVaRestTraceSnapshot Two;
		Two.InFlight.Add(B);
		Two.InFlight.Last().RequestId = 91;
		Two.InFlight.Add(B);
		Two.InFlight.Last().RequestId = 92;
		Small.ApplyAttached(Two, SmallPolicy, Pending.ToSharedRef(), Allocator);
		if (!TestEqual(TEXT("all live row slots occupied"), CountRows(Small), 2))
			return false;
		const int64 BeforeCountRefusal = Probe.TextBytesCopied.load();
		const int64 BeforeCountBacking = Probe.LiveBacking.load();
		const uint64 RefusedBefore = Small.Diagnostics().RefusedRows;
		Two.InFlight.Add(B);
		Two.InFlight.Last().RequestId = 93;
		Small.ApplyAttached(Two, SmallPolicy, Pending.ToSharedRef(), Allocator);
		TestEqual(TEXT("live row cap refuses N plus one"), CountRows(Small), 2);
		TestEqual(TEXT("count refusal visible"), Small.Diagnostics().RefusedRows, RefusedBefore + 1);
		TestEqual(TEXT("count refusal before text copy"), Probe.TextBytesCopied.load(), BeforeCountRefusal);
		TestEqual(TEXT("count refusal before secondary row allocation"), Probe.LiveBacking.load(), BeforeCountBacking);
		TestTrue(TEXT("count case retains byte slack"), SmallBudget->UsedBytes.load() < SmallBudget->Limit / 2);
		Small.Clear();
		const int64 Spare = SmallBudget->Limit - SmallBudget->UsedBytes.load();
		auto Saturated = FStoreCredit::Acquire(SmallBudget, Spare);
		if (!TestTrue(TEXT("independent byte saturation admitted"), Saturated.IsSet()))
			return false;
		const int64 CopyBeforeBytes = Probe.TextBytesCopied.load();
		Small.ApplyAttached(Direct, SmallPolicy, Pending.ToSharedRef(), Allocator);
		TestEqual(TEXT("base bytes refuse with count slack"), CountRows(Small), 0);
		TestEqual(TEXT("base bytes refuse before copy"), Probe.TextBytesCopied.load(), CopyBeforeBytes);
	}
	TestEqual(TEXT("root shared pending returned to zero"), Pending->Bytes.load(), int64(0));
	TestTrue(TEXT("store byte highwater bounded"), Store.Diagnostics().StorePeakBytes <= Budget->Limit);
	TestTrue(TEXT("pending byte highwater bounded"), Pending->PeakBytes.load() <= Pending->ByteLimit);
	AddInfo(FString::Printf(TEXT("StoreOwned baseline=%lld current=%lld peak=%lld actualBacking=%lld actualPeak=%lld"),
		FixedBytes, Budget->UsedBytes.load(), Store.Diagnostics().StorePeakBytes, Probe.LiveBacking.load(), Probe.PeakBacking.load()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorPacketHandoffTest,
	"VaRest.Inspector.Store.PacketHandoff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorPacketHandoffTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	using namespace VaRest::Inspector::Private::StoreTesting;
	FFixtureRoots Roots(*this);
	if (!Roots.Initialize())
		return false;
	auto& Allocator = Roots.Context().Allocator;
	auto& Probe = Roots.Context().Probe;
	FCapturePolicy Policy;
	FVaRestTraceExcerptEvent Excerpt;
	Excerpt.Owner = EVaRestTraceExcerptOwner::Response;
	Excerpt.RequestId = 77;
	Excerpt.DispatchId = 5;
	Excerpt.Attempt = 1;
	Excerpt.Excerpt.StopReason = TEXT("safe capture sentinel");
	auto Bytes = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
	Bytes->Reserve(2048);
	Bytes->Add(uint8('x'));
	Excerpt.Excerpt.Bytes = Bytes;
	Excerpt.Excerpt.TotalBytes = 1;
	FVaRestTraceEvent Borrowed;
	Borrowed.Kind = EVaRestTraceKind::Excerpt;
	Borrowed.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceExcerptEvent>(), MoveTemp(Excerpt));
	const int32 SourceRefs = Bytes.GetSharedReferenceCount();
	FPacketLayout Layout;
	if (!TestTrue(TEXT("supported full measured layout"), FPacketCodec::Measure(Borrowed, Policy, Allocator, Layout) == EMeasure::Full))
		return false;
	TestEqual(TEXT("measurement copies no text"), Probe.TextBytesCopied.load(), int64(0));
	TestEqual(TEXT("measurement acquires no byte ref"), Probe.ExcerptRefsAcquired.load(), int64(0));
	auto Base = FCaptureQueue::BaselineCharge(Allocator);
	if (!TestTrue(TEXT("supported queue baseline"), Base.IsSet()))
		return false;
	const int64 Baseline = Base.GetValue();
	{
		auto DeniedAccount = Roots.NewPending(8, Baseline + Layout.PendingCharge - 1);
		if (!TestTrue(TEXT("denied control root admitted"), DeniedAccount.IsValid()))
			return false;
		FCaptureQueue Denied(Policy, 1, DeniedAccount.ToSharedRef(), Allocator);
		if (!TestTrue(TEXT("baseline admitted"), Denied.IsSupported()))
			return false;
		const EPush Result = Denied.TryPush(Borrowed);
		TestTrue(TEXT("full excerpt denied"), Result == EPush::ScalarOnly || Result == EPush::Dropped);
		TestEqual(TEXT("denial before excerpt reference"), Probe.ExcerptRefsAcquired.load(), int64(0));
		TestEqual(TEXT("denial before borrowed text copy"), Probe.TextBytesCopied.load(), int64(0));
		TestEqual(TEXT("denial preserves actual source refs"), Bytes.GetSharedReferenceCount(), SourceRefs);
		Denied.Close();
		while (Denied.TryPopExcerpt().IsSet())
		{
		}
	}
	auto Pending = Roots.NewPending(8, Baseline + Layout.PendingCharge);
	if (!TestTrue(TEXT("handoff pending root admitted"), Pending.IsValid()))
		return false;
	{
		FCaptureQueue Queue(Policy, 2, Pending.ToSharedRef(), Allocator);
		if (!TestTrue(TEXT("queue available"), Queue.IsSupported()))
			return false;
		if (!TestTrue(TEXT("exact byte capacity admitted"), Queue.TryPush(Borrowed) == EPush::Accepted))
			return false;
		TestEqual(TEXT("one retained source reference"), Probe.ExcerptRefsAcquired.load(), int64(1));
		auto Dequeued = Queue.TryPopExcerpt();
		if (!TestTrue(TEXT("dequeued actual packet"), Dequeued.IsSet()))
			return false;
		TestEqual(TEXT("dequeued ticket still charged"), Pending->Bytes.load(), Baseline + Layout.PendingCharge);
		TestEqual(TEXT("captured identity"), Dequeued->Fixed().Scalars.DispatchId, uint64(5));
		TestEqual(TEXT("captured borrowed text"), FString(Dequeued->Text(EText::StopReason)), FString(TEXT("safe capture sentinel")));
		auto Needed = FStorePacketOwner::Charge(Dequeued.GetValue(), Allocator);
		if (!TestTrue(TEXT("store bound available"), Needed.IsSet()))
			return false;
		auto Small = MakeBudgetForTest(Roots, 4096 + Needed.GetValue() - 1);
		auto StoreBudget = MakeBudgetForTest(Roots, 4096 + Needed.GetValue());
		if (!TestTrue(TEXT("handoff store roots admitted"), bool(Small) && bool(StoreBudget)))
			return false;
		FStorePacketPtr Stored;
		FPacket Invalid;
		TestFalse(TEXT("invalid packet has no charge"), FStorePacketOwner::Charge(Invalid, Allocator).IsSet());
		TestFalse(TEXT("invalid packet handoff refused"), FStorePacketOwner::TryTake(Invalid, StoreBudget, Allocator, Stored));
		TestFalse(TEXT("null budget refused"), FStorePacketOwner::TryTake(Dequeued.GetValue(), {}, Allocator, Stored));
		FAllocationCounters WrongProbe;
		FAllocationAdapter WrongAdapter(WrongProbe, GetSupportedAllocator());
		TestFalse(TEXT("wrong adapter refused before ownership"), FStorePacketOwner::TryTake(Dequeued.GetValue(), StoreBudget, WrongAdapter, Stored));
		TestFalse(TEXT("store refuses before transfer"), FStorePacketOwner::TryTake(Dequeued.GetValue(), Small, Allocator, Stored));
		TestTrue(TEXT("refusal leaves input valid and output empty"), Dequeued->IsValid() && !Stored.IsValid());
		TestEqual(TEXT("refusal preserves pending owner"), Pending->Bytes.load(), Baseline + Layout.PendingCharge);
		TestEqual(TEXT("refusal preserves store bootstrap"), Small->UsedBytes.load(), int64(4096));
		if (!TestTrue(TEXT("exact store bound admits overlap transfer"), FStorePacketOwner::TryTake(Dequeued.GetValue(), StoreBudget, Allocator, Stored)))
			return false;
		TestEqual(TEXT("overlap actually observed"), Probe.OverlapObservations.load(), int64(1));
		TestEqual(TEXT("pending released after transfer"), Pending->Bytes.load(), Baseline);
		TestEqual(TEXT("store remains charged"), StoreBudget->UsedBytes.load(), int64(4096) + Needed.GetValue());
		Dequeued.Reset();
		if (!TestTrue(TEXT("stored source byte owner exists"), Stored.IsValid() && Stored->Get().Excerpt().IsValid()))
			return false;
		TestEqual(TEXT("stored source byte still alive"), Stored->Get().Excerpt()->Num(), 1);
		TestEqual(TEXT("store carries same real excerpt ref"), Bytes.GetSharedReferenceCount(), SourceRefs + 1);
		TestEqual(TEXT("transferred view exposes initialized extent"), Stored->View().Backing.Num(), int32(Layout.BackingRequest));
		Stored.Reset();
		TestEqual(TEXT("store charge released after actual free"), StoreBudget->UsedBytes.load(), int64(4096));
		TestEqual(TEXT("source refs return after stored free"), Bytes.GetSharedReferenceCount(), SourceRefs);
		if (!TestTrue(TEXT("second admission"), Queue.TryPush(Borrowed) == EPush::Accepted))
			return false;
		auto Held = Queue.TryPopExcerpt();
		if (!TestTrue(TEXT("second packet held"), Held.IsSet()))
			return false;
		TestEqual(TEXT("held ticket retained"), Pending->Bytes.load(), Baseline + Layout.PendingCharge);
		TestTrue(TEXT("pending covers held real backing and source capacity"), Pending->Bytes.load() >= Probe.LiveBacking.load() + int64(Bytes->GetAllocatedSize()));
		Held.Reset();
		TestEqual(TEXT("destruction releases pending"), Pending->Bytes.load(), Baseline);
		Queue.Close();
	}
	TestEqual(TEXT("queue baseline released last"), Pending->Bytes.load(), int64(0));
	TestEqual(TEXT("all consumer backing freed"), Probe.LiveBacking.load(), int64(0));
	TestTrue(TEXT("pending peak stays within limit"), Pending->PeakBytes.load() <= Pending->ByteLimit);
	return true;
}

#endif
