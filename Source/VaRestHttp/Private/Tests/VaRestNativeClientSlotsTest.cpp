// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "VaRestNativeClientSlots.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestNativeScope.h"
#include "VaRestRequestSpec.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeClientSlotCapacityTest, "VaRest.Http.Managed.ClientSlotCapacity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeClientSlotCapacityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	auto Control = FNativeLifetimeControl::Create();
	FNativeClientSlots& Slots = Control->ClientSlotsOnGT();
	TArray<TSharedPtr<FNativeScopeLifetime, ESPMode::ThreadSafe>> Scopes;
	TArray<FNativeClientReservation> Reservations;
	Reservations.AddDefaulted(Slots.Capacity);
	FName Error;
	for (uint32 Group = 0; Group < 4; ++Group)
	{
		auto Scope = FNativeScopeLifetime::Open({}, 0.0);
		if (!TestTrue(TEXT("fixture owns a real native scope"), Scope.IsValid()))
			return false;
		Scopes.Add(Scope);
		for (uint32 Index = 0; Index < Slots.MaxPerScope; ++Index)
		{
			if (!TestTrue(TEXT("prepublication reservation fits exact per-scope inventory"), Slots.TryReserve(Scope.ToSharedRef(), Reservations[Group * Slots.MaxPerScope + Index], Error)))
				return false;
		}
		FNativeClientReservation Refused;
		TestFalse(TEXT("65th reserved or live client refuses within one scope"), Slots.TryReserve(Scope.ToSharedRef(), Refused, Error));
		TestEqual(TEXT("scope capacity reports normal finite refusal"), Error, FName(TEXT("LifecycleCapacity")));
		TestFalse(TEXT("refusal has no published registration"), Refused.IsValid());
	}
	auto ExtraScope = FNativeScopeLifetime::Open({}, 0.0);
	FNativeClientReservation Extra;
	TestEqual(TEXT("four scopes occupy the exact process capacity"), Slots.NumOccupied(), Slots.Capacity);
	TestFalse(TEXT("257th client refuses even in an empty scope"), Slots.TryReserve(ExtraScope.ToSharedRef(), Extra, Error));
	Reservations.Reset(); // actual exclusive reservation destructors unwind on GT
	TestEqual(TEXT("unpublished failures release every client slot without a retirement history"), Slots.NumOccupied(), uint32(0));
	TestTrue(TEXT("released capacity is reusable before creating a client"), Slots.TryReserve(ExtraScope.ToSharedRef(), Extra, Error));
	Extra.Abort();
	for (auto& Scope : Scopes)
		Scope->Close(0.0);
	ExtraScope->Close(0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeClientReservationTest, "VaRest.Http.Managed.ClientReservationUnwind",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeClientReservationTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	auto Control = FNativeLifetimeControl::Create();
	FNativeClientSlots& Slots = Control->ClientSlotsOnGT();
	auto Scope = FNativeScopeLifetime::Open({}, 0.0);
	FNativeClientReservation Reservation;
	FName Error;
	if (!TestTrue(TEXT("slot is reserved before any client is observable"), Slots.TryReserve(Scope.ToSharedRef(), Reservation, Error)))
		return false;
	const auto Old = Reservation.InitialRegistration();
	TestTrue(TEXT("initial registration already contains a gate and fixed operation store"), Old.Gate.IsValid() && Old.Operations != nullptr);
	TestTrue(TEXT("close can reenter while Create still owns its reservation"), Slots.BeginRetirement(Old));
	TestTrue(TEXT("reentrant close permanently seals the initial gate"), Old.Gate->IsClosed());
	TestFalse(TEXT("reserved Create cannot be recycled underneath its paused caller"), Slots.TryReleaseQuiescent(Old));
	Reservation.Abort();
	TestEqual(TEXT("exclusive construction unwind releases the now-quiescent slot"), Slots.NumOccupied(), uint32(0));
	TestFalse(TEXT("old construction identity cannot close another row"), Slots.BeginRetirement(Old));
	for (uint32 Index = 1; Index < Slots.Capacity; ++Index)
	{
		FNativeClientReservation Intervening;
		if (!TestTrue(TEXT("fixture advances through the real fixed inventory before reuse"), Slots.TryReserve(Scope.ToSharedRef(), Intervening, Error)))
			return false;
		Intervening.Abort();
	}
	if (!TestTrue(TEXT("quiescent inventory admits the replacement"), Slots.TryReserve(Scope.ToSharedRef(), Reservation, Error)))
		return false;
	const auto Current = Reservation.InitialRegistration();
	TestEqual(TEXT("fixture reuses the same client index"), Current.Client.Index, Old.Client.Index);
	TestTrue(TEXT("actual client generation advances before reuse"), Current.Client.Generation != Old.Client.Generation);
	TestFalse(TEXT("old-generation close cannot mutate the replacement gate"), Slots.BeginRetirement(Old));
	TestFalse(TEXT("replacement gate remains open after a stale close"), Current.Gate->IsClosed());
	Reservation.Abort();
	Slots.BeginStop();
	TestFalse(TEXT("stop refuses new client construction before any state publication"), Slots.TryReserve(Scope.ToSharedRef(), Reservation, Error));
	TestEqual(TEXT("stop refusal is explicit"), Error, FName(TEXT("LifecycleClosed")));
	Scope->Close(0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeClientWeakRegistryTest, "VaRest.Http.Managed.ClientSlotWeakRegistry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeClientWeakRegistryTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	auto Control = FNativeLifetimeControl::Create();
	FNativeClientSlots& Slots = Control->ClientSlotsOnGT();
	auto Scope = FNativeScopeLifetime::Open({}, 0.0);
	FNativeClientReservation Reservation;
	FName Error;
	if (!TestTrue(TEXT("real native client construction reserves its slot first"), Slots.TryReserve(Scope.ToSharedRef(), Reservation, Error)))
		return false;
	const auto Registration = Reservation.InitialRegistration();
	FClientConfig Config = *FClientAccess::State(Rig.Client)->GetConfig();
	Config.ScopeLifetime = Scope;
	Config.ScopeId = Scope->GetId();
	TSharedPtr<FClientState, ESPMode::ThreadSafe> Client = FClientState::Create(MoveTemp(Config));
	TWeakPtr<FClientState, ESPMode::ThreadSafe> Weak = Client;
	TestTrue(TEXT("matching real client completes the construction reservation"), Reservation.CommitClient(Client.ToSharedRef()));
	TestTrue(TEXT("current exact generation resolves its weak client"), Slots.PinClient(Registration) == Client);
	Client.Reset();
	TestFalse(TEXT("registry does not become an internal strong client owner"), Weak.IsValid());
	TestFalse(TEXT("expired weak client returns no replacement or wrapper"), Slots.PinClient(Registration).IsValid());
	Slots.BeginRetirement(Registration);
	TestTrue(TEXT("no queue/native obligations in this fixture; quiescent closing slot releases"), Slots.TryReleaseQuiescent(Registration));
	TestFalse(TEXT("released registration no longer addresses a client row"), Slots.PinClient(Registration).IsValid());
	Scope->Close(0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeClientGenerationTest, "VaRest.Http.Managed.ClientGenerationExhaustion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeClientGenerationTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	auto Control = FNativeLifetimeControl::Create();
	FNativeClientSlots& Slots = Control->ClientSlotsOnGT();
	Slots.SeedFreeGenerationsForTests(MAX_uint64);
	auto Scope = FNativeScopeLifetime::Open({}, 0.0);
	FName Error;
	for (uint32 Index = 0; Index < Slots.Capacity; ++Index)
	{
		FNativeClientReservation Reservation;
		if (!TestTrue(TEXT("fixture takes a real last-generation client reservation"), Slots.TryReserve(Scope.ToSharedRef(), Reservation, Error)))
			return false;
		const auto Old = Reservation.InitialRegistration();
		TestEqual(TEXT("published identity is actually the final uint64 generation"), Old.Client.Generation, uint64(MAX_uint64));
		Reservation.Abort();
		TestFalse(TEXT("retired final-generation identity cannot regain authority"), Slots.BeginRetirement(Old));
	}
	FNativeClientReservation Refused;
	TestEqual(TEXT("exhausted generations retain no client/scope payload"), Slots.NumOccupied(), uint32(0));
	TestFalse(TEXT("exhausted process inventory fails closed instead of wrapping a generation"), Slots.TryReserve(Scope.ToSharedRef(), Refused, Error));
	TestEqual(TEXT("nonwrapping exhaustion has a finite capacity result"), Error, FName(TEXT("LifecycleCapacity")));
	Scope->Close(0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeCapturedStorageTest, "VaRest.Http.Managed.CapturedControlStorage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeCapturedStorageTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("CapturedControlStorage requires an actual worker; skipped without multithreading support"));
		return true;
	}
	FTestRig Rig;
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Control = FNativeLifetimeControl::Create();
	TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> WeakControl = Control;
	auto Scope = FNativeScopeLifetime::Open({}, 0.0);
	FNativeClientReservation ClientReservation;
	FName Error;
	if (!TestTrue(TEXT("Control owns storage before client publication"), Control->ClientSlotsOnGT().TryReserve(Scope.ToSharedRef(), ClientReservation, Error)))
		return false;
	auto Registration = ClientReservation.InitialRegistration();
	FClientConfig Config = *FClientAccess::State(Rig.Client)->GetConfig();
	Config.ScopeLifetime = Scope;
	Config.ScopeId = Scope->GetId();
	auto Client = FClientState::Create(MoveTemp(Config));
	ClientReservation.CommitClient(Client);
	auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(Client, {});
	auto Sender = Control->AdoptInput(Payload);
	FClientWorkLease Work;
	Registration.Gate->TryAcquire(Work);
	FNativeOperationReservation Operation;
	Registration.Operations->TryClaim(Work, Sender, Operation);
	int32 AnchorCalls = 0;
	bool WrongThread = false;
	auto InputAnchor = Operation.ForkInputAnchor();
	Operation.SetReleaseAnchor([Input = MoveTemp(InputAnchor), &AnchorCalls, &WrongThread](bool Discarded) mutable {
		WrongThread |= !IsInGameThread();
		++AnchorCalls;
		Input.Finish(Discarded);
		Input.Reset();
	});
	auto Initial = Operation.Publish();
	auto Physical = Registration.Operations->TryAddDispatchOnGT(Initial.Reference());
	Registration.Operations->MarkTerminalOnGT(Initial.Reference());
	Initial.Reset();
	Control->ClientSlotsOnGT().BeginRetirement(Registration);
	Registration.Control.Reset();
	Control.Reset(); // only the actual admitted token/input anchor retain the Control-owned allocation now
	std::atomic<bool> RanOffGT{false};
	auto Completed = Async(EAsyncExecution::ThreadPool, [Held = MoveTemp(Physical), &RanOffGT]() mutable {
		RanOffGT.store(!IsInGameThread());
		Held.Reset();
	});
	Completed.Wait();
	TestTrue(TEXT("actual late physical method ran on a worker"), RanOffGT.load());
	TestEqual(TEXT("worker cannot destroy the captured GT anchor"), AnchorCalls, 0);
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Kept = WeakControl.Pin();
	if (!TestTrue(TEXT("captured Control still owns the real slot and cell array"), Kept.IsValid()))
		return false;
	Registration.Control = Kept;
	const auto Sweep = Kept->ClientSlotsOnGT().SweepOperations();
	TestTrue(TEXT("global client and operation scan bounds are actual visit counts"), Sweep.ClientsInspected <= 256 && Sweep.OperationCellsInspected <= 256);
	TestEqual(TEXT("actual Control sweep destroys the one ready affine anchor on GT"), AnchorCalls, 1);
	TestFalse(TEXT("late callback never becomes the GT anchor's destruction thread"), WrongThread);
	Kept->CollectInputsOnGT();
	TestEqual(TEXT("the captured whole-input ownership also reaches actual GT deletion"), Kept->NumPendingInputs(), uint64(0));
	TestTrue(TEXT("only actual quiescence releases the client slot"), Kept->ClientSlotsOnGT().TryReleaseQuiescent(Registration));
	Kept->StopInputsOnGT();
	TestFalse(TEXT("fully quiescent Control needs no exit self-retention"), Kept->IsExitRetentionRequested());
	Registration.Control.Reset();
	Kept.Reset();
	TestFalse(TEXT("normal completion retains no empty-Control history"), WeakControl.IsValid());
	Scope->Close(0.0);
	return true;
}

#endif
