// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "VaRestNativeOperationStore.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestRequestSpec.h"

namespace VaRest::Http::Testing::OperationStorePrivate
{
void Anchor(FNativeOperationReservation& Reservation, int32& Calls, bool& WrongThread)
{
	auto Input = Reservation.ForkInputAnchor();
	Reservation.SetReleaseAnchor([Input = MoveTemp(Input), &Calls, &WrongThread](bool Discarded) mutable {
		WrongThread |= !IsInGameThread();
		++Calls;
		Input.Finish(Discarded);
		Input.Reset();
	});
}
void Collect(FNativeLifetimeControl& Control)
{
	for (uint32 Pass = 0; Pass < 16 && Control.NumPendingInputs() != 0; ++Pass)
		Control.CollectInputsOnGT();
}
} // namespace VaRest::Http::Testing::OperationStorePrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeOperationCellTest, "VaRest.Http.Managed.OperationCellAuthority",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeOperationCellTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("OperationCellAuthority requires an actual worker; skipped without multithreading support"));
		return true;
	}
	FTestRig Rig;
	auto Control = FNativeLifetimeControl::Create();
	auto Gate = MakeShared<FClientAdmissionGate, ESPMode::ThreadSafe>();
	FNativeOperationStore Store(Control, Gate);
	int32 AnchorCalls = 0;
	bool WrongThread = false;
	auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(Rig.Client), {});
	auto Sender = Control->AdoptInput(Payload);
	FClientWorkLease Work;
	Gate->TryAcquire(Work);
	FNativeOperationReservation Reservation;
	if (!TestTrue(TEXT("counted native work claims a fixed cell"), Store.TryClaim(Work, Sender, Reservation)))
		return false;
	OperationStorePrivate::Anchor(Reservation, AnchorCalls, WrongThread);
	auto Initial = Reservation.Publish();
	const FNativeOperationRef Old = Initial.Reference();
	auto Logical = Store.TryAddLogicalTailOnGT(Old);
	auto Physical = Store.TryAddDispatchOnGT(Old);
	TestTrue(TEXT("extra logical and physical work own actual authorities"), Logical.IsHeld() && Physical.IsHeld());
	Gate->ClosePermanently();
	Store.SealOnGT();
	TestTrue(TEXT("terminal selection addresses its exact generation"), Store.MarkTerminalOnGT(Old));
	Initial.Reset();
	Logical.Reset();
	TestEqual(TEXT("outstanding physical authority retains admitted work"), Gate->NumOwnedOperations(), uint32(1));
	TestEqual(TEXT("GT cannot reclaim an unsettled physical cell"), Store.SweepOnGT(Store.Capacity).Reclaimed, uint32(0));
	std::atomic<bool> RanOffGT{false};
	auto Released = Async(EAsyncExecution::ThreadPool, [Held = MoveTemp(Physical), &RanOffGT]() mutable {
		RanOffGT.store(!IsInGameThread());
		Held.Reset();
	});
	Released.Wait();
	TestTrue(TEXT("physical settlement callable actually ran off GT"), RanOffGT.load());
	TestEqual(TEXT("actual physical method settles its gate lease"), Gate->NumOwnedOperations(), uint32(0));
	TestEqual(TEXT("worker settlement never destroys the GT release anchor"), AnchorCalls, 0);
	auto NextGate = MakeShared<FClientAdmissionGate, ESPMode::ThreadSafe>();
	TestFalse(TEXT("zero operation count alone cannot recycle unreclaimed storage"), Store.ResetForNextClientOnGT(NextGate));
	TestEqual(TEXT("GT reclaims exactly one completed authoritative cell"), Store.SweepOnGT(Store.Capacity).Reclaimed, uint32(1));
	TestEqual(TEXT("release anchor runs exactly once on GT"), AnchorCalls, 1);
	TestFalse(TEXT("GT anchor is not moved into the worker tail"), WrongThread);
	TestTrue(TEXT("only quiescent reclaimed storage may bind a fresh gate"), Store.ResetForNextClientOnGT(NextGate));
	auto NextPayload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(Rig.Client), {});
	auto NextSender = Control->AdoptInput(NextPayload);
	FClientWorkLease NextWork;
	NextGate->TryAcquire(NextWork);
	FNativeOperationReservation Next;
	Store.TryClaim(NextWork, NextSender, Next);
	OperationStorePrivate::Anchor(Next, AnchorCalls, WrongThread);
	auto NextInitial = Next.Publish();
	const FNativeOperationRef Current = NextInitial.Reference();
	TestEqual(TEXT("fixture actually reuses the same fixed index"), Current.Cell.Index, Old.Cell.Index);
	TestTrue(TEXT("reuse advances generation rather than aliasing old authority"), Current.Cell.Generation != Old.Cell.Generation);
	TestFalse(TEXT("stale old-generation terminal cannot mutate replacement"), Store.MarkTerminalOnGT(Old));
	TestEqual(TEXT("stale old generation leaves replacement admission intact"), NextGate->NumOwnedOperations(), uint32(1));
	Store.MarkTerminalOnGT(Current);
	NextInitial.Reset();
	Store.SweepOnGT(Store.Capacity);
	OperationStorePrivate::Collect(*Control);
	TestEqual(TEXT("all native cell anchors released"), AnchorCalls, 2);
	TestEqual(TEXT("store has no completed-cell history"), Store.NumOccupied(), uint32(0));
	TestEqual(TEXT("whole input payloads also finish GT reclamation"), Control->NumPendingInputs(), uint64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeOperationCapacityTest, "VaRest.Http.Managed.OperationCellCapacity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeOperationCapacityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	auto Control = FNativeLifetimeControl::Create();
	auto Gate = MakeShared<FClientAdmissionGate, ESPMode::ThreadSafe>();
	FNativeOperationStore Store(Control, Gate);
	int32 AnchorCalls = 0;
	bool WrongThread = false;
	for (uint32 Index = 0; Index < Store.Capacity; ++Index)
	{
		auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(Rig.Client), {});
		auto Sender = Control->AdoptInput(Payload);
		FClientWorkLease Work;
		Gate->TryAcquire(Work);
		FNativeOperationReservation Reservation;
		Store.TryClaim(Work, Sender, Reservation);
		OperationStorePrivate::Anchor(Reservation, AnchorCalls, WrongThread);
		auto Initial = Reservation.Publish();
		Store.MarkTerminalOnGT(Initial.Reference());
	}
	TestEqual(TEXT("logical settlement does not pretend GT cell cleanup occurred"), Gate->NumOwnedOperations(), uint32(0));
	TestEqual(TEXT("all fixed cell anchors still await GT reclamation"), Store.NumOccupied(), Store.Capacity);
	auto ExtraPayload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(Rig.Client), {});
	auto ExtraSender = Control->AdoptInput(ExtraPayload);
	FClientWorkLease ExtraWork;
	Gate->TryAcquire(ExtraWork);
	FNativeOperationReservation Refused;
	TestFalse(TEXT("cell exhaustion refuses without overflow allocation"), Store.TryClaim(ExtraWork, ExtraSender, Refused));
	TestTrue(TEXT("cell refusal preserves caller work and whole input ownership"), ExtraWork.IsHeld() && ExtraSender.IsValid());
	ExtraWork.Release();
	ExtraSender.PublishReady();
	ExtraSender.Finish(true);
	ExtraSender.Reset();
	TestEqual(TEXT("GT fixed sweep reclaims exactly its occupied capacity"), Store.SweepOnGT(Store.Capacity).Reclaimed, Store.Capacity);
	OperationStorePrivate::Collect(*Control);
	TestEqual(TEXT("capacity fixture destroys every admitted anchor on GT"), AnchorCalls, int32(Store.Capacity));
	TestFalse(TEXT("capacity reclamation never runs on sender"), WrongThread);
	TestEqual(TEXT("no cell or refusal payload remains"), Control->NumPendingInputs(), uint64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeOperationAbortTest, "VaRest.Http.Managed.OperationClaimUnwind",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeOperationAbortTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("OperationClaimUnwind requires an actual worker; skipped without multithreading support"));
		return true;
	}
	FTestRig Rig;
	auto Control = FNativeLifetimeControl::Create();
	auto Gate = MakeShared<FClientAdmissionGate, ESPMode::ThreadSafe>();
	FNativeOperationStore Store(Control, Gate);
	int32 AnchorCalls = 0;
	bool WrongThread = false;
	std::atomic<bool> Claimed{false};
	std::atomic<bool> RanOffGT{false};
	auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(Rig.Client), {});
	auto Unwound = Async(EAsyncExecution::ThreadPool, [Control, Gate, &Store, &AnchorCalls, &WrongThread, &Claimed, &RanOffGT, Payload = MoveTemp(Payload)]() mutable {
		RanOffGT.store(!IsInGameThread());
		auto Sender = Control->AdoptInput(Payload);
		FClientWorkLease Work;
		Gate->TryAcquire(Work);
		FNativeOperationReservation Reservation;
		Claimed.store(Store.TryClaim(Work, Sender, Reservation));
		OperationStorePrivate::Anchor(Reservation, AnchorCalls, WrongThread);
		// Abort destructor readies the whole input before release-publishing its abandoned cell for GT cleanup.
	});
	Unwound.Wait();
	TestTrue(TEXT("exclusive claim and unwind callable actually ran off GT"), RanOffGT.load());
	TestTrue(TEXT("worker initialized a real exclusive claim"), Claimed.load());
	TestEqual(TEXT("unpublished unwind cannot run affine anchor on worker"), AnchorCalls, 0);
	TestEqual(TEXT("abandoned work waits for actual GT terminal processing"), Gate->NumOwnedOperations(), uint32(1));
	Store.SweepOnGT(Store.Capacity);
	OperationStorePrivate::Collect(*Control);
	TestEqual(TEXT("GT cleanup settles unpublished native work"), Gate->NumOwnedOperations(), uint32(0));
	TestEqual(TEXT("abandoned exact anchor releases once"), AnchorCalls, 1);
	TestFalse(TEXT("abort anchor destruction remains GT"), WrongThread);
	TestEqual(TEXT("unwind leaves no cell"), Store.NumOccupied(), uint32(0));
	TestEqual(TEXT("unwind leaves no input payload"), Control->NumPendingInputs(), uint64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeOperationGenerationTest, "VaRest.Http.Managed.OperationGenerationExhaustion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeOperationGenerationTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	auto Control = FNativeLifetimeControl::Create();
	auto Gate = MakeShared<FClientAdmissionGate, ESPMode::ThreadSafe>();
	FNativeOperationStore Store(Control, Gate);
	Store.SeedFreeGenerationsForTests(MAX_uint64 >> 2);
	int32 AnchorCalls = 0;
	bool WrongThread = false;
	for (uint32 Index = 0; Index < Store.Capacity; ++Index)
	{
		auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(Rig.Client), {});
		auto Sender = Control->AdoptInput(Payload);
		FClientWorkLease Work;
		Gate->TryAcquire(Work);
		FNativeOperationReservation Reservation;
		Store.TryClaim(Work, Sender, Reservation);
		OperationStorePrivate::Anchor(Reservation, AnchorCalls, WrongThread);
		auto Initial = Reservation.Publish();
		TestEqual(TEXT("actual published handle is at its final nonzero generation"), Initial.Reference().Cell.Generation, MAX_uint64 >> 2);
		Store.MarkTerminalOnGT(Initial.Reference());
		Initial.Reset();
		Store.SweepOnGT(Store.Capacity);
	}
	auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(Rig.Client), {});
	auto Sender = Control->AdoptInput(Payload);
	FClientWorkLease Work;
	Gate->TryAcquire(Work);
	FNativeOperationReservation Refused;
	TestFalse(TEXT("exhausted generations retire cells rather than wrap or alias"), Store.TryClaim(Work, Sender, Refused));
	TestTrue(TEXT("generation refusal preserves unconsumed caller authorities"), Work.IsHeld() && Sender.IsValid());
	Work.Release();
	Sender.PublishReady();
	Sender.Finish(true);
	Sender.Reset();
	OperationStorePrivate::Collect(*Control);
	TestEqual(TEXT("retired cells retain no completed payload"), Store.NumOccupied(), uint32(0));
	TestEqual(TEXT("exhaustion fixture releases actual inputs"), Control->NumPendingInputs(), uint64(0));
	TestFalse(TEXT("generation cleanup anchors stay on GT"), WrongThread);
	return true;
}

#endif
