// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestManagedOperation.h"
#include "VaRestNativeOwnershipFixture.h"

namespace VaRest::Http::Testing::ManagedPayloadPrivate
{
struct FDestructionProbe
{
	explicit FDestructionProbe(TSharedRef<NativeOwnership::FProbeState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState))
	{
	}
	~FDestructionProbe()
	{
		++State->CaptureDestructors;
		State->bWrongThread |= !IsInGameThread();
	}
	TSharedRef<NativeOwnership::FProbeState, ESPMode::ThreadSafe> State;
};

TUniquePtr<FRequestSpec> Input(NativeOwnership::FFixture& F)
{
	return FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(F.Rig.Client),
		[Capture = MakeShared<FDestructionProbe, ESPMode::ThreadSafe>(F.Probes), State = F.Probes](const FString&) { ++State->UserCalls; });
}

FManagedOperationPayload* Initialize(NativeOwnership::FFixture& F)
{
	auto* Payload = F.Claim.CreateManagedPayload(); // unique owner installed BEFORE any affine move/allocation
	if (!Payload)
		return nullptr;
	Payload->SendConfig = FClientAccess::State(F.Rig.Client)->GetConfig();
	Payload->Exchange = MakeShared<FVaRestExchange, ESPMode::ThreadSafe>();
	auto& Ex = Internals(*Payload->Exchange);
	Ex.State = FClientAccess::State(F.Rig.Client);
	Ex.Config = Payload->SendConfig;
	Ex.OnQueued = [Capture = MakeShared<FDestructionProbe, ESPMode::ThreadSafe>(F.Probes), State = F.Probes](const FString&) { ++State->UserCalls; };
	return Payload;
}
} // namespace VaRest::Http::Testing::ManagedPayloadPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedParkedCompletionTest, "VaRest.Http.Managed.PayloadParkedCompletionStop",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedParkedCompletionTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	NativeOwnership::FFixture F(true);
	if (!TestNotNull(TEXT("actual Control owns the reserved fixed store"), F.Store))
		return false;
	auto Input = ManagedPayloadPrivate::Input(F);
	F.Sender = F.Control->AdoptInput(Input);
	F.Gate->TryAcquire(F.Work);
	if (!TestTrue(TEXT("counted work claims all three initial reservations"), F.Store->TryClaim(F.Work, F.Sender, F.Claim, 3)))
		return false;
	auto* Payload = ManagedPayloadPrivate::Initialize(F);
	if (!TestNotNull(TEXT("whole residual input and actual exchange have a typed owner"), Payload))
		return false;
	F.Handoff = F.Claim.Publish(F.Completion, F.DeliveryWorker);
	if (!TestTrue(TEXT("initial authority is actually published"), F.Handoff.IsHeld()))
		return false;
	TestTrue(TEXT("held initial authority reaches actual anchored payload"), F.Handoff.ManagedPayloadOnGT() == Payload);
	Payload->ParkCompletionOnGT(MoveTemp(F.Completion), MoveTemp(F.DeliveryWorker));
	F.Handoff.Reset();
	TestEqual(TEXT("parked completion retains admitted work"), F.Gate->NumOwnedOperations(), uint32(1));
	TestEqual(TEXT("actual affine captures have not been destroyed"), F.Probes->CaptureDestructors, 0);
	F.Control->StopInputsOnGT();
	TestEqual(TEXT("Stop breaks internal parked-completion cycle"), F.Gate->NumOwnedOperations(), uint32(0));
	TestEqual(TEXT("GT reclaimed actual cell, not just its count"), F.Store->NumOccupied(), uint32(0));
	TestEqual(TEXT("actual residual and exchange captures both destroyed"), F.Probes->CaptureDestructors, 2);
	TestFalse(TEXT("actual payload destruction stayed on GT"), F.Probes->bWrongThread);
	TestEqual(TEXT("discard does not fabricate normal queued callback"), F.Probes->UserCalls, 0);
	TestEqual(TEXT("residual whole input deleted too"), F.Control->NumPendingInputs(), uint64(0));
	return true; // F aborts only the UNPUBLISHED client reservation and its known empty private Control
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedTakenCompletionTest, "VaRest.Http.Managed.PayloadTakenCompletionStop",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedTakenCompletionTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	for (uint32 Count = 2; Count <= 3; ++Count)
	{
		NativeOwnership::FFixture F(true);
		if (!TestNotNull(TEXT("Control reserves actual store allocation"), F.Store))
			return false;
		auto Input = ManagedPayloadPrivate::Input(F);
		F.Sender = F.Control->AdoptInput(Input);
		F.Gate->TryAcquire(F.Work);
		if (!TestTrue(TEXT("all initial permissions preadmitted"), F.Store->TryClaim(F.Work, F.Sender, F.Claim, Count)))
			return false;
		auto* Payload = ManagedPayloadPrivate::Initialize(F);
		if (!TestNotNull(TEXT("actual payload owner installed before publication"), Payload))
			return false;
		F.Handoff = F.Claim.Publish(F.Completion, F.DeliveryWorker);
		if (!TestTrue(TEXT("initial handoff published"), F.Handoff.IsHeld()))
			return false;
		TestTrue(TEXT("initial authority exposes same actual payload"), F.Handoff.ManagedPayloadOnGT() == Payload);
		Payload->ParkCompletionOnGT(MoveTemp(F.Completion), MoveTemp(F.DeliveryWorker));
		FNativeLogicalTailLease SameOutput;
		if (!TestFalse(TEXT("aliased Take refuses BEFORE selecting/moving Completion"), Payload->TakeCompletionOnGT(SameOutput, SameOutput)))
			return false;
		TestFalse(TEXT("alias refusal leaves caller's output empty"), SameOutput.IsHeld());
		if (!TestTrue(TEXT("distinct retry still takes the real reserved Completion"), Payload->TakeCompletionOnGT(F.Completion, F.DeliveryWorker)))
			return false;
		if (!TestTrue(TEXT("successful Take actually retains Completion"), F.Completion.IsHeld()))
			return false;
		TestEqual(TEXT("worker exists only for three-tail shape"), F.DeliveryWorker.IsHeld(), Count == 3);
		TestTrue(TEXT("taken Completion protects actual payload pointer"), F.Completion.ManagedPayloadOnGT() == Payload);
		FNativeLogicalTailLease DuplicateCompletion, DuplicateWorker;
		TestFalse(TEXT("second selector cannot manufacture authority"), Payload->TakeCompletionOnGT(DuplicateCompletion, DuplicateWorker));
		F.Handoff.Reset();
		F.Control->StopInputsOnGT();
		TestEqual(TEXT("Stop cannot consume actual task/stack Completion"), F.Gate->NumOwnedOperations(), uint32(1));
		TestEqual(TEXT("exact generation remains occupied"), F.Store->NumOccupied(), uint32(1));
		TestEqual(TEXT("Stop has not destroyed borrowed affine owners"), F.Probes->CaptureDestructors, 0);
		F.Completion.Reset();
		TestEqual(TEXT("optional worker independently retains work"), F.Gate->NumOwnedOperations(), Count == 3 ? uint32(1) : uint32(0));
		F.DeliveryWorker.Reset();
		TestEqual(TEXT("last authority consumes stopped work lease"), F.Gate->NumOwnedOperations(), uint32(0));
		TestEqual(TEXT("native settlement alone does not destroy payload"), F.Probes->CaptureDestructors, 0);
		for (uint32 Pass = 0; Pass < 8 && F.Store->NumOccupied() != 0; ++Pass)
			F.Control->ClientSlotsOnGT().SweepOperations();
		F.Control->CollectInputsOnGT();
		TestEqual(TEXT("bounded cursor eventually reclaims same cell"), F.Store->NumOccupied(), uint32(0));
		TestEqual(TEXT("actual callback owners each destroyed once on GT"), F.Probes->CaptureDestructors, 2);
		TestFalse(TEXT("neither capture died off GT"), F.Probes->bWrongThread);
		TestEqual(TEXT("stopped cleanup invokes no normal callback"), F.Probes->UserCalls, 0);
		TestEqual(TEXT("whole residual input reclaimed"), F.Control->NumPendingInputs(), uint64(0));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeFixtureFailureCleanupTest, "VaRest.Http.Managed.OwnedFixtureFailureCleanup",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeFixtureFailureCleanupTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	for (uint32 Mode = 0; Mode < 2; ++Mode)
	{
		const bool bOwnedStore = Mode != 0;
		auto Observed = MakeShared<NativeOwnership::FProbeState, ESPMode::ThreadSafe>();
		TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> WeakControl;
		{
			NativeOwnership::FFixture F(bOwnedStore);
			F.Probes = Observed;
			WeakControl = F.Control;
			if (!TestNotNull(TEXT("failure-path fixture owns exact store"), F.Store))
				return false;
			auto Input = ManagedPayloadPrivate::Input(F);
			F.Sender = F.Control->AdoptInput(Input);
			F.Gate->TryAcquire(F.Work);
			if (!bOwnedStore)
			{
				TestFalse(TEXT("real refusal leaves already-adopted input for fixture-owned teardown"), F.Store->TryClaim(F.Work, F.Sender, F.Claim, 0));
			}
			else
			{
				if (!TestTrue(TEXT("real exclusive claim precedes stopped-anchor refusal"), F.Store->TryClaim(F.Work, F.Sender, F.Claim, 3)))
					return false;
				F.Control->StopInputsOnGT();
				TestNull(TEXT("actual stopped Control refuses the new anchor"), F.Claim.CreateManagedPayload());
			}
			// No manual Sender/Claim/Work reset/sweep/collection: same scope-exit path as an early failed assertion return.
		}
		TestFalse(TEXT("own failed setup leaves neither pending input nor retained Control"), WeakControl.IsValid());
		TestEqual(TEXT("actual failed-setup capture destroyed exactly once"), Observed->CaptureDestructors, 1);
		TestFalse(TEXT("failed-setup actual capture destroyed on GT"), Observed->bWrongThread);
		TestEqual(TEXT("failure cleanup never invents callback success"), Observed->UserCalls, 0);
	}
	return true;
}

#endif
