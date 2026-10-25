// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspector.h"

#if WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR

#include "Async/Async.h"
#include "Containers/Array.h"
#include "HAL/UnrealMemory.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Templates/Function.h"
#include "Templates/SharedPointerInternals.h"
#include "VaRestClock.h"
#include "VaRestInspectorAllocation.h"
#include "VaRestInspectorBudget.h"
#include "VaRestInspectorPacket.h"
#include "VaRestInspectorQueue.h"
#include "VaRestInspectorStore.h"

#include <limits>

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

namespace VaRest::Inspector::Private::StoreTesting
{
// Additional Task6 fixtures reuse the accepted finite root topology. Source,
// clocks and test-owned assertion copies are upstream fixture allocations.
static bool PushApplyExpected(FAutomationTestBase& Test, FCaptureQueue& Queue, FStore& Store,
	const FVaRestTraceEvent& Event, EPush Expected, bool bExpectApply = true)
{
	if (!Test.TestTrue(TEXT("exact expected queue admission result"), Queue.TryPush(Event) == Expected))
		return false;
	auto Packet = Event.Kind == EVaRestTraceKind::Excerpt ? Queue.TryPopExcerpt() : Queue.TryPopMetadata();
	if (!Test.TestTrue(TEXT("expected lane contains valid real packet"), Packet.IsSet() && Packet->IsValid()))
		return false;
	if (Expected == EPush::ScalarOnly)
	{
		Test.TestTrue(TEXT("actual scalar packet carries omission flag"), Packet->Fixed().Header.bMetadataOmitted);
		Test.TestEqual(TEXT("scalar packet has no header pairs"), Packet->Headers().Num(), 0);
		Test.TestEqual(TEXT("scalar packet has no warning pairs"), Packet->Warnings().Num(), 0);
		for (int32 Field = 0; Field < static_cast<int32>(EText::Count); ++Field)
			Test.TestTrue(TEXT("scalar packet has no copied text"), Packet->Text(static_cast<EText>(Field)).IsEmpty());
	}
	return Test.TestEqual(TEXT("expected store application result"), Store.Apply(MoveTemp(Packet.GetValue())), bExpectApply);
}

static bool VisitRow(FAutomationTestBase& Test, const FStore& Store, uint64 Id,
	TFunctionRef<void(const FVaRestInspectorRecordView&)> Check)
{
	int32 Found = 0;
	const bool bEnumerated = Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
		if (Row.RequestId == Id)
		{
			++Found;
			Check(Row);
		}
	});
	Test.TestTrue(TEXT("real store enumeration succeeds"), bEnumerated);
	return Test.TestEqual(TEXT("exactly one expected row"), Found, 1);
}

class FNonfiniteReceiptClock final : public IVaRestClock
{
public:
	double Now() const override { return std::numeric_limits<double>::quiet_NaN(); }
	FVaRestTimerHandle SetTimer(double Delay, TUniqueFunction<void()>&& Callback) override
	{
		return Timers.SetTimer(Delay, MoveTemp(Callback));
	}
	void ClearTimer(FVaRestTimerHandle& Handle) override { Timers.ClearTimer(Handle); }
private:
	FVaRestVirtualClock Timers;
};

template <typename F>
static bool WithDiagnosticStore(FAutomationTestBase& Test, const TCHAR* Case, F&& Body, bool bNonfiniteReceipt = false)
{
	Test.AddInfo(Case);
	FFixtureRoots Roots(Test);
	if (!Roots.Initialize())
		return false;
	auto Clock = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
	TSharedRef<IVaRestClock, ESPMode::ThreadSafe> Receipt = Clock;
	if (bNonfiniteReceipt)
		Receipt = MakeShared<FNonfiniteReceiptClock, ESPMode::ThreadSafe>();
	FCapturePolicy Policy;
	Policy.Limits.MaxRecords = 64;
	auto Budget = MakeBudgetForTest(Roots, Policy.Limits.StoreBytes);
	auto Pending = Roots.NewPending(32, 4 * 1024 * 1024);
	if (!Test.TestTrue(TEXT("diagnostic case roots admitted"), bool(Budget) && Pending.IsValid()))
		return false;
	FStore Store(Policy.Limits, Receipt);
	if (!Test.TestTrue(TEXT("diagnostic store initialized"), Store.Initialize(Budget)))
		return false;
	Store.BeginGeneration(1, 0);
	const int64 FixedBytes = Budget->UsedBytes.load();
	const int64 FixedBacking = Roots.Context().Probe.LiveBacking.load();
	bool bResult = false;
	{
		FCaptureQueue Queue(Policy, 1, Pending.ToSharedRef(), Roots.Context().Allocator);
		if (!Test.TestTrue(TEXT("diagnostic queue supported"), Queue.IsSupported()))
			return false;
		ON_SCOPE_EXIT
		{
			Queue.Close();
			while (Queue.TryPopMetadata().IsSet()) {}
			while (Queue.TryPopExcerpt().IsSet()) {}
		};
		bResult = Body(Store, Queue, Budget, Pending, Roots.Context(), *Clock);
	}
	Test.TestEqual(TEXT("diagnostic pending count drained"), Pending->Count.load(), int64(0));
	Test.TestEqual(TEXT("diagnostic queue and packet bytes freed"), Pending->Bytes.load(), int64(0));
	Store.Clear();
	Test.TestEqual(TEXT("diagnostic Clear returns fixed store credit"), Budget->UsedBytes.load(), FixedBytes);
	Test.TestEqual(TEXT("diagnostic Clear frees actual child backing"), Roots.Context().Probe.LiveBacking.load(), FixedBacking);
	Test.TestEqual(TEXT("diagnostic Clear destroys row and header controllers"), Budget->Probe.LiveControllers.load(), int64(0));
	Test.TestTrue(TEXT("diagnostic store peak bounded"), Budget->PeakBytes.load() <= Budget->Limit);
	Test.TestTrue(TEXT("diagnostic pending peak bounded"), Pending->PeakBytes.load() <= Pending->ByteLimit);
	return bResult;
}

static FVaRestTraceEvent Completion(uint64 Id)
{
	FVaRestTraceCompleted Value;
	Value.RequestId = Id;
	Value.Response.Status = 200;
	Value.Response.BodyBytes = 1234;
	Value.Response.Storage = EVaRestTraceResponseStorage::Memory;
	Value.Response.Headers.Set(TEXT("X-Delivered"), TEXT("ordinary ***"));
	Value.Response.EffectiveUrl = TEXT("https://example.test/delivered?token=***");
	Value.Response.ContentType = TEXT("application/json");
	Value.ResultType = TEXT("DiagnosticResult");
	Value.DecodeStartAt = 999998;
	Value.DecodeEndAt = 999999;
	FVaRestTraceEvent Event;
	Event.Kind = EVaRestTraceKind::Completed;
	Event.Seq = 900;
	Event.Time = 1000000;
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceCompleted>(), MoveTemp(Value));
	return Event;
}
static FVaRestTraceEvent NoteEvent(uint64 Id, EVaRestTraceNoteKind Kind, int32 Attempt, uint64 Seq)
{
	FVaRestTraceNote Value;
	Value.RequestId = Id;
	Value.Kind = Kind;
	Value.Attempt = Attempt;
	if (Kind == EVaRestTraceNoteKind::RetryScheduled)
	{
		Value.Reason = TEXT("Status503");
		Value.Delay = 2.5;
	}
	if (Kind == EVaRestTraceNoteKind::CacheStored)
		Value.Bytes = 321;
	if (Kind == EVaRestTraceNoteKind::Queued || Kind == EVaRestTraceNoteKind::QueueExpired)
		Value.QueueId = FString::Printf(TEXT("queue-%llu"), Seq);
	if (Kind == EVaRestTraceNoteKind::Queued)
		Value.Reason = TEXT("Offline");
	FVaRestTraceEvent Event;
	Event.Kind = EVaRestTraceKind::Note;
	Event.Seq = Seq;
	Event.Time = double(Seq) + 0.25;
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceNote>(), MoveTemp(Value));
	return Event;
}
static FVaRestTraceEvent DispatchEvent(uint64 Id, uint64 DispatchId)
{
	FVaRestTraceAttemptDispatched Value;
	Value.RequestId = Id;
	Value.DispatchId = DispatchId;
	Value.Attempt = 1;
	Value.AttemptStart = 11;
	FVaRestTraceEvent Event;
	Event.Kind = EVaRestTraceKind::AttemptDispatched;
	Event.Seq = 20;
	Event.Time = 12;
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptDispatched>(), MoveTemp(Value));
	return Event;
}
static FVaRestTraceEvent EndEvent(uint64 Id, uint64 DispatchId, int32 Status)
{
	FVaRestTraceAttemptEnded Value;
	Value.RequestId = Id;
	Value.DispatchId = DispatchId;
	Value.Attempt = 1;
	Value.Status = Status;
	Value.BytesUp = 17;
	Value.BytesDown = 31;
	Value.bExcerptPending = true;
	Value.UploadDoneAt = 13;
	Value.StatusAt = 14;
	Value.TransportDoneAt = 15;
	Value.EffectiveUrl = TEXT("https://example.test/wire");
	Value.ContentType = TEXT("text/plain");
	Value.Headers.Set(TEXT("X-Wire"), TEXT("physical"));
	FVaRestTraceEvent Event;
	Event.Kind = EVaRestTraceKind::AttemptEnded;
	Event.Seq = 30;
	Event.Time = 15;
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptEnded>(), MoveTemp(Value));
	return Event;
}
static FVaRestTraceEvent ExcerptEvent(uint64 Id, uint64 DispatchId, uint64 Seq, double Time)
{
	FVaRestTraceExcerptEvent Value;
	Value.RequestId = Id;
	Value.DispatchId = DispatchId;
	Value.Owner = DispatchId ? EVaRestTraceExcerptOwner::Response : EVaRestTraceExcerptOwner::Request;
	Value.Attempt = DispatchId ? 1 : 0;
	Value.Excerpt.bRedacted = true;
	Value.Excerpt.TotalBytes = 31;
	auto Bytes = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
	Bytes->Reserve(128);
	Bytes->Add(uint8('x'));
	Value.Excerpt.Bytes = Bytes;
	FVaRestTraceEvent Event;
	Event.Kind = EVaRestTraceKind::Excerpt;
	Event.Seq = Seq;
	Event.Time = Time;
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceExcerptEvent>(), MoveTemp(Value));
	return Event;
}
static void CheckCompletion(FAutomationTestBase& Test, const FVaRestInspectorRecordView& Row,
	const FVaRestTraceEvent& Event, EVaRestInspectorEndState End, double Receipt,
	EVaRestInspectorDetailState Metadata = EVaRestInspectorDetailState::Present)
{
	const auto& Expected = Event.Payload.Get<FVaRestTraceCompleted>();
	Test.TestTrue(TEXT("actual completion is terminal and unpinned"), Row.bTerminalKnown && Row.bActualCompletionObserved && !Row.bPinned);
	Test.TestTrue(TEXT("explicit Outcome maps to named end state"), Row.EndState == End);
	Test.TestTrue(TEXT("explicit completion metadata availability"), Row.CompletedMetadataState == Metadata);
	if (FMath::IsFinite(Receipt))
		Test.TestEqual(TEXT("first observer receipt preserved"), Row.CompletedObservedAt, Receipt);
	else
		Test.TestFalse(TEXT("nonfinite receipt is not invented zero"), FMath::IsFinite(Row.CompletedObservedAt));
	Test.TestEqual(TEXT("dedicated delivered status retained"), Row.FinalStatus, Expected.Response.Status);
	Test.TestEqual(TEXT("delivered body size retained independently"), Row.FinalBodyBytes, Expected.Response.BodyBytes);
	Test.TestTrue(TEXT("delivered storage preserved"), Row.FinalStorage == Expected.Response.Storage);
	Test.TestEqual(TEXT("source completion time preserved"), Row.CompletedAt, Event.Time);
	Test.TestEqual(TEXT("source decode begin preserved"), Row.DecodeStartAt, Expected.DecodeStartAt);
	Test.TestEqual(TEXT("source decode end preserved"), Row.DecodeEndAt, Expected.DecodeEndAt);
	Test.TestTrue(TEXT("final preview uses known cache provenance only"), Row.FinalPreviewState ==
		(Expected.bFromCache ? EVaRestInspectorDetailState::NoProducer : EVaRestInspectorDetailState::Absent));
	if (!Test.TestNotNull(TEXT("completion scalar descriptor always available"), Row.Completed.Fixed))
		return;
	const auto& Fixed = *Row.Completed.Fixed;
	const auto& Actual = Fixed.Scalars;
	Test.TestTrue(TEXT("descriptor remains Completed"), Fixed.Header.Kind == EVaRestTraceKind::Completed);
	Test.TestEqual(TEXT("source completion Seq preserved"), Fixed.Header.Seq, Event.Seq);
	Test.TestEqual(TEXT("descriptor source time preserved"), Fixed.Header.Time, Event.Time);
	Test.TestEqual(TEXT("completion request identity preserved"), Actual.RequestId, Expected.RequestId);
	Test.TestTrue(TEXT("typed outcome retained"), Actual.Outcome == Expected.Outcome);
	Test.TestTrue(TEXT("typed cancel reason retained"), Actual.CancelReason == Expected.CancelReason);
	Test.TestTrue(TEXT("typed error kind retained"), Actual.ErrorKind == Expected.Error.Kind);
	Test.TestEqual(TEXT("error status stays separate"), Actual.ErrorHttpStatus, Expected.Error.HttpStatus);
	Test.TestEqual(TEXT("fixed final response status retained"), Actual.Status, Expected.Response.Status);
	Test.TestEqual(TEXT("fixed delivered bytes retained"), Actual.DeliveredBodyBytes, Expected.Response.BodyBytes);
	Test.TestTrue(TEXT("fixed response storage retained"), Actual.Storage == Expected.Response.Storage);
	Test.TestEqual(TEXT("result FName retained"), Actual.ResultType, Expected.ResultType);
	Test.TestEqual(TEXT("warning total survives optional detail refusal"), Actual.DecodeWarningCount, Expected.DecodeWarningCount);
	Test.TestEqual(TEXT("envelope applied scalar retained"), Actual.bEnvelopeApplied, Expected.Envelope.bApplied);
	Test.TestTrue(TEXT("final cache lookup retained independently"), Actual.CacheLookup == Expected.CacheLookup);
	Test.TestEqual(TEXT("actual from-cache flag retained"), Actual.bFromCache, Expected.bFromCache);
	Test.TestEqual(TEXT("unsent refresh identity retained"), Actual.AuthRefreshId, Expected.AuthRefreshId);
	Test.TestEqual(TEXT("fixed decode begin retained"), Actual.DecodeStartAt, Expected.DecodeStartAt);
	Test.TestEqual(TEXT("fixed decode end retained"), Actual.DecodeEndAt, Expected.DecodeEndAt);
	Test.TestEqual(TEXT("packet metadata flag matches completion availability"), Fixed.Header.bMetadataOmitted,
		Metadata == EVaRestInspectorDetailState::MetadataOmitted);
}
static void CheckNote(FAutomationTestBase& Test, const FVaRestInspectorPacketView& Actual, const FVaRestTraceEvent& Event)
{
	if (!Test.TestNotNull(TEXT("retained note packet exists"), Actual.Fixed))
		return;
	const auto& Expected = Event.Payload.Get<FVaRestTraceNote>();
	const auto& Scalars = Actual.Fixed->Scalars;
	Test.TestTrue(TEXT("six typed note kinds preserved"), Scalars.NoteKind == Expected.Kind);
	Test.TestEqual(TEXT("actual note Attempt including pre-attempt zero"), Scalars.Attempt, Expected.Attempt);
	Test.TestEqual(TEXT("note logical id preserved"), Scalars.RequestId, Expected.RequestId);
	Test.TestEqual(TEXT("note reason FName preserved"), Scalars.Reason, Expected.Reason);
	Test.TestEqual(TEXT("note delay preserved"), Scalars.Delay, Expected.Delay);
	Test.TestEqual(TEXT("note RetryAfter including unknown minus one"), Scalars.RetryAfter, Expected.RetryAfter);
	Test.TestEqual(TEXT("note synchronous bytes preserved"), Scalars.NoteBytes, Expected.Bytes);
	Test.TestEqual(TEXT("note source sequence preserved"), Actual.Fixed->Header.Seq, Event.Seq);
	Test.TestEqual(TEXT("note source time preserved"), Actual.Fixed->Header.Time, Event.Time);
	Test.TestEqual(TEXT("note QueueId preserved"), FString(Actual.Text(EText::QueueId)), Expected.QueueId);
}
} // namespace VaRest::Inspector::Private::StoreTesting

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorCompletionTest,
	"VaRest.Inspector.Store.Completion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorCompletionTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	using namespace VaRest::Inspector::Private::StoreTesting;
	bool bAll = true;
	bAll &= WithDiagnosticStore(*this, TEXT("Completion: named outcomes, cancel and error enums, queue replay"),
		[&](FStore& Store, FCaptureQueue& Queue, auto&, auto&, auto&, FVaRestVirtualClock&) {
			struct FOutcomeCase { EVaRestTraceOutcome Outcome; EVaRestInspectorEndState End; };
			const FOutcomeCase Outcomes[] = {
				{EVaRestTraceOutcome::Success, EVaRestInspectorEndState::Success},
				{EVaRestTraceOutcome::Error, EVaRestInspectorEndState::Error},
				{EVaRestTraceOutcome::Queued, EVaRestInspectorEndState::Queued},
				{EVaRestTraceOutcome::Canceled, EVaRestInspectorEndState::Canceled}};
			uint64 Id = 100;
			for (const auto& Case : Outcomes)
			{
				auto Event = Completion(Id++);
				auto& Value = Event.Payload.Get<FVaRestTraceCompleted>();
				Value.Outcome = Case.Outcome;
				Value.Response.Status = 0; // Outcome remains authoritative even without HTTP.
				Value.Error.Kind = Case.Outcome == EVaRestTraceOutcome::Error ? EVaRestErrorKind::Config :
					Case.Outcome == EVaRestTraceOutcome::Canceled ? EVaRestErrorKind::Canceled : EVaRestErrorKind::None;
				Value.CancelReason = Case.Outcome == EVaRestTraceOutcome::Canceled ? EVaRestCancelReason::User : EVaRestCancelReason::None;
				Value.AuthRefreshId = 707;
				if (!PushApplyExpected(*this, Queue, Store, Event, EPush::Accepted)) return false;
				VisitRow(*this, Store, Value.RequestId, [&](const auto& Row) {
					CheckCompletion(*this, Row, Event, Case.End, 0);
					TestTrue(TEXT("unknown-id completion is Partial"), Row.bPartial);
					TestEqual(TEXT("unsent outcome invents no dispatch"), Row.Dispatches.Num(), 0);
				});
			}
			const EVaRestCancelReason Reasons[] = {EVaRestCancelReason::None, EVaRestCancelReason::User,
				EVaRestCancelReason::OwnerGone, EVaRestCancelReason::Shutdown};
			for (const auto Reason : Reasons)
			{
				auto Event = Completion(Id++);
				auto& Value = Event.Payload.Get<FVaRestTraceCompleted>();
				Value.CancelReason = Reason;
				const bool bCanceled = Reason != EVaRestCancelReason::None;
				Value.Outcome = bCanceled ? EVaRestTraceOutcome::Canceled : EVaRestTraceOutcome::Success;
				Value.Error.Kind = bCanceled ? EVaRestErrorKind::Canceled : EVaRestErrorKind::None;
				if (!PushApplyExpected(*this, Queue, Store, Event, EPush::Accepted)) return false;
				VisitRow(*this, Store, Value.RequestId, [&](const auto& Row) {
					CheckCompletion(*this, Row, Event, bCanceled ? EVaRestInspectorEndState::Canceled : EVaRestInspectorEndState::Success, 0);
				});
			}
			const EVaRestErrorKind Errors[] = {EVaRestErrorKind::None, EVaRestErrorKind::Canceled,
				EVaRestErrorKind::Connection, EVaRestErrorKind::Timeout, EVaRestErrorKind::Http,
				EVaRestErrorKind::Server, EVaRestErrorKind::Decode, EVaRestErrorKind::Auth,
				EVaRestErrorKind::Offline, EVaRestErrorKind::Config};
			for (const auto Kind : Errors)
			{
				auto Event = Completion(Id++);
				auto& Value = Event.Payload.Get<FVaRestTraceCompleted>();
				Value.Error.Kind = Kind;
				Value.Error.HttpStatus = 599;
				Value.Error.Code = TEXT("ERR-***");
				Value.Error.Message = TEXT("diagnostic ***");
				Value.AuthRefreshId = Kind == EVaRestErrorKind::Auth ? 909 : 0;
				Value.Response.Status = Kind == EVaRestErrorKind::Config ? 0 : 200;
				Value.Outcome = Kind == EVaRestErrorKind::None ? EVaRestTraceOutcome::Success :
					Kind == EVaRestErrorKind::Canceled ? EVaRestTraceOutcome::Canceled : EVaRestTraceOutcome::Error;
				Value.CancelReason = Kind == EVaRestErrorKind::Canceled ? EVaRestCancelReason::User : EVaRestCancelReason::None;
				const auto End = Kind == EVaRestErrorKind::None ? EVaRestInspectorEndState::Success :
					Kind == EVaRestErrorKind::Canceled ? EVaRestInspectorEndState::Canceled : EVaRestInspectorEndState::Error;
				if (!PushApplyExpected(*this, Queue, Store, Event, EPush::Accepted)) return false;
				VisitRow(*this, Store, Value.RequestId, [&](const auto& Row) {
					CheckCompletion(*this, Row, Event, End, 0);
					TestEqual(TEXT("error code retained"), FString(Row.Completed.Text(EText::ErrorCode)), Value.Error.Code);
					TestEqual(TEXT("error message retained"), FString(Row.Completed.Text(EText::ErrorMessage)), Value.Error.Message);
					TestEqual(TEXT("Config and unsent auth have no fabricated physical row"), Row.Dispatches.Num(), 0);
				});
			}
			auto QueuedNote = NoteEvent(43, EVaRestTraceNoteKind::Queued, 0, 1);
			if (!PushApplyExpected(*this, Queue, Store, Started(43), EPush::Accepted) ||
				!PushApplyExpected(*this, Queue, Store, QueuedNote, EPush::Accepted)) return false;
			auto Queued = Completion(43);
			auto& QueuedValue = Queued.Payload.Get<FVaRestTraceCompleted>();
			QueuedValue.Outcome = EVaRestTraceOutcome::Queued;
			QueuedValue.Response.Status = 0;
			if (!PushApplyExpected(*this, Queue, Store, Queued, EPush::Accepted)) return false;
			const uint64 ReplayIds[] = {44, 45};
			for (uint64 ReplayId : ReplayIds)
			{
				auto Replay = Started(ReplayId);
				auto& Value = Replay.Payload.Get<FVaRestTraceStarted>();
				Value.QueueReplayOf.QueueId = QueuedNote.Payload.Get<FVaRestTraceNote>().QueueId;
				Value.QueueReplayOf.OriginalRequestId = ReplayId == 44 ? 43 : 0;
				if (!PushApplyExpected(*this, Queue, Store, Replay, EPush::Accepted)) return false;
				VisitRow(*this, Store, ReplayId, [&](const auto& Row) {
					TestTrue(TEXT("replay has distinct nonzero request identity"), Row.RequestId != 0 && Row.RequestId != 43);
					TestTrue(TEXT("replay remains live independently"), Row.bPinned && !Row.bTerminalKnown);
					if (!TestNotNull(TEXT("replay Started retained"), Row.Started.Fixed)) return;
					TestEqual(TEXT("original request zero stays unknown across runs"), Row.Started.Fixed->Scalars.OriginalRequestId, Value.QueueReplayOf.OriginalRequestId);
					TestEqual(TEXT("durable replay QueueId retained"), FString(Row.Started.Text(EText::QueueId)), Value.QueueReplayOf.QueueId);
				});
			}
			VisitRow(*this, Store, 43, [&](const auto& Row) {
				CheckCompletion(*this, Row, Queued, EVaRestInspectorEndState::Queued, 0);
				TestEqual(TEXT("queued logical result has no network dispatch"), Row.Dispatches.Num(), 0);
				if (TestEqual(TEXT("queued reason note retained"), Row.Notes.Num(), 1)) CheckNote(*this, Row.Notes[0], QueuedNote);
			});
			return true;
		});
	bAll &= WithDiagnosticStore(*this, TEXT("Completion: physical304 delivered200, diagnostics, metadata barrier"),
		[&](FStore& Store, FCaptureQueue& Queue, auto&, auto&, auto&, FVaRestVirtualClock& Clock) {
			auto Start = Started(200);
			auto& Begin = Start.Payload.Get<FVaRestTraceStarted>();
			Begin.Initiator.OwnerPath = TEXT("/Game/Owner.***");
			Begin.Initiator.ScriptObject = TEXT("BP_Ordinary***");
			Begin.Initiator.ScriptFunction = TEXT("Fetch***");
			Begin.Initiator.SourceFile = TEXT("Source/Ordinary***.cpp");
			Begin.Initiator.ScriptCodeOffset = 123;
			Begin.Initiator.SourceLine = 456;
			Begin.CacheLookup = EVaRestTraceCacheLookup::Miss;
			Begin.bCacheLookupPending = true;
			Begin.QueueReplayOf.QueueId = TEXT("replay-guid-ordinary");
			Begin.QueueReplayOf.OriginalRequestId = 99;
			auto Note = NoteEvent(200, EVaRestTraceNoteKind::CacheRevalidated, 1, 25);
			if (!PushApplyExpected(*this, Queue, Store, Start, EPush::Accepted) ||
				!PushApplyExpected(*this, Queue, Store, DispatchEvent(200, 7), EPush::Accepted) ||
				!PushApplyExpected(*this, Queue, Store, EndEvent(200, 7, 304), EPush::Accepted) ||
				!PushApplyExpected(*this, Queue, Store, ExcerptEvent(200, 7, 31, 16), EPush::Accepted) ||
				!PushApplyExpected(*this, Queue, Store, Note, EPush::Accepted)) return false;
			auto Event = Completion(200);
			auto& End = Event.Payload.Get<FVaRestTraceCompleted>();
			End.Outcome = EVaRestTraceOutcome::Error;
			End.Error.Kind = EVaRestErrorKind::Server;
			End.Error.HttpStatus = 599;
			End.Error.Code = TEXT("predicate-failed");
			End.Error.Message = TEXT("ordinary *** error");
			End.CacheLookup = EVaRestTraceCacheLookup::Revalidate;
			End.bFromCache = true;
			End.AuthRefreshId = 808;
			End.Envelope.bApplied = true;
			End.Envelope.DataPath = TEXT("data");
			End.Envelope.PredicatePath = TEXT("ok");
			End.Envelope.Expected = TEXT("true");
			End.Envelope.Actual = TEXT("false ***");
			End.Envelope.Code = TEXT("ENV-***");
			End.Envelope.Message = TEXT("envelope ***");
			End.DecodeWarningCount = 40;
			for (int32 Index = 0; Index < 32; ++Index)
			{
				auto& Warning = End.DecodeWarnings.AddDefaulted_GetRef();
				Warning.Path = FString::Printf(TEXT("items[%d].ordinary***"), Index);
				Warning.Message = FString::Printf(TEXT("warning-%d ***"), Index);
			}
			if (!PushApplyExpected(*this, Queue, Store, Event, EPush::Accepted)) return false;
			uint64 RowRevision = 0;
			const uint64 StoreRevision = Store.Revision();
			TArray<FVaRestInspectorTimelineEntry> TimelineBefore;
			VisitRow(*this, Store, 200, [&](const auto& Row) {
				CheckCompletion(*this, Row, Event, EVaRestInspectorEndState::Error, 0);
				RowRevision = Row.Revision;
				TimelineBefore.Append(Row.Timeline.GetData(), Row.Timeline.Num());
				TestEqual(TEXT("304 wire must deliver final200"), Row.FinalStatus, 200);
				TestEqual(TEXT("wire down excludes cached delivered entity"), Row.WireBytesDown, uint64(31));
				TestEqual(TEXT("wire upload remains physical"), Row.WireBytesUp, uint64(17));
				TestTrue(TEXT("source total phase remains known"), Row.Total.bKnown);
				TestEqual(TEXT("total uses source clock not observer"), Row.Total.Seconds, 999990.0);
				TestTrue(TEXT("decode and deliver remain known"), Row.Decode.bKnown && Row.Deliver.bKnown);
				TestEqual(TEXT("source decode duration"), Row.Decode.Seconds, 1.0);
				TestEqual(TEXT("source deliver duration"), Row.Deliver.Seconds, 1.0);
				if (TestEqual(TEXT("one physical dispatch retained"), Row.Dispatches.Num(), 1))
				{
					const auto& Dispatch = Row.Dispatches[0];
					if (TestNotNull(TEXT("physical wire metadata retained"), Dispatch.Ended.Fixed))
						TestEqual(TEXT("physical wire status stays304"), Dispatch.Ended.Fixed->Scalars.Status, 304);
					TestEqual(TEXT("physical304 owns its real excerpt"), Dispatch.ResponseExcerpt.Excerpt.Num(), 1);
					if (Dispatch.ResponseExcerpt.Excerpt.Num() == 1) TestEqual(TEXT("physical excerpt byte retained"), Dispatch.ResponseExcerpt.Excerpt[0], uint8('x'));
				}
				TestTrue(TEXT("cached logical preview explicitly unavailable"), Row.FinalPreviewState == EVaRestInspectorDetailState::NoProducer);
				TestEqual(TEXT("Completed does not duplicate physical excerpt bytes"), Row.Completed.Excerpt.Num(), 0);
				TestTrue(TEXT("initiator was captured"), Row.bInitiatorKnown);
				if (TestNotNull(TEXT("Started owns initiator metadata"), Row.Started.Fixed))
				{
					TestEqual(TEXT("initiator owner path"), FString(Row.Started.Text(EText::OwnerPath)), Begin.Initiator.OwnerPath);
					TestEqual(TEXT("initiator script object"), FString(Row.Started.Text(EText::ScriptObject)), Begin.Initiator.ScriptObject);
					TestEqual(TEXT("initiator script function"), FString(Row.Started.Text(EText::ScriptFunction)), Begin.Initiator.ScriptFunction);
					TestEqual(TEXT("initiator source file"), FString(Row.Started.Text(EText::SourceFile)), Begin.Initiator.SourceFile);
					TestEqual(TEXT("initiator script offset"), Row.Started.Fixed->Scalars.ScriptCodeOffset, 123);
					TestEqual(TEXT("initiator source line"), Row.Started.Fixed->Scalars.SourceLine, 456);
					TestEqual(TEXT("original replay request retained"), Row.Started.Fixed->Scalars.OriginalRequestId, uint64(99));
					TestEqual(TEXT("Started QueueId retained"), FString(Row.Started.Text(EText::QueueId)), Begin.QueueReplayOf.QueueId);
					TestTrue(TEXT("Started lookup distinct from final"), Row.Started.Fixed->Scalars.CacheLookup == EVaRestTraceCacheLookup::Miss);
				}
				if (!TestNotNull(TEXT("full completion metadata retained"), Row.Completed.Fixed)) return;
				const auto Warnings = Row.Completed.Pairs(Row.Completed.Fixed->Warnings);
				if (TestEqual(TEXT("forty warnings retain legal first32 pairs"), Warnings.Num(), 32))
					for (int32 Index = 0; Index < 32; ++Index)
					{
						TestEqual(TEXT("each warning path preserved"), FString(Row.Completed.Read(Warnings[Index].First)), End.DecodeWarnings[Index].Path);
						TestEqual(TEXT("each warning message preserved"), FString(Row.Completed.Read(Warnings[Index].Second)), End.DecodeWarnings[Index].Message);
					}
				const auto Headers = Row.Completed.Pairs(Row.Completed.Fixed->Headers);
				if (TestEqual(TEXT("final headers distinct from physical headers"), Headers.Num(), 1))
				{
					TestEqual(TEXT("final header name"), FString(Row.Completed.Read(Headers[0].First)), FString(TEXT("X-Delivered")));
					TestEqual(TEXT("final header value"), FString(Row.Completed.Read(Headers[0].Second)), FString(TEXT("ordinary ***")));
				}
				TestEqual(TEXT("final effective URL"), FString(Row.Completed.Text(EText::EffectiveUrl)), End.Response.EffectiveUrl);
				TestEqual(TEXT("final content type"), FString(Row.Completed.Text(EText::ResponseContentType)), End.Response.ContentType);
				TestEqual(TEXT("envelope data path"), FString(Row.Completed.Text(EText::DataPath)), End.Envelope.DataPath);
				TestEqual(TEXT("envelope predicate path"), FString(Row.Completed.Text(EText::PredicatePath)), End.Envelope.PredicatePath);
				TestEqual(TEXT("envelope expected"), FString(Row.Completed.Text(EText::Expected)), End.Envelope.Expected);
				TestEqual(TEXT("envelope actual"), FString(Row.Completed.Text(EText::Actual)), End.Envelope.Actual);
				TestEqual(TEXT("envelope code"), FString(Row.Completed.Text(EText::EnvelopeCode)), End.Envelope.Code);
				TestEqual(TEXT("envelope message"), FString(Row.Completed.Text(EText::EnvelopeMessage)), End.Envelope.Message);
			});
			Clock.Advance(7);
			auto Duplicate = Completion(200);
			Duplicate.Time = 1000010;
			Duplicate.Payload.Get<FVaRestTraceCompleted>().Response.Status = 503;
			const FVaRestTraceEvent Later[] = {Duplicate, Started(200), DispatchEvent(200, 8), EndEvent(200, 8, 500),
				NoteEvent(200, EVaRestTraceNoteKind::RetryScheduled, 2, 1001)};
			uint64 Violations = Store.Diagnostics().ContractViolations;
			for (const auto& Rejected : Later)
			{
				if (!PushApplyExpected(*this, Queue, Store, Rejected, EPush::Accepted, false)) return false;
				TestEqual(TEXT("late metadata counted once globally"), Store.Diagnostics().ContractViolations, ++Violations);
				TestEqual(TEXT("late metadata leaves store content revision unchanged"), Store.Revision(), StoreRevision);
				VisitRow(*this, Store, 200, [&](const auto& Row) {
					CheckCompletion(*this, Row, Event, EVaRestInspectorEndState::Error, 0);
					TestEqual(TEXT("duplicate never changes row revision"), Row.Revision, RowRevision);
					if (TestEqual(TEXT("late metadata leaves timeline length unchanged"), Row.Timeline.Num(), TimelineBefore.Num()))
						for (int32 Index = 0; Index < TimelineBefore.Num(); ++Index)
						{
							const auto& Before = TimelineBefore[Index]; const auto& After = Row.Timeline[Index];
							TestTrue(TEXT("late metadata leaves every timeline scalar unchanged"), Before.Kind == After.Kind && Before.Seq == After.Seq &&
								Before.Time == After.Time && Before.Attempt == After.Attempt && Before.DispatchId == After.DispatchId && Before.DetailState == After.DetailState);
						}
					if (TestEqual(TEXT("late metadata leaves note owner count unchanged"), Row.Notes.Num(), 1)) CheckNote(*this, Row.Notes[0], Note);
					TestEqual(TEXT("late metadata cannot replace delivered URL"), FString(Row.Completed.Text(EText::EffectiveUrl)), End.Response.EffectiveUrl);
				});
			}
			return true;
		});

	bAll &= WithDiagnosticStore(*this, TEXT("Completion: actual unresolved excerpt promises prohibit premature sealing"),
		[&](FStore& Store, FCaptureQueue& Queue, auto&, auto&, auto&, FVaRestVirtualClock&) {
			auto Start = Started(250);
			Start.Payload.Get<FVaRestTraceStarted>().bExcerptPending = true;
			auto End = EndEvent(250, 7, 200);
			End.Payload.Get<FVaRestTraceAttemptEnded>().bExcerptPending = true;
			auto Event = Completion(250);
			// Deliberately deliver neither promised request nor response excerpt.
			if (!PushApplyExpected(*this, Queue, Store, Start, EPush::Accepted) ||
				!PushApplyExpected(*this, Queue, Store, DispatchEvent(250, 7), EPush::Accepted) ||
				!PushApplyExpected(*this, Queue, Store, End, EPush::Accepted) ||
				!PushApplyExpected(*this, Queue, Store, Event, EPush::Accepted)) return false;
			VisitRow(*this, Store, 250, [&](const auto& Row) {
				CheckCompletion(*this, Row, Event, EVaRestInspectorEndState::Success, 0);
				TestTrue(TEXT("pending-excerpt completion retains actual terminal truth"), Row.bTerminalKnown && Row.bActualCompletionObserved);
				TestFalse(TEXT("actual unresolved excerpt promises prohibit blanket sealing"), Row.bSealed);
				if (TestNotNull(TEXT("request promise source retained"), Row.Started.Fixed))
					TestTrue(TEXT("Started really announced a pending request excerpt"), Row.Started.Fixed->Scalars.bExcerptPending);
				TestEqual(TEXT("neither promised excerpt has arrived"), Row.Excerpts.Num(), 0);
				if (!TestEqual(TEXT("pending response belongs to one physical dispatch"), Row.Dispatches.Num(), 1)) return;
				const auto& Dispatch = Row.Dispatches[0];
				if (TestNotNull(TEXT("response promise source retained"), Dispatch.Ended.Fixed))
					TestTrue(TEXT("Ended really announced a pending response excerpt"), Dispatch.Ended.Fixed->Scalars.bExcerptPending);
				TestNull(TEXT("physical response excerpt has no owner yet"), Dispatch.ResponseExcerpt.Fixed);
				TestEqual(TEXT("physical response has no excerpt bytes yet"), Dispatch.ResponseExcerpt.Excerpt.Num(), 0);
			});
			return true;
		});
	bAll &= WithDiagnosticStore(*this, TEXT("Completion: cache provenance, unapplied envelope and stream"),
		[&](FStore& Store, FCaptureQueue& Queue, auto&, auto&, auto&, FVaRestVirtualClock&) {
			struct FCacheCase { EVaRestTraceCacheLookup Lookup; bool bFromCache; };
			const FCacheCase Cases[] = {{EVaRestTraceCacheLookup::HitMemory, true}, {EVaRestTraceCacheLookup::HitDisk, true},
				{EVaRestTraceCacheLookup::Stale, true}, {EVaRestTraceCacheLookup::Revalidate, true},
				{EVaRestTraceCacheLookup::HitMemory, false}, {EVaRestTraceCacheLookup::Miss, false}, {EVaRestTraceCacheLookup::None, false}};
			uint64 Id = 300;
			for (const auto& Case : Cases)
			{
				auto Start = Started(Id);
				Start.Payload.Get<FVaRestTraceStarted>().CacheLookup = EVaRestTraceCacheLookup::Miss;
				auto CacheNote = NoteEvent(Id, EVaRestTraceNoteKind::CacheServedStale, 0, 1);
				if (!PushApplyExpected(*this, Queue, Store, Start, EPush::Accepted) ||
					!PushApplyExpected(*this, Queue, Store, CacheNote, EPush::Accepted)) return false;
				auto Event = Completion(Id++);
				auto& Value = Event.Payload.Get<FVaRestTraceCompleted>();
				Value.CacheLookup = Case.Lookup;
				Value.bFromCache = Case.bFromCache;
				if (!PushApplyExpected(*this, Queue, Store, Event, EPush::Accepted)) return false;
				VisitRow(*this, Store, Value.RequestId, [&](const auto& Row) {
					CheckCompletion(*this, Row, Event, EVaRestInspectorEndState::Success, 0);
					TestEqual(TEXT("cache metadata does not invent network details"), Row.Dispatches.Num(), 0);
					if (TestEqual(TEXT("pre-attempt cache note retained"), Row.Notes.Num(), 1)) CheckNote(*this, Row.Notes[0], CacheNote);
				});
			}
			auto ParsedError = Completion(398);
			auto& Parsed = ParsedError.Payload.Get<FVaRestTraceCompleted>();
			Parsed.Outcome = EVaRestTraceOutcome::Error;
			Parsed.Error.Kind = EVaRestErrorKind::Http;
			Parsed.Error.HttpStatus = 599;
			Parsed.Response.Status = 400;
			Parsed.Envelope.bApplied = false;
			Parsed.Envelope.Code = TEXT("parsed-error-code");
			Parsed.Envelope.Message = TEXT("parsed-error-message ***");
			if (!PushApplyExpected(*this, Queue, Store, ParsedError, EPush::Accepted)) return false;
			VisitRow(*this, Store, 398, [&](const auto& Row) {
				CheckCompletion(*this, Row, ParsedError, EVaRestInspectorEndState::Error, 0);
				TestEqual(TEXT("unapplied envelope code remains visible"), FString(Row.Completed.Text(EText::EnvelopeCode)), Parsed.Envelope.Code);
				TestEqual(TEXT("unapplied envelope message remains visible"), FString(Row.Completed.Text(EText::EnvelopeMessage)), Parsed.Envelope.Message);
			});
			auto Stream = Completion(399);
			auto& Value = Stream.Payload.Get<FVaRestTraceCompleted>();
			Value.Response.BodyBytes = -1;
			Value.Response.Storage = EVaRestTraceResponseStorage::Stream;
			if (!PushApplyExpected(*this, Queue, Store, EndEvent(399, 9, 206), EPush::Accepted) ||
				!PushApplyExpected(*this, Queue, Store, ExcerptEvent(399, 9, 4, 16), EPush::Accepted) ||
				!PushApplyExpected(*this, Queue, Store, Stream, EPush::Accepted)) return false;
			VisitRow(*this, Store, 399, [&](const auto& Row) {
				CheckCompletion(*this, Row, Stream, EVaRestInspectorEndState::Success, 0);
				TestEqual(TEXT("stream delivered length is unknown despite wire and excerpt"), Row.FinalBodyBytes, int64(-1));
				TestTrue(TEXT("network final association cannot be guessed from one end"), Row.FinalPreviewState == EVaRestInspectorDetailState::Absent);
			});
			return true;
		});
	bAll &= WithDiagnosticStore(*this, TEXT("Completion: REQUIRED6.1 scalar queue omission"),
		[&](FStore& Store, FCaptureQueue& Queue, auto&, auto& Pending, auto&, FVaRestVirtualClock&) {
			if (!PushApplyExpected(*this, Queue, Store, Started(400), EPush::Accepted)) return false;
			VisitRow(*this, Store, 400, [&](const auto& Row) { TestFalse(TEXT("ScalarOnly regression begins with clean row"), Row.bMetadataOmitted); });
			auto Scalar = Completion(400);
			auto& Value = Scalar.Payload.Get<FVaRestTraceCompleted>();
			Value.Outcome = EVaRestTraceOutcome::Canceled;
			Value.CancelReason = EVaRestCancelReason::OwnerGone;
			Value.Error.Kind = EVaRestErrorKind::Canceled;
			Value.Error.HttpStatus = 418;
			Value.Error.Message = FString::ChrN(FNormalizedLimits::MaxStringChars + 1, TCHAR('x'));
			Value.Response.Status = 201;
			Value.Response.BodyBytes = 7654;
			Value.CacheLookup = EVaRestTraceCacheLookup::HitDisk;
			Value.bFromCache = true;
			Value.AuthRefreshId = 321;
			Value.DecodeWarningCount = 40;
			Value.Envelope.bApplied = true;
			const uint64 UpstreamBefore = Pending->MetadataOmitted.load();
			const uint64 LocalBefore = Store.Diagnostics().MetadataOmitted;
			if (!PushApplyExpected(*this, Queue, Store, Scalar, EPush::ScalarOnly)) return false;
			TestEqual(TEXT("REQUIRED6.1 upstream scalar omission increments exactly once"), Pending->MetadataOmitted.load(), UpstreamBefore + 1);
			TestEqual(TEXT("REQUIRED6.1 successful transfer does not duplicate upstream omission"), Store.Diagnostics().MetadataOmitted, LocalBefore);
			VisitRow(*this, Store, 400, [&](const auto& Row) {
				CheckCompletion(*this, Row, Scalar, EVaRestInspectorEndState::Canceled, 0, EVaRestInspectorDetailState::MetadataOmitted);
				TestTrue(TEXT("REQUIRED6.1 clean row now exposes omission"), Row.bMetadataOmitted);
				TestTrue(TEXT("upstream scalar packet really retained"), Row.Completed.Backing.Num() > 0);
				TestTrue(TEXT("over-cap source error text never appears"), Row.Completed.Text(EText::ErrorMessage).IsEmpty());
			});
			return true;
		});
	bAll &= WithDiagnosticStore(*this, TEXT("Completion: REQUIRED6.1 independent local owner refusal"),
		[&](FStore& Store, FCaptureQueue& Queue, auto& Budget, auto& Pending, FAllocationLifetime& Lifetime, FVaRestVirtualClock& Clock) {
			if (!PushApplyExpected(*this, Queue, Store, Started(401), EPush::Accepted)) return false;
			if (!TestEqual(TEXT("local refusal fixture has exactly its mutation anchor"), CountRows(Store), 1)) return false;
			bool bOnlyStartedAnchor = false;
			if (!VisitRow(*this, Store, 401, [&](const auto& Row) {
				bOnlyStartedAnchor = Row.Kind == EVaRestInspectorRecordKind::Http && Row.Started.Fixed &&
					Row.Started.Fixed->Header.Kind == EVaRestTraceKind::Started && !Row.Started.Fixed->Header.bSnapshot &&
					Row.Started.Fixed->Scalars.RequestId == 401 && Row.bPinned && !Row.bPartial && !Row.bTerminalKnown &&
					!Row.bActualCompletionObserved && !Row.bSealed && Row.EndState == EVaRestInspectorEndState::InFlight &&
					!Row.Completed.Fixed && Row.Dispatches.Num() == 0 && Row.DispatchEvents.Num() == 0 &&
					Row.Notes.Num() == 0 && Row.Excerpts.Num() == 0 && Row.Started.Excerpt.Num() == 0;
			})) return false;
			if (!TestTrue(TEXT("local refusal has only real nonterminal Started and no optional owners"), bOnlyStartedAnchor)) return false;
			if (!TestTrue(TEXT("local refusal preserves genuine record count slack64"), CountRows(Store) < 64)) return false;
			Clock.Advance(3);
			auto Refused = Completion(401);
			auto& Expected = Refused.Payload.Get<FVaRestTraceCompleted>();
			Expected.Outcome = EVaRestTraceOutcome::Error;
			Expected.CancelReason = EVaRestCancelReason::Shutdown;
			Expected.Error.Kind = EVaRestErrorKind::Auth;
			Expected.Error.HttpStatus = 597;
			Expected.Error.Code = TEXT("auth-ordinary");
			Expected.Error.Message = TEXT("optional metadata locally refused");
			Expected.ResultType = TEXT("FallbackResult");
			Expected.Response.Status = 202;
			Expected.Response.BodyBytes = 9876;
			Expected.Response.Storage = EVaRestTraceResponseStorage::Unknown;
			Expected.CacheLookup = EVaRestTraceCacheLookup::Stale;
			Expected.bFromCache = true;
			Expected.AuthRefreshId = 876;
			Expected.DecodeWarningCount = 40;
			Expected.Envelope.bApplied = true;
			Expected.DecodeStartAt = 998;
			Expected.DecodeEndAt = 999;
			const int64 PendingBase = Pending->Bytes.load();
			const int64 BackingBefore = Lifetime.Probe.LiveBacking.load();
			const uint64 LocalRefusalBefore = Store.Diagnostics().MetadataOmitted;
			const uint64 UpstreamRefusalBefore = Pending->MetadataOmitted.load();
			{
				auto Pressure = FStoreCredit::Acquire(Budget, Budget->Limit - Budget->UsedBytes.load());
				if (!TestTrue(TEXT("local completion pressure reserved with row count slack"), Pressure.IsSet() && CountRows(Store) == 1)) return false;
				if (!PushApplyExpected(*this, Queue, Store, Refused, EPush::Accepted)) return false;
				TestEqual(TEXT("local refusal does not invent upstream omission"), Pending->MetadataOmitted.load(), UpstreamRefusalBefore);
				TestEqual(TEXT("local completion ownership refusal counted once"), Store.Diagnostics().MetadataOmitted, LocalRefusalBefore + 1);
				TestEqual(TEXT("local refusal retains no new actual backing"), Lifetime.Probe.LiveBacking.load(), BackingBefore);
				TestEqual(TEXT("local refusal releases packet pending ownership"), Pending->Bytes.load(), PendingBase);
				TestEqual(TEXT("local refusal leaves pending event count zero"), Pending->Count.load(), int64(0));
				VisitRow(*this, Store, 401, [&](const auto& Row) {
					CheckCompletion(*this, Row, Refused, EVaRestInspectorEndState::Error, 3, EVaRestInspectorDetailState::MetadataOmitted);
					TestTrue(TEXT("local refusal is visible on row"), Row.bMetadataOmitted);
					TestEqual(TEXT("scalar fallback has no retained text backing"), Row.Completed.Backing.Num(), 0);
					TestEqual(TEXT("scalar fallback has no excerpt reference"), Row.Completed.Excerpt.Num(), 0);
					if (!TestNotNull(TEXT("local refusal still exposes fixed completion"), Row.Completed.Fixed)) return;
					TestEqual(TEXT("fallback header slice cleared"), Row.Completed.Fixed->Headers.Count, uint32(0));
					TestEqual(TEXT("fallback warning slice cleared"), Row.Completed.Fixed->Warnings.Count, uint32(0));
					TestEqual(TEXT("fallback header offset cleared"), Row.Completed.Fixed->Headers.Offset, uint32(0));
					TestEqual(TEXT("fallback warning offset cleared"), Row.Completed.Fixed->Warnings.Offset, uint32(0));
					for (int32 Field = 0; Field < static_cast<int32>(EText::Count); ++Field)
					{
						TestEqual(TEXT("every fallback text offset cleared"), Row.Completed.Fixed->Text[Field].Offset, uint32(0));
						TestEqual(TEXT("every fallback text length cleared"), Row.Completed.Fixed->Text[Field].Chars, uint32(0));
						TestTrue(TEXT("fallback text read is empty"), Row.Completed.Text(static_cast<EText>(Field)).IsEmpty());
					}
				});
			}
			return true;
		});
	bAll &= WithDiagnosticStore(*this, TEXT("Completion: interrupted source absence and first actual receipt"),
		[&](FStore& Store, FCaptureQueue& Queue, auto&, auto& Pending, FAllocationLifetime& Lifetime, FVaRestVirtualClock& Clock) {
			auto Start = Started(500);
			if (!PushApplyExpected(*this, Queue, Store, Start, EPush::Accepted)) return false;
			uint64 Arrival = 0;
			VisitRow(*this, Store, 500, [&](const auto& Row) { Arrival = Row.ArrivalOrdinal; });
			FCapturePolicy Policy;
			FVaRestTraceSnapshot Absent;
			Store.ApplyAttached(Absent, Policy, Pending.ToSharedRef(), Lifetime.Allocator);
			VisitRow(*this, Store, 500, [&](const auto& Row) {
				TestTrue(TEXT("source absence is interrupted without terminal result"), Row.EndState == EVaRestInspectorEndState::CaptureInterrupted && !Row.bTerminalKnown && !Row.bPinned);
				TestFalse(TEXT("source absence cannot invent actual completion receipt"), Row.bActualCompletionObserved);
				TestNull(TEXT("source absence cannot invent completion descriptor"), Row.Completed.Fixed);
			});
			Clock.Advance(2);
			auto Event = Completion(500);
			if (!PushApplyExpected(*this, Queue, Store, Event, EPush::Accepted)) return false;
			VisitRow(*this, Store, 500, [&](const auto& Row) {
				CheckCompletion(*this, Row, Event, EVaRestInspectorEndState::Success, 2);
				TestEqual(TEXT("first real completion enriches original arrival"), Row.ArrivalOrdinal, Arrival);
			});
			const uint64 Revision = Store.Revision();
			Absent.InFlight.Add(Start.Payload.Get<FVaRestTraceStarted>());
			Store.ApplyAttached(Absent, Policy, Pending.ToSharedRef(), Lifetime.Allocator);
			TestEqual(TEXT("attach cannot revive terminal row or change revision"), Store.Revision(), Revision);
			VisitRow(*this, Store, 500, [&](const auto& Row) { CheckCompletion(*this, Row, Event, EVaRestInspectorEndState::Success, 2); });
			return true;
		});
	bAll &= WithDiagnosticStore(*this, TEXT("Completion: nonfinite observer receipt remains unknown"),
		[&](FStore& Store, FCaptureQueue& Queue, auto&, auto&, auto&, FVaRestVirtualClock&) {
			auto Event = Completion(600);
			if (!PushApplyExpected(*this, Queue, Store, Event, EPush::Accepted)) return false;
			VisitRow(*this, Store, 600, [&](const auto& Row) {
				CheckCompletion(*this, Row, Event, EVaRestInspectorEndState::Success, std::numeric_limits<double>::quiet_NaN());
			});
			return true;
		}, true);
	return bAll;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorNotesTest,
	"VaRest.Inspector.Store.Notes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorNotesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	using namespace VaRest::Inspector::Private::StoreTesting;
	bool bAll = true;
	bAll &= WithDiagnosticStore(*this, TEXT("Notes: six real kinds and actual pre-attempt zero"),
		[&](FStore& Store, FCaptureQueue& Queue, auto&, auto& Pending, auto&, FVaRestVirtualClock&) {
			if (!PushApplyExpected(*this, Queue, Store, Started(700), EPush::Accepted)) return false;
			const EVaRestTraceNoteKind Kinds[] = {EVaRestTraceNoteKind::CacheRevalidated, EVaRestTraceNoteKind::CacheStored,
				EVaRestTraceNoteKind::CacheServedStale, EVaRestTraceNoteKind::RetryScheduled,
				EVaRestTraceNoteKind::Queued, EVaRestTraceNoteKind::QueueExpired};
			TArray<FVaRestTraceEvent> Events;
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(Kinds); ++Index)
			{
				const int32 Attempt = Index == 0 || Index == 1 || Index == 3 ? 2 : 0;
				auto Event = NoteEvent(700, Kinds[Index], Attempt, uint64(10 + Index));
				if (Kinds[Index] == EVaRestTraceNoteKind::RetryScheduled)
					Event.Payload.Get<FVaRestTraceNote>().RetryAfter = 4.5;
				if (!PushApplyExpected(*this, Queue, Store, Event, EPush::Accepted)) return false;
				Events.Add(MoveTemp(Event));
			}
			VisitRow(*this, Store, 700, [&](const auto& Row) {
				if (TestEqual(TEXT("all six note kinds have owners"), Row.Notes.Num(), 6))
					for (int32 Index = 0; Index < 6; ++Index) CheckNote(*this, Row.Notes[Index], Events[Index]);
				TestEqual(TEXT("notes never fabricate physical dispatches"), Row.Dispatches.Num(), 0);
				TestEqual(TEXT("notes never fabricate observed dispatch count"), Row.ObservedDispatches, uint64(0));
				TestEqual(TEXT("retry schedule does not advance actual current attempt"), Row.CurrentAttempt, 0);
				TestTrue(TEXT("notes preserve admitted live pin"), Row.bPinned && !Row.bTerminalKnown);
				TestEqual(TEXT("six-note history has no omissions"), Row.OmittedNotes, uint64(0));
				if (TestEqual(TEXT("Started and six notes have timeline observations"), Row.Timeline.Num(), 7))
					for (int32 Index = 0; Index < 6; ++Index)
					{
						const auto& Entry = Row.Timeline[Index + 1];
						TestTrue(TEXT("note timeline kind and retained detail"), Entry.Kind == EVaRestTraceKind::Note && Entry.DetailState == EVaRestInspectorDetailState::Present);
						TestEqual(TEXT("timeline retains actual note Attempt"), Entry.Attempt, Events[Index].Payload.Get<FVaRestTraceNote>().Attempt);
						TestEqual(TEXT("timeline retains original note Seq"), Entry.Seq, Events[Index].Seq);
						TestEqual(TEXT("timeline retains original note Time"), Entry.Time, Events[Index].Time);
					}
			});
			auto Scalar = NoteEvent(700, EVaRestTraceNoteKind::QueueExpired, 0, 50);
			Scalar.Payload.Get<FVaRestTraceNote>().QueueId = FString::ChrN(FNormalizedLimits::MaxStringChars + 1, TCHAR('q'));
			const uint64 Upstream = Pending->MetadataOmitted.load();
			const uint64 Local = Store.Diagnostics().MetadataOmitted;
			if (!PushApplyExpected(*this, Queue, Store, Scalar, EPush::ScalarOnly)) return false;
			TestEqual(TEXT("scalar note counted once upstream"), Pending->MetadataOmitted.load(), Upstream + 1);
			TestEqual(TEXT("scalar note does not duplicate upstream count locally"), Store.Diagnostics().MetadataOmitted, Local);
			VisitRow(*this, Store, 700, [&](const auto& Row) {
				TestTrue(TEXT("scalar note omission visible on row"), Row.bMetadataOmitted);
				if (!TestEqual(TEXT("scalar note retains an admitted bounded owner"), Row.Notes.Num(), 7)) return;
				const auto& Note = Row.Notes[6];
				if (!TestNotNull(TEXT("scalar note fixed retained"), Note.Fixed)) return;
				TestTrue(TEXT("scalar note kind remains QueueExpired"), Note.Fixed->Scalars.NoteKind == EVaRestTraceNoteKind::QueueExpired);
				TestEqual(TEXT("scalar pre-attempt note remains Attempt0"), Note.Fixed->Scalars.Attempt, 0);
				TestTrue(TEXT("scalar note metadata explicitly omitted"), Note.Fixed->Header.bMetadataOmitted);
				TestTrue(TEXT("omitted QueueId is empty"), Note.Text(EText::QueueId).IsEmpty());
				if (TestEqual(TEXT("scalar note has a timeline observation"), Row.Timeline.Num(), 8))
					TestTrue(TEXT("scalar note timeline declares omission"), Row.Timeline[7].DetailState == EVaRestInspectorDetailState::MetadataOmitted);
			});
			return true;
		});
	bAll &= WithDiagnosticStore(*this, TEXT("Notes: isolated64 count cap, safe timeline invalidation, failed replacement"),
		[&](FStore& Store, FCaptureQueue& Queue, auto& Budget, auto& Pending, FAllocationLifetime& Lifetime, FVaRestVirtualClock&) {
			auto Start = Started(800);
			Start.Seq = 10;
			if (!PushApplyExpected(*this, Queue, Store, Start, EPush::Accepted)) return false;
			for (int32 Index = 0; Index < 65; ++Index)
				if (!PushApplyExpected(*this, Queue, Store, NoteEvent(800, EVaRestTraceNoteKind::Queued, 0, uint64(100 + Index)), EPush::Accepted)) return false;
			TestTrue(TEXT("note count cap exercised with independent byte slack"), Budget->UsedBytes.load() < Budget->Limit / 2);
			VisitRow(*this, Store, 800, [&](const auto& Row) {
				TestTrue(TEXT("oldest-note eviction never evicts pinned base"), Row.bPinned && !Row.bTerminalKnown);
				TestEqual(TEXT("exactly one old note omitted at65"), Row.OmittedNotes, uint64(1));
				TestEqual(TEXT("note cap did not exhaust timeline cap"), Row.OmittedTimeline, uint64(0));
				if (TestEqual(TEXT("exact64 retained note owners"), Row.Notes.Num(), 64))
					for (int32 Index = 0; Index < 64; ++Index)
						CheckNote(*this, Row.Notes[Index], NoteEvent(800, EVaRestTraceNoteKind::Queued, 0, uint64(101 + Index)));
				if (TestEqual(TEXT("all66 timeline observations remain"), Row.Timeline.Num(), 66))
				{
					TestEqual(TEXT("old note timeline keeps original identity"), Row.Timeline[1].Seq, uint64(100));
					TestTrue(TEXT("overwritten owner invalidates surviving timeline detail"), Row.Timeline[1].DetailState == EVaRestInspectorDetailState::Evicted);
					TestTrue(TEXT("new note does not alias old serial"), Row.Timeline[65].DetailState == EVaRestInspectorDetailState::Present);
					TestEqual(TEXT("newest timeline identity belongs to newest owner"), Row.Timeline[65].Seq, uint64(164));
				}
			});
			const int64 BeforeBacking = Lifetime.Probe.LiveBacking.load();
			const int64 BeforePending = Pending->Bytes.load();
			{
				auto Pressure = FStoreCredit::Acquire(Budget, Budget->Limit - Budget->UsedBytes.load());
				if (!TestTrue(TEXT("full-ring replacement byte pressure acquired"), Pressure.IsSet())) return false;
				if (!PushApplyExpected(*this, Queue, Store, NoteEvent(800, EVaRestTraceNoteKind::Queued, 0, 9999), EPush::Accepted)) return false;
				VisitRow(*this, Store, 800, [&](const auto& Row) {
					TestEqual(TEXT("failed replacement counts incoming omission"), Row.OmittedNotes, uint64(2));
					if (TestEqual(TEXT("failed candidate preserves all64 old owners"), Row.Notes.Num(), 64))
						for (int32 Index = 0; Index < 64; ++Index)
							CheckNote(*this, Row.Notes[Index], NoteEvent(800, EVaRestTraceNoteKind::Queued, 0, uint64(101 + Index)));
					if (TestEqual(TEXT("failed candidate still has scalar timeline observation"), Row.Timeline.Num(), 67))
					{
						TestTrue(TEXT("failed replacement cannot invalidate surviving oldest owner"), Row.Timeline[2].DetailState == EVaRestInspectorDetailState::Present);
						TestTrue(TEXT("failed candidate timeline explicitly unavailable"), Row.Timeline[66].DetailState == EVaRestInspectorDetailState::MetadataOmitted);
					}
				});
				TestEqual(TEXT("failed candidate frees its pending ownership"), Pending->Bytes.load(), BeforePending);
				TestEqual(TEXT("failed replacement has no leaked real backing"), Lifetime.Probe.LiveBacking.load(), BeforeBacking);
			}
			return true;
		});
	bAll &= WithDiagnosticStore(*this, TEXT("Notes: isolated128 timeline count and delayed worker source order"),
		[&](FStore& Store, FCaptureQueue& Queue, auto& Budget, auto&, auto&, FVaRestVirtualClock& Clock) {
			// Accepted same-identity Started observations exercise the independent
			// scalar history cap without filling the note or physical-slot caps.
			for (uint64 Index = 0; Index < 129; ++Index)
			{
				auto Event = Started(900);
				Event.Seq = 1000 + Index;
				Event.Time = 100 + double(Index);
				if (!PushApplyExpected(*this, Queue, Store, Event, EPush::Accepted)) return false;
			}
			VisitRow(*this, Store, 900, [&](const auto& Row) {
				TestEqual(TEXT("timeline count limit is128"), Row.Timeline.Num(), 128);
				TestEqual(TEXT("129 accepted observations omit one oldest scalar"), Row.OmittedTimeline, uint64(1));
				TestEqual(TEXT("timeline cap independent of note count"), Row.Notes.Num(), 0);
				TestEqual(TEXT("timeline cap causes no note omission"), Row.OmittedNotes, uint64(0));
				if (Row.Timeline.Num() == 128)
					for (int32 Index = 0; Index < 128; ++Index)
					{
						TestEqual(TEXT("timeline FIFO keeps raw source Seq"), Row.Timeline[Index].Seq, uint64(1001 + Index));
						TestEqual(TEXT("timeline FIFO keeps raw source time"), Row.Timeline[Index].Time, 101.0 + Index);
					}
			});
			TestTrue(TEXT("timeline count control has byte slack"), Budget->UsedBytes.load() < Budget->Limit / 2);
			Clock.Advance(5000);
			auto WorkerExcerpt = ExcerptEvent(900, 0, 5, 2.5);
			auto Published = Async(EAsyncExecution::ThreadPool, [&Queue, &WorkerExcerpt] { return Queue.TryPush(WorkerExcerpt); });
			const EPush WorkerResult = Published.Get(); // Owned worker finished before any return or queue destruction.
			if (!TestTrue(TEXT("real worker excerpt admitted"), WorkerResult == EPush::Accepted)) return false;
			auto Metadata = Started(900);
			Metadata.Seq = 4000;
			Metadata.Time = 500;
			if (!PushApplyExpected(*this, Queue, Store, Metadata, EPush::Accepted)) return false;
			auto Packet = Queue.TryPopExcerpt();
			if (!TestTrue(TEXT("earlier source worker packet still in excerpt lane"), Packet.IsSet() && Packet->IsValid())) return false;
			if (!TestTrue(TEXT("worker packet applied after newer metadata"), Store.Apply(MoveTemp(Packet.GetValue())))) return false;
			VisitRow(*this, Store, 900, [&](const auto& Row) {
				TestEqual(TEXT("131 observations retain128"), Row.OmittedTimeline, uint64(3));
				if (!TestEqual(TEXT("timeline remains bounded after delayed excerpt"), Row.Timeline.Num(), 128)) return;
				TestEqual(TEXT("FIFO first retained observation"), Row.Timeline[0].Seq, uint64(1003));
				TestEqual(TEXT("newer metadata precedes delayed excerpt by receipt"), Row.Timeline[126].Seq, uint64(4000));
				const auto& Last = Row.Timeline[127];
				TestTrue(TEXT("last receipt is the actual excerpt"), Last.Kind == EVaRestTraceKind::Excerpt);
				TestEqual(TEXT("delayed worker keeps smaller source Seq"), Last.Seq, uint64(5));
				TestEqual(TEXT("delayed worker keeps source time instead of observer5000"), Last.Time, 2.5);
				TestEqual(TEXT("request excerpt keeps actual Attempt0"), Last.Attempt, 0);
				TestEqual(TEXT("request excerpt keeps DispatchId0"), Last.DispatchId, uint64(0));
				TestEqual(TEXT("no physical dispatch fabricated for request excerpt"), Row.Dispatches.Num(), 0);
			});
			return true;
		});
	bAll &= WithDiagnosticStore(*this, TEXT("Notes: first ring and later owner byte refusal with count slack"),
		[&](FStore& Store, FCaptureQueue& Queue, auto& Budget, auto& Pending, FAllocationLifetime& Lifetime, FVaRestVirtualClock&) {
			if (!PushApplyExpected(*this, Queue, Store, Started(1000), EPush::Accepted)) return false;
			const int64 BeforeRingBacking = Lifetime.Probe.LiveBacking.load();
			const int64 BeforePending = Pending->Bytes.load();
			const int64 BeforeRingBytes = Budget->UsedBytes.load();
			{
				auto Pressure = FStoreCredit::Acquire(Budget, Budget->Limit - Budget->UsedBytes.load());
				if (!TestTrue(TEXT("first note ring pressure admitted"), Pressure.IsSet())) return false;
				if (!PushApplyExpected(*this, Queue, Store, NoteEvent(1000, EVaRestTraceNoteKind::Queued, 0, 1), EPush::Accepted)) return false;
				VisitRow(*this, Store, 1000, [&](const auto& Row) {
					TestEqual(TEXT("byte refusal before first note ring publishes no note"), Row.Notes.Num(), 0);
					TestEqual(TEXT("first ring refusal visibly omits one note"), Row.OmittedNotes, uint64(1));
					TestTrue(TEXT("first ring refusal preserves pinned base and marks loss"), Row.bPinned && Row.bMetadataOmitted);
				});
				TestEqual(TEXT("first ring refuses without hidden allocation"), Lifetime.Probe.LiveBacking.load(), BeforeRingBacking);
				TestEqual(TEXT("first ring refusal drains pending bytes"), Pending->Bytes.load(), BeforePending);
			}
			TestEqual(TEXT("first ring refusal leaves store bytes unchanged"), Budget->UsedBytes.load(), BeforeRingBytes);
			auto Kept = NoteEvent(1000, EVaRestTraceNoteKind::Queued, 0, 2);
			if (!PushApplyExpected(*this, Queue, Store, Kept, EPush::Accepted)) return false;
			const int64 BeforeOwnerBytes = Budget->UsedBytes.load();
			const int64 BeforeOwnerBacking = Lifetime.Probe.LiveBacking.load();
			const uint64 LocalBefore = Store.Diagnostics().MetadataOmitted;
			{
				auto Pressure = FStoreCredit::Acquire(Budget, Budget->Limit - Budget->UsedBytes.load());
				if (!TestTrue(TEXT("existing-ring owner pressure admitted"), Pressure.IsSet())) return false;
				if (!PushApplyExpected(*this, Queue, Store, NoteEvent(1000, EVaRestTraceNoteKind::QueueExpired, 0, 3), EPush::Accepted)) return false;
				VisitRow(*this, Store, 1000, [&](const auto& Row) {
					if (TestEqual(TEXT("owner byte refusal has63 note slots slack"), Row.Notes.Num(), 1)) CheckNote(*this, Row.Notes[0], Kept);
					TestEqual(TEXT("independent second byte refusal visible"), Row.OmittedNotes, uint64(2));
					TestTrue(TEXT("owner pressure cannot evict pinned base"), Row.bPinned);
					if (TestEqual(TEXT("all four accepted source observations retained"), Row.Timeline.Num(), 4))
					{
						TestTrue(TEXT("retained old note timeline remains valid"), Row.Timeline[2].DetailState == EVaRestInspectorDetailState::Present);
						TestTrue(TEXT("incoming refused note timeline declares omission"), Row.Timeline[3].DetailState == EVaRestInspectorDetailState::MetadataOmitted);
					}
				});
				TestEqual(TEXT("local note owner refusal counted once"), Store.Diagnostics().MetadataOmitted, LocalBefore + 1);
				TestEqual(TEXT("owner refusal leaves old actual backing intact"), Lifetime.Probe.LiveBacking.load(), BeforeOwnerBacking);
				TestEqual(TEXT("owner refusal releases pending packet"), Pending->Bytes.load(), BeforePending);
				TestEqual(TEXT("note pressure has real row count slack"), CountRows(Store), 1);
			}
			TestEqual(TEXT("allocated ring remains honestly charged after refusal"), Budget->UsedBytes.load(), BeforeOwnerBytes);
			return true;
		});
	bAll &= WithDiagnosticStore(*this, TEXT("Notes: lazy timeline byte refusal and later recovery"),
		[&](FStore& Store, FCaptureQueue& Queue, auto& Budget, auto& Pending, FAllocationLifetime& Lifetime, FVaRestVirtualClock&) {
			// A real direct-attach admission with pending bytes unavailable leaves
			// a scalar source-present base, without an accepted packet observation.
			FCapturePolicy Policy;
			FVaRestTraceSnapshot Snapshot;
			Snapshot.InFlight.Add(Started(1050).Payload.Get<FVaRestTraceStarted>());
			{
				auto PendingPressure = FPendingBaseline::Acquire(Pending.ToSharedRef(), Pending->ByteLimit - Pending->Bytes.load());
				if (!TestTrue(TEXT("direct attach pending refusal reserved"), PendingPressure.IsSet())) return false;
				Store.ApplyAttached(Snapshot, Policy, Pending.ToSharedRef(), Lifetime.Allocator);
			}
			VisitRow(*this, Store, 1050, [&](const auto& Row) {
				TestTrue(TEXT("source-present scalar base admitted"), Row.bPartial && Row.bPinned);
				TestNull(TEXT("pending refusal copied no Started owner"), Row.Started.Fixed);
				TestEqual(TEXT("no packet observation invented by failed direct attach"), Row.Timeline.Num(), 0);
			});
			const int64 BeforeBacking = Lifetime.Probe.LiveBacking.load();
			const int64 BeforeBytes = Budget->UsedBytes.load();
			{
				auto Pressure = FStoreCredit::Acquire(Budget, Budget->Limit - BeforeBytes);
				if (!TestTrue(TEXT("lazy rings byte pressure admitted with count slack"), Pressure.IsSet() && CountRows(Store) == 1)) return false;
				if (!PushApplyExpected(*this, Queue, Store, NoteEvent(1050, EVaRestTraceNoteKind::Queued, 0, 1), EPush::Accepted)) return false;
				VisitRow(*this, Store, 1050, [&](const auto& Row) {
					TestEqual(TEXT("no uncredited timeline block allocated"), Row.Timeline.Num(), 0);
					TestEqual(TEXT("timeline byte omission counted despite128 slots slack"), Row.OmittedTimeline, uint64(1));
					TestEqual(TEXT("note block independently refuses"), Row.Notes.Num(), 0);
					TestEqual(TEXT("note byte omission counted despite64 slots slack"), Row.OmittedNotes, uint64(1));
				});
				TestEqual(TEXT("lazy block refusal adds no actual backing"), Lifetime.Probe.LiveBacking.load(), BeforeBacking);
			}
			TestEqual(TEXT("lazy block refusal keeps previous credit"), Budget->UsedBytes.load(), BeforeBytes);
			auto Event = NoteEvent(1050, EVaRestTraceNoteKind::Queued, 0, 2);
			if (!PushApplyExpected(*this, Queue, Store, Event, EPush::Accepted)) return false;
			VisitRow(*this, Store, 1050, [&](const auto& Row) {
				if (TestEqual(TEXT("note admission recovers after byte pressure"), Row.Notes.Num(), 1)) CheckNote(*this, Row.Notes[0], Event);
				if (TestEqual(TEXT("timeline admission recovers after byte pressure"), Row.Timeline.Num(), 1))
					TestEqual(TEXT("recovery retains new source identity"), Row.Timeline[0].Seq, uint64(2));
				TestEqual(TEXT("prior timeline loss remains visible"), Row.OmittedTimeline, uint64(1));
			});
			return true;
		});
	bAll &= WithDiagnosticStore(*this, TEXT("Notes: borrowed views remain valid until visitor returns"),
		[&](FStore& Store, FCaptureQueue& Queue, auto&, auto&, auto&, FVaRestVirtualClock&) {
			auto Event = NoteEvent(1100, EVaRestTraceNoteKind::CacheServedStale, 0, 1);
			if (!PushApplyExpected(*this, Queue, Store, Started(1100), EPush::Accepted) ||
				!PushApplyExpected(*this, Queue, Store, Event, EPush::Accepted)) return false;
			int32 Visits = 0;
			TestTrue(TEXT("outer borrowed visitor runs"), Store.ForEachRecord([&](const auto& Row) {
				++Visits;
				TestFalse(TEXT("nested enumeration refuses instead of escaping scratch"), Store.ForEachRecord([](const auto&) {}));
				Store.Clear();
				if (TestEqual(TEXT("deferred Clear preserves note during visitor"), Row.Notes.Num(), 1)) CheckNote(*this, Row.Notes[0], Event);
				TestEqual(TEXT("deferred Clear preserves timeline during visitor"), Row.Timeline.Num(), 2);
			}));
			TestEqual(TEXT("single visitor delivered"), Visits, 1);
			TestEqual(TEXT("Clear executes after borrowed views expire"), CountRows(Store), 0);
			return true;
		});
	return bAll;
}

#endif
