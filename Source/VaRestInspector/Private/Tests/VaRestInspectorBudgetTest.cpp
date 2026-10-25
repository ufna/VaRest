// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspector.h"

#if WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR

#include "Async/Async.h"
#include "Containers/Array.h"
#include "HAL/UnrealMemory.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Templates/AlignmentTemplates.h"
#include "Templates/SharedPointerInternals.h"
#include "VaRestClock.h"
#include "VaRestInspectorAllocation.h"
#include "VaRestInspectorBudget.h"
#include "VaRestInspectorPacket.h"
#include "VaRestInspectorPolicy.h"
#include "VaRestInspectorQueue.h"
#include "VaRestInspectorRecords.h"
#include "VaRestInspectorStore.h"
#include "VaRestInspectorSnapshot.h"
#include "VaRestTrace.h"

#include <atomic>
#include <limits>

namespace VaRest::Inspector::Private::Testing
{
bool ReadLiveProfile(FAutomationTestBase& Test);
}

namespace VaRest::Inspector::Private::BudgetTesting
{
using FSnapshot = TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>;
using FWeakSnapshot = TWeakPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>;

struct FStoreAccess
{
	static FStoreStoragePlan Plan(const FStore& Store) { return Store.EffectiveStoragePlan(); }
	static int32 NumClients(const FStore& Store) { return Store.NumClients(); }
	static FStringView ClientLabel(const FStore& Store, uint64 ClientId) { return Store.ClientLabel(ClientId); }
	static int32 NumStats(const FStore& Store) { return Store.NumStatsGroups(); }
	static int32 NumLinks(const FStore& Store) { return Store.NumLinks(); }
	static int64 RowBound(const FStore& Store) { return Store.RowStorageBound(); }
	static int64 TimelineBound(const FStore& Store) { return Store.TimelineStorageBound(); }
	static bool SnapshotCosts(const FStore& Store, int64& Outer, int64& Nested) { return Store.MeasureSnapshotCosts(Outer, Nested); }
	static bool HasLink(const FStore& Store, const FLinkSeed& Seed) { return Store.HasLink(Seed); }
	static int32 TargetLinks(const FStore& Store, uint64 Target) { return Store.NumLinksForTarget(Target); }
	static void Stop(FStore& Store) { Store.StopPendingExcerpts(); }
	static int32 Expire(FStore& Store) { return Store.ExpirePendingExcerpts(); }
};

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

class FRoots
{
public:
	FRoots(FAutomationTestBase& InTest, bool bInRecreation)
		: Test(InTest)
		, bRecreation(bInRecreation)
		, StartupLimit(bInRecreation ? 6 * 4096 : 4 * 4096)
	{
	}
	~FRoots()
	{
		Lease.Reset();
		if (Pending)
		{
			Test.TestEqual(TEXT("budget fixture pending count drained"), Pending->Count.load(), int64(0));
			Test.TestEqual(TEXT("budget fixture pending bytes drained"), Pending->Bytes.load(), int64(0));
			Test.TestEqual(TEXT("budget fixture pending reservations drained"), Pending->ExcerptReservations.load(), int64(0));
			Pending.Reset();
			StartupUsed.fetch_sub(4096);
		}
		if (Budget)
		{
			Test.TestEqual(TEXT("budget fixture only bootstrap remains"), Budget->UsedBytes.load(), Budget->BootstrapBytes);
			Test.TestEqual(TEXT("budget fixture controllers destroyed"), Budget->Probe.LiveControllers.load(), int64(0));
			Test.TestEqual(TEXT("budget fixture final weak slots released"), Budget->SnapshotControllers.load(), int64(0));
			Budget.SafeRelease();
			StartupUsed.fetch_sub(4096);
		}
		if (Lifetime)
		{
			Test.TestEqual(TEXT("budget fixture actual backing freed"), Lifetime->Probe.LiveBacking.load(), int64(0));
			Lifetime.SafeRelease();
			StartupUsed.fetch_sub(4096);
		}
		if (StandbyUsed)
		{
			StartupUsed.fetch_sub(StandbyUsed);
			StandbyUsed = 0;
		}
		Test.TestEqual(TEXT("budget fixture allowances released"), StartupUsed.load(), int64(0));
	}
	bool Initialize(int64 StoreLimit, int64 PendingLimit, int64 PendingCount)
	{
		if (!Testing::ReadLiveProfile(Test) || !AdmitRoot(sizeof(FAllocationLifetime), alignof(FAllocationLifetime)))
			return false;
		Lifetime = new FAllocationLifetime(GetSupportedAllocator());
		if (!Test.TestTrue(TEXT("actual context fits root allowance"), FMemory::GetAllocSize(Lifetime.GetReference()) <= 4096))
			return false;
		const int64 Before = StartupUsed.load();
		const bool bReady = FStore::CalibrateRows(*Lifetime, StartupUsed, StartupLimit);
		Test.AddInfo(FString::Printf(TEXT("BudgetRowCalibration Request=%lld Alignment=%lld Capacity=%lld StartupLimit=%lld"),
			Lifetime->Probe.CalibrationRequest.load(), Lifetime->Probe.CalibrationAlignment.load(),
			Lifetime->Probe.CalibrationCapacity.load(), StartupLimit));
		Test.TestEqual(TEXT("retained calibration scratch freed"), Lifetime->Probe.LiveBacking.load(), int64(0));
		Test.TestEqual(TEXT("retained calibration credit returned"), StartupUsed.load(), Before);
		if (!Test.TestTrue(TEXT("one common controller calibration"), bReady))
			return false;
		using PendingC = SharedPointerInternals::TIntrusiveReferenceController<FPendingAccount, ESPMode::ThreadSafe>;
		if (!AdmitRoot(sizeof(PendingC), alignof(PendingC)))
			return false;
		Pending = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(PendingCount, PendingLimit);
		Pending->Lifetime = Lifetime;
		if (!AdmitRoot(sizeof(FBudgetAccount), alignof(FBudgetAccount)))
			return false;
		Budget = new FBudgetAccount(StoreLimit, 4096, Lifetime);
		if (!Test.TestTrue(TEXT("actual budget fits root allowance"), FMemory::GetAllocSize(Budget.GetReference()) <= 4096))
			return false;
		Lease.Emplace(*Budget);
		if (bRecreation && !Test.TestTrue(TEXT("two standby roots pre-admitted"), TryReserve(StartupUsed, StartupLimit, 2 * 4096)))
			return false;
		if (bRecreation)
			StandbyUsed = 2 * 4096;
		return true;
	}
	bool CompleteRecreation(FStore& Store, const FNormalizedLimits& Effective)
	{
		auto NextPending = NewPendingForRecreation(Effective.PendingBytes, Effective.PendingEvents);
		auto NextBudget = NewBudgetForRecreation(Effective.StoreBytes);
		if (!NextPending || !NextBudget || !Store.CompleteRecreation(Effective, *Budget, *Pending, NextBudget, NextPending.ToSharedRef()))
		{
			NextPending.Reset();
			NextBudget.SafeRelease();
			StartupUsed.fetch_sub(StandbyUsed);
			StandbyUsed = 0;
			return false;
		}
		Lease.Reset();
		Pending.Reset();
		Budget.SafeRelease();
		StartupUsed.fetch_sub(2 * 4096);
		Pending = NextPending;
		Budget = NextBudget;
		Lease.Emplace(*Budget);
		StandbyUsed = 0;
		return true;
	}
	TSharedPtr<FPendingAccount, ESPMode::ThreadSafe> NewPendingForRecreation(int64 ByteLimit, int64 CountLimit)
	{
		using PendingC = SharedPointerInternals::TIntrusiveReferenceController<FPendingAccount, ESPMode::ThreadSafe>;
		if (!CalibrateStandbyRoot(sizeof(PendingC), alignof(PendingC)))
			return {};
		auto Result = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(CountLimit, ByteLimit);
		Result->Lifetime = Lifetime;
		return Result;
	}
	TRefCountPtr<FBudgetAccount> NewBudgetForRecreation(int64 Limit)
	{
		if (!CalibrateStandbyRoot(sizeof(FBudgetAccount), alignof(FBudgetAccount)))
			return {};
		TRefCountPtr<FBudgetAccount> Result = new FBudgetAccount(Limit, 4096, Lifetime);
		if (!Test.TestTrue(TEXT("standby budget actual allocation fits allowance"), FMemory::GetAllocSize(Result.GetReference()) <= 4096))
			return {};
		return Result;
	}
	TRefCountPtr<FAllocationLifetime> Lifetime;
	TSharedPtr<FPendingAccount, ESPMode::ThreadSafe> Pending;
	TRefCountPtr<FBudgetAccount> Budget;
private:
	bool AdmitRoot(SIZE_T Request, uint32 Alignment)
	{
		if (!Test.TestTrue(TEXT("exact root request/alignment audited"), Request > 0 && Request <= 1024 && Alignment > 0 && Alignment <= 16 && (Alignment & (Alignment - 1)) == 0))
			return false;
		if (!Test.TestTrue(TEXT("fixture root pre-admitted"), TryReserve(StartupUsed, StartupLimit, 4096)))
			return false;
		FAllocationCounters Probe;
		FAllocationAdapter Scratch(Probe, GetSupportedAllocator());
		const int64 Before = StartupUsed.load();
		const bool bReady = Scratch.Calibrate(StartupUsed, StartupLimit, Request, Alignment);
		const int64 OrdinaryAlignment = Request <= 8 ? 8 : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
		Test.AddInfo(FString::Printf(TEXT("BudgetFixtureRoot Request=%lld Alignment=%lld Capacity=%lld StartupLimit=%lld"),
			Probe.CalibrationRequest.load(), Probe.CalibrationAlignment.load(), Probe.CalibrationCapacity.load(), StartupLimit));
		Test.TestEqual(TEXT("root calibration exact request"), Probe.CalibrationRequest.load(), int64(Request));
		Test.TestEqual(TEXT("root calibration exact ordinary alignment"), Probe.CalibrationAlignment.load(), OrdinaryAlignment);
		Test.TestEqual(TEXT("root scratch freed before credit return"), Probe.LiveBacking.load(), int64(0));
		Test.TestEqual(TEXT("root scratch allowance returned"), StartupUsed.load(), Before);
		if (!Test.TestTrue(TEXT("actual fixture root qualification"), bReady))
		{
			StartupUsed.fetch_sub(4096);
			return false;
		}
		return true;
	}
	bool CalibrateStandbyRoot(SIZE_T Request, uint32 Alignment)
	{
		FAllocationCounters Probe;
		FAllocationAdapter Scratch(Probe, GetSupportedAllocator());
		const int64 Before = StartupUsed.load();
		const bool bReady = Scratch.Calibrate(StartupUsed, StartupLimit, Request, Alignment);
		const int64 OrdinaryAlignment = Request <= 8 ? 8 : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
		Test.TestEqual(TEXT("standby exact request"), Probe.CalibrationRequest.load(), int64(Request));
		Test.TestEqual(TEXT("standby exact ordinary alignment"), Probe.CalibrationAlignment.load(), OrdinaryAlignment);
		Test.TestEqual(TEXT("standby scratch freed"), Probe.LiveBacking.load(), int64(0));
		Test.TestEqual(TEXT("standby allowance returned"), StartupUsed.load(), Before);
		return Test.TestTrue(TEXT("standby actual root calibration"), bReady);
	}
	FAutomationTestBase& Test;
	bool bRecreation = false;
	int64 StartupLimit = 0, StandbyUsed = 0;
	std::atomic<int64> StartupUsed{0};
	TOptional<FBudgetControlLease> Lease;
};

struct FCase
{
	FAutomationTestBase& Test;
	FRoots& Roots;
	FStore& Store;
	TUniquePtr<FCaptureQueue>& Queue;
	FReceiptClock& Clock;
	FCapturePolicy Policy;
	int64 FixedBytes = 0, FixedBacking = 0;
	uint64 FixedAccountIdentity = 0;
};

static FVaRestTraceEvent Started(uint64 Id, bool bPending = false, bool bHeaders = true)
{
	FVaRestTraceStarted V;
	V.RequestId = Id;
	V.ClientId = 9;
	V.ScopeId = 0;
	V.Verb = TEXT("POST");
	V.Url = TEXT("https://example.test/budget/{id}");
	V.PathTemplate = TEXT("/budget/{id}");
	V.Endpoint = TEXT("BudgetEndpoint");
	V.StartedAt = 1000 + Id;
	V.StartUtc = FDateTime(2026, 10, 25);
	V.CurrentAttempt = 1;
	V.bExcerptPending = bPending;
	V.Body.Bytes = 31;
	V.Body.ContentType = TEXT("text/plain");
	if (bHeaders)
		V.Headers.Set(TEXT("X-Start"), TEXT("start-value"));
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::Started;
	E.Seq = Id;
	E.Time = V.StartedAt;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Dispatch(uint64 Id, uint64 Physical, bool bHuge = false)
{
	FVaRestTraceAttemptDispatched V;
	V.RequestId = Id;
	V.DispatchId = Physical;
	V.Attempt = 1;
	V.AttemptStart = 2000 + Physical;
	V.GapKind = EVaRestTraceGap::Backoff;
	V.GapSeconds = 1;
	V.AuthRefreshId = 77;
	if (bHuge)
	{
		const FString Big = FString::ChrN(16384, TEXT('h'));
		for (int32 Index = 0; Index < 128; ++Index)
			V.Headers.Set(*FString::Printf(TEXT("X-Huge-%d"), Index), Big);
	}
	else
		V.Headers.Set(TEXT("X-Physical"), TEXT("physical-value"));
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::AttemptDispatched;
	E.Seq = Physical;
	E.Time = V.AttemptStart;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptDispatched>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent DroppedDispatch(uint64 Id, uint64 Physical)
{
	auto E = Dispatch(Id, Physical);
	auto& V = E.Payload.Get<FVaRestTraceAttemptDispatched>();
	const FString Big = FString::ChrN(128, TEXT('d'));
	for (int32 Index = 0; Index < 192; ++Index)
		V.Headers.Set(*FString::Printf(TEXT("X-Drop-%d"), Index), Big);
	return E;
}
static FVaRestTraceEvent End(uint64 Id, uint64 Physical, bool bPending = true)
{
	FVaRestTraceAttemptEnded V;
	V.RequestId = Id;
	V.DispatchId = Physical;
	V.Attempt = 1;
	V.Status = 200;
	V.BytesUp = 17;
	V.BytesDown = 31;
	V.bExcerptPending = bPending;
	V.Headers.Set(TEXT("X-End"), TEXT("end-value"));
	V.EffectiveUrl = TEXT("https://example.test/physical");
	V.ContentType = TEXT("text/plain");
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::AttemptEnded;
	E.Seq = Physical + 100;
	E.Time = 3000 + Physical;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptEnded>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Completed(uint64 Id, bool bHeaders = true)
{
	FVaRestTraceCompleted V;
	V.RequestId = Id;
	V.Outcome = EVaRestTraceOutcome::Success;
	V.Response.Status = 200;
	V.Response.BodyBytes = 31;
	V.Response.Storage = EVaRestTraceResponseStorage::Memory;
	V.Response.EffectiveUrl = TEXT("https://example.test/final");
	V.Response.ContentType = TEXT("application/json");
	if (bHeaders)
		V.Response.Headers.Set(TEXT("X-Final"), TEXT("final-value"));
	V.ResultType = TEXT("BudgetResult");
	V.DecodeStartAt = 4000;
	V.DecodeEndAt = 4001;
	V.Envelope.bApplied = true;
	V.Envelope.DataPath = TEXT("data");
	V.AuthRefreshId = 88;
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::Completed;
	E.Seq = 900 + Id;
	E.Time = 4002;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceCompleted>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Excerpt(uint64 Id, uint64 Physical = 0, uint8 Byte = 'x', int32 Reserve = 128)
{
	FVaRestTraceExcerptEvent V;
	V.RequestId = Id;
	V.DispatchId = Physical;
	V.Attempt = Physical ? 1 : 0;
	V.Owner = Physical ? EVaRestTraceExcerptOwner::Response : EVaRestTraceExcerptOwner::Request;
	V.Excerpt.bRedacted = true;
	V.Excerpt.TotalBytes = 31;
	auto Bytes = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
	Bytes->Reserve(Reserve);
	Bytes->Add(Byte);
	V.Excerpt.Bytes = Bytes;
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::Excerpt;
	E.Seq = Physical + 1000;
	E.Time = 5000 + Physical;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceExcerptEvent>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Note(uint64 Id, uint64 Seq)
{
	FVaRestTraceNote V;
	V.RequestId = Id;
	V.Kind = EVaRestTraceNoteKind::Queued;
	V.Attempt = 0;
	V.QueueId = FString::Printf(TEXT("queue-%llu"), Seq);
	V.Reason = TEXT("Offline");
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::Note;
	E.Seq = Seq;
	E.Time = 6000 + Seq;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceNote>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Client(uint64 Id, const TCHAR* Label)
{
	FVaRestTraceClient V;
	V.ClientId = Id;
	V.ScopeId = 0;
	V.DisplayName = Label;
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::Client;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceClient>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Marker(uint64 Cause)
{
	FVaRestTraceMarker V;
	V.Kind = EVaRestTraceMarkerKind::Offline;
	V.ScopeId = 0;
	V.ClientId = 9;
	V.CauseRequestId = Cause;
	V.Text = TEXT("offline");
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::Marker;
	E.Seq = 10000 + Cause;
	E.Time = 7000 + Cause;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceMarker>(), MoveTemp(V));
	return E;
}

static bool Deliver(FCase& C, const FVaRestTraceEvent& E, EPush Expected = EPush::Accepted, bool bRequireApplied = true,
	int64* OutStoreCharge = nullptr, bool* OutStoreApplied = nullptr)
{
	const EPush Result = C.Queue->TryPush(E);
	if (!C.Test.TestTrue(TEXT("real queue produces expected admission result"), Result == Expected))
		return false;
	auto Packet = E.Kind == EVaRestTraceKind::Excerpt ? C.Queue->TryPopExcerpt() : C.Queue->TryPopMetadata();
	if (!C.Test.TestTrue(TEXT("real admitted packet dequeued"), Packet.IsSet()))
		return false;
	if (OutStoreCharge)
	{
		const auto WrapperBound = C.Roots.Lifetime->Allocator.Bound(sizeof(FStorePacketOwner), alignof(FStorePacketOwner));
		int64 ExpectedCharge = Packet.GetValue().RetainedCapacity();
		const bool bMeasured = Packet.GetValue().IsValid() && WrapperBound.IsSet() && WrapperBound.GetValue() <= SIZE_T(MAX_int64) &&
			AddChecked(ExpectedCharge, int64(WrapperBound.GetValue()), MAX_int64);
		if (!C.Test.TestTrue(TEXT("independent store charge measured before handoff"), bMeasured))
			return false;
		*OutStoreCharge = ExpectedCharge;
	}
	const bool bApplied = C.Store.Apply(MoveTemp(Packet.GetValue()));
	if (OutStoreApplied)
		*OutStoreApplied = bApplied;
	return !bRequireApplied || C.Test.TestTrue(TEXT("real packet applied"), bApplied);
}
static bool RotateQueue(FCase& C, uint64 Generation)
{
	if (C.Queue)
	{
		C.Queue->Close();
		while (C.Queue->TryPopMetadata().IsSet()) {}
		while (C.Queue->TryPopExcerpt().IsSet()) {}
		C.Queue.Reset();
	}
	C.Queue.Reset(new FCaptureQueue(C.Policy, Generation, C.Roots.Pending.ToSharedRef(), C.Roots.Lifetime->Allocator));
	if (!C.Test.TestTrue(TEXT("new generation real queue supported"), C.Queue->IsSupported()) ||
		!C.Test.TestTrue(TEXT("new generation actual loss witness bound"), C.Store.BindLossWitness(C.Roots.Pending.ToSharedRef())))
		return false;
	C.Store.BeginGeneration(Generation, 0);
	return true;
}
static bool Take(FCase& C, FSnapshot& Out)
{
	if (!C.Test.TestTrue(TEXT("bounded snapshot transaction succeeds"), C.Store.TrySnapshot(Out)))
		return false;
	return C.Test.TestTrue(TEXT("published snapshot is valid"), Out.IsValid());
}
static int32 CountRows(const FStore& Store)
{
	int32 Count = 0;
	Store.ForEachRecord([&](const auto&) { ++Count; });
	return Count;
}
static int32 CountMarkers(const FStore& Store)
{
	int32 Count = 0;
	Store.ForEachRecord([&](const auto& Row) { Count += Row.Kind == EVaRestInspectorRecordKind::Marker ? 1 : 0; });
	return Count;
}
template <typename F>
static void VisitStore(FAutomationTestBase& Test, const FStore& Store, uint64 Id, F&& Check)
{
	int32 Found = 0;
	Test.TestTrue(TEXT("store enumeration succeeds"), Store.ForEachRecord([&](const auto& Row) {
		if (Row.RequestId == Id || (Id == 0 && Row.Kind == EVaRestInspectorRecordKind::Marker))
		{
			++Found;
			Check(Row);
		}
	}));
	Test.TestEqual(TEXT("exactly one expected store row"), Found, 1);
}
template <typename F>
static void VisitSnapshot(FAutomationTestBase& Test, const FVaRestInspectorSnapshot& Snapshot, uint64 Id, F&& Check)
{
	int32 Found = 0;
	Snapshot.ForEachRecord([&](const auto& Row) {
		if (Row.RequestId == Id || (Id == 0 && Row.Kind == EVaRestInspectorRecordKind::Marker))
		{
			++Found;
			Check(Row);
		}
	});
	Test.TestEqual(TEXT("exactly one expected snapshot row"), Found, 1);
}

template <typename F>
static bool WithStore(FAutomationTestBase& Test, const TCHAR* Name, F&& Body, int32 MaxRows = 4,
	int64 StoreLimit = 64ll * 1024 * 1024, int64 PendingLimit = 4ll * 1024 * 1024, bool bRecreation = false, int32 BodyBytes = 0)
{
	Test.AddInfo(Name);
	FCapturePolicy Policy;
	if (BodyBytes > 0)
		Policy.Limits.BodyBytes = BodyBytes;
	Policy.Limits.MaxRecords = MaxRows;
	Policy.Limits.PendingEvents = 32;
	Policy.Limits.PendingBytes = PendingLimit;
	Policy.Limits.StoreBytes = StoreLimit;
	FRoots Roots(Test, bRecreation);
	if (!Roots.Initialize(StoreLimit, PendingLimit, Policy.Limits.PendingEvents))
		return false;
	auto Clock = MakeShared<FReceiptClock, ESPMode::ThreadSafe>();
	bool bResult = false;
	{
		FStore Store(Policy.Limits, Clock);
		if (!Test.TestTrue(TEXT("budget case store initialized"), Store.Initialize(Roots.Budget)))
			return false;
		Store.BeginGeneration(1, 0);
		const int64 FixedBytes = Roots.Budget->UsedBytes.load();
		const int64 FixedBacking = Roots.Lifetime->Probe.LiveBacking.load();
		TUniquePtr<FCaptureQueue> Queue(new FCaptureQueue(Policy, 1, Roots.Pending.ToSharedRef(), Roots.Lifetime->Allocator));
		if (!Test.TestTrue(TEXT("budget case real queue supported"), Queue->IsSupported()))
			return false;
		if (!Test.TestTrue(TEXT("actual pending loss witness bound"), Store.BindLossWitness(Roots.Pending.ToSharedRef())))
			return false;
		Store.BeginGeneration(1, 0);
		FCase C{Test, Roots, Store, Queue, *Clock, Policy, FixedBytes, FixedBacking, Roots.Budget->Identity};
		bResult = Body(C);
		if (Queue)
		{
			Queue->Close();
			while (Queue->TryPopMetadata().IsSet()) {}
			while (Queue->TryPopExcerpt().IsSet()) {}
			Queue.Reset();
		}
		Test.TestEqual(TEXT("all pending packet bytes drained"), Roots.Pending->Bytes.load(), int64(0));
		Test.TestEqual(TEXT("all pending packet counts drained"), Roots.Pending->Count.load(), int64(0));
		Test.TestEqual(TEXT("no excerpt reservation survives case"), Roots.Pending->ExcerptReservations.load(), int64(0));
		const bool bReleased = Store.Diagnostics().bPendingLimits;
		Store.Clear();
		Test.TestEqual(TEXT("fixed baseline belongs to current account"), Roots.Budget->Identity, C.FixedAccountIdentity);
		if (bReleased)
		{
			Test.TestEqual(TEXT("pending-limit Clear releases fixed credit"), Roots.Budget->UsedBytes.load(), Roots.Budget->BootstrapBytes);
			Test.TestEqual(TEXT("pending-limit Clear releases fixed backing"), Roots.Lifetime->Probe.LiveBacking.load(), int64(0));
		}
		else
		{
			Test.TestEqual(TEXT("case Clear returns exact fixed credit"), Roots.Budget->UsedBytes.load(), C.FixedBytes);
			Test.TestEqual(TEXT("case Clear returns exact fixed backing"), Roots.Lifetime->Probe.LiveBacking.load(), C.FixedBacking);
		}
		Test.TestEqual(TEXT("case has no surviving controller"), Roots.Budget->Probe.LiveControllers.load(), int64(0));
		Test.TestEqual(TEXT("case has no surviving weak slot"), Roots.Budget->SnapshotControllers.load(), int64(0));
		Test.TestTrue(TEXT("store peak bounded"), Roots.Budget->PeakBytes.load() <= Roots.Budget->Limit);
		Test.TestTrue(TEXT("pending peak bounded"), Roots.Pending->PeakBytes.load() <= Roots.Pending->ByteLimit);
	}
	Test.TestEqual(TEXT("store destruction releases fixed backing"), Roots.Lifetime->Probe.LiveBacking.load(), int64(0));
	Test.TestEqual(TEXT("store destruction leaves admitted bootstrap"), Roots.Budget->UsedBytes.load(), Roots.Budget->BootstrapBytes);
	return bResult;
}

// Bounded owning expected data for the added Started-only proof cases.
// These strings are upstream test data; no borrowed view escapes a visitor.
struct FStartedProofImage
{
	FString StartedValues, HeaderValues, RowValues;
	const FPacketFixed* FixedIdentity = nullptr;
	const uint8* BackingIdentity = nullptr;
	uint64 Revision = 0;
	EVaRestInspectorHeadersState HeadersState = EVaRestInspectorHeadersState::Unknown;
	template <typename T> static void Add(FString& Out, T Value) { Out.Appendf(TEXT("%llu|"), uint64(Value)); }
	static void Add(FString& Out, double Value) { Out.Appendf(TEXT("%.17g|"), Value); }
	static void Add(FString& Out, FStringView Value)
	{
		Out.Appendf(TEXT("%d:"), Value.Len());
		if (Value.Len()) Out.AppendChars(Value.GetData(), Value.Len());
		Out += TEXT("|");
	}
	static void Add(FString& Out, const FString& Value) { Add(Out, FStringView(Value)); }
	static void Pairs(FString& Out, const FVaRestInspectorPacketView& P, FArraySlice Slice)
	{
		const auto Values = P.Pairs(Slice); Add(Out, Values.Num());
		for (const auto& Pair : Values) { Add(Out, P.Read(Pair.First)); Add(Out, P.Read(Pair.Second)); }
	}
	static FString Packet(const FVaRestInspectorPacketView& P, bool bHeaders)
	{
		FString Out; Add(Out, P.Fixed != nullptr);
		if (!P.Fixed) return Out;
		const auto& H = P.Fixed->Header;
		Add(Out, H.Kind); Add(Out, H.Source); Add(Out, H.Seq); Add(Out, H.Generation); Add(Out, H.MetadataOrdinal);
		Add(Out, H.Time); Add(Out, H.bSnapshot); Add(Out, H.bMetadataOmitted);
		const auto& S = P.Fixed->Scalars;
#define BUDGET_PROOF_SCALAR(Field) Add(Out, S.Field)
		BUDGET_PROOF_SCALAR(RequestId); BUDGET_PROOF_SCALAR(DispatchId); BUDGET_PROOF_SCALAR(ScopeId); BUDGET_PROOF_SCALAR(ClientId);
		BUDGET_PROOF_SCALAR(OriginalRequestId); BUDGET_PROOF_SCALAR(AuthRefreshId); BUDGET_PROOF_SCALAR(CauseRequestId);
		BUDGET_PROOF_SCALAR(SocketId); BUDGET_PROOF_SCALAR(FrameId); BUDGET_PROOF_SCALAR(Attempt); BUDGET_PROOF_SCALAR(CurrentAttempt);
		BUDGET_PROOF_SCALAR(Status); BUDGET_PROOF_SCALAR(ErrorHttpStatus); BUDGET_PROOF_SCALAR(ScriptCodeOffset); BUDGET_PROOF_SCALAR(SourceLine);
		BUDGET_PROOF_SCALAR(PIEInstance); BUDGET_PROOF_SCALAR(DecodeWarningCount); BUDGET_PROOF_SCALAR(BodyBytes); BUDGET_PROOF_SCALAR(BytesUp);
		BUDGET_PROOF_SCALAR(BytesDown); BUDGET_PROOF_SCALAR(NoteBytes); BUDGET_PROOF_SCALAR(DeliveredBodyBytes); BUDGET_PROOF_SCALAR(TotalBytes);
		BUDGET_PROOF_SCALAR(StartedAt); BUDGET_PROOF_SCALAR(AttemptStart); BUDGET_PROOF_SCALAR(GapSeconds); BUDGET_PROOF_SCALAR(AuthWait);
		BUDGET_PROOF_SCALAR(LimiterWait); BUDGET_PROOF_SCALAR(UploadDoneAt); BUDGET_PROOF_SCALAR(StatusAt); BUDGET_PROOF_SCALAR(TransportDoneAt);
		BUDGET_PROOF_SCALAR(DecodeStartAt); BUDGET_PROOF_SCALAR(DecodeEndAt); BUDGET_PROOF_SCALAR(Delay); BUDGET_PROOF_SCALAR(RetryAfter);
		Add(Out, S.StartUtc.GetTicks()); Add(Out, S.Endpoint.ToString()); Add(Out, S.Tag.ToString()); Add(Out, S.CachePolicy.ToString());
		Add(Out, S.FailureDetail.ToString()); Add(Out, S.ResultType.ToString()); Add(Out, S.Reason.ToString());
		BUDGET_PROOF_SCALAR(Priority); BUDGET_PROOF_SCALAR(Flags); BUDGET_PROOF_SCALAR(BodyKind); BUDGET_PROOF_SCALAR(CacheLookup);
		BUDGET_PROOF_SCALAR(GapKind); BUDGET_PROOF_SCALAR(Failure); BUDGET_PROOF_SCALAR(Outcome); BUDGET_PROOF_SCALAR(CancelReason);
		BUDGET_PROOF_SCALAR(ErrorKind); BUDGET_PROOF_SCALAR(Storage); BUDGET_PROOF_SCALAR(NoteKind); BUDGET_PROOF_SCALAR(Owner);
		BUDGET_PROOF_SCALAR(ExcerptKind); BUDGET_PROOF_SCALAR(Skip); BUDGET_PROOF_SCALAR(MarkerKind); BUDGET_PROOF_SCALAR(bCacheLookupPending);
		BUDGET_PROOF_SCALAR(bExcerptPending); BUDGET_PROOF_SCALAR(bConditional); BUDGET_PROOF_SCALAR(bFromCache); BUDGET_PROOF_SCALAR(bEnvelopeApplied);
		BUDGET_PROOF_SCALAR(bTruncated); BUDGET_PROOF_SCALAR(bRedacted); BUDGET_PROOF_SCALAR(bOpened); BUDGET_PROOF_SCALAR(bEditorScope); BUDGET_PROOF_SCALAR(bCreated);
#undef BUDGET_PROOF_SCALAR
		for (int32 Index = 0; Index < int32(EText::Count); ++Index) Add(Out, P.Text(EText(Index)));
		if (bHeaders) Pairs(Out, P, P.Fixed->Headers);
		Pairs(Out, P, P.Fixed->Warnings);
		Add(Out, P.Excerpt.Num()); for (uint8 Byte : P.Excerpt) Add(Out, Byte);
		return Out;
	}
	static FString Row(const FVaRestInspectorRecordView& R)
	{
		FString Out;
#define BUDGET_PROOF_ROW(Field) Add(Out, R.Field)
		BUDGET_PROOF_ROW(Kind); BUDGET_PROOF_ROW(RequestId); BUDGET_PROOF_ROW(ScopeId); BUDGET_PROOF_ROW(ClientId);
		BUDGET_PROOF_ROW(ArrivalOrdinal); BUDGET_PROOF_ROW(BirthMetadataOrdinal); BUDGET_PROOF_ROW(CurrentAttempt); BUDGET_PROOF_ROW(StartedAt);
		Add(Out, R.StartUtc.GetTicks()); BUDGET_PROOF_ROW(bStartedAtKnown); BUDGET_PROOF_ROW(bStartUtcKnown); BUDGET_PROOF_ROW(bInitiatorKnown);
		BUDGET_PROOF_ROW(bMetadataOmitted); Add(Out, R.ScopeLabel); Add(Out, R.ClientLabel); BUDGET_PROOF_ROW(DispatchHistory); BUDGET_PROOF_ROW(EndState);
		BUDGET_PROOF_ROW(bPartial); BUDGET_PROOF_ROW(bPinned); BUDGET_PROOF_ROW(bSealed); BUDGET_PROOF_ROW(bTerminalKnown);
		BUDGET_PROOF_ROW(ObservedDispatches); BUDGET_PROOF_ROW(ObservedEnds); BUDGET_PROOF_ROW(OmittedDispatches); BUDGET_PROOF_ROW(OmittedEnds);
		BUDGET_PROOF_ROW(OmittedExcerpts); BUDGET_PROOF_ROW(OmittedNotes); BUDGET_PROOF_ROW(OmittedTimeline);
		BUDGET_PROOF_ROW(WireBytesUp); BUDGET_PROOF_ROW(WireBytesDown); BUDGET_PROOF_ROW(bWireBytesUnknown); BUDGET_PROOF_ROW(bCountersSaturated);
		BUDGET_PROOF_ROW(FinalStatus); BUDGET_PROOF_ROW(FinalBodyBytes); BUDGET_PROOF_ROW(FinalStorage); BUDGET_PROOF_ROW(CompletedAt);
		BUDGET_PROOF_ROW(DecodeStartAt); BUDGET_PROOF_ROW(DecodeEndAt); BUDGET_PROOF_ROW(bActualCompletionObserved); BUDGET_PROOF_ROW(CompletedObservedAt);
		BUDGET_PROOF_ROW(CompletedMetadataState); BUDGET_PROOF_ROW(FinalPreviewState); BUDGET_PROOF_ROW(RequestExcerptState);
#undef BUDGET_PROOF_ROW
		const auto AddPhase = [&Out](const FVaRestInspectorPhase& Phase) { Add(Out, Phase.bKnown); Add(Out, Phase.Seconds); };
		AddPhase(R.Total); AddPhase(R.Decode); AddPhase(R.Deliver);
		Add(Out, R.Dispatches.Num()); Add(Out, R.DispatchEvents.Num()); Add(Out, R.Notes.Num());
		Add(Out, Packet(R.Completed, true)); Add(Out, Packet(R.Marker, true));
		Add(Out, R.Excerpts.Num()); for (const auto& P : R.Excerpts) Add(Out, Packet(P, true));
		Add(Out, R.Timeline.Num());
		for (const auto& T : R.Timeline) { Add(Out, T.Kind); Add(Out, T.Seq); Add(Out, T.Time); Add(Out, T.Attempt); Add(Out, T.DispatchId); Add(Out, T.DetailState); }
		return Out;
	}
	bool Capture(FAutomationTestBase& Test, const FVaRestInspectorRecordView& R)
	{
		if (!Test.TestNotNull(TEXT("proof Started owner exists"), R.Started.Fixed) ||
			!Test.TestTrue(TEXT("proof fixture has no unrelated detail domain"), R.Dispatches.IsEmpty() && R.DispatchEvents.IsEmpty() && R.Notes.IsEmpty() && !R.Completed.Fixed && !R.Marker.Fixed)) return false;
		StartedValues = Packet(R.Started, false); RowValues = Row(R); HeaderValues.Reset();
		Pairs(HeaderValues, R.Started, R.Started.Fixed->Headers);
		FixedIdentity = R.Started.Fixed; BackingIdentity = R.Started.Backing.GetData(); Revision = R.Revision; HeadersState = R.StartedHeadersState;
		return true;
	}
	bool Check(FAutomationTestBase& Test, const FVaRestInspectorRecordView& R, bool bCompacted, bool bSameOwner, bool bSameRevision) const
	{
		if (!Test.TestNotNull(TEXT("proof Started remains present"), R.Started.Fixed)) return false;
		bool b = Test.TestEqualSensitive(TEXT("every non-header Started field preserved"), Packet(R.Started, false), StartedValues);
		b &= Test.TestEqualSensitive(TEXT("base scalars promises excerpts and timeline preserved"), Row(R), RowValues);
		b &= Test.TestEqual(TEXT("Started header availability agrees with actual operation"), R.StartedHeadersState, bCompacted ? EVaRestInspectorHeadersState::Evicted : HeadersState);
		if (bCompacted) b &= Test.TestEqual(TEXT("only header pairs removed by compact"), R.Started.Fixed->Headers.Count, uint32(0));
		else { FString ActualHeaders; Pairs(ActualHeaders, R.Started, R.Started.Fixed->Headers); b &= Test.TestEqualSensitive(TEXT("ordered original header pairs preserved"), ActualHeaders, HeaderValues); }
		if (bSameOwner) b &= Test.TestTrue(TEXT("refused compact keeps exact source owner"), R.Started.Fixed == FixedIdentity && R.Started.Backing.GetData() == BackingIdentity);
		if (bSameRevision) b &= Test.TestEqual(TEXT("unchanged source row revision"), R.Revision, Revision);
		else b &= Test.TestTrue(TEXT("successful compact touches source revision"), R.Revision > Revision);
		return b;
	}
};
static FVaRestTraceEvent RichProofStarted(uint64 Id)
{
	auto E = Started(Id, true);
	auto& V = E.Payload.Get<FVaRestTraceStarted>();
	V.ClientId = 29; V.Endpoint = TEXT("CompactEndpoint"); V.Tag = TEXT("CompactTag"); V.Priority = EVaRestPriority::High;
	V.Flags = EVaRestTraceRequestFlags::Idempotent | EVaRestTraceRequestFlags::QueueWhenOffline | EVaRestTraceRequestFlags::NoAuth;
	V.QueueReplayOf.QueueId = TEXT("21D2AE25-219E-44E9-8C71-54A90FF6C401"); V.QueueReplayOf.OriginalRequestId = 4321;
	V.Initiator.OwnerPath = TEXT("/Test/CompactOwner"); V.Initiator.ScriptObject = TEXT("CompactObject");
	V.Initiator.ScriptFunction = TEXT("CompactFunction"); V.Initiator.ScriptCodeOffset = 17;
	V.Initiator.SourceFile = TEXT("compact-fixture.cpp"); V.Initiator.SourceLine = 97;
	V.CachePolicy = TEXT("CompactPolicy"); V.CacheLookup = EVaRestTraceCacheLookup::Revalidate; V.bCacheLookupPending = true;
	V.Body.Bytes = 719; V.Body.Kind = EVaRestBodyKind::Text; V.Body.ContentType = TEXT("application/proof-body");
	const FString HeaderValue = FString::ChrN(2048, TEXT('r'));
	for (int32 Index = 0; Index < 32; ++Index) V.Headers.Set(*FString::Printf(TEXT("X-Proof-%d"), Index), HeaderValue);
	return E;
}
static bool MeasureProofCompact(FCase& C, uint64 Id, int64& OutCharge)
{
	bool bMeasured = false;
	VisitStore(C.Test, C.Store, Id, [&](const auto& R) {
		if (!C.Test.TestNotNull(TEXT("compact measurement has actual source"), R.Started.Fixed)) return;
		int64 Request = int64(Align(sizeof(FPacketFixed), alignof(TCHAR)));
		for (const auto& Slice : R.Started.Fixed->Text)
		{
			if (!Slice.Chars) continue;
			if (!C.Test.TestTrue(TEXT("retained slice and terminator in actual source extent"), Slice.Offset % alignof(TCHAR) == 0 &&
				uint64(Slice.Offset) + (uint64(Slice.Chars) + 1) * sizeof(TCHAR) <= uint64(R.Started.Backing.Num())) ||
				!C.Test.TestTrue(TEXT("checked compact text measurement"), AddChecked(Request, (int64(Slice.Chars) + 1) * sizeof(TCHAR), FNormalizedLimits::MaxMetadataBytes))) return;
		}
		const auto Backing = C.Roots.Lifetime->Allocator.Bound(SIZE_T(Request), alignof(FPacketFixed));
		const auto Wrapper = C.Roots.Lifetime->Allocator.Bound(sizeof(FStorePacketOwner), alignof(FStorePacketOwner));
		if (!C.Test.TestTrue(TEXT("complete independent compact bounds exist"), Backing.IsSet() && Wrapper.IsSet())) return;
		OutCharge = int64(Backing.GetValue());
		bMeasured = C.Test.TestTrue(TEXT("checked compact wrapper sum"), AddChecked(OutCharge, int64(Wrapper.GetValue()), MAX_int64));
	});
	return bMeasured;
}

} // namespace VaRest::Inspector::Private::BudgetTesting

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorStoreBudgetTest,
	"VaRest.Inspector.Store.Budget.Admission", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorStoreBudgetTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	using namespace VaRest::Inspector::Private::BudgetTesting;
	bool bAll = true;

	bAll &= WithStore(*this, TEXT("Budget: count saturation evicts oldest eligible row without resetting retained snapshots"), [](FCase& C) {
		bool b = Deliver(C, Started(1));
		b &= Deliver(C, Started(2));
		b &= Deliver(C, Started(3), EPush::Accepted, false);
		if (!C.Test.TestTrue(TEXT("third pinned row refused at count cap"), CountRows(C.Store) == 2)) return false;
		VisitStore(C.Test, C.Store, 1, [&](const auto& Row) {
			C.Test.TestTrue(TEXT("pinned oldest survives count refusal"), Row.bPinned);
			C.Test.TestEqual(TEXT("pinned oldest arrival remains first"), Row.ArrivalOrdinal, uint64(1));
		});
		b &= Deliver(C, Completed(1));
		FSnapshot Old;
		if (!Take(C, Old)) return false;
		b &= Deliver(C, Started(3));
		if (!C.Test.TestTrue(TEXT("completed oldest is FIFO evicted"), CountRows(C.Store) == 2)) return false;
		VisitStore(C.Test, C.Store, 2, [&](const auto& Row) { C.Test.TestTrue(TEXT("newer pinned row preserved"), Row.bPinned && !Row.bSealed); });
		VisitStore(C.Test, C.Store, 3, [&](const auto& Row) { C.Test.TestTrue(TEXT("new row admitted after eligible removal"), Row.RequestId == 3); });
		VisitSnapshot(C.Test, *Old, 1, [&](const auto& Row) { C.Test.TestTrue(TEXT("retained completed row immutable"), Row.bSealed && Row.bTerminalKnown); });
		C.Store.Clear();
		C.Test.TestTrue(TEXT("snapshot controller remains charged across Clear"), C.Roots.Budget->SnapshotControllers.load() > 0);
		Old.Reset();
		return b;
	}, 2);

	bAll &= WithStore(*this, TEXT("Budget: allocated excerpt capacity and byte saturation with count slack"), [](FCase& C) {
		if (!Deliver(C, Started(1))) return false;
		const int64 Before = C.Roots.Budget->UsedBytes.load();
		int64 Charge = 0;
		if (!Deliver(C, Excerpt(1, 0, 'q', 128), EPush::Accepted, true, &Charge)) return false;
		C.Test.TestEqual(TEXT("store retains backing plus actual excerpt allocated capacity and wrapper"), C.Roots.Budget->UsedBytes.load(), Before + Charge);
		FSnapshot Old;
		if (!Take(C, Old)) return false;
		C.Store.Clear();
		const int64 Held = C.Roots.Budget->UsedBytes.load();
		C.Test.TestTrue(TEXT("retained snapshot keeps excerpt charge across Clear"), Held >= Before + Charge);
		if (!RotateQueue(C, 2)) return false;
		if (!Deliver(C, Started(2, false, false))) return false;
		const int64 BeforeRefusal = C.Roots.Budget->UsedBytes.load();
		const auto LargeExcerpt = Excerpt(2, 0, 'z', 2 * 1024 * 1024);
		FPacketLayout LargeLayout;
		const EMeasure LargeMeasured = FPacketCodec::Measure(LargeExcerpt, C.Policy, C.Roots.Lifetime->Allocator, LargeLayout);
		if (!C.Test.TestTrue(TEXT("large excerpt has actual captured capacity"), LargeMeasured == EMeasure::Full && LargeLayout.ExcerptCapacity >= 2 * 1024 * 1024)) return false;
		if (!C.Test.TestTrue(TEXT("byte refusal has actual row-count slack"), FStoreAccess::Plan(C.Store).MaxRows > CountRows(C.Store))) return false;
		bool bApplied = true;
		if (!Deliver(C, LargeExcerpt, EPush::Accepted, false, nullptr, &bApplied)) return false;
		C.Test.TestFalse(TEXT("large excerpt store admission refused on bytes with count slack"), bApplied);
		C.Test.TestEqual(TEXT("refusal did not allocate or reset retained charge"), C.Roots.Budget->UsedBytes.load(), BeforeRefusal);
		C.Test.TestEqual(TEXT("only the new base row remains after refused excerpt"), CountRows(C.Store), 1);
		Old.Reset();
		return true;
	}, 2, 1024 * 1024, 4 * 1024 * 1024, false, 2 * 1024 * 1024);

	bAll &= WithStore(*this, TEXT("Budget: globally bounded clients stats links markers and actual free"), [](FCase& C) {
		const int64 Fixed = C.Roots.Budget->UsedBytes.load();
		const int64 FixedBacking = C.Roots.Lifetime->Probe.LiveBacking.load();
		for (int32 Index = 0; Index < 300; ++Index)
		{
			const FString Text = FString::Printf(TEXT("key-%d"), Index);
			const FStatsKeyView Key{0, 1, NAME_None, FStringView(Text)};
			const FStatsGroup* Group = C.Store.FindOrAdmitStatsGroup(Key, {});
			if (!C.Test.TestNotNull(TEXT("real stats group admitted"), Group)) return false;
			if (Index >= 256 && !C.Test.TestTrue(TEXT("per-client overflow selected after 256 keys"), Group->bOverflow)) return false;
		}
		C.Test.TestEqual(TEXT("one client has 256 named groups plus one overflow"), FStoreAccess::NumStats(C.Store), 257);
		for (uint64 Client = 2; Client <= 1100; ++Client)
		{
			const FString Text = FString::Printf(TEXT("key-%llu"), Client);
			const FStatsKeyView Key{0, Client, NAME_None, FStringView(Text)};
			const FStatsGroup* Group = C.Store.FindOrAdmitStatsGroup(Key, {});
			if (!C.Test.TestNotNull(TEXT("global stats group admitted"), Group)) return false;
		}
		C.Test.TestEqual(TEXT("global stats cap includes exactly one overflow"), FStoreAccess::NumStats(C.Store), 1024);
		const FString RejectedText = TEXT("rejected-key");
		const FStatsGroup* Rejected = C.Store.FindOrAdmitStatsGroup({0, 2000, NAME_None, FStringView(RejectedText)}, {});
		if (!C.Test.TestNotNull(TEXT("overflow group exists after global cap"), Rejected) || !C.Test.TestTrue(TEXT("rejected key uses shared overflow"), Rejected->bOverflow)) return false;
		for (uint64 Id = 1; Id <= 1100; ++Id)
		{
			const FString Label = FString::Printf(TEXT("client-%llu"), Id);
			if (!Deliver(C, Client(Id, *Label))) return false;
		}
		C.Test.TestEqual(TEXT("client table capped at 1024"), FStoreAccess::NumClients(C.Store), 1024);
		if (!Deliver(C, Client(2000, TEXT("overflow-client")), EPush::Accepted, true)) return false;
		C.Test.TestTrue(TEXT("overflow client label omitted visibly"), C.Store.Diagnostics().OmittedClients > 0);
		for (uint64 Id = 1; Id <= 5000; ++Id)
		{
			const FString QueueId = FString::Printf(TEXT("queue-%llu"), Id);
			const FLinkSeed Seed{Id, Id % 64 + 1000, FStringView(QueueId), false};
			const ELinkAdmission Result = C.Store.TryAdmitLink(Seed, {});
			if (!C.Test.TestTrue(TEXT("real link admitted or replaced"), Result == ELinkAdmission::Added || Result == ELinkAdmission::ReplacedOldest)) return false;
		}
		C.Test.TestEqual(TEXT("global link cap enforced"), FStoreAccess::NumLinks(C.Store), 4096);
		FSnapshot EmptyLinks;
		if (!Take(C, EmptyLinks)) return false;
		C.Test.TestEqual(TEXT("all 4096 independently retained links published"), EmptyLinks->GetLinks().Num(), 4096);
		if (EmptyLinks->GetLinks().Num() != 4096) return false;
		for (int32 Index = 0; Index < 4096; ++Index)
		{
			const uint64 From = uint64(Index) + 905;
			const auto& Link = EmptyLinks->GetLinks()[Index];
			C.Test.TestEqual(TEXT("surviving link source FIFO identity"), Link.FromRequestId, From);
			C.Test.TestEqual(TEXT("surviving link exact target"), Link.ToRequestId, From % 64 + 1000);
			C.Test.TestEqualSensitive(TEXT("surviving link owned QueueId"), Link.QueueId, FString::Printf(TEXT("queue-%llu"), From));
			C.Test.TestFalse(TEXT("manual queue link is not auth"), Link.bAuth);
			C.Test.TestFalse(TEXT("absent target without tombstone stays unknown"), Link.bTargetEvicted);
		}
		EmptyLinks.Reset();
		for (uint64 Cause = 1; Cause <= 20; ++Cause)
			if (!Deliver(C, Marker(Cause))) return false;
		C.Test.TestEqual(TEXT("marker rows share bounded arrival storage"), CountMarkers(C.Store), 2);
		C.Store.Clear();
		C.Test.TestEqual(TEXT("auxiliary actual store credit returns to fixed baseline"), C.Roots.Budget->UsedBytes.load(), Fixed);
		C.Test.TestEqual(TEXT("auxiliary actual backing returns to fixed baseline"), C.Roots.Lifetime->Probe.LiveBacking.load(), FixedBacking);
		C.Test.TestEqual(TEXT("only overflow stats remains after Clear"), FStoreAccess::NumStats(C.Store), 1);
		C.Test.TestEqual(TEXT("links freed after Clear"), FStoreAccess::NumLinks(C.Store), 0);
		C.Test.TestEqual(TEXT("clients freed after Clear"), FStoreAccess::NumClients(C.Store), 0);
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Budget: client label charges exact equality update and refusal preserve numeric identity"), [](FCase& C) {
		const int64 Fixed = C.Roots.Budget->UsedBytes.load();
		const FString Exact = TEXT("exact-client");
		const auto ExactBound = C.Roots.Lifetime->Allocator.Bound((SIZE_T(Exact.Len()) + 1) * sizeof(TCHAR), alignof(TCHAR));
		if (!C.Test.TestTrue(TEXT("exact client label bound exists"), ExactBound.IsSet())) return false;
		const int64 PressureBytes = C.Roots.Budget->Limit - C.Roots.Budget->UsedBytes.load() - int64(ExactBound.GetValue());
		if (!C.Test.TestTrue(TEXT("exact equality pressure fits"), PressureBytes > 0)) return false;
		auto ExactPressure = FStoreCredit::Acquire(C.Roots.Budget, PressureBytes);
		if (!C.Test.TestTrue(TEXT("exact equality pressure admitted"), ExactPressure.IsSet())) return false;
		if (!Deliver(C, Client(1, *Exact))) return false;
		C.Test.TestEqual(TEXT("exact remaining label charge succeeds"), C.Roots.Budget->UsedBytes.load(), C.Roots.Budget->Limit);
		ExactPressure.Reset();
		int64 Expected = int64(ExactBound.GetValue());
		C.Test.TestEqual(TEXT("first label owns its independent certified charge"), C.Roots.Budget->UsedBytes.load(), Fixed + Expected);
		C.Test.TestTrue(TEXT("numeric client one resolves exact label"), FStoreAccess::ClientLabel(C.Store, 1) == FStringView(Exact));
		const FString Second = TEXT("second-client-with-a-longer-label");
		const FString Third = TEXT("third");
		const FString Labels[] = {Second, Third};
		for (int32 Index = 0; Index < 2; ++Index)
		{
			const auto Bound = C.Roots.Lifetime->Allocator.Bound((SIZE_T(Labels[Index].Len()) + 1) * sizeof(TCHAR), alignof(TCHAR));
			if (!C.Test.TestTrue(TEXT("unique client label bound exists"), Bound.IsSet())) return false;
			Expected += int64(Bound.GetValue());
			if (!Deliver(C, Client(uint64(Index + 2), *Labels[Index]))) return false;
			C.Test.TestEqual(TEXT("unique client labels own their exact aggregate charge"), C.Roots.Budget->UsedBytes.load(), Fixed + Expected);
			C.Test.TestTrue(TEXT("unique numeric client resolves its own label"), FStoreAccess::ClientLabel(C.Store, uint64(Index + 2)) == FStringView(Labels[Index]));
		}
		const FString Updated = TEXT("updated-second-client-label-with-new-length");
		const auto UpdatedBound = C.Roots.Lifetime->Allocator.Bound((SIZE_T(Updated.Len()) + 1) * sizeof(TCHAR), alignof(TCHAR));
		const auto SecondBound = C.Roots.Lifetime->Allocator.Bound((SIZE_T(Second.Len()) + 1) * sizeof(TCHAR), alignof(TCHAR));
		if (!C.Test.TestTrue(TEXT("updated and previous label bounds exist"), UpdatedBound.IsSet() && SecondBound.IsSet())) return false;
		Expected += int64(UpdatedBound.GetValue()) - int64(SecondBound.GetValue());
		if (!Deliver(C, Client(2, *Updated))) return false;
		C.Test.TestEqual(TEXT("label update replaces exactly its old charge"), C.Roots.Budget->UsedBytes.load(), Fixed + Expected);
		C.Test.TestTrue(TEXT("updated numeric client resolves new label"), FStoreAccess::ClientLabel(C.Store, 2) == FStringView(Updated));
		C.Test.TestTrue(TEXT("other numeric client label remains unchanged"), FStoreAccess::ClientLabel(C.Store, 1) == FStringView(Exact));
		C.Test.TestTrue(TEXT("unknown numeric client has no fabricated label"), FStoreAccess::ClientLabel(C.Store, 999).IsEmpty());
		const FString Refused = TEXT("refused-replacement-label");
		const auto RefusedBound = C.Roots.Lifetime->Allocator.Bound((SIZE_T(Refused.Len()) + 1) * sizeof(TCHAR), alignof(TCHAR));
		if (!C.Test.TestTrue(TEXT("refused label bound exists"), RefusedBound.IsSet() && RefusedBound.GetValue() > 1)) return false;
		const int64 BeforeRefusal = C.Roots.Budget->UsedBytes.load();
		const int64 RefusalPressureBytes = C.Roots.Budget->Limit - BeforeRefusal - (int64(RefusedBound.GetValue()) - 1);
		if (!C.Test.TestTrue(TEXT("one-byte-less pressure fits"), RefusalPressureBytes > 0)) return false;
		auto RefusalPressure = FStoreCredit::Acquire(C.Roots.Budget, RefusalPressureBytes);
		if (!C.Test.TestTrue(TEXT("one-byte-less pressure admitted"), RefusalPressure.IsSet())) return false;
		const uint64 OmittedBefore = C.Store.Diagnostics().OmittedClients;
		if (!Deliver(C, Client(2, *Refused))) return false;
		C.Test.TestEqual(TEXT("label refusal preserves numeric client count"), FStoreAccess::NumClients(C.Store), 3);
		C.Test.TestTrue(TEXT("label refusal preserves previous value"), FStoreAccess::ClientLabel(C.Store, 2) == FStringView(Updated));
		C.Test.TestEqual(TEXT("one-byte-less refusal performs no store charge"), C.Roots.Budget->UsedBytes.load(), BeforeRefusal + RefusalPressureBytes);
		C.Test.TestEqual(TEXT("label refusal is visibly omitted"), C.Store.Diagnostics().OmittedClients, OmittedBefore + 1);
		RefusalPressure.Reset();
		return true;
	}, 4);

	bAll &= WithStore(*this, TEXT("Budget: dropped dispatch invalidates affirmative history while scalar dispatch remains physical"), [](FCase& C) {
		if (!Deliver(C, Started(1))) return false;
		VisitStore(C.Test, C.Store, 1, [&](const auto& Row) {
			C.Test.TestEqual(TEXT("clean Started known never dispatched"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::KnownNeverDispatched);
			C.Test.TestEqual(TEXT("captured Started headers present"), Row.StartedHeadersState, EVaRestInspectorHeadersState::Captured);
		});
		const uint64 BeforeLoss = C.Roots.Pending->LossEpoch.load();
		const EPush Dropped = C.Queue->TryPush(DroppedDispatch(1, 101));
		C.Test.TestEqual(TEXT("actual dispatch dropped with count slack"), Dropped, EPush::Dropped);
		C.Test.TestEqual(TEXT("real loss epoch advanced"), C.Roots.Pending->LossEpoch.load(), BeforeLoss + 1);
		C.Test.TestEqual(TEXT("dropped dispatch has no ordinal hole"), C.Queue->LastPublishedMetadata(), uint64(1));
		if (!Deliver(C, Started(2))) return false;
		C.Test.TestEqual(TEXT("next successful metadata ordinal remains continuous"), C.Queue->LastPublishedMetadata(), uint64(2));
		VisitStore(C.Test, C.Store, 1, [&](const auto& Row) {
			C.Test.TestEqual(TEXT("dropped dispatch makes history unknown"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::Unknown);
			C.Test.TestEqual(TEXT("dropped dispatch does not authorize header trim"), Row.StartedHeadersState, EVaRestInspectorHeadersState::Captured);
		});
		if (!Deliver(C, Dispatch(2, 202, true), EPush::ScalarOnly)) return false;
		VisitStore(C.Test, C.Store, 2, [&](const auto& Row) {
			C.Test.TestEqual(TEXT("scalar-only dispatch remains physical"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::Dispatched);
			C.Test.TestTrue(TEXT("scalar-only dispatch has actual identity"), Row.Dispatches.Num() == 1 && Row.Dispatches[0].DispatchId == 202);
		});
		if (!RotateQueue(C, 2)) return false;
		if (!Deliver(C, Started(3))) return false;
		VisitStore(C.Test, C.Store, 1, [&](const auto& Row) { C.Test.TestEqual(TEXT("old unsealed history invalidated across generation"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::Unknown); });
		VisitStore(C.Test, C.Store, 3, [&](const auto& Row) { C.Test.TestEqual(TEXT("fresh generation row may qualify"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::KnownNeverDispatched); });
		FSnapshot During;
		C.Test.TestTrue(TEXT("outer enumeration begins"), C.Store.ForEachRecord([&](const auto&) {
			const EPush Result = C.Queue->TryPush(DroppedDispatch(3, 303));
			C.Test.TestEqual(TEXT("visitor causes real queue loss"), Result, EPush::Dropped);
			C.Test.TestFalse(TEXT("reentrant snapshot refuses when loss refresh is required"), C.Store.TrySnapshot(During));
			C.Test.TestTrue(TEXT("refused reentrant snapshot leaves output unchanged"), !During);
		}));
		VisitStore(C.Test, C.Store, 3, [&](const auto& Row) { C.Test.TestEqual(TEXT("deferred loss refresh applies at unwind"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::Unknown); });
		return true;
	}, 3, 64ll * 1024 * 1024, 16384);

	bAll &= WithStore(*this, TEXT("Budget: processed real queue loss permits later stable reentrant snapshot"), [](FCase& C) {
		if (!Deliver(C, Started(1))) return false;
		uint64 CleanRowRevision = 0;
		VisitStore(C.Test, C.Store, 1, [&](const auto& Row) {
			C.Test.TestEqual(TEXT("prior to real loss history is clean"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::KnownNeverDispatched);
			CleanRowRevision = Row.Revision;
		});
		const uint64 BeforeLoss = C.Roots.Pending->LossEpoch.load();
		C.Test.TestEqual(TEXT("prior real queue dispatch is dropped"), C.Queue->TryPush(DroppedDispatch(1, 101)), EPush::Dropped);
		C.Test.TestEqual(TEXT("prior real loss advances witness"), C.Roots.Pending->LossEpoch.load(), BeforeLoss + 1);
		uint64 ProcessedRowRevision = 0;
		VisitStore(C.Test, C.Store, 1, [&](const auto& Row) {
			C.Test.TestEqual(TEXT("earlier ordinary observation processes dirty history"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::Unknown);
			C.Test.TestTrue(TEXT("earlier loss refresh changes row revision"), Row.Revision > CleanRowRevision);
			ProcessedRowRevision = Row.Revision;
		});
		const uint64 ProcessedRevision = C.Store.Revision();
		const uint64 ProcessedEpoch = C.Roots.Pending->LossEpoch.load();
		FSnapshot During;
		int32 Visited = 0;
		C.Test.TestTrue(TEXT("later stable enumeration begins"), C.Store.ForEachRecord([&](const auto& Row) {
			++Visited;
			C.Test.TestEqual(TEXT("later visitor retains processed Unknown history"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::Unknown);
			C.Test.TestEqual(TEXT("later visitor retains processed row revision"), Row.Revision, ProcessedRowRevision);
			C.Test.TestTrue(TEXT("already processed dirty history permits reentrant snapshot"), C.Store.TrySnapshot(During));
			C.Test.TestEqual(TEXT("reentrant snapshot does not mutate live revision"), C.Store.Revision(), ProcessedRevision);
		}));
		C.Test.TestEqual(TEXT("stable reentrant control visits one actual row"), Visited, 1);
		if (!C.Test.TestTrue(TEXT("processed dirty reentrant snapshot was published"), During.IsValid())) return false;
		C.Test.TestEqual(TEXT("no new loss occurred during later read"), C.Roots.Pending->LossEpoch.load(), ProcessedEpoch);
		C.Test.TestEqual(TEXT("stable enumeration unwind preserves revision"), C.Store.Revision(), ProcessedRevision);
		VisitStore(C.Test, C.Store, 1, [&](const auto& Row) {
			C.Test.TestEqual(TEXT("live history remains Unknown after reentrant read"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::Unknown);
			C.Test.TestEqual(TEXT("live row revision remains unchanged after reentrant read"), Row.Revision, ProcessedRowRevision);
		});
		C.Store.Clear();
		C.Test.TestEqual(TEXT("immutable dirty snapshot retains captured revision after Clear"), During->GetRevision(), ProcessedRevision);
		VisitSnapshot(C.Test, *During, 1, [&](const auto& Row) {
			C.Test.TestEqual(TEXT("immutable snapshot retains Unknown history after Clear"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::Unknown);
			C.Test.TestEqual(TEXT("immutable snapshot retains processed row revision"), Row.Revision, ProcessedRowRevision);
		});
		return true;
	}, 1, 64ll * 1024 * 1024, 16384);

	bAll &= WithStore(*this, TEXT("Budget: compact Started and dispatched long-poll preservation under real byte pressure"), [](FCase& C) {
		if (!Deliver(C, Completed(10))) return false;
		auto StartedToCompact = Started(1);
		auto& StartedPayload = StartedToCompact.Payload.Get<FVaRestTraceStarted>();
		const FString HeaderValue = FString::ChrN(2048, TEXT('s'));
		for (int32 Index = 0; Index < 32; ++Index)
			StartedPayload.Headers.Set(*FString::Printf(TEXT("X-Compact-%d"), Index), HeaderValue);
		int64 StartedCharge = 0;
		if (!Deliver(C, StartedToCompact, EPush::Accepted, true, &StartedCharge)) return false;
		if (!Deliver(C, Started(2))) return false;
		if (!Deliver(C, Dispatch(2, 22))) return false;
		if (!Deliver(C, End(2, 22))) return false;
		if (!Deliver(C, Excerpt(2, 22, 'l', 1024 * 1024))) return false;
		if (!Deliver(C, Started(3))) return false;
		C.Test.TestEqual(TEXT("count pressure evicted only the completed oldest row"), CountRows(C.Store), 3);
		VisitStore(C.Test, C.Store, 1, [&](const auto& Row) {
			C.Test.TestEqual(TEXT("completed eviction leaves active headers untouched"), Row.StartedHeadersState, EVaRestInspectorHeadersState::Captured);
		});
		FSnapshot BeforeCompact;
		if (!Take(C, BeforeCompact)) return false;
		const auto ExcerptEvent = Excerpt(3, 0, 's', 128 * 1024);
		FPacketLayout Layout;
		const EMeasure Measured = FPacketCodec::Measure(ExcerptEvent, C.Policy, C.Roots.Lifetime->Allocator, Layout);
		if (!C.Test.TestTrue(TEXT("replacement excerpt measured full"), Measured == EMeasure::Full)) return false;
		const auto WrapperBound = C.Roots.Lifetime->Allocator.Bound(sizeof(FStorePacketOwner), alignof(FStorePacketOwner));
		if (!C.Test.TestTrue(TEXT("wrapper bound exists"), WrapperBound.IsSet())) return false;
		const int64 ExcerptCharge = int64(Layout.BackingBound + Layout.ExcerptCapacity + WrapperBound.GetValue());
		int64 CompactRequest = int64(Align(sizeof(FPacketFixed), alignof(TCHAR)));
		VisitStore(C.Test, C.Store, 1, [&](const auto& Row) {
			for (const FTextSlice& Slice : Row.Started.Fixed->Text)
				if (Slice.Chars)
					CompactRequest += (int64(Slice.Chars) + 1) * sizeof(TCHAR);
		});
		const auto CompactBound = C.Roots.Lifetime->Allocator.Bound(SIZE_T(CompactRequest), alignof(FPacketFixed));
		if (!C.Test.TestTrue(TEXT("compact backing bound exists"), CompactBound.IsSet())) return false;
		const int64 CompactCharge = int64(CompactBound.GetValue() + WrapperBound.GetValue());
		const int64 NetSaving = StartedCharge - CompactCharge;
		const int64 RequiredAfterTrim = ExcerptCharge - NetSaving;
		const int64 DesiredFree = CompactCharge > RequiredAfterTrim ? CompactCharge : RequiredAfterTrim;
		if (!C.Test.TestTrue(TEXT("compact replacement creates sufficient certified headroom"),
			NetSaving > 0 && CompactCharge < ExcerptCharge && DesiredFree >= 0 && DesiredFree < ExcerptCharge)) return false;
		const int64 PressureBytes = C.Roots.Budget->Limit - C.Roots.Budget->UsedBytes.load() - DesiredFree;
		if (!C.Test.TestTrue(TEXT("independent pressure credit fits computed headroom"), PressureBytes > 0)) return false;
		auto Pressure = FStoreCredit::Acquire(C.Roots.Budget, PressureBytes);
		if (!C.Test.TestTrue(TEXT("independent pressure credit admitted"), Pressure.IsSet())) return false;
		if (!Deliver(C, ExcerptEvent)) return false;
		VisitStore(C.Test, C.Store, 1, [&](const auto& Row) {
			C.Test.TestEqual(TEXT("eligible known-never headers compacted to Evicted"), Row.StartedHeadersState, EVaRestInspectorHeadersState::Evicted);
			C.Test.TestTrue(TEXT("non-header Started metadata survives"), !Row.Started.Text(EText::Url).IsEmpty() && !Row.Started.Text(EText::PathTemplate).IsEmpty());
		});
		VisitSnapshot(C.Test, *BeforeCompact, 1, [&](const auto& Row) {
			C.Test.TestEqual(TEXT("old snapshot retains original header state"), Row.StartedHeadersState, EVaRestInspectorHeadersState::Captured);
			C.Test.TestTrue(TEXT("old snapshot retains original header pairs"), Row.Started.Pairs(Row.Started.Fixed->Headers).Num() > 0);
		});
		VisitStore(C.Test, C.Store, 2, [&](const auto& Row) {
			C.Test.TestEqual(TEXT("dispatched long-poll history remains dispatched"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::Dispatched);
			C.Test.TestEqual(TEXT("dispatched response detail remains present"), Row.Dispatches[0].ResponseExcerptState, EVaRestInspectorDetailState::Present);
			C.Test.TestTrue(TEXT("dispatched long-poll backing retained"), Row.Dispatches[0].ResponseExcerpt.Backing.Num() > 0);
		});
		BeforeCompact.Reset();
		Pressure.Reset();
		return true;
	}, 3, 8 * 1024 * 1024);


	bAll &= WithStore(*this, TEXT("Budget: clean generation invalidation rebind and retained history"), [](FCase& C) {
		if (!Deliver(C, Started(1))) return false;
		FSnapshot BeforeBoundary;
		if (!Take(C, BeforeBoundary)) return false;
		const uint64 Epoch = C.Roots.Pending->LossEpoch.load();
		const uint64 Revision = C.Store.Revision();
		C.Test.TestTrue(TEXT("same bound witness is idempotent"), C.Store.BindLossWitness(C.Roots.Pending.ToSharedRef()));
		C.Test.TestEqual(TEXT("idempotent rebind preserves revision"), C.Store.Revision(), Revision);
		VisitStore(C.Test, C.Store, 1, [&](const auto& Row) { C.Test.TestEqual(TEXT("idempotent rebind preserves clean certificate"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::KnownNeverDispatched); });
		if (!RotateQueue(C, 2)) return false;
		C.Test.TestEqual(TEXT("generation boundary did not fabricate loss"), C.Roots.Pending->LossEpoch.load(), Epoch);
		VisitStore(C.Test, C.Store, 1, [&](const auto& Row) {
			C.Test.TestEqual(TEXT("unchanged epoch invalidates old clean pinned history"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::Unknown);
			C.Test.TestTrue(TEXT("old row remains pinned"), Row.bPinned);
		});
		if (!Deliver(C, Started(2))) return false;
		VisitStore(C.Test, C.Store, 2, [&](const auto& Row) { C.Test.TestEqual(TEXT("fresh matching-generation Started obtains clean history"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::KnownNeverDispatched); });
		VisitSnapshot(C.Test, *BeforeBoundary, 1, [&](const auto& Row) { C.Test.TestEqual(TEXT("published history remains immutable across generation"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::KnownNeverDispatched); });
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Budget: unbound witness never certifies Started"), [](FCase& C) {
		auto Clock = MakeShared<FReceiptClock, ESPMode::ThreadSafe>();
		FStore Unbound(C.Policy.Limits, Clock);
		if (!C.Test.TestTrue(TEXT("real unbound store initialized on same bounded fixture account"), Unbound.Initialize(C.Roots.Budget))) return false;
		Unbound.BeginGeneration(1, 0);
		FCase U{C.Test, C.Roots, Unbound, C.Queue, *Clock, C.Policy};
		if (!Deliver(U, Started(1))) return false;
		VisitStore(C.Test, Unbound, 1, [&](const auto& Row) {
			C.Test.TestEqual(TEXT("unbound Started history remains unknown"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::Unknown);
			C.Test.TestEqual(TEXT("unbound header detail remains captured"), Row.StartedHeadersState, EVaRestInspectorHeadersState::Captured);
		});
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Budget: real loss before Completed invalidates before immutable sealing"), [](FCase& C) {
		if (!Deliver(C, Started(1))) return false;
		C.Test.TestTrue(TEXT("loss control has actual count slack"), C.Roots.Pending->Count.load() < C.Roots.Pending->CountLimit);
		const uint64 Epoch = C.Roots.Pending->LossEpoch.load();
		C.Test.TestEqual(TEXT("real dispatch drop before Completed"), C.Queue->TryPush(DroppedDispatch(1, 101)), EPush::Dropped);
		C.Test.TestEqual(TEXT("pre-Completed real epoch advances"), C.Roots.Pending->LossEpoch.load(), Epoch + 1);
		if (!Deliver(C, Completed(1))) return false;
		VisitStore(C.Test, C.Store, 1, [&](const auto& Row) { C.Test.TestTrue(TEXT("Completed seals only downgraded unknown history"), Row.bSealed && Row.DispatchHistory == EVaRestInspectorDispatchHistory::Unknown); });
		return true;
	}, 2, 64ll * 1024 * 1024, 16384);

	bAll &= WithStore(*this, TEXT("Budget: unrelated loss after sealing preserves immutable history"), [](FCase& C) {
		if (!Deliver(C, Started(1)) || !Deliver(C, Completed(1))) return false;
		FSnapshot Old;
		if (!Take(C, Old)) return false;
		uint64 Revision = 0;
		VisitStore(C.Test, C.Store, 1, [&](const auto& Row) {
			C.Test.TestTrue(TEXT("no-loss control sealed known-never history"), Row.bSealed && Row.DispatchHistory == EVaRestInspectorDispatchHistory::KnownNeverDispatched);
			Revision = Row.Revision;
		});
		C.Test.TestEqual(TEXT("unrelated post-seal real drop"), C.Queue->TryPush(DroppedDispatch(999, 999)), EPush::Dropped);
		VisitStore(C.Test, C.Store, 1, [&](const auto& Row) {
			C.Test.TestEqual(TEXT("post-seal loss cannot rewrite history"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::KnownNeverDispatched);
			C.Test.TestEqual(TEXT("post-seal loss cannot rewrite revision"), Row.Revision, Revision);
		});
		VisitSnapshot(C.Test, *Old, 1, [&](const auto& Row) { C.Test.TestEqual(TEXT("retained sealed image preserves history"), Row.DispatchHistory, EVaRestInspectorDispatchHistory::KnownNeverDispatched); });
		return true;
	}, 2, 64ll * 1024 * 1024, 16384);

	for (bool bExpiry : {false, true})
		bAll &= WithStore(*this, bExpiry ? TEXT("Budget: real loss before expiry seals Unknown") : TEXT("Budget: real loss before stop seals Unknown"), [bExpiry](FCase& C) {
			if (!Deliver(C, Started(1, true)) || !Deliver(C, Completed(1))) return false;
			const uint64 Epoch = C.Roots.Pending->LossEpoch.load();
			C.Test.TestEqual(TEXT("unrelated real drop before promise resolution"), C.Queue->TryPush(DroppedDispatch(999, 999)), EPush::Dropped);
			C.Test.TestEqual(TEXT("promise-resolution control epoch advances"), C.Roots.Pending->LossEpoch.load(), Epoch + 1);
			// No Store observation may refresh history between this loss and the operation under test.
			if (bExpiry)
			{
				C.Clock.Set(5.0);
				C.Test.TestEqual(TEXT("actual owned expiry resolves one row"), FStoreAccess::Expire(C.Store), 1);
			}
			else
				FStoreAccess::Stop(C.Store);
			FSnapshot Old;
			if (!Take(C, Old)) return false;
			VisitSnapshot(C.Test, *Old, 1, [&](const auto& Row) {
				C.Test.TestTrue(TEXT("promise resolution seals downgraded history"), Row.bSealed && Row.DispatchHistory == EVaRestInspectorDispatchHistory::Unknown);
				C.Test.TestEqual(TEXT("promise resolves through actual requested control"), Row.RequestExcerptState,
					bExpiry ? EVaRestInspectorDetailState::TimedOut : EVaRestInspectorDetailState::CaptureStopped);
			});
			return true;
		}, 2, 64ll * 1024 * 1024, 16384);

	bAll &= WithStore(*this, TEXT("Budget: reusable link target tombstones survive complete bounded churn"), [](FCase& C) {
		const auto Plan = FStoreAccess::Plan(C.Store);
		const int64 Bytes = C.Roots.Budget->UsedBytes.load();
		const int64 Backing = C.Roots.Lifetime->Probe.LiveBacking.load();
		const uint64 Last = uint64(Plan.LinkTargetCapacity) + 1;
		for (uint64 Target = 1; Target <= Last; ++Target)
		{
			const auto Result = C.Store.TryAdmitLink({77, Target, {}, false}, {});
			if (!C.Test.TestTrue(TEXT("ordinary target churn always admits or FIFO replaces"), Result == ELinkAdmission::Added || Result == ELinkAdmission::ReplacedOldest)) return false;
		}
		C.Test.TestEqual(TEXT("churn preserves actual global capacity"), FStoreAccess::NumLinks(C.Store), Plan.LinkCapacity);
		C.Test.TestFalse(TEXT("global oldest link identity was removed"), FStoreAccess::HasLink(C.Store, {77, 1, {}, false}));
		C.Test.TestTrue(TEXT("global newest link identity remains"), FStoreAccess::HasLink(C.Store, {77, Last, {}, false}));
		C.Test.TestEqual(TEXT("newest target owns one real link"), FStoreAccess::TargetLinks(C.Store, Last), 1);
		C.Test.TestEqual(TEXT("surviving tuple deduplicates"), C.Store.TryAdmitLink({77, Last, {}, false}, {}), ELinkAdmission::AlreadyPresent);
		// 1 and Last collide under the actual power-of-two target mask.
		C.Test.TestEqual(TEXT("colliding deleted target can be reused without losing survivor"), C.Store.TryAdmitLink({88, 1, {}, false}, {}), ELinkAdmission::ReplacedOldest);
		C.Test.TestTrue(TEXT("colliding older surviving target remains reachable"), FStoreAccess::HasLink(C.Store, {77, Last, {}, false}));
		C.Test.TestTrue(TEXT("colliding replacement target becomes reachable"), FStoreAccess::HasLink(C.Store, {88, 1, {}, false}));
		C.Test.TestEqual(TEXT("empty QueueId churn adds no dynamic byte charge"), C.Roots.Budget->UsedBytes.load(), Bytes);
		C.Test.TestEqual(TEXT("empty QueueId churn adds no backing allocation"), C.Roots.Lifetime->Probe.LiveBacking.load(), Backing);
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Budget: target65 evicts actual oldest identity and preserves dedup"), [](FCase& C) {
		for (uint64 From = 1; From <= 65; ++From)
		{
			const auto Result = C.Store.TryAdmitLink({From, 100000, {}, false}, {});
			if (!C.Test.TestTrue(TEXT("target fixture real link admitted"), Result == ELinkAdmission::Added || Result == ELinkAdmission::ReplacedOldest)) return false;
		}
		C.Test.TestEqual(TEXT("actual target cap is64"), FStoreAccess::TargetLinks(C.Store, 100000), 64);
		C.Test.TestEqual(TEXT("per-target cap is independent of global4096"), FStoreAccess::NumLinks(C.Store), 64);
		C.Test.TestFalse(TEXT("target oldest identity is evicted"), FStoreAccess::HasLink(C.Store, {1, 100000, {}, false}));
		C.Test.TestTrue(TEXT("target next-oldest survives"), FStoreAccess::HasLink(C.Store, {2, 100000, {}, false}));
		C.Test.TestTrue(TEXT("target65 newest survives"), FStoreAccess::HasLink(C.Store, {65, 100000, {}, false}));
		const uint64 Revision = C.Store.Revision();
		C.Test.TestEqual(TEXT("duplicate tuple performs no overwrite"), C.Store.TryAdmitLink({65, 100000, {}, false}, {}), ELinkAdmission::AlreadyPresent);
		C.Test.TestEqual(TEXT("duplicate tuple does not change revision"), C.Store.Revision(), Revision);
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Budget: retained private stats keys and512-bin copies survive Clear"), [](FCase& C) {
		const int64 Fixed = C.Roots.Budget->UsedBytes.load();
		const int64 FixedBacking = C.Roots.Lifetime->Probe.LiveBacking.load();
		const FString Keys[] = {TEXT("retained-first"), TEXT("retained-second"), TEXT("retained-third")};
		for (const FString& Key : Keys)
			if (!C.Test.TestNotNull(TEXT("private stats group genuinely admitted"), C.Store.FindOrAdmitStatsGroup({0, 1, NAME_None, FStringView(Key)}, {}))) return false;
		int64 O = 0, N = 0;
		if (!C.Test.TestTrue(TEXT("complete private stats snapshot cost measured before allocation"), FStoreAccess::SnapshotCosts(C.Store, O, N))) return false;
		FSnapshot Old;
		if (!Take(C, Old)) return false;
		C.Store.Clear();
		C.Test.TestEqual(TEXT("retained private stats domain remains independently charged"), C.Roots.Budget->UsedBytes.load(), Fixed + 4096 + O + N);
		C.Test.TestEqual(TEXT("retained snapshot owns overflow plus every named group"), FSnapshotBuilder::PrivateStatsCount(*Old), 4);
		for (const FString& Key : Keys)
			C.Test.TestTrue(TEXT("retained private key survives mutable store destruction"), FSnapshotBuilder::HasPrivateStatsGroup(*Old, {0, 1, NAME_None, FStringView(Key)}));
		C.Test.TestTrue(TEXT("all real copied512-bin payloads remain initialized"), FSnapshotBuilder::PrivateStatsHistogramsAreZero(*Old));
		int32 PublicStats = 0;
		bool SeenKeys[3] = {};
		int32 PublicOverflow = 0;
		Old->ForEachStats([&](const FVaRestInspectorHttpStats& Group) {
			++PublicStats;
			PublicOverflow += Group.bOverflow ? 1 : 0;
			C.Test.TestEqual(TEXT("public copied group scope is real Process"), Group.ScopeId, uint64(0));
			C.Test.TestEqual(TEXT("public copied group client preserves overflow distinction"), Group.ClientId, Group.bOverflow ? uint64(0) : uint64(1));
			C.Test.TestTrue(TEXT("manual copied group has no invented endpoint"), Group.Endpoint.IsNone());
			if (Group.bOverflow)
				C.Test.TestTrue(TEXT("single public overflow has no rejected key"), Group.Key.IsEmpty());
			else
			{
				int32 Matches = 0;
				for (int32 Index = 0; Index < 3; ++Index)
					if (Group.Key.Equals(Keys[Index], ESearchCase::CaseSensitive)) { SeenKeys[Index] = true; ++Matches; }
				C.Test.TestEqual(TEXT("actual admitted public key survives Clear exactly"), Matches, 1);
			}
			const uint64 Scalars[] = {Group.Started, Group.Completed, Group.Success, Group.Error, Group.Queued, Group.Canceled,
				Group.Dispatches, Group.Retries, Group.AuthReplays, Group.RetryScheduled, Group.UnknownDispatchClassification,
				Group.CacheHit, Group.CacheDisk, Group.Revalidated, Group.Stale, Group.Replayed, Group.WireBytesUp, Group.WireBytesDown,
				Group.Partial, Group.Incomplete, Group.Dropped, Group.DurationCount, Group.DurationUnderflow, Group.DurationOverflow,
				Group.DurationUnknown, Group.DurationNonFinite, Group.ErrorsByKind.Canceled, Group.ErrorsByKind.Connection,
				Group.ErrorsByKind.Timeout, Group.ErrorsByKind.Http, Group.ErrorsByKind.Server, Group.ErrorsByKind.Decode,
				Group.ErrorsByKind.Auth, Group.ErrorsByKind.Offline, Group.ErrorsByKind.Config, Group.ErrorsByKind.Unknown};
			for (uint64 Value : Scalars) C.Test.TestEqual(TEXT("manual public copy invents no observation"), Value, uint64(0));
			for (uint64 Bin : Group.Histogram) C.Test.TestEqual(TEXT("all512 public copied bins survive Clear"), Bin, uint64(0));
			C.Test.TestTrue(TEXT("manual public copy preserves zero extrema and flags"), Group.DurationSum == 0 && Group.DurationMin == 0 &&
				Group.DurationMax == 0 && !Group.bSaturated && !Group.bWireBytesUnknown && !Group.bDurationCountsSaturated && !Group.ErrorsByKind.bSaturated);
		});
		C.Test.TestEqual(TEXT("public snapshot owns the actual four copied groups"), PublicStats, 4);
		C.Test.TestEqual(TEXT("one public overflow is independently counted"), PublicOverflow, 1);
		for (bool Seen : SeenKeys) C.Test.TestTrue(TEXT("each actual admitted key appears publicly"), Seen);
		C.Test.TestTrue(TEXT("public links remain staged empty"), Old->GetLinks().IsEmpty());
		Old.Reset();
		C.Test.TestEqual(TEXT("final private-copy release returns exact fixed charge"), C.Roots.Budget->UsedBytes.load(), Fixed);
		C.Test.TestEqual(TEXT("final private-copy release returns exact actual backing"), C.Roots.Lifetime->Probe.LiveBacking.load(), FixedBacking);
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Budget proof: real scalar-byte-refused dispatch cannot authorize pinned detail trim"), [](FCase& C) {
		const auto RequestExcerpt = Excerpt(1, 0, 'u', 4096);
		const auto& SourceBytes = RequestExcerpt.Payload.Get<FVaRestTraceExcerptEvent>().Excerpt.Bytes;
		if (!C.Test.TestTrue(TEXT("initial source request excerpt owns bytes"), SourceBytes.IsValid()) ||
			!C.Test.TestEqual(TEXT("initial source request excerpt Num is one"), SourceBytes->Num(), 1)) return false;
		if (!C.Test.TestNotNull(TEXT("initial source excerpt byte storage exists"), SourceBytes->GetData()) ||
			!C.Test.TestEqual(TEXT("initial source request excerpt byte is u"), int32((*SourceBytes)[0]), int32('u'))) return false;
		const SIZE_T SourceCapacity = SourceBytes->GetAllocatedSize();
		FPacketLayout RequestLayout;
		const auto ExcerptWrapper = C.Roots.Lifetime->Allocator.Bound(sizeof(FStorePacketOwner), alignof(FStorePacketOwner));
		if (!C.Test.TestTrue(TEXT("initial excerpt has full codec layout and independent wrapper bound"),
			FPacketCodec::Measure(RequestExcerpt, C.Policy, C.Roots.Lifetime->Allocator, RequestLayout) == EMeasure::Full &&
			ExcerptWrapper.IsSet() && ExcerptWrapper.GetValue() <= SIZE_T(MAX_int64) && RequestLayout.BackingBound <= SIZE_T(MAX_int64) &&
			SourceCapacity <= SIZE_T(MAX_int64) && SourceCapacity > SIZE_T(SourceBytes->Num()))) return false;
		if (!C.Test.TestEqual(TEXT("codec charges actual source allocated capacity rather than Num"), RequestLayout.ExcerptCapacity, int64(SourceCapacity))) return false;
		int64 ExpectedExcerptCharge = int64(RequestLayout.BackingBound);
		if (!C.Test.TestTrue(TEXT("independent checked backing capacity wrapper demand"), AddChecked(ExpectedExcerptCharge, int64(SourceCapacity), MAX_int64) &&
			AddChecked(ExpectedExcerptCharge, int64(ExcerptWrapper.GetValue()), MAX_int64))) return false;
		int64 ExcerptCharge = 0;
		if (!Deliver(C, Started(1, true))) return false;
		const int64 BeforeExcerpt = C.Roots.Budget->UsedBytes.load();
		if (!Deliver(C, RequestExcerpt, EPush::Accepted, true, &ExcerptCharge)) return false;
		C.Test.TestEqual(TEXT("delivered handoff demand equals independent excerpt allocation demand"), ExcerptCharge, ExpectedExcerptCharge);
		C.Test.TestEqual(TEXT("initial actual store delta includes allocated capacity"), C.Roots.Budget->UsedBytes.load(), BeforeExcerpt + ExpectedExcerptCharge);
		bool bReady = false;
		VisitStore(C.Test, C.Store, 1, [&](const auto& R) {
			bReady = C.Test.TestTrue(TEXT("loss control begins with clean pinned captured owned detail"), R.bPinned &&
				R.DispatchHistory == EVaRestInspectorDispatchHistory::KnownNeverDispatched && R.StartedHeadersState == EVaRestInspectorHeadersState::Captured &&
				R.RequestExcerptState == EVaRestInspectorDetailState::Present);
			if (!C.Test.TestEqual(TEXT("initial store request excerpt count is one"), R.Excerpts.Num(), 1)) { bReady = false; return; }
			const auto& Actual = R.Excerpts[0];
			if (!C.Test.TestNotNull(TEXT("initial store request excerpt metadata exists"), Actual.Fixed) ||
				!C.Test.TestNotNull(TEXT("initial store request excerpt byte storage exists"), Actual.Excerpt.GetData()) ||
				!C.Test.TestEqual(TEXT("initial store request excerpt Num is one"), Actual.Excerpt.Num(), 1)) { bReady = false; return; }
			bReady &= C.Test.TestEqual(TEXT("initial store request excerpt byte is u"), int32(Actual.Excerpt[0]), int32('u'));
			bReady &= C.Test.TestTrue(TEXT("initial store request excerpt retains actual source byte owner"), Actual.Excerpt.GetData() == SourceBytes->GetData());
		});
		if (!bReady) return false;
		const auto LostDispatch = Dispatch(1, 101, true);
		FPacketLayout ScalarLayout;
		if (!C.Test.TestEqual(TEXT("oversized dispatch genuinely measures scalar-only"), FPacketCodec::Measure(LostDispatch, C.Policy, C.Roots.Lifetime->Allocator, ScalarLayout), EMeasure::ScalarOnly)) return false;
		const auto Fixed = C.Roots.Lifetime->Allocator.Bound(sizeof(FPacketFixed), alignof(FPacketFixed));
		const SIZE_T Node = C.Roots.Lifetime->Allocator.QueueNodeBound();
		if (!C.Test.TestTrue(TEXT("actual scalar backing and node bounds"), Fixed.IsSet() && Node > 0 && Node <= SIZE_T(MAX_int64))) return false;
		int64 ScalarCharge = int64(Fixed.GetValue());
		if (!C.Test.TestTrue(TEXT("independent exact scalar charge"), AddChecked(ScalarCharge, int64(Node), MAX_int64) && ScalarCharge == ScalarLayout.PendingCharge && ScalarCharge > 1)) return false;
		const int64 PendingPressureBytes = C.Roots.Pending->ByteLimit - C.Roots.Pending->Bytes.load() - (ScalarCharge - 1);
		if (!C.Test.TestTrue(TEXT("scalar drop has actual count slack and positive byte pressure"), C.Roots.Pending->Count.load() == 0 && C.Roots.Pending->CountLimit > 0 && PendingPressureBytes > 0)) return false;
		const uint64 Epoch = C.Roots.Pending->LossEpoch.load(), Published = C.Queue->LastPublishedMetadata();
		{
			auto PendingPressure = FPendingBaseline::Acquire(C.Roots.Pending.ToSharedRef(), PendingPressureBytes);
			if (!C.Test.TestTrue(TEXT("real pending byte pressure admitted"), PendingPressure.IsSet())) return false;
			const int64 Copies = C.Roots.Lifetime->Probe.TextBytesCopied.load(), Refs = C.Roots.Lifetime->Probe.ExcerptRefsAcquired.load();
			if (!C.Test.TestEqual(TEXT("scalar dispatch itself actually drops with count slack"), C.Queue->TryPush(LostDispatch), EPush::Dropped)) return false;
			C.Test.TestFalse(TEXT("dropped scalar has no published packet"), C.Queue->TryPopMetadata().IsSet());
			C.Test.TestEqual(TEXT("actual scalar drop advances LossEpoch"), C.Roots.Pending->LossEpoch.load(), Epoch + 1);
			C.Test.TestEqual(TEXT("actual drop leaves successful ordinal continuous"), C.Queue->LastPublishedMetadata(), Published);
			C.Test.TestEqual(TEXT("dropped scalar copied no text"), C.Roots.Lifetime->Probe.TextBytesCopied.load(), Copies);
			C.Test.TestEqual(TEXT("dropped scalar retained no excerpt"), C.Roots.Lifetime->Probe.ExcerptRefsAcquired.load(), Refs);
		}
		FStartedProofImage Expected;
		const uint8* ExcerptBacking = nullptr;
		const uint8* ExcerptBytes = nullptr;
		bool bCaptured = false;
		VisitStore(C.Test, C.Store, 1, [&](const auto& R) {
			bCaptured = C.Test.TestTrue(TEXT("real loss refresh downgrades history before pressure"), R.DispatchHistory == EVaRestInspectorDispatchHistory::Unknown) && Expected.Capture(C.Test, R);
			if (C.Test.TestEqual(TEXT("protected row owns one actual request excerpt"), R.Excerpts.Num(), 1)) { ExcerptBacking = R.Excerpts[0].Backing.GetData(); ExcerptBytes = R.Excerpts[0].Excerpt.GetData(); }
		});
		if (!bCaptured || !C.Test.TestNotNull(TEXT("request excerpt actual backing recorded"), ExcerptBacking) || !C.Test.TestNotNull(TEXT("actual core excerpt byte owner recorded"), ExcerptBytes)) return false;
		const FString Label = TEXT("unknown-trim-pressure-client");
		const auto LabelBound = C.Roots.Lifetime->Allocator.Bound((SIZE_T(Label.Len()) + 1) * sizeof(TCHAR), alignof(TCHAR));
		if (!C.Test.TestTrue(TEXT("label demand fits count but requires protected detail bytes"), LabelBound.IsSet() && LabelBound.GetValue() > 1 && ExcerptCharge > int64(LabelBound.GetValue()) && FStoreAccess::NumClients(C.Store) < FStoreAccess::Plan(C.Store).ClientCapacity)) return false;
		if (!C.Test.TestEqual(TEXT("real label packet admitted upstream"), C.Queue->TryPush(Client(777, *Label)), EPush::Accepted)) return false;
		auto Packet = C.Queue->TryPopMetadata();
		if (!C.Test.TestTrue(TEXT("real label packet dequeued before store observation"), Packet.IsSet())) return false;
		const int64 PressureBytes = C.Roots.Budget->Limit - C.Roots.Budget->UsedBytes.load() - (int64(LabelBound.GetValue()) - 1);
		if (!C.Test.TestTrue(TEXT("label pressure arithmetic valid"), PressureBytes > 0)) return false;
		auto Pressure = FStoreCredit::Acquire(C.Roots.Budget, PressureBytes);
		if (!C.Test.TestTrue(TEXT("real store byte pressure admitted"), Pressure.IsSet())) return false;
		const int64 Used = C.Roots.Budget->UsedBytes.load(), Copies = C.Roots.Lifetime->Probe.TextBytesCopied.load(), Refs = C.Roots.Lifetime->Probe.ExcerptRefsAcquired.load();
		const uint64 Omitted = C.Store.Diagnostics().OmittedClients;
		C.Test.TestTrue(TEXT("real Client apply reaches optional label admission"), C.Store.Apply(MoveTemp(Packet.GetValue())));
		C.Test.TestEqual(TEXT("label refuses rather than trimming unknown pinned row"), C.Store.Diagnostics().OmittedClients, Omitted + 1);
		C.Test.TestEqual(TEXT("numeric client registered in bounded slot"), FStoreAccess::NumClients(C.Store), 1);
		C.Test.TestTrue(TEXT("refused label remains unknown"), FStoreAccess::ClientLabel(C.Store, 777).IsEmpty());
		C.Test.TestEqual(TEXT("attempted unsafe trim releases no store charge"), C.Roots.Budget->UsedBytes.load(), Used);
		C.Test.TestEqual(TEXT("refused label and compact copied no store text"), C.Roots.Lifetime->Probe.TextBytesCopied.load(), Copies);
		C.Test.TestEqual(TEXT("refused label and trim acquire no store excerpt reference"), C.Roots.Lifetime->Probe.ExcerptRefsAcquired.load(), Refs);
		VisitStore(C.Test, C.Store, 1, [&](const auto& R) {
			Expected.Check(C.Test, R, false, true, true);
			if (C.Test.TestEqual(TEXT("unknown history retains actual excerpt owner"), R.Excerpts.Num(), 1)) { C.Test.TestTrue(TEXT("actual excerpt metadata backing identity survives attempted trim"), R.Excerpts[0].Backing.GetData() == ExcerptBacking); C.Test.TestTrue(TEXT("actual core excerpt byte owner identity survives attempted trim"), R.Excerpts[0].Excerpt.GetData() == ExcerptBytes); }
		});
		C.Test.TestEqual(TEXT("next successful metadata publication has no lost ordinal"), C.Queue->LastPublishedMetadata(), Published + 1);
		Pressure.Reset();
		return true;
	}, 2);

	bAll &= WithStore(*this, TEXT("Budget proof: full non-header Started and pending promise survive successful compact"), [](FCase& C) {
		const auto Rich = RichProofStarted(1);
		FPacketLayout StartedLayout;
		if (!C.Test.TestEqual(TEXT("rich source has complete bounded metadata"), FPacketCodec::Measure(Rich, C.Policy, C.Roots.Lifetime->Allocator, StartedLayout), EMeasure::Full)) return false;
		int64 OldCharge = 0;
		if (!Deliver(C, Rich, EPush::Accepted, true, &OldCharge) || !Deliver(C, Started(2, false, false))) return false;
		if (!C.Test.TestTrue(TEXT("compact byte pressure has actual row-count slack"), CountRows(C.Store) < FStoreAccess::Plan(C.Store).MaxRows)) return false;
		FSnapshot Old;
		if (!Take(C, Old)) return false;
		FStartedProofImage Expected;
		bool bCaptured = false;
		VisitStore(C.Test, C.Store, 1, [&](const auto& R) { bCaptured = C.Test.TestTrue(TEXT("source is clean pinned with outstanding request promise"), R.bPinned && R.DispatchHistory == EVaRestInspectorDispatchHistory::KnownNeverDispatched && R.RequestExcerptState == EVaRestInspectorDetailState::Pending && R.Excerpts.IsEmpty()) && Expected.Capture(C.Test, R); });
		if (!bCaptured) return false;
		int64 CompactCharge = 0;
		if (!MeasureProofCompact(C, 1, CompactCharge) || !C.Test.TestTrue(TEXT("complete replacement strictly smaller"), OldCharge > CompactCharge && CompactCharge > 0)) return false;
		const auto Incoming = Excerpt(2, 0, 'c', 128 * 1024);
		FPacketLayout Layout;
		const auto Wrapper = C.Roots.Lifetime->Allocator.Bound(sizeof(FStorePacketOwner), alignof(FStorePacketOwner));
		if (!C.Test.TestTrue(TEXT("incoming excerpt independently measures full"), Wrapper.IsSet() && FPacketCodec::Measure(Incoming, C.Policy, C.Roots.Lifetime->Allocator, Layout) == EMeasure::Full)) return false;
		int64 Demand = int64(Layout.BackingBound);
		if (!C.Test.TestTrue(TEXT("checked actual excerpt and wrapper demand"), Layout.ExcerptCapacity == int64(Incoming.Payload.Get<FVaRestTraceExcerptEvent>().Excerpt.Bytes->GetAllocatedSize()) && AddChecked(Demand, Layout.ExcerptCapacity, MAX_int64) && AddChecked(Demand, int64(Wrapper.GetValue()), MAX_int64))) return false;
		const int64 Saving = OldCharge - CompactCharge, Headroom = FMath::Max(CompactCharge, Demand - Saving);
		const int64 PressureBytes = C.Roots.Budget->Limit - C.Roots.Budget->UsedBytes.load() - Headroom;
		if (!C.Test.TestTrue(TEXT("independent simultaneous replacement and final demand equations"), Headroom >= CompactCharge && Headroom < Demand && Headroom + Saving >= Demand && PressureBytes > 0)) return false;
		if (!C.Test.TestEqual(TEXT("incoming excerpt actually queued"), C.Queue->TryPush(Incoming), EPush::Accepted)) return false;
		auto Packet = C.Queue->TryPopExcerpt();
		if (!C.Test.TestTrue(TEXT("actual incoming excerpt dequeued"), Packet.IsSet())) return false;
		auto Pressure = FStoreCredit::Acquire(C.Roots.Budget, PressureBytes);
		if (!C.Test.TestTrue(TEXT("actual calculated store pressure admitted"), Pressure.IsSet())) return false;
		const int64 Used = C.Roots.Budget->UsedBytes.load();
		C.Test.TestTrue(TEXT("actual excerpt admission succeeds after compact"), C.Store.Apply(MoveTemp(Packet.GetValue())));
		C.Test.TestEqual(TEXT("only complete net saving and incoming demand change ledger"), C.Roots.Budget->UsedBytes.load(), Used + Demand - Saving);
		VisitStore(C.Test, C.Store, 1, [&](const auto& R) { Expected.Check(C.Test, R, true, false, false); });
		VisitSnapshot(C.Test, *Old, 1, [&](const auto& R) { Expected.Check(C.Test, R, false, false, true); });
		VisitStore(C.Test, C.Store, 2, [&](const auto& R) { C.Test.TestEqual(TEXT("incoming body reached anchored second row"), R.RequestExcerptState, EVaRestInspectorDetailState::Present); });
		Pressure.Reset(); Old.Reset();
		return true;
	}, 3, 8 * 1024 * 1024, 4 * 1024 * 1024, false, 128 * 1024);

	bAll &= WithStore(*this, TEXT("Budget proof: one-byte-short simultaneous compact headroom preserves complete source"), [](FCase& C) {
		const auto Rich = RichProofStarted(1);
		FPacketLayout StartedLayout;
		if (!C.Test.TestEqual(TEXT("refusal rich source measures full"), FPacketCodec::Measure(Rich, C.Policy, C.Roots.Lifetime->Allocator, StartedLayout), EMeasure::Full)) return false;
		int64 OldCharge = 0;
		if (!Deliver(C, Rich, EPush::Accepted, true, &OldCharge) || !Deliver(C, Started(2, false, false))) return false;
		if (!C.Test.TestTrue(TEXT("refusal has actual row-count slack"), CountRows(C.Store) < FStoreAccess::Plan(C.Store).MaxRows)) return false;
		FSnapshot Old;
		if (!Take(C, Old)) return false;
		FStartedProofImage Expected;
		bool bCaptured = false;
		VisitStore(C.Test, C.Store, 1, [&](const auto& R) { bCaptured = C.Test.TestTrue(TEXT("only source trim candidate has pending promise and no owned excerpt"), R.bPinned && R.DispatchHistory == EVaRestInspectorDispatchHistory::KnownNeverDispatched && R.RequestExcerptState == EVaRestInspectorDetailState::Pending && R.Excerpts.IsEmpty()) && Expected.Capture(C.Test, R); });
		if (!bCaptured) return false;
		int64 CompactCharge = 0;
		if (!MeasureProofCompact(C, 1, CompactCharge) || !C.Test.TestTrue(TEXT("refusal replacement still strictly smaller"), OldCharge > CompactCharge && CompactCharge > 1)) return false;
		const auto Incoming = Excerpt(2, 0, 'n', 128 * 1024);
		FPacketLayout Layout;
		const auto Wrapper = C.Roots.Lifetime->Allocator.Bound(sizeof(FStorePacketOwner), alignof(FStorePacketOwner));
		if (!C.Test.TestTrue(TEXT("refusal incoming excerpt measures full"), Wrapper.IsSet() && FPacketCodec::Measure(Incoming, C.Policy, C.Roots.Lifetime->Allocator, Layout) == EMeasure::Full)) return false;
		int64 Demand = int64(Layout.BackingBound);
		if (!C.Test.TestTrue(TEXT("refusal demand independently includes allocated excerpt capacity"), Layout.ExcerptCapacity == int64(Incoming.Payload.Get<FVaRestTraceExcerptEvent>().Excerpt.Bytes->GetAllocatedSize()) && AddChecked(Demand, Layout.ExcerptCapacity, MAX_int64) && AddChecked(Demand, int64(Wrapper.GetValue()), MAX_int64) && Demand > CompactCharge)) return false;
		if (!C.Test.TestEqual(TEXT("refused-body input genuinely queued"), C.Queue->TryPush(Incoming), EPush::Accepted)) return false;
		auto Packet = C.Queue->TryPopExcerpt();
		if (!C.Test.TestTrue(TEXT("refused-body packet genuinely dequeued"), Packet.IsSet())) return false;
		const int64 PressureBytes = C.Roots.Budget->Limit - C.Roots.Budget->UsedBytes.load() - (CompactCharge - 1);
		if (!C.Test.TestTrue(TEXT("one-byte-short pressure arithmetic valid"), PressureBytes > 0)) return false;
		auto Pressure = FStoreCredit::Acquire(C.Roots.Budget, PressureBytes);
		if (!C.Test.TestTrue(TEXT("one-byte-short actual store credit admitted"), Pressure.IsSet())) return false;
		const int64 Used = C.Roots.Budget->UsedBytes.load(), Copies = C.Roots.Lifetime->Probe.TextBytesCopied.load(), Refs = C.Roots.Lifetime->Probe.ExcerptRefsAcquired.load();
		uint64 OmittedBefore = 0;
		VisitStore(C.Test, C.Store, 2, [&](const auto& R) { OmittedBefore = R.OmittedExcerpts; });
		C.Test.TestFalse(TEXT("compact cannot use net saving before simultaneous credit fits"), C.Store.Apply(MoveTemp(Packet.GetValue())));
		C.Test.TestEqual(TEXT("no-headroom refusal preserves complete store charge"), C.Roots.Budget->UsedBytes.load(), Used);
		C.Test.TestEqual(TEXT("failed compact reservation copies no store text"), C.Roots.Lifetime->Probe.TextBytesCopied.load(), Copies);
		C.Test.TestEqual(TEXT("failed admission acquires no store excerpt reference"), C.Roots.Lifetime->Probe.ExcerptRefsAcquired.load(), Refs);
		VisitStore(C.Test, C.Store, 1, [&](const auto& R) { Expected.Check(C.Test, R, false, true, true); });
		VisitSnapshot(C.Test, *Old, 1, [&](const auto& R) { Expected.Check(C.Test, R, false, false, true); });
		VisitStore(C.Test, C.Store, 2, [&](const auto& R) { C.Test.TestEqual(TEXT("real excerpt refusal is visibly counted"), R.OmittedExcerpts, OmittedBefore + 1); });
		Pressure.Reset(); Old.Reset();
		return true;
	}, 3, 8 * 1024 * 1024, 4 * 1024 * 1024, false, 128 * 1024);

	return bAll;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorStoreBudgetClearRetainedTest,
	"VaRest.Inspector.Store.Budget.ClearRetained", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorStoreBudgetClearRetainedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	using namespace VaRest::Inspector::Private::BudgetTesting;
	FCapturePolicy Policy;
	Policy.Limits.MaxRecords = 1;
	Policy.Limits.PendingEvents = 32;
	FRoots Roots(*this, false);
	if (!Roots.Initialize(Policy.Limits.StoreBytes, Policy.Limits.PendingBytes, Policy.Limits.PendingEvents))
		return false;
	auto Clock = MakeShared<FReceiptClock, ESPMode::ThreadSafe>();
	{
		FStore Store(Policy.Limits, Clock);
		if (!TestTrue(TEXT("retained equation store initialized"), Store.Initialize(Roots.Budget))) return false;
		TUniquePtr<FCaptureQueue> Queue(new FCaptureQueue(Policy, 1, Roots.Pending.ToSharedRef(), Roots.Lifetime->Allocator));
		if (!TestTrue(TEXT("retained equation queue supported"), Queue->IsSupported()) ||
			!TestTrue(TEXT("retained equation witness bound"), Store.BindLossWitness(Roots.Pending.ToSharedRef()))) return false;
		Store.BeginGeneration(1, 0);
		FCase C{*this, Roots, Store, Queue, *Clock, Policy};
		const int64 B = Roots.Budget->BootstrapBytes;
		const int64 F = FStoreAccess::Plan(Store).FixedBytes - B;
		TestEqual(TEXT("fixed startup equals independently measured plan"), Roots.Budget->UsedBytes.load(), B + F);
		int64 StartedCharge = 0, CompletedCharge = 0;
		if (!Deliver(C, Started(1, false, false), EPush::Accepted, true, &StartedCharge) ||
			!Deliver(C, Completed(1, false), EPush::Accepted, true, &CompletedCharge)) return false;
		VisitStore(*this, Store, 1, [&](const auto& Row) {
			TestTrue(TEXT("retained equation is genuinely sealed"), Row.bSealed);
			TestEqual(TEXT("equation Started has no headers"), Row.Started.Fixed->Headers.Count, uint32(0));
			TestEqual(TEXT("equation Completed has no headers"), Row.Completed.Fixed->Headers.Count, uint32(0));
			TestTrue(TEXT("equation has no excerpt owners or client label"), Row.Excerpts.IsEmpty() && Row.ClientLabel.IsEmpty());
		});
		Queue->Close();
		while (Queue->TryPopMetadata().IsSet()) {}
		while (Queue->TryPopExcerpt().IsSet()) {}
		Queue.Reset();
		const int64 R = FStoreAccess::RowBound(Store) + 4096;
		const int64 T = FStoreAccess::TimelineBound(Store);
		const int64 P = StartedCharge + CompletedCharge;
		const int64 Controller = 4096;
		int64 O = 0, N = 0;
		if (!TestTrue(TEXT("independent actual snapshot requests measured without reservation"), FStoreAccess::SnapshotCosts(Store, O, N))) return false;
		TestTrue(TEXT("all independent retained terms positive"), B > 0 && F > 0 && R > 0 && T > 0 && P > 0 && O > 0 && N > 0);
		TestEqual(TEXT("row timeline and both packet owners independently charged"), Roots.Budget->UsedBytes.load(), B + F + R + T + P);
		FSnapshot Old;
		if (!Take(C, Old)) return false;
		const int64 L = B + F + R + T + P + Controller + O + N;
		AddInfo(FString::Printf(TEXT("ClearRetained certificate B=%lld F=%lld R=%lld T=%lld P=%lld C=%lld O=%lld N=%lld L=%lld"),
			B, F, R, T, P, Controller, O, N, L));
		TestEqual(TEXT("exact M3 independent equation before Clear"), Roots.Budget->UsedBytes.load(), L);
		TestEqual(TEXT("peak has no transient charge"), Roots.Budget->PeakBytes.load(), L);
		Store.Clear();
		TestEqual(TEXT("all retained components remain charged after Clear1"), Roots.Budget->UsedBytes.load(), L);
		Store.Clear();
		TestEqual(TEXT("all retained components remain charged after Clear2"), Roots.Budget->UsedBytes.load(), L);
		TestEqual(TEXT("peak remains exact after both Clears"), Roots.Budget->PeakBytes.load(), L);
		Old.Reset();
		TestEqual(TEXT("ordinary reader release leaves independently measured fixed storage"), Roots.Budget->UsedBytes.load(), B + F);
		TestEqual(TEXT("ordinary reader release frees every snapshot slot"), Roots.Budget->SnapshotControllers.load(), int64(0));
		// Empty fixed storage is destroyed directly at scope exit. No third Clear.
	}
	TestEqual(TEXT("ordinary fixed teardown leaves bootstrap"), Roots.Budget->UsedBytes.load(), Roots.Budget->BootstrapBytes);
	TestEqual(TEXT("ordinary fixed teardown frees adapter backing"), Roots.Lifetime->Probe.LiveBacking.load(), int64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorWeakControllerBudgetTest,
	"VaRest.Inspector.Store.WeakControllerBudget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorWeakControllerBudgetTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	using namespace VaRest::Inspector::Private::BudgetTesting;
	bool bAll = true;
	bAll &= WithStore(*this, TEXT("WeakControllerBudget: four controller slots survive strong release until final weak"), [](FCase& C) {
		if (!Deliver(C, Started(1))) return false;
		const int64 BaseBytes = C.Roots.Budget->UsedBytes.load();
		const int64 BaseBacking = C.Roots.Lifetime->Probe.LiveBacking.load();
		const int64 BaseControllers = C.Roots.Budget->Probe.LiveControllers.load();
		FSnapshot Strong[4], Out;
		FWeakSnapshot Weak[4];
		for (int32 Index = 0; Index < 4; ++Index)
		{
			if (!Take(C, Strong[Index])) return false;
			Weak[Index] = Strong[Index];
		}
		Out = Strong[0];
		const auto* Sentinel = Out.Get();
		C.Test.TestFalse(TEXT("fifth snapshot controller refused"), C.Store.TrySnapshot(Out));
		C.Test.TestTrue(TEXT("slot refusal preserves output"), Out.Get() == Sentinel);
		Out.Reset();
		for (auto& Owner : Strong) Owner.Reset();
		C.Test.TestEqual(TEXT("payload and nested domains freed at last strong"), C.Roots.Lifetime->Probe.LiveBacking.load(), BaseBacking);
		C.Test.TestEqual(TEXT("only four4096 controller credits remain"), C.Roots.Budget->UsedBytes.load(), BaseBytes + 4 * 4096);
		C.Test.TestEqual(TEXT("all four weak-held slots remain occupied"), C.Roots.Budget->SnapshotControllers.load(), int64(4));
		C.Test.TestEqual(TEXT("stored deleters live through final weak"), C.Roots.Budget->Probe.LiveControllers.load(), BaseControllers + 4);
		for (const auto& W : Weak) C.Test.TestFalse(TEXT("expired weak cannot reacquire payload"), W.Pin().IsValid());
		C.Test.TestFalse(TEXT("payload destruction did not open fifth slot"), C.Store.TrySnapshot(Out));
		C.Test.TestTrue(TEXT("null output remains null on weak-held refusal"), !Out);
		const int64 Destroyed = C.Roots.Budget->Probe.DestroyedControllers.load();
		std::atomic<bool> Released{false};
		auto Worker = Async(EAsyncExecution::Thread, [Owner = MoveTemp(Weak[0]), &Released]() mutable {
			Owner.Reset();
			Released.store(true, std::memory_order_release);
		});
		Worker.Get();
		C.Test.TestTrue(TEXT("worker final-weak release joined"), Released.load(std::memory_order_acquire));
		C.Test.TestEqual(TEXT("worker final weak releases exactly one controller"), C.Roots.Budget->Probe.DestroyedControllers.load(), Destroyed + 1);
		C.Test.TestEqual(TEXT("worker final weak releases exactly one slot"), C.Roots.Budget->SnapshotControllers.load(), int64(3));
		C.Test.TestEqual(TEXT("worker final weak releases exactly4096 credit"), C.Roots.Budget->UsedBytes.load(), BaseBytes + 3 * 4096);
		if (!Take(C, Out)) return false;
		C.Test.TestEqual(TEXT("new snapshot uses released slot"), C.Roots.Budget->SnapshotControllers.load(), int64(4));
		Out.Reset();
		for (auto& W : Weak) W.Reset();
		C.Test.TestEqual(TEXT("all final weak credits released normally"), C.Roots.Budget->UsedBytes.load(), BaseBytes);
		C.Test.TestEqual(TEXT("all final weak slots released normally"), C.Roots.Budget->SnapshotControllers.load(), int64(0));
		return true;
	}, 1);

	bAll &= WithStore(*this, TEXT("WeakControllerBudget: requested limits apply only after actual quiescence"), [](FCase& C) {
		if (!Deliver(C, Started(1)) || !Deliver(C, Completed(1))) return false;
		FSnapshot Old;
		if (!Take(C, Old)) return false;
		FNormalizedLimits Requested = C.Policy.Limits;
		Requested.StoreBytes /= 2;
		Requested.PendingBytes /= 2;
		Requested.PendingEvents = 8;
		C.Store.RequestLimits(Requested);
		C.Queue->Close();
		while (C.Queue->TryPopMetadata().IsSet()) {}
		while (C.Queue->TryPopExcerpt().IsSet()) {}
		C.Store.Clear();
		C.Test.TestFalse(TEXT("retained snapshot and live queue baseline prevent recreation"), C.Store.CanRecreateAccount(*C.Roots.Budget, *C.Roots.Pending));
		{
			auto Independent = C.Roots.NewBudgetForRecreation(C.Policy.Limits.StoreBytes);
			if (!C.Test.TestTrue(TEXT("separately preadmitted same-limit account exists"), bool(Independent))) return false;
			C.Test.TestFalse(TEXT("fresh account cannot bypass retained readers via Initialize"), C.Store.Initialize(Independent));
			C.Test.TestEqual(TEXT("rejected independent account remains at bootstrap"), Independent->UsedBytes.load(), Independent->BootstrapBytes);
		}

		C.Test.TestTrue(TEXT("same old account can readmit fixed storage"), C.Store.Initialize(C.Roots.Budget));
		C.Test.TestTrue(TEXT("requested limits remain pending"), C.Store.Diagnostics().bPendingLimits);
		C.Test.TestEqual(TEXT("old effective limit preserved while readers remain"), C.Store.Diagnostics().EffectiveStoreLimit, C.Policy.Limits.StoreBytes);
		C.Queue.Reset();
		C.Test.TestFalse(TEXT("retained snapshot still prevents recreation after queue closure"), C.Store.CanRecreateAccount(*C.Roots.Budget, *C.Roots.Pending));
		const int64 OldPeak = C.Roots.Budget->PeakBytes.load();
		const int64 OldPendingPeak = C.Roots.Pending->PeakBytes.load();
		Old.Reset();
		C.Store.Clear();
		C.Test.TestTrue(TEXT("actual quiescence permits recreation"), C.Store.CanRecreateAccount(*C.Roots.Budget, *C.Roots.Pending));
		{
			auto Independent = C.Roots.NewBudgetForRecreation(C.Policy.Limits.StoreBytes);
			if (!C.Test.TestTrue(TEXT("quiescent unrelated budget root preadmitted"), bool(Independent))) return false;
			FBudgetControlLease UnrelatedLease(*Independent);
			C.Test.TestFalse(TEXT("unrelated old identity cannot authorize otherwise quiescent recreation"), C.Store.CanRecreateAccount(*Independent, *C.Roots.Pending));
		}

		{
			auto IndependentPending = C.Roots.NewPendingForRecreation(C.Roots.Pending->ByteLimit, C.Roots.Pending->CountLimit);
			if (!C.Test.TestTrue(TEXT("separately preadmitted pending account exists"), bool(IndependentPending))) return false;
			C.Test.TestFalse(TEXT("unrelated pending account cannot authorize recreation"), C.Store.CanRecreateAccount(*C.Roots.Budget, *IndependentPending));
		}

		const uint64 OldIdentity = C.Roots.Budget->Identity;
		bool bOldQuiescent = C.Test.TestTrue(TEXT("old fixed baseline belongs to originating account"), C.FixedAccountIdentity == OldIdentity);
		bOldQuiescent &= C.Test.TestTrue(TEXT("old recreation phase has no queue"), !C.Queue);
		bOldQuiescent &= C.Test.TestEqual(TEXT("old recreation phase has no rows"), CountRows(C.Store), 0);
		bOldQuiescent &= C.Test.TestEqual(TEXT("old recreation phase has bootstrap credit only"), C.Roots.Budget->UsedBytes.load(), C.Roots.Budget->BootstrapBytes);
		bOldQuiescent &= C.Test.TestEqual(TEXT("old recreation phase has no live controllers"), C.Roots.Budget->Probe.LiveControllers.load(), int64(0));
		bOldQuiescent &= C.Test.TestEqual(TEXT("old recreation phase has no weak slots"), C.Roots.Budget->SnapshotControllers.load(), int64(0));
		bOldQuiescent &= C.Test.TestEqual(TEXT("old recreation phase frees all actual backing"), C.Roots.Lifetime->Probe.LiveBacking.load(), int64(0));
		bOldQuiescent &= C.Test.TestEqual(TEXT("old recreation phase drains Pending bytes"), C.Roots.Pending->Bytes.load(), int64(0));
		bOldQuiescent &= C.Test.TestEqual(TEXT("old recreation phase drains Pending count"), C.Roots.Pending->Count.load(), int64(0));
		bOldQuiescent &= C.Test.TestEqual(TEXT("old recreation phase drains excerpt reservations"), C.Roots.Pending->ExcerptReservations.load(), int64(0));
		if (!bOldQuiescent || !C.Test.TestTrue(TEXT("explicit recreation succeeds"), C.Roots.CompleteRecreation(C.Store, Requested))) return false;
		// Capture the replacement allocation set before any new queue or payload exists.
		const auto NewPlan = FStoreAccess::Plan(C.Store);
		bool bNewFixedOnly = C.Test.TestTrue(TEXT("successful recreation changes account identity"), C.Roots.Budget->Identity != OldIdentity);
		bNewFixedOnly &= C.Test.TestTrue(TEXT("new fixed phase has no queue"), !C.Queue);
		bNewFixedOnly &= C.Test.TestEqual(TEXT("new fixed phase has no rows"), CountRows(C.Store), 0);
		bNewFixedOnly &= C.Test.TestTrue(TEXT("new fixed phase has an independently measured supported plan"), NewPlan.bSupported && NewPlan.bFits);
		bNewFixedOnly &= C.Test.TestEqual(TEXT("new fixed credit equals effective plan"), C.Roots.Budget->UsedBytes.load(), NewPlan.FixedBytes);
		bNewFixedOnly &= C.Test.TestEqual(TEXT("new fixed phase drains Pending bytes"), C.Roots.Pending->Bytes.load(), int64(0));
		bNewFixedOnly &= C.Test.TestEqual(TEXT("new fixed phase drains Pending count"), C.Roots.Pending->Count.load(), int64(0));
		bNewFixedOnly &= C.Test.TestEqual(TEXT("new fixed phase drains excerpt reservations"), C.Roots.Pending->ExcerptReservations.load(), int64(0));
		bNewFixedOnly &= C.Test.TestEqual(TEXT("new fixed phase has no live controllers"), C.Roots.Budget->Probe.LiveControllers.load(), int64(0));
		bNewFixedOnly &= C.Test.TestEqual(TEXT("new fixed phase has no weak slots"), C.Roots.Budget->SnapshotControllers.load(), int64(0));
		if (!bNewFixedOnly) return false;
		const int64 NewFixedBacking = C.Roots.Lifetime->Probe.LiveBacking.load();
		C.Test.AddInfo(FString::Printf(TEXT("RecreatedFixedBaseline OldAccount=%llu NewAccount=%llu OldCredit=%lld NewCredit=%lld OldBacking=%lld NewBacking=%lld"),
			OldIdentity, C.Roots.Budget->Identity, C.FixedBytes, NewPlan.FixedBytes, C.FixedBacking, NewFixedBacking));
		C.FixedBytes = NewPlan.FixedBytes;
		C.FixedBacking = NewFixedBacking;
		C.FixedAccountIdentity = C.Roots.Budget->Identity;
		C.Test.TestEqual(TEXT("new effective store limit applied"), C.Store.Diagnostics().EffectiveStoreLimit, Requested.StoreBytes);
		C.Test.TestEqual(TEXT("new requested store limit visible"), C.Store.Diagnostics().RequestedStoreLimit, Requested.StoreBytes);
		C.Test.TestFalse(TEXT("pending limits cleared only after successful recreation"), C.Store.Diagnostics().bPendingLimits);
		C.Test.TestTrue(TEXT("store lifetime high-water preserved"), C.Roots.Budget->PeakBytes.load() >= OldPeak);
		C.Test.TestTrue(TEXT("pending lifetime high-water preserved"), C.Roots.Pending->PeakBytes.load() >= OldPendingPeak);
		C.Policy.Limits = Requested;
		if (!RotateQueue(C, 2)) return false;
		return Deliver(C, Started(2));
	}, 2, 64ll * 1024 * 1024, 4ll * 1024 * 1024, true);


	bAll &= WithStore(*this, TEXT("WeakControllerBudget: failed recreation preserves originating identity and effective limits"), [](FCase& C) {
		FNormalizedLimits Requested = C.Policy.Limits;
		Requested.StoreBytes = 8192;
		C.Store.RequestLimits(Requested);
		C.Queue->Close();
		while (C.Queue->TryPopMetadata().IsSet()) {}
		while (C.Queue->TryPopExcerpt().IsSet()) {}
		C.Queue.Reset();
		C.Store.Clear();
		C.Test.TestTrue(TEXT("failure transaction begins at real quiescence"), C.Store.CanRecreateAccount(*C.Roots.Budget, *C.Roots.Pending));
		C.Test.TestFalse(TEXT("unsupported genuine minimum refuses recreation"), C.Roots.CompleteRecreation(C.Store, Requested));
		C.Test.TestTrue(TEXT("failure preserves actual originating old account readmission"), C.Store.Initialize(C.Roots.Budget));
		C.Test.TestTrue(TEXT("failure preserves requested limits as pending"), C.Store.Diagnostics().bPendingLimits);
		C.Test.TestEqual(TEXT("failure preserves old effective limit"), C.Store.Diagnostics().EffectiveStoreLimit, C.Policy.Limits.StoreBytes);
		return true;
	}, 2, 64ll * 1024 * 1024, 4ll * 1024 * 1024, true);

	bAll &= WithStore(*this, TEXT("WeakControllerBudget: four expired weak slots survive Clear attach stop resume and pending limits"), [](FCase& C) {
		if (!Deliver(C, Started(1, true))) return false;
		FSnapshot Strong[4], Fifth;
		FWeakSnapshot Weak[4];
		for (int32 Index = 0; Index < 4; ++Index)
		{
			if (!Take(C, Strong[Index])) return false;
			Weak[Index] = Strong[Index];
		}
		for (auto& Owner : Strong) Owner.Reset();
		FNormalizedLimits Requested = C.Policy.Limits;
		Requested.StoreBytes = 8192;
		C.Store.RequestLimits(Requested);
		C.Test.TestTrue(TEXT("requested limit is below actually retained bytes"), Requested.StoreBytes < C.Roots.Budget->UsedBytes.load());
		for (uint64 Generation = 2; Generation <= 3; ++Generation)
		{
			C.Store.Clear();
			C.Test.TestEqual(TEXT("Clear keeps four4096 weak credits on old ledger"), C.Roots.Budget->UsedBytes.load(), C.Roots.Budget->BootstrapBytes + 4 * 4096);
			C.Test.TestEqual(TEXT("Clear keeps four expired snapshot slots"), C.Roots.Budget->SnapshotControllers.load(), int64(4));
			C.Test.TestTrue(TEXT("same old ledger readmits fixed storage while weak controllers survive"), C.Store.Initialize(C.Roots.Budget));
			if (!RotateQueue(C, Generation)) return false;
			FVaRestTraceSnapshot Attached;
			const auto Event = Started(Generation, true);
			Attached.InFlight.Add(Event.Payload.Get<FVaRestTraceStarted>());
			C.Test.TestTrue(TEXT("real attach survives weak-held cap"), C.Store.ApplyAttached(Attached, C.Policy, C.Roots.Pending.ToSharedRef(), C.Roots.Lifetime->Allocator));
			FStoreAccess::Stop(C.Store);
			const int64 Copies = C.Roots.Lifetime->Probe.TextBytesCopied.load();
			const int64 Refs = C.Roots.Lifetime->Probe.ExcerptRefsAcquired.load();
			C.Test.TestFalse(TEXT("fifth snapshot still refuses after actual lifecycle controls"), C.Store.TrySnapshot(Fifth));
			C.Test.TestTrue(TEXT("fifth refusal keeps output empty"), !Fifth);
			C.Test.TestEqual(TEXT("weak-held refusal performs no text copy"), C.Roots.Lifetime->Probe.TextBytesCopied.load(), Copies);
			C.Test.TestEqual(TEXT("weak-held refusal acquires no excerpt reference"), C.Roots.Lifetime->Probe.ExcerptRefsAcquired.load(), Refs);
			C.Test.TestTrue(TEXT("limits remain pending under weak-held controllers"), C.Store.Diagnostics().bPendingLimits);
			C.Test.TestEqual(TEXT("old effective limit remains during lifecycle"), C.Store.Diagnostics().EffectiveStoreLimit, C.Policy.Limits.StoreBytes);
		}
		for (auto& Owner : Weak)
		{
			C.Test.TestFalse(TEXT("expired weak remains unable to pin payload"), Owner.Pin().IsValid());
			Owner.Reset();
		}
		C.Test.TestEqual(TEXT("final weak release frees all slots"), C.Roots.Budget->SnapshotControllers.load(), int64(0));
		C.Test.TestTrue(TEXT("weak release alone does not apply requested limits"), C.Store.Diagnostics().bPendingLimits);
		C.Test.TestEqual(TEXT("weak release alone preserves old effective limit"), C.Store.Diagnostics().EffectiveStoreLimit, C.Policy.Limits.StoreBytes);
		return true;
	}, 2);

	{
		FCapturePolicy Policy;
		Policy.Limits.StoreBytes = 8192;
		FRoots Roots(*this, false);
		if (!Roots.Initialize(Policy.Limits.StoreBytes, Policy.Limits.PendingBytes, Policy.Limits.PendingEvents))
			return false;
		auto Clock = MakeShared<FReceiptClock, ESPMode::ThreadSafe>();
		FStore Store(Policy.Limits, Clock);
		TestFalse(TEXT("small budget initialization fails independently of snapshot slots"), Store.Initialize(Roots.Budget));
		TestEqual(TEXT("small budget has no snapshot controller"), Roots.Budget->SnapshotControllers.load(), int64(0));
		TestTrue(TEXT("small budget reports refusal or unsupported minimum"), Store.Diagnostics().RefusedRows > 0 || Store.Diagnostics().UnsupportedAllocator > 0);
	}
	bAll &= WithStore(*this, TEXT("WeakControllerBudget proof: four strong old payloads survive Clear attach stop resume before distinct weak teardown"), [](FCase& C) {
		if (!Deliver(C, RichProofStarted(1))) return false;
		FStartedProofImage Expected;
		bool bCaptured = false;
		VisitStore(C.Test, C.Store, 1, [&](const auto& R) { bCaptured = Expected.Capture(C.Test, R); });
		if (!bCaptured) return false;
		int64 Outer = 0, Nested = 0;
		if (!C.Test.TestTrue(TEXT("four-payload complete O/N independently measured"), FStoreAccess::SnapshotCosts(C.Store, Outer, Nested))) return false;
		int64 Each = 4096;
		if (!C.Test.TestTrue(TEXT("four-payload checked C/P/N charge"), AddChecked(Each, Outer, MAX_int64) && AddChecked(Each, Nested, MAX_int64) && Each <= MAX_int64 / 4)) return false;
		FSnapshot Strong[4], Fifth;
		FWeakSnapshot Weak[4];
		const uint64 AccountIdentity = C.Roots.Budget->Identity, SnapshotRevision = C.Store.Revision();
		for (int32 Index = 0; Index < 4; ++Index) { if (!Take(C, Strong[Index])) return false; Weak[Index] = Strong[Index]; }
		const auto CheckRetainedAtStage = [&]() {
			bool b = true;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				if (!C.Test.TestTrue(TEXT("old snapshot strong payload still valid during every control"), Strong[Index].IsValid())) return false;
				b &= C.Test.TestEqual(TEXT("old snapshot global revision unchanged by later controls"), Strong[Index]->GetRevision(), SnapshotRevision);
				VisitSnapshot(C.Test, *Strong[Index], 1, [&](const auto& R) { b &= Expected.Check(C.Test, R, false, false, true); });
			}
			Fifth = Strong[0]; const auto* Sentinel = Fifth.Get();
			const int64 Used = C.Roots.Budget->UsedBytes.load(), Copies = C.Roots.Lifetime->Probe.TextBytesCopied.load(), Refs = C.Roots.Lifetime->Probe.ExcerptRefsAcquired.load();
			b &= C.Test.TestFalse(TEXT("four strong old payloads prevent fifth after lifecycle stage"), C.Store.TrySnapshot(Fifth));
			b &= C.Test.TestTrue(TEXT("strong-held refusal preserves exact nonnull output"), Fifth.Get() == Sentinel);
			b &= C.Test.TestEqual(TEXT("strong-held refusal preserves aggregate ledger"), C.Roots.Budget->UsedBytes.load(), Used);
			b &= C.Test.TestEqual(TEXT("strong-held refusal copies no text"), C.Roots.Lifetime->Probe.TextBytesCopied.load(), Copies);
			b &= C.Test.TestEqual(TEXT("strong-held refusal acquires no reference"), C.Roots.Lifetime->Probe.ExcerptRefsAcquired.load(), Refs);
			b &= C.Test.TestEqual(TEXT("four strong snapshot slots span every stage"), C.Roots.Budget->SnapshotControllers.load(), int64(4));
			b &= C.Test.TestEqual(TEXT("controls retain originating budget identity"), C.Roots.Budget->Identity, AccountIdentity);
			b &= C.Test.TestTrue(TEXT("aggregate live store ledger remains within immutable cap"), C.Roots.Budget->UsedBytes.load() <= C.Roots.Budget->Limit && C.Roots.Budget->PeakBytes.load() <= C.Roots.Budget->Limit);
			Fifth.Reset();
			return b;
		};
		if (!CheckRetainedAtStage()) return false;
		for (uint64 Generation = 2; Generation <= 3; ++Generation)
		{
			C.Store.Clear();
			if (!CheckRetainedAtStage()) return false;
			if (!RotateQueue(C, Generation)) return false;
			if (!CheckRetainedAtStage()) return false;
			FVaRestTraceSnapshot Attached;
			const auto Event = Started(Generation, true);
			Attached.InFlight.Add(Event.Payload.Get<FVaRestTraceStarted>());
			if (!C.Test.TestTrue(TEXT("real source attach succeeds while all four old payloads remain"), C.Store.ApplyAttached(Attached, C.Policy, C.Roots.Pending.ToSharedRef(), C.Roots.Lifetime->Allocator))) return false;
			VisitStore(C.Test, C.Store, Generation, [&](const auto& R) { C.Test.TestTrue(TEXT("attached current source is pinned with genuine pending promise"), R.bPinned && !R.bTerminalKnown && R.RequestExcerptState == EVaRestInspectorDetailState::Pending); });
			if (!CheckRetainedAtStage()) return false;
			FStoreAccess::Stop(C.Store);
			VisitStore(C.Test, C.Store, Generation, [&](const auto& R) { C.Test.TestTrue(TEXT("real stop resolves only current promise without fabricating result"), R.RequestExcerptState == EVaRestInspectorDetailState::CaptureStopped && !R.bTerminalKnown && !R.bSealed); });
			if (!CheckRetainedAtStage()) return false;
		}
		C.Queue->Close(); while (C.Queue->TryPopMetadata().IsSet()) {} while (C.Queue->TryPopExcerpt().IsSet()) {} C.Queue.Reset();
		C.Store.Clear();
		C.Test.TestEqual(TEXT("four complete old payload domains remain independently charged after final live Clear"), C.Roots.Budget->UsedBytes.load(), C.FixedBytes + 4 * Each);
		for (auto& Owner : Strong) Owner.Reset();
		for (const auto& Owner : Weak) C.Test.TestFalse(TEXT("last strong release expires every old payload"), Owner.Pin().IsValid());
		C.Test.TestEqual(TEXT("separate last-strong teardown releases O/N but retains four controllers"), C.Roots.Budget->UsedBytes.load(), C.FixedBytes + 4 * 4096);
		C.Test.TestEqual(TEXT("last strong leaves four weak-held slots"), C.Roots.Budget->SnapshotControllers.load(), int64(4));
		C.Test.TestEqual(TEXT("last strong leaves four stored deleters"), C.Roots.Budget->Probe.LiveControllers.load(), int64(4));
		C.Test.TestEqual(TEXT("all old payload backing actually freed before weak release"), C.Roots.Lifetime->Probe.LiveBacking.load(), C.FixedBacking);
		C.Test.TestFalse(TEXT("weak-only retained controllers still block fifth"), C.Store.TrySnapshot(Fifth));
		C.Test.TestTrue(TEXT("weak-only refusal preserves empty output"), !Fifth);
		for (auto& Owner : Weak) Owner.Reset();
		C.Test.TestEqual(TEXT("separate final weak releases exact controller credits"), C.Roots.Budget->UsedBytes.load(), C.FixedBytes);
		C.Test.TestEqual(TEXT("separate final weak releases all four slots"), C.Roots.Budget->SnapshotControllers.load(), int64(0));
		C.Test.TestEqual(TEXT("separate final weak destroys every stored deleter"), C.Roots.Budget->Probe.LiveControllers.load(), int64(0));
		C.Test.TestEqual(TEXT("normal old payload cleanup restores exact fixed backing"), C.Roots.Lifetime->Probe.LiveBacking.load(), C.FixedBacking);
		return true;
	}, 2);

	return bAll;
}

#endif // WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR
