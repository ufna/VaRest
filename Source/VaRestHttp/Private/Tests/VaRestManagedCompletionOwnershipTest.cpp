// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "VaRestManagedCompletion.h"
#include "VaRestManagedOperation.h"
#include "VaRestNativeOwnershipFixture.h"

namespace VaRest::Http::Testing::ManagedCompletionPrivate
{
struct FState
{
	int32 HandlerDeaths = 0;
	int32 ClockDeaths = 0;
	int32 Deliveries = 0;
	int32 Bookkeeping = 0;
	bool WrongThread = false;
	std::atomic<bool> WorkerOffGT{false};
};
class FHandler final : public IResultHandler
{
public:
	explicit FHandler(TSharedRef<FState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState))
	{
	}
	~FHandler() override
	{
		++State->HandlerDeaths;
		State->WrongThread |= !IsInGameThread();
	}
	FName GetResultType() const override { return NAME_None; }
	bool DecodesBody() const override { return false; }
	bool DecodesJson() const override { return false; }
	bool AppliesEnvelope() const override { return false; }
	void Decode(FFinish&) override {}
	void Deliver(FFinish&) override { ++State->Deliveries; }
	bool WantsProgress() const override { return false; }
	void Progress(const FVaRestProgress&) override {}

private:
	TSharedRef<FState, ESPMode::ThreadSafe> State;
};
class FClock final : public IVaRestClock
{
public:
	explicit FClock(TSharedRef<FState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState))
	{
	}
	~FClock() override
	{
		++State->ClockDeaths;
		State->WrongThread |= !IsInGameThread();
	}
	double Now() const override { return 0.0; }
	FVaRestTimerHandle SetTimer(double, TUniqueFunction<void()>&&) override { return {}; }
	void ClearTimer(FVaRestTimerHandle& Handle) override { Handle = {}; }

private:
	TSharedRef<FState, ESPMode::ThreadSafe> State;
};

FManagedOperationPayload* Prepare(NativeOwnership::FFixture& F)
{
	if (!F.Store)
		return nullptr;
	auto Input = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(F.Rig.Client), {});
	F.Sender = F.Control->AdoptInput(Input);
	if (!F.Gate->TryAcquire(F.Work) || !F.Store->TryClaim(F.Work, F.Sender, F.Claim, 2))
		return nullptr;
	auto* Owner = F.Claim.CreateManagedPayload();
	if (!Owner)
		return nullptr;
	Owner->SendConfig = FClientAccess::State(F.Rig.Client)->GetConfig();
	Owner->Exchange = MakeShared<FVaRestExchange, ESPMode::ThreadSafe>();
	auto& Ex = Internals(*Owner->Exchange);
	Ex.State = FClientAccess::State(F.Rig.Client);
	Ex.Config = Owner->SendConfig;
	Ex.Managed = Owner->Reference();
	F.Handoff = F.Claim.Publish(F.Completion, F.DeliveryWorker);
	if (!F.Handoff.IsHeld())
		return nullptr;
	Owner->ParkCompletionOnGT(MoveTemp(F.Completion), MoveTemp(F.DeliveryWorker));
	return Owner;
}
} // namespace VaRest::Http::Testing::ManagedCompletionPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedCompletionAtCapacityTest, "VaRest.Http.Managed.CompletionAtLogicalCapacity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedCompletionAtCapacityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	NativeOwnership::FFixture F(true);
	auto* Owner = ManagedCompletionPrivate::Prepare(F);
	if (!TestNotNull(TEXT("actual claimed typed payload and parked initial completion"), Owner))
		return false;
	const auto Ref = F.Handoff.Reference();
	for (uint32 I = 0; I < 62; ++I)
	{
		auto Child = F.Store->TryAddLogicalTailOnGT(Ref);
		if (!TestTrue(TEXT("actual remaining logical capacity"), Child.IsHeld()))
			return false;
		F.Children.Add(MoveTemp(Child));
	}
	TestFalse(TEXT("no uncounted sixty-fifth child"), F.Store->TryAddLogicalTailOnGT(Ref).IsHeld());
	FNativeLogicalTailLease Same;
	if (!TestFalse(TEXT("aliased initial selector preserves parked owners"), F.Store->TakeInitialCompletionOnGT(Ref, Same, Same)))
		return false;
	if (!TestTrue(TEXT("existing initial completion transfers at exactly64 without a new permit"), F.Store->TakeInitialCompletionOnGT(Ref, F.Completion, F.DeliveryWorker)))
		return false;
	auto State = MakeShared<ManagedCompletionPrivate::FState, ESPMode::ThreadSafe>();
	auto& Frame = Owner->CompletionOnGT();
	Frame.Handler = MakeUnique<ManagedCompletionPrivate::FHandler>(State);
	Frame.Finish.Clock = MakeShared<ManagedCompletionPrivate::FClock, ESPMode::ThreadSafe>(State);
	Frame.CompleteKernel = [](FManagedCompletionRef Held) {
		Held->Reference().Store->MarkTerminalOnGT(Held->Reference()); // genuine foundation terminal; no public HTTP result is claimed
		Held->Payload().bFinalized = true;
		Held->Payload().Handler->Deliver(Held->Payload().Finish);
		Held->DisposeOnGT(false);
	};
	TSharedPtr<FManagedCompletionAccess, ESPMode::ThreadSafe> Access = FManagedCompletionAccess::CreateOnGT(Frame, MoveTemp(F.Completion));
	F.Handoff.Reset();
	F.Children.Reset();
	Access->CompleteOnGT();
	TestEqual(TEXT("actual handler invoked exactly once"), State->Deliveries, 1);
	TestEqual(TEXT("actual handler destroyed before final completion authority"), State->HandlerDeaths, 1);
	TestEqual(TEXT("actual finish clock destroyed before final completion authority"), State->ClockDeaths, 1);
	TestEqual(TEXT("executing/access owner still retains counted work"), F.Gate->NumOwnedOperations(), uint32(1));
	Access.Reset();
	TestEqual(TEXT("GTK completion release settles actual work"), F.Gate->NumOwnedOperations(), uint32(0));
	F.Store->SweepOnGT(FNativeOperationStore::Capacity);
	F.Control->CollectInputsOnGT();
	TestEqual(TEXT("actual same cell and input reclaimed"), F.Store->NumOccupied(), uint32(0));
	TestEqual(TEXT("actual input count zero"), F.Control->NumPendingInputs(), uint64(0));
	TestFalse(TEXT("all actual affine data destruction stayed GTK"), State->WrongThread);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedAbandonedCompletionTest, "VaRest.Http.Managed.AbandonedCompletionReturnsExistingAuthority",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedAbandonedCompletionTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("requires an actual separate native worker"));
		return true;
	}
	NativeOwnership::FFixture F(true);
	auto* Owner = ManagedCompletionPrivate::Prepare(F);
	if (!TestNotNull(TEXT("actual Control-owned cell and payload"), Owner))
		return false;
	if (!TestTrue(TEXT("actual initial completion selected"), F.Store->TakeInitialCompletionOnGT(F.Handoff.Reference(), F.Completion, F.DeliveryWorker)))
		return false;
	auto State = MakeShared<ManagedCompletionPrivate::FState, ESPMode::ThreadSafe>();
	auto& Frame = Owner->CompletionOnGT();
	Frame.Handler = MakeUnique<ManagedCompletionPrivate::FHandler>(State);
	Frame.Finish.Clock = MakeShared<ManagedCompletionPrivate::FClock, ESPMode::ThreadSafe>(State);
	Frame.CompleteKernel = [State](FManagedCompletionRef) { ++State->Deliveries; };
	Frame.DiscardKernel = [State] { ++State->Bookkeeping; };
	TSharedPtr<FManagedCompletionAccess, ESPMode::ThreadSafe> Access = FManagedCompletionAccess::CreateOnGT(Frame, MoveTemp(F.Completion));
	F.Handoff.Reset();
	F.Control->StopInputsOnGT();
	TestEqual(TEXT("taken completion retains actual handler through Stop"), State->HandlerDeaths, 0);
	TestEqual(TEXT("taken completion retains actual finish through Stop"), State->ClockDeaths, 0);
	F.Producer = Async(EAsyncExecution::Thread, [Held = MoveTemp(Access), State]() mutable {
		State->WorkerOffGT.store(!IsInGameThread(), std::memory_order_relaxed);
		Held.Reset();
	});
	F.Producer.Wait(); // owned native body reset only; not a standalone weak-memory or pool-closure proof
	TestTrue(TEXT("the actual final access was dropped by a separate native worker"), State->WorkerOffGT.load(std::memory_order_relaxed));
	TestEqual(TEXT("worker's final native access never destroys affine handler"), State->HandlerDeaths, 0);
	TestEqual(TEXT("worker's final native access never destroys affine finish"), State->ClockDeaths, 0);
	TestEqual(TEXT("mailbox retains SAME initial permission until GTK cleanup"), F.Gate->NumOwnedOperations(), uint32(1));
	F.Store->SweepOnGT(FNativeOperationStore::Capacity);
	F.Control->CollectInputsOnGT();
	TestEqual(TEXT("actual GTK discard bookkeeping once"), State->Bookkeeping, 1);
	TestEqual(TEXT("actual handler destroyed on owned GTK service"), State->HandlerDeaths, 1);
	TestEqual(TEXT("actual finish clock destroyed on owned GTK service"), State->ClockDeaths, 1);
	TestEqual(TEXT("no manufactured normal result after stopped abandonment"), State->Deliveries, 0);
	TestEqual(TEXT("no internal completion cycle retains gate"), F.Gate->NumOwnedOperations(), uint32(0));
	TestEqual(TEXT("actual cell reclaimed after data cleanup"), F.Store->NumOccupied(), uint32(0));
	TestEqual(TEXT("whole input reclaimed"), F.Control->NumPendingInputs(), uint64(0));
	TestFalse(TEXT("actual affine payload destruction GTK only"), State->WrongThread);
	return true;
}

#endif
