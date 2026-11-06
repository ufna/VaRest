// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "VaRestManagedDispatch.h"
#include "VaRestManagedOperation.h"
#include "VaRestNativeOwnershipFixture.h"

namespace VaRest::Http::Testing::ManagedDispatchStopPrivate
{
struct FState
{
	std::atomic<int32> ClockDeaths{0};
	std::atomic<int32> CallbackDeaths{0};
	std::atomic<int32> Deliveries{0};
	std::atomic<int32> Outcomes{0};
	std::atomic<bool> WrongThread{false};
	std::atomic<bool> ReadOffGT{false};
	std::atomic<double> ObservedLastRead{0.0};
};
class FClock final : public IVaRestClock
{
public:
	explicit FClock(TSharedRef<FState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState))
	{
	}
	~FClock()
	{
		if (!IsInGameThread())
			State->WrongThread.store(true, std::memory_order_relaxed);
		State->ObservedLastRead.store(LastRead, std::memory_order_relaxed);
		State->ClockDeaths.fetch_add(1, std::memory_order_relaxed);
	}
	double Now() const override
	{
		State->ReadOffGT.store(!IsInGameThread(), std::memory_order_relaxed);
		LastRead = 91.0; // actual ORIGINAL clock data: only the worker writes it, GT destructor must acquire that last use
		return LastRead;
	}
	FVaRestTimerHandle SetTimer(double Delay, TUniqueFunction<void()>&& Callback) override
	{
		check(IsInGameThread());
		Callback = nullptr;
		return {};
	}
	void ClearTimer(FVaRestTimerHandle& Handle) override { check(IsInGameThread()); }

private:
	TSharedRef<FState, ESPMode::ThreadSafe> State;
	mutable double LastRead = 13.0;
};
struct FCapture
{
	explicit FCapture(TSharedRef<FState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState))
	{
	}
	~FCapture()
	{
		if (!IsInGameThread())
			State->WrongThread.store(true, std::memory_order_relaxed);
		State->CallbackDeaths.fetch_add(1, std::memory_order_relaxed);
	}
	TSharedRef<FState, ESPMode::ThreadSafe> State;
};

/** Every failure/latent cancellation resumes ONLY its own worker before the existing owned fixture cleanup waits. */
struct FContext
{
	explicit FContext(FAutomationTestBase* InTest)
		: Test(InTest)
		, F(MakeUnique<NativeOwnership::FFixture>(true))
	{
	}
	~FContext()
	{
		check(IsInGameThread());
		Probe->Resume.store(true, std::memory_order_release);
		F.Reset(); // joins its own producer only; never pumps a global GT queue
	}
	bool Start()
	{
		if (!F->Store)
			return false;
		auto Input = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(F->Rig.Client), {});
		F->Sender = F->Control->AdoptInput(Input);
		if (!F->Gate->TryAcquire(F->Work) || !F->Store->TryClaim(F->Work, F->Sender, F->Claim, 2))
			return false;
		auto* Payload = F->Claim.CreateManagedPayload();
		if (!Payload)
			return false;
		F->Handoff = F->Claim.Publish(F->Completion, F->DeliveryWorker);
		if (!F->Handoff.IsHeld())
			return false;
		Payload->ParkCompletionOnGT(MoveTemp(F->Completion), MoveTemp(F->DeliveryWorker));
		TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Original = MakeShared<FClock, ESPMode::ThreadSafe>(State);
		TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> View;
		FVaRestTransportComplete Complete;
		{
			FDispatchTag Tag;
			Tag.Serial = 1;
			FVaRestOutcomeCallback Outcome = [Capture = MakeUnique<FCapture>(State), Held = State](FVaRestAttemptOutcome&&) {
				Held->Outcomes.fetch_add(1, std::memory_order_relaxed);
			};
			FManagedAttemptDelivery Delivery = [Held = State](FDispatchTag&&, FVaRestTransportResult&&, FVaRestOutcomeCallback&&) {
				Held->Deliveries.fetch_add(1, std::memory_order_relaxed);
			};
			auto Prepared = Payload->DispatchesOnGT().PrepareOnGT(Tag, Outcome, Original, Delivery);
			if (!Prepared.IsValid())
				return false;
			Prepared.PausePublicationForTests(Probe);
			View = Prepared.ClockViewOnGT();
			Complete = Prepared.TakeCompletionOnGT();
		}
		Original.Reset(); // no external original owner or leftover preparation may hide a wrong slot cleanup
		F->Producer = Async(EAsyncExecution::ThreadPool, [Done = MoveTemp(Complete), Clock = MoveTemp(View)]() mutable {
			FVaRestTransportResult Result;
			Result.Status = 200;
			Result.Body.Init(uint8(0x5a), 4096); // real nonempty result moves into the actual slot before the rendezvous
			Done(MoveTemp(Result));              // paused inside actual Publish; GT stops Control before allowing the second live check
			Clock->Now();                        // final actual original use AFTER the GT's rendezvous observation, even though Stopped
			Clock.Reset();
			Done = nullptr; // actual last guard/view access ends inside the worker body, independently of promise/closure retirement
		});
		return true;
	}
	FAutomationTestBase* Test;
	TSharedRef<FState, ESPMode::ThreadSafe> State = MakeShared<FState, ESPMode::ThreadSafe>();
	TSharedRef<FManagedDispatchPublicationProbe, ESPMode::ThreadSafe> Probe = MakeShared<FManagedDispatchPublicationProbe, ESPMode::ThreadSafe>();
	TUniquePtr<NativeOwnership::FFixture> F;
	bool bStopped = false;
	uint32 RemainingUpdates = 600; // finite automation failure escape, not a time/weak-memory proof
};
using FContextRef = TSharedRef<FContext, ESPMode::ThreadSafe>;
} // namespace VaRest::Http::Testing::ManagedDispatchStopPrivate

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FVaRestManagedDispatchStopCommand,
	VaRest::Http::Testing::ManagedDispatchStopPrivate::FContextRef, Context);

bool FVaRestManagedDispatchStopCommand::Update()
{
	using namespace VaRest::Http;
	if (Context->RemainingUpdates-- == 0)
	{
		Context->Test->AddError(TEXT("owned dispatch publication/cleanup did not progress within the fixture's finite update budget"));
		return true; // FContext resumes its owned worker before fixture destruction on every exit
	}
	if (!Context->bStopped)
	{
		// RELAXED progress observation intentionally supplies NO worker-to-GT reclamation fence.
		if (!Context->Probe->Published.load(std::memory_order_relaxed))
			return false;
		Context->F->Control->StopInputsOnGT();
		Context->Test->TestEqual(TEXT("paused actual result/view access prevents original destruction"), Context->State->ClockDeaths.load(std::memory_order_relaxed), 0);
		Context->Test->TestEqual(TEXT("actual stage callback still belongs to the GT slot"), Context->State->CallbackDeaths.load(std::memory_order_relaxed), 0);
		Context->bStopped = true;
		Context->Probe->Resume.store(true, std::memory_order_release); // orders GT toward worker; not its later clock use toward GT
		return false;
	}
	// Keep initial Handoff held: pre-service combined settlement short-circuits on this logical tail, not a worker counter.
	// No producer Wait/join, test acquire of worker completion or gate-count read precedes this actual owned slot cleanup.
	Context->F->Store->SweepOnGT(FNativeOperationStore::Capacity);
	if (Context->State->ClockDeaths.load(std::memory_order_relaxed) == 0)
		return false;
	Context->Test->TestEqual(TEXT("GT destroyed actual original once"), Context->State->ClockDeaths.load(std::memory_order_relaxed), 1);
	Context->Test->TestEqual(TEXT("original destructor observes the actual last worker method's ordinary data"), Context->State->ObservedLastRead.load(std::memory_order_relaxed), 91.0);
	Context->Test->TestTrue(TEXT("last original method actually ran off GT"), Context->State->ReadOffGT.load(std::memory_order_relaxed));
	Context->Test->TestFalse(TEXT("actual callback and original clock owners die on GT"), Context->State->WrongThread.load(std::memory_order_relaxed));
	Context->Test->TestEqual(TEXT("actual original callback destroyed once"), Context->State->CallbackDeaths.load(std::memory_order_relaxed), 1);
	Context->Test->TestEqual(TEXT("stopped publication invokes no bridge"), Context->State->Deliveries.load(std::memory_order_relaxed), 0);
	Context->Test->TestEqual(TEXT("stopped publication invents no outcome"), Context->State->Outcomes.load(std::memory_order_relaxed), 0);
	Context->F->Handoff.Reset();
	Context->F->Store->SweepOnGT(FNativeOperationStore::Capacity);
	Context->F->Control->CollectInputsOnGT();
	Context->Test->TestEqual(TEXT("actual stopped cell reclaimed"), Context->F->Store->NumOccupied(), uint32(0));
	Context->Test->TestEqual(TEXT("actual residual input reclaimed"), Context->F->Control->NumPendingInputs(), uint64(0));
	Context->Test->TestEqual(TEXT("all real operation authority ended"), Context->F->Gate->NumOwnedOperations(), uint32(0));
	// Only AFTER the actual teardown observations, cleanup may join this owned producer. This cannot fence the earlier reads.
	Context->F->Producer.Wait();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedDispatchStopFenceTest, "VaRest.Http.Managed.DispatchStoppedPublicationFence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedDispatchStopFenceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("DispatchStoppedPublicationFence needs an actual worker; skipped without multithreading support"));
		return true;
	}
	auto Context = MakeShared<ManagedDispatchStopPrivate::FContext, ESPMode::ThreadSafe>(this);
	if (!TestTrue(TEXT("real owned dispatch fixture starts its actual producer"), Context->Start()))
		return false;
	ADD_LATENT_AUTOMATION_COMMAND(FVaRestManagedDispatchStopCommand(Context));
	return true;
}

#endif
