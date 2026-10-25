// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspector.h"

#if WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR
#include "Async/Async.h"
#include "HAL/UnrealMemory.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Templates/SharedPointerInternals.h"
#include "Templates/TypeHash.h"
#include "VaRestClock.h"
#include "VaRestInspectorStats.h"
#include "VaRestInspectorSnapshot.h"
#include "VaRestInspectorQueue.h"
#include "VaRestInspectorStore.h"
#include "VaRestTrace.h"
#include <atomic>
#include <cmath>
#include <limits>

namespace VaRest::Inspector::Private::Testing
{
bool ReadLiveProfile(FAutomationTestBase& Test);
}
namespace VaRest::Inspector::Private::StatsTesting
{
using FSnapshot = TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>;
struct FStoreAccess
{
	static FStoreStoragePlan Plan(const FStore& Store) { return Store.EffectiveStoragePlan(); }
	static FStoreStoragePlan PlanFor(const FNormalizedLimits& Limits, const FBudgetAccount& Account) { return FStore::MeasureStoragePlan(Limits, Account); }
	static int32 Groups(const FStore& Store) { return Store.NumStatsGroups(); }
	static int64 RowBound(const FStore& Store) { return Store.RowStorageBound(); }
	static int64 TimelineBound(const FStore& Store) { return Store.TimelineStorageBound(); }
	static bool Costs(const FStore& Store, int64& Outer, int64& Nested) { return Store.MeasureSnapshotCosts(Outer, Nested); }
	static bool Pressure(FStore& Store) { return Store.MakeAdmissionRoom({Store.EffectiveStoragePlan().MaxRows, 0, {}}); }
	static void Stop(FStore& Store) { Store.StopPendingExcerpts(); }
	static int32 Expire(FStore& Store) { return Store.ExpirePendingExcerpts(); }
};

// Finite upstream fixture roots, independently pre-admitted and calibrated.
// This is not the Task11 production root factory or its quiescence proof.
class FReceiptClock final : public IVaRestClock
{
public:
	double Now() const override { return Value; }
	void Set(double InValue) { Value = InValue; }
	FVaRestTimerHandle SetTimer(double Delay, TUniqueFunction<void()>&& Callback) override
	{
		return Timers.SetTimer(Delay, MoveTemp(Callback));
	}
	void ClearTimer(FVaRestTimerHandle& Handle) override { Timers.ClearTimer(Handle); }

private:
	double Value = 0;
	FVaRestVirtualClock Timers;
};

// Explicit finite fixture exclusion, not Task11's production root factory.
// One context, one pending controller, one budget, one calibration scratch.
// Event/signature/clock/framework allocations below are upstream test data.
class FRoots
{
public:
	explicit FRoots(FAutomationTestBase& InTest)
		: Test(InTest)
	{
	}
	~FRoots()
	{
		if (Pending)
		{
			Test.TestEqual(TEXT("stats fixture pending count drained"), Pending->Count.load(), int64(0));
			Test.TestEqual(TEXT("stats fixture pending bytes drained"), Pending->Bytes.load(), int64(0));
			Test.TestEqual(TEXT("stats fixture excerpt reservations drained"), Pending->ExcerptReservations.load(), int64(0));
			Pending.Reset();
			StartupUsed.fetch_sub(4096);
		}
		if (Budget)
		{
			Test.TestEqual(TEXT("stats fixture only bootstrap remains"), Budget->UsedBytes.load(), Budget->BootstrapBytes);
			Test.TestEqual(TEXT("stats fixture controllers destroyed"), Budget->Probe.LiveControllers.load(), int64(0));
			Test.TestEqual(TEXT("stats fixture final weak slots released"), Budget->SnapshotControllers.load(), int64(0));
			Budget.SafeRelease();
			StartupUsed.fetch_sub(4096);
		}
		if (Lifetime)
		{
			Test.TestEqual(TEXT("stats fixture all actual backing freed"), Lifetime->Probe.LiveBacking.load(), int64(0));
			Lifetime.SafeRelease();
			StartupUsed.fetch_sub(4096);
		}
		Test.TestEqual(TEXT("stats fixture allowances released after actual root frees"), StartupUsed.load(), int64(0));
	}
	bool Initialize(int64 StoreLimit, int64 PendingLimit, int64 PendingCount)
	{
		if (!Testing::ReadLiveProfile(Test) || !AdmitRoot(sizeof(FAllocationLifetime), alignof(FAllocationLifetime)))
			return false;
		Lifetime = new FAllocationLifetime(GetSupportedAllocator());
		if (!Test.TestTrue(TEXT("actual context fits excluded root allowance"), FMemory::GetAllocSize(Lifetime.GetReference()) <= 4096))
			return false;
		const int64 Before = StartupUsed.load();
		const bool bReady = FStore::CalibrateRows(*Lifetime, StartupUsed, StartupLimit);
		Test.AddInfo(FString::Printf(TEXT("StatsRowCalibration Request=%lld Alignment=%lld Capacity=%lld StartupLimit=%lld"),
			Lifetime->Probe.CalibrationRequest.load(), Lifetime->Probe.CalibrationAlignment.load(),
			Lifetime->Probe.CalibrationCapacity.load(), StartupLimit));
		Test.TestEqual(TEXT("retained calibration scratch actually freed"), Lifetime->Probe.LiveBacking.load(), int64(0));
		Test.TestEqual(TEXT("retained calibration returns transient credit"), StartupUsed.load(), Before);
		if (!Test.TestTrue(TEXT("one retained common controller calibration"), bReady))
			return false;
		using C = SharedPointerInternals::TIntrusiveReferenceController<FPendingAccount, ESPMode::ThreadSafe>;
		if (!AdmitRoot(sizeof(C), alignof(C)))
			return false;
		Pending = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(PendingCount, PendingLimit);
		Pending->Lifetime = Lifetime;
		if (!AdmitRoot(sizeof(FBudgetAccount), alignof(FBudgetAccount)))
			return false;
		Budget = new FBudgetAccount(StoreLimit, 4096, Lifetime);
		return Test.TestTrue(TEXT("actual budget fits excluded root allowance"), FMemory::GetAllocSize(Budget.GetReference()) <= 4096);
	}
	TRefCountPtr<FAllocationLifetime> Lifetime;
	TSharedPtr<FPendingAccount, ESPMode::ThreadSafe> Pending;
	TRefCountPtr<FBudgetAccount> Budget;

private:
	bool AdmitRoot(SIZE_T Request, uint32 Alignment)
	{
		if (!Test.TestTrue(TEXT("exact fixture request and alignment audited"), Request > 0 && Request <= 1024 &&
																					Alignment > 0 && Alignment <= 16 && (Alignment & (Alignment - 1)) == 0))
			return false;
		if (!Test.TestTrue(TEXT("fixture root pre-admitted before allocation"), TryReserve(StartupUsed, StartupLimit, 4096)))
			return false;
		FAllocationCounters Probe;
		FAllocationAdapter Scratch(Probe, GetSupportedAllocator());
		const int64 Before = StartupUsed.load();
		const bool bReady = Scratch.Calibrate(StartupUsed, StartupLimit, Request, Alignment);
		const int64 OrdinaryAlignment = Request <= 8 ? 8 : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
		Test.AddInfo(FString::Printf(TEXT("StatsFixtureRoot Request=%lld Alignment=%lld Capacity=%lld StartupLimit=%lld"),
			Probe.CalibrationRequest.load(), Probe.CalibrationAlignment.load(), Probe.CalibrationCapacity.load(), StartupLimit));
		Test.TestEqual(TEXT("root calibration exact request"), Probe.CalibrationRequest.load(), int64(Request));
		Test.TestEqual(TEXT("root calibration effective ordinary alignment"), Probe.CalibrationAlignment.load(), OrdinaryAlignment);
		Test.TestEqual(TEXT("root scratch freed before credit return"), Probe.LiveBacking.load(), int64(0));
		Test.TestEqual(TEXT("root scratch allowance returned"), StartupUsed.load(), Before);
		if (!Test.TestTrue(TEXT("actual fixture root qualification"), bReady))
		{
			StartupUsed.fetch_sub(4096);
			return false;
		}
		return true;
	}
	static constexpr int64 StartupLimit = 4 * 4096;
	FAutomationTestBase& Test;
	std::atomic<int64> StartupUsed{0};
};

struct FCase
{
	FAutomationTestBase& Test;
	FRoots& Roots;
	FStore& Store;
	FCaptureQueue& Queue;
	FReceiptClock& Clock;
	FCapturePolicy Policy;
};
template <typename F>
static bool WithStore(FAutomationTestBase& Test, const TCHAR* Name, F&& Body, int32 MaxRows = 16,
	int64 StoreBytes = 64ll * 1024 * 1024, int64 PendingBytes = 4ll * 1024 * 1024)
{
	Test.AddInfo(Name);
	FCapturePolicy Policy;
	Policy.Limits.MaxRecords = MaxRows;
	Policy.Limits.StoreBytes = StoreBytes;
	Policy.Limits.PendingEvents = 32;
	Policy.Limits.PendingBytes = PendingBytes;
	FRoots Roots(Test);
	if (!Roots.Initialize(StoreBytes, PendingBytes, Policy.Limits.PendingEvents))
		return false;
	auto Clock = MakeShared<FReceiptClock, ESPMode::ThreadSafe>();
	Clock->Set(123.5);
	bool Result = false;
	{
		FStore Store(Policy.Limits, Clock);
		if (!Test.TestTrue(TEXT("stats store initializes"), Store.Initialize(Roots.Budget)))
			return false;
		const int64 Fixed = Roots.Budget->UsedBytes.load(), FixedBacking = Roots.Lifetime->Probe.LiveBacking.load();
		const auto Plan = FStoreAccess::Plan(Store);
		Test.TestEqual(TEXT("actual fixed plan equals startup charge"), Fixed, Plan.FixedBytes);
		Test.TestTrue(TEXT("actual measured row bound is supported"), FStoreAccess::RowBound(Store) > 0);
		Test.AddInfo(FString::Printf(TEXT("StatsPlan Fixed=%lld RowBound=%lld MaxRows=%d Groups=%d StatsIndex=%d Retained=%d"),
			Plan.FixedBytes, FStoreAccess::RowBound(Store), Plan.MaxRows, Plan.StatsGroupCapacity, Plan.StatsIndexCapacity, Plan.RetainedCapacity));
		{
			FCaptureQueue Queue(Policy, 1, Roots.Pending.ToSharedRef(), Roots.Lifetime->Allocator);
			if (!Test.TestTrue(TEXT("real queue supported"), Queue.IsSupported()) ||
				!Test.TestTrue(TEXT("same pending loss witness binds"), Store.BindLossWitness(Roots.Pending.ToSharedRef())))
				return false;
			Store.BeginGeneration(1, 0);
			ON_SCOPE_EXIT
			{
				Queue.Close();
				while (Queue.TryPopMetadata().IsSet())
				{
				}
				while (Queue.TryPopExcerpt().IsSet())
				{
				}
			};
			FCase C{Test, Roots, Store, Queue, *Clock, Policy};
			Result = Body(C);
		}
		Test.TestEqual(TEXT("queue/pending bytes drain"), Roots.Pending->Bytes.load(), int64(0));
		Test.TestEqual(TEXT("queue/pending count drains"), Roots.Pending->Count.load(), int64(0));
		Test.TestEqual(TEXT("no producer excerpt reservation remains"), Roots.Pending->ExcerptReservations.load(), int64(0));
		Store.Clear();
		Test.TestEqual(TEXT("Clear returns exact independent fixed charge"), Roots.Budget->UsedBytes.load(), Fixed);
		Test.TestEqual(TEXT("Clear returns actual fixed-only backing"), Roots.Lifetime->Probe.LiveBacking.load(), FixedBacking);
		Test.TestEqual(TEXT("all controller slots release normally"), Roots.Budget->SnapshotControllers.load(), int64(0));
		Test.TestEqual(TEXT("all actual controllers release normally"), Roots.Budget->Probe.LiveControllers.load(), int64(0));
		Test.TestTrue(TEXT("store highwater obeys immutable cap"), Roots.Budget->PeakBytes.load() <= Roots.Budget->Limit);
		Test.TestTrue(TEXT("pending highwater obeys immutable cap"), Roots.Pending->PeakBytes.load() <= Roots.Pending->ByteLimit);
	}
	Test.TestEqual(TEXT("ordinary Store destruction frees all backing"), Roots.Lifetime->Probe.LiveBacking.load(), int64(0));
	Test.TestEqual(TEXT("ordinary destruction leaves only bootstrap"), Roots.Budget->UsedBytes.load(), Roots.Budget->BootstrapBytes);
	return Result;
}
static FVaRestTraceEvent Started(uint64 Id, const TCHAR* Endpoint = TEXT("StatsEndpoint"), double Time = 0, uint64 Client = 9,
	const TCHAR* Template = TEXT("/stats/{id}"), const TCHAR* Url = TEXT("https://example.test/stats"))
{
	FVaRestTraceStarted V;
	V.RequestId = Id; V.ClientId = Client; V.ScopeId = 0; V.Endpoint = FName(Endpoint);
	V.Verb = TEXT("GET"); V.PathTemplate = Template; V.Url = Url;
	V.StartedAt = Time; V.CurrentAttempt = 1;
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::Started; E.Seq = Id * 100 + 1; E.Time = Time;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Completed(uint64 Id, EVaRestTraceOutcome Outcome = EVaRestTraceOutcome::Success,
	EVaRestErrorKind Error = EVaRestErrorKind::None, double Time = 1, bool FromCache = false,
	EVaRestTraceCacheLookup Lookup = EVaRestTraceCacheLookup::None)
{
	FVaRestTraceCompleted V;
	V.RequestId = Id; V.Outcome = Outcome; V.Error.Kind = Error;
	V.bFromCache = FromCache; V.CacheLookup = Lookup; V.Response.Status = 200;
	V.Response.BodyBytes = 123456; // A delivered entity is never a wire observation.
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::Completed; E.Seq = Id * 100 + 90; E.Time = Time;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceCompleted>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Dispatch(uint64 Id, uint64 Physical, EVaRestTraceGap Gap = EVaRestTraceGap::None, int32 Attempt = 1)
{
	FVaRestTraceAttemptDispatched V;
	V.RequestId = Id; V.DispatchId = Physical; V.Attempt = Attempt;
	V.GapKind = Gap; V.GapSeconds = 0; V.AuthRefreshId = 0;
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::AttemptDispatched; E.Seq = Id * 100 + Physical + 2; E.Time = .25;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptDispatched>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Ended(uint64 Id, uint64 Physical, int64 Up = 1, int64 Down = 2, int32 Status = 200, bool Pending = false)
{
	FVaRestTraceAttemptEnded V;
	V.RequestId = Id; V.DispatchId = Physical; V.Attempt = 1;
	V.BytesUp = Up; V.BytesDown = Down; V.Status = Status; V.bExcerptPending = Pending;
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::AttemptEnded; E.Seq = Id * 100 + Physical + 40; E.Time = .5;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptEnded>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Note(uint64 Id, EVaRestTraceNoteKind Kind, uint64 Seq)
{
	FVaRestTraceNote V;
	V.RequestId = Id; V.Kind = Kind; V.Attempt = 1;
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::Note; E.Seq = Seq; E.Time = .75;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceNote>(), MoveTemp(V));
	return E;
}
static bool Deliver(FCase& C, const FVaRestTraceEvent& E, bool Expected = true)
{
	if (!C.Test.TestEqual(TEXT("stats real codec/queue accepts source"), C.Queue.TryPush(E), EPush::Accepted)) return false;
	auto Packet = C.Queue.TryPopMetadata();
	if (!C.Test.TestTrue(TEXT("stats actual packet dequeued"), Packet.IsSet())) return false;
	return C.Test.TestEqual(TEXT("stats actual Store ingress result"), C.Store.Apply(MoveTemp(Packet.GetValue())), Expected);
}
static FSnapshot Take(FCase& C)
{
	FSnapshot Out;
	C.Test.TestTrue(TEXT("complete stats snapshot transaction succeeds"), C.Store.TrySnapshot(Out));
	C.Test.TestTrue(TEXT("complete stats snapshot is published"), Out.IsValid());
	return Out;
}
template <typename F>
static void Group(FCase& C, const FSnapshot& Snapshot, const TCHAR* Endpoint, F&& Check, uint64 Client = 9, const TCHAR* Key = TEXT(""))
{
	if (!C.Test.TestTrue(TEXT("stats owner exists for borrowed visit"), Snapshot.IsValid())) return;
	int32 Found = 0;
	Snapshot->ForEachStats([&](const FVaRestInspectorHttpStats& V) {
		if (!V.bOverflow && V.ClientId == Client && V.Endpoint == FName(Endpoint) && V.Key.Equals(Key, ESearchCase::CaseSensitive))
		{
			++Found; Check(V);
		}
	});
	C.Test.TestEqual(TEXT("exact independently selected named stats group"), Found, 1);
}
template <typename F>
static void Overflow(FCase& C, const FSnapshot& Snapshot, F&& Check)
{
	if (!C.Test.TestTrue(TEXT("overflow owner exists"), Snapshot.IsValid())) return;
	int32 Found = 0;
	Snapshot->ForEachStats([&](const FVaRestInspectorHttpStats& V) { if (V.bOverflow) { ++Found; Check(V); } });
	C.Test.TestEqual(TEXT("exactly one globally counted overflow"), Found, 1);
}
static FString Image(const FSnapshot& Snapshot)
{
	FString Text;
	Snapshot->ForEachStats([&](const FVaRestInspectorHttpStats& V) {
		Text += FString::Printf(TEXT("%llu/%llu/%s/%d:"), V.ScopeId, V.ClientId, *V.Endpoint.ToString(), V.Key.Len());
		Text += V.Key;
#define IMAGE_STATS(Field) Text += FString::Printf(TEXT("/%llu"), uint64(V.Field))
		IMAGE_STATS(bOverflow); IMAGE_STATS(bSaturated); IMAGE_STATS(bWireBytesUnknown); IMAGE_STATS(bDurationCountsSaturated);
		IMAGE_STATS(Started); IMAGE_STATS(Completed); IMAGE_STATS(Success); IMAGE_STATS(Error); IMAGE_STATS(Queued); IMAGE_STATS(Canceled);
		IMAGE_STATS(Dispatches); IMAGE_STATS(Retries); IMAGE_STATS(AuthReplays); IMAGE_STATS(RetryScheduled); IMAGE_STATS(UnknownDispatchClassification);
		IMAGE_STATS(CacheHit); IMAGE_STATS(CacheDisk); IMAGE_STATS(Revalidated); IMAGE_STATS(Stale); IMAGE_STATS(Replayed);
		IMAGE_STATS(WireBytesUp); IMAGE_STATS(WireBytesDown); IMAGE_STATS(Partial); IMAGE_STATS(Incomplete); IMAGE_STATS(Dropped);
		IMAGE_STATS(ErrorsByKind.Canceled); IMAGE_STATS(ErrorsByKind.Connection); IMAGE_STATS(ErrorsByKind.Timeout); IMAGE_STATS(ErrorsByKind.Http);
		IMAGE_STATS(ErrorsByKind.Server); IMAGE_STATS(ErrorsByKind.Decode); IMAGE_STATS(ErrorsByKind.Auth); IMAGE_STATS(ErrorsByKind.Offline);
		IMAGE_STATS(ErrorsByKind.Config); IMAGE_STATS(ErrorsByKind.Unknown); IMAGE_STATS(ErrorsByKind.bSaturated);
		IMAGE_STATS(DurationCount); IMAGE_STATS(DurationUnderflow); IMAGE_STATS(DurationOverflow); IMAGE_STATS(DurationUnknown); IMAGE_STATS(DurationNonFinite);
#undef IMAGE_STATS
		Text += FString::Printf(TEXT("/%.17g/%.17g/%.17g"), V.DurationSum, V.DurationMin, V.DurationMax);
		for (uint64 Bin : V.Histogram) Text += FString::Printf(TEXT("/%llu"), Bin);
		Text += TEXT(";");
	});
	return Text;
}
static uint64 ErrorSum(const FVaRestInspectorErrorCounts& V)
{
	return V.Canceled + V.Connection + V.Timeout + V.Http + V.Server + V.Decode + V.Auth + V.Offline + V.Config + V.Unknown;
}
static FVaRestInspectorHttpStats Totals(const FSnapshot& Snapshot)
{
	FVaRestInspectorHttpStats Out;
	Snapshot->ForEachStats([&](const FVaRestInspectorHttpStats& V) {
#define SUM_STATS(Field) Out.Field += V.Field
		SUM_STATS(Started); SUM_STATS(Completed); SUM_STATS(Success); SUM_STATS(Error); SUM_STATS(Queued); SUM_STATS(Canceled);
		SUM_STATS(Dispatches); SUM_STATS(Retries); SUM_STATS(AuthReplays); SUM_STATS(RetryScheduled); SUM_STATS(UnknownDispatchClassification);
		SUM_STATS(CacheHit); SUM_STATS(CacheDisk); SUM_STATS(Revalidated); SUM_STATS(Stale); SUM_STATS(Replayed);
		SUM_STATS(WireBytesUp); SUM_STATS(WireBytesDown); SUM_STATS(Partial); SUM_STATS(Incomplete); SUM_STATS(Dropped);
		SUM_STATS(ErrorsByKind.Canceled); SUM_STATS(ErrorsByKind.Connection); SUM_STATS(ErrorsByKind.Timeout); SUM_STATS(ErrorsByKind.Http);
		SUM_STATS(ErrorsByKind.Server); SUM_STATS(ErrorsByKind.Decode); SUM_STATS(ErrorsByKind.Auth); SUM_STATS(ErrorsByKind.Offline);
		SUM_STATS(ErrorsByKind.Config); SUM_STATS(ErrorsByKind.Unknown);
		SUM_STATS(DurationCount); SUM_STATS(DurationUnderflow); SUM_STATS(DurationOverflow); SUM_STATS(DurationUnknown); SUM_STATS(DurationNonFinite);
#undef SUM_STATS
		for (int32 Bin = 0; Bin < 512; ++Bin) Out.Histogram[Bin] += V.Histogram[Bin];
	});
	return Out;
}
static int32 HttpRows(const FSnapshot& Snapshot)
{
	int32 Count = 0;
	Snapshot->ForEachRecord([&](const auto& V) { Count += V.Kind == EVaRestInspectorRecordKind::Http ? 1 : 0; });
	return Count;
}
static double Boundary(int32 Index)
{
	double Value = 1e-6;
	for (int32 N = 0; N < Index; ++N) Value *= 1.1;
	return Value;
}
static bool RealDuration(FCase& C, uint64 Id, const TCHAR* Endpoint, double Seconds)
{
	if (!Deliver(C, Started(Id, Endpoint, 0))) return false;
	const auto E = Completed(Id, EVaRestTraceOutcome::Success, EVaRestErrorKind::None, Seconds);
	if (!C.Test.TestEqual(TEXT("real duration source is accepted by codec"), C.Queue.TryPush(E), EPush::Accepted)) return false;
	auto P = C.Queue.TryPopMetadata();
	if (!P.IsSet()) return false;
	C.Test.TestTrue(TEXT("real completion source time preserves independent duration input exactly"), P->Fixed().Header.Time == Seconds);
	return C.Test.TestTrue(TEXT("real duration completion is applied"), C.Store.Apply(MoveTemp(P.GetValue())));
}
} // namespace VaRest::Inspector::Private::StatsTesting

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorStatsHttpTest,
	"VaRest.Inspector.Stats.Http", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorStatsHttpTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	using namespace VaRest::Inspector::Private::StatsTesting;
	bool bAll = true;
	bAll &= WithStore(*this, TEXT("Stats.Http01: actual terminal classes, typed errors, all-row eviction and Clear isolation"), [](FCase& C) {
		const EVaRestTraceOutcome Outcomes[] = {EVaRestTraceOutcome::Success, EVaRestTraceOutcome::Error, EVaRestTraceOutcome::Error,
			EVaRestTraceOutcome::Error, EVaRestTraceOutcome::Error, EVaRestTraceOutcome::Queued, EVaRestTraceOutcome::Canceled};
		const EVaRestErrorKind Errors[] = {EVaRestErrorKind::Timeout, EVaRestErrorKind::Timeout, EVaRestErrorKind::Decode,
			EVaRestErrorKind::None, static_cast<EVaRestErrorKind>(255), EVaRestErrorKind::Decode, EVaRestErrorKind::Canceled};
		for (int32 I = 0; I < 7; ++I)
			if (!Deliver(C, Started(I + 1)) || !Deliver(C, Completed(I + 1, Outcomes[I], Errors[I], 2))) return false;
		auto Before = Take(C); if (!Before) return false;
		const FString Expected = Image(Before);
		Group(C, Before, TEXT("StatsEndpoint"), [&](const auto& V) {
			C.Test.TestEqual(TEXT("actual observed starts"), V.Started, uint64(7));
			C.Test.TestEqual(TEXT("actual terminal observations"), V.Completed, uint64(7));
			C.Test.TestEqual(TEXT("one success"), V.Success, uint64(1));
			C.Test.TestEqual(TEXT("four actual Error outcomes"), V.Error, uint64(4));
			C.Test.TestEqual(TEXT("one actual queued outcome"), V.Queued, uint64(1));
			C.Test.TestEqual(TEXT("one actual canceled outcome"), V.Canceled, uint64(1));
			C.Test.TestEqual(TEXT("typed errors equal actual Error outcomes"), ErrorSum(V.ErrorsByKind), V.Error);
			C.Test.TestEqual(TEXT("Timeout error observed once"), V.ErrorsByKind.Timeout, uint64(1));
			C.Test.TestEqual(TEXT("Decode error observed once"), V.ErrorsByKind.Decode, uint64(1));
			C.Test.TestEqual(TEXT("Error None and future kinds use Unknown"), V.ErrorsByKind.Unknown, uint64(2));
			C.Test.TestEqual(TEXT("Canceled outcome adds no error bucket"), V.ErrorsByKind.Canceled, uint64(0));
			C.Test.TestEqual(TEXT("queued residence is excluded from durations"), V.DurationCount, uint64(6));
			C.Test.TestEqual(TEXT("no physical request was invented"), V.Dispatches, uint64(0));
		});
		if (!C.Test.TestTrue(TEXT("common pressure removes every eligible HTTP history row"), FStoreAccess::Pressure(C.Store))) return false;
		auto Evicted = Take(C); if (!Evicted) return false;
		C.Test.TestEqual(TEXT("all HTTP rows actually evicted"), HttpRows(Evicted), 0);
		C.Test.TestEqual(TEXT("H1 post-eviction cumulative Completed survives"), Totals(Evicted).Completed, uint64(7));
		C.Test.TestTrue(TEXT("complete stats values and512 bins survive every row eviction"), Image(Evicted).Equals(Expected, ESearchCase::CaseSensitive));
		C.Store.Clear();
		auto Cleared = Take(C); if (!Cleared) return false;
		C.Test.TestEqual(TEXT("Clear resets actual completion totals"), Totals(Cleared).Completed, uint64(0));
		C.Test.TestEqual(TEXT("Clear resets every typed bucket"), ErrorSum(Totals(Cleared).ErrorsByKind), uint64(0));
		C.Test.TestTrue(TEXT("old copied key/counters/errors/all512 bins remain immutable"), Image(Before).Equals(Expected, ESearchCase::CaseSensitive));
		return true;
	}, 8);

	bAll &= WithStore(*this, TEXT("Stats.Http02: physical identities,503 retry,401 auth and same-Attempt redispatch"), [](FCase& C) {
		const TCHAR* Endpoints[] = {TEXT("Physical503Backoff"), TEXT("Physical401Refresh"), TEXT("PhysicalSameAttempt")};
		for (uint64 Id = 1; Id <= 3; ++Id)
		{
			if (!Deliver(C, Started(Id, Endpoints[Id - 1])) || !Deliver(C, Dispatch(Id, 101)) || !Deliver(C, Ended(Id, 101, 5, 50, Id == 1 ? 503 : Id == 2 ? 401 : 200))) return false;
			if (Id == 1)
			{
				const auto N = Note(Id, EVaRestTraceNoteKind::RetryScheduled, 991);
				if (!Deliver(C, N) || !Deliver(C, N)) return false;
			}
			const EVaRestTraceGap Gap = Id == 1 ? EVaRestTraceGap::Backoff : Id == 2 ? EVaRestTraceGap::SessionRefresh : EVaRestTraceGap::None;
			const auto D = Dispatch(Id, 102, Gap, Id == 3 ? 1 : 2);
			if (!Deliver(C, D) || !Deliver(C, D) || !Deliver(C, Ended(Id, 102, 7, 70)) || !Deliver(C, Completed(Id))) return false;
		}
		auto S = Take(C); if (!S) return false;
		const auto CheckScenarioCounts = [&](const FSnapshot& Snapshot) {
			const uint64 ExpectedRetries[] = {1, 0, 0};
			const uint64 ExpectedAuth[] = {0, 1, 0};
			for (int32 Scenario = 0; Scenario < 3; ++Scenario)
				Group(C, Snapshot, Endpoints[Scenario], [&](const auto& V) {
					C.Test.TestEqual(FString::Printf(TEXT("%s has its own exact retry count"), Endpoints[Scenario]), V.Retries, ExpectedRetries[Scenario]);
					C.Test.TestEqual(FString::Printf(TEXT("%s has its own exact auth replay count"), Endpoints[Scenario]), V.AuthReplays, ExpectedAuth[Scenario]);
					C.Test.TestEqual(TEXT("each actual scenario has two distinct physical ids"), V.Dispatches, uint64(2));
					C.Test.TestEqual(TEXT("each scenario has no inferred classification"), V.UnknownDispatchClassification, uint64(0));
				});
			const auto V = Totals(Snapshot);
			C.Test.TestEqual(TEXT("six actual distinct physical ids"), V.Dispatches, uint64(6));
			C.Test.TestEqual(TEXT("503 explicit Backoff increments retry once"), V.Retries, uint64(1));
			C.Test.TestEqual(TEXT("401 Attempt2 zero-gap RefreshId0 increments auth once"), V.AuthReplays, uint64(1));
			C.Test.TestEqual(TEXT("bounded duplicate scheduling note adds no executed retry"), V.RetryScheduled, uint64(1));
			C.Test.TestEqual(TEXT("all classifications are actual labels"), V.UnknownDispatchClassification, uint64(0));
			C.Test.TestEqual(TEXT("wire upload comes from six Ended observations"), V.WireBytesUp, uint64(36));
			C.Test.TestEqual(TEXT("wire download excludes delivered entities"), V.WireBytesDown, uint64(360));
		};
		CheckScenarioCounts(S);
		const FString Expected = Image(S);
		if (!FStoreAccess::Pressure(C.Store)) return false;
		auto Empty = Take(C); if (!Empty) return false;
		C.Test.TestEqual(TEXT("all physical-history rows are removed"), HttpRows(Empty), 0);
		CheckScenarioCounts(Empty);
		C.Test.TestTrue(TEXT("physical/error/duration totals persist after eviction"), Image(Empty).Equals(Expected, ESearchCase::CaseSensitive));
		return true;
	}, 4);

	for (bool bStop : {true, false})
		bAll &= WithStore(*this, bStop ? TEXT("Stats.Http03: orphan detail Stop leaves late auth classification pending") :
			TEXT("Stats.Http04: orphan has no deadline and expiry0 leaves late auth pending"), [bStop](FCase& C) {
			if (!Deliver(C, Ended(1, 101, 11, 13, 200, true))) return false;
			if (bStop) FStoreAccess::Stop(C.Store);
			else
			{
				C.Clock.Set(1000);
				C.Test.TestEqual(TEXT("never-completed orphan has no actual deadline"), FStoreAccess::Expire(C.Store), 0);
			}
			C.Store.ForEachRecord([&](const auto& R) {
				C.Test.TestEqual(TEXT("actual orphan detail state is branch-specific"), R.Dispatches[0].ResponseExcerptState,
					bStop ? EVaRestInspectorDetailState::CaptureStopped : EVaRestInspectorDetailState::Pending);
			});
			auto Pending = Take(C); if (!Pending) return false;
			Overflow(C, Pending, [&](const auto& V) { C.Test.TestEqual(TEXT("orphan/detail operations are not logical closure"), V.UnknownDispatchClassification, uint64(0)); });
			if (!Deliver(C, Dispatch(1, 101, EVaRestTraceGap::SessionRefresh, 2)) || !Deliver(C, Completed(1))) return false;
			auto Done = Take(C); if (!Done) return false;
			Overflow(C, Done, [&](const auto& V) {
				C.Test.TestEqual(TEXT("Ended and late Dispatched share one identity"), V.Dispatches, uint64(1));
				C.Test.TestEqual(TEXT("real late zero-gap auth classification wins before closure"), V.AuthReplays, uint64(1));
				C.Test.TestEqual(TEXT("no premature Unknown from orphan/Stop/expiry0"), V.UnknownDispatchClassification, uint64(0));
			});
			return true;
		}, 2);

	bAll &= WithStore(*this, TEXT("Stats.Http05: actual Completed closure and deadline expiry do not add Unknown twice"), [](FCase& C) {
		if (!Deliver(C, Ended(1, 101, 1, 2, 200, true)) || !Deliver(C, Completed(1))) return false;
		auto Before = Take(C); if (!Before) return false;
		Overflow(C, Before, [&](const auto& V) { C.Test.TestEqual(TEXT("first actual Completed finalizes missing dispatch classification"), V.UnknownDispatchClassification, uint64(1)); });
		C.Clock.Set(129);
		C.Test.TestEqual(TEXT("post-Completed actual deadline expires one row"), FStoreAccess::Expire(C.Store), 1);
		C.Store.ForEachRecord([&](const auto& R) { C.Test.TestTrue(TEXT("actual expiry changes pending detail and seals"), R.bSealed && R.Dispatches[0].ResponseExcerptState == EVaRestInspectorDetailState::TimedOut); });
		auto After = Take(C); if (!After) return false;
		Overflow(C, After, [&](const auto& V) { C.Test.TestEqual(TEXT("real detail expiry preserves finalized classification"), V.UnknownDispatchClassification, uint64(1)); });
		if (!Deliver(C, Dispatch(1, 101, EVaRestTraceGap::SessionRefresh), false)) return false;
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Stats.Http06: closure/removal/re-admission preserves bounded physical/completion dedup"), [](FCase& C) {
		if (!Deliver(C, Ended(1, 101, 5, 7)) || !Deliver(C, Completed(1))) return false;
		for (int32 Round = 0; Round < 2; ++Round)
		{
			if (!FStoreAccess::Pressure(C.Store) || !Deliver(C, Ended(1, 101, 5, 7)) || !Deliver(C, Completed(1))) return false;
		}
		auto S = Take(C); if (!S) return false;
		Overflow(C, S, [&](const auto& V) {
			C.Test.TestEqual(TEXT("bounded tombstone keeps one physical id"), V.Dispatches, uint64(1));
			C.Test.TestEqual(TEXT("bounded tombstone keeps one completion"), V.Completed, uint64(1));
			C.Test.TestEqual(TEXT("bounded tombstone keeps finalized Unknown once"), V.UnknownDispatchClassification, uint64(1));
			C.Test.TestEqual(TEXT("duplicate re-admission cannot add ended wire twice"), V.WireBytesDown, uint64(7));
		});
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Stats.Http07: authoritative attached absence freezes missing classification before late metadata"), [](FCase& C) {
		if (!Deliver(C, Ended(1, 101))) return false;
		FVaRestTraceSnapshot Empty;
		if (!C.Test.TestTrue(TEXT("actual authoritative absence reconciliation"), C.Store.ApplyAttached(Empty, C.Policy, C.Roots.Pending.ToSharedRef(), C.Roots.Lifetime->Allocator))) return false;
		if (!Deliver(C, Dispatch(1, 101, EVaRestTraceGap::SessionRefresh, 2)) || !Deliver(C, Completed(1))) return false;
		auto S = Take(C); if (!S) return false;
		Overflow(C, S, [&](const auto& V) {
			C.Test.TestEqual(TEXT("authoritative absence finalized Unknown once"), V.UnknownDispatchClassification, uint64(1));
			C.Test.TestEqual(TEXT("late actual metadata does not redistribute finalized totals"), V.AuthReplays, uint64(0));
			C.Test.TestEqual(TEXT("late actual metadata does not infer retry"), V.Retries, uint64(0));
		});
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Stats.Http08: inherited Known category cannot supply a missing numeric start timestamp"), [](FCase& C) {
		if (!Deliver(C, Started(1, TEXT("ReAdmitDuration"), 5))) return false;
		FVaRestTraceSnapshot Empty;
		if (!C.Store.ApplyAttached(Empty, C.Policy, C.Roots.Pending.ToSharedRef(), C.Roots.Lifetime->Allocator) || !FStoreAccess::Pressure(C.Store)) return false;
		if (!Deliver(C, Completed(1, EVaRestTraceOutcome::Success, EVaRestErrorKind::None, 10))) return false;
		auto S = Take(C); if (!S) return false;
		Group(C, S, TEXT("ReAdmitDuration"), [&](const auto& V) {
			C.Test.TestEqual(TEXT("previous actual Started remains deduplicated"), V.Started, uint64(1));
			C.Test.TestEqual(TEXT("fresh actual completion is still observed"), V.Completed, uint64(1));
			C.Test.TestEqual(TEXT("default new-row zero is not a numeric timestamp"), V.DurationCount, uint64(0));
			C.Test.TestEqual(TEXT("missing current source timestamp is explicitly unknown"), V.DurationUnknown, uint64(1));
		});
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Stats.Http09: final eligible orphan removal is actual classification closure"), [](FCase& C) {
		if (!Deliver(C, Ended(1, 101)) || !FStoreAccess::Pressure(C.Store)) return false;
		auto Removed = Take(C); if (!Removed) return false;
		C.Test.TestEqual(TEXT("orphan truly removed through common pressure"), HttpRows(Removed), 0);
		Overflow(C, Removed, [&](const auto& V) { C.Test.TestEqual(TEXT("final removal closes one pending classification"), V.UnknownDispatchClassification, uint64(1)); });
		if (!Deliver(C, Dispatch(1, 101, EVaRestTraceGap::SessionRefresh, 2)) || !Deliver(C, Completed(1))) return false;
		auto Late = Take(C); if (!Late) return false;
		Overflow(C, Late, [&](const auto& V) {
			C.Test.TestEqual(TEXT("retained removal closure remains once-only"), V.UnknownDispatchClassification, uint64(1));
			C.Test.TestEqual(TEXT("late metadata cannot rewrite removed classification"), V.AuthReplays, uint64(0));
		});
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Stats.Http10: negative, affirmative known-zero and positive saturated wire are independent"), [](FCase& C) {
		if (!Deliver(C, Started(1, TEXT("NegativeWire"))) || !Deliver(C, Dispatch(1, 101)) ||
			!Deliver(C, Ended(1, 101, -1, -2)) || !Deliver(C, Completed(1))) return false;
		if (!Deliver(C, Started(2, TEXT("KnownZero"))) || !Deliver(C, Completed(2, EVaRestTraceOutcome::Success, EVaRestErrorKind::None, 1, true, EVaRestTraceCacheLookup::HitMemory))) return false;
		if (!Deliver(C, Started(3, TEXT("SaturatedWire")))) return false;
		for (uint64 Physical = 1; Physical <= 3; ++Physical)
			if (!Deliver(C, Dispatch(3, Physical)) || !Deliver(C, Ended(3, Physical, MAX_int64, MAX_int64))) return false;
		if (!Deliver(C, Completed(3))) return false;
		auto S = Take(C); if (!S) return false;
		Group(C, S, TEXT("NegativeWire"), [&](const auto& V) {
			C.Test.TestTrue(TEXT("negative wire is unknown without arithmetic saturation"), V.bWireBytesUnknown && !V.bSaturated);
			C.Test.TestEqual(TEXT("negative upload adds zero"), V.WireBytesUp, uint64(0));
			C.Test.TestEqual(TEXT("negative download adds zero"), V.WireBytesDown, uint64(0));
		});
		Group(C, S, TEXT("KnownZero"), [&](const auto& V) {
			C.Test.TestFalse(TEXT("clean bound no-dispatch cache completion has known zero wire"), V.bWireBytesUnknown);
			C.Test.TestEqual(TEXT("delivered cached body is not network download"), V.WireBytesDown, uint64(0));
		});
		Group(C, S, TEXT("SaturatedWire"), [&](const auto& V) {
			C.Test.TestTrue(TEXT("positive addition saturation is visible"), V.bSaturated);
			C.Test.TestFalse(TEXT("positive saturation is separate from unknown source bytes"), V.bWireBytesUnknown);
			C.Test.TestEqual(TEXT("positive upload clamps"), V.WireBytesUp, MAX_uint64);
			C.Test.TestEqual(TEXT("positive download clamps"), V.WireBytesDown, MAX_uint64);
		});
		return true;
	}, 4);

	bAll &= WithStore(*this, TEXT("Stats.Http11: actual cache notes,304,stale,replay and canceled scheduling"), [](FCase& C) {
		auto InitialStale = Started(1, TEXT("Cache"));
		InitialStale.Payload.Get<FVaRestTraceStarted>().CacheLookup = EVaRestTraceCacheLookup::Stale;
		if (!Deliver(C, InitialStale) || !Deliver(C, Completed(1))) return false;
		if (!Deliver(C, Started(2, TEXT("Cache"))) || !Deliver(C, Completed(2, EVaRestTraceOutcome::Success, EVaRestErrorKind::None, 1, true, EVaRestTraceCacheLookup::HitDisk))) return false;
		if (!Deliver(C, Started(3, TEXT("Cache"))) || !Deliver(C, Dispatch(3, 301)) || !Deliver(C, Ended(3, 301, 3, 30, 304)) ||
			!Deliver(C, Note(3, EVaRestTraceNoteKind::CacheRevalidated, 3001)) || !Deliver(C, Completed(3))) return false;
		if (!Deliver(C, Started(4, TEXT("Cache"))) || !Deliver(C, Note(4, EVaRestTraceNoteKind::CacheServedStale, 4001)) ||
			!Deliver(C, Completed(4, EVaRestTraceOutcome::Success, EVaRestErrorKind::None, 1, true, EVaRestTraceCacheLookup::Stale))) return false;
		auto GuidOnly = Started(5, TEXT("Cache"));
		GuidOnly.Payload.Get<FVaRestTraceStarted>().QueueReplayOf.QueueId = TEXT("queue-guid-original-zero");
		if (!Deliver(C, GuidOnly) || !Deliver(C, Completed(5, EVaRestTraceOutcome::Queued))) return false;
		auto OriginalOnly = Started(6, TEXT("Cache"));
		OriginalOnly.Payload.Get<FVaRestTraceStarted>().QueueReplayOf.OriginalRequestId = 42;
		if (!Deliver(C, OriginalOnly) || !Deliver(C, Completed(6))) return false;
		if (!Deliver(C, Started(7, TEXT("Cache"))) || !Deliver(C, Note(7, EVaRestTraceNoteKind::RetryScheduled, 7001)) || !Deliver(C, Completed(7, EVaRestTraceOutcome::Canceled))) return false;
		auto S = Take(C); if (!S) return false;
		Group(C, S, TEXT("Cache"), [&](const auto& V) {
			C.Test.TestEqual(TEXT("only final actual cache provenance adds hits"), V.CacheHit, uint64(2));
			C.Test.TestEqual(TEXT("only actual final disk hit adds disk"), V.CacheDisk, uint64(1));
			C.Test.TestEqual(TEXT("actual304 revalidation note observed once"), V.Revalidated, uint64(1));
			C.Test.TestEqual(TEXT("note plus final stale is one observation"), V.Stale, uint64(1));
			C.Test.TestEqual(TEXT("GUID Original0 and nonzero Original replay are real starts"), V.Replayed, uint64(2));
			C.Test.TestEqual(TEXT("actual queued outcome"), V.Queued, uint64(1));
			C.Test.TestEqual(TEXT("canceled backoff schedules but does not execute retry"), V.RetryScheduled, uint64(1));
			C.Test.TestEqual(TEXT("only one actual network identity"), V.Dispatches, uint64(1));
			C.Test.TestEqual(TEXT("scheduled retry never invents execution"), V.Retries, uint64(0));
			C.Test.TestEqual(TEXT("wire excludes all cached/delivered byte lengths"), V.WireBytesDown, uint64(30));
		});
		return true;
	}, 8);

	bAll &= WithStore(*this, TEXT("Stats.Http12: attached queue provenance does not invent observed start or replay"), [](FCase& C) {
		auto E = Started(1, TEXT("AttachedQueue"), 3);
		auto& V = E.Payload.Get<FVaRestTraceStarted>();
		V.QueueReplayOf.QueueId = TEXT("attached-guid-original-zero"); V.CurrentAttempt = 3;
		FVaRestTraceSnapshot Attached; Attached.InFlight.Add(V);
		if (!C.Store.ApplyAttached(Attached, C.Policy, C.Roots.Pending.ToSharedRef(), C.Roots.Lifetime->Allocator)) return false;
		auto Before = Take(C); if (!Before) return false;
		C.Test.TestEqual(TEXT("attached GUID provenance creates actual public link"), Before->GetLinks().Num(), 1);
		C.Test.TestTrue(TEXT("attached GUID-only link has no numeric guessed target"), Before->GetLinks()[0].ToRequestId == 0 &&
			Before->GetLinks()[0].QueueId.Equals(TEXT("attached-guid-original-zero"), ESearchCase::CaseSensitive));
		Group(C, Before, TEXT("AttachedQueue"), [&](const auto& S) {
			C.Test.TestEqual(TEXT("attachment did not observe pre-attach Started"), S.Started, uint64(0));
			C.Test.TestEqual(TEXT("attachment did not observe pre-attach replay"), S.Replayed, uint64(0));
			C.Test.TestEqual(TEXT("attachment marks observed Partial once"), S.Partial, uint64(1));
			C.Test.TestEqual(TEXT("CurrentAttempt3 invents no network work"), S.Dispatches, uint64(0));
		});
		if (!Deliver(C, E) || !Deliver(C, Completed(1, EVaRestTraceOutcome::Success, EVaRestErrorKind::None, 4))) return false;
		auto After = Take(C); if (!After) return false;
		Group(C, After, TEXT("AttachedQueue"), [&](const auto& S) {
			C.Test.TestEqual(TEXT("future real accepted Started is counted once"), S.Started, uint64(1));
			C.Test.TestEqual(TEXT("future real accepted replay is counted once"), S.Replayed, uint64(1));
			C.Test.TestEqual(TEXT("real completion may use actual attached start time"), S.DurationCount, uint64(1));
			C.Test.TestTrue(TEXT("partial attachment cannot claim complete known wire"), S.bWireBytesUnknown);
		});
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Stats.Http13: actual generation,source,zero-id,terminal and duplicate gates"), [](FCase& C) {
		if (!Deliver(C, Started(1, TEXT("Ingress")))) return false;
		if (!Deliver(C, Started(1, TEXT("Ingress"), std::numeric_limits<double>::quiet_NaN()))) return false;
		{
			FCaptureQueue Other(C.Policy, 2, C.Roots.Pending.ToSharedRef(), C.Roots.Lifetime->Allocator);
			if (!C.Test.TestTrue(TEXT("different actual generation queue exists"), Other.IsSupported()) ||
				!C.Test.TestEqual(TEXT("other generation real packet accepted upstream"), Other.TryPush(Dispatch(1, 101)), EPush::Accepted)) return false;
			auto P = Other.TryPopMetadata();
			if (!P.IsSet() || !C.Test.TestFalse(TEXT("wrong generation rejected before stats"), C.Store.Apply(MoveTemp(P.GetValue())))) return false;
			Other.Close();
		}
		auto WrongSource = Dispatch(1, 101);
		WrongSource.Source = static_cast<EVaRestTraceSource>(255);
		C.Test.TestEqual(TEXT("real codec refuses unsupported source before creating Store input"), C.Queue.TryPush(WrongSource), EPush::Unsupported);
		if (!Deliver(C, Dispatch(0, 101), false) || !Deliver(C, Dispatch(1, 0), false) || !Deliver(C, Dispatch(1, 101)) ||
			!Deliver(C, Dispatch(1, 101)) || !Deliver(C, Ended(1, 101)) || !Deliver(C, Ended(1, 101)) ||
			!Deliver(C, Completed(1)) || !Deliver(C, Completed(1), false) || !Deliver(C, Dispatch(1, 102), false)) return false;
		auto S = Take(C); if (!S) return false;
		Group(C, S, TEXT("Ingress"), [&](const auto& V) {
			C.Test.TestEqual(TEXT("duplicate Started does not double-count actual observation"), V.Started, uint64(1));
			C.Test.TestTrue(TEXT("duplicate malformed start does not replace proven first numeric source time"), V.DurationCount == 1 && V.DurationNonFinite == 0);
			C.Test.TestEqual(TEXT("only valid distinct physical input counted"), V.Dispatches, uint64(1));
			C.Test.TestEqual(TEXT("only first actual completion counted"), V.Completed, uint64(1));
			C.Test.TestEqual(TEXT("duplicate Ended contributes wire once"), V.WireBytesDown, uint64(2));
		});
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Stats.Http14: failed row admission cannot create scalar stats"), [](FCase& C) {
		if (!Deliver(C, Started(1, TEXT("PinnedIngress"))) || !Deliver(C, Started(2, TEXT("RefusedIngress")), false)) return false;
		auto S = Take(C); if (!S) return false;
		C.Test.TestEqual(TEXT("only admitted pinned row exists"), HttpRows(S), 1);
		C.Test.TestEqual(TEXT("failed row creates no observed start"), Totals(S).Started, uint64(1));
		C.Test.TestEqual(TEXT("only overflow plus admitted named group"), FStoreAccess::Groups(C.Store), 2);
		return true;
	}, 1);

	bAll &= WithStore(*this, TEXT("Stats.Http15: scalar admission survives real optional detail refusal and deduplicates without slots"), [](FCase& C) {
		if (!Deliver(C, Started(1, TEXT("RefusedDetail")))) return false;
		const int64 Before = C.Roots.Budget->UsedBytes.load();
		auto Hold = FStoreCredit::Acquire(C.Roots.Budget, C.Roots.Budget->Limit - Before);
		if (!C.Test.TestTrue(TEXT("real independent budget pressure exhausts optional detail room"), Hold.IsSet())) return false;
		if (!Deliver(C, Dispatch(1, 101, EVaRestTraceGap::Backoff, 2)) || !Deliver(C, Dispatch(1, 101, EVaRestTraceGap::Backoff, 2))) return false;
		C.Store.ForEachRecord([&](const auto& R) { C.Test.TestTrue(TEXT("no optional dispatch slot could be owned"), R.Dispatches.IsEmpty() && R.bMetadataOmitted); });
		Hold.Reset();
		if (!Deliver(C, Ended(1, 101, 5, 7)) || !Deliver(C, Ended(1, 101, 5, 7)) || !Deliver(C, Completed(1))) return false;
		auto S = Take(C); if (!S) return false;
		Group(C, S, TEXT("RefusedDetail"), [&](const auto& V) {
			C.Test.TestEqual(TEXT("fixed identity witnessed refused-detail first dispatch once"), V.Dispatches, uint64(1));
			C.Test.TestEqual(TEXT("duplicate without optional slot adds no retry"), V.Retries, uint64(1));
			C.Test.TestEqual(TEXT("later real Ended adds bytes once"), V.WireBytesDown, uint64(7));
		});
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Stats.Http16:17th distinct physical identity is visibly refused without untracked totals"), [](FCase& C) {
		if (!Deliver(C, Started(1, TEXT("IdentityCap")))) return false;
		for (uint64 Physical = 1; Physical <= 17; ++Physical)
			if (!Deliver(C, Dispatch(1, Physical, EVaRestTraceGap::Backoff, 2)) || !Deliver(C, Ended(1, Physical))) return false;
		if (!Deliver(C, Dispatch(1, 17, EVaRestTraceGap::Backoff, 2)) || !Deliver(C, Ended(1, 17, 100, 200)) || !Deliver(C, Completed(1))) return false;
		auto S = Take(C); if (!S) return false;
		Group(C, S, TEXT("IdentityCap"), [&](const auto& V) {
			C.Test.TestEqual(TEXT("only16 independently witnessed identities count"), V.Dispatches, uint64(16));
			C.Test.TestEqual(TEXT("untracked17th cannot add retries repeatedly"), V.Retries, uint64(16));
			C.Test.TestEqual(TEXT("untracked17th cannot add ended bytes"), V.WireBytesDown, uint64(32));
			C.Test.TestTrue(TEXT("physical identity loss visibly marks wire unknown"), V.bWireBytesUnknown);
		});
		C.Test.TestTrue(TEXT("identity refusal also increments actual metadata omission"), C.Store.Diagnostics().MetadataOmitted > 0);
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Stats.Http17: real source durations,zero,unknown,nonfinite,underflow,overflow and boundaries"), [](FCase& C) {
		if (!RealDuration(C, 1, TEXT("ZeroDuration"), 0) || !RealDuration(C, 2, TEXT("UnderDuration"), .5e-6) ||
			!RealDuration(C, 3, TEXT("UpperDuration"), Boundary(510) * 2)) return false;
		if (!Deliver(C, Completed(4))) return false;
		if (!Deliver(C, Started(5, TEXT("NegativeDuration"), -1)) || !Deliver(C, Completed(5))) return false;
		if (!Deliver(C, Started(6, TEXT("ReversedDuration"), 3)) || !Deliver(C, Completed(6, EVaRestTraceOutcome::Success, EVaRestErrorKind::None, 2))) return false;
		if (!Deliver(C, Started(7, TEXT("NonFiniteDuration"), std::numeric_limits<double>::quiet_NaN())) || !Deliver(C, Completed(7)) ||
			!Deliver(C, Started(8, TEXT("NonFiniteDuration"))) || !Deliver(C, Completed(8, EVaRestTraceOutcome::Success, EVaRestErrorKind::None, std::numeric_limits<double>::infinity()))) return false;
		if (!Deliver(C, Started(9, TEXT("QueuedDuration"))) || !Deliver(C, Completed(9, EVaRestTraceOutcome::Queued, EVaRestErrorKind::None, 1e9))) return false;
		const double Edge = Boundary(127);
		if (!RealDuration(C, 10, TEXT("BoundaryDuration"), std::nextafter(Edge, 0.0)) ||
			!RealDuration(C, 11, TEXT("BoundaryDuration"), Edge) || !RealDuration(C, 12, TEXT("BoundaryDuration"), std::nextafter(Edge, std::numeric_limits<double>::infinity()))) return false;
		const double Mixed[] = {0, .5e-6, .0001, Boundary(510) * 2};
		for (int32 I = 0; I < 4; ++I) if (!RealDuration(C, 20 + I, TEXT("MixedDuration"), Mixed[I])) return false;
		if (!RealDuration(C, 30, TEXT("SumSaturation"), std::numeric_limits<double>::max()) ||
			!RealDuration(C, 31, TEXT("SumSaturation"), std::numeric_limits<double>::max())) return false;
		const double ClippedLow = Boundary(127) * 1.001, ClippedHigh = Boundary(127) * 1.009;
		if (!RealDuration(C, 32, TEXT("ClampedDuration"), ClippedLow) || !RealDuration(C, 33, TEXT("ClampedDuration"), ClippedHigh)) return false;
		auto S = Take(C); if (!S) return false;
		Group(C, S, TEXT("ZeroDuration"), [&](const auto& V) {
			C.Test.TestEqual(TEXT("exact zero occupies bin0"), V.Histogram[0], uint64(1));
			const auto Q = V.Quantile(50, 100);
			C.Test.TestTrue(TEXT("zero quantile is exact inclusive point"), Q.Kind == EVaRestInspectorQuantileKind::Zero && Q.bHasValue && Q.Value == 0 && Q.Lower == 0 && Q.Upper == 0 && Q.bLowerInclusive && Q.bUpperInclusive);
		});
		Group(C, S, TEXT("UnderDuration"), [&](const auto& V) {
			C.Test.TestEqual(TEXT("positive sub-microsecond sample has dedicated underflow"), V.DurationUnderflow, uint64(1));
			C.Test.TestEqual(TEXT("underflow is not an ordinary bin1 precision claim"), V.Histogram[1], uint64(0));
			const auto Q = V.Quantile(95, 100);
			C.Test.TestTrue(TEXT("underflow exposes a range without point estimate"), Q.Kind == EVaRestInspectorQuantileKind::Underflow && !Q.bHasValue && Q.Lower > 0 && Q.Upper < 1e-6);
		});
		Group(C, S, TEXT("UpperDuration"), [&](const auto& V) {
			C.Test.TestEqual(TEXT("finite upper sample has overflow histogram"), V.Histogram[511], uint64(1));
			C.Test.TestEqual(TEXT("finite upper sample separately exposes overflow count"), V.DurationOverflow, uint64(1));
			const auto Q = V.Quantile(99, 100);
			C.Test.TestTrue(TEXT("upper overflow exposes a range without precision claim"), Q.Kind == EVaRestInspectorQuantileKind::Overflow && !Q.bHasValue);
		});
		Group(C, S, TEXT("NegativeDuration"), [&](const auto& V) { C.Test.TestTrue(TEXT("negative start interval is unknown"), V.DurationCount == 0 && V.DurationUnknown == 1); });
		Group(C, S, TEXT("ReversedDuration"), [&](const auto& V) { C.Test.TestTrue(TEXT("completion before actual start is unknown"), V.DurationCount == 0 && V.DurationUnknown == 1); });
		Group(C, S, TEXT("NonFiniteDuration"), [&](const auto& V) { C.Test.TestTrue(TEXT("actual nonfinite inputs never enter ranks"), V.DurationCount == 0 && V.DurationNonFinite == 2 && V.Quantile(50, 100).Kind == EVaRestInspectorQuantileKind::Unknown); });
		Group(C, S, TEXT("QueuedDuration"), [&](const auto& V) { C.Test.TestTrue(TEXT("durable queued residence is not a request duration"), V.DurationCount == 0 && V.DurationUnknown == 0 && V.DurationNonFinite == 0); });
		Group(C, S, TEXT("BoundaryDuration"), [&](const auto& V) {
			C.Test.TestEqual(TEXT("nextafter below exact boundary remains prior bin"), V.Histogram[127], uint64(1));
			C.Test.TestEqual(TEXT("exact boundary and nextafter above enter upper bin"), V.Histogram[128], uint64(2));
		});
		Group(C, S, TEXT("MixedDuration"), [&](const auto& V) {
			C.Test.TestEqual(TEXT("mixed finite sample count includes exceptions"), V.DurationCount, uint64(4));
			C.Test.TestEqual(TEXT("rank1 selects exact zero"), V.Quantile(1, 4).Kind, EVaRestInspectorQuantileKind::Zero);
			C.Test.TestEqual(TEXT("rank2 selects dedicated underflow"), V.Quantile(2, 4).Kind, EVaRestInspectorQuantileKind::Underflow);
			C.Test.TestEqual(TEXT("rank3 selects ordinary interval"), V.Quantile(3, 4).Kind, EVaRestInspectorQuantileKind::LogInterval);
			C.Test.TestEqual(TEXT("rank4 selects finite upper overflow"), V.Quantile(4, 4).Kind, EVaRestInspectorQuantileKind::Overflow);
		});
		Group(C, S, TEXT("SumSaturation"), [&](const auto& V) {
			C.Test.TestTrue(TEXT("floating sum saturation clamps without erasing count precision"), V.bSaturated && !V.bDurationCountsSaturated && V.DurationSum == std::numeric_limits<double>::max());
			C.Test.TestEqual(TEXT("two finite huge values retain exact duration count"), V.DurationCount, uint64(2));
			C.Test.TestEqual(TEXT("unrelated sum saturation still returns true exceptional rank"), V.Quantile(50, 100).Kind, EVaRestInspectorQuantileKind::Overflow);
		});
		Group(C, S, TEXT("ClampedDuration"), [&](const auto& V) {
			const auto Q = V.Quantile(50, 100);
			C.Test.TestTrue(TEXT("actual same-bin samples lie below original logarithmic midpoint"), V.Histogram[128] == 2 && ClippedHigh < std::sqrt(Boundary(127) * Boundary(128)));
			C.Test.TestTrue(TEXT("original bin midpoint is clamped to observed max rather than recentered in clipped range"), Q.bHasValue && Q.Value == ClippedHigh);
			C.Test.TestTrue(TEXT("reported interval independently intersects observed extrema"), Q.Lower == ClippedLow && Q.Upper == ClippedHigh && Q.bLowerInclusive && Q.bUpperInclusive);
		});
		Overflow(C, S, [&](const auto& V) { C.Test.TestTrue(TEXT("missing Started source is explicitly unknown"), V.DurationUnknown == 1 && V.DurationCount == 0); });
		return true;
	}, 4);

	bAll &= WithStore(*this, TEXT("Stats.Http18: H3 real eleven-duration geometric percentile with unclamped selected interval"), [](FCase& C) {
		const double Lower = .18066374773038418, Upper = .19873012250342262;
		C.Test.TestTrue(TEXT("independent rounded lower boundary is exact"), Boundary(127) == Lower);
		C.Test.TestTrue(TEXT("independent rounded upper boundary is exact"), Boundary(128) == Upper);
		const double NearLower = Lower * (1 + 1e-8);
		if (!C.Test.TestTrue(TEXT("near-lower sample is strictly in actual ordinary interval"), NearLower > Lower && NearLower < Upper)) return false;
		if (!RealDuration(C, 1, TEXT("Geometric"), Lower / 2)) return false;
		for (uint64 Id = 2; Id <= 10; ++Id) if (!RealDuration(C, Id, TEXT("Geometric"), NearLower)) return false;
		if (!RealDuration(C, 11, TEXT("Geometric"), Upper * 2)) return false;
		auto S = Take(C); if (!S) return false;
		Group(C, S, TEXT("Geometric"), [&](const auto& V) {
			C.Test.TestEqual(TEXT("actual P50 denominator is eleven"), V.DurationCount, uint64(11));
			C.Test.TestEqual(TEXT("nine actual source values occupy independently chosen bin128"), V.Histogram[128], uint64(9));
			const auto Q = V.Quantile(50, 100);
			C.Test.TestEqual(TEXT("P50 rank6 selects ordinary bin"), Q.Kind, EVaRestInspectorQuantileKind::LogInterval);
			C.Test.TestTrue(TEXT("selected interval lower exact and not masked by min clamp"), Q.Lower == Lower);
			C.Test.TestTrue(TEXT("selected interval upper exact and not masked by max clamp"), Q.Upper == Upper);
			C.Test.TestTrue(TEXT("actual global extrema bracket both candidate centers"), V.DurationMin < Lower && V.DurationMax > Upper);
			C.Test.TestTrue(TEXT("H3 actual midpoint is geometric"), std::abs(Q.Value - std::sqrt(Lower * Upper)) <= 1e-15);
			C.Test.TestTrue(TEXT("H3 actual relative error obeys geometric bound not generic5percent"), std::abs(Q.Value / NearLower - 1) <= std::sqrt(Upper / Lower) - 1 + 1e-12);
			C.Test.TestTrue(TEXT("relative-error control independently discriminates arithmetic midpoint"), ((Lower + Upper) / 2) / NearLower - 1 > std::sqrt(Upper / Lower) - 1 + 1e-12);
		});
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Stats.Http19: actual queue drop delta survives marker refusal and Clear rebaseline"), [](FCase& C) {
		const auto DropOnce = [&]() {
			while (C.Roots.Pending->Count.load() < C.Roots.Pending->CountLimit)
				if (!C.Test.TestEqual(TEXT("count-pressure source packet genuinely enqueued"), C.Queue.TryPush(Dispatch(999, 101)), EPush::Accepted)) return false;
			C.Test.TestEqual(TEXT("real queue drops at actual count cap"), C.Queue.TryPush(Dispatch(999, 102)), EPush::Dropped);
			while (C.Queue.TryPopMetadata().IsSet()) {}
			return true;
		};
		if (!DropOnce()) return false;
		const int64 Used = C.Roots.Budget->UsedBytes.load();
		auto Hold = FStoreCredit::Acquire(C.Roots.Budget, C.Roots.Budget->Limit - Used);
		if (!Hold.IsSet()) return false;
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending);
		Hold.Reset();
		auto S = Take(C); if (!S) return false;
		Overflow(C, S, [&](const auto& V) { C.Test.TestEqual(TEXT("actual dropped event contributes one even when marker cannot allocate"), V.Dropped, uint64(1)); });
		C.Test.TestTrue(TEXT("actual marker admission was visibly refused"), C.Store.Diagnostics().OmittedMarkers > 0);
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending);
		auto Same = Take(C); if (!Same) return false;
		Overflow(C, Same, [&](const auto& V) { C.Test.TestEqual(TEXT("same sample adds no duplicate drop"), V.Dropped, uint64(1)); });
		C.Store.Clear();
		C.Test.TestEqual(TEXT("Clear does not reset actual lifetime dropped counter"), C.Roots.Pending->DroppedCount.load(), uint64(1));
		if (!DropOnce()) return false;
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending);
		auto New = Take(C); if (!New) return false;
		Overflow(C, New, [&](const auto& V) { C.Test.TestEqual(TEXT("new capture counts only new actual source delta"), V.Dropped, uint64(1)); });
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Stats.Http20 SCALAR: source saturation propagates with zero new dropped-count delta"), [](FCase& C) {
		// Representational boundary control, not a claim that MAX events ran.
		C.Roots.Pending->MetadataOmitted.store(MAX_uint64);
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending);
		auto S = Take(C); if (!S) return false;
		Overflow(C, S, [&](const auto& V) {
			C.Test.TestEqual(TEXT("metadata scalar is not a dropped event count"), V.Dropped, uint64(0));
			C.Test.TestTrue(TEXT("actual sampled source saturation is visible"), V.bSaturated);
		});
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending);
		return true;
	}, 2);
	bAll &= WithStore(*this, TEXT("Stats.Http21: late proven context does not redistribute earlier overflow observations"), [](FCase& C) {
		if (!Deliver(C, Ended(1, 101, 5, 7)) || !Deliver(C, Started(1, TEXT("LateContext"))) ||
			!Deliver(C, Dispatch(1, 101, EVaRestTraceGap::SessionRefresh, 2)) || !Deliver(C, Completed(1))) return false;
		auto S = Take(C); if (!S) return false;
		Overflow(C, S, [&](const auto& V) {
			C.Test.TestEqual(TEXT("first orphan physical observation stays in overflow"), V.Dispatches, uint64(1));
			C.Test.TestEqual(TEXT("late classification stays with that physical identity"), V.AuthReplays, uint64(1));
			C.Test.TestEqual(TEXT("earlier source bytes remain in original overflow"), V.WireBytesDown, uint64(7));
			C.Test.TestEqual(TEXT("late actual completion does not migrate backward"), V.Completed, uint64(0));
			C.Test.TestEqual(TEXT("real late gap avoids invented Unknown"), V.UnknownDispatchClassification, uint64(0));
		});
		Group(C, S, TEXT("LateContext"), [&](const auto& V) {
			C.Test.TestEqual(TEXT("future actual Started uses newly proven named key"), V.Started, uint64(1));
			C.Test.TestEqual(TEXT("future actual completion uses newly proven named key"), V.Completed, uint64(1));
			C.Test.TestEqual(TEXT("old physical count was not redistributed into named key"), V.Dispatches, uint64(0));
			C.Test.TestEqual(TEXT("old auth count was not redistributed into named key"), V.AuthReplays, uint64(0));
			C.Test.TestEqual(TEXT("old bytes were not redistributed into named key"), V.WireBytesDown, uint64(0));
		});
		return true;
	}, 2);
	return bAll;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorStatsBoundsTest,
	"VaRest.Inspector.Stats.Bounds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorStatsBoundsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	using namespace VaRest::Inspector::Private::StatsTesting;
	bool bAll = true;
	AddInfo(TEXT("Stats.Bounds01: public mutable histogram validation and exact rational ranks"));
	{
		FVaRestInspectorHttpStats Valid;
		Valid.DurationCount = 1; Valid.Histogram[128] = 1;
		Valid.DurationMin = Valid.DurationMax = Boundary(127) * (1 + 1e-8);
		const auto Unknown = [&](const FVaRestInspectorHttpStats& V, const TCHAR* Name) {
			const auto Q = V.Quantile(50, 100);
			TestTrue(Name, Q.Kind == EVaRestInspectorQuantileKind::Unknown && !Q.bHasValue && Q.Value == 0 && Q.Lower == 0 && Q.Upper == 0);
		};
		TestEqual(TEXT("consistent mutable value has ordinary quantile"), Valid.Quantile(50, 100).Kind, EVaRestInspectorQuantileKind::LogInterval);
		{ auto V = Valid; V.DurationCount = 2; Unknown(V, TEXT("mismatched public count refuses precision")); }
		{ auto V = Valid; V.DurationOverflow = 1; Unknown(V, TEXT("mismatched duplicate overflow tally refuses precision")); }
		{ auto V = Valid; V.Histogram[127] = MAX_uint64; V.DurationCount = MAX_uint64; Unknown(V, TEXT("checked public histogram sum cannot wrap")); }
		{ auto V = Valid; V.DurationMin = std::numeric_limits<double>::quiet_NaN(); Unknown(V, TEXT("NaN extrema refuse precision")); }
		{ auto V = Valid; V.DurationMax = std::numeric_limits<double>::infinity(); Unknown(V, TEXT("infinite extrema refuse precision")); }
		{ auto V = Valid; V.DurationMin = -1; Unknown(V, TEXT("negative extrema refuse precision")); }
		{ auto V = Valid; V.DurationMin = V.DurationMax + 1; Unknown(V, TEXT("inverted extrema refuse precision")); }
		{ auto V = Valid; V.DurationMin = Boundary(126); Unknown(V, TEXT("unoccupied lowest category cannot be invented by min")); }
		{ auto V = Valid; V.DurationMax = Boundary(129); Unknown(V, TEXT("unoccupied highest category cannot be invented by max")); }
		{ FVaRestInspectorHttpStats V; V.DurationCount = 1; V.Histogram[0] = 1; V.DurationMin = V.DurationMax = 1; Unknown(V, TEXT("positive extrema cannot intersect exact zero")); }
		{ FVaRestInspectorHttpStats V; V.DurationCount = V.DurationUnderflow = 1; V.DurationMin = V.DurationMax = 1e-6; Unknown(V, TEXT("underflow upper boundary is excluded")); }
		{ FVaRestInspectorHttpStats V; V.DurationCount = V.DurationOverflow = V.Histogram[511] = 1; V.DurationMin = V.DurationMax = Boundary(509); Unknown(V, TEXT("upper overflow cannot intersect an ordinary sample")); }
		Unknown(FVaRestInspectorHttpStats{}, TEXT("empty public value has no quantile"));
		TestEqual(TEXT("zero denominator is unknown"), Valid.Quantile(1, 0).Kind, EVaRestInspectorQuantileKind::Unknown);
		TestEqual(TEXT("numerator beyond denominator is unknown"), Valid.Quantile(101, 100).Kind, EVaRestInspectorQuantileKind::Unknown);
		{ auto V = Valid; V.bDurationCountsSaturated = true; const auto Q = V.Quantile(50, 100); TestTrue(TEXT("lost count precision explicitly reports Saturated without rank"), Q.Kind == EVaRestInspectorQuantileKind::Saturated && !Q.bHasValue); }
		{ auto V = Valid; V.bSaturated = true; V.DurationSum = std::numeric_limits<double>::max(); TestEqual(TEXT("unrelated counter/sum saturation does not erase exact ranks"), V.Quantile(50, 100).Kind, EVaRestInspectorQuantileKind::LogInterval); }
		FVaRestInspectorHttpStats Huge;
		Huge.DurationCount = MAX_uint64; Huge.Histogram[0] = MAX_uint64 - 1; Huge.DurationUnderflow = 1;
		Huge.DurationMin = 0; Huge.DurationMax = .5e-6;
		TestEqual(TEXT("exact huge rational first rank"), Huge.Quantile(0, MAX_uint32).Kind, EVaRestInspectorQuantileKind::Zero);
		TestEqual(TEXT("exact huge rational last rank without multiplication wrap"), Huge.Quantile(MAX_uint32, MAX_uint32).Kind, EVaRestInspectorQuantileKind::Underflow);
		TestEqual(TEXT("exact huge rational middle rank"), Huge.Quantile(1, 2).Kind, EVaRestInspectorQuantileKind::Zero);
		FVaRestInspectorErrorCounts E;
		for (EVaRestErrorKind Kind : {EVaRestErrorKind::Canceled, EVaRestErrorKind::Connection, EVaRestErrorKind::Timeout,
			EVaRestErrorKind::Http, EVaRestErrorKind::Server, EVaRestErrorKind::Decode, EVaRestErrorKind::Auth,
			EVaRestErrorKind::Offline, EVaRestErrorKind::Config, EVaRestErrorKind::None, static_cast<EVaRestErrorKind>(255)}) E.Add(Kind);
		TestEqual(TEXT("exhaustive public enum mapping plus future fallback"), ErrorSum(E), uint64(11));
		TestEqual(TEXT("None and future errors choose Unknown"), E.Unknown, uint64(2));
		E.Timeout = MAX_uint64; E.Add(EVaRestErrorKind::Timeout);
		TestTrue(TEXT("each public typed error counter saturates independently"), E.Timeout == MAX_uint64 && E.bSaturated && E.Decode == 1 && E.Unknown == 2);
	}

	bAll &= WithStore(*this, TEXT("Stats.Bounds02: deterministic Endpoint,template,host,IPv6 and proven context keys"), [](FCase& C) {
		if (!Deliver(C, Started(1, TEXT("EndpointKey"))) || !Deliver(C, Completed(1))) return false;
		if (!Deliver(C, Started(2, TEXT(""), 0, 9, TEXT("/items/{id}"), TEXT("https://x.test/items/one?q=1"))) || !Deliver(C, Completed(2))) return false;
		if (!Deliver(C, Started(3, TEXT(""), 0, 9, TEXT("/items/{id}"), TEXT("https://x.test/items/two?q=2"))) || !Deliver(C, Completed(3))) return false;
		if (!Deliver(C, Started(4, TEXT(""), 0, 9, TEXT(""), TEXT("https://user@EXAMPLE.test:8443/one?nonce=1#fragment"))) || !Deliver(C, Completed(4))) return false;
		if (!Deliver(C, Started(5, TEXT(""), 0, 9, TEXT(""), TEXT("https://example.test/two?nonce=2"))) || !Deliver(C, Completed(5))) return false;
		if (!Deliver(C, Started(6, TEXT(""), 0, 9, TEXT(""), TEXT("https://[2001:DB8::1]:443/secret?q=1"))) || !Deliver(C, Completed(6))) return false;
		if (!Deliver(C, Started(7, TEXT("RealClientZero"), 0, 0)) || !Deliver(C, Completed(7))) return false;
		auto OtherScope = Started(8, TEXT("WrongScope")); OtherScope.Payload.Get<FVaRestTraceStarted>().ScopeId = 1;
		if (!Deliver(C, OtherScope) || !Deliver(C, Completed(8)) || !Deliver(C, Ended(9, 901)) || !Deliver(C, Completed(9))) return false;
		if (!Deliver(C, Started(10, TEXT(""), 0, 9, TEXT(""), TEXT("https://:443/no-host"))) || !Deliver(C, Completed(10))) return false;
		if (!Deliver(C, Started(11, TEXT(""), 0, 9, TEXT(""), TEXT("https://[not-ipv6]/no-host"))) || !Deliver(C, Completed(11))) return false;
		if (!Deliver(C, Started(12, TEXT(""), 0, 9, TEXT(""), TEXT("https://example.test:bad/no-host"))) || !Deliver(C, Completed(12))) return false;
		auto S = Take(C); if (!S) return false;
		Group(C, S, TEXT("EndpointKey"), [&](const auto& V) { C.Test.TestTrue(TEXT("actual FName endpoint uses no copied text key"), V.Key.IsEmpty()); });
		Group(C, S, TEXT(""), [&](const auto& V) { C.Test.TestEqual(TEXT("redacted template identity ignores substituted URL values"), V.Completed, uint64(2)); }, 9, TEXT("T:GET /items/{id}"));
		Group(C, S, TEXT(""), [&](const auto& V) { C.Test.TestEqual(TEXT("host key drops userinfo,port,path,query,fragment and normalizes ASCII case"), V.Completed, uint64(2)); }, 9, TEXT("H:GET example.test"));
		Group(C, S, TEXT(""), [&](const auto& V) { C.Test.TestEqual(TEXT("bracketed IPv6 host key is bounded and normalized"), V.Completed, uint64(1)); }, 9, TEXT("H:GET [2001:db8::1]"));
		Group(C, S, TEXT("RealClientZero"), [&](const auto& V) { C.Test.TestEqual(TEXT("source-proven real client0 is named, not unknown"), V.Started, uint64(1)); }, 0);
		Overflow(C, S, [&](const auto& V) {
			C.Test.TestEqual(TEXT("unsupported scope,orphan,malformed authority/IPv6/port use one overflow"), V.Completed, uint64(5));
			C.Test.TestTrue(TEXT("overflow retains no rejected key or endpoint"), V.Key.IsEmpty() && V.Endpoint.IsNone());
		});
		C.Test.TestEqual(TEXT("exact actual named memberships plus one overflow"), FStoreAccess::Groups(C.Store), 6);
		return true;
	}, 4);

	bAll &= WithStore(*this, TEXT("Stats.Bounds03: per-client257th key conserves real scalars in one overflow"), [](FCase& C) {
		for (uint64 Id = 1; Id <= 257; ++Id)
		{
			const FString Template = FString::Printf(TEXT("/keys/%llu/{id}"), Id);
			if (!Deliver(C, Started(Id, TEXT(""), 0, 9, *Template)) || !Deliver(C, Completed(Id, EVaRestTraceOutcome::Error, EVaRestErrorKind::Timeout))) return false;
		}
		C.Test.TestEqual(TEXT("256 named plus one globally counted overflow"), FStoreAccess::Groups(C.Store), 257);
		auto S = Take(C); if (!S) return false;
		C.Test.TestEqual(TEXT("per-client cap does not lose observed Completed"), Totals(S).Completed, uint64(257));
		C.Test.TestEqual(TEXT("typed errors conserved across named and overflow groups"), ErrorSum(Totals(S).ErrorsByKind), uint64(257));
		Overflow(C, S, [&](const auto& V) { C.Test.TestTrue(TEXT("257th client key is counted without retaining rejected text"), V.Completed == 1 && V.ErrorsByKind.Timeout == 1 && V.Key.IsEmpty()); });
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Stats.Bounds04: H2 actual global1024 cap with1100 real clients and independent byte slack"), [](FCase& C) {
		const auto Plan = FStoreAccess::Plan(C.Store);
		if (!C.Test.TestEqual(TEXT("real effective global capacity includes overflow"), Plan.StatsGroupCapacity, 1024) ||
			!C.Test.TestEqual(TEXT("real hash table has safe independent backing slack"), Plan.StatsIndexCapacity, 2048)) return false;
		for (uint64 Client = 1; Client <= 1100; ++Client)
		{
			const auto E = Started(10000 + Client, TEXT(""), 0, Client, TEXT(""), TEXT("https://CAP.test/path?changing=value"));
			if (Client == 1024)
			{
				FPacketLayout Layout;
				if (!C.Test.TestTrue(TEXT("global boundary source fully measured"), FPacketCodec::Measure(E, C.Policy, C.Roots.Lifetime->Allocator, Layout) == EMeasure::Full)) return false;
				const auto GroupBound = C.Roots.Lifetime->Allocator.Bound(sizeof(FStatsGroupSlot), alignof(FStatsGroupSlot));
				const auto KeyBound = C.Roots.Lifetime->Allocator.Bound(sizeof(TEXT("H:GET cap.test")), alignof(TCHAR));
				const auto OwnerBound = C.Roots.Lifetime->Allocator.Bound(sizeof(FStorePacketOwner), alignof(FStorePacketOwner));
				if (!GroupBound.IsSet() || !KeyBound.IsSet() || !OwnerBound.IsSet()) return false;
				const int64 FullNewNamed = int64(GroupBound.GetValue() + KeyBound.GetValue() + OwnerBound.GetValue() + Layout.BackingBound) +
					FStoreAccess::RowBound(C.Store) + 4096 + FStoreAccess::TimelineBound(C.Store);
				C.Test.TestTrue(TEXT("H2 byte room independently admits whole extra group,key,row,packet,timeline/controller"), C.Roots.Budget->Limit - C.Roots.Budget->UsedBytes.load() >= FullNewNamed);
				C.Test.TestTrue(TEXT("no snapshot cap can mask H2 admission"), C.Roots.Budget->SnapshotControllers.load() == 0);
			}
			if (!Deliver(C, E)) return false;
			if (Client == 1024)
				if (!C.Test.TestEqual(TEXT("H2 immediate real group count stays1024 before any snapshot"), FStoreAccess::Groups(C.Store), 1024)) return false;
			if (!Deliver(C, Completed(10000 + Client, EVaRestTraceOutcome::Error, EVaRestErrorKind::Decode))) return false;
		}
		C.Test.TestEqual(TEXT("global cap remains1024 after1100 actual clients"), FStoreAccess::Groups(C.Store), 1024);
		auto S = Take(C); if (!S) return false;
		C.Test.TestEqual(TEXT("all1100 actual completed/errors remain conserved"), Totals(S).Completed, uint64(1100));
		C.Test.TestEqual(TEXT("typed errors include process overflow"), ErrorSum(Totals(S).ErrorsByKind), uint64(1100));
		Overflow(C, S, [&](const auto& V) {
			C.Test.TestEqual(TEXT("77 rejected named keys contribute actual overflow errors"), V.Error, uint64(77));
			C.Test.TestEqual(TEXT("overflow includes every Decode bucket"), V.ErrorsByKind.Decode, uint64(77));
			C.Test.TestTrue(TEXT("overflow owns no rejected per-client key"), V.Key.IsEmpty());
		});
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Stats.Bounds05: real fixed-first data,failed key admission rollback,reuse and heap/key frees"), [](FCase& C) {
		const int64 Fixed = C.Roots.Budget->UsedBytes.load(), Backing = C.Roots.Lifetime->Probe.LiveBacking.load();
		FStatsGroup* First = C.Store.FindOrAdmitStatsGroup({0, 9, FName(TEXT("FixedFirst")), {}}, {});
		if (!C.Test.TestTrue(TEXT("first named endpoint is an actual admitted group"), First && !First->bOverflow)) return false;
		C.Test.TestEqual(TEXT("fixed-first endpoint adds no freeable object/key charge"), C.Roots.Budget->UsedBytes.load(), Fixed);
		C.Test.TestEqual(TEXT("fixed-first endpoint adds no backing allocation"), C.Roots.Lifetime->Probe.LiveBacking.load(), Backing);
		if (!Deliver(C, Started(1, TEXT("FixedFirst"))) || !Deliver(C, Completed(1))) return false;
		{ auto S = Take(C); if (!S) return false; Group(C, S, TEXT("FixedFirst"), [&](const auto& V) { C.Test.TestTrue(TEXT("fixed-first value contains genuine source observations"), V.Started == 1 && V.Completed == 1); }); }
		C.Store.Clear();
		C.Test.TestEqual(TEXT("ordinary first-slot reset returns exact measured fixed charge"), C.Roots.Budget->UsedBytes.load(), Fixed);
		auto Hold = FStoreCredit::Acquire(C.Roots.Budget, C.Roots.Budget->Limit - Fixed);
		if (!Hold.IsSet()) return false;
		FStatsGroup* Refused = C.Store.FindOrAdmitStatsGroup({0, 9, NAME_None, FStringView(TEXT("failed-fallback-key"))}, {});
		C.Test.TestTrue(TEXT("real byte admission refuses fallback text to counted overflow"), Refused && Refused->bOverflow);
		C.Test.TestEqual(TEXT("failed first key never publishes membership"), FStoreAccess::Groups(C.Store), 1);
		C.Test.TestEqual(TEXT("refused fallback allocates no retained backing"), C.Roots.Lifetime->Probe.LiveBacking.load(), Backing);
		Hold.Reset();
		FString TooLong; TooLong.AppendChars(TEXT("x"), 1);
		for (int32 I = 0; I < FNormalizedLimits::MaxStringChars; ++I) TooLong.AppendChar(TCHAR('x'));
		Refused = C.Store.FindOrAdmitStatsGroup({0, 9, NAME_None, FStringView(TooLong)}, {});
		C.Test.TestTrue(TEXT("complete overlong key refuses without truncation collision"), Refused && Refused->bOverflow);
		C.Test.TestEqual(TEXT("overlong key retains no bytes"), C.Roots.Budget->UsedBytes.load(), Fixed);
		const auto FirstKeyBound = C.Roots.Lifetime->Allocator.Bound(sizeof(TEXT("reused-fallback")), alignof(TCHAR));
		const auto SecondKeyBound = C.Roots.Lifetime->Allocator.Bound(sizeof(TEXT("second-fallback")), alignof(TCHAR));
		const auto ObjectBound = C.Roots.Lifetime->Allocator.Bound(sizeof(FStatsGroupSlot), alignof(FStatsGroupSlot));
		if (!FirstKeyBound.IsSet() || !SecondKeyBound.IsSet() || !ObjectBound.IsSet()) return false;
		First = C.Store.FindOrAdmitStatsGroup({0, 9, NAME_None, FStringView(TEXT("reused-fallback"))}, {});
		if (!C.Test.TestTrue(TEXT("unused fixed slot is reusable after failed key admission"), First && !First->bOverflow)) return false;
		C.Test.TestEqual(TEXT("fixed-first fallback charges only actual text bound"), C.Roots.Budget->UsedBytes.load(), Fixed + int64(FirstKeyBound.GetValue()));
		FStatsGroup* Second = C.Store.FindOrAdmitStatsGroup({0, 9, NAME_None, FStringView(TEXT("second-fallback"))}, {});
		if (!C.Test.TestTrue(TEXT("second group has independent heap ownership"), Second && !Second->bOverflow && Second != First)) return false;
		C.Test.TestEqual(TEXT("second group/key actual full charge is independent"), C.Roots.Budget->UsedBytes.load(),
			Fixed + int64(FirstKeyBound.GetValue() + SecondKeyBound.GetValue() + ObjectBound.GetValue()));
		C.Store.Clear();
		C.Test.TestEqual(TEXT("all fixed-first and heap keys free before credit return"), C.Roots.Budget->UsedBytes.load(), Fixed);
		C.Test.TestEqual(TEXT("all dynamic actual group/key backing is freed"), C.Roots.Lifetime->Probe.LiveBacking.load(), Backing);
		return true;
	}, 2);

	AddInfo(TEXT("Stats.Bounds06: pure measured cap2 threshold and genuine supported cap1/cap2 stores"));
	int64 Cap2Limit = 0;
	{
		FRoots Search(*this);
		if (Search.Initialize(64ll * 1024 * 1024, 4ll * 1024 * 1024, 32))
		{
			FNormalizedLimits Limits; Limits.MaxRecords = 1;
			const auto Measure = [&](int64 Limit) {
				FBudgetAccount StackAccount(Limit, Search.Budget->BootstrapBytes, Search.Lifetime);
				return FStoreAccess::PlanFor(Limits, StackAccount);
			};
			int64 Low = 8192, High = Search.Budget->Limit;
			const auto HighPlan = Measure(High);
			if (TestTrue(TEXT("actual pure-plan search has supported/fitting upper witness"), HighPlan.bSupported && HighPlan.bFits && HighPlan.StatsGroupCapacity >= 2))
			{
				while (Low < High)
				{
					const int64 Middle = Low + (High - Low) / 2;
					const auto P = Measure(Middle);
					if (P.bSupported && P.bFits && P.StatsGroupCapacity >= 2) High = Middle;
					else Low = Middle + 1;
				}
				const auto At = Measure(Low), Below = Measure(Low - 1);
				if (TestTrue(TEXT("actual supported fitting Plan2/Plan1 boundary exists"), At.bSupported && At.bFits && At.StatsGroupCapacity == 2 &&
					Below.bSupported && Below.bFits && Below.StatsGroupCapacity == 1)) Cap2Limit = Low;
				AddInfo(FString::Printf(TEXT("StatsActualCapBoundary Limit=%lld AtFixed=%lld BelowFixed=%lld AtGroups=%d BelowGroups=%d"),
					Low, At.FixedBytes, Below.FixedBytes, At.StatsGroupCapacity, Below.StatsGroupCapacity));
			}
		}
		else bAll = false;
	}
	if (Cap2Limit == 0) bAll = false;
	else for (int32 Capacity : {1, 2})
		bAll &= WithStore(*this, Capacity == 1 ? TEXT("Stats.Bounds06a: actual tiny plan only counted overflow") :
			TEXT("Stats.Bounds06b: actual tiny plan one fixed named plus overflow"), [Capacity](FCase& C) {
			const auto P = FStoreAccess::Plan(C.Store);
			if (!C.Test.TestTrue(TEXT("ordinary instantiated plan reproduces measured cap"), P.bSupported && P.bFits && P.StatsGroupCapacity == Capacity)) return false;
			C.Test.TestTrue(TEXT("actual whole fixed bytes respect independent quarter limit"), P.FixedBytes <= C.Roots.Budget->Limit / 4);
			C.Test.TestTrue(TEXT("actual larger row/controller base remains admissible"), P.FixedBytes + P.MinimumHttpBytes <= C.Roots.Budget->Limit);
			if (!Deliver(C, Started(1, TEXT("Tiny"))) || !Deliver(C, Completed(1))) return false;
			C.Test.TestEqual(TEXT("real counted membership follows actual effective capacity"), FStoreAccess::Groups(C.Store), Capacity);
			auto S = Take(C); if (!S) return false;
			if (Capacity == 1) Overflow(C, S, [&](const auto& V) { C.Test.TestEqual(TEXT("cap1 keeps real observed data in sole overflow"), V.Completed, uint64(1)); });
			else Group(C, S, TEXT("Tiny"), [&](const auto& V) { C.Test.TestEqual(TEXT("cap2 fixed-first contains real data"), V.Completed, uint64(1)); });
			return true;
		}, 1, Cap2Limit - (Capacity == 1 ? 1 : 0));

	bAll &= WithStore(*this, TEXT("Stats.Bounds07: complete owning stats snapshots,Clear churn and one-byte-short atomic refusal"), [](FCase& C) {
		if (!Deliver(C, Started(1, TEXT(""), 0, 9, TEXT("/immutable-first/{id}"))) || !Deliver(C, Dispatch(1, 101)) ||
			!Deliver(C, Ended(1, 101, -1, 9)) || !Deliver(C, Completed(1, EVaRestTraceOutcome::Error, EVaRestErrorKind::Timeout))) return false;
		if (!Deliver(C, Started(2, TEXT(""), 0, 9, TEXT("/immutable-second/{id}"))) || !Deliver(C, Completed(2, EVaRestTraceOutcome::Error, EVaRestErrorKind::Decode)) ||
			!Deliver(C, Completed(3, EVaRestTraceOutcome::Error, static_cast<EVaRestErrorKind>(255)))) return false;
		auto Old = Take(C); if (!Old) return false;
		const FString Expected = Image(Old);
		Group(C, Old, TEXT(""), [&](const auto& V) {
			C.Test.TestTrue(TEXT("public copied key owns its actual capacity including terminator"), V.Key.GetAllocatedSize() >= SIZE_T(V.Key.Len() + 1) * sizeof(TCHAR));
			C.Test.TestTrue(TEXT("full copied typed errors,unknown wire and finite histogram are present"), V.ErrorsByKind.Timeout == 1 && V.bWireBytesUnknown && V.DurationCount == 1);
		}, 9, TEXT("T:GET /immutable-first/{id}"));
		const FVaRestInspectorHttpStats* Addresses[3] = {};
		const TCHAR* KeyAddresses[3] = {};
		int32 At = 0;
		Old->ForEachStats([&](const auto& V) { if (At < 3) { Addresses[At] = &V; KeyAddresses[At] = V.Key.GetCharArray().GetData(); } ++At; });
		C.Test.TestEqual(TEXT("borrowed visitor sees three actual already-owned values"), At, 3);
		C.Test.TestTrue(TEXT("different stored groups have different stable addresses"), Addresses[0] != Addresses[1] && Addresses[1] != Addresses[2] && Addresses[0] != Addresses[2]);
		const int64 Used = C.Roots.Budget->UsedBytes.load(), Backing = C.Roots.Lifetime->Probe.LiveBacking.load(), Copied = C.Roots.Lifetime->Probe.TextBytesCopied.load();
		for (int32 Visit = 0; Visit < 8; ++Visit)
		{
			At = 0;
			Old->ForEachStats([&](const auto& V) {
				if (At < 3) C.Test.TestTrue(TEXT("visitor borrows the same stored value/key without owning temporary"), &V == Addresses[At] && V.Key.GetCharArray().GetData() == KeyAddresses[At]);
				++At;
			});
		}
		C.Test.TestEqual(TEXT("visitor leaves store credit unchanged"), C.Roots.Budget->UsedBytes.load(), Used);
		C.Test.TestEqual(TEXT("visitor acquires no Inspector backing"), C.Roots.Lifetime->Probe.LiveBacking.load(), Backing);
		C.Test.TestEqual(TEXT("visitor copies no Inspector text"), C.Roots.Lifetime->Probe.TextBytesCopied.load(), Copied);
		C.Store.Clear();
		if (!Deliver(C, Started(100, TEXT(""), 0, 17, TEXT("/new-table/{id}"))) || !Deliver(C, Completed(100))) return false;
		C.Test.TestTrue(TEXT("old complete keys/scalars/errors/all512 bins survive Clear and table churn"), Image(Old).Equals(Expected, ESearchCase::CaseSensitive));
		int64 Outer = 0, Nested = 0;
		if (!C.Test.TestTrue(TEXT("entire current public-copy transaction measured before allocation"), FStoreAccess::Costs(C.Store, Outer, Nested))) return false;
		const int64 Need = 4096 + Outer + Nested;
		const int64 Free = C.Roots.Budget->Limit - C.Roots.Budget->UsedBytes.load();
		if (!C.Test.TestTrue(TEXT("fixture has exact transaction pressure headroom"), Need > 4096 && Free >= Need)) return false;
		auto Hold = FStoreCredit::Acquire(C.Roots.Budget, Free - Need + 1);
		if (!Hold.IsSet()) return false;
		FSnapshot Out = Old;
		const auto* Original = Out.Get();
		const int64 BeforeRefusal = C.Roots.Budget->UsedBytes.load(), BeforeBacking = C.Roots.Lifetime->Probe.LiveBacking.load(), BeforeSlots = C.Roots.Budget->SnapshotControllers.load();
		C.Test.TestFalse(TEXT("one byte short refuses the complete public stats copy"), C.Store.TrySnapshot(Out));
		C.Test.TestTrue(TEXT("failed copy preserves exact prior Out owner"), Out.Get() == Original);
		C.Test.TestEqual(TEXT("failed copy fully unwinds temporary credits"), C.Roots.Budget->UsedBytes.load(), BeforeRefusal);
		C.Test.TestEqual(TEXT("failed copy acquires no lingering actual backing"), C.Roots.Lifetime->Probe.LiveBacking.load(), BeforeBacking);
		C.Test.TestEqual(TEXT("failed copy releases temporary snapshot slot"), C.Roots.Budget->SnapshotControllers.load(), BeforeSlots);
		C.Test.TestTrue(TEXT("failed copy leaves old complete values unchanged"), Image(Out).Equals(Expected, ESearchCase::CaseSensitive));
		Hold.Reset();
		if (!C.Test.TestTrue(TEXT("ordinary exact copy succeeds after releasing independent pressure"), C.Store.TrySnapshot(Out))) return false;
		C.Test.TestTrue(TEXT("new transaction publishes distinct owning snapshot"), Out.Get() != Original);
		Group(C, Out, TEXT(""), [&](const auto& V) { C.Test.TestEqual(TEXT("new capture has only actual new table observations"), V.Completed, uint64(1)); }, 17, TEXT("T:GET /new-table/{id}"));
		C.Test.TestTrue(TEXT("successful new copy cannot mutate old values"), Image(Old).Equals(Expected, ESearchCase::CaseSensitive));
		return true;
	}, 4);

	bAll &= WithStore(*this, TEXT("Stats.Bounds08: four strong public stats snapshots and controller credit through final weak"), [](FCase& C) {
		if (!Deliver(C, Started(1, TEXT(""), 0, 9, TEXT("/four-owned-keys/{id}"))) || !Deliver(C, Completed(1))) return false;
		const int64 LiveStore = C.Roots.Budget->UsedBytes.load();
		FSnapshot Readers[4];
		TWeakPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe> Weak[4];
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Readers[Index] = Take(C); if (!Readers[Index]) return false;
			Weak[Index] = Readers[Index];
		}
		const FString Expected = Image(Readers[0]);
		FSnapshot Fifth = Readers[0]; const auto* Prior = Fifth.Get();
		C.Test.TestFalse(TEXT("fifth full owning stats snapshot is refused"), C.Store.TrySnapshot(Fifth));
		C.Test.TestTrue(TEXT("four-strong refusal preserves caller Out"), Fifth.Get() == Prior);
		C.Test.TestEqual(TEXT("four actual controller slots are occupied"), C.Roots.Budget->SnapshotControllers.load(), int64(4));
		for (const auto& R : Readers) C.Test.TestTrue(TEXT("every reader owns identical full immutable value"), Image(R).Equals(Expected, ESearchCase::CaseSensitive));
		Fifth.Reset();
		for (auto& R : Readers) R.Reset();
		C.Test.TestEqual(TEXT("all public group/key payloads free before remaining controller credit"), C.Roots.Budget->UsedBytes.load(), LiveStore + 4 * 4096);
		C.Test.TestEqual(TEXT("last strong release does not release weak controller slots"), C.Roots.Budget->SnapshotControllers.load(), int64(4));
		FSnapshot Refused;
		C.Test.TestFalse(TEXT("four final-weak slots still block a fresh snapshot"), C.Store.TrySnapshot(Refused));
		for (auto& W : Weak) W.Reset();
		C.Test.TestEqual(TEXT("final weak destruction releases every exact controller credit"), C.Roots.Budget->UsedBytes.load(), LiveStore);
		C.Test.TestEqual(TEXT("final weak destruction releases every snapshot slot"), C.Roots.Budget->SnapshotControllers.load(), int64(0));
		auto Ordinary = Take(C); if (!Ordinary) return false;
		C.Test.TestTrue(TEXT("ordinary copy after weak drain remains complete"), Image(Ordinary).Equals(Expected, ESearchCase::CaseSensitive));
		return true;
	}, 2);
	bAll &= WithStore(*this, TEXT("Stats.Bounds09 SCALAR: actual ingress saturates admitted counters/bins and copies flags before Clear"), [](FCase& C) {
		// Seed a representational boundary in one real charged group. This is
		// not a claim that MAX requests ran; the next actual event is real.
		FStatsGroup* V = C.Store.FindOrAdmitStatsGroup({0, 9, FName(TEXT("ScalarBoundary")), {}}, {});
		if (!V || V->bOverflow) return false;
		V->Started = V->Completed = V->Error = V->ErrorsByKind.Timeout = MAX_uint64;
		V->DurationCount = V->Histogram[0] = MAX_uint64;
		V->DurationMin = V->DurationMax = 0;
		if (!Deliver(C, Started(1, TEXT("ScalarBoundary"))) || !Deliver(C, Completed(1, EVaRestTraceOutcome::Error, EVaRestErrorKind::Timeout, 0))) return false;
		auto Old = Take(C); if (!Old) return false;
		Group(C, Old, TEXT("ScalarBoundary"), [&](const auto& S) {
			C.Test.TestTrue(TEXT("real next ingress independently clamps counters"), S.Started == MAX_uint64 && S.Completed == MAX_uint64 && S.Error == MAX_uint64);
			C.Test.TestTrue(TEXT("real next typed error clamps and copies nested saturation"), S.ErrorsByKind.Timeout == MAX_uint64 && S.ErrorsByKind.bSaturated);
			C.Test.TestTrue(TEXT("actual duration count/bin overflow explicitly loses rank precision"), S.DurationCount == MAX_uint64 && S.Histogram[0] == MAX_uint64 && S.bDurationCountsSaturated && S.bSaturated);
			C.Test.TestEqual(TEXT("copied actual duration saturation refuses percentile precision"), S.Quantile(50, 100).Kind, EVaRestInspectorQuantileKind::Saturated);
		});
		const FString Expected = Image(Old);
		C.Store.Clear();
		if (!Deliver(C, Started(2, TEXT("ScalarBoundary"))) || !Deliver(C, Completed(2))) return false;
		auto Fresh = Take(C); if (!Fresh) return false;
		Group(C, Fresh, TEXT("ScalarBoundary"), [&](const auto& S) { C.Test.TestTrue(TEXT("Clear resets every saturation/count/bin domain"), S.Started == 1 && S.Completed == 1 && S.Error == 0 && S.ErrorsByKind.Timeout == 0 && !S.ErrorsByKind.bSaturated && !S.bDurationCountsSaturated && !S.bSaturated); });
		C.Test.TestTrue(TEXT("old saturated copied value survives Clear unchanged"), Image(Old).Equals(Expected, ESearchCase::CaseSensitive));
		return true;
	}, 2);
	return bAll;
}
#endif
