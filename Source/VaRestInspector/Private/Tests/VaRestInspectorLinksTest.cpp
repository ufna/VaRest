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
#include "VaRestInspectorLinks.h"
#include "VaRestInspectorQueue.h"
#include "VaRestInspectorStore.h"
#include "VaRestTrace.h"
#include <atomic>
#include <limits>

namespace VaRest::Inspector::Private::Testing
{
bool ReadLiveProfile(FAutomationTestBase& Test);
}
namespace VaRest::Inspector::Private::LinksTesting
{
using FSnapshot = TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>;
struct FStoreAccess
{
	static FStoreStoragePlan Plan(const FStore& Store) { return Store.EffectiveStoragePlan(); }
	static int32 Links(const FStore& Store) { return Store.NumLinks(); }
	static int32 Targets(const FStore& Store, uint64 Id) { return Store.NumLinksForTarget(Id); }
	static int64 RowBound(const FStore& Store) { return Store.RowStorageBound(); }
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
			Test.TestEqual(TEXT("links fixture pending count drained"), Pending->Count.load(), int64(0));
			Test.TestEqual(TEXT("links fixture pending bytes drained"), Pending->Bytes.load(), int64(0));
			Test.TestEqual(TEXT("links fixture excerpt reservations drained"), Pending->ExcerptReservations.load(), int64(0));
			Pending.Reset();
			StartupUsed.fetch_sub(4096);
		}
		if (Budget)
		{
			Test.TestEqual(TEXT("links fixture only bootstrap remains"), Budget->UsedBytes.load(), Budget->BootstrapBytes);
			Test.TestEqual(TEXT("links fixture controllers destroyed"), Budget->Probe.LiveControllers.load(), int64(0));
			Test.TestEqual(TEXT("links fixture final weak slots released"), Budget->SnapshotControllers.load(), int64(0));
			Budget.SafeRelease();
			StartupUsed.fetch_sub(4096);
		}
		if (Lifetime)
		{
			Test.TestEqual(TEXT("links fixture all actual backing freed"), Lifetime->Probe.LiveBacking.load(), int64(0));
			Lifetime.SafeRelease();
			StartupUsed.fetch_sub(4096);
		}
		Test.TestEqual(TEXT("links fixture allowances released after actual root frees"), StartupUsed.load(), int64(0));
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
		Test.AddInfo(FString::Printf(TEXT("LinksRowCalibration Request=%lld Alignment=%lld Capacity=%lld StartupLimit=%lld"),
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
		Test.AddInfo(FString::Printf(TEXT("LinksFixtureRoot Request=%lld Alignment=%lld Capacity=%lld StartupLimit=%lld"),
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
		if (!Test.TestTrue(TEXT("links store initializes"), Store.Initialize(Roots.Budget)))
			return false;
		const int64 Fixed = Roots.Budget->UsedBytes.load(), FixedBacking = Roots.Lifetime->Probe.LiveBacking.load();
		const auto Plan = FStoreAccess::Plan(Store);
		Test.TestEqual(TEXT("actual fixed plan equals startup charge"), Fixed, Plan.FixedBytes);
		Test.TestTrue(TEXT("actual measured row bound is supported"), FStoreAccess::RowBound(Store) > 0);
		Test.AddInfo(FString::Printf(TEXT("LinksPlan Fixed=%lld RowBound=%lld MaxRows=%d Links=%d Targets=%d Retained=%d"),
			Plan.FixedBytes, FStoreAccess::RowBound(Store), Plan.MaxRows, Plan.LinkCapacity, Plan.LinkTargetCapacity, Plan.RetainedCapacity));
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
static FVaRestTraceEvent Started(uint64 Id, uint64 Original = 0, const TCHAR* QueueId = TEXT(""), uint64 Scope = 0, uint64 Client = 9)
{
	FVaRestTraceStarted V;
	V.RequestId = Id;
	V.ScopeId = Scope;
	V.ClientId = Client;
	V.Verb = TEXT("GET");
	V.Url = TEXT("https://example.test/links");
	V.StartedAt = 1;
	V.CurrentAttempt = 1;
	V.QueueReplayOf.OriginalRequestId = Original;
	V.QueueReplayOf.QueueId = QueueId;
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::Started;
	E.Seq = Id * 10 + 1;
	E.Time = 1;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Completed(uint64 Id, uint64 Refresh = 0, EVaRestTraceOutcome Outcome = EVaRestTraceOutcome::Success)
{
	FVaRestTraceCompleted V;
	V.RequestId = Id;
	V.AuthRefreshId = Refresh;
	V.Outcome = Outcome;
	V.Response.Status = 200;
	V.Response.BodyBytes = 0;
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::Completed;
	E.Seq = Id * 10 + 2;
	E.Time = 2;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceCompleted>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Dispatch(uint64 Id, uint64 Refresh)
{
	FVaRestTraceAttemptDispatched V;
	V.RequestId = Id;
	V.DispatchId = Id + 100;
	V.Attempt = 1;
	V.AuthRefreshId = Refresh;
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::AttemptDispatched;
	E.Seq = Id * 10 + 3;
	E.Time = 1.5;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptDispatched>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Note(uint64 Id, const TCHAR* QueueId, uint64 Seq = 777)
{
	FVaRestTraceNote V;
	V.RequestId = Id;
	V.Kind = EVaRestTraceNoteKind::QueueExpired;
	V.Attempt = 0;
	V.QueueId = QueueId;
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::Note;
	E.Seq = Seq;
	E.Time = 77.25;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceNote>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Marker(uint64 Cause = 0)
{
	FVaRestTraceMarker V;
	V.ScopeId = 0;
	V.ClientId = 9;
	V.CauseRequestId = Cause;
	V.Kind = EVaRestTraceMarkerKind::Offline;
	V.Text = TEXT("offline");
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::Marker;
	E.Seq = 900;
	E.Time = 90.5;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceMarker>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Scope(uint64 Id, bool Open, const TCHAR* Label)
{
	FVaRestTraceScope V;
	V.ScopeId = Id;
	V.bOpened = Open;
	V.Label = Label;
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::Scope;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceScope>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Client(uint64 Id, uint64 ScopeId, const TCHAR* Label)
{
	FVaRestTraceClient V;
	V.ClientId = Id;
	V.ScopeId = ScopeId;
	V.DisplayName = Label;
	FVaRestTraceEvent E;
	E.Kind = EVaRestTraceKind::Client;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceClient>(), MoveTemp(V));
	return E;
}
static bool Deliver(FCase& C, const FVaRestTraceEvent& E, EPush Expected = EPush::Accepted, bool ApplyExpected = true)
{
	if (!C.Test.TestTrue(TEXT("real queue admits expected codec form"), C.Queue.TryPush(E) == Expected))
		return false;
	auto Packet = E.Kind == EVaRestTraceKind::Excerpt ? C.Queue.TryPopExcerpt() : C.Queue.TryPopMetadata();
	if (!C.Test.TestTrue(TEXT("actual packet dequeued"), Packet.IsSet()))
		return false;
	return C.Test.TestEqual(TEXT("real Store application matches expectation"), C.Store.Apply(MoveTemp(Packet.GetValue())), ApplyExpected);
}
static FSnapshot Take(FCase& C)
{
	FSnapshot Out;
	C.Test.TestTrue(TEXT("real owning snapshot admits"), C.Store.TrySnapshot(Out));
	C.Test.TestTrue(TEXT("real owning snapshot valid"), Out.IsValid());
	return Out;
}
template <typename F>
static void Row(FCase& C, uint64 Id, F&& Check)
{
	int32 Found = 0;
	C.Test.TestTrue(TEXT("GT store visitor succeeds"), C.Store.ForEachRecord([&](const auto& V) {
		if (V.Kind == EVaRestInspectorRecordKind::Http && V.RequestId == Id)
		{
			++Found;
			Check(V);
		}
	}));
	C.Test.TestEqual(TEXT("exact HTTP row observed"), Found, 1);
}
template <typename F>
static void SnapshotRow(FCase& C, const FSnapshot& S, uint64 Id, F&& Check)
{
	if (!C.Test.TestTrue(TEXT("snapshot owner exists before visit"), S.IsValid()))
		return;
	int32 Found = 0;
	S->ForEachRecord([&](const auto& V) { if (V.Kind==EVaRestInspectorRecordKind::Http && V.RequestId==Id) { ++Found; Check(V); } });
	C.Test.TestEqual(TEXT("exact retained HTTP row observed"), Found, 1);
}
static int32 Markers(const FStore& Store, EVaRestInspectorMarkerKind Kind)
{
	int32 Count = 0;
	Store.ForEachRecord([&](const auto& V) { Count += V.MarkerInfo.Kind == Kind ? 1 : 0; });
	return Count;
}
static void Link(FCase& C, const FSnapshot& S, uint64 From, uint64 To, bool Auth, const TCHAR* QueueId, bool Evicted = false)
{
	if (!C.Test.TestTrue(TEXT("link snapshot owner valid"), S.IsValid()))
		return;
	int32 Found = 0;
	for (const auto& L : S->GetLinks())
		if (L.FromRequestId == From && L.ToRequestId == To && L.bAuth == Auth && L.QueueId.Equals(QueueId, ESearchCase::CaseSensitive))
		{
			++Found;
			C.Test.TestEqual(TEXT("target availability projected from actual retained ids"), L.bTargetEvicted, Evicted);
		}
	C.Test.TestEqual(TEXT("exact independent link tuple occurs once"), Found, 1);
}
static FString Content(const FVaRestInspectorRecordView& V)
{
	FString Out = FString::Printf(TEXT("%llu/%llu/%llu/%d/%d/%d/%d/%d/%d/%d/%d/%lld/%llu/%llu/"),
		V.RequestId, V.ScopeId, V.ClientId, int(V.Kind), int(V.bContextKnown), int(V.bSealed), int(V.bPinned), int(V.bTerminalKnown),
		int(V.EndState), int(V.DispatchHistory), V.FinalStatus, V.FinalBodyBytes, V.ArrivalOrdinal, V.BirthMetadataOrdinal);
	if (!V.ScopeLabel.IsEmpty())
		Out.Append(V.ScopeLabel.GetData(), V.ScopeLabel.Len());
	Out += TEXT("|");
	if (!V.ClientLabel.IsEmpty())
		Out.Append(V.ClientLabel.GetData(), V.ClientLabel.Len());
	const FVaRestInspectorPacketView Packets[] = {V.Started, V.Completed, V.Marker};
	for (const auto& P : Packets)
	{
		Out += P.Fixed ? TEXT("P") : TEXT("-");
		if (P.Fixed)
		{
			Out += FString::Printf(TEXT("%llu/%g/%d/%d"), P.Fixed->Header.Seq, P.Fixed->Header.Time, int(P.Fixed->Header.Kind), int(P.Fixed->Header.bMetadataOmitted));
			for (int32 I = 0; I < int32(EText::Count); ++I)
			{
				const auto T = P.Read(P.Fixed->Text[I]);
				Out += TEXT("|");
				if (!T.IsEmpty())
					Out.Append(T.GetData(), T.Len());
			}
			for (uint8 B : P.Backing)
				Out += FString::Printf(TEXT("%02x"), uint32(B));
		}
	}
	for (const auto& T : V.Timeline)
		Out += FString::Printf(TEXT("/%d/%llu/%g/%d/%llu/%d"), int(T.Kind), T.Seq, T.Time, T.Attempt, T.DispatchId, int(T.DetailState));
	return Out;
}
} // namespace VaRest::Inspector::Private::LinksTesting

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorLinksAndMarkersTest, "VaRest.Inspector.Store.LinksAndMarkers", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorLinksAndMarkersTest::RunTest(const FString&)
{
	using namespace VaRest::Inspector::Private;
	using namespace VaRest::Inspector::Private::LinksTesting;
	bool All = true;
	All &= WithStore(*this, TEXT("Links: observed auth backlinks leave sealed refresh bytes/revision immutable; actual tombstones expire"), [](FCase& C) {
        if (!Deliver(C,Started(10)) || !Deliver(C,Completed(10))) return false;
        uint64 SealedRevision=0; FString SealedContent;
        Row(C,10,[&](const auto& V) { C.Test.TestTrue(TEXT("refresh10 actually sealed"),V.bSealed); SealedRevision=V.Revision; SealedContent=Content(V); });
        auto Old=Take(C); if (!Old) return false;
        if (!Deliver(C,Started(11))) return false;
        C.Test.TestEqual(TEXT("parked source alone invents no auth edge"),FStoreAccess::Links(C.Store),0);
        const uint64 Before=C.Store.Revision();
        if (!Deliver(C,Dispatch(11,10))) return false;
        C.Test.TestTrue(TEXT("external auth observation advances global revision"),C.Store.Revision()>Before);
        Row(C,10,[&](const auto& V) {
            C.Test.TestEqual(TEXT("new backlink never touches sealed target revision"),V.Revision,SealedRevision);
            C.Test.TestEqualSensitive(TEXT("new backlink preserves sealed target content"),Content(V),SealedContent);
        });
        SnapshotRow(C,Old,10,[&](const auto& V) {
            C.Test.TestEqual(TEXT("retained sealed target revision stays immutable"),V.Revision,SealedRevision);
            C.Test.TestEqualSensitive(TEXT("retained sealed target packet bytes stay immutable"),Content(V),SealedContent);
        });
        auto Linked=Take(C); Link(C,Linked,11,10,true,TEXT("")); Linked.Reset();
        // Two live slots:11 stays pinned, so ordinary admission evicts10.
        if (!Deliver(C,Completed(20))) return false;
        auto Evicted=Take(C); Link(C,Evicted,11,10,true,TEXT(""),true); Evicted.Reset();
        C.Test.TestEqual(TEXT("real scaled tombstone ring size"),FStoreAccess::Plan(C.Store).RetainedCapacity,2);
        if (!Deliver(C,Completed(21)) || !Deliver(C,Completed(22))) return false;
        auto Expired=Take(C); Link(C,Expired,11,10,true,TEXT(""),false); Expired.Reset();
        // Actual completion-only observation also supplies its explicit auth id.
        if (!Deliver(C,Completed(30,10)) || !Deliver(C,Completed(11,10))) return false;
        auto Final=Take(C); Link(C,Final,11,10,true,TEXT("")); Link(C,Final,30,10,true,TEXT(""));
        C.Test.TestEqual(TEXT("dispatched/completed same tuple deduplicates"),Final?Final->GetLinks().Num():-1,2);
        SnapshotRow(C,Old,10,[&](const auto& V) { C.Test.TestEqualSensitive(TEXT("eviction never mutates retained target"),Content(V),SealedContent); });
        return true; }, 2);

	All &= WithStore(*this, TEXT("Links: real queue replay keeps owned GUID across Clear; original0 never guesses tempting12"), [](FCase& C) {
		if (!Deliver(C, Started(12)) || !Deliver(C, Started(13, 12, TEXT("q"))))
			return false;
		auto Old = Take(C);
		Link(C, Old, 13, 12, false, TEXT("q"));
		C.Store.Clear();
		C.Queue.Close();
		C.Store.BeginGeneration(2, 0);
		FCaptureQueue RebootQueue(C.Policy, 2, C.Roots.Pending.ToSharedRef(), C.Roots.Lifetime->Allocator);
		ON_SCOPE_EXIT
		{
			RebootQueue.Close();
			while (RebootQueue.TryPopMetadata().IsSet())
			{
			}
			while (RebootQueue.TryPopExcerpt().IsSet())
			{
			}
		};
		FCase Reboot{C.Test, C.Roots, C.Store, RebootQueue, C.Clock, C.Policy};
		if (!Deliver(Reboot, Started(12)) || !Deliver(Reboot, Started(13, 0, TEXT("q"))))
			return false;
		auto Current = Take(Reboot);
		Link(Reboot, Current, 13, 0, false, TEXT("q"));
		if (!Current)
			return false;
		C.Test.TestEqual(TEXT("one actual GUID-only replay link"), Current->GetLinks().Num(), 1);
		for (const auto& L : Current->GetLinks())
			C.Test.TestFalse(TEXT("no numeric cross-process13-to12 guess"), L.FromRequestId == 13 && L.ToRequestId == 12);
		C.Test.TestEqual(TEXT("zero is not a real target directory"), FStoreAccess::Targets(C.Store, 0), 0);
		Link(C, Old, 13, 12, false, TEXT("q"));
		return true;
	});

	All &= WithStore(*this, TEXT("Links: actual global-only GUID4096 FIFO, real-target64 and exact dedup"), [](FCase& C) {
		C.Test.TestEqual(TEXT("full global link capacity selected"), FStoreAccess::Plan(C.Store).LinkCapacity, 4096);
		for (uint64 Id = 1; Id <= 4097; ++Id)
		{
			const FString Q = FString::Printf(TEXT("unknown-%llu"), Id);
			const auto R = C.Store.TryAdmitLink({Id, 0, FStringView(Q), false}, {});
			if (!C.Test.TestTrue(TEXT("real zero-target edge admitted"), R == (Id <= 4096 ? ELinkAdmission::Added : ELinkAdmission::ReplacedOldest)))
				return false;
		}
		C.Test.TestEqual(TEXT("zero target has no target64 bucket"), FStoreAccess::Targets(C.Store, 0), 0);
		auto Full = Take(C);
		if (!Full)
			return false;
		C.Test.TestEqual(TEXT("real published global cap"), Full->GetLinks().Num(), 4096);
		if (Full->GetLinks().Num() != 4096)
			return false;
		for (int32 I = 0; I < 4096; ++I)
		{
			const auto& L = Full->GetLinks()[I];
			const uint64 Id = uint64(I) + 2;
			C.Test.TestEqual(TEXT("global FIFO surviving identity"), L.FromRequestId, Id);
			C.Test.TestEqual(TEXT("unknown original remains zero"), L.ToRequestId, uint64(0));
			C.Test.TestEqualSensitive(TEXT("independently owned distinct GUID"), L.QueueId, FString::Printf(TEXT("unknown-%llu"), Id));
			C.Test.TestFalse(TEXT("unknown target is not evicted"), L.bTargetEvicted);
		}
		const uint64 Revision = C.Store.Revision();
		const int64 Used = C.Roots.Budget->UsedBytes.load();
		C.Test.TestTrue(TEXT("exact duplicate is a no-op"), C.Store.TryAdmitLink({4097, 0, TEXT("unknown-4097"), false}, {}) == ELinkAdmission::AlreadyPresent);
		C.Test.TestEqual(TEXT("dedup creates no charge"), C.Roots.Budget->UsedBytes.load(), Used);
		C.Test.TestEqual(TEXT("dedup creates no revision"), C.Store.Revision(), Revision);
		C.Store.Clear();
		for (uint64 Id = 1; Id <= 65; ++Id)
			if (!C.Test.TestTrue(TEXT("real target edges obey target FIFO"), C.Store.TryAdmitLink({Id, 900, {}, true}, {}) == (Id <= 64 ? ELinkAdmission::Added : ELinkAdmission::ReplacedOldest)))
				return false;
		auto Target = Take(C);
		if (!Target)
			return false;
		C.Test.TestEqual(TEXT("absent real numeric target still has cap64"), Target->GetLinks().Num(), 64);
		C.Test.TestEqual(TEXT("actual target directory count64"), FStoreAccess::Targets(C.Store, 900), 64);
		if (Target->GetLinks().Num() != 64)
			return false;
		for (int32 I = 0; I < 64; ++I)
			C.Test.TestEqual(TEXT("target FIFO identity survives exactly"), Target->GetLinks()[I].FromRequestId, uint64(I) + 2);
		Link(C, Full, 4097, 0, false, TEXT("unknown-4097"));
		Target.Reset();
		C.Store.Clear();
		if (!C.Test.TestTrue(TEXT("oldest unrelated real target edge"), C.Store.TryAdmitLink({1, 800, {}, true}, {}) == ELinkAdmission::Added))
			return false;
		for (uint64 Id = 100; Id < 164; ++Id)
			if (!C.Test.TestTrue(TEXT("target64 prepared behind unrelated global oldest"), C.Store.TryAdmitLink({Id, 900, {}, true}, {}) == ELinkAdmission::Added))
				return false;
		for (uint64 I = 0; I < 4031; ++I)
			if (!C.Test.TestTrue(TEXT("global cap filled independently of target900"), C.Store.TryAdmitLink({10000 + I, 100000 + I, {}, true}, {}) == ELinkAdmission::Added))
				return false;
		C.Test.TestTrue(TEXT("both real caps replace their own oldest"), C.Store.TryAdmitLink({164, 900, {}, true}, {}) == ELinkAdmission::ReplacedOldest);
		auto Both = Take(C);
		if (!Both)
			return false;
		C.Test.TestEqual(TEXT("global then target FIFO may remove two distinct edges"), Both->GetLinks().Num(), 4095);
		C.Test.TestEqual(TEXT("target64 remains exact under simultaneous caps"), FStoreAccess::Targets(C.Store, 900), 64);
		for (const auto& L : Both->GetLinks())
			C.Test.TestTrue(TEXT("both distinct oldest identities really removed"), L.FromRequestId != 1 && L.FromRequestId != 100);
		Link(C, Both, 164, 900, true, TEXT(""));
		return true;
	});

	All &= WithStore(*this, TEXT("Links: bounded full-probe deleted target and forward-index collision churn"), [](FCase& C) {
        const auto P=FStoreAccess::Plan(C.Store);
        C.Test.TestTrue(TEXT("small store uses actual scaled link cap"),P.LinkCapacity>0 && P.LinkCapacity<4096);
        const int32 Rounds=P.LinkTargetCapacity*2+P.LinkCapacity;
        TArray<uint64> FromIds; FromIds.Reserve(Rounds);
        uint64 PreviousFrom=0;
        for(int32 I=0;I<Rounds;++I)
        {
            // Target hashes collide in the actual power-of-two directory.
            const uint64 Target=100+uint64(I)*uint64(P.LinkTargetCapacity);
            // Choose real full tuples colliding in the forward index too.
            // The bounded upstream search is not an admission/oracle hook.
            uint64 From=0;
            for(int32 Attempt=0;Attempt<P.LinkIndexCapacity*32;++Attempt)
            {
                const uint64 Candidate=++PreviousFrom;
                uint32 H=HashCombine(GetTypeHash(Candidate),GetTypeHash(Target));
                H=HashCombine(H,GetTypeHash(FStringView())); H=HashCombine(H,1u);
                if((H & uint32(P.LinkIndexCapacity-1))==0) { From=Candidate; break; }
            }
            if(!C.Test.TestTrue(TEXT("bounded actual forward collision constructed"),From!=0)) return false;
            FromIds.Add(From);
            const auto R=C.Store.TryAdmitLink({From,Target,{},true},{});
            if(!C.Test.TestTrue(TEXT("ordinary colliding churn admits/reuses tombstones"),R==ELinkAdmission::Added || R==ELinkAdmission::ReplacedOldest)) return false;
        }
        auto S=Take(C); if(!S) return false;
        C.Test.TestEqual(TEXT("scaled global cap holds under long churn"),S->GetLinks().Num(),P.LinkCapacity);
        if(S->GetLinks().Num()!=P.LinkCapacity) return false;
        for(int32 I=0;I<P.LinkCapacity;++I)
        {
            const int32 Ordinal=Rounds-P.LinkCapacity+I;
            C.Test.TestEqual(TEXT("churn forward FIFO identity"),S->GetLinks()[I].FromRequestId,FromIds[Ordinal]);
            C.Test.TestEqual(TEXT("churn exact colliding target"),S->GetLinks()[I].ToRequestId,100+uint64(Ordinal)*uint64(P.LinkTargetCapacity));
        }
        const auto Omitted=C.Store.Diagnostics().OmittedLinks;
        auto Pressure=FStoreCredit::Acquire(C.Roots.Budget,C.Roots.Budget->Limit-C.Roots.Budget->UsedBytes.load());
        if(!C.Test.TestTrue(TEXT("real pressure credit admitted"),Pressure.IsSet())) return false;
        C.Test.TestTrue(TEXT("independent nonempty edge refuses without room"),C.Store.TryAdmitLink({999999,0,TEXT("not-copied"),false},{})==ELinkAdmission::Refused);
        C.Test.TestEqual(TEXT("link refusal is diagnosed"),C.Store.Diagnostics().OmittedLinks,Omitted+1);
        Pressure.Reset();
        return true; }, 4, 1024 * 1024);

	All &= WithStore(*this, TEXT("Context: genuine Process metadata, scoped clients and unknown mutable orphans in both visitors"), [](FCase& C) {
		const uint64 Initial = C.Store.Revision();
		if (!Deliver(C, Scope(0, true, TEXT("Process"))))
			return false;
		C.Test.TestTrue(TEXT("first actual Process open observation changes global metadata"), C.Store.Revision() > Initial);
		const uint64 Open = C.Store.Revision();
		if (!Deliver(C, Scope(0, true, TEXT("Process"))))
			return false;
		C.Test.TestEqual(TEXT("same Process metadata does not invent a change"), C.Store.Revision(), Open);
		if (!Deliver(C, Scope(7, true, TEXT("PIE fiction"))) || !Deliver(C, Scope(0, false, TEXT("untrusted-label"))))
			return false;
		C.Test.TestEqual(TEXT("unsupported scope and label visibly omitted"), C.Store.Diagnostics().OmittedScopes, uint64(2));
		int32 BeforeRows = 0;
		C.Store.ForEachRecord([&](const auto&) { ++BeforeRows; });
		C.Test.TestEqual(TEXT("scope metadata creates no HTTP row"), BeforeRows, 0);
		if (!Deliver(C, Client(9, 0, TEXT("client-nine"))) || !Deliver(C, Started(1)) || !Deliver(C, Note(2, TEXT("orphan-q"))))
			return false;
		Row(C, 1, [&](const auto& V) { C.Test.TestTrue(TEXT("actual Started certifies Process context"), V.bContextKnown && V.ScopeLabel == TEXT("Process") && V.ClientLabel == TEXT("client-nine")); });
		Row(C, 2, [&](const auto& V) { C.Test.TestFalse(TEXT("orphan zero ids do not certify context"),V.bContextKnown); C.Test.TestTrue(TEXT("orphan default0 remains unlabeled"),V.ScopeLabel.IsEmpty() && V.ClientLabel.IsEmpty()); });
		auto Mutable = Take(C);
		SnapshotRow(C, Mutable, 1, [&](const auto& V) { C.Test.TestTrue(TEXT("mutable snapshot carries proven Process/client label"), V.bContextKnown && V.ScopeLabel == TEXT("Process") && V.ClientLabel == TEXT("client-nine")); });
		SnapshotRow(C, Mutable, 2, [&](const auto& V) { C.Test.TestTrue(TEXT("mutable orphan snapshot remains unknown"), !V.bContextKnown && V.ScopeLabel.IsEmpty() && V.ClientLabel.IsEmpty()); });
		if (!Deliver(C, Client(9, 99, TEXT("wrong-scope"))) || !Deliver(C, Completed(1)))
			return false;
		Row(C, 1, [&](const auto& V) { C.Test.TestTrue(TEXT("wrong-scope client label supplies no attribution"), V.bSealed && V.bContextKnown && V.ScopeLabel == TEXT("Process") && V.ClientLabel.IsEmpty()); });
		auto Sealed = Take(C);
		SnapshotRow(C, Sealed, 1, [&](const auto& V) { C.Test.TestTrue(TEXT("sealed snapshot uses identical scope predicate"), V.bContextKnown && V.ScopeLabel == TEXT("Process") && V.ClientLabel.IsEmpty()); });
		FVaRestTraceSnapshot Attached;
		FVaRestTraceScope P;
		P.ScopeId = 0;
		P.bOpened = true;
		P.Label = TEXT("Process");
		Attached.Scopes.Add(P);
		FVaRestTraceScope Unsupported;
		Unsupported.ScopeId = 8;
		Unsupported.Label = TEXT("another PIE fiction");
		Attached.Scopes.Add(Unsupported);
		FVaRestTraceClient A;
		A.ClientId = 9;
		A.ScopeId = 0;
		A.DisplayName = TEXT("attached-nine");
		Attached.Clients.Add(A);
		auto Started3 = Started(3);
		Attached.InFlight.Add(Started3.Payload.Get<FVaRestTraceStarted>());
		const uint64 BeforeAttach = C.Store.Revision();
		if (!C.Test.TestTrue(TEXT("actual attached Scope/Client/Started ingestion"), C.Store.ApplyAttached(Attached, C.Policy, C.Roots.Pending.ToSharedRef(), C.Roots.Lifetime->Allocator)))
			return false;
		C.Test.TestTrue(TEXT("attached Process/client metadata updates global revision"), C.Store.Revision() > BeforeAttach);
		C.Test.TestEqual(TEXT("unsupported attached scope is visible"), C.Store.Diagnostics().OmittedScopes, uint64(3));
		Row(C, 3, [&](const auto& V) { C.Test.TestTrue(TEXT("validated attached Started carries context before optional text"), V.bContextKnown && V.ScopeLabel == TEXT("Process") && V.ClientLabel == TEXT("attached-nine")); });
		SnapshotRow(C, Mutable, 1, [&](const auto& V) { C.Test.TestTrue(TEXT("old client copy survives metadata updates"), V.ClientLabel == TEXT("client-nine")); });
		if (!Deliver(C, Marker(3)))
			return false;
		int32 Core = 0, UnknownDerived = 0;
		C.Store.ForEachRecord([&](const auto& V) {
			if (V.MarkerInfo.Kind == EVaRestInspectorMarkerKind::Core)
			{
				++Core;
				C.Test.TestTrue(TEXT("real core marker context and packet retained"), V.bContextKnown && V.ScopeLabel == TEXT("Process") && V.ClientLabel.IsEmpty() && V.Marker.Fixed);
				C.Test.TestTrue(TEXT("real core source fields preserved"), V.MarkerInfo.bHasSourceObservation && V.MarkerInfo.SourceSeq == 900 && V.MarkerInfo.SourceTime == 90.5 && V.MarkerInfo.CauseRequestId == 3);
			}
			if (V.MarkerInfo.Kind == EVaRestInspectorMarkerKind::DerivedQueueExpired && V.MarkerInfo.CauseRequestId == 2)
			{
				++UnknownDerived;
				C.Test.TestTrue(TEXT("orphan expiry never inherits current Process/client metadata"), !V.bContextKnown && V.ScopeLabel.IsEmpty() && V.ClientLabel.IsEmpty());
			}
		});
		C.Test.TestEqual(TEXT("one real core marker"), Core, 1);
		C.Test.TestEqual(TEXT("one unknown-context derived marker"), UnknownDerived, 1);
		C.Store.Clear();
		SnapshotRow(C, Mutable, 2, [&](const auto& V) { C.Test.TestTrue(TEXT("old orphan copy stays unknown after Clear"), !V.bContextKnown && V.ScopeLabel.IsEmpty()); });
		return true;
	});

	All &= WithStore(*this, TEXT("Markers: actual accepted QueueExpired Note, source omission, immutable owned text and shared arrival"), [](FCase& C) {
		if (!Deliver(C, Started(1)) || !Deliver(C, Marker(1)) || !Deliver(C, Note(1, TEXT("expiry-guid"))))
			return false;
		Row(C, 1, [&](const auto& V) {
			C.Test.TestEqual(TEXT("typed source Note remains retained exactly once"), V.Notes.Num(), 1);
			if (V.Notes.Num() == 1)
				C.Test.TestTrue(TEXT("source Note Attempt0 and GUID unchanged"), V.Notes[0].Fixed->Scalars.NoteKind == EVaRestTraceNoteKind::QueueExpired && V.Notes[0].Fixed->Scalars.Attempt == 0 && V.Notes[0].Read(V.Notes[0].Fixed->Text[int32(EText::QueueId)]) == TEXT("expiry-guid"));
		});
		uint64 Last = 0;
		int32 Position = 0, Derived = 0;
		C.Store.ForEachRecord([&](const auto& V) {
			C.Test.TestTrue(TEXT("HTTP/core/derived use one arrival order"), V.ArrivalOrdinal > Last);
			Last = V.ArrivalOrdinal;
			if (Position == 0)
				C.Test.TestTrue(TEXT("first arrival HTTP1"), V.Kind == EVaRestInspectorRecordKind::Http && V.RequestId == 1);
			if (Position == 1)
				C.Test.TestTrue(TEXT("second arrival core marker"), V.MarkerInfo.Kind == EVaRestInspectorMarkerKind::Core);
			if (V.MarkerInfo.Kind == EVaRestInspectorMarkerKind::DerivedQueueExpired)
			{
				++Derived;
				C.Test.TestTrue(TEXT("derived tag is sealed and never reinterprets Note as core Marker"), V.bSealed && !V.Marker.Fixed);
				C.Test.TestTrue(TEXT("exact actual Note source/cause and local receipt preserved"), V.MarkerInfo.bHasSourceObservation && V.MarkerInfo.SourceSeq == 777 && V.MarkerInfo.SourceTime == 77.25 && V.MarkerInfo.CauseRequestId == 1 && V.MarkerInfo.ObservedAt == 123.5);
				C.Test.TestTrue(TEXT("derived marker owns GUID and proven context without present client label"), V.MarkerInfo.QueueId == TEXT("expiry-guid") && V.bContextKnown && V.ScopeLabel == TEXT("Process") && V.ClientLabel.IsEmpty() && !V.bMetadataOmitted);
			}
			++Position;
		});
		C.Test.TestEqual(TEXT("one marker from one actually accepted Note"), Derived, 1);
		auto Retained = Take(C);
		if (!Retained)
			return false;
		const FString TooLong = FString::ChrN(FNormalizedLimits::MaxStringChars + 1, TCHAR('q'));
		if (!Deliver(C, Note(1, *TooLong, 778), EPush::ScalarOnly))
			return false;
		int32 Omitted = 0;
		C.Store.ForEachRecord([&](const auto& V) { if(V.MarkerInfo.Kind==EVaRestInspectorMarkerKind::DerivedQueueExpired && V.MarkerInfo.SourceSeq==778) {
            ++Omitted; C.Test.TestTrue(TEXT("exact accepted scalar Note omission propagated BEFORE seal"),V.bMetadataOmitted && V.MarkerInfo.QueueId.IsEmpty() && V.MarkerInfo.bHasSourceObservation && V.MarkerInfo.SourceTime==77.25);
        } });
		C.Test.TestEqual(TEXT("codec-preserved scalar Note really derives"), Omitted, 1);
		if (!Deliver(C, Completed(1, 0, EVaRestTraceOutcome::Queued)))
			return false;
		const int32 Before = Markers(C.Store, EVaRestInspectorMarkerKind::DerivedQueueExpired);
		if (!Deliver(C, Note(1, TEXT("late-expiry"), 779), EPush::Accepted, false))
			return false;
		C.Test.TestEqual(TEXT("real expiry after Queued terminal stays rejected, no derived marker"), Markers(C.Store, EVaRestInspectorMarkerKind::DerivedQueueExpired), Before);
		C.Store.Clear();
		int32 Owned = 0;
		Retained->ForEachRecord([&](const auto& V) { if(V.MarkerInfo.Kind==EVaRestInspectorMarkerKind::DerivedQueueExpired) {
            ++Owned; C.Test.TestTrue(TEXT("old marker independently owns QueueId across Clear/source Note teardown"),V.MarkerInfo.QueueId==TEXT("expiry-guid") && V.MarkerInfo.SourceSeq==777 && V.bSealed);
        } });
		C.Test.TestEqual(TEXT("old retained marker immutable"), Owned, 1);
		return true;
	});

	All &= WithStore(*this, TEXT("Markers: source-preserving count refusal and invalid local receipt never undo accepted Note"), [](FCase& C) {
        if(!Deliver(C,Started(1))) return false;
        const uint64 Before=C.Store.Diagnostics().OmittedMarkers;
        if(!Deliver(C,Note(1,TEXT("protected-note")))) return false;
        C.Test.TestEqual(TEXT("independent marker count admission refuses"),C.Store.Diagnostics().OmittedMarkers,Before+1);
        C.Test.TestEqual(TEXT("no side marker bypasses shared row count"),Markers(C.Store,EVaRestInspectorMarkerKind::DerivedQueueExpired),0);
        Row(C,1,[&](const auto& V) { C.Test.TestTrue(TEXT("accepted source Note survives marker-only refusal"),V.Notes.Num()==1 && V.Notes[0].Read(V.Notes[0].Fixed->Text[int32(EText::QueueId)])==TEXT("protected-note") && V.bPinned); });
        C.Clock.Set(std::numeric_limits<double>::quiet_NaN());
        if(!Deliver(C,Note(1,TEXT("bad-receipt"),778))) return false;
        C.Test.TestEqual(TEXT("nonfinite derived receipt visibly refuses"),C.Store.Diagnostics().OmittedMarkers,Before+2);
        Row(C,1,[&](const auto& V) { C.Test.TestEqual(TEXT("both actual Notes remain after optional refusal"),V.Notes.Num(),2); });
        return true; }, 1);

	All &= WithStore(*this, TEXT("Markers: nonfinite derived receipt refuses with real count and byte slack"), [](FCase& C) {
        if(!Deliver(C,Started(1))) return false;
        C.Clock.Set(std::numeric_limits<double>::quiet_NaN());
        if(!Deliver(C,Note(1,TEXT("nonfinite-source-note"),800))) return false;
        C.Test.TestTrue(TEXT("derived refusal has real independent row and byte slack"),FStoreAccess::Plan(C.Store).MaxRows>1 && C.Roots.Budget->UsedBytes.load()<C.Roots.Budget->Limit/2);
        C.Test.TestEqual(TEXT("nonfinite optional derived marker is visibly omitted"),C.Store.Diagnostics().OmittedMarkers,uint64(1));
        C.Test.TestEqual(TEXT("nonfinite value never published as local receipt"),Markers(C.Store,EVaRestInspectorMarkerKind::DerivedQueueExpired),0);
        Row(C,1,[&](const auto& V) { C.Test.TestTrue(TEXT("actual source Note retained despite nonfinite optional receipt"),V.Notes.Num()==1 && V.Notes[0].Fixed->Header.Seq==800); });
        C.Clock.Set(321);
        if(!Deliver(C,Note(1,TEXT("finite-source-note"),801))) return false;
        C.Test.TestEqual(TEXT("separate accepted Note with finite receipt can derive normally"),Markers(C.Store,EVaRestInspectorMarkerKind::DerivedQueueExpired),1);
        return true; }, 4);

	All &= WithStore(*this, TEXT("Markers: complete row/controller/text admission one byte short preserves the same retained Note"), [](FCase& C) {
        auto Initial=Note(1,TEXT("initial-note"),700);
        Initial.Payload.Get<FVaRestTraceNote>().Kind=EVaRestTraceNoteKind::Queued;
        if(!Deliver(C,Started(1)) || !Deliver(C,Initial)) return false;
        const auto Expiry=Note(1,TEXT("independent-marker-text"),701);
        if(!C.Test.TestTrue(TEXT("real expiry queue admission"),C.Queue.TryPush(Expiry)==EPush::Accepted)) return false;
        auto Packet=C.Queue.TryPopMetadata(); if(!C.Test.TestTrue(TEXT("actual expiry packet"),Packet.IsSet())) return false;
        const auto Wrapper=C.Roots.Lifetime->Allocator.Bound(sizeof(FStorePacketOwner),alignof(FStorePacketOwner));
        const auto Text=C.Roots.Lifetime->Allocator.Bound((SIZE_T(Expiry.Payload.Get<FVaRestTraceNote>().QueueId.Len())+1)*sizeof(TCHAR),alignof(TCHAR));
        if(!C.Test.TestTrue(TEXT("independent concrete Note wrapper/text requests supported"),Wrapper.IsSet() && Text.IsSet())) return false;
        const int64 PacketCharge=Packet->RetainedCapacity()+int64(Wrapper.GetValue());
        const int64 MarkerCharge=FStoreAccess::RowBound(C.Store)+4096+int64(Text.GetValue());
        const int64 Before=C.Roots.Budget->UsedBytes.load();
        const int64 PressureBytes=C.Roots.Budget->Limit-Before-PacketCharge-(MarkerCharge-1);
        auto Pressure=FStoreCredit::Acquire(C.Roots.Budget,PressureBytes);
        if(!C.Test.TestTrue(TEXT("actual byte pressure leaves source plus marker-minus-one"),PressureBytes>0 && Pressure.IsSet())) return false;
        const int64 Copies=C.Roots.Lifetime->Probe.TextBytesCopied.load();
        const uint64 Omitted=C.Store.Diagnostics().OmittedMarkers;
        if(!C.Test.TestTrue(TEXT("source Note really accepted under pressure"),C.Store.Apply(MoveTemp(Packet.GetValue())))) return false;
        C.Test.TestEqual(TEXT("complete marker charge refuses by one byte"),C.Store.Diagnostics().OmittedMarkers,Omitted+1);
        C.Test.TestEqual(TEXT("failed combined admission copies no marker string"),C.Roots.Lifetime->Probe.TextBytesCopied.load(),Copies);
        C.Test.TestEqual(TEXT("only real source packet/wrapper charge added"),C.Roots.Budget->UsedBytes.load(),Before+PressureBytes+PacketCharge);
        Row(C,1,[&](const auto& V) { C.Test.TestTrue(TEXT("same accepted Note survives independent marker byte refusal"),V.Notes.Num()==2 && V.Notes[1].Fixed->Header.Seq==701 && V.Notes[1].Read(V.Notes[1].Fixed->Text[int32(EText::QueueId)])==TEXT("independent-marker-text")); });
        Pressure.Reset();
        return true; }, 4);

	All &= WithStore(*this, TEXT("Overflow: real count drops, independent fixed counters, one immutable first sample per observed episode"), [](FCase& C) {
		if (!Deliver(C, Started(1)))
			return false;
		for (int32 I = 0; I < 32; ++I)
			if (!C.Test.TestTrue(TEXT("actual count slack fills with owned queued packets"), C.Queue.TryPush(Started(1000 + I)) == EPush::Accepted))
				return false;
		const auto Dropped = Started(9000);
		FPacketLayout Layout;
		if (!C.Test.TestTrue(TEXT("independent dropped layout measured"), FPacketCodec::Measure(Dropped, C.Policy, C.Roots.Lifetime->Allocator, Layout) == EMeasure::Full))
			return false;
		const uint64 BeforeEpoch = C.Queue.LossEpoch();
		if (!C.Test.TestTrue(TEXT("real full queue count causes drop"), C.Queue.TryPush(Dropped) == EPush::Dropped))
			return false;
		C.Test.TestEqual(TEXT("actual upstream dropped count"), C.Roots.Pending->DroppedCount.load(), uint64(1));
		C.Test.TestEqual(TEXT("actual upstream dropped diagnostic charge"), C.Roots.Pending->DroppedBytes.load(), uint64(Layout.PendingCharge));
		C.Test.TestTrue(TEXT("actual loss witness advances"), C.Queue.LossEpoch() > BeforeEpoch);
		C.Store.ObserveQueueLoss(999, *C.Roots.Pending);
		FPendingAccount Other(32, C.Policy.Limits.PendingBytes);
		Other.Lifetime = C.Roots.Lifetime;
		C.Store.ObserveQueueLoss(1, Other);
		C.Store.ForEachRecord([&](const auto&) { C.Store.ObserveQueueLoss(1, *C.Roots.Pending); });
		C.Test.TestEqual(TEXT("wrong generation/witness/borrowed visitor cannot sample"), C.Store.Diagnostics().DroppedEvents, uint64(0));
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending);
		C.Test.TestEqual(TEXT("fixed true drop count sampled"), C.Store.Diagnostics().DroppedEvents, uint64(1));
		C.Test.TestEqual(TEXT("fixed true loss charge sampled"), C.Store.Diagnostics().DroppedBytes, uint64(Layout.PendingCharge));
		C.Test.TestEqual(TEXT("first actual episode summary"), Markers(C.Store, EVaRestInspectorMarkerKind::CaptureOverflow), 1);
		auto Old = Take(C);
		if (!Old)
			return false;
		C.Test.TestEqual(TEXT("snapshot fixed diagnostics copied"), Old->GetDiagnostics().DroppedEvents, uint64(1));
		// An invalid no-delta poll must not close this episode.
		C.Store.ObserveQueueLoss(999, *C.Roots.Pending);
		if (!C.Test.TestTrue(TEXT("second actual count drop"), C.Queue.TryPush(Dropped) == EPush::Dropped))
			return false;
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending);
		C.Test.TestEqual(TEXT("second sample updates fixed diagnostics"), C.Store.Diagnostics().DroppedEvents, uint64(2));
		C.Test.TestEqual(TEXT("contiguous lossy polls share ONE marker attempt"), Markers(C.Store, EVaRestInspectorMarkerKind::CaptureOverflow), 1);
		int32 First = 0;
		C.Store.ForEachRecord([&](const auto& V) { if(V.MarkerInfo.Kind==EVaRestInspectorMarkerKind::CaptureOverflow) {
            ++First; C.Test.TestTrue(TEXT("sealed first summary never becomes episode final totals"),V.bSealed && V.MarkerInfo.DroppedEvents==1 && V.MarkerInfo.DroppedBytes==uint64(Layout.PendingCharge));
            C.Test.TestTrue(TEXT("internal overflow source/cause/context explicitly unknown"),!V.MarkerInfo.bHasSourceObservation && !V.bContextKnown && V.MarkerInfo.SourceSeq==0 && V.MarkerInfo.CauseRequestId==0 && V.ScopeLabel.IsEmpty() && !V.Marker.Fixed && V.MarkerInfo.ObservedAt==123.5);
        } });
		C.Test.TestEqual(TEXT("one first-sample row observed"), First, 1);
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending); // Valid no-delta rearm.
		if (!C.Test.TestTrue(TEXT("new observed episode has a real drop"), C.Queue.TryPush(Dropped) == EPush::Dropped))
			return false;
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending);
		C.Test.TestEqual(TEXT("new observed episode admits second summary"), Markers(C.Store, EVaRestInspectorMarkerKind::CaptureOverflow), 2);
		C.Test.TestEqual(TEXT("all actual drops remain in fixed diagnostics"), C.Store.Diagnostics().DroppedEvents, uint64(3));
		C.Store.Clear();
		C.Test.TestEqual(TEXT("Clear resets displayed loss totals"), C.Store.Diagnostics().DroppedEvents, uint64(0));
		C.Test.TestEqual(TEXT("Clear never resets actual source totals"), C.Roots.Pending->DroppedCount.load(), uint64(3));
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending);
		C.Test.TestEqual(TEXT("Clear rebaseline does not report old losses again"), Markers(C.Store, EVaRestInspectorMarkerKind::CaptureOverflow), 0);
		int32 Retained = 0;
		Old->ForEachRecord([&](const auto& V) { if(V.MarkerInfo.Kind==EVaRestInspectorMarkerKind::CaptureOverflow) { ++Retained; C.Test.TestEqual(TEXT("old sealed sample survives Clear"),V.MarkerInfo.DroppedEvents,uint64(1)); } });
		C.Test.TestEqual(TEXT("old overflow owner kept its own value"), Retained, 1);
		C.Queue.Close();
		while (C.Queue.TryPopMetadata().IsSet())
		{
		}
		while (C.Queue.TryPopExcerpt().IsSet())
		{
		}
		// All owned synchronous producer calls above have returned before this
		// generation baseline. This is not Task11's production lifecycle proof.
		C.Store.BeginGeneration(2, 0);
		FCaptureQueue Next(C.Policy, 2, C.Roots.Pending.ToSharedRef(), C.Roots.Lifetime->Allocator);
		ON_SCOPE_EXIT
		{
			Next.Close();
			while (Next.TryPopMetadata().IsSet())
			{
			}
			while (Next.TryPopExcerpt().IsSet())
			{
			}
		};
		for (int32 I = 0; I < 32; ++I)
			if (!C.Test.TestTrue(TEXT("new real queue fills independently"), Next.TryPush(Started(2000 + I)) == EPush::Accepted))
				return false;
		if (!C.Test.TestTrue(TEXT("new generation actual loss"), Next.TryPush(Dropped) == EPush::Dropped))
			return false;
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending);
		C.Test.TestEqual(TEXT("old generation cannot sample new account totals"), C.Store.Diagnostics().DroppedEvents, uint64(0));
		C.Store.ObserveQueueLoss(2, *C.Roots.Pending);
		C.Test.TestEqual(TEXT("new generation counts only post-baseline delta"), C.Store.Diagnostics().DroppedEvents, uint64(1));
		C.Test.TestFalse(TEXT("finite actual observations are not falsely saturated"), C.Store.Diagnostics().bLossCountersSaturated);
		return true;
	});

	All &= WithStore(*this, TEXT("Overflow: real byte fallback independently reports skipped excerpt and metadata, epoch is not a drop count"), [](FCase& C) {
        FVaRestTraceExcerptEvent V; V.RequestId=50; V.Owner=EVaRestTraceExcerptOwner::Request;
        V.Excerpt.bRedacted=true; V.Excerpt.TotalBytes=65536;
        auto Bytes=MakeShared<TArray<uint8>,ESPMode::ThreadSafe>(); Bytes->Init(uint8('x'),65536); V.Excerpt.Bytes=Bytes;
        FVaRestTraceEvent E; E.Kind=EVaRestTraceKind::Excerpt; E.Seq=500; E.Time=5;
        E.Payload=FVaRestTracePayload(TInPlaceType<FVaRestTraceExcerptEvent>(),MoveTemp(V));
        FPacketLayout Full;
        if(!C.Test.TestTrue(TEXT("real full excerpt layout exceeds independent pending cap with count slack"),FPacketCodec::Measure(E,C.Policy,C.Roots.Lifetime->Allocator,Full)==EMeasure::Full && Full.PendingCharge>C.Roots.Pending->ByteLimit && C.Roots.Pending->Count.load()<C.Roots.Pending->CountLimit)) return false;
        const auto PushExcerpt=[&]() {
            if(!C.Test.TestTrue(TEXT("real byte limit admits descriptor-only fallback"),C.Queue.TryPush(E)==EPush::ScalarOnly)) return false;
            auto P=C.Queue.TryPopExcerpt(); return C.Test.TestTrue(TEXT("actual fallback packet retained without excerpt bytes"),P.IsSet() && !P->Excerpt());
        };
        if(!PushExcerpt()) return false;
        C.Test.TestEqual(TEXT("actual scalar excerpt is NOT a dropped event"),C.Roots.Pending->DroppedCount.load(),uint64(0));
        C.Test.TestEqual(TEXT("actual excerpt skip counter increments"),C.Roots.Pending->ExcerptsSkipped.load(),uint64(1));
        C.Test.TestTrue(TEXT("epoch advanced despite zero drops"),C.Queue.LossEpoch()>0);
        C.Store.ObserveQueueLoss(1,*C.Roots.Pending);
        C.Test.TestEqual(TEXT("fixed diagnostics do not use epoch as dropped count"),C.Store.Diagnostics().DroppedEvents,uint64(0));
        C.Test.TestEqual(TEXT("fixed skipped excerpts survive without packet owner"),C.Store.Diagnostics().ExcerptsSkipped,uint64(1));
        C.Test.TestEqual(TEXT("actual declared loss bytes, not source Num or RSS"),C.Store.Diagnostics().DroppedBytes,uint64(Full.PendingCharge));
        const FString Huge=FString::ChrN(FNormalizedLimits::MaxStringChars+1,TCHAR('n'));
        const auto Metadata=Note(51,*Huge,501);
        if(!C.Test.TestTrue(TEXT("real overlong Note uses scalar codec"),C.Queue.TryPush(Metadata)==EPush::ScalarOnly)) return false;
        { auto P=C.Queue.TryPopMetadata(); if(!C.Test.TestTrue(TEXT("actual metadata-omitted Note packet"),P.IsSet() && P->Fixed().Header.bMetadataOmitted)) return false; }
        C.Store.ObserveQueueLoss(1,*C.Roots.Pending);
        C.Test.TestEqual(TEXT("independent metadata counter retained in fixed aggregate"),C.Store.Diagnostics().MetadataOmitted,uint64(1));
        C.Test.TestEqual(TEXT("same observed episode keeps one first-sample marker"),Markers(C.Store,EVaRestInspectorMarkerKind::CaptureOverflow),1);
        C.Store.ObserveQueueLoss(1,*C.Roots.Pending);
        C.Clock.Set(std::numeric_limits<double>::infinity());
        if(!PushExcerpt()) return false;
        C.Store.ObserveQueueLoss(1,*C.Roots.Pending);
        const uint64 Refused=C.Store.Diagnostics().OmittedMarkers;
        C.Test.TestEqual(TEXT("nonfinite first local receipt refuses optional marker"),Refused,uint64(1));
        C.Test.TestEqual(TEXT("refused marker never hides real new skip"),C.Store.Diagnostics().ExcerptsSkipped,uint64(2));
        C.Clock.Set(200);
        if(!PushExcerpt()) return false;
        C.Store.ObserveQueueLoss(1,*C.Roots.Pending);
        C.Test.TestEqual(TEXT("refused episode does not retry on later lossy poll"),C.Store.Diagnostics().OmittedMarkers,Refused);
        C.Test.TestEqual(TEXT("no invented receipt or marker from failed episode"),Markers(C.Store,EVaRestInspectorMarkerKind::CaptureOverflow),1);
        C.Store.ObserveQueueLoss(1,*C.Roots.Pending);
        if(!PushExcerpt()) return false;
        C.Store.ObserveQueueLoss(1,*C.Roots.Pending);
        C.Test.TestEqual(TEXT("valid no-delta rearm permits a genuinely new episode"),Markers(C.Store,EVaRestInspectorMarkerKind::CaptureOverflow),2);
        C.Test.TestEqual(TEXT("every actual skipped excerpt accounted independently"),C.Store.Diagnostics().ExcerptsSkipped,uint64(4));
        return true; }, 16, 64ll * 1024 * 1024, 32768);

	All &= WithStore(*this, TEXT("Overflow: full pinned store refuses summary once per episode but all fixed loss metrics survive"), [](FCase& C) {
        if(!Deliver(C,Started(1))) return false;
        for(int32 I=0;I<32;++I) if(!C.Test.TestTrue(TEXT("real count pressure"),C.Queue.TryPush(Started(5000+I))==EPush::Accepted)) return false;
        const auto Drop=[&]() { return C.Test.TestTrue(TEXT("actual producer drop under count cap"),C.Queue.TryPush(Started(9999))==EPush::Dropped); };
        if(!Drop()) return false; C.Store.ObserveQueueLoss(1,*C.Roots.Pending);
        C.Test.TestEqual(TEXT("real shared marker cap refuses first summary"),C.Store.Diagnostics().OmittedMarkers,uint64(1));
        if(!Drop()) return false; C.Store.ObserveQueueLoss(1,*C.Roots.Pending);
        C.Test.TestEqual(TEXT("second lossy poll does not retry refusal"),C.Store.Diagnostics().OmittedMarkers,uint64(1));
        C.Test.TestEqual(TEXT("both real drops visible without any marker owner"),C.Store.Diagnostics().DroppedEvents,uint64(2));
        C.Store.ObserveQueueLoss(1,*C.Roots.Pending);
        if(!Drop()) return false; C.Store.ObserveQueueLoss(1,*C.Roots.Pending);
        C.Test.TestEqual(TEXT("new episode makes exactly one new refusal"),C.Store.Diagnostics().OmittedMarkers,uint64(2));
        C.Test.TestEqual(TEXT("fixed aggregate retains all three drops"),C.Store.Diagnostics().DroppedEvents,uint64(3));
        C.Test.TestEqual(TEXT("no unbounded side summary list"),Markers(C.Store,EVaRestInspectorMarkerKind::CaptureOverflow),0);
        Row(C,1,[&](const auto& V) { C.Test.TestTrue(TEXT("pinned HTTP owner survives summary pressure"),V.bPinned && V.Started.Fixed); });
        return true; }, 1);
	All &= WithStore(*this, TEXT("SCALAR boundary: real diagnostic atomics saturate, rebaseline and reject an injected decrease; no producer-count/E2E claim"), [](FCase& C) {
		// Finite arithmetic controls on the REAL diagnostic atomics. These are
		// not 2^64 producer events, loss episodes or a production lifecycle proof.
		if (!Deliver(C, Started(1))) return false;
		C.Clock.Set(std::numeric_limits<double>::quiet_NaN());
		AddSaturated(C.Roots.Pending->MetadataOmitted, MAX_uint64 - 3);
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending); // One real optional receipt refusal.
		if (!Deliver(C, Note(1, TEXT("scalar-omission-one"), 900)) ||
			!Deliver(C, Note(1, TEXT("scalar-omission-two"), 901))) return false;
		C.Test.TestEqual(TEXT("three actual optional refusals fill aggregate metadata exactly"), C.Store.Diagnostics().MetadataOmitted, MAX_uint64);
		C.Test.TestEqual(TEXT("source scalar stays below MAX independently"), C.Roots.Pending->MetadataOmitted.load(), MAX_uint64 - 3);
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending); // No-delta episode close.
		C.Clock.Set(456);
		AddSaturated(C.Roots.Pending->MetadataOmitted);
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending);
		C.Test.TestEqual(TEXT("source still below MAX at actual aggregate overflow"), C.Roots.Pending->MetadataOmitted.load(), MAX_uint64 - 2);
		C.Test.TestTrue(TEXT("aggregate overflow sets fixed saturation flag"), C.Store.Diagnostics().bLossCountersSaturated);
		int32 Aggregate = 0;
		C.Store.ForEachRecord([&](const auto& V) {
			if (V.MarkerInfo.Kind == EVaRestInspectorMarkerKind::CaptureOverflow)
			{
				++Aggregate;
				C.Test.TestTrue(TEXT("first sample flag reflects AFTER aggregate addition"), V.MarkerInfo.bLossSaturated == C.Store.Diagnostics().bLossCountersSaturated && V.MarkerInfo.bLossSaturated);
				C.Test.TestEqual(TEXT("summary retains exact new independently sampled metadata delta"), V.MarkerInfo.MetadataOmitted, uint64(1));
			}
		});
		C.Test.TestEqual(TEXT("one finite aggregate-boundary summary"), Aggregate, 1);
		C.Store.Clear();
		AddSaturated(C.Roots.Pending->DroppedCount, MAX_uint64);
		AddSaturated(C.Roots.Pending->DroppedBytes, MAX_uint64);
		AddSaturated(C.Roots.Pending->MetadataOmitted, 7);
		AddSaturated(C.Roots.Pending->ExcerptsSkipped, MAX_uint64);
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending);
		C.Test.TestEqual(TEXT("actual saturating addition reaches MAX without wrap"), C.Roots.Pending->MetadataOmitted.load(), MAX_uint64);
		C.Test.TestTrue(TEXT("source MAX makes loss saturation explicit"), C.Store.Diagnostics().bLossCountersSaturated);
		C.Store.Clear();
		C.Test.TestEqual(TEXT("Clear preserves actual source MAX"), C.Roots.Pending->DroppedCount.load(), MAX_uint64);
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending);
		C.Test.TestTrue(TEXT("source MAX visible even without positive delta after rebaseline"), C.Store.Diagnostics().bLossCountersSaturated);
		C.Test.TestEqual(TEXT("rebaseline invents no huge new count"), C.Store.Diagnostics().DroppedEvents, uint64(0));
		C.Test.TestEqual(TEXT("no-delta MAX creates no marker"), Markers(C.Store, EVaRestInspectorMarkerKind::CaptureOverflow), 0);
		AddSaturated(C.Roots.Pending->DroppedCount, 1);
		C.Test.TestEqual(TEXT("AddSaturated at MAX never wraps"), C.Roots.Pending->DroppedCount.load(), MAX_uint64);
		// Explicit SCALAR contract-fault injection, not a producer/reset path.
		C.Roots.Pending->DroppedCount.exchange(MAX_uint64 - 7, std::memory_order_acq_rel);
		const uint64 Violations = C.Store.Diagnostics().ContractViolations;
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending);
		C.Test.TestEqual(TEXT("decreasing same-lifetime total is a contract violation"), C.Store.Diagnostics().ContractViolations, Violations + 1);
		C.Test.TestEqual(TEXT("decrease never unsigned-wraps into loss count"), C.Store.Diagnostics().DroppedEvents, uint64(0));
		C.Test.TestEqual(TEXT("decrease creates no invented summary"), Markers(C.Store, EVaRestInspectorMarkerKind::CaptureOverflow), 0);
		AddSaturated(C.Roots.Pending->DroppedCount, 2);
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending);
		C.Test.TestEqual(TEXT("conservative baseline resumes with exact next scalar delta"), C.Store.Diagnostics().DroppedEvents, uint64(2));
		return true; }, 4);
	All &= WithStore(*this, TEXT("SCALAR boundary: genuine Store-only omission at aggregateMAX sets saturation before any new source delta"), [](FCase& C) {
		// Independent of the previous queue-addition boundary case. Finite
		// AddSaturated arithmetic seeds the REAL diagnostic atomics; the final
		// overflow is a genuine accepted Note's optional marker/clock refusal.
		// No 2^64 producer-event or production-lifecycle claim is made.
		if (!Deliver(C, Started(1))) return false;
		C.Clock.Set(std::numeric_limits<double>::quiet_NaN());
		AddSaturated(C.Roots.Pending->MetadataOmitted, MAX_uint64 - 3);
		C.Store.ObserveQueueLoss(1, *C.Roots.Pending);
		if (!Deliver(C, Note(1, TEXT("store-only-omission-one"), 1000)) ||
			!Deliver(C, Note(1, TEXT("store-only-omission-two"), 1001))) return false;
		C.Test.TestEqual(TEXT("mixed aggregate reaches MAX exactly without arithmetic loss"), C.Store.Diagnostics().MetadataOmitted, MAX_uint64);
		C.Test.TestFalse(TEXT("independent Store-only precondition: no overflow yet"), C.Store.Diagnostics().bLossCountersSaturated);
		const uint64 SourceMetadata = C.Roots.Pending->MetadataOmitted.load();
		const uint64 SourceEpoch = C.Roots.Pending->LossEpoch.load();
		const uint64 Omitted = C.Store.Diagnostics().OmittedMarkers;
		C.Test.TestEqual(TEXT("source diagnostic remains below MAX before Store-only overflow"), SourceMetadata, MAX_uint64 - 3);
		if (!Deliver(C, Note(1, TEXT("store-only-omission-three"), 1002))) return false;
		// No ObserveQueueLoss or new source delta occurs between the exact-MAX
		// precondition and these assertions. Store's own omission must set it.
		C.Test.TestEqual(TEXT("Store-only overflow keeps aggregate capped at MAX"), C.Store.Diagnostics().MetadataOmitted, MAX_uint64);
		C.Test.TestTrue(TEXT("genuine Store-only lost increment sets saturation immediately"), C.Store.Diagnostics().bLossCountersSaturated);
		C.Test.TestEqual(TEXT("third real optional marker refusal happened"), C.Store.Diagnostics().OmittedMarkers, Omitted + 1);
		C.Test.TestEqual(TEXT("source total unchanged across Store-only transition"), C.Roots.Pending->MetadataOmitted.load(), SourceMetadata);
		C.Test.TestEqual(TEXT("no producer loss epoch was fabricated"), C.Roots.Pending->LossEpoch.load(), SourceEpoch);
		C.Test.TestEqual(TEXT("refused markers create no hidden rows"), Markers(C.Store, EVaRestInspectorMarkerKind::DerivedQueueExpired), 0);
		Row(C, 1, [&](const auto& V) {
			C.Test.TestEqual(TEXT("all three genuine source Notes stay retained"), V.Notes.Num(), 3);
			if (V.Notes.Num() == 3)
				C.Test.TestEqual(TEXT("actual final retained Note proves this refusal source"), V.Notes[2].Fixed->Header.Seq, uint64(1002));
		});
		return true;
	}, 4);
	return All;
}
#endif
