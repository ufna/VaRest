// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspector.h"

#if WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR

#include "Async/Async.h"
#include "Containers/Array.h"
#include "Containers/StringView.h"
#include "Containers/UnrealString.h"
#include "HAL/UnrealMemory.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeExit.h"
#include "Templates/Function.h"
#include "Templates/RefCounting.h"
#include "Templates/SharedPointer.h"
#include "Templates/SharedPointerInternals.h"
#include "VaRestClock.h"
#include "VaRestInspectorAllocation.h"
#include "VaRestInspectorBudget.h"
#include "VaRestInspectorPacket.h"
#include "VaRestInspectorPolicy.h"
#include "VaRestInspectorQueue.h"
#include "VaRestInspectorRecords.h"
#include "VaRestInspectorStore.h"
#include "VaRestTrace.h"

#include <atomic>
#include <initializer_list>
#include <limits>

namespace VaRest::Inspector::Private::Testing
{
// Existing read-only current-profile check; never sets support or calibrates.
bool ReadLiveProfile(FAutomationTestBase& Test);
}

namespace VaRest::Inspector::Private::SnapshotTesting
{
using FSnapshot = TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>;
using FWeakSnapshot = TWeakPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>;
using EDetail = EVaRestInspectorDetailState;

// Proposed narrow friend in Store.h. These call genuine bounded operations;
// there is no packet-credit access, fake eviction, setter or allocator hook.
struct FStoreAccess
{
	static int32 Expire(FStore& Store) { return Store.ExpirePendingExcerpts(); }
	static void Stop(FStore& Store) { Store.StopPendingExcerpts(); }
	// True during enumeration means accepted for deferred eligibility recheck.
	static bool Remove(FStore& Store, uint64 Id) { return Store.RemoveEligibleRow(Id); }
	static int32 IndexCapacity(const FStore& Store) { return Store.IndexCapacity(); }
	static int32 RetainedIdCapacity(const FStore& Store) { return Store.RetainedIdCapacity(); }
	static uint64 Generation(const FStore& Store) { return Store.Generation; }
	static uint64 Ordinal(const FStore& Store) { return Store.LastConsumedMetadataOrdinal; }
	static uint64 OrphanEpoch(const FStore& Store) { return Store.OrphanEpoch; }
};

// Upstream clock fixture, including invalid-receipt controls. No real timer is
// used by these tests; timer methods retain the actual IVaRestClock contract.
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
	explicit FRoots(FAutomationTestBase& InTest) : Test(InTest) {}
	~FRoots()
	{
		if (Pending)
		{
			Test.TestEqual(TEXT("snapshot fixture pending count drained"), Pending->Count.load(), int64(0));
			Test.TestEqual(TEXT("snapshot fixture pending bytes drained"), Pending->Bytes.load(), int64(0));
			Test.TestEqual(TEXT("snapshot fixture excerpt reservations drained"), Pending->ExcerptReservations.load(), int64(0));
			Pending.Reset();
			StartupUsed.fetch_sub(4096);
		}
		if (Budget)
		{
			Test.TestEqual(TEXT("snapshot fixture only bootstrap remains"), Budget->UsedBytes.load(), Budget->BootstrapBytes);
			Test.TestEqual(TEXT("snapshot fixture controllers destroyed"), Budget->Probe.LiveControllers.load(), int64(0));
			Test.TestEqual(TEXT("snapshot fixture final weak slots released"), Budget->SnapshotControllers.load(), int64(0));
			Budget.SafeRelease();
			StartupUsed.fetch_sub(4096);
		}
		if (Lifetime)
		{
			Test.TestEqual(TEXT("snapshot fixture all actual backing freed"), Lifetime->Probe.LiveBacking.load(), int64(0));
			Lifetime.SafeRelease();
			StartupUsed.fetch_sub(4096);
		}
		Test.TestEqual(TEXT("snapshot fixture allowances released after actual root frees"), StartupUsed.load(), int64(0));
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
		Test.AddInfo(FString::Printf(TEXT("SnapshotRowCalibration Request=%lld Alignment=%lld Capacity=%lld StartupLimit=%lld"),
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
		Test.AddInfo(FString::Printf(TEXT("SnapshotFixtureRoot Request=%lld Alignment=%lld Capacity=%lld StartupLimit=%lld"),
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
static bool WithStore(FAutomationTestBase& Test, const TCHAR* Name, F&& Body, int32 MaxRows = 64)
{
	Test.AddInfo(Name);
	FCapturePolicy Policy;
	Policy.Limits.MaxRecords = MaxRows;
	Policy.Limits.PendingEvents = 32;
	Policy.Limits.PendingBytes = 4 * 1024 * 1024;
	FRoots Roots(Test);
	if (!Roots.Initialize(Policy.Limits.StoreBytes, Policy.Limits.PendingBytes, Policy.Limits.PendingEvents))
		return false;
	auto Clock = MakeShared<FReceiptClock, ESPMode::ThreadSafe>();
	bool bResult = false;
	{
		FStore Store(Policy.Limits, Clock);
		if (!Test.TestTrue(TEXT("snapshot case store initialized"), Store.Initialize(Roots.Budget)))
			return false;
		Store.BeginGeneration(1, 0);
		const int64 FixedBytes = Roots.Budget->UsedBytes.load();
		const int64 FixedBacking = Roots.Lifetime->Probe.LiveBacking.load();
		{
			FCaptureQueue Queue(Policy, 1, Roots.Pending.ToSharedRef(), Roots.Lifetime->Allocator);
			if (!Test.TestTrue(TEXT("snapshot case real queue supported"), Queue.IsSupported()))
				return false;
			ON_SCOPE_EXIT
			{
				Queue.Close();
				while (Queue.TryPopMetadata().IsSet()) {}
				while (Queue.TryPopExcerpt().IsSet()) {}
				Test.TestEqual(TEXT("no publisher/excerpt reservation survives case"), Queue.OutstandingExcerptReservations(), int64(0));
			};
			FCase C{Test, Roots, Store, Queue, *Clock, Policy};
			bResult = Body(C);
		}
		Test.TestEqual(TEXT("all queue and pending packet bytes drained"), Roots.Pending->Bytes.load(), int64(0));
		Test.TestEqual(TEXT("all pending packet counts drained"), Roots.Pending->Count.load(), int64(0));
		Store.Clear();
		Test.TestEqual(TEXT("case Clear returns exact fixed credit"), Roots.Budget->UsedBytes.load(), FixedBytes);
		Test.TestEqual(TEXT("case Clear returns exact fixed actual backing"), Roots.Lifetime->Probe.LiveBacking.load(), FixedBacking);
		Test.TestEqual(TEXT("case has no surviving controller"), Roots.Budget->Probe.LiveControllers.load(), int64(0));
		Test.TestEqual(TEXT("case has no surviving weak slot"), Roots.Budget->SnapshotControllers.load(), int64(0));
		Test.TestTrue(TEXT("store peak bounded"), Roots.Budget->PeakBytes.load() <= Roots.Budget->Limit);
		Test.TestTrue(TEXT("pending peak bounded"), Roots.Pending->PeakBytes.load() <= Roots.Pending->ByteLimit);
	}
	Test.TestEqual(TEXT("store destruction releases fixed backing"), Roots.Lifetime->Probe.LiveBacking.load(), int64(0));
	Test.TestEqual(TEXT("store destruction leaves admitted bootstrap"), Roots.Budget->UsedBytes.load(), Roots.Budget->BootstrapBytes);
	return bResult;
}

static FVaRestTraceEvent Started(uint64 Id, bool bPending = false)
{
	FVaRestTraceStarted V;
	V.RequestId = Id; V.ClientId = 9; V.ScopeId = 0;
	V.Verb = TEXT("POST"); V.Url = TEXT("https://example.test/snapshot?token=***");
	V.PathTemplate = TEXT("/snapshot/{id}"); V.Endpoint = TEXT("SnapshotEndpoint"); V.Tag = TEXT("SnapshotTag");
	V.Headers.Set(TEXT("X-Start"), TEXT("start-value"));
	V.Initiator.OwnerPath = TEXT("/Test/Owner"); V.Initiator.ScriptObject = TEXT("Object");
	V.Initiator.ScriptFunction = TEXT("Function"); V.Initiator.ScriptCodeOffset = 7;
	V.Initiator.SourceFile = TEXT("fixture.cpp"); V.Initiator.SourceLine = 31;
	V.StartedAt = 999990; V.StartUtc = FDateTime(2026, 10, 24); V.CurrentAttempt = 1;
	V.bExcerptPending = bPending; V.Body.Bytes = 31; V.Body.ContentType = TEXT("text/plain");
	FVaRestTraceEvent E; E.Kind = EVaRestTraceKind::Started; E.Seq = 1; E.Time = 999990;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Dispatch(uint64 Id, uint64 Physical, int32 Attempt = 1)
{
	FVaRestTraceAttemptDispatched V;
	V.RequestId = Id; V.DispatchId = Physical; V.Attempt = Attempt;
	V.AttemptStart = 999991; V.AuthWait = .25; V.LimiterWait = .5;
	V.GapKind = EVaRestTraceGap::Backoff; V.GapSeconds = 1; V.AuthRefreshId = 77;
	V.Headers.Set(TEXT("X-Physical"), TEXT("same-immutable-headers"));
	FVaRestTraceEvent E; E.Kind = EVaRestTraceKind::AttemptDispatched; E.Seq = Physical; E.Time = 999992;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptDispatched>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent End(uint64 Id, uint64 Physical, bool bPending = true)
{
	FVaRestTraceAttemptEnded V;
	V.RequestId = Id; V.DispatchId = Physical; V.Attempt = 1; V.Status = 200;
	V.UploadDoneAt = 999993; V.StatusAt = 999994; V.TransportDoneAt = 999995;
	V.BytesUp = 17; V.BytesDown = 31; V.bExcerptPending = bPending;
	V.Headers.Set(TEXT("X-End"), TEXT("end-value"));
	V.EffectiveUrl = TEXT("https://example.test/physical"); V.ContentType = TEXT("text/plain");
	FVaRestTraceEvent E; E.Kind = EVaRestTraceKind::AttemptEnded; E.Seq = Physical + 100; E.Time = 999995;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptEnded>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Completed(uint64 Id, bool bCache = false)
{
	FVaRestTraceCompleted V;
	V.RequestId = Id; V.Outcome = EVaRestTraceOutcome::Success;
	V.Response.Status = 200; V.Response.BodyBytes = 31; V.Response.Storage = EVaRestTraceResponseStorage::Memory;
	V.Response.EffectiveUrl = TEXT("https://example.test/final"); V.Response.ContentType = TEXT("application/json");
	V.Response.Headers.Set(TEXT("X-Final"), TEXT("final-value"));
	V.ResultType = TEXT("SnapshotResult"); V.DecodeStartAt = 999998; V.DecodeEndAt = 999999;
	V.Envelope.bApplied = true; V.Envelope.DataPath = TEXT("data");
	V.AuthRefreshId = 88; V.bFromCache = bCache;
	V.CacheLookup = bCache ? EVaRestTraceCacheLookup::Revalidate : EVaRestTraceCacheLookup::Miss;
	FVaRestTraceEvent E; E.Kind = EVaRestTraceKind::Completed; E.Seq = 900; E.Time = 1000000;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceCompleted>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Excerpt(uint64 Id, uint64 Physical = 0, uint8 Byte = 'x', EVaRestExcerptSkip Skip = EVaRestExcerptSkip::None)
{
	FVaRestTraceExcerptEvent V;
	V.RequestId = Id; V.DispatchId = Physical; V.Attempt = Physical ? 1 : 0;
	V.Owner = Physical ? EVaRestTraceExcerptOwner::Response : EVaRestTraceExcerptOwner::Request;
	V.Excerpt.bRedacted = true; V.Excerpt.TotalBytes = 31; V.Excerpt.SkipReason = Skip;
	if (Skip == EVaRestExcerptSkip::None)
	{
		auto Bytes = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
		Bytes->Reserve(128); Bytes->Add(Byte); V.Excerpt.Bytes = Bytes;
	}
	else
		V.Excerpt.Kind = EVaRestExcerptKind::Skipped;
	FVaRestTraceEvent E; E.Kind = EVaRestTraceKind::Excerpt; E.Seq = Physical + 1000; E.Time = 1000001;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceExcerptEvent>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Note(uint64 Id, uint64 Seq)
{
	FVaRestTraceNote V;
	V.RequestId = Id; V.Kind = EVaRestTraceNoteKind::Queued; V.Attempt = 0;
	V.QueueId = FString::Printf(TEXT("queue-%llu"), Seq); V.Reason = TEXT("Offline");
	FVaRestTraceEvent E; E.Kind = EVaRestTraceKind::Note; E.Seq = Seq;
	E.Time = Seq % 2 ? 0 : 1000000; // Application order must not be replaced by source-time sorting.
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceNote>(), MoveTemp(V));
	return E;
}
static FVaRestTraceEvent Client(const TCHAR* Label)
{
	FVaRestTraceClient V; V.ClientId = 9; V.ScopeId = 0; V.DisplayName = Label;
	FVaRestTraceEvent E; E.Kind = EVaRestTraceKind::Client;
	E.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceClient>(), MoveTemp(V));
	return E;
}

static bool Deliver(FCase& C, const FVaRestTraceEvent& E, EPush Expected = EPush::Accepted, bool bRequireApplied = true)
{
	const EPush Result = C.Queue.TryPush(E);
	if (!C.Test.TestTrue(TEXT("real queue produces expected admission result"), Result == Expected))
		return false;
	auto Packet = E.Kind == EVaRestTraceKind::Excerpt ? C.Queue.TryPopExcerpt() : C.Queue.TryPopMetadata();
	if (!C.Test.TestTrue(TEXT("real admitted packet dequeued"), Packet.IsSet()))
		return false;
	const bool bApplied = C.Store.Apply(MoveTemp(Packet.GetValue()));
	return !bRequireApplied || C.Test.TestTrue(TEXT("real packet applied"), bApplied);
}

template <typename F>
static bool Visit(FAutomationTestBase& Test, const FStore& Store, uint64 Id, F&& Check)
{
	int32 Count = 0;
	Test.TestTrue(TEXT("store enumeration succeeds"), Store.ForEachRecord([&](const auto& Row) {
		if (Row.RequestId == Id) { ++Count; Check(Row); }
	}));
	return Test.TestEqual(TEXT("exactly one expected live row"), Count, 1);
}
template <typename F>
static bool Visit(FAutomationTestBase& Test, const FSnapshot& Snapshot, uint64 Id, F&& Check)
{
	if (!Test.TestTrue(TEXT("snapshot exists before enumeration"), Snapshot.IsValid()))
		return false;
	int32 Count = 0;
	Snapshot->ForEachRecord([&](const auto& Row) { if (Row.RequestId == Id) { ++Count; Check(Row); } });
	return Test.TestEqual(TEXT("exactly one expected snapshot row"), Count, 1);
}
static int32 CountRows(const FStore& Store)
{
	int32 N = 0; Store.ForEachRecord([&](const auto&) { ++N; }); return N;
}
static int32 CountSealed(const FStore& Store)
{
	int32 N = 0; Store.ForEachRecord([&](const auto& R) { N += R.bSealed ? 1 : 0; }); return N;
}
static void CheckResponse(FAutomationTestBase& Test, const FVaRestInspectorRecordView& Row, uint64 Physical, EDetail State, int32 FirstByte = -1)
{
	int32 Found = 0;
	for (const auto& D : Row.Dispatches)
	{
		if (D.DispatchId != Physical) continue;
		++Found;
		Test.TestTrue(TEXT("physical response availability"), D.ResponseExcerptState == State);
		if (FirstByte >= 0 && Test.TestEqual(TEXT("physical excerpt actual byte count"), D.ResponseExcerpt.Excerpt.Num(), 1))
			Test.TestEqual(TEXT("physical excerpt actual byte"), int32(D.ResponseExcerpt.Excerpt[0]), FirstByte);
	}
	Test.TestEqual(TEXT("one exact physical response slot"), Found, 1);
}
static void CheckRequest(FAutomationTestBase& Test, const FVaRestInspectorRecordView& Row, EDetail State, int32 FirstByte = -1)
{
	Test.TestTrue(TEXT("request excerpt availability"), Row.RequestExcerptState == State);
	if (FirstByte < 0) return;
	int32 Found = 0;
	for (const auto& E : Row.Excerpts)
	{
		if (!E.Fixed || E.Fixed->Scalars.Owner != EVaRestTraceExcerptOwner::Request) continue;
		++Found;
		if (Test.TestEqual(TEXT("request excerpt actual byte count"), E.Excerpt.Num(), 1))
			Test.TestEqual(TEXT("request excerpt actual byte"), int32(E.Excerpt[0]), FirstByte);
	}
	Test.TestEqual(TEXT("one actual request excerpt"), Found, 1);
}

// Semantic, owning test images: no padding comparisons and no borrowed views
// retained. Covers all public row scalars, packet scalars/text/pairs and rings.
struct FSnapshotRecordImage
{
	FString Text;
	template <typename T> void Add(T Value) { Text.Appendf(TEXT("%llu|"), uint64(Value)); }
	void Add(double Value) { Text.Appendf(TEXT("%.17g|"), Value); }
	void Add(FStringView Value)
	{
		Text.Appendf(TEXT("%d:"), Value.Len());
		if (Value.Len()) Text.AppendChars(Value.GetData(), Value.Len());
		Text += TEXT("|");
	}
	void Add(const FString& Value) { Add(FStringView(Value)); }
	void Phase(const FVaRestInspectorPhase& P) { Add(P.bKnown); Add(P.Seconds); }
	void Packet(const FVaRestInspectorPacketView& P)
	{
		Add(P.Fixed != nullptr);
		if (!P.Fixed) return;
		const auto& H = P.Fixed->Header;
		Add(H.Kind); Add(H.Source); Add(H.Seq); Add(H.Generation); Add(H.MetadataOrdinal);
		Add(H.Time); Add(H.bSnapshot); Add(H.bMetadataOmitted);
		const auto& S = P.Fixed->Scalars;
#define SNAPSHOT_SCALAR(Field) Add(S.Field)
		SNAPSHOT_SCALAR(RequestId); SNAPSHOT_SCALAR(DispatchId); SNAPSHOT_SCALAR(ScopeId); SNAPSHOT_SCALAR(ClientId);
		SNAPSHOT_SCALAR(OriginalRequestId); SNAPSHOT_SCALAR(AuthRefreshId); SNAPSHOT_SCALAR(CauseRequestId);
		SNAPSHOT_SCALAR(SocketId); SNAPSHOT_SCALAR(FrameId); SNAPSHOT_SCALAR(Attempt); SNAPSHOT_SCALAR(CurrentAttempt);
		SNAPSHOT_SCALAR(Status); SNAPSHOT_SCALAR(ErrorHttpStatus); SNAPSHOT_SCALAR(ScriptCodeOffset); SNAPSHOT_SCALAR(SourceLine);
		SNAPSHOT_SCALAR(PIEInstance); SNAPSHOT_SCALAR(DecodeWarningCount); SNAPSHOT_SCALAR(BodyBytes); SNAPSHOT_SCALAR(BytesUp);
		SNAPSHOT_SCALAR(BytesDown); SNAPSHOT_SCALAR(NoteBytes); SNAPSHOT_SCALAR(DeliveredBodyBytes); SNAPSHOT_SCALAR(TotalBytes);
		SNAPSHOT_SCALAR(StartedAt); SNAPSHOT_SCALAR(AttemptStart); SNAPSHOT_SCALAR(GapSeconds); SNAPSHOT_SCALAR(AuthWait);
		SNAPSHOT_SCALAR(LimiterWait); SNAPSHOT_SCALAR(UploadDoneAt); SNAPSHOT_SCALAR(StatusAt); SNAPSHOT_SCALAR(TransportDoneAt);
		SNAPSHOT_SCALAR(DecodeStartAt); SNAPSHOT_SCALAR(DecodeEndAt); SNAPSHOT_SCALAR(Delay); SNAPSHOT_SCALAR(RetryAfter);
		Add(S.StartUtc.GetTicks()); Add(S.Endpoint.ToString()); Add(S.Tag.ToString()); Add(S.CachePolicy.ToString());
		Add(S.FailureDetail.ToString()); Add(S.ResultType.ToString()); Add(S.Reason.ToString());
		SNAPSHOT_SCALAR(Priority); SNAPSHOT_SCALAR(Flags); SNAPSHOT_SCALAR(BodyKind); SNAPSHOT_SCALAR(CacheLookup);
		SNAPSHOT_SCALAR(GapKind); SNAPSHOT_SCALAR(Failure); SNAPSHOT_SCALAR(Outcome); SNAPSHOT_SCALAR(CancelReason);
		SNAPSHOT_SCALAR(ErrorKind); SNAPSHOT_SCALAR(Storage); SNAPSHOT_SCALAR(NoteKind); SNAPSHOT_SCALAR(Owner);
		SNAPSHOT_SCALAR(ExcerptKind); SNAPSHOT_SCALAR(Skip); SNAPSHOT_SCALAR(MarkerKind); SNAPSHOT_SCALAR(bCacheLookupPending);
		SNAPSHOT_SCALAR(bExcerptPending); SNAPSHOT_SCALAR(bConditional); SNAPSHOT_SCALAR(bFromCache); SNAPSHOT_SCALAR(bEnvelopeApplied);
		SNAPSHOT_SCALAR(bTruncated); SNAPSHOT_SCALAR(bRedacted); SNAPSHOT_SCALAR(bOpened); SNAPSHOT_SCALAR(bEditorScope); SNAPSHOT_SCALAR(bCreated);
#undef SNAPSHOT_SCALAR
		for (int32 I = 0; I < int32(EText::Count); ++I) Add(P.Text(EText(I)));
		for (const auto Slice : {P.Fixed->Headers, P.Fixed->Warnings})
		{
			const auto Pairs = P.Pairs(Slice); Add(Pairs.Num());
			for (const auto& Pair : Pairs) { Add(P.Read(Pair.First)); Add(P.Read(Pair.Second)); }
		}
		Add(P.Excerpt.Num());
		for (uint8 Byte : P.Excerpt) Add(Byte);
	}
	void Row(const FVaRestInspectorRecordView& R)
	{
#define SNAPSHOT_ROW(Field) Add(R.Field)
		SNAPSHOT_ROW(Kind); SNAPSHOT_ROW(RequestId); SNAPSHOT_ROW(Revision); SNAPSHOT_ROW(ScopeId); SNAPSHOT_ROW(ClientId);
		SNAPSHOT_ROW(ArrivalOrdinal); SNAPSHOT_ROW(BirthMetadataOrdinal); SNAPSHOT_ROW(CurrentAttempt); SNAPSHOT_ROW(StartedAt);
		Add(R.StartUtc.GetTicks()); SNAPSHOT_ROW(bStartedAtKnown); SNAPSHOT_ROW(bStartUtcKnown); SNAPSHOT_ROW(bInitiatorKnown);
		SNAPSHOT_ROW(bMetadataOmitted); Add(R.ScopeLabel); Add(R.ClientLabel); SNAPSHOT_ROW(EndState); SNAPSHOT_ROW(bPartial);
		SNAPSHOT_ROW(bPinned); SNAPSHOT_ROW(bSealed); SNAPSHOT_ROW(bTerminalKnown); SNAPSHOT_ROW(ObservedDispatches); SNAPSHOT_ROW(ObservedEnds);
		SNAPSHOT_ROW(OmittedDispatches); SNAPSHOT_ROW(OmittedEnds); SNAPSHOT_ROW(OmittedExcerpts); SNAPSHOT_ROW(OmittedNotes);
		SNAPSHOT_ROW(OmittedTimeline); SNAPSHOT_ROW(WireBytesUp); SNAPSHOT_ROW(WireBytesDown); SNAPSHOT_ROW(bWireBytesUnknown);
		SNAPSHOT_ROW(bCountersSaturated); SNAPSHOT_ROW(FinalStatus); SNAPSHOT_ROW(FinalBodyBytes); SNAPSHOT_ROW(FinalStorage);
		SNAPSHOT_ROW(CompletedAt); SNAPSHOT_ROW(DecodeStartAt); SNAPSHOT_ROW(DecodeEndAt); SNAPSHOT_ROW(bActualCompletionObserved);
		SNAPSHOT_ROW(CompletedObservedAt); SNAPSHOT_ROW(CompletedMetadataState); SNAPSHOT_ROW(FinalPreviewState); SNAPSHOT_ROW(RequestExcerptState);
#undef SNAPSHOT_ROW
		Phase(R.Total); Phase(R.Decode); Phase(R.Deliver); Packet(R.Started); Packet(R.Completed); Packet(R.Marker);
		Add(R.Dispatches.Num());
		for (const auto& D : R.Dispatches)
		{
			Add(D.DispatchId); Add(D.Attempt); Add(D.bHasDispatched); Add(D.bHasEnded); Add(D.bCombinedSendWait);
			Add(D.AttemptStart); Add(D.DispatchAt); Add(D.UploadDoneAt); Add(D.StatusAt); Add(D.TransportDoneAt);
			Add(D.GapKind); Add(D.GapSeconds); Add(D.HeadersState); Add(D.ResponseExcerptState);
			Packet(D.Dispatched); Packet(D.Ended); Packet(D.ResponseExcerpt);
			Phase(D.Auth); Phase(D.Queued); Phase(D.Prepare); Phase(D.Send); Phase(D.Wait); Phase(D.Receive); Phase(D.Wall); Phase(D.Gap);
		}
		for (const auto Packets : {R.DispatchEvents, R.Notes, R.Excerpts})
		{
			Add(Packets.Num()); for (const auto& P : Packets) Packet(P);
		}
		Add(R.Timeline.Num());
		for (const auto& T : R.Timeline) { Add(T.Kind); Add(T.Seq); Add(T.Time); Add(T.Attempt); Add(T.DispatchId); Add(T.DetailState); }
	}
};
static FString Image(const FSnapshot& Snapshot)
{
	FSnapshotRecordImage Out;
	Out.Add(Snapshot->GetRevision());
	const auto& D = Snapshot->GetDiagnostics();
	Out.Add(D.DroppedEvents); Out.Add(D.DroppedBytes); Out.Add(D.MetadataOmitted); Out.Add(D.StoreBytes);
	Out.Add(D.StorePeakBytes); Out.Add(D.SnapshotControllers); Out.Add(D.PendingBytes); Out.Add(D.PendingPeakBytes);
	Snapshot->ForEachRecord([&](const auto& R) { Out.Row(R); });
	return MoveTemp(Out.Text);
}
static FSnapshot Take(FCase& C)
{
	FSnapshot Out;
	C.Test.TestTrue(TEXT("bounded snapshot transaction succeeds"), C.Store.TrySnapshot(Out));
	C.Test.TestTrue(TEXT("published snapshot is valid"), Out.IsValid());
	if (Out)
	{
		C.Test.TestEqual(TEXT("snapshot captures current revision"), Out->GetRevision(), C.Store.Revision());
		int32 Stats = 0; Out->ForEachStats([&](const auto&) { ++Stats; });
		C.Test.TestEqual(TEXT("Stats remain truthfully empty until Task10"), Stats, 0);
		C.Test.TestEqual(TEXT("Links remain empty until Task9"), Out->GetLinks().Num(), 0);
	}
	return Out;
}
template <typename F>
static bool Mutate(FCase& C, F&& Mutation)
{
	auto Before = Take(C);
	if (!Before) return false;
	const FString Expected = Image(Before);
	const bool bChanged = Mutation();
	auto After = Take(C);
	if (!After) return false;
	const bool bStable = C.Test.TestTrue(TEXT("retained complete snapshot unchanged across mutation"), Image(Before) == Expected);
	return bChanged && bStable;
}
static bool EventMutation(FCase& C, const FVaRestTraceEvent& E, EPush Expected = EPush::Accepted, bool bApplied = true)
{
	return Mutate(C, [&] { return Deliver(C, E, Expected, bApplied); });
}
} // namespace VaRest::Inspector::Private::SnapshotTesting

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorSealingTest,
	"VaRest.Inspector.Store.Sealing", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorSealingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	using namespace VaRest::Inspector::Private::SnapshotTesting;
	bool bAll = true;
	bAll &= WithStore(*this, TEXT("Sealing: early request/physical excerpts and duplicate resolution"), [&](FCase& C) {
		bool b = EventMutation(C, Excerpt(1, 0, 'q'));
		Visit(*this, C.Store, 1, [&](const auto& R) {
			TestTrue(TEXT("early excerpt base is unpinned partial"), R.bPartial && !R.bPinned && !R.bTerminalKnown && !R.bSealed);
			TestTrue(TEXT("early excerpt reports CaptureInterrupted"), R.EndState == EVaRestInspectorEndState::CaptureInterrupted);
			CheckRequest(*this, R, EDetail::Present, 'q');
		});
		b &= EventMutation(C, Excerpt(1, 101, 'r'));
		b &= EventMutation(C, Started(1, true));
		b &= EventMutation(C, Dispatch(1, 101));
		b &= EventMutation(C, End(1, 101));
		Visit(*this, C.Store, 1, [&](const auto& R) {
			TestTrue(TEXT("actual Started provides source liveness"), R.bPinned && !R.bTerminalKnown);
			CheckRequest(*this, R, EDetail::Present, 'q'); CheckResponse(*this, R, 101, EDetail::Present, 'r');
		});
		const uint64 Revision = C.Store.Revision();
		b &= EventMutation(C, Excerpt(1, 0, 'z'), EPush::Accepted, false);
		b &= EventMutation(C, Excerpt(1, 101, 'z'), EPush::Accepted, false);
		TestEqual(TEXT("duplicate resolved excerpt does not change history/revision"), C.Store.Revision(), Revision);
		b &= EventMutation(C, Completed(1));
		Visit(*this, C.Store, 1, [&](const auto& R) {
			TestTrue(TEXT("already resolved promises seal at actual completion"), R.bSealed && R.bActualCompletionObserved && !R.bPinned);
			CheckRequest(*this, R, EDetail::Present, 'q'); CheckResponse(*this, R, 101, EDetail::Present, 'r');
		});
		b &= EventMutation(C, Started(2)); b &= EventMutation(C, Completed(2));
		Visit(*this, C.Store, 2, [&](const auto& R) { TestTrue(TEXT("no promise seals immediately, positive control"), R.bSealed); });
		return b;
	});
	bAll &= WithStore(*this, TEXT("Sealing: late excerpts after Completed, distinct physical identities"), [&](FCase& C) {
		bool b = EventMutation(C, Started(10, true));
		b &= EventMutation(C, Dispatch(10, 501)); b &= EventMutation(C, End(10, 501));
		b &= EventMutation(C, Dispatch(10, 502)); b &= EventMutation(C, End(10, 502));
		b &= EventMutation(C, Completed(10));
		Visit(*this, C.Store, 10, [&](const auto& R) {
			TestTrue(TEXT("terminal pending row remains mutable"), R.bTerminalKnown && !R.bSealed);
			TestEqual(TEXT("first actual receipt is true zero"), R.CompletedObservedAt, 0.0);
			TestEqual(TEXT("source time remains independent"), R.CompletedAt, 1000000.0);
			CheckRequest(*this, R, EDetail::Pending); CheckResponse(*this, R, 501, EDetail::Pending); CheckResponse(*this, R, 502, EDetail::Pending);
		});
		// Mutant1 must reach these unchanged actual-byte assertions, not only setup.
		b &= EventMutation(C, Excerpt(10, 0, 'a'), EPush::Accepted, false);
		b &= EventMutation(C, Excerpt(10, 501, 'b'), EPush::Accepted, false);
		Visit(*this, C.Store, 10, [&](const auto& R) {
			CheckRequest(*this, R, EDetail::Present, 'a'); CheckResponse(*this, R, 501, EDetail::Present, 'b');
			CheckResponse(*this, R, 502, EDetail::Pending);
			TestFalse(TEXT("same Attempt does not resolve another physical promise"), R.bSealed);
		});
		b &= EventMutation(C, Excerpt(10, 502, 'c'), EPush::Accepted, false);
		Visit(*this, C.Store, 10, [&](const auto& R) {
			CheckResponse(*this, R, 502, EDetail::Present, 'c');
			TestTrue(TEXT("late final promised body seals real row"), R.bSealed);
		});
		const uint64 Revision = C.Store.Revision();
		b &= EventMutation(C, Excerpt(10, 999, 'z'), EPush::Accepted, false);
		b &= EventMutation(C, Excerpt(10, 501, 'z'), EPush::Accepted, false);
		TestEqual(TEXT("sealed or absent late slot never reopens immutable row"), C.Store.Revision(), Revision);
		return b;
	});
	bAll &= WithStore(*this, TEXT("Sealing: observer deadlines, duplicate Completed and no-completion control"), [&](FCase& C) {
		bool b = EventMutation(C, Started(20, true)); b &= EventMutation(C, Completed(20));
		b &= EventMutation(C, Started(21, true));
		C.Clock.Set(4.0);
		b &= EventMutation(C, Completed(20), EPush::Accepted, false);
		C.Clock.Set(4.999);
		b &= Mutate(C, [&] { TestEqual(TEXT("4.999 is not eligible"), FStoreAccess::Expire(C.Store), 0); return true; });
		Visit(*this, C.Store, 20, [&](const auto& R) { CheckRequest(*this, R, EDetail::Pending); TestFalse(TEXT("pending before five seconds"), R.bSealed); });
		C.Clock.Set(5.0);
		b &= Mutate(C, [&] { TestEqual(TEXT("5.000 expires exactly one actual completion"), FStoreAccess::Expire(C.Store), 1); return true; });
		Visit(*this, C.Store, 20, [&](const auto& R) {
			CheckRequest(*this, R, EDetail::TimedOut); TestTrue(TEXT("receipt timeout seals"), R.bSealed);
			TestEqual(TEXT("duplicate did not extend first receipt"), R.CompletedObservedAt, 0.0);
		});
		C.Clock.Set(100);
		TestEqual(TEXT("uncompleted row has no deadline"), FStoreAccess::Expire(C.Store), 0);
		Visit(*this, C.Store, 21, [&](const auto& R) { CheckRequest(*this, R, EDetail::Pending); TestFalse(TEXT("timeout cannot invent completion"), R.bTerminalKnown || R.bSealed); });
		return b;
	});
	bAll &= WithStore(*this, TEXT("Sealing: explicit Backlog/Discarded/descriptor and Bodies-off"), [&](FCase& C) {
		bool b = EventMutation(C, Started(30, true)); b &= EventMutation(C, End(30, 301)); b &= EventMutation(C, Completed(30));
		b &= EventMutation(C, Excerpt(30, 0, 0, EVaRestExcerptSkip::Backlog));
		b &= EventMutation(C, Excerpt(30, 301, 0, EVaRestExcerptSkip::Discarded));
		Visit(*this, C.Store, 30, [&](const auto& R) { CheckRequest(*this, R, EDetail::Backlog); CheckResponse(*this, R, 301, EDetail::Discarded); TestTrue(TEXT("explicit skips resolve promises"), R.bSealed); });
		b &= EventMutation(C, Started(31, true)); b &= EventMutation(C, Completed(31));
		auto Descriptor = Excerpt(31);
		auto& D = Descriptor.Payload.Get<FVaRestTraceExcerptEvent>();
		D.Excerpt.Bytes.Reset(); D.Excerpt.Kind = EVaRestExcerptKind::Binary; D.Excerpt.Sha256Prefix = TEXT("12345678");
		b &= EventMutation(C, Descriptor);
		Visit(*this, C.Store, 31, [&](const auto& R) { CheckRequest(*this, R, EDetail::Present); TestTrue(TEXT("descriptor-only result is present and seals"), R.bSealed); });
		b &= EventMutation(C, Started(32, true)); b &= EventMutation(C, End(32, 321)); b &= EventMutation(C, Completed(32));
		b &= EventMutation(C, Started(33, true));
		b &= Mutate(C, [&] { FStoreAccess::Stop(C.Store); return true; });
		Visit(*this, C.Store, 32, [&](const auto& R) { CheckRequest(*this, R, EDetail::CaptureStopped); CheckResponse(*this, R, 321, EDetail::CaptureStopped); TestTrue(TEXT("Bodies-off resolves terminal promises"), R.bSealed); });
		Visit(*this, C.Store, 33, [&](const auto& R) { CheckRequest(*this, R, EDetail::CaptureStopped); TestFalse(TEXT("Bodies-off never invents terminal result"), R.bSealed || R.bTerminalKnown); });
		FCapturePolicy Off = C.Policy; Off.bBodies = false;
		FCaptureQueue OffQueue(Off, 1, C.Roots.Pending.ToSharedRef(), C.Roots.Lifetime->Allocator);
		TestTrue(TEXT("Bodies-off queue supported"), OffQueue.IsSupported());
		TestTrue(TEXT("Bodies-off codec rejects a raw excerpt"), OffQueue.TryPush(Excerpt(33)) == EPush::Unsupported);
		OffQueue.Close();
		return b;
	});
	bAll &= WithStore(*this, TEXT("Sealing: real queue scalar Backlog versus fully dropped excerpt"), [&](FCase& C) {
		bool b = Deliver(C, Started(40, true)); b &= Deliver(C, Completed(40));
		const auto Fixed = C.Roots.Lifetime->Allocator.Bound(sizeof(FPacketFixed), alignof(FPacketFixed));
		if (!TestTrue(TEXT("actual scalar layout bound available"), Fixed.IsSet())) return false;
		const int64 ScalarCharge = int64(Fixed.GetValue()) + int64(C.Roots.Lifetime->Allocator.QueueNodeBound());
		const int64 PressureBytes = C.Roots.Pending->ByteLimit - C.Roots.Pending->Bytes.load() - ScalarCharge;
		if (!TestTrue(TEXT("positive scalar fallback pressure"), PressureBytes > 0)) return false;
		{
			auto Pressure = FPendingBaseline::Acquire(C.Roots.Pending.ToSharedRef(), PressureBytes);
			if (!TestTrue(TEXT("real byte pressure admitted"), Pressure.IsSet())) return false;
			auto Large = Excerpt(40);
			auto Bytes = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
			Bytes->SetNumUninitialized(4096); FMemory::Memset(Bytes->GetData(), 'p', Bytes->Num());
			Large.Payload.Get<FVaRestTraceExcerptEvent>().Excerpt.Bytes = Bytes;
			b &= EventMutation(C, Large, EPush::ScalarOnly);
		}
		Visit(*this, C.Store, 40, [&](const auto& R) { CheckRequest(*this, R, EDetail::Backlog); TestTrue(TEXT("actual queue fallback resolves promised body"), R.bSealed); });
		b &= Deliver(C, Started(41, true)); b &= Deliver(C, Completed(41));
		{
			// Genuine admitted packet count exhaustion, not a counter override.
			for (int64 I = 0; I < C.Roots.Pending->CountLimit; ++I)
				b &= TestTrue(TEXT("fill actual pending lane"), C.Queue.TryPush(Note(41, uint64(I))) == EPush::Accepted);
			auto Before = Take(C); if (!Before) return false;
			const FString Frozen = Image(Before);
			TestTrue(TEXT("count-full excerpt actually dropped"), C.Queue.TryPush(Excerpt(41)) == EPush::Dropped);
			while (C.Queue.TryPopMetadata().IsSet()) {}
			TestFalse(TEXT("dropped excerpt has no imaginary packet"), C.Queue.TryPopExcerpt().IsSet());
			TestTrue(TEXT("dropped source event leaves retained snapshot unchanged"), Image(Before) == Frozen);
		}
		Visit(*this, C.Store, 41, [&](const auto& R) { CheckRequest(*this, R, EDetail::Pending); TestFalse(TEXT("drop cannot seal immediately"), R.bSealed); });
		C.Clock.Set(5);
		TestEqual(TEXT("dropped excerpt resolves only by observer timeout"), FStoreAccess::Expire(C.Store), 1);
		Visit(*this, C.Store, 41, [&](const auto& R) { CheckRequest(*this, R, EDetail::TimedOut); });
		return b;
	});
	bAll &= WithStore(*this, TEXT("Sealing: promises survive optional packet ownership refusal"), [&](FCase& C) {
		bool b = Deliver(C, Note(45, 1)); b &= Deliver(C, Dispatch(45, 451));
		auto Before = Take(C); if (!Before) return false;
		const FString Frozen = Image(Before);
		{
			const int64 Available = C.Roots.Budget->Limit - C.Roots.Budget->UsedBytes.load();
			auto Pressure = FStoreCredit::Acquire(C.Roots.Budget, Available);
			if (!TestTrue(TEXT("real store pressure covers all remaining optional credit"), Pressure.IsSet())) return false;
			b &= Deliver(C, Started(45, true)); b &= Deliver(C, End(45, 451));
			b &= Deliver(C, Excerpt(45, 0, 'd'), EPush::Accepted, false);
			b &= Deliver(C, Excerpt(45, 451, 'e'), EPush::Accepted, false);
		}
		Visit(*this, C.Store, 45, [&](const auto& R) {
			TestTrue(TEXT("Started source scalars survive refused packet owner"), R.bStartedAtKnown && R.bPinned);
			TestNull(TEXT("optional Started packet really refused"), R.Started.Fixed);
			CheckRequest(*this, R, EDetail::Pending); CheckResponse(*this, R, 451, EDetail::Pending);
			TestTrue(TEXT("both refused excerpt transfers counted"), R.OmittedExcerpts >= 2);
		});
		b &= Deliver(C, Completed(45));
		C.Clock.Set(5);
		TestEqual(TEXT("refused transferred bodies retain one completion deadline"), FStoreAccess::Expire(C.Store), 1);
		Visit(*this, C.Store, 45, [&](const auto& R) { CheckRequest(*this, R, EDetail::TimedOut); CheckResponse(*this, R, 451, EDetail::TimedOut); TestTrue(TEXT("refused bodies seal only after actual timeout"), R.bSealed); });
		TestTrue(TEXT("old snapshot survives optional packet refusal and timeout"), Image(Before) == Frozen);
		return b;
	});
	bAll &= WithStore(*this, TEXT("Sealing: early orphan reconciliation, sixteen slots and actual base cap"), [&](FCase& C) {
		FVaRestTraceSnapshot Empty;
		bool b = C.Store.ApplyAttached(Empty, C.Policy, C.Roots.Pending.ToSharedRef(), C.Roots.Lifetime->Allocator);
		const uint64 Epoch = FStoreAccess::OrphanEpoch(C.Store);
		b &= EventMutation(C, Excerpt(50, 501));
		TestTrue(TEXT("new local orphan requests reconciliation after previous empty walk"), FStoreAccess::OrphanEpoch(C.Store) > Epoch);
		FVaRestTraceSnapshot Present;
		Present.InFlight.Add(Started(50).Payload.Get<FVaRestTraceStarted>());
		b &= Mutate(C, [&] { return C.Store.ApplyAttached(Present, C.Policy, C.Roots.Pending.ToSharedRef(), C.Roots.Lifetime->Allocator); });
		Visit(*this, C.Store, 50, [&](const auto& R) { TestTrue(TEXT("actual source presence promotes early orphan"), R.bPinned); });
		b &= Mutate(C, [&] { return C.Store.ApplyAttached(Empty, C.Policy, C.Roots.Pending.ToSharedRef(), C.Roots.Lifetime->Allocator); });
		Visit(*this, C.Store, 50, [&](const auto& R) { TestFalse(TEXT("absence unpins without terminal result"), R.bPinned || R.bTerminalKnown || R.bSealed); });
		for (uint64 Id = 502; Id <= 516; ++Id) b &= EventMutation(C, Excerpt(50, Id));
		const auto Before = C.Store.Diagnostics();
		b &= EventMutation(C, Excerpt(50, 517), EPush::Accepted, false);
		Visit(*this, C.Store, 50, [&](const auto& R) { TestEqual(TEXT("physical excerpt slots bounded at sixteen"), R.Dispatches.Num(), 16); });
		TestTrue(TEXT("unadmitted early physical excerpt diagnosed"), C.Store.Diagnostics().MetadataOmitted > Before.MetadataOmitted);
		Visit(*this, C.Store, 50, [&](const auto& R) { TestEqual(TEXT("unadmitted early physical excerpt counted exactly"), R.OmittedExcerpts, uint64(1)); });
		b &= Deliver(C, Started(51));
		const auto Full = C.Store.Diagnostics();
		b &= EventMutation(C, Excerpt(52), EPush::Accepted, false);
		TestEqual(TEXT("no unbounded orphan base past actual row cap"), CountRows(C.Store), 2);
		TestTrue(TEXT("refused early base counted"), C.Store.Diagnostics().RefusedRows > Full.RefusedRows);
		return b;
	}, 2);
	bAll &= WithStore(*this, TEXT("Sealing: nonfinite receipt/current clock and bounded 256 expiry batch"), [&](FCase& C) {
		C.Clock.Set(std::numeric_limits<double>::quiet_NaN());
		bool b = Deliver(C, Started(60, true)); b &= Deliver(C, Completed(60));
		C.Clock.Set(0);
		for (uint64 Id = 100; Id < 400; ++Id) { b &= Deliver(C, Started(Id, true)); b &= Deliver(C, Completed(Id)); }
		auto Before = Take(C); if (!Before) return false;
		const FString Frozen = Image(Before);
		C.Clock.Set(std::numeric_limits<double>::infinity());
		TestEqual(TEXT("nonfinite current clock cannot advance expiration"), FStoreAccess::Expire(C.Store), 0);
		C.Clock.Set(5);
		TestEqual(TEXT("first due batch bounded to256"), FStoreAccess::Expire(C.Store), 256);
		TestEqual(TEXT("exactly256 finite rows sealed"), CountSealed(C.Store), 256);
		TestEqual(TEXT("next due batch finishes remaining44"), FStoreAccess::Expire(C.Store), 44);
		TestEqual(TEXT("nonfinite receipt cannot block finite rows"), CountSealed(C.Store), 300);
		Visit(*this, C.Store, 60, [&](const auto& R) { TestFalse(TEXT("invalid first receipt preserved"), FMath::IsFinite(R.CompletedObservedAt)); CheckRequest(*this, R, EDetail::Pending); TestFalse(TEXT("nonfinite first receipt never invented a deadline"), R.bSealed); });
		TestTrue(TEXT("old300-row snapshot survives both expiry batches"), Image(Before) == Frozen);
		C.Clock.Set(100);
		b &= Deliver(C, Completed(60), EPush::Accepted, false);
		TestEqual(TEXT("duplicate cannot replace nonfinite first receipt"), FStoreAccess::Expire(C.Store), 0);
		Before.Reset();
		b &= EventMutation(C, Excerpt(60));
		Visit(*this, C.Store, 60, [&](const auto& R) { TestTrue(TEXT("late actual resolution can seal invalid-clock row"), R.bSealed); });
		return b;
	}, 512);
	bAll &= WithStore(*this, TEXT("Sealing: cache physical304 remains separate from final preview"), [&](FCase& C) {
		bool b = EventMutation(C, Started(70)); b &= EventMutation(C, Dispatch(70, 701));
		auto Wire = End(70, 701); Wire.Payload.Get<FVaRestTraceAttemptEnded>().Status = 304;
		b &= EventMutation(C, Wire); b &= EventMutation(C, Completed(70, true));
		b &= EventMutation(C, Excerpt(70, 701, 'w'));
		Visit(*this, C.Store, 70, [&](const auto& R) {
			TestEqual(TEXT("delivered cache status stays200"), R.FinalStatus, 200);
			TestTrue(TEXT("cache final preview remains NoProducer"), R.FinalPreviewState == EDetail::NoProducer);
			CheckResponse(*this, R, 701, EDetail::Present, 'w');
			if (TestEqual(TEXT("one physical cache slot"), R.Dispatches.Num(), 1) && TestNotNull(TEXT("wire Ended descriptor"), R.Dispatches[0].Ended.Fixed))
				TestEqual(TEXT("physical status remains304"), R.Dispatches[0].Ended.Fixed->Scalars.Status, 304);
		});
		return b;
	});
	return bAll;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorSnapshotIsolationTest,
	"VaRest.Inspector.Store.SnapshotIsolation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorSnapshotIsolationTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	using namespace VaRest::Inspector::Private::SnapshotTesting;
	bool bAll = true;
	bAll &= WithStore(*this, TEXT("SnapshotIsolation: mutable deep clone, physical append, late body and rings"), [&](FCase& C) {
		bool b = Deliver(C, Client(TEXT("original-client")));
		b &= Deliver(C, Started(1000, true)); b &= Deliver(C, Dispatch(1000, 1)); b &= Deliver(C, End(1000, 1));
		b &= Deliver(C, Note(1000, 7));
		auto Old = Take(C); if (!Old) return false;
		const FString Frozen = Image(Old);
		const uint8* LiveBacking = nullptr;
		Visit(*this, C.Store, 1000, [&](const auto& R) { LiveBacking = R.Started.Backing.GetData(); });
		Visit(*this, Old, 1000, [&](const auto& R) {
			TestTrue(TEXT("unsealed Started has fresh snapshot backing"), R.Started.Backing.GetData() && R.Started.Backing.GetData() != LiveBacking);
			TestEqual(TEXT("snapshot owns original client label"), FString(R.ClientLabel), FString(TEXT("original-client")));
		});
		// Mutant2 changes real share-vs-clone selection in measurement AND build.
		b &= Deliver(C, Dispatch(1000, 2));
		Visit(*this, C.Store, 1000, [&](const auto& R) { TestEqual(TEXT("new physical dispatch reached live row"), R.Dispatches.Num(), 2); });
		Visit(*this, Old, 1000, [&](const auto& R) { TestEqual(TEXT("MUTANT2 old unsealed snapshot retains one dispatch"), R.Dispatches.Num(), 1); });
		TestTrue(TEXT("complete old row image unchanged after new dispatch"), Image(Old) == Frozen);
		b &= Deliver(C, End(1000, 2)); b &= Deliver(C, Completed(1000));
		b &= Deliver(C, Excerpt(1000, 0, 'u')); b &= Deliver(C, Excerpt(1000, 1, 'v'));
		Visit(*this, Old, 1000, [&](const auto& R) { CheckRequest(*this, R, EDetail::Pending); CheckResponse(*this, R, 1, EDetail::Pending); TestFalse(TEXT("old mutable snapshot remains nonterminal"), R.bTerminalKnown); });
		TestTrue(TEXT("late excerpt backing cannot mutate old snapshot"), Image(Old) == Frozen);
		b &= Deliver(C, Excerpt(1000, 2, 'w'));
		Visit(*this, C.Store, 1000, [&](const auto& R) { TestTrue(TEXT("live row eventually sealed"), R.bSealed); });
		b &= Deliver(C, Client(TEXT("replacement-client")));
		C.Store.BeginGeneration(2, 300); C.Store.Clear();
		TestTrue(TEXT("mutable cloned labels/backing survive generation and Clear"), Image(Old) == Frozen);
		Old.Reset();
		C.Store.BeginGeneration(1, 0);
		b &= Deliver(C, Started(1001));
		for (uint64 Seq = 100; Seq < 164; ++Seq) b &= Deliver(C, Note(1001, Seq));
		auto Rings = Take(C); if (!Rings) return false;
		const FString RingImage = Image(Rings);
		for (uint64 Seq = 164; Seq < 230; ++Seq) b &= Deliver(C, Note(1001, Seq));
		Visit(*this, C.Store, 1001, [&](const auto& R) {
			TestEqual(TEXT("notes independently bounded64"), R.Notes.Num(), 64);
			TestEqual(TEXT("timeline independently wraps128"), R.Timeline.Num(), 128);
			if (TestEqual(TEXT("timeline remains available"), R.Timeline.Num(), 128))
			{
				TestEqual(TEXT("timeline application order first retained source sequence"), R.Timeline[0].Seq, uint64(102));
				TestTrue(TEXT("evicted note serial marks old timeline availability"), R.Timeline[0].DetailState == EDetail::Evicted);
				TestEqual(TEXT("raw source time preserved without sorting"), R.Timeline.Last().Time, 0.0);
			}
		});
		TestTrue(TEXT("cloned note/timeline rings isolate eviction and wrap"), Image(Rings) == RingImage);
		C.Store.Clear();
		TestTrue(TEXT("cloned ring backing survives real frees"), Image(Rings) == RingImage);
		return b;
	});
	bAll &= WithStore(*this, TEXT("SnapshotIsolation: scalar Completed fallback owns its Fixed and first receipt"), [&](FCase& C) {
		bool b = Deliver(C, Started(1100, true));
		auto Completion = Completed(1100);
		auto& V = Completion.Payload.Get<FVaRestTraceCompleted>();
		V.Response.EffectiveUrl = FString::ChrN(FNormalizedLimits::MaxStringChars + 1, TCHAR('x'));
		V.Error.Kind = EVaRestErrorKind::Http; V.Error.HttpStatus = 599; V.DecodeWarningCount = 44;
		b &= Deliver(C, Completion, EPush::ScalarOnly);
		auto Copy = Take(C); if (!Copy) return false;
		const FString Frozen = Image(Copy);
		const FPacketFixed* LiveFixed = nullptr;
		Visit(*this, C.Store, 1100, [&](const auto& R) { LiveFixed = R.Completed.Fixed; });
		Visit(*this, Copy, 1100, [&](const auto& R) {
			TestTrue(TEXT("fallback explicitly metadata omitted"), R.CompletedMetadataState == EDetail::MetadataOmitted);
			TestTrue(TEXT("fallback Fixed cloned independently"), R.Completed.Fixed && R.Completed.Fixed != LiveFixed);
			TestEqual(TEXT("fallback has no borrowed backing"), R.Completed.Backing.Num(), 0);
			if (R.Completed.Fixed)
			{
				TestEqual(TEXT("fallback retains typed warning total"), R.Completed.Fixed->Scalars.DecodeWarningCount, 44);
				TestEqual(TEXT("fallback error status independent from delivered status"), R.Completed.Fixed->Scalars.ErrorHttpStatus, 599);
				TestEqual(TEXT("fallback result status"), R.Completed.Fixed->Scalars.Status, 200);
			}
			TestEqual(TEXT("fallback actual observer receipt preserved"), R.CompletedObservedAt, 0.0);
			TestEqual(TEXT("fallback source receipt not substituted"), R.CompletedAt, 1000000.0);
		});
		C.Clock.Set(5); FStoreAccess::Expire(C.Store); C.Store.Clear();
		TestTrue(TEXT("fallback descriptor remains valid after source death"), Image(Copy) == Frozen);
		return b;
	});
	bAll &= WithStore(*this, TEXT("SnapshotIsolation: real eligible removal retains charged sealed backing, collisions and tombstones"), [&](FCase& C) {
		const int32 Capacity = FStoreAccess::IndexCapacity(C.Store);
		const int32 Retained = FStoreAccess::RetainedIdCapacity(C.Store);
		if (!TestTrue(TEXT("actual bounded index capacity"), Capacity > 0 && (Capacity & (Capacity - 1)) == 0) ||
			!TestTrue(TEXT("actual finite retained-id capacity"), Retained > 0 && Retained <= 4096)) return false;
		// For these <2^32 ids the actual multiplicative hash has identical low
		// mask bits when the ids differ by the actual power-of-two capacity.
		const uint64 A = 1, B = A + uint64(Capacity), D = B + uint64(Capacity);
		const auto Bucket = [Capacity](uint64 Id) { return (uint32(Id ^ (Id >> 32)) * 0x9e3779b9u) & uint32(Capacity - 1); };
		TestEqual(TEXT("A/B collide under actual hash"), Bucket(A), Bucket(B));
		TestEqual(TEXT("A/D collide under actual hash"), Bucket(A), Bucket(D));
		bool b = Deliver(C, Started(A)); b &= Deliver(C, Completed(A));
		b &= Deliver(C, Started(B)); b &= Deliver(C, Started(D)); b &= Deliver(C, Completed(D));
		auto Reader = Take(C); if (!Reader) return false;
		const FString Frozen = Image(Reader);
		const uint8* LiveBacking = nullptr;
		Visit(*this, C.Store, A, [&](const auto& R) {
			TestTrue(TEXT("real eviction candidate sealed and eligible"), R.bSealed && R.bTerminalKnown && !R.bPinned);
			TestNotNull(TEXT("real full Started packet exists"), R.Started.Fixed);
			TestTrue(TEXT("real full Started backing admitted"), R.Started.Backing.Num() > 0);
			TestEqual(TEXT("full retained URL is actual data"), FString(R.Started.Text(EText::Url)), Started(A).Payload.Get<FVaRestTraceStarted>().Url);
			LiveBacking = R.Started.Backing.GetData();
		});
		Visit(*this, Reader, A, [&](const auto& R) {
			TestTrue(TEXT("sealed snapshot actually shares the existing packet backing"), LiveBacking && R.Started.Backing.GetData() == LiveBacking);
		});
		const int64 ChargedBefore = C.Roots.Budget->UsedBytes.load();
		const int64 BackingBefore = C.Roots.Lifetime->Probe.LiveBacking.load();
		TestTrue(TEXT("genuine eligible row removal succeeds"), FStoreAccess::Remove(C.Store, A));
		// Mutant3: temporary credit reset of the existing Started packet here;
		// the future delta must guard packet presence for metadata-only rows.
		// These assertions precede reader release and any incidental allocation.
		TestEqual(TEXT("MUTANT3 retained sealed packet remains charged after actual removal"), C.Roots.Budget->UsedBytes.load(), ChargedBefore);
		TestEqual(TEXT("MUTANT3 actual backing remains alive under retained reader"), C.Roots.Lifetime->Probe.LiveBacking.load(), BackingBefore);
		TestEqual(TEXT("real row removed from arrival/index"), CountRows(C.Store), 2);
		TestFalse(TEXT("pinned collided row refuses removal"), FStoreAccess::Remove(C.Store, B));
		TestTrue(TEXT("retained shared reader remains readable after real removal"), Image(Reader) == Frozen);
		b &= Deliver(C, Dispatch(B, 401));
		Visit(*this, C.Store, B, [&](const auto& R) { TestEqual(TEXT("collided live lookup survives deleted predecessor"), R.Dispatches.Num(), 1); });
		Visit(*this, C.Store, D, [&](const auto& R) { TestTrue(TEXT("second collided live row survives"), R.bSealed); });
		b &= Deliver(C, Excerpt(A), EPush::Accepted, false);
		TestEqual(TEXT("retained tombstone prevents late resurrection"), CountRows(C.Store), 2);
		const uint64 Replacement = D + uint64(Capacity);
		b &= Deliver(C, Started(Replacement));
		b &= Deliver(C, Dispatch(B, 402));
		Visit(*this, C.Store, B, [&](const auto& R) { TestEqual(TEXT("Deleted reuse does not insert duplicate collided id"), R.Dispatches.Num(), 2); });
		TestEqual(TEXT("replacement occupies real freed capacity"), CountRows(C.Store), 3);
		for (int32 I = 0; I < Retained + 2 * Capacity; ++I)
		{
			const uint64 Id = 100000 + uint64(I);
			b &= Deliver(C, Completed(Id));
			b &= TestTrue(TEXT("rotate genuine finite tombstone retention"), FStoreAccess::Remove(C.Store, Id));
		}
		b &= Deliver(C, Dispatch(B, 403));
		Visit(*this, C.Store, B, [&](const auto& R) { TestEqual(TEXT("full-probe tombstone churn retains unique collided row"), R.Dispatches.Num(), 3); });
		b &= Deliver(C, Excerpt(A, 0, 'n'));
		Visit(*this, C.Store, A, [&](const auto& R) { TestTrue(TEXT("expired tombstone permits new unpinned Partial"), R.bPartial && !R.bPinned && !R.bTerminalKnown); CheckRequest(*this, R, EDetail::Present, 'n'); });
		TestTrue(TEXT("old reader unaffected by fresh same-id row and collision reuse"), Image(Reader) == Frozen);
		Reader.Reset();
		// Normal cleanup must succeed even with the temporary packet-credit
		// mutant: no direct ledger subtraction, second decrement or stale allocator.
		return b;
	}, 8);
	bAll &= WithStore(*this, TEXT("SnapshotIsolation: four controller slots survive strong release until final weak"), [&](FCase& C) {
		const int64 BaseBytes = C.Roots.Budget->UsedBytes.load();
		const int64 BaseBacking = C.Roots.Lifetime->Probe.LiveBacking.load();
		const int64 BaseControllers = C.Roots.Budget->Probe.LiveControllers.load();
		FSnapshot Strong[4]; FWeakSnapshot Weak[4];
		for (int32 I = 0; I < 4; ++I)
		{
			Strong[I] = Take(C); if (!Strong[I]) return false;
			Weak[I] = Strong[I];
		}
		FSnapshot Out = Strong[0]; const auto* Sentinel = Out.Get();
		TestFalse(TEXT("fifth snapshot controller refused"), C.Store.TrySnapshot(Out));
		TestTrue(TEXT("slot refusal preserves caller output"), Out.Get() == Sentinel);
		Out.Reset();
		for (auto& Owner : Strong) Owner.Reset();
		TestEqual(TEXT("payload and entire nested domains really freed at last strong"), C.Roots.Lifetime->Probe.LiveBacking.load(), BaseBacking);
		TestEqual(TEXT("only four4096 controller credits remain"), C.Roots.Budget->UsedBytes.load(), BaseBytes + 4 * 4096);
		TestEqual(TEXT("all four weak-held slots remain occupied"), C.Roots.Budget->SnapshotControllers.load(), int64(4));
		TestEqual(TEXT("stored deleters live through final weak"), C.Roots.Budget->Probe.LiveControllers.load(), BaseControllers + 4);
		for (const auto& W : Weak) TestFalse(TEXT("expired weak cannot reacquire payload"), W.Pin().IsValid());
		TestFalse(TEXT("payload destruction did not open fifth slot"), C.Store.TrySnapshot(Out));
		TestFalse(TEXT("null output remains null on weak-held refusal"), Out.IsValid());
		const int64 Destroyed = C.Roots.Budget->Probe.DestroyedControllers.load();
		std::atomic<bool> Released{false};
		auto Worker = Async(EAsyncExecution::Thread, [Owner = MoveTemp(Weak[0]), &Released]() mutable {
			Owner.Reset(); // Before signal and thread completion, not lambda teardown.
			Released.store(true, std::memory_order_release);
		});
		Worker.Get();
		TestTrue(TEXT("worker final-weak release joined"), Released.load(std::memory_order_acquire));
		TestEqual(TEXT("worker final weak releases exactly one controller"), C.Roots.Budget->Probe.DestroyedControllers.load(), Destroyed + 1);
		TestEqual(TEXT("worker final weak releases exactly one slot"), C.Roots.Budget->SnapshotControllers.load(), int64(3));
		TestEqual(TEXT("worker final weak releases exactly4096 credit"), C.Roots.Budget->UsedBytes.load(), BaseBytes + 3 * 4096);
		Out = Take(C); if (!Out) return false;
		TestEqual(TEXT("new snapshot uses newly released slot"), C.Roots.Budget->SnapshotControllers.load(), int64(4));
		Out.Reset();
		for (auto& W : Weak) W.Reset();
		TestEqual(TEXT("all final weak credits released normally"), C.Roots.Budget->UsedBytes.load(), BaseBytes);
		TestEqual(TEXT("all final weak slots released normally"), C.Roots.Budget->SnapshotControllers.load(), int64(0));
		return true;
	});
	bAll &= WithStore(*this, TEXT("SnapshotIsolation: full-domain byte refusal, unchanged Out and worker payload release"), [&](FCase& C) {
		bool b = Deliver(C, Started(1200, true)); b &= Deliver(C, Dispatch(1200, 1));
		b &= Deliver(C, Excerpt(1200, 0, 'f'));
		auto Out = Take(C); if (!Out) return false;
		const auto* Sentinel = Out.Get(); const FString Frozen = Image(Out);
		const auto P = C.Roots.Lifetime->Allocator.Bound(sizeof(FVaRestInspectorSnapshot), alignof(FVaRestInspectorSnapshot));
		if (!TestTrue(TEXT("actual outer payload bound"), P.IsSet())) return false;
		// Leave C+P available but no N: pre-admission must unwind both without
		// allocating, copying or acquiring any additional excerpt owner.
		const int64 Spare = 4096 + int64(P.GetValue());
		const int64 PressureBytes = C.Roots.Budget->Limit - C.Roots.Budget->UsedBytes.load() - Spare;
		if (!TestTrue(TEXT("real byte-pressure allowance positive"), PressureBytes > 0)) return false;
		{
			auto Pressure = FStoreCredit::Acquire(C.Roots.Budget, PressureBytes);
			if (!TestTrue(TEXT("real independent pressure credit admitted"), Pressure.IsSet())) return false;
			const int64 Bytes = C.Roots.Budget->UsedBytes.load();
			const int64 Backing = C.Roots.Lifetime->Probe.LiveBacking.load();
			const int64 Refs = C.Roots.Lifetime->Probe.ExcerptRefsAcquired.load();
			const int64 Copies = C.Roots.Lifetime->Probe.TextBytesCopied.load();
			const int64 Slots = C.Roots.Budget->SnapshotControllers.load();
			TestTrue(TEXT("byte refusal independent from slot pressure"), Slots < 4);
			TestFalse(TEXT("entire nested N must fit before construction"), C.Store.TrySnapshot(Out));
			TestTrue(TEXT("byte refusal leaves exact nonnull Out owner unchanged"), Out.Get() == Sentinel);
			TestEqual(TEXT("failed N admission rolls back C/P credits"), C.Roots.Budget->UsedBytes.load(), Bytes);
			TestEqual(TEXT("failed N admission makes no backing allocation"), C.Roots.Lifetime->Probe.LiveBacking.load(), Backing);
			TestEqual(TEXT("failed N admission acquires no excerpt reference"), C.Roots.Lifetime->Probe.ExcerptRefsAcquired.load(), Refs);
			TestEqual(TEXT("failed N admission copies no text"), C.Roots.Lifetime->Probe.TextBytesCopied.load(), Copies);
			TestEqual(TEXT("failed N admission returns transient slot"), C.Roots.Budget->SnapshotControllers.load(), Slots);
		}
		TestTrue(TEXT("refused transaction does not mutate retained snapshot"), Image(Out) == Frozen);
		C.Store.Clear();
		const int64 Before = C.Roots.Lifetime->Probe.LiveBacking.load();
		std::atomic<bool> Released{false};
		auto Worker = Async(EAsyncExecution::Thread, [Owner = MoveTemp(Out), &Released]() mutable {
			Owner.Reset(); Released.store(true, std::memory_order_release);
		});
		Worker.Get();
		TestTrue(TEXT("worker payload release joined before accounting"), Released.load(std::memory_order_acquire));
		TestTrue(TEXT("worker actually frees complete cloned domain"), C.Roots.Lifetime->Probe.LiveBacking.load() < Before);
		TestEqual(TEXT("worker payload final owner releases snapshot slot"), C.Roots.Budget->SnapshotControllers.load(), int64(0));
		return b;
	});
	bAll &= WithStore(*this, TEXT("SnapshotIsolation: nested domain exceeds1MiB using supported individual requests"), [&](FCase& C) {
		bool b = true;
		for (uint64 Id = 2000; Id < 2080; ++Id)
		{
			auto E = Started(Id);
			E.Payload.Get<FVaRestTraceStarted>().Url = FString::ChrN(8192, TCHAR('a' + Id % 20));
			b &= Deliver(C, E);
		}
		const int64 BeforeBytes = C.Roots.Budget->UsedBytes.load();
		const int64 BeforeBacking = C.Roots.Lifetime->Probe.LiveBacking.load();
		auto Large = Take(C); if (!Large) return false;
		TestTrue(TEXT("complete measured snapshot domain exceeds a single request cap"), C.Roots.Budget->UsedBytes.load() - BeforeBytes > 1024 * 1024);
		TestTrue(TEXT("fresh mutable packet backing actually allocated"), C.Roots.Lifetime->Probe.LiveBacking.load() > BeforeBacking);
		const FString Frozen = Image(Large);
		C.Store.Clear();
		Visit(*this, Large, 2000, [&](const auto& R) { TestEqual(TEXT("large copied text survives source free"), R.Started.Text(EText::Url).Len(), 8192); });
		TestTrue(TEXT("complete multi-block snapshot survives Clear"), Image(Large) == Frozen);
		return b;
	}, 128);
	bAll &= WithStore(*this, TEXT("SnapshotIsolation: shared sealed labels are captured independently of client table"), [&](FCase& C) {
		bool b = Deliver(C, Client(TEXT("sealed-label"))); b &= Deliver(C, Started(3000)); b &= Deliver(C, Completed(3000));
		auto Old = Take(C); if (!Old) return false;
		const FString Frozen = Image(Old);
		b &= EventMutation(C, Client(TEXT("renamed-label")));
		Visit(*this, Old, 3000, [&](const auto& R) { TestTrue(TEXT("shared sealed control"), R.bSealed); TestEqual(TEXT("old shared reader keeps captured label"), FString(R.ClientLabel), FString(TEXT("sealed-label"))); });
		C.Store.Clear();
		TestTrue(TEXT("shared row plus copied context survives client table free"), Image(Old) == Frozen);
		return b;
	});
	bAll &= WithStore(*this, TEXT("SnapshotIsolation: deferred lifecycle precedence, stable borrows and observable raw refusal"), [&](FCase& C) {
		bool b = Deliver(C, Started(4000, true)); b &= Deliver(C, Completed(4000));
		b &= Deliver(C, Started(4001)); b &= Deliver(C, Completed(4001));
		b &= Deliver(C, Started(4002, true));
		auto Old = Take(C); if (!Old) return false;
		const FString Frozen = Image(Old);
		bool bOnce = false;
		TestTrue(TEXT("outer borrowed enumeration succeeds"), C.Store.ForEachRecord([&](const auto& R) {
			if (bOnce) return; bOnce = true;
			FSnapshotRecordImage Before; Before.Row(R);
			bool bNestedCallback = false;
			TestFalse(TEXT("nested borrowed enumeration explicitly refused"), C.Store.ForEachRecord([&](const auto&) { bNestedCallback = true; }));
			TestFalse(TEXT("refused nested enumeration never calls visitor"), bNestedCallback);
			TestTrue(TEXT("reentrant raw packet admitted upstream"), C.Queue.TryPush(Note(4002, 777)) == EPush::Accepted);
			auto Raw = C.Queue.TryPopMetadata();
			if (TestTrue(TEXT("reentrant raw packet really exists"), Raw.IsSet()))
				TestFalse(TEXT("reentrant owned ingestion is explicitly refused"), C.Store.Apply(MoveTemp(Raw.GetValue())));
			FVaRestTraceSnapshot Borrowed;
			Borrowed.InFlight.Add(Started(4999).Payload.Get<FVaRestTraceStarted>());
			TestFalse(TEXT("reentrant borrowed attach explicitly refused without retaining input"), C.Store.ApplyAttached(Borrowed, C.Policy, C.Roots.Pending.ToSharedRef(), C.Roots.Lifetime->Allocator));
			C.Store.BeginGeneration(2, 17); C.Store.BeginGeneration(3, 19);
			FStoreAccess::Stop(C.Store); C.Clock.Set(5);
			TestEqual(TEXT("reentrant expiry reports deferred zero immediate expirations"), FStoreAccess::Expire(C.Store), 0);
			TestTrue(TEXT("reentrant eligible removal accepts bounded deferred request"), FStoreAccess::Remove(C.Store, 4001));
			TestEqual(TEXT("generation request not applied during borrowed callback"), FStoreAccess::Generation(C.Store), uint64(1));
			FSnapshotRecordImage After; After.Row(R);
			TestEqual(TEXT("all borrowed fields survive queued lifecycle operations"), After.Text, Before.Text);
		}));
		TestTrue(TEXT("outer callback exercised"), bOnce);
		TestEqual(TEXT("last requested generation applied at unwind"), FStoreAccess::Generation(C.Store), uint64(3));
		TestEqual(TEXT("last requested consumed ordinal applied at unwind"), FStoreAccess::Ordinal(C.Store), uint64(19));
		TestEqual(TEXT("genuine deferred eligible removal applied"), CountRows(C.Store), 2);
		Visit(*this, C.Store, 4000, [&](const auto& R) { CheckRequest(*this, R, EDetail::CaptureStopped); TestTrue(TEXT("stop precedes due expiry"), R.bSealed); });
		Visit(*this, C.Store, 4002, [&](const auto& R) { CheckRequest(*this, R, EDetail::CaptureStopped); TestFalse(TEXT("deferred stop does not fabricate completion"), R.bTerminalKnown); });
		TestTrue(TEXT("snapshot survives all deferred lifecycle actions"), Image(Old) == Frozen);
		Old.Reset();
		bOnce = false;
		TestTrue(TEXT("Clear precedence borrowed enumeration succeeds"), C.Store.ForEachRecord([&](const auto& R) {
			if (bOnce) return; bOnce = true;
			FSnapshotRecordImage Before; Before.Row(R);
			C.Store.Clear(); C.Store.BeginGeneration(4, 30); FStoreAccess::Stop(C.Store);
			TestEqual(TEXT("Clear coalesces pending expiry"), FStoreAccess::Expire(C.Store), 0);
			TestTrue(TEXT("Clear coalesces accepted eligible removal"), FStoreAccess::Remove(C.Store, 4000));
			FSnapshotRecordImage After; After.Row(R); TestEqual(TEXT("Clear preserves current borrow until return"), After.Text, Before.Text);
		}));
		TestEqual(TEXT("deferred Clear empties real store"), CountRows(C.Store), 0);
		TestEqual(TEXT("generation survives dominating Clear"), FStoreAccess::Generation(C.Store), uint64(4));
		TestEqual(TEXT("generation ordinal survives dominating Clear"), FStoreAccess::Ordinal(C.Store), uint64(30));
		FCaptureQueue Current(C.Policy, 4, C.Roots.Pending.ToSharedRef(), C.Roots.Lifetime->Allocator);
		ON_SCOPE_EXIT { Current.Close(); while (Current.TryPopMetadata().IsSet()) {} while (Current.TryPopExcerpt().IsSet()) {} };
		if (!TestTrue(TEXT("current generation queue admitted"), Current.IsSupported())) return false;
		TestTrue(TEXT("new current-generation packet admitted"), Current.TryPush(Started(4003, true)) == EPush::Accepted);
		auto Fresh = Current.TryPopMetadata();
		if (!TestTrue(TEXT("new current-generation packet exists"), Fresh.IsSet())) return false;
		TestTrue(TEXT("new current-generation packet applied"), C.Store.Apply(MoveTemp(Fresh.GetValue())));
		Visit(*this, C.Store, 4003, [&](const auto& R) { CheckRequest(*this, R, EDetail::Pending); TestTrue(TEXT("Clear discarded obsolete stop/remove actions"), R.bPinned && !R.bSealed); });
		return b;
	});
	bAll &= WithStore(*this, TEXT("SnapshotIsolation: expiry alone is deferred until borrowed callback returns"), [&](FCase& C) {
		bool b = Deliver(C, Started(4500, true)); b &= Deliver(C, Completed(4500));
		auto Before = Take(C); if (!Before) return false;
		const FString Frozen = Image(Before);
		C.Clock.Set(5);
		TestTrue(TEXT("expiry-only outer enumeration succeeds"), C.Store.ForEachRecord([&](const auto& R) {
			TestEqual(TEXT("expiry-only request accepted with zero immediate work"), FStoreAccess::Expire(C.Store), 0);
			CheckRequest(*this, R, EDetail::Pending);
			TestFalse(TEXT("expiry-only request cannot invalidate current borrow"), R.bSealed);
		}));
		Visit(*this, C.Store, 4500, [&](const auto& R) { CheckRequest(*this, R, EDetail::TimedOut); TestTrue(TEXT("expiry flag performs real work at unwind"), R.bSealed); });
		TestTrue(TEXT("expiry-only unwind leaves older snapshot intact"), Image(Before) == Frozen);
		return b;
	});
	bAll &= WithStore(*this, TEXT("SnapshotIsolation: old generation, malformed identity and joined excerpt publisher"), [&](FCase& C) {
		bool b = Deliver(C, Started(5000, true)); b &= Deliver(C, Completed(5000));
		auto Old = Take(C); if (!Old) return false;
		const FString Frozen = Image(Old);
		C.Store.BeginGeneration(2, 0);
		b &= Deliver(C, Excerpt(5000), EPush::Accepted, false);
		Visit(*this, C.Store, 5000, [&](const auto& R) { CheckRequest(*this, R, EDetail::Pending); TestFalse(TEXT("old-generation body does not resolve current promise"), R.bSealed); });
		C.Store.Clear();
		b &= Deliver(C, Excerpt(5000), EPush::Accepted, false);
		TestEqual(TEXT("old-generation body after Clear cannot create orphan"), CountRows(C.Store), 0);
		TestTrue(TEXT("snapshot survives generation rotation and Clear"), Image(Old) == Frozen);
		Old.Reset(); C.Store.BeginGeneration(1, 0);
		auto BadRequest = Excerpt(5001, 88); BadRequest.Payload.Get<FVaRestTraceExcerptEvent>().Owner = EVaRestTraceExcerptOwner::Request;
		auto BadResponse = Excerpt(5002); BadResponse.Payload.Get<FVaRestTraceExcerptEvent>().Owner = EVaRestTraceExcerptOwner::Response;
		const uint64 Violations = C.Store.Diagnostics().ContractViolations;
		b &= Deliver(C, BadRequest, EPush::Accepted, false); b &= Deliver(C, BadResponse, EPush::Accepted, false);
		TestEqual(TEXT("owner/physical mismatch cannot admit base"), CountRows(C.Store), 0);
		TestEqual(TEXT("both malformed identities diagnosed"), C.Store.Diagnostics().ContractViolations, Violations + 2);
		b &= Deliver(C, Started(5003, true)); b &= Deliver(C, Completed(5003));
		auto Worker = Async(EAsyncExecution::Thread, [&Queue = C.Queue, Event = Excerpt(5003, 0, 'j')] {
			return Queue.TryPush(Event);
		});
		const EPush Published = Worker.Get(); // No queue wrapper may die before join.
		TestTrue(TEXT("joined background excerpt publisher admitted"), Published == EPush::Accepted);
		auto Packet = C.Queue.TryPopExcerpt();
		if (!TestTrue(TEXT("joined worker excerpt dequeued"), Packet.IsSet())) return false;
		b &= TestTrue(TEXT("joined worker excerpt applied on GT"), C.Store.Apply(MoveTemp(Packet.GetValue())));
		Visit(*this, C.Store, 5003, [&](const auto& R) { CheckRequest(*this, R, EDetail::Present, 'j'); TestTrue(TEXT("worker body completes promise"), R.bSealed); });
		TestEqual(TEXT("joined publisher has no excerpt reservation"), C.Queue.OutstandingExcerptReservations(), int64(0));
		return b;
	});
	return bAll;
}

#endif // WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR
