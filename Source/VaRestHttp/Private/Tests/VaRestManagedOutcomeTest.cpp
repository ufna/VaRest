// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "VaRestManagedOutcome.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "VaRestManagedOperation.h"
#include "VaRestNativeOwnershipFixture.h"

namespace VaRest::Http::Testing::ManagedOutcomePrivate
{
struct FProbe
{
	std::atomic<int32> Destructions{0};
	std::atomic<int32> Calls{0};
	std::atomic<bool> WrongThread{false};
	std::atomic<bool> BodyActive{false};
	std::atomic<bool> DestroyedDuringBody{false};
	std::atomic<bool> WorkerRan{false};
};
struct FCapture
{
	FCapture(TSharedRef<FProbe, ESPMode::ThreadSafe> InProbe, TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> InStop = {})
		: Probe(MoveTemp(InProbe))
		, Stop(MoveTemp(InStop))
	{
	}
	~FCapture()
	{
		++Probe->Destructions;
		if (!IsInGameThread())
			Probe->WrongThread.store(true);
		if (Probe->BodyActive.load())
			Probe->DestroyedDuringBody.store(true);
		if (auto Control = Stop.Pin())
			Control->StopInputsOnGT();
	}
	TSharedRef<FProbe, ESPMode::ThreadSafe> Probe;
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
	F.Handoff = F.Claim.Publish(F.Completion, F.DeliveryWorker);
	return F.Handoff.IsHeld() ? Payload : nullptr;
}

void Sweep(NativeOwnership::FFixture& F)
{
	for (uint32 Pass = 0; Pass < 8 && F.Store->NumOccupied() != 0; ++Pass)
		F.Control->ClientSlotsOnGT().SweepOperations();
	F.Control->CollectInputsOnGT();
}
} // namespace VaRest::Http::Testing::ManagedOutcomePrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedOutcomeWorkerDropTest, "VaRest.Http.Managed.OutcomeWorkerDrop",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedOutcomeWorkerDropTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("OutcomeWorkerDrop needs an actual worker; skipped without multithreading support"));
		return true;
	}
	NativeOwnership::FFixture F(true);
	auto* Payload = ManagedOutcomePrivate::Initialize(F);
	if (!TestNotNull(TEXT("actual Control-owned cell and payload"), Payload))
		return false;
	auto Probe = MakeShared<ManagedOutcomePrivate::FProbe, ESPMode::ThreadSafe>();
	FVaRestOutcomeCallback Original = [Capture = MakeUnique<ManagedOutcomePrivate::FCapture>(Probe), Probe](FVaRestAttemptOutcome&&) { ++Probe->Calls; };
	auto Adapter = Payload->OutcomesOnGT().WrapOnGT(Original);
	if (!TestTrue(TEXT("genuine logical tail reserves the actual callback before exposing its adapter"), bool(Adapter)))
		return false;
	TestFalse(TEXT("original callback moved into the actual GT owner"), bool(Original));
	F.Store->MarkTerminalOnGT(F.Handoff.Reference());
	F.Completion.Reset();
	F.Handoff.Reset();
	F.Control->StopInputsOnGT();
	TestEqual(TEXT("retained actual adapter still owns logical settlement"), F.Gate->NumOwnedOperations(), uint32(1));
	F.Producer = Async(EAsyncExecution::ThreadPool, [Held = MoveTemp(Adapter), Probe]() mutable {
		Probe->WorkerRan.store(!IsInGameThread());
		Held = nullptr; // actual adapter destruction IN the worker body, not inferred from promise/closure destruction
	});
	F.Producer.Wait();
	TestTrue(TEXT("actual off-GT adapter destruction occurred"), Probe->WorkerRan.load());
	TestEqual(TEXT("native authority really settled on that worker"), F.Gate->NumOwnedOperations(), uint32(0));
	TestEqual(TEXT("actual callback capture was not the worker's payload"), Probe->Destructions.load(), 0);
	ManagedOutcomePrivate::Sweep(F);
	TestEqual(TEXT("owned GT sweep destroys the actual uncalled capture once"), Probe->Destructions.load(), 1);
	TestFalse(TEXT("actual captured destructor remained GT-affine"), Probe->WrongThread.load());
	TestEqual(TEXT("an uncalled drop invents no stage outcome or user call"), Probe->Calls.load(), 0);
	TestEqual(TEXT("actual cell reclaimed"), F.Store->NumOccupied(), uint32(0));
	TestEqual(TEXT("actual residual input reclaimed"), F.Control->NumPendingInputs(), uint64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedOutcomeReentrantTest, "VaRest.Http.Managed.OutcomeReentrantStop",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedOutcomeReentrantTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	NativeOwnership::FFixture F(true);
	auto* Payload = ManagedOutcomePrivate::Initialize(F);
	if (!TestNotNull(TEXT("actual typed payload initialized"), Payload))
		return false;
	auto Probe = MakeShared<ManagedOutcomePrivate::FProbe, ESPMode::ThreadSafe>();
	FVaRestOutcomeCallback Adapter;
	FVaRestOutcomeCallback Original = [Capture = MakeUnique<ManagedOutcomePrivate::FCapture>(Probe, F.Control), Probe, &Adapter, &F](FVaRestAttemptOutcome&&) {
		++Probe->Calls;
		Probe->BodyActive.store(true);
		Adapter = nullptr;           // drop the LAST external adapter owner from within its actual callback
		F.Control->StopInputsOnGT(); // consumes parked completion and tries real owned GT reclamation during that method
		Probe->BodyActive.store(false);
	};
	Adapter = Payload->OutcomesOnGT().WrapOnGT(Original);
	if (!TestTrue(TEXT("actual anchored original has a real native adapter"), bool(Adapter)))
		return false;
	Payload->ParkCompletionOnGT(MoveTemp(F.Completion), MoveTemp(F.DeliveryWorker));
	F.Handoff.Reset(); // no fixture-only stack tail may hide a missing adapter method pin during the reentrant Stop
	Adapter(FVaRestAttemptOutcome::Canceled());
	TestFalse(TEXT("actual callback reset its last external adapter"), bool(Adapter));
	TestTrue(TEXT("callback and captured destructor reentered the real private Stop"), F.Control->IsStopped());
	TestEqual(TEXT("actual original invoked once"), Probe->Calls.load(), 1);
	TestEqual(TEXT("actual capture explicitly destroyed before invocation returns"), Probe->Destructions.load(), 1);
	TestFalse(TEXT("method authority prevents callback destruction below its active body"), Probe->DestroyedDuringBody.load());
	TestFalse(TEXT("reentrant capture destruction is GT-affine"), Probe->WrongThread.load());
	TestEqual(TEXT("method return settles the last actual logical authority"), F.Gate->NumOwnedOperations(), uint32(0));
	ManagedOutcomePrivate::Sweep(F);
	TestEqual(TEXT("reentrant Stop leaves no callback/completion cycle"), F.Store->NumOccupied(), uint32(0));
	TestEqual(TEXT("residual whole input released"), F.Control->NumPendingInputs(), uint64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedOutcomeCapacityTest, "VaRest.Http.Managed.OutcomeCapacityAndReuse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedOutcomeCapacityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	NativeOwnership::FFixture F(true);
	auto* Payload = ManagedOutcomePrivate::Initialize(F);
	if (!TestNotNull(TEXT("actual fixed store payload initialized"), Payload))
		return false;
	auto Probe = MakeShared<ManagedOutcomePrivate::FProbe, ESPMode::ThreadSafe>();
	TArray<FVaRestOutcomeCallback> Held;
	constexpr uint32 Available = FManagedOperationSettlement::MaxLogicalTails - 2; // real initial Handoff and Completion
	for (uint32 Index = 0; Index < Available; ++Index)
	{
		FVaRestOutcomeCallback Original = [Probe](FVaRestAttemptOutcome&&) { ++Probe->Calls; };
		auto Adapter = Payload->OutcomesOnGT().WrapOnGT(Original);
		if (!TestTrue(TEXT("callback uses the shared real logical capacity"), bool(Adapter)))
			return false;
		Held.Add(MoveTemp(Adapter));
	}
	FVaRestOutcomeCallback Excess = [Probe](FVaRestAttemptOutcome&&) { ++Probe->Calls; };
	auto Refused = Payload->OutcomesOnGT().WrapOnGT(Excess);
	TestFalse(TEXT("one beyond actual64 logical tails is refused"), bool(Refused));
	TestTrue(TEXT("capacity refusal preserves the caller's actual callback"), bool(Excess));
	Excess = nullptr;
	TUniqueFunction<void()> ExcessTask = [Probe] { ++Probe->Calls; };
	auto RefusedTask = Payload->OutcomesOnGT().WrapTaskOnGT(ExcessTask);
	TestFalse(TEXT("GT task callbacks share that SAME64 logical capacity"), bool(RefusedTask));
	TestTrue(TEXT("task capacity refusal preserves its actual caller callback"), bool(ExcessTask));
	ExcessTask = nullptr;
	Held[0](FVaRestAttemptOutcome::Canceled());
	Held[0](FVaRestAttemptOutcome::Canceled());
	TestEqual(TEXT("retained adapter never invokes the original twice"), Probe->Calls.load(), 1);
	Held.Reset();
	for (uint32 Index = 0; Index < 80; ++Index)
	{
		if ((Index & 1) != 0)
		{
			TUniqueFunction<void()> Original = [Probe] { ++Probe->Calls; };
			auto Adapter = Payload->OutcomesOnGT().WrapTaskOnGT(Original);
			if (!TestTrue(TEXT("task and outcome callbacks reuse the same fixed positions"), bool(Adapter)))
				return false;
			Adapter();
			Adapter();
			Adapter = nullptr;
			continue;
		}
		FVaRestOutcomeCallback Original = [Probe](FVaRestAttemptOutcome&&) { ++Probe->Calls; };
		auto Adapter = Payload->OutcomesOnGT().WrapOnGT(Original);
		if (!TestTrue(TEXT("local fixed cleanup reuses positions beyond64 without a cell/ticker sweep"), bool(Adapter)))
			return false;
		Adapter(FVaRestAttemptOutcome::Canceled());
		Adapter(FVaRestAttemptOutcome::Canceled());
		Adapter = nullptr;
	}
	TestEqual(TEXT("each actual original runs exactly once across reuse"), Probe->Calls.load(), 81);
	F.Store->CloseChildrenOnGT(F.Handoff.Reference());
	FVaRestOutcomeCallback Closed = [Probe](FVaRestAttemptOutcome&&) { ++Probe->Calls; };
	auto AfterClose = Payload->OutcomesOnGT().WrapOnGT(Closed);
	TestFalse(TEXT("child closure admits no replacement logical callback"), bool(AfterClose));
	TestTrue(TEXT("closed refusal also preserves the caller"), bool(Closed));
	Closed = nullptr;
	F.Store->MarkTerminalOnGT(F.Handoff.Reference());
	F.Completion.Reset();
	F.Handoff.Reset();
	ManagedOutcomePrivate::Sweep(F);
	TestEqual(TEXT("fixed callback reuse retains no completed cell history"), F.Store->NumOccupied(), uint32(0));
	TestEqual(TEXT("actual work settled"), F.Gate->NumOwnedOperations(), uint32(0));
	TestEqual(TEXT("actual input released"), F.Control->NumPendingInputs(), uint64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedTaskReentrantTest, "VaRest.Http.Managed.TaskCallbackReentrantStop",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedTaskReentrantTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	NativeOwnership::FFixture F(true);
	auto* Payload = ManagedOutcomePrivate::Initialize(F);
	if (!TestNotNull(TEXT("actual task callback owner initialized"), Payload))
		return false;
	auto Probe = MakeShared<ManagedOutcomePrivate::FProbe, ESPMode::ThreadSafe>();
	TUniqueFunction<void()> Adapter;
	TUniqueFunction<void()> Original = [Capture = MakeUnique<ManagedOutcomePrivate::FCapture>(Probe, F.Control), Probe, &Adapter, &F] {
		++Probe->Calls;
		Probe->BodyActive.store(true);
		Adapter = nullptr;
		F.Control->StopInputsOnGT();
		Probe->BodyActive.store(false);
	};
	Adapter = Payload->OutcomesOnGT().WrapTaskOnGT(Original);
	if (!TestTrue(TEXT("actual void task has one native counted adapter"), bool(Adapter)))
		return false;
	Payload->ParkCompletionOnGT(MoveTemp(F.Completion), MoveTemp(F.DeliveryWorker));
	F.Handoff.Reset();
	Adapter();
	TestFalse(TEXT("task reset its last external callable"), bool(Adapter));
	TestTrue(TEXT("actual task and capture destructor stopped private Control"), F.Control->IsStopped());
	TestEqual(TEXT("actual task invoked once"), Probe->Calls.load(), 1);
	TestEqual(TEXT("actual task capture explicitly destroyed on GTK before method return"), Probe->Destructions.load(), 1);
	TestFalse(TEXT("task method pin spans callback body and owned reentrant cleanup"), Probe->DestroyedDuringBody.load());
	TestFalse(TEXT("task capture destruction stayed GTK"), Probe->WrongThread.load());
	TestEqual(TEXT("method return settles the final real tail"), F.Gate->NumOwnedOperations(), uint32(0));
	ManagedOutcomePrivate::Sweep(F);
	TestEqual(TEXT("task leaves no captured completion cycle"), F.Store->NumOccupied(), uint32(0));
	TestEqual(TEXT("task whole input reclaimed"), F.Control->NumPendingInputs(), uint64(0));
	return true;
}

#endif
