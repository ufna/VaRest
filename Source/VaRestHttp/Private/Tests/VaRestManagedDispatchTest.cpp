// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "VaRestManagedDispatch.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "VaRestManagedOperation.h"
#include "VaRestNativeOwnershipFixture.h"

namespace VaRest::Http::Testing::ManagedDispatchPrivate
{
struct FClockState
{
	std::atomic<int32> Destructions{0};
	std::atomic<int32> Reads{0};
	std::atomic<int32> InRead{0};
	std::atomic<bool> WrongThread{false};
	std::atomic<bool> DestroyedDuringRead{false};
	std::atomic<bool> ReadOffGT{false};
	std::atomic<double> Actual{0.0};
	std::atomic<double> Value{37.25};
};
class FClock final : public IVaRestClock
{
public:
	FClock(TSharedRef<FClockState, ESPMode::ThreadSafe> InState, TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> InTimers,
		TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> InStop = {})
		: State(MoveTemp(InState))
		, Timers(MoveTemp(InTimers))
		, Stop(MoveTemp(InStop))
	{
	}
	~FClock()
	{
		State->Destructions.fetch_add(1);
		State->WrongThread.store(!IsInGameThread());
		State->DestroyedDuringRead.store(State->InRead.load() != 0);
		if (auto Control = Stop.Pin())
			Control->StopInputsOnGT();
	}
	double Now() const override
	{
		const auto KeepState = State;
		KeepState->Reads.fetch_add(1);
		KeepState->InRead.fetch_add(1);
		if (OnRead)
		{
			auto Once = MoveTemp(OnRead);
			Once();
		}
		KeepState->InRead.fetch_sub(1);
		return KeepState->Value.load();
	}
	FVaRestTimerHandle SetTimer(double Delay, TUniqueFunction<void()>&& Callback) override { return Timers->SetTimer(Delay, MoveTemp(Callback)); }
	void ClearTimer(FVaRestTimerHandle& Handle) override { Timers->ClearTimer(Handle); }
	mutable TUniqueFunction<void()> OnRead;

private:
	TSharedRef<FClockState, ESPMode::ThreadSafe> State;
	TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Timers;
	TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Stop;
};

FManagedOperationPayload* Initialize(NativeOwnership::FFixture& F)
{
	if (!F.Store)
		return nullptr;
	auto Input = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(F.Rig.Client), {});
	F.Sender = F.Control->AdoptInput(Input);
	if (!F.Gate->TryAcquire(F.Work) || !F.Store->TryClaim(F.Work, F.Sender, F.Claim, 2))
		return nullptr;
	auto* Payload = F.Claim.CreateManagedPayload();
	if (!Payload)
		return nullptr;
	Payload->SendConfig = FClientAccess::State(F.Rig.Client)->GetConfig();
	Payload->Exchange = MakeShared<FVaRestExchange, ESPMode::ThreadSafe>();
	Internals(*Payload->Exchange).State = FClientAccess::State(F.Rig.Client);
	Internals(*Payload->Exchange).Config = Payload->SendConfig;
	F.Handoff = F.Claim.Publish(F.Completion, F.DeliveryWorker);
	return F.Handoff.IsHeld() ? Payload : nullptr;
}

void Sweep(NativeOwnership::FFixture& F)
{
	for (uint32 Pass = 0; Pass < 8 && F.Store->NumOccupied() != 0; ++Pass)
		F.Control->ClientSlotsOnGT().SweepOperations();
	F.Control->CollectInputsOnGT();
}
} // namespace VaRest::Http::Testing::ManagedDispatchPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedClockOwnerTest, "VaRest.Http.Managed.DispatchClockOwner",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedClockOwnerTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("DispatchClockOwner needs an actual worker; skipped without multithreading support"));
		return true;
	}
	NativeOwnership::FFixture F(true);
	auto* Payload = ManagedDispatchPrivate::Initialize(F);
	if (!TestNotNull(TEXT("fixture owns a real operation payload and cell"), Payload))
		return false;
	auto State = MakeShared<ManagedDispatchPrivate::FClockState, ESPMode::ThreadSafe>();
	TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Original = MakeShared<ManagedDispatchPrivate::FClock, ESPMode::ThreadSafe>(State, F.Rig.Clock);
	TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> View;
	{
		FDispatchTag Tag;
		Tag.Serial = 1;
		FVaRestOutcomeCallback Outcome = [](FVaRestAttemptOutcome&&) {};
		FManagedAttemptDelivery Delivery = [](FDispatchTag&&, FVaRestTransportResult&&, FVaRestOutcomeCallback&&) {};
		auto Prepared = Payload->DispatchesOnGT().PrepareOnGT(Tag, Outcome, Original, Delivery);
		if (!TestTrue(TEXT("real dispatch authority and owned payload position reserved"), Prepared.IsValid()))
			return false;
		auto Unsent = Prepared.TakeUnstartedOutcomeOnGT(); // no transport/task is launched by this ownership-only case
		View = Prepared.ClockViewOnGT();
		if (Unsent)
			Unsent(FVaRestAttemptOutcome::Canceled());
	}
	Original.Reset(); // the slot, not a hidden test local, is now the original clock's only strong owner
	TestEqual(TEXT("preparation never samples and caches original time"), State->Reads.load(), 0);
	State->Value.store(51.5); // change AFTER preparation; the late call must observe the actual original's current value
	F.Store->MarkTerminalOnGT(F.Handoff.Reference());
	F.Handoff.Reset();
	F.Completion.Reset();
	TestEqual(TEXT("actual retained clock view keeps physical settlement outstanding"), F.Gate->NumOwnedOperations(), uint32(1));
	F.Control->StopInputsOnGT();
	F.Producer = Async(EAsyncExecution::ThreadPool, [Held = MoveTemp(View), State]() mutable {
		State->ReadOffGT.store(!IsInGameThread());
		State->Actual.store(Held->Now()); // exact already-owned late read, not substituted platform time or stopped sentinel
		Held.Reset();
	});
	F.Producer.Wait();
	TestTrue(TEXT("late read and final view release actually ran off GT"), State->ReadOffGT.load());
	TestEqual(TEXT("late Now forwards the exact current original result"), State->Actual.load(), 51.5);
	TestEqual(TEXT("actual late original virtual method invoked exactly once"), State->Reads.load(), 1);
	TestEqual(TEXT("last worker view drop settles the real native authority"), F.Gate->NumOwnedOperations(), uint32(0));
	TestEqual(TEXT("original clock was not destroyed by worker view release"), State->Destructions.load(), 0);
	ManagedDispatchPrivate::Sweep(F);
	TestEqual(TEXT("GT then destroys the actual original clock once"), State->Destructions.load(), 1);
	TestFalse(TEXT("original clock destruction is GT-affine"), State->WrongThread.load());
	TestEqual(TEXT("actual cell and input reclaimed"), F.Store->NumOccupied(), uint32(0));
	TestEqual(TEXT("actual input inventory empty"), F.Control->NumPendingInputs(), uint64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedClockReentrantTest, "VaRest.Http.Managed.DispatchClockReentrantStop",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedClockReentrantTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	NativeOwnership::FFixture F(true);
	auto* Payload = ManagedDispatchPrivate::Initialize(F);
	if (!TestNotNull(TEXT("actual owned operation initialized"), Payload))
		return false;
	auto State = MakeShared<ManagedDispatchPrivate::FClockState, ESPMode::ThreadSafe>();
	TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Weak = F.Control;
	TSharedPtr<ManagedDispatchPrivate::FClock, ESPMode::ThreadSafe> Original = MakeShared<ManagedDispatchPrivate::FClock, ESPMode::ThreadSafe>(State, F.Rig.Clock, Weak);
	TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> View;
	{
		FDispatchTag Tag;
		Tag.Serial = 1;
		FVaRestOutcomeCallback Outcome = [](FVaRestAttemptOutcome&&) {};
		FManagedAttemptDelivery Delivery = [](FDispatchTag&&, FVaRestTransportResult&&, FVaRestOutcomeCallback&&) {};
		auto Prepared = Payload->DispatchesOnGT().PrepareOnGT(Tag, Outcome, Original, Delivery);
		if (!TestTrue(TEXT("prepared dispatch has an actual counted native authority"), Prepared.IsValid()))
			return false;
		auto Unsent = Prepared.TakeUnstartedOutcomeOnGT();
		View = Prepared.ClockViewOnGT();
		if (Unsent)
			Unsent(FVaRestAttemptOutcome::Canceled());
	}
	Original->OnRead = [&View, Weak] {
		View.Reset(); // release the LAST external view owner from inside the original virtual Now method
		if (auto Control = Weak.Pin())
			Control->ClientSlotsOnGT().SweepOperations(); // must not reclaim the original clock beneath that active method
	};
	Original.Reset();
	Payload->ParkCompletionOnGT(MoveTemp(F.Completion), MoveTemp(F.DeliveryWorker));
	TestEqual(TEXT("reentrant exact Now still returns original value"), View->Now(), 37.25);
	TestFalse(TEXT("callback actually dropped its external view"), View.IsValid());
	TestEqual(TEXT("whole-method self pin prevented original destruction during Now"), State->Destructions.load(), 0);
	TestFalse(TEXT("no original clock destructor ran beneath its active virtual call"), State->DestroyedDuringRead.load());
	F.Handoff.Reset();
	ManagedDispatchPrivate::Sweep(F); // original clock destructor reenters Stop from a formerly LIVE owned sweep
	TestTrue(TEXT("actual original destructor reentered private Control Stop"), F.Control->IsStopped());
	TestEqual(TEXT("fresh stopped recheck consumes still-parked completion"), F.Gate->NumOwnedOperations(), uint32(0));
	TestEqual(TEXT("no parked self-cycle survives the reentrant stop"), F.Store->NumOccupied(), uint32(0));
	TestEqual(TEXT("original clock destroyed exactly once on GT"), State->Destructions.load(), 1);
	TestFalse(TEXT("reentrant cleanup clock destruction stayed GT"), State->WrongThread.load());
	TestEqual(TEXT("reentrant owned cleanup also finishes exact input"), F.Control->NumPendingInputs(), uint64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedDispatchReuseTest, "VaRest.Http.Managed.DispatchSlotReuse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedDispatchReuseTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	NativeOwnership::FFixture F(true);
	auto* Payload = ManagedDispatchPrivate::Initialize(F);
	if (!TestNotNull(TEXT("actual operation owns its fixed dispatch storage"), Payload))
		return false;
	for (uint32 Serial = 1; Serial <= 40; ++Serial)
	{
		{
			FDispatchTag Tag;
			Tag.Serial = Serial;
			FVaRestOutcomeCallback Outcome = [State = F.Probes](FVaRestAttemptOutcome&&) { ++State->UserCalls; };
			FManagedAttemptDelivery Delivery = [](FDispatchTag&&, FVaRestTransportResult&&, FVaRestOutcomeCallback&&) {};
			auto Prepared = Payload->DispatchesOnGT().PrepareOnGT(Tag, Outcome, F.Rig.Clock, Delivery);
			if (!TestTrue(TEXT("sequential preparations reuse storage beyond its32 positions"), Prepared.IsValid()))
				return false;
			TestEqual(TEXT("actual slot tag belongs to this preparation"), Prepared.TagOnGT().Serial, Serial);
			auto Unsent = Prepared.TakeUnstartedOutcomeOnGT();
			if (Unsent)
				Unsent(FVaRestAttemptOutcome::Canceled());
		}
		// No cell/ticker sweep between preparations: the next GT preparation must reclaim its own expired slot locally.
	}
	TestEqual(TEXT("all actual unstarted callbacks invoked exactly once on GT"), F.Probes->UserCalls, 40);
	F.Store->MarkTerminalOnGT(F.Handoff.Reference());
	F.Completion.Reset();
	F.Handoff.Reset();
	F.Store->SweepOnGT(F.Store->Capacity);
	F.Control->CollectInputsOnGT();
	TestEqual(TEXT("reused slots keep no completed operation history"), F.Store->NumOccupied(), uint32(0));
	TestEqual(TEXT("same operation released admitted work once"), F.Gate->NumOwnedOperations(), uint32(0));
	return true;
}

#endif
