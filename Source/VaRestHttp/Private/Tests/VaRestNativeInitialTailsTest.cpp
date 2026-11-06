// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "VaRestNativeOwnershipFixture.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeInitialClosureTest, "VaRest.Http.Managed.InitialTailsBeforeTerminal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeInitialClosureTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	for (uint32 Count = 2; Count <= 3; ++Count)
	{
		NativeOwnership::FFixture F;
		auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(F.Rig.Client), {});
		F.Sender = F.Control->AdoptInput(Payload);
		F.Gate->TryAcquire(F.Work);
		if (!TestTrue(TEXT("managed initial reservations claim before publication"), F.Store->TryClaim(F.Work, F.Sender, F.Claim, Count)))
			return false; // F owns exact store/input cleanup on this return too
		if (!TestTrue(TEXT("actual input anchor is available before publication"), NativeOwnership::Anchor(F.Claim, F.Probes)))
			return false;
		F.Handoff = F.Claim.Publish(F.Completion, F.DeliveryWorker);
		if (!TestTrue(TEXT("handoff and completion are distinct actual authorities"), F.Handoff.IsHeld() && F.Completion.IsHeld()))
			return false;
		TestEqual(TEXT("only OnHttpThread reserves delivery worker"), F.DeliveryWorker.IsHeld(), Count == 3);
		const auto Ref = F.Handoff.Reference();
		TestTrue(TEXT("final outcome can close children before terminal bookkeeping"), F.Store->CloseChildrenOnGT(Ref));
		TestFalse(TEXT("closed children refuse another logical launch"), F.Store->TryAddLogicalTailOnGT(Ref).IsHeld());
		TestFalse(TEXT("closed children refuse another physical start"), F.Store->TryAddDispatchOnGT(Ref).IsHeld());
		F.Handoff.Reset();
		F.Completion.Reset();
		F.DeliveryWorker.Reset();
		TestEqual(TEXT("child closure and zero tails alone do not invent a terminal"), F.Gate->NumOwnedOperations(), uint32(1));
		TestEqual(TEXT("nonterminal cell cannot be reclaimed"), F.Store->SweepOnGT(F.Store->Capacity).Reclaimed, uint32(0));
		TestTrue(TEXT("real terminal selection remains separate"), F.Store->MarkTerminalOnGT(Ref));
		TestEqual(TEXT("real terminal now settles the admitted work"), F.Gate->NumOwnedOperations(), uint32(0));
		TestEqual(TEXT("settlement has not destroyed the affine payload"), F.Probes->AnchorCalls, 0);
		TestEqual(TEXT("GT reclaims the exact completed generation"), F.Store->SweepOnGT(F.Store->Capacity).Reclaimed, uint32(1));
		TestEqual(TEXT("payload destruction occurs exactly once"), F.Probes->AnchorCalls, 1);
		TestFalse(TEXT("initial reservations never move cleanup off GT"), F.Probes->bWrongThread);
		F.Control->CollectInputsOnGT();
		TestEqual(TEXT("residual whole input is actually collected"), F.Control->NumPendingInputs(), uint64(0));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeInitialCapacityTest, "VaRest.Http.Managed.InitialTailsShareCapacity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeInitialCapacityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	for (uint32 Count = 2; Count <= 3; ++Count)
	{
		NativeOwnership::FFixture F;
		auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(F.Rig.Client), {});
		F.Sender = F.Control->AdoptInput(Payload);
		F.Gate->TryAcquire(F.Work);
		TestFalse(TEXT("zero initial reservations refuse without consuming owners"), F.Store->TryClaim(F.Work, F.Sender, F.Claim, 0));
		TestFalse(TEXT("unsupported initial shape refuses without consuming owners"), F.Store->TryClaim(F.Work, F.Sender, F.Claim, 4));
		TestTrue(TEXT("invalid shape leaves actual work/input with caller"), F.Work.IsHeld() && F.Sender.IsValid() && !F.Claim.IsValid());
		if (!TestTrue(TEXT("valid managed shape claims after earlier refusals"), F.Store->TryClaim(F.Work, F.Sender, F.Claim, Count)))
			return false;
		if (!TestTrue(TEXT("actual capacity input anchor is installed"), NativeOwnership::Anchor(F.Claim, F.Probes)))
			return false;
		F.Handoff = F.Claim.Publish(F.Completion, F.DeliveryWorker);
		if (!TestTrue(TEXT("managed publication owns its initial handoff"), F.Handoff.IsHeld()))
			return false;
		const auto Ref = F.Handoff.Reference();
		for (uint32 Index = Count; Index < FManagedOperationSettlement::MaxLogicalTails; ++Index)
		{
			auto Child = F.Store->TryAddLogicalTailOnGT(Ref);
			TestTrue(TEXT("remaining capacity admits an actual child"), Child.IsHeld());
			F.Children.Add(MoveTemp(Child));
		}
		TestFalse(TEXT("initial reservations count toward the same exact64 cap"), F.Store->TryAddLogicalTailOnGT(Ref).IsHeld());
		F.Store->MarkTerminalOnGT(Ref);
		F.Children.Reset();
		F.Handoff.Reset();
		TestEqual(TEXT("completion protects operation after initial stack ends"), F.Gate->NumOwnedOperations(), uint32(1));
		F.Completion.Reset();
		if (Count == 3)
		{
			TestEqual(TEXT("actual delivery worker is independent of completion"), F.Gate->NumOwnedOperations(), uint32(1));
			TestEqual(TEXT("GT cannot reclaim while reserved worker survives"), F.Store->SweepOnGT(F.Store->Capacity).Reclaimed, uint32(0));
		}
		F.DeliveryWorker.Reset();
		TestEqual(TEXT("last actual reservation releases work exactly once"), F.Gate->NumOwnedOperations(), uint32(0));
		TestEqual(TEXT("GT then destroys the completed payload"), F.Store->SweepOnGT(F.Store->Capacity).Reclaimed, uint32(1));
		TestEqual(TEXT("one anchor and no offGT destruction"), F.Probes->AnchorCalls, 1);
		TestFalse(TEXT("payload stayed on GT"), F.Probes->bWrongThread);
		F.Control->CollectInputsOnGT();
		TestEqual(TEXT("capacity test leaves no whole input"), F.Control->NumPendingInputs(), uint64(0));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeInitialAbortTest, "VaRest.Http.Managed.InitialTailsUnwind",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeInitialAbortTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("InitialTailsUnwind requires an actual worker; skipped without multithreading support"));
		return true;
	}
	std::atomic<bool> Claimed{false}, RanOffGT{false}, Anchored{false};
	NativeOwnership::FFixture F;
	auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(F.Rig.Client), {});
	F.Producer = Async(EAsyncExecution::ThreadPool, [Control = F.Control, Gate = F.Gate, Store = F.Store, Probes = F.Probes, &Claimed, &RanOffGT, &Anchored, Payload = MoveTemp(Payload)]() mutable {
		RanOffGT.store(!IsInGameThread());
		auto Sender = Control->AdoptInput(Payload);
		FClientWorkLease Work;
		Gate->TryAcquire(Work);
		FNativeOperationReservation Claim;
		Claimed.store(Store->TryClaim(Work, Sender, Claim, 3));
		if (Claim.IsValid())
			Anchored.store(NativeOwnership::Anchor(Claim, Probes));
		// Local Claim unwinds BEFORE the void promise; remaining queued-function captures are native/inert.
	});
	F.Producer.Wait();
	TestTrue(TEXT("claim/unwind actually executed off GT"), RanOffGT.load());
	TestTrue(TEXT("worker claimed a real three-tail cell"), Claimed.load());
	TestTrue(TEXT("worker installed the real whole-input anchor"), Anchored.load());
	TestEqual(TEXT("unwind did not destroy the payload on worker"), F.Probes->AnchorCalls, 0);
	TestEqual(TEXT("live abort awaits genuine GT discarded processing"), F.Gate->NumOwnedOperations(), uint32(1));
	TestEqual(TEXT("all initial authority drops permit actual GT reclamation"), F.Store->SweepOnGT(F.Store->Capacity).Reclaimed, uint32(1));
	TestEqual(TEXT("multi-tail unwind leaves no hidden admitted count"), F.Gate->NumOwnedOperations(), uint32(0));
	TestEqual(TEXT("unwind anchor runs once on GT"), F.Probes->AnchorCalls, 1);
	TestFalse(TEXT("worker is never last owner of affine payload"), F.Probes->bWrongThread);
	F.Control->CollectInputsOnGT();
	TestEqual(TEXT("no cell remains after three-tail abort"), F.Store->NumOccupied(), uint32(0));
	TestEqual(TEXT("no whole input remains after three-tail abort"), F.Control->NumPendingInputs(), uint64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeInitialPublishRefusalTest, "VaRest.Http.Managed.InitialTailsPublicationRefusal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeInitialPublishRefusalTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	NativeOwnership::FFixture F;
	auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(F.Rig.Client), {});
	F.Sender = F.Control->AdoptInput(Payload);
	F.Gate->TryAcquire(F.Work);
	if (!TestTrue(TEXT("claim reserves all three initial owners"), F.Store->TryClaim(F.Work, F.Sender, F.Claim, 3)))
		return false;
	if (!TestTrue(TEXT("publication-refusal input anchor is installed"), NativeOwnership::Anchor(F.Claim, F.Probes)))
		return false;
	FNativeLogicalTailLease SameOutput;
	auto Refused = F.Claim.Publish(SameOutput, SameOutput);
	const bool bAliasRefused = !Refused.IsHeld() && !SameOutput.IsHeld();
	TestTrue(TEXT("aliased publication exposes no authority"), bAliasRefused);
	if (!TestTrue(TEXT("alias refusal preserves exclusive claim for retry or owned abort"), F.Claim.IsValid()) || !bAliasRefused)
		return false;
	TestEqual(TEXT("alias refusal preserves actual claimed admission"), F.Gate->NumOwnedOperations(), uint32(1));
	F.Store->SealOnGT();
	F.Handoff = F.Claim.Publish(F.Completion, F.DeliveryWorker);
	TestFalse(TEXT("sealed publication exposes no initial authority"), F.Handoff.IsHeld() || F.Completion.IsHeld() || F.DeliveryWorker.IsHeld());
	TestFalse(TEXT("sealed refusal consumes the exclusive reservation"), F.Claim.IsValid());
	TestEqual(TEXT("refused publication does not invoke affine anchor on sender"), F.Probes->AnchorCalls, 0);
	TestEqual(TEXT("actual GT sweep reclaims all unexposed reservations"), F.Store->SweepOnGT(F.Store->Capacity).Reclaimed, uint32(1));
	TestEqual(TEXT("publication refusal leaves no admission leak"), F.Gate->NumOwnedOperations(), uint32(0));
	TestEqual(TEXT("publication refusal destroys payload once"), F.Probes->AnchorCalls, 1);
	TestFalse(TEXT("publication refusal payload remains GT"), F.Probes->bWrongThread);
	F.Control->CollectInputsOnGT();
	TestEqual(TEXT("publication refusal leaves no whole input"), F.Control->NumPendingInputs(), uint64(0));
	return true;
}

#endif
