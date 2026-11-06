// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "VaRestManagedCompletion.h"
#include "VaRestManagedOperation.h"
#include "VaRestNativeOwnershipFixture.h"

namespace VaRest::Http::Testing::ManagedCancelPrivate
{
struct FCapture
{
	explicit FCapture(TSharedRef<NativeOwnership::FProbeState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState))
	{
	}
	~FCapture()
	{
		++State->CaptureDestructors;
		State->bWrongThread |= !IsInGameThread();
	}
	TSharedRef<NativeOwnership::FProbeState, ESPMode::ThreadSafe> State;
};
FManagedOperationPayload* Prepare(NativeOwnership::FFixture& F, uint32 InitialTails = 2)
{
	if (!F.Store)
		return nullptr;
	auto Input = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(F.Rig.Client),
		[Capture = MakeUnique<FCapture>(F.Probes), Seen = F.Probes](const FString&) { ++Seen->UserCalls; });
	F.Sender = F.Control->AdoptInput(Input);
	if (!F.Gate->TryAcquire(F.Work) || !F.Store->TryClaim(F.Work, F.Sender, F.Claim, InitialTails))
		return nullptr;
	auto* Owner = F.Claim.CreateManagedPayload();
	if (!Owner)
		return nullptr;
	Owner->SendConfig = FClientAccess::State(F.Rig.Client)->GetConfig();
	Owner->Exchange = MakeShared<FVaRestExchange, ESPMode::ThreadSafe>();
	auto& Ex = Internals(*Owner->Exchange);
	Ex.Managed = Owner->Reference();
	Ex.State = FClientAccess::State(F.Rig.Client);
	Ex.Config = Owner->SendConfig;
	Ex.RequestId = FVaRestTrace::NewRequestId();
	// Actual pre-Enter foundation state; a GTK cancel records reason/atomic input, Enter remains the initial Handoff's work.
	F.Handoff = F.Claim.PublishManaged();
	return F.Handoff.IsHeld() ? Owner : nullptr;
}
} // namespace VaRest::Http::Testing::ManagedCancelPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedCancelOnGTTest, "VaRest.Http.Managed.CancelActualGTKBeforeEnter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedCancelOnGTTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	NativeOwnership::FFixture F(true);
	auto* Owner = ManagedCancelPrivate::Prepare(F);
	if (!TestNotNull(TEXT("real published native Completion and residual input owner"), Owner))
		return false;
	auto& Ex = Internals(*Owner->Exchange);
	const FVaRestRequestHandle Handle(Owner->Exchange.ToSharedRef());
	TestTrue(TEXT("immutable weak route is backed by actual parked native Completion"), Ex.NativeCompletion.IsValid());
	Handle.Cancel(); // REAL public method reaches REAL native Access and State::Cancel, never a modeled cancel predicate
	TestTrue(TEXT("public managed cancel immediately sets actual atomic input"), Owner->Exchange->IsCancelRequested());
	TestEqual(TEXT("real GTK control plane accepted actual User cancellation"), Ex.CancelReason, EVaRestCancelReason::User);
	TestEqual(TEXT("pre-Enter actual phase is preserved"), Ex.Phase, EVaRestExchangePhase::Created);
	TestEqual(TEXT("cancel itself invents no terminal/user callback"), F.Probes->UserCalls, 0);
	F.Handoff.Reset();
	TestEqual(TEXT("initial Completion alone retains admitted operation"), F.Gate->NumOwnedOperations(), uint32(1));
	F.Control->StopInputsOnGT();
	F.Store->SweepOnGT(FNativeOperationStore::Capacity);
	F.Control->CollectInputsOnGT();
	TestEqual(TEXT("Stop releases only parked shared native owner without an internal cycle"), F.Gate->NumOwnedOperations(), uint32(0));
	TestEqual(TEXT("actual cell reclaimed"), F.Store->NumOccupied(), uint32(0));
	TestEqual(TEXT("actual residual callback capture destroyed GTK"), F.Probes->CaptureDestructors, 1);
	TestFalse(TEXT("no worker affine destruction"), F.Probes->bWrongThread);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedSharedCompletionCapacityTest, "VaRest.Http.Managed.SharedInitialCompletionAtCapacity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedSharedCompletionCapacityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	for (uint32 Initial = 2; Initial <= 3; ++Initial)
	{
		NativeOwnership::FFixture F(true);
		auto* Owner = ManagedCancelPrivate::Prepare(F, Initial);
		if (!TestNotNull(TEXT("real native shared publication"), Owner))
			return false;
		const auto Ref = F.Handoff.Reference();
		for (uint32 I = Initial; I < 64; ++I)
		{
			auto Child = F.Store->TryAddLogicalTailOnGT(Ref);
			if (!TestTrue(TEXT("actual remaining initial-inclusive capacity"), Child.IsHeld()))
				return false;
			F.Children.Add(MoveTemp(Child));
		}
		TestFalse(TEXT("no extra sixty-fifth permission"), F.Store->TryAddLogicalTailOnGT(Ref).IsHeld());
		TestFalse(TEXT("legacy selector does not consume a different parked shared owner"), F.Store->TakeInitialCompletionOnGT(Ref, F.Completion, F.DeliveryWorker));
		TSharedPtr<FManagedCompletionAccess, ESPMode::ThreadSafe> Selected;
		if (!TestTrue(TEXT("shared selector transfers SAME initial native authority at64"), F.Store->TakeManagedCompletionOnGT(Ref, Selected, F.DeliveryWorker)))
			return false;
		if (!TestTrue(TEXT("actual shared access is held"), Selected.IsValid()))
			return false;
		TestTrue(TEXT("selected native owner protects actual unique operation"), &Selected->OperationOnGT() == Owner);
		TestEqual(TEXT("optional existing worker matches original shape"), F.DeliveryWorker.IsHeld(), Initial == 3);
		TSharedPtr<FManagedCompletionAccess, ESPMode::ThreadSafe> Again;
		FNativeLogicalTailLease Extra;
		TestFalse(TEXT("one-shot selector cannot create another completion"), F.Store->TakeManagedCompletionOnGT(Ref, Again, Extra));
		F.Children.Reset();
		F.Handoff.Reset();
		F.DeliveryWorker.Reset();
		F.Control->StopInputsOnGT();
		TestEqual(TEXT("Stop cannot steal taken native task/stack authority"), F.Gate->NumOwnedOperations(), uint32(1));
		TestEqual(TEXT("actual residual remains alive through selected authority"), F.Probes->CaptureDestructors, 0);
		Selected.Reset(); // no typed frame was ever bound; only native authority ends, affine owners still wait for GTK sweep
		TestEqual(TEXT("last actual native owner settles counted operation"), F.Gate->NumOwnedOperations(), uint32(0));
		TestEqual(TEXT("native settlement alone never destroys actual affine capture"), F.Probes->CaptureDestructors, 0);
		F.Store->SweepOnGT(FNativeOperationStore::Capacity);
		F.Control->CollectInputsOnGT();
		TestEqual(TEXT("actual callback capture destroyed once on GTK"), F.Probes->CaptureDestructors, 1);
		TestEqual(TEXT("actual cell and input reclaimed"), F.Store->NumOccupied(), uint32(0));
		TestEqual(TEXT("actual pending input zero"), F.Control->NumPendingInputs(), uint64(0));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedStoppedCancelInputTest, "VaRest.Http.Managed.StoppedWorkerCancelAtomicInput",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedStoppedCancelInputTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("requires an actual separate native worker"));
		return true;
	}
	NativeOwnership::FFixture F(true);
	auto* Owner = ManagedCancelPrivate::Prepare(F);
	if (!TestNotNull(TEXT("actual native shared owner"), Owner))
		return false;
	const FVaRestRequestHandle Handle(Owner->Exchange.ToSharedRef());
	F.Control->StopInputsOnGT();
	TestFalse(TEXT("Stop actually dropped the only parked native Completion owner"), Internals(*Owner->Exchange).NativeCompletion.IsValid());
	TestEqual(TEXT("actual initial Handoff still retains the exact row"), F.Gate->NumOwnedOperations(), uint32(1));
	auto WorkerOffGT = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	F.Producer = Async(EAsyncExecution::Thread, [Handle, WorkerOffGT] {
		WorkerOffGT->store(!IsInGameThread(), std::memory_order_relaxed);
		Handle.Cancel();
	});
	F.Producer.Wait(); // own body and method locals ended; remaining closure captures are weak/native, not affine
	TestTrue(TEXT("actual public Cancel ran on a separate native worker"), WorkerOffGT->load(std::memory_order_relaxed));
	TestTrue(TEXT("stopped Cancel still records actual immediate atomic input"), Owner->Exchange->IsCancelRequested());
	TestEqual(TEXT("no GTK state was touched by stopped worker"), Internals(*Owner->Exchange).CancelReason, EVaRestCancelReason::None);
	TestEqual(TEXT("actual affine residual not destroyed by worker"), F.Probes->CaptureDestructors, 0);
	F.Handoff.Reset();
	F.Store->SweepOnGT(FNativeOperationStore::Capacity);
	F.Control->CollectInputsOnGT();
	TestEqual(TEXT("owned stopped input reclaimed GTK after real method ended"), F.Probes->CaptureDestructors, 1);
	TestFalse(TEXT("actual destruction stayed GTK"), F.Probes->bWrongThread);
	TestEqual(TEXT("exact cell count zero"), F.Store->NumOccupied(), uint32(0));
	// No scheduler queue census, LIVE background-post/once/discard progress or weak-memory proof is claimed by this case.
	return true;
}

#endif
