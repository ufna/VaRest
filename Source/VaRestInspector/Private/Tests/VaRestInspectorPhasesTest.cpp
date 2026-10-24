// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspector.h"

#if WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "HAL/UnrealMemory.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/AutomationTest.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Templates/SharedPointerInternals.h"
#include "VaRestClock.h"
#include "VaRestInspectorAllocation.h"
#include "VaRestInspectorBudget.h"
#include "VaRestInspectorPacket.h"
#include "VaRestInspectorPhases.h"
#include "VaRestInspectorPolicy.h"
#include "VaRestInspectorQueue.h"
#include "VaRestInspectorRecords.h"
#include "VaRestInspectorStore.h"
#include "VaRestTrace.h"

#include <atomic>
#include <limits>

namespace VaRest::Inspector::Private::Testing
{
// An ordinary external symbol in QueueTest, independent of unity ordering.
// This reads the actual profile without calibration or Inspector allocation.
bool ReadLiveProfile(FAutomationTestBase& Test);
}

namespace VaRest::Inspector::Private::PhaseTesting
{
// Fixture exclusion only: one context, one pending root controller, one budget
// root, plus ONE transient calibration allowance. Task11 owns production roots.
// Source events/headers/byte arrays, clock and framework ownership are upstream.
class FPhaseFixtureRoots
{
public:
	explicit FPhaseFixtureRoots(FAutomationTestBase& InTest) : Test(InTest) {}
	~FPhaseFixtureRoots()
	{
		if (Pending)
		{
			Test.TestEqual(TEXT("phase fixture pending count drained"), Pending->Count.load(), int64(0));
			Test.TestEqual(TEXT("phase fixture pending bytes drained"), Pending->Bytes.load(), int64(0));
			Test.TestEqual(TEXT("phase fixture pending root exclusive"), Pending.GetSharedReferenceCount(), 1);
			Pending.Reset();
			StartupUsed.fetch_sub(4096);
		}
		if (Budget)
		{
			Test.TestEqual(TEXT("phase fixture budget credits drained"), Budget->UsedBytes.load(), Budget->BootstrapBytes);
			Test.TestEqual(TEXT("phase fixture controllers drained"), Budget->Probe.LiveControllers.load(), int64(0));
			Test.TestEqual(TEXT("phase fixture budget root exclusive"), int32(Budget->GetRefCount()), 1);
			Budget.SafeRelease();
			StartupUsed.fetch_sub(4096);
		}
		if (Lifetime)
		{
			Test.TestEqual(TEXT("phase fixture actual backing freed"), Lifetime->Probe.LiveBacking.load(), int64(0));
			Test.TestEqual(TEXT("phase fixture context exclusive after accounts"), int32(Lifetime->GetRefCount()), 1);
			Lifetime.SafeRelease();
			StartupUsed.fetch_sub(4096);
		}
		Test.TestEqual(TEXT("phase fixture allowances returned after releases"), StartupUsed.load(), int64(0));
	}
	bool Initialize(const FCapturePolicy& Policy)
	{
		if (!Testing::ReadLiveProfile(Test) || !AdmitRoot(sizeof(FAllocationLifetime), alignof(FAllocationLifetime)))
			return false;
		Lifetime = new FAllocationLifetime(GetSupportedAllocator());
		const SIZE_T ContextCapacity = FMemory::GetAllocSize(Lifetime.GetReference());
		if (!Test.TestTrue(TEXT("actual context fits pre-admitted root"), ContextCapacity >= sizeof(FAllocationLifetime) && ContextCapacity <= 4096))
			return false;
		const int64 BeforeCalibration = StartupUsed.load();
		if (!Test.TestTrue(TEXT("actual row and header controller correspondence calibrated"), FStore::CalibrateRows(*Lifetime, StartupUsed, StartupLimit)))
			return false;
		using HeaderC = SharedPointerInternals::TReferenceControllerWithDeleter<FDispatchHeaderOwner, TInspectorDeleter<FDispatchHeaderOwner>, ESPMode::ThreadSafe>;
		constexpr uint32 HeaderOrdinaryAlignment = sizeof(HeaderC) <= 8 ? 8 : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
		Test.AddInfo(FString::Printf(TEXT("PhaseControllerCalibration request=%lld ordinary=%lld capacity=%lld headerRequest=%llu headerOrdinary=%u startupLimit=%lld"),
			Lifetime->Probe.CalibrationRequest.load(), Lifetime->Probe.CalibrationAlignment.load(), Lifetime->Probe.CalibrationCapacity.load(),
			uint64(sizeof(HeaderC)), HeaderOrdinaryAlignment, StartupLimit));
		Test.TestEqual(TEXT("retained calibration exact header request correspondence"), Lifetime->Probe.CalibrationRequest.load(), int64(sizeof(HeaderC)));
		Test.TestEqual(TEXT("retained calibration exact ordinary alignment correspondence"), Lifetime->Probe.CalibrationAlignment.load(), int64(HeaderOrdinaryAlignment));
		Test.TestTrue(TEXT("current concrete header calibration accepted"), HasInspectorControllerCalibration<FDispatchHeaderOwner>(*Lifetime));
		Test.TestEqual(TEXT("retained calibration returns scratch allowance"), StartupUsed.load(), BeforeCalibration);
		Test.TestEqual(TEXT("retained calibration leaves no backing"), Lifetime->Probe.LiveBacking.load(), int64(0));
		using PendingC = SharedPointerInternals::TIntrusiveReferenceController<FPendingAccount, ESPMode::ThreadSafe>;
		if (!AdmitRoot(sizeof(PendingC), alignof(PendingC)))
			return false;
		Pending = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(Policy.Limits.PendingEvents, Policy.Limits.PendingBytes);
		Pending->Lifetime = Lifetime;
		if (!AdmitRoot(sizeof(FBudgetAccount), alignof(FBudgetAccount)))
			return false;
		Budget = new FBudgetAccount(Policy.Limits.StoreBytes, 4096, Lifetime);
		const SIZE_T BudgetCapacity = FMemory::GetAllocSize(Budget.GetReference());
		return Test.TestTrue(TEXT("actual budget fits pre-admitted root"), BudgetCapacity >= sizeof(FBudgetAccount) && BudgetCapacity <= 4096);
	}
	FAllocationLifetime& Context() const { return *Lifetime; }
	TRefCountPtr<FBudgetAccount> Account() const { return Budget; }
	TSharedRef<FPendingAccount, ESPMode::ThreadSafe> PendingAccount() const { return Pending.ToSharedRef(); }

private:
	bool AdmitRoot(SIZE_T Request, uint32 TypeAlignment)
	{
		if (!Test.TestTrue(TEXT("phase root exact audited shape"), Request > 0 && Request <= 1024 && TypeAlignment > 0 && TypeAlignment <= 16 && (TypeAlignment & (TypeAlignment - 1)) == 0))
			return false;
		if (!Test.TestTrue(TEXT("phase root admitted before allocation"), TryReserve(StartupUsed, StartupLimit, 4096)))
			return false;
		FAllocationCounters ScratchProbe;
		FAllocationAdapter Scratch(ScratchProbe, GetSupportedAllocator());
		const int64 Before = StartupUsed.load();
		const bool bReady = Scratch.Calibrate(StartupUsed, StartupLimit, Request, TypeAlignment);
		const int64 OrdinaryAlignment = Request <= 8 ? 8 : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
		Test.AddInfo(FString::Printf(TEXT("PhaseRootCalibration request=%lld ordinary=%lld capacity=%lld startupUsed=%lld startupLimit=%lld"),
			ScratchProbe.CalibrationRequest.load(), ScratchProbe.CalibrationAlignment.load(), ScratchProbe.CalibrationCapacity.load(), StartupUsed.load(), StartupLimit));
		Test.TestEqual(TEXT("phase root same actual request calibration"), ScratchProbe.CalibrationRequest.load(), int64(Request));
		Test.TestEqual(TEXT("phase root effective ordinary alignment"), ScratchProbe.CalibrationAlignment.load(), OrdinaryAlignment);
		Test.TestEqual(TEXT("phase root scratch allowance returned"), StartupUsed.load(), Before);
		Test.TestEqual(TEXT("phase root scratch actually freed"), ScratchProbe.LiveBacking.load(), int64(0));
		if (!Test.TestTrue(TEXT("phase root real calibration succeeded"), bReady))
		{
			StartupUsed.fetch_sub(4096);
			return false;
		}
		return true;
	}
	static constexpr int64 StartupLimit = 4 * 4096;
	FAutomationTestBase& Test;
	std::atomic<int64> StartupUsed{0};
	TRefCountPtr<FAllocationLifetime> Lifetime;
	TSharedPtr<FPendingAccount, ESPMode::ThreadSafe> Pending;
	TRefCountPtr<FBudgetAccount> Budget;
};

static bool WithStore(FAutomationTestBase& Test, const FCapturePolicy& Policy,
	TFunctionRef<bool(FStore&, FCaptureQueue&, FPhaseFixtureRoots&)> Body)
{
	FPhaseFixtureRoots Roots(Test);
	if (!Roots.Initialize(Policy))
		return false;
	auto Clock = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
	Clock->Advance(1000000); // Deliberately unrelated observer clock.
	FStore Store(Policy.Limits, Clock);
	if (!Test.TestTrue(TEXT("phase store initialized"), Store.Initialize(Roots.Account())))
		return false;
	Store.BeginGeneration(1, 0);
	const int64 FixedCharge = Roots.Account()->UsedBytes.load();
	const int64 FixedBacking = Roots.Context().Probe.LiveBacking.load();
	bool bResult = false;
	{
		FCaptureQueue Queue(Policy, 1, Roots.PendingAccount(), Roots.Context().Allocator);
		if (!Test.TestTrue(TEXT("phase queue initialized"), Queue.IsSupported()))
			return false;
		bResult = Body(Store, Queue, Roots);
		Queue.Close();
	}
	Store.Clear();
	Test.TestEqual(TEXT("phase Clear returns store fixed charge"), Roots.Account()->UsedBytes.load(), FixedCharge);
	Test.TestEqual(TEXT("phase Clear and queue teardown free actual backing"), Roots.Context().Probe.LiveBacking.load(), FixedBacking);
	Test.TestEqual(TEXT("phase queue baseline released"), Roots.PendingAccount()->Bytes.load(), int64(0));
	Test.TestTrue(TEXT("phase store peak bounded"), Roots.Account()->PeakBytes.load() <= Roots.Account()->Limit);
	Test.TestTrue(TEXT("phase pending peak bounded"), Roots.PendingAccount()->PeakBytes.load() <= Roots.PendingAccount()->ByteLimit);
	return bResult;
}

template <typename T>
static FVaRestTraceEvent Event(EVaRestTraceKind Kind, double Time, uint64 Seq, T&& Payload)
{
	FVaRestTraceEvent Result;
	Result.Kind = Kind;
	Result.Source = EVaRestTraceSource::Http;
	Result.Time = Time;
	Result.Seq = Seq;
	Result.Payload = FVaRestTracePayload(TInPlaceType<T>(), MoveTemp(Payload));
	return Result;
}
static FVaRestTraceEvent Started(uint64 Id, double Time = 10)
{
	FVaRestTraceStarted Value;
	Value.RequestId = Id;
	Value.ClientId = 9;
	Value.StartedAt = Time;
	Value.Verb = TEXT("GET");
	Value.Url = TEXT("https://example.test/phases");
	return Event(EVaRestTraceKind::Started, Time, 1, MoveTemp(Value));
}
struct FDispatchInput
{
	uint64 Id = 1;
	int32 Attempt = 1;
	double AttemptStart = 12, At = 16, Auth = 1, Queued = 2;
	double Upload = 17, Status = 20, Done = 22;
	EVaRestTraceGap Gap = EVaRestTraceGap::None;
	double GapSeconds = 0;
	int32 StatusCode = 200;
	int64 BytesUp = 10, BytesDown = 30;
	EVaRestTransportFailure Failure = EVaRestTransportFailure::None;
};
static FVaRestTraceEvent Dispatched(uint64 RequestId, const FDispatchInput& Input, int32 HeaderVariant = 0)
{
	FVaRestTraceAttemptDispatched Value;
	Value.RequestId = RequestId;
	Value.DispatchId = Input.Id;
	Value.Attempt = Input.Attempt;
	Value.AttemptStart = Input.AttemptStart;
	Value.AuthWait = Input.Auth;
	Value.AuthRefreshId = Input.Auth > 0 || Input.Gap == EVaRestTraceGap::SessionRefresh ? 902 : 0;
	Value.LimiterWait = Input.Queued;
	Value.GapKind = Input.Gap;
	Value.GapSeconds = Input.GapSeconds;
	if (HeaderVariant != 3)
	{
		if (HeaderVariant == 1)
		{
			Value.Headers.Set(TEXT("X-Two"), TEXT("visible-two"));
			Value.Headers.Set(TEXT("X-One"), TEXT("visible-one"));
		}
		else
		{
			Value.Headers.Set(TEXT("X-One"), HeaderVariant == 2 ? TEXT("changed-one") : TEXT("visible-one"));
			Value.Headers.Set(TEXT("X-Two"), TEXT("visible-two"));
		}
	}
	return Event(EVaRestTraceKind::AttemptDispatched, Input.At, 10 + Input.Id, MoveTemp(Value));
}
static FVaRestTraceEvent Ended(uint64 RequestId, const FDispatchInput& Input)
{
	FVaRestTraceAttemptEnded Value;
	Value.RequestId = RequestId;
	Value.DispatchId = Input.Id;
	Value.Attempt = Input.Attempt;
	Value.Status = Input.StatusCode;
	Value.BytesUp = Input.BytesUp;
	Value.BytesDown = Input.BytesDown;
	Value.Failure = Input.Failure;
	Value.UploadDoneAt = Input.Upload;
	Value.StatusAt = Input.Status;
	Value.TransportDoneAt = Input.Done;
	Value.EffectiveUrl = TEXT("https://example.test/wire");
	Value.ContentType = TEXT("application/json");
	Value.bExcerptPending = Input.Failure != EVaRestTransportFailure::Canceled;
	return Event(EVaRestTraceKind::AttemptEnded, Input.Done >= 0 ? Input.Done : Input.At, 100 + Input.Id, MoveTemp(Value));
}
static FVaRestTraceEvent Completed(uint64 Id, int32 Status = 200, double At = 25, double DecodeStart = 23, double DecodeEnd = 24,
	EVaRestTraceOutcome Outcome = EVaRestTraceOutcome::Success)
{
	FVaRestTraceCompleted Value;
	Value.RequestId = Id;
	Value.Outcome = Outcome;
	Value.CancelReason = Outcome == EVaRestTraceOutcome::Canceled ? EVaRestCancelReason::User : EVaRestCancelReason::None;
	Value.Response.Status = Status;
	Value.Response.BodyBytes = 4321;
	Value.Response.Storage = EVaRestTraceResponseStorage::Memory;
	Value.Response.EffectiveUrl = TEXT("https://example.test/delivered");
	Value.Response.ContentType = TEXT("application/final");
	Value.Error.HttpStatus = 599; // Deliberately different from delivered status.
	Value.DecodeStartAt = DecodeStart;
	Value.DecodeEndAt = DecodeEnd;
	return Event(EVaRestTraceKind::Completed, At, 500, MoveTemp(Value));
}
static TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe> Bytes(uint8 Value, int32 Capacity = 4096)
{
	auto Result = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
	Result->Reserve(Capacity);
	Result->Add(Value);
	Result->Add(uint8('!'));
	return Result;
}
static FVaRestTraceEvent Excerpt(uint64 RequestId, uint64 DispatchId, int32 Attempt,
	const TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe>& Body, EVaRestTraceExcerptOwner Owner = EVaRestTraceExcerptOwner::Response)
{
	FVaRestTraceExcerptEvent Value;
	Value.RequestId = RequestId;
	Value.DispatchId = DispatchId;
	Value.Attempt = Attempt;
	Value.Owner = Owner;
	Value.Excerpt.Bytes = Body;
	Value.Excerpt.TotalBytes = Body->Num();
	Value.Excerpt.Kind = EVaRestExcerptKind::Text;
	return Event(EVaRestTraceKind::Excerpt, 24, 300 + DispatchId, MoveTemp(Value));
}
static bool PushApply(FAutomationTestBase& Test, FCaptureQueue& Queue, FStore& Store, const FVaRestTraceEvent& Source,
	EPush Expected = EPush::Accepted, bool bExpectApply = true)
{
	if (!Test.TestTrue(TEXT("phase source packet admission"), Queue.TryPush(Source) == Expected))
		return false;
	auto Packet = Source.Kind == EVaRestTraceKind::Excerpt ? Queue.TryPopExcerpt() : Queue.TryPopMetadata();
	if (!Test.TestTrue(TEXT("phase actual packet dequeued"), Packet.IsSet()))
		return false;
	return Test.TestEqual(TEXT("phase packet Apply result"), Store.Apply(MoveTemp(Packet.GetValue())), bExpectApply);
}
static const FVaRestInspectorDispatchView* FindDispatch(const FVaRestInspectorRecordView& Row, uint64 Id)
{
	for (const auto& Dispatch : Row.Dispatches)
		if (Dispatch.DispatchId == Id)
			return &Dispatch;
	return nullptr;
}
static bool HasBody(const FVaRestInspectorDispatchView* Dispatch, uint64 Id, uint8 FirstByte)
{
	return Dispatch && Dispatch->ResponseExcerpt.Fixed && Dispatch->ResponseExcerpt.Fixed->Scalars.DispatchId == Id &&
		Dispatch->ResponseExcerpt.Fixed->Scalars.Owner == EVaRestTraceExcerptOwner::Response &&
		Dispatch->ResponseExcerpt.Excerpt.Num() == 2 && Dispatch->ResponseExcerpt.Excerpt[0] == FirstByte && Dispatch->ResponseExcerpt.Excerpt[1] == uint8('!');
}
static void CheckPhase(FAutomationTestBase& Test, const FString& Label, const FVaRestInspectorPhase& Actual, double Expected)
{
	Test.TestEqual(Label + TEXT(" known"), Actual.bKnown, Expected >= 0);
	if (Expected >= 0)
		Test.TestTrue(Label + TEXT(" seconds"), FMath::IsNearlyEqual(Actual.Seconds, Expected, 1.e-9));
}
struct FExpectedDispatch
{
	FDispatchInput Input;
	bool bStart = true, bEnd = true, bCombined = false;
	// Auth, Queued, Prepare, Send, Wait, Receive, Wall, Gap. -1 means unknown.
	double Seconds[8] = {1, 2, 1, 1, 3, 2, 10, 0};
};
static void CheckDispatch(FAutomationTestBase& Test, const FString& Prefix, const FVaRestInspectorDispatchView& Actual, const FExpectedDispatch& Expected)
{
	Test.TestEqual(Prefix + TEXT(" DispatchId"), Actual.DispatchId, Expected.Input.Id);
	Test.TestEqual(Prefix + TEXT(" source Attempt"), Actual.Attempt, Expected.Input.Attempt);
	Test.TestEqual(Prefix + TEXT(" start presence"), Actual.bHasDispatched, Expected.bStart);
	Test.TestEqual(Prefix + TEXT(" end presence"), Actual.bHasEnded, Expected.bEnd);
	Test.TestEqual(Prefix + TEXT(" combined Send/Wait label"), Actual.bCombinedSendWait, Expected.bCombined);
	const FVaRestInspectorPhase Phases[] = {Actual.Auth, Actual.Queued, Actual.Prepare, Actual.Send, Actual.Wait, Actual.Receive, Actual.Wall, Actual.Gap};
	const TCHAR* Names[] = {TEXT("Auth"), TEXT("Queued"), TEXT("Prepare"), TEXT("Send"), TEXT("Wait"), TEXT("Receive"), TEXT("Wall"), TEXT("Gap")};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Phases); ++Index)
		CheckPhase(Test, Prefix + Names[Index], Phases[Index], Expected.Seconds[Index]);
	if (Expected.bStart)
	{
		Test.TestEqual(Prefix + TEXT(" raw AttemptStart"), Actual.AttemptStart, Expected.Input.AttemptStart);
		Test.TestEqual(Prefix + TEXT(" raw DispatchAt"), Actual.DispatchAt, Expected.Input.At);
		Test.TestTrue(Prefix + TEXT(" raw gap kind"), Actual.GapKind == Expected.Input.Gap);
		Test.TestEqual(Prefix + TEXT(" raw gap seconds"), Actual.GapSeconds, Expected.Input.GapSeconds);
		if (Test.TestNotNull(Prefix + TEXT(" current dispatched fixed"), Actual.Dispatched.Fixed))
		{
			Test.TestEqual(Prefix + TEXT(" composed current id"), Actual.Dispatched.Fixed->Scalars.DispatchId, Expected.Input.Id);
			Test.TestEqual(Prefix + TEXT(" composed current Seq"), Actual.Dispatched.Fixed->Header.Seq, uint64(10) + Expected.Input.Id);
			Test.TestEqual(Prefix + TEXT(" composed current time"), Actual.Dispatched.Fixed->Header.Time, Expected.Input.At);
			Test.TestEqual(Prefix + TEXT(" captured auth wait"), Actual.Dispatched.Fixed->Scalars.AuthWait, Expected.Input.Auth);
			Test.TestEqual(Prefix + TEXT(" captured limiter wait"), Actual.Dispatched.Fixed->Scalars.LimiterWait, Expected.Input.Queued);
			Test.TestEqual(Prefix + TEXT(" captured refresh identity"), Actual.Dispatched.Fixed->Scalars.AuthRefreshId,
				uint64(Expected.Input.Auth > 0 || Expected.Input.Gap == EVaRestTraceGap::SessionRefresh ? 902 : 0));
		}
	}
	if (Expected.bEnd)
	{
		Test.TestEqual(Prefix + TEXT(" raw upload"), Actual.UploadDoneAt, Expected.Input.Upload);
		Test.TestEqual(Prefix + TEXT(" raw status"), Actual.StatusAt, Expected.Input.Status);
		Test.TestEqual(Prefix + TEXT(" raw transport done"), Actual.TransportDoneAt, Expected.Input.Done);
		if (Test.TestNotNull(Prefix + TEXT(" ended fixed"), Actual.Ended.Fixed))
		{
			Test.TestEqual(Prefix + TEXT(" physical status"), Actual.Ended.Fixed->Scalars.Status, Expected.Input.StatusCode);
			Test.TestEqual(Prefix + TEXT(" physical end id"), Actual.Ended.Fixed->Scalars.DispatchId, Expected.Input.Id);
			Test.TestTrue(Prefix + TEXT(" physical failure retained"), Actual.Ended.Fixed->Scalars.Failure == Expected.Input.Failure);
		}
	}
	if (Expected.Seconds[0] >= 0 && Expected.Seconds[1] >= 0 && Expected.Seconds[6] >= 0 && Expected.Seconds[2] >= 0 && Expected.Seconds[5] >= 0 &&
		Expected.Seconds[4] >= 0 && (Expected.bCombined || Expected.Seconds[3] >= 0))
	{
		const double Sum = Actual.Auth.Seconds + Actual.Queued.Seconds + Actual.Prepare.Seconds +
			(Expected.bCombined ? 0 : Actual.Send.Seconds) + Actual.Wait.Seconds + Actual.Receive.Seconds;
		Test.TestTrue(Prefix + TEXT(" complete attempt component sum"), FMath::IsNearlyEqual(Sum, Actual.Wall.Seconds, 1.e-9));
	}
}

enum class ECase : uint8
{
	Success, Retry, AuthReplay, AuthPeer, DuplicateForward, DuplicateReverse,
	ParkedCancel, SyntheticCancel, UnknownUpload, UnknownStatus, LateAttach, EqualTimes
};
struct FTableCase { const TCHAR* Name; ECase Kind; };
static bool RunCase(FAutomationTestBase& Test, const FTableCase& Case)
{
	FCapturePolicy Policy;
	Policy.Limits.MaxRecords = 4;
	return WithStore(Test, Policy, [&](FStore& Store, FCaptureQueue& Queue, FPhaseFixtureRoots& Roots) {
		const FString Prefix = FString(Case.Name) + TEXT(" / ");
		Test.AddInfo(Prefix);
		constexpr uint64 RequestId = 41;
		const bool bDuplicate = Case.Kind == ECase::DuplicateForward || Case.Kind == ECase::DuplicateReverse;
		FExpectedDispatch Expected[2];
		int32 ExpectedCount = 1;
		uint64 ExpectedStarts = 1, ExpectedEnds = 1;
		uint64 ExpectedUp = 10, ExpectedDown = 30;
		double CompletionTime = 25, DecodeStart = 23, DecodeEnd = 24;
		int32 FinalStatus = 200;
		EVaRestTraceOutcome Outcome = EVaRestTraceOutcome::Success;
		if (Case.Kind == ECase::LateAttach)
		{
			FVaRestTraceSnapshot Snapshot;
			FVaRestTraceStarted& Attached = Snapshot.InFlight.AddDefaulted_GetRef();
			Attached.RequestId = RequestId;
			Attached.CurrentAttempt = 3;
			Attached.StartedAt = 10;
			Store.ApplyAttached(Snapshot, Policy, Roots.PendingAccount(), Roots.Context().Allocator);
			Expected[0].Input.Attempt = 3;
			Expected[0].bStart = false;
			ExpectedStarts = 0;
			const double UnknownStart[] = {-1, -1, -1, -1, 3, 2, -1, -1};
			for (int32 Index = 0; Index < 8; ++Index) Expected[0].Seconds[Index] = UnknownStart[Index];
			Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
				Test.TestEqual(Prefix + TEXT("snapshot CurrentAttempt"), Row.CurrentAttempt, 3);
				Test.TestEqual(Prefix + TEXT("snapshot creates no dispatch history"), Row.Dispatches.Num(), 0);
				Test.TestFalse(Prefix + TEXT("unknown historical UTC"), Row.bStartUtcKnown);
			});
		}
		else if (!PushApply(Test, Queue, Store, Started(RequestId)))
			return false;
		if (Case.Kind == ECase::Retry || Case.Kind == ECase::AuthReplay)
		{
			ExpectedCount = 2;
			ExpectedStarts = ExpectedEnds = 2;
			ExpectedUp = 30; ExpectedDown = 70;
			Expected[0].Input = {1, 1, 10, 11, 0, 0, 12, 13, 14, EVaRestTraceGap::None, 0,
				Case.Kind == ECase::Retry ? 503 : 401, 10, 30};
			const double First[] = {0, 0, 1, 1, 1, 1, 4, 0};
			for (int32 Index = 0; Index < 8; ++Index) Expected[0].Seconds[Index] = First[Index];
			if (Case.Kind == ECase::Retry)
				Expected[1].Input = {2, 2, 17, 19, 0, 1, 20, 21, 22, EVaRestTraceGap::Backoff, 3, 200, 20, 40};
			else
			{
				Expected[1].Input = {2, 2, 18, 20, 0, 1, 21, 22, 23, EVaRestTraceGap::SessionRefresh, 4, 200, 20, 40};
				CompletionTime = 26; DecodeStart = 24; DecodeEnd = 25;
			}
			const double Second[] = {0, 1, 1, 1, 1, 1, 5, Case.Kind == ECase::Retry ? 3.0 : 4.0};
			for (int32 Index = 0; Index < 8; ++Index) Expected[1].Seconds[Index] = Second[Index];
		}
		if (Case.Kind == ECase::AuthPeer)
		{
			Expected[0].Input = {1, 1, 10, 16, 4, 1, 17, 18, 19, EVaRestTraceGap::None, 0, 200, 10, 30};
			const double Peer[] = {4, 1, 1, 1, 1, 1, 9, 0};
			for (int32 Index = 0; Index < 8; ++Index) Expected[0].Seconds[Index] = Peer[Index];
		}
		if (bDuplicate)
		{
			ExpectedCount = 2; ExpectedStarts = ExpectedEnds = 2;
			ExpectedUp = 30; ExpectedDown = 70;
			Expected[0].Input.StatusCode = 503;
			Expected[1].Input = {2, 1, 13, 17, 1, 1, 18, 21, 23, EVaRestTraceGap::None, 0, 200, 20, 40};
			const double Second[] = {1, 1, 2, 1, 3, 2, 10, 0};
			for (int32 Index = 0; Index < 8; ++Index) Expected[1].Seconds[Index] = Second[Index];
		}
		if (Case.Kind == ECase::ParkedCancel)
		{
			ExpectedCount = 0; ExpectedStarts = ExpectedEnds = ExpectedUp = ExpectedDown = 0;
			Outcome = EVaRestTraceOutcome::Canceled; FinalStatus = 0; DecodeStart = DecodeEnd = -1;
		}
		if (Case.Kind == ECase::SyntheticCancel)
		{
			Expected[0].Input = {1, 1, 12, 12, 0, 0, -1, -1, -1, EVaRestTraceGap::None, 0, 0, 0, 0};
			Expected[0].Input.Failure = EVaRestTransportFailure::Canceled;
			const double Cancel[] = {0, 0, 0, -1, -1, -1, -1, 0};
			for (int32 Index = 0; Index < 8; ++Index) Expected[0].Seconds[Index] = Cancel[Index];
			ExpectedUp = ExpectedDown = 0; CompletionTime = 12;
			Outcome = EVaRestTraceOutcome::Canceled; FinalStatus = 0; DecodeStart = DecodeEnd = -1;
		}
		if (Case.Kind == ECase::UnknownUpload)
		{
			Expected[0].Input.Upload = -1; Expected[0].bCombined = true;
			Expected[0].Seconds[3] = -1; Expected[0].Seconds[4] = 4;
		}
		if (Case.Kind == ECase::UnknownStatus)
		{
			Expected[0].Input.Status = Expected[0].Input.Done = -1;
			Expected[0].Seconds[4] = Expected[0].Seconds[5] = Expected[0].Seconds[6] = -1;
			DecodeStart = DecodeEnd = -1;
		}
		if (Case.Kind == ECase::EqualTimes)
		{
			Expected[0].Input = {1, 1, 10, 10, 0, 0, 10, 10, 10, EVaRestTraceGap::None, 0, 200, 10, 30};
			for (double& Seconds : Expected[0].Seconds) Seconds = 0;
			CompletionTime = DecodeStart = DecodeEnd = 10;
		}
		for (int32 Index = 0; Index < ExpectedCount; ++Index)
		{
			if (Expected[Index].bStart && !PushApply(Test, Queue, Store, Dispatched(RequestId, Expected[Index].Input))) return false;
			if (!bDuplicate && !PushApply(Test, Queue, Store, Ended(RequestId, Expected[Index].Input))) return false;
		}
		auto FirstBody = Bytes(uint8('A'));
		auto SecondBody = Bytes(uint8('B'), 8192);
		if (bDuplicate)
		{
			const int32 FirstEnd = Case.Kind == ECase::DuplicateReverse ? 1 : 0;
			if (!PushApply(Test, Queue, Store, Ended(RequestId, Expected[FirstEnd].Input)) ||
				!PushApply(Test, Queue, Store, Ended(RequestId, Expected[1 - FirstEnd].Input))) return false;
			// The first body is deliberately delayed behind the second body. Both
			// travel through actual accepted codec/queue/store ownership BEFORE Completed.
			if (!PushApply(Test, Queue, Store, Excerpt(RequestId, 2, 1, SecondBody)) ||
				!PushApply(Test, Queue, Store, Excerpt(RequestId, 1, 1, FirstBody))) return false;
			Test.TestEqual(Prefix + TEXT("first real source reference retained"), FirstBody.GetSharedReferenceCount(), 2);
			Test.TestEqual(Prefix + TEXT("second real source reference retained"), SecondBody.GetSharedReferenceCount(), 2);
		}
		auto Completion = Completed(RequestId, FinalStatus, CompletionTime, DecodeStart, DecodeEnd, Outcome);
		if (Case.Kind == ECase::ParkedCancel)
			Completion.Payload.Get<FVaRestTraceCompleted>().AuthRefreshId = 902;
		if (!PushApply(Test, Queue, Store, Completion)) return false;
		int32 Visits = 0;
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			++Visits;
			Test.TestEqual(Prefix + TEXT("physical slot pairing count"), Row.Dispatches.Num(), ExpectedCount);
			Test.TestEqual(Prefix + TEXT("observed physical starts"), Row.ObservedDispatches, ExpectedStarts);
			Test.TestEqual(Prefix + TEXT("observed physical ends"), Row.ObservedEnds, ExpectedEnds);
			Test.TestEqual(Prefix + TEXT("current source attempt"), Row.CurrentAttempt, ExpectedCount ? Expected[ExpectedCount - 1].Input.Attempt : 0);
			Test.TestEqual(Prefix + TEXT("both physical wire-up contributions"), Row.WireBytesUp, ExpectedUp);
			Test.TestEqual(Prefix + TEXT("both physical wire-down contributions"), Row.WireBytesDown, ExpectedDown);
			Test.TestEqual(Prefix + TEXT("delivered status independently selected"), Row.FinalStatus, FinalStatus);
			Test.TestEqual(Prefix + TEXT("delivered body size separate from wire"), Row.FinalBodyBytes, int64(4321));
			Test.TestTrue(Prefix + TEXT("delivered body storage"), Row.FinalStorage == EVaRestTraceResponseStorage::Memory);
			CheckPhase(Test, Prefix + TEXT("Total"), Row.Total, CompletionTime - 10);
			CheckPhase(Test, Prefix + TEXT("Decode"), Row.Decode, DecodeStart < 0 ? -1 : DecodeEnd - DecodeStart);
			CheckPhase(Test, Prefix + TEXT("Deliver"), Row.Deliver, DecodeEnd < 0 ? -1 : CompletionTime - DecodeEnd);
			if (Test.TestNotNull(Prefix + TEXT("real Completed metadata owner"), Row.Completed.Fixed))
			{
				Test.TestEqual(Prefix + TEXT("Completed.Response status"), Row.Completed.Fixed->Scalars.Status, FinalStatus);
				Test.TestEqual(Prefix + TEXT("error status remains distinct"), Row.Completed.Fixed->Scalars.ErrorHttpStatus, 599);
				Test.TestEqual(Prefix + TEXT("final URL is not old wire URL"), FString(Row.Completed.Text(EText::EffectiveUrl)), FString(TEXT("https://example.test/delivered")));
				if (Case.Kind == ECase::ParkedCancel)
					Test.TestEqual(Prefix + TEXT("unsent park retains real refresh identity"), Row.Completed.Fixed->Scalars.AuthRefreshId, uint64(902));
			}
			for (int32 Index = 0; Index < ExpectedCount; ++Index)
			{
				const auto* Found = FindDispatch(Row, Expected[Index].Input.Id);
				if (Test.TestNotNull(Prefix + FString::Printf(TEXT("pairing dispatch %llu"), Expected[Index].Input.Id), Found))
					CheckDispatch(Test, Prefix, *Found, Expected[Index]);
			}
			if (bDuplicate)
			{
				// Never early-return on the count/pairing failure: both body assertions
				// must discriminate the Attempt-matching mutant in BOTH end orders.
				Test.TestTrue(Prefix + TEXT("body ownership dispatch1 actual A bytes"), HasBody(FindDispatch(Row, 1), 1, uint8('A')));
				Test.TestTrue(Prefix + TEXT("body ownership dispatch2 actual B bytes"), HasBody(FindDispatch(Row, 2), 2, uint8('B')));
			}
		});
		Test.TestEqual(Prefix + TEXT("exactly one logical request"), Visits, 1);
		Store.Clear();
		Test.TestEqual(Prefix + TEXT("first body ref freed on Clear"), FirstBody.GetSharedReferenceCount(), 1);
		Test.TestEqual(Prefix + TEXT("second body ref freed on Clear"), SecondBody.GetSharedReferenceCount(), 1);
		return true;
	});
}

static bool CheckHeaders(FAutomationTestBase& Test)
{
	FCapturePolicy Policy;
	Policy.Limits.MaxRecords = 4;
	return WithStore(Test, Policy, [&](FStore& Store, FCaptureQueue& Queue, FPhaseFixtureRoots& Roots) {
		constexpr uint64 Id = 71;
		if (!PushApply(Test, Queue, Store, Started(Id))) return false;
		FDispatchInput Input;
		if (!PushApply(Test, Queue, Store, Dispatched(Id, Input))) return false;
		const int64 FirstCharge = Roots.Account()->UsedBytes.load();
		const int64 FirstBacking = Roots.Context().Probe.LiveBacking.load();
		const int64 FirstControllers = Roots.Account()->Probe.LiveControllers.load();
		const int64 PendingBaseline = Roots.PendingAccount()->Bytes.load();
		const auto SlotBound = Roots.Context().Allocator.Bound(sizeof(FDispatchSlot), alignof(FDispatchSlot));
		if (!Test.TestTrue(TEXT("exact independent slot bound"), SlotBound.IsSet())) return false;
		Input.Id = 2; Input.At = 17; Input.AttemptStart = 13;
		if (!PushApply(Test, Queue, Store, Dispatched(Id, Input))) return false;
		Test.TestEqual(TEXT("equal headers charge only independently admitted second slot"), Roots.Account()->UsedBytes.load(), FirstCharge + int64(SlotBound.GetValue()));
		Test.TestEqual(TEXT("equal headers create no second shared controller"), Roots.Account()->Probe.LiveControllers.load(), FirstControllers);
		Test.TestTrue(TEXT("equal headers actual backing grows only by slot storage"), Roots.Context().Probe.LiveBacking.load() > FirstBacking && Roots.Context().Probe.LiveBacking.load() <= FirstBacking + int64(SlotBound.GetValue()));
		Test.TestEqual(TEXT("incoming duplicate backing freed with pending ticket"), Roots.PendingAccount()->Bytes.load(), PendingBaseline);
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			const auto* First = FindDispatch(Row, 1);
			const auto* Second = FindDispatch(Row, 2);
			if (!Test.TestTrue(TEXT("two header-sharing slots exist"), First && Second && First->Dispatched.Fixed && Second->Dispatched.Fixed)) return;
			Test.TestTrue(TEXT("equal headers retain one actual backing"), First->Dispatched.Backing.GetData() == Second->Dispatched.Backing.GetData());
			Test.TestTrue(TEXT("equal headers remain positively captured"), First->HeadersState == EVaRestInspectorHeadersState::Captured && Second->HeadersState == EVaRestInspectorHeadersState::Captured);
			Test.TestEqual(TEXT("shared backing still has current second id"), Second->Dispatched.Fixed->Scalars.DispatchId, uint64(2));
			Test.TestEqual(TEXT("shared backing still has current second timestamp"), Second->Dispatched.Fixed->Header.Time, 17.0);
			Test.TestEqual(TEXT("shared backing still has current second AttemptStart"), Second->Dispatched.Fixed->Scalars.AttemptStart, 13.0);
			const auto Pairs = Second->Dispatched.Pairs(Second->Dispatched.Fixed->Headers);
			if (Test.TestEqual(TEXT("shared ordered header count"), Pairs.Num(), 2))
			{
				Test.TestEqual(TEXT("shared exact first key"), FString(Second->Dispatched.Read(Pairs[0].First)), FString(TEXT("X-One")));
				Test.TestEqual(TEXT("shared exact first value"), FString(Second->Dispatched.Read(Pairs[0].Second)), FString(TEXT("visible-one")));
			}
		});
		Input.Id = 3;
		if (!PushApply(Test, Queue, Store, Dispatched(Id, Input, 1))) return false;
		Input.Id = 4;
		if (!PushApply(Test, Queue, Store, Dispatched(Id, Input, 2))) return false;
		Test.TestEqual(TEXT("reordered and changed sets add two distinct controllers"), Roots.Account()->Probe.LiveControllers.load(), FirstControllers + 2);
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			const auto* A = FindDispatch(Row, 2); const auto* B = FindDispatch(Row, 3); const auto* C = FindDispatch(Row, 4);
			if (!Test.TestTrue(TEXT("distinct header controls retained"), A && B && C)) return;
			Test.TestTrue(TEXT("reordering is not header equality"), A->Dispatched.Backing.GetData() != B->Dispatched.Backing.GetData());
			Test.TestTrue(TEXT("value change is not header equality"), B->Dispatched.Backing.GetData() != C->Dispatched.Backing.GetData());
		});
		// Empty source and capture-disabled cannot be distinguished by this codec.
		// Both remain Unknown, and an intervening unknown observation breaks reuse.
		Input.Id = 5;
		if (!PushApply(Test, Queue, Store, Dispatched(Id, Input, 3))) return false;
		Input.Id = 6;
		if (!PushApply(Test, Queue, Store, Dispatched(Id, Input, 2))) return false;
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			const auto* Before = FindDispatch(Row, 4); const auto* Empty = FindDispatch(Row, 5); const auto* After = FindDispatch(Row, 6);
			if (!Test.TestTrue(TEXT("unknown-header barrier slots exist"), Before && Empty && After)) return;
			Test.TestTrue(TEXT("zero headers are unknown, never known-empty"), Empty->HeadersState == EVaRestInspectorHeadersState::Unknown);
			Test.TestEqual(TEXT("unknown headers expose no prior backing"), Empty->Dispatched.Backing.Num(), 0);
			Test.TestTrue(TEXT("unknown intervening observation breaks old-set reuse"), Before->Dispatched.Backing.GetData() != After->Dispatched.Backing.GetData());
		});
		// Admit the next slot but leave no credit for its changed header owner.
		const int64 Available = Roots.Account()->Limit - Roots.Account()->UsedBytes.load();
		auto Pressure = FStoreCredit::Acquire(Roots.Account(), Available - int64(SlotBound.GetValue()));
		if (!Test.TestTrue(TEXT("header-only pressure admitted with slot allowance"), Pressure.IsSet())) return false;
		const int64 ControllersBefore = Roots.Account()->Probe.LiveControllers.load();
		Input.Id = 7;
		if (!PushApply(Test, Queue, Store, Dispatched(Id, Input, 1))) return false;
		Test.TestEqual(TEXT("refused headers create no controller"), Roots.Account()->Probe.LiveControllers.load(), ControllersBefore);
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			const auto* Dispatch = FindDispatch(Row, 7);
			if (!Test.TestNotNull(TEXT("header refusal preserves admitted scalar slot"), Dispatch)) return;
			Test.TestTrue(TEXT("header refusal is explicit"), Dispatch->HeadersState == EVaRestInspectorHeadersState::MetadataOmitted);
			Test.TestEqual(TEXT("header refusal has no previous backing"), Dispatch->Dispatched.Backing.Num(), 0);
			CheckPhase(Test, TEXT("header refusal keeps Prepare"), Dispatch->Prepare, 1);
		});
		Pressure.Reset();
		return true;
	});
}

static bool CheckOwnerLifetime(FAutomationTestBase& Test)
{
	FCapturePolicy Policy;
	return WithStore(Test, Policy, [&](FStore&, FCaptureQueue& Queue, FPhaseFixtureRoots& Roots) {
		FDispatchInput Input;
		const int64 BaseCharge = Roots.Account()->UsedBytes.load();
		const int64 BaseBacking = Roots.Context().Probe.LiveBacking.load();
		const int64 BaseControllers = Roots.Account()->Probe.LiveControllers.load();
		const auto Source = Dispatched(81, Input);
		FPacketLayout Layout;
		if (!Test.TestTrue(TEXT("header lifetime actual layout measured"), FPacketCodec::Measure(Source, Policy, Roots.Context().Allocator, Layout) == EMeasure::Full)) return false;
		if (!Test.TestTrue(TEXT("header lifetime packet admitted"), Queue.TryPush(Source) == EPush::Accepted)) return false;
		auto Packet = Queue.TryPopMetadata();
		if (!Test.TestTrue(TEXT("header lifetime packet dequeued"), Packet.IsSet())) return false;
		TSharedPtr<const FDispatchHeaderOwner, ESPMode::ThreadSafe> Owner;
		FPacket Invalid;
		Test.TestFalse(TEXT("header factory invalid input refused"), FDispatchHeaderOwner::TryTake(Invalid, Roots.Account(), Roots.Context().Allocator, Owner));
		Test.TestFalse(TEXT("header factory null account refused"), FDispatchHeaderOwner::TryTake(Packet.GetValue(), {}, Roots.Context().Allocator, Owner));
		FAllocationCounters WrongProbe;
		FAllocationAdapter WrongAllocator(WrongProbe, GetSupportedAllocator());
		Test.TestFalse(TEXT("header factory wrong adapter refused"), FDispatchHeaderOwner::TryTake(Packet.GetValue(), Roots.Account(), WrongAllocator, Owner));
		const int64 PendingCharge = Roots.PendingAccount()->Bytes.load();
		const int64 PacketBacking = Roots.Context().Probe.LiveBacking.load();
		{
			auto Full = FStoreCredit::Acquire(Roots.Account(), Roots.Account()->Limit - Roots.Account()->UsedBytes.load());
			if (!Test.TestTrue(TEXT("header lifetime byte pressure"), Full.IsSet())) return false;
			Test.TestFalse(TEXT("header factory pre-admission refusal"), FDispatchHeaderOwner::TryTake(Packet.GetValue(), Roots.Account(), Roots.Context().Allocator, Owner));
			Test.TestTrue(TEXT("refused factory leaves input/output unchanged"), Packet->IsValid() && !Owner);
			Test.TestEqual(TEXT("refused factory preserves pending credit"), Roots.PendingAccount()->Bytes.load(), PendingCharge);
			Test.TestEqual(TEXT("refused factory allocates no backing"), Roots.Context().Probe.LiveBacking.load(), PacketBacking);
		}
		// Permit the empty shared candidate but refuse its packet transfer. The
		// unpublished candidate must unwind without consuming the source packet.
		const auto HeaderPayloadBound = Roots.Context().Allocator.Bound(sizeof(FDispatchHeaderOwner), alignof(FDispatchHeaderOwner));
		if (!Test.TestTrue(TEXT("exact shared header payload bound"), HeaderPayloadBound.IsSet())) return false;
		{
			const int64 CandidateCharge = 4096 + int64(HeaderPayloadBound.GetValue());
			auto Partial = FStoreCredit::Acquire(Roots.Account(), Roots.Account()->Limit - Roots.Account()->UsedBytes.load() - CandidateCharge);
			if (!Test.TestTrue(TEXT("unpublished header candidate pressure admitted"), Partial.IsSet())) return false;
			const int64 BeforeCandidate = Roots.Account()->UsedBytes.load();
			Test.TestFalse(TEXT("unpublished header candidate refuses packet transfer"), FDispatchHeaderOwner::TryTake(Packet.GetValue(), Roots.Account(), Roots.Context().Allocator, Owner));
			Test.TestTrue(TEXT("unpublished candidate leaves input and output unchanged"), Packet->IsValid() && !Owner);
			Test.TestEqual(TEXT("unpublished candidate releases all own credits"), Roots.Account()->UsedBytes.load(), BeforeCandidate);
			Test.TestEqual(TEXT("unpublished candidate actually frees payload"), Roots.Context().Probe.LiveBacking.load(), PacketBacking);
			Test.TestEqual(TEXT("unpublished candidate destroys controller"), Roots.Account()->Probe.LiveControllers.load(), BaseControllers);
			Test.TestEqual(TEXT("unpublished candidate preserves pending input"), Roots.PendingAccount()->Bytes.load(), PendingCharge);
		}
		const int64 BeforeOverlap = Roots.Context().Probe.OverlapObservations.load();
		if (!Test.TestTrue(TEXT("header factory real overlap transfer"), FDispatchHeaderOwner::TryTake(Packet.GetValue(), Roots.Account(), Roots.Context().Allocator, Owner))) return false;
		Test.TestFalse(TEXT("successful header transfer clears source packet"), Packet->IsValid());
		Test.TestEqual(TEXT("header transfer observed overlap"), Roots.Context().Probe.OverlapObservations.load(), BeforeOverlap + 1);
		auto Second = Owner;
		TWeakPtr<const FDispatchHeaderOwner, ESPMode::ThreadSafe> Weak = Owner;
		const int64 SharedCharge = Roots.Account()->UsedBytes.load();
		const int64 SharedBacking = Roots.Context().Probe.LiveBacking.load();
		Owner.Reset();
		Test.TestEqual(TEXT("original header reference release retains shared charge"), Roots.Account()->UsedBytes.load(), SharedCharge);
		Test.TestEqual(TEXT("original header reference release retains actual backing"), Roots.Context().Probe.LiveBacking.load(), SharedBacking);
		const FVaRestInspectorPacketView View = Second->View();
		if (Test.TestNotNull(TEXT("remaining header reference has actual fixed"), View.Fixed))
		{
			Test.TestEqual(TEXT("shared header view uses initialized extent"), View.Backing.Num(), int32(Layout.BackingRequest));
			const auto Pairs = View.Pairs(View.Fixed->Headers);
			if (Test.TestEqual(TEXT("remaining header reference has actual pairs"), Pairs.Num(), 2))
				Test.TestEqual(TEXT("remaining header bytes survive original reference"), FString(View.Read(Pairs[0].Second)), FString(TEXT("visible-one")));
		}
		Second.Reset();
		Test.TestFalse(TEXT("last strong header released"), Weak.Pin().IsValid());
		Test.TestEqual(TEXT("header payload and packet backing actually freed"), Roots.Context().Probe.LiveBacking.load(), BaseBacking);
		Test.TestEqual(TEXT("weak header retains only controller charge"), Roots.Account()->UsedBytes.load(), BaseCharge + 4096);
		Test.TestEqual(TEXT("weak header keeps stored deleter alive"), Roots.Account()->Probe.LiveControllers.load(), BaseControllers + 1);
		Weak.Reset();
		Test.TestEqual(TEXT("last weak header returns controller charge"), Roots.Account()->UsedBytes.load(), BaseCharge);
		Test.TestEqual(TEXT("last weak header destroys stored deleter"), Roots.Account()->Probe.LiveControllers.load(), BaseControllers);
		return true;
	});
}

static bool CheckLimitsAndScalars(FAutomationTestBase& Test)
{
	FCapturePolicy Policy;
	Policy.Limits.MaxRecords = 4;
	return WithStore(Test, Policy, [&](FStore& Store, FCaptureQueue& Queue, FPhaseFixtureRoots& Roots) {
		constexpr uint64 Id = 91;
		if (!PushApply(Test, Queue, Store, Started(Id))) return false;
		FDispatchInput Input;
		Input.BytesUp = 2; Input.BytesDown = 3;
		for (uint64 DispatchId = 1; DispatchId <= 17; ++DispatchId)
		{
			Input.Id = DispatchId;
			if (!PushApply(Test, Queue, Store, Dispatched(Id, Input)) || !PushApply(Test, Queue, Store, Ended(Id, Input))) return false;
		}
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			Test.TestEqual(TEXT("physical detail count cap"), Row.Dispatches.Num(), 16);
			Test.TestEqual(TEXT("start observations survive slot cap"), Row.ObservedDispatches, uint64(17));
			Test.TestEqual(TEXT("end observations survive slot cap"), Row.ObservedEnds, uint64(17));
			Test.TestEqual(TEXT("wire up survives slot cap"), Row.WireBytesUp, uint64(34));
			Test.TestEqual(TEXT("wire down survives slot cap"), Row.WireBytesDown, uint64(51));
			Test.TestEqual(TEXT("one refused physical start detail"), Row.OmittedDispatches, uint64(1));
			Test.TestEqual(TEXT("one refused physical end detail"), Row.OmittedEnds, uint64(1));
		});
		Test.TestTrue(TEXT("slot count pressure has byte slack"), Roots.Account()->UsedBytes.load() < Roots.Account()->Limit / 2);
		// A duplicate of a RETAINED id does not double count a physical end.
		Input.Id = 1;
		if (!PushApply(Test, Queue, Store, Ended(Id, Input))) return false;
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			Test.TestEqual(TEXT("retained duplicate end is not counted twice"), Row.ObservedEnds, uint64(17));
			Test.TestEqual(TEXT("retained duplicate wire bytes not counted twice"), Row.WireBytesDown, uint64(51));
		});
		Store.Clear();
		if (!PushApply(Test, Queue, Store, Started(Id))) return false;
		auto Full = FStoreCredit::Acquire(Roots.Account(), Roots.Account()->Limit - Roots.Account()->UsedBytes.load());
		if (!Test.TestTrue(TEXT("slot byte pressure independent of count"), Full.IsSet())) return false;
		const int64 BeforeBacking = Roots.Context().Probe.LiveBacking.load();
		if (!PushApply(Test, Queue, Store, Dispatched(Id, Input)) || !PushApply(Test, Queue, Store, Ended(Id, Input))) return false;
		Test.TestEqual(TEXT("refused slots allocate no surviving backing"), Roots.Context().Probe.LiveBacking.load(), BeforeBacking);
		if (!PushApply(Test, Queue, Store, Completed(Id, 207))) return false;
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			Test.TestEqual(TEXT("slot bytes refuse with count slack"), Row.Dispatches.Num(), 0);
			Test.TestEqual(TEXT("refused slot preserves physical start"), Row.ObservedDispatches, uint64(1));
			Test.TestEqual(TEXT("refused slot preserves physical end"), Row.ObservedEnds, uint64(1));
			Test.TestEqual(TEXT("refused slot preserves wire bytes"), Row.WireBytesDown, uint64(3));
			Test.TestEqual(TEXT("Completed scalar survives optional-owner refusal"), Row.FinalStatus, 207);
			Test.TestEqual(TEXT("Completed size survives optional-owner refusal"), Row.FinalBodyBytes, int64(4321));
			Test.TestTrue(TEXT("Completed storage survives optional-owner refusal"), Row.FinalStorage == EVaRestTraceResponseStorage::Memory);
			Test.TestTrue(TEXT("Completed omission remains visible"), Row.bMetadataOmitted);
			CheckPhase(Test, TEXT("refused completion Total"), Row.Total, 15);
			CheckPhase(Test, TEXT("refused completion Decode"), Row.Decode, 1);
			CheckPhase(Test, TEXT("refused completion Deliver"), Row.Deliver, 1);
		});
		Full.Reset();
		Store.Clear();
		if (!PushApply(Test, Queue, Store, Started(Id))) return false;
		Input.BytesUp = -1; Input.BytesDown = -1;
		if (!PushApply(Test, Queue, Store, Ended(Id, Input))) return false;
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			Test.TestEqual(TEXT("negative wire up never wraps"), Row.WireBytesUp, uint64(0));
			Test.TestEqual(TEXT("negative wire down never wraps"), Row.WireBytesDown, uint64(0));
			Test.TestTrue(TEXT("negative wire observation is unknown"), Row.bWireBytesUnknown);
		});
		// Existing slot/count slack: refuse on actual allocated array capacity,
		// which is deliberately much larger than its two visible bytes.
		auto LargeBody = Bytes(uint8('L'), 32768);
		if (!Test.TestTrue(TEXT("capacity control actual excerpt queued"), Queue.TryPush(Excerpt(Id, 1, 1, LargeBody)) == EPush::Accepted)) return false;
		auto HeldExcerpt = Queue.TryPopExcerpt();
		if (!Test.TestTrue(TEXT("capacity control actual excerpt dequeued"), HeldExcerpt.IsSet())) return false;
		const auto ExcerptCharge = FStorePacketOwner::Charge(HeldExcerpt.GetValue(), Roots.Context().Allocator);
		if (!Test.TestTrue(TEXT("capacity control exact store charge"), ExcerptCharge.IsSet() && ExcerptCharge.GetValue() > int64(LargeBody->Num()))) return false;
		{
			auto OneByteShort = FStoreCredit::Acquire(Roots.Account(), Roots.Account()->Limit - Roots.Account()->UsedBytes.load() - ExcerptCharge.GetValue() + 1);
			if (!Test.TestTrue(TEXT("excerpt capacity one-byte-short pressure"), OneByteShort.IsSet())) return false;
			Test.TestTrue(TEXT("excerpt refusal preserves accepted scalar row"), Store.Apply(MoveTemp(HeldExcerpt.GetValue())));
			Test.TestEqual(TEXT("refused allocated-capacity excerpt releases real source ref"), LargeBody.GetSharedReferenceCount(), 1);
			Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
				Test.TestEqual(TEXT("refused excerpt retains existing physical slot"), Row.Dispatches.Num(), 1);
				Test.TestFalse(TEXT("capacity refusal does not retain body bytes"), HasBody(FindDispatch(Row, 1), 1, uint8('L')));
				Test.TestEqual(TEXT("capacity refusal counted"), Row.OmittedExcerpts, uint64(1));
			});
		}
		Store.Clear();
		if (!PushApply(Test, Queue, Store, Started(Id))) return false;
		Input.BytesUp = Input.BytesDown = MAX_int64;
		for (uint64 DispatchId = 1; DispatchId <= 3; ++DispatchId)
		{
			Input.Id = DispatchId;
			if (!PushApply(Test, Queue, Store, Ended(Id, Input))) return false;
		}
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			Test.TestEqual(TEXT("wire up saturates without wrapping"), Row.WireBytesUp, uint64(MAX_uint64));
			Test.TestEqual(TEXT("wire down saturates without wrapping"), Row.WireBytesDown, uint64(MAX_uint64));
			Test.TestTrue(TEXT("wire saturation is explicit"), Row.bCountersSaturated);
			Test.TestEqual(TEXT("orphan ends do not fabricate starts"), Row.ObservedDispatches, uint64(0));
		});
		return true;
	});
}

static bool CheckAttachAndExcerptIdentity(FAutomationTestBase& Test)
{
	FCapturePolicy Policy;
	return WithStore(Test, Policy, [&](FStore& Store, FCaptureQueue& Queue, FPhaseFixtureRoots& Roots) {
		constexpr uint64 Id = 101;
		if (!PushApply(Test, Queue, Store, Started(Id))) return false;
		FDispatchInput Current;
		Current.Id = 2; Current.Attempt = 2;
		if (!PushApply(Test, Queue, Store, Dispatched(Id, Current))) return false;
		const int64 CopiedBefore = Roots.Context().Probe.TextBytesCopied.load();
		const int64 ChargeBefore = Roots.Account()->UsedBytes.load();
		FVaRestTraceSnapshot Snapshot;
		FVaRestTraceStarted& Attached = Snapshot.InFlight.AddDefaulted_GetRef();
		Attached.RequestId = Id; Attached.CurrentAttempt = 3; Attached.StartedAt = 10;
		Store.ApplyAttached(Snapshot, Policy, Roots.PendingAccount(), Roots.Context().Allocator);
		Test.TestEqual(TEXT("duplicate attach updates scalars without Started copy"), Roots.Context().Probe.TextBytesCopied.load(), CopiedBefore);
		Test.TestEqual(TEXT("duplicate attach takes no second Started owner"), Roots.Account()->UsedBytes.load(), ChargeBefore);
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			Test.TestEqual(TEXT("source attachment refreshes CurrentAttempt"), Row.CurrentAttempt, 3);
			Test.TestEqual(TEXT("source attempt does not synthesize physical slot"), Row.Dispatches.Num(), 1);
			Test.TestTrue(TEXT("source presence remains pinned and Partial"), Row.bPinned && Row.bPartial);
		});
		FDispatchInput Old;
		if (!PushApply(Test, Queue, Store, Ended(Id, Old)) || !PushApply(Test, Queue, Store, Started(Id))) return false;
		auto OldBody = Bytes(uint8('O'));
		if (!PushApply(Test, Queue, Store, Excerpt(Id, 1, 1, OldBody))) return false;
		auto RequestBody = Bytes(uint8('Q'));
		auto MalformedBody = Bytes(uint8('X'));
		if (!PushApply(Test, Queue, Store, Excerpt(Id, 0, 0, RequestBody, EVaRestTraceExcerptOwner::Request))) return false;
		if (!PushApply(Test, Queue, Store, Excerpt(Id, 0, 1, MalformedBody), EPush::Accepted, false)) return false;
		Test.TestEqual(TEXT("malformed response0 acquired no retained body owner"), MalformedBody.GetSharedReferenceCount(), 1);
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			Test.TestEqual(TEXT("old end/excerpt and historical Started cannot regress current attempt"), Row.CurrentAttempt, 3);
			const auto* Orphan = FindDispatch(Row, 1);
			if (Test.TestNotNull(TEXT("old orphan Ended has its own physical slot"), Orphan))
			{
				Test.TestFalse(TEXT("old orphan does not invent dispatch start"), Orphan->bHasDispatched);
				CheckPhase(Test, TEXT("old orphan Prepare unknown"), Orphan->Prepare, -1);
			}
			bool bRequestBody = false, bZeroResponse = false;
			for (const auto& Body : Row.Excerpts)
			{
				if (!Body.Fixed) continue;
				if (Body.Fixed->Scalars.Owner == EVaRestTraceExcerptOwner::Request)
					bRequestBody = Body.Fixed->Scalars.DispatchId == 0 && Body.Excerpt.Num() == 2 && Body.Excerpt[0] == uint8('Q');
				else if (Body.Fixed->Scalars.DispatchId == 0) bZeroResponse = true;
			}
			Test.TestTrue(TEXT("request-owner0 retains its real bytes"), bRequestBody);
			Test.TestFalse(TEXT("response-owner0 never assigned by Attempt"), bZeroResponse);
		});
		// An early body can create one bounded slot; its later metadata fills it.
		auto EarlyBody = Bytes(uint8('E'));
		if (!PushApply(Test, Queue, Store, Excerpt(Id, 9, 3, EarlyBody))) return false;
		Current.Id = 9; Current.Attempt = 3;
		if (!PushApply(Test, Queue, Store, Dispatched(Id, Current)) || !PushApply(Test, Queue, Store, Ended(Id, Current))) return false;
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			Test.TestTrue(TEXT("early excerpt remains bound to actual physical id"), HasBody(FindDispatch(Row, 9), 9, uint8('E')));
		});
		Store.Clear();
		Test.TestEqual(TEXT("request excerpt freed after Clear"), RequestBody.GetSharedReferenceCount(), 1);
		Test.TestEqual(TEXT("early excerpt freed after Clear"), EarlyBody.GetSharedReferenceCount(), 1);
		Test.TestEqual(TEXT("old physical excerpt freed after Clear"), OldBody.GetSharedReferenceCount(), 1);
		return true;
	});
}

static bool CheckHeaderPolicyAndOmission(FAutomationTestBase& Test)
{
	FCapturePolicy Policy;
	Policy.bHeaders = false;
	bool bResult = WithStore(Test, Policy, [&](FStore& Store, FCaptureQueue& Queue, FPhaseFixtureRoots&) {
		if (!PushApply(Test, Queue, Store, Started(111)) || !PushApply(Test, Queue, Store, Dispatched(111, FDispatchInput{}))) return false;
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			const auto* Dispatch = FindDispatch(Row, 1);
			if (!Test.TestNotNull(TEXT("policy-disabled headers keep scalar dispatch"), Dispatch)) return;
			Test.TestTrue(TEXT("policy-disabled headers remain Unknown"), Dispatch->HeadersState == EVaRestInspectorHeadersState::Unknown);
			Test.TestEqual(TEXT("policy-disabled headers have no borrowed prior backing"), Dispatch->Dispatched.Backing.Num(), 0);
		});
		return true;
	});
	Policy.bHeaders = true;
	bResult &= WithStore(Test, Policy, [&](FStore& Store, FCaptureQueue& Queue, FPhaseFixtureRoots& Roots) {
		if (!PushApply(Test, Queue, Store, Started(112))) return false;
		FDispatchInput Input;
		if (!PushApply(Test, Queue, Store, Dispatched(112, Input))) return false;
		Input.Id = 2;
		auto Oversized = Dispatched(112, Input);
		Oversized.Payload.Get<FVaRestTraceAttemptDispatched>().Headers.Set(TEXT("X-Large"), FString::ChrN(FNormalizedLimits::MaxStringChars + 1, TCHAR('x')));
		if (!PushApply(Test, Queue, Store, Oversized, EPush::ScalarOnly)) return false;
		Input.Id = 3;
		if (!PushApply(Test, Queue, Store, Dispatched(112, Input))) return false;
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			const auto* A = FindDispatch(Row, 1); const auto* Omitted = FindDispatch(Row, 2); const auto* B = FindDispatch(Row, 3);
			if (!Test.TestTrue(TEXT("omitted-header barrier retained scalar slots"), A && Omitted && B)) return;
			Test.TestTrue(TEXT("ScalarOnly headers visibly omitted"), Omitted->HeadersState == EVaRestInspectorHeadersState::MetadataOmitted);
			Test.TestEqual(TEXT("ScalarOnly headers expose no prior backing"), Omitted->Dispatched.Backing.Num(), 0);
			Test.TestTrue(TEXT("omitted observation breaks earlier equal-header reuse"), A->Dispatched.Backing.GetData() != B->Dispatched.Backing.GetData());
		});
		// Narrow natural flag carry only. Task6 still owns its full completion
		// diagnostics/control matrix and VaRest-zw5.2.6.1 acceptance.
		Store.Clear();
		if (!PushApply(Test, Queue, Store, Started(113))) return false;
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			Test.TestFalse(TEXT("ScalarOnly completion control starts without prior omission"), Row.bMetadataOmitted);
		});
		auto LargeCompletion = Completed(113);
		LargeCompletion.Payload.Get<FVaRestTraceCompleted>().Error.Message = FString::ChrN(FNormalizedLimits::MaxStringChars + 1, TCHAR('y'));
		const uint64 QueueOmissionsBefore = Roots.PendingAccount()->MetadataOmitted.load();
		if (!PushApply(Test, Queue, Store, LargeCompletion, EPush::ScalarOnly)) return false;
		Test.TestEqual(TEXT("one upstream completion omission"), Roots.PendingAccount()->MetadataOmitted.load(), QueueOmissionsBefore + 1);
		Store.ForEachRecord([&](const FVaRestInspectorRecordView& Row) {
			Test.TestTrue(TEXT("ScalarOnly Completed row omission truth survives transfer"), Row.bMetadataOmitted);
			Test.TestEqual(TEXT("ScalarOnly Completed delivered scalar"), Row.FinalStatus, 200);
			if (Test.TestNotNull(TEXT("ScalarOnly Completed retained packet"), Row.Completed.Fixed))
				Test.TestTrue(TEXT("ScalarOnly Completed packet omission flag"), Row.Completed.Fixed->Header.bMetadataOmitted);
		});
		return true;
	});
	return bResult;
}
} // namespace VaRest::Inspector::Private::PhaseTesting

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorPhasesTest, "VaRest.Inspector.Capture.Phases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorPhasesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	using namespace VaRest::Inspector::Private::PhaseTesting;
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	const double Infinity = std::numeric_limits<double>::infinity();
	struct FBetweenCase { const TCHAR* Name; double Begin, End, Expected; };
	const FBetweenCase BetweenCases[] = {
		{TEXT("finite ordinary interval"), 10, 13, 3}, {TEXT("genuine equal endpoints"), 10, 10, 0},
		{TEXT("genuine zero clock"), 0, 0, 0}, {TEXT("source -1 begin"), -1, 10, -1},
		{TEXT("source -1 end"), 10, -1, -1}, {TEXT("inverted endpoints"), 11, 10, -1},
		{TEXT("NaN begin"), NaN, 10, -1}, {TEXT("NaN end"), 10, NaN, -1},
		{TEXT("infinite begin"), Infinity, Infinity, -1}, {TEXT("infinite end"), 10, Infinity, -1}
	};
	for (const auto& Case : BetweenCases)
		CheckPhase(*this, Case.Name, Between(Case.Begin, Case.End), Case.Expected);
	const FTableCase Cases[] = {
		{TEXT("success"), ECase::Success}, {TEXT("503/backoff/200"), ECase::Retry},
		{TEXT("expired-auth401/refresh/replay200"), ECase::AuthReplay}, {TEXT("peer auth wait before first dispatch"), ECase::AuthPeer},
		{TEXT("Attempt1 both physical ends forward"), ECase::DuplicateForward}, {TEXT("Attempt1 both physical ends reverse"), ECase::DuplicateReverse},
		{TEXT("pre-dispatch parked cancellation"), ECase::ParkedCancel}, {TEXT("synthetic cancel-at-dispatch"), ECase::SyntheticCancel},
		{TEXT("unknown upload combined Send/Wait"), ECase::UnknownUpload}, {TEXT("unknown status and transport"), ECase::UnknownStatus},
		{TEXT("late attachment missing dispatch start"), ECase::LateAttach}, {TEXT("all genuine equal milestones"), ECase::EqualTimes}
	};
	bool bResult = true;
	for (const auto& Case : Cases) bResult &= RunCase(*this, Case);
	bResult &= CheckHeaders(*this);
	bResult &= CheckOwnerLifetime(*this);
	bResult &= CheckLimitsAndScalars(*this);
	bResult &= CheckAttachAndExcerptIdentity(*this);
	bResult &= CheckHeaderPolicyAndOmission(*this);
	return bResult;
}

#endif
