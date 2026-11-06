// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Future.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestNativeClientSlots.h"
#include "VaRestNativeOperationStore.h"
#include "VaRestNativeScope.h"
#include "VaRestRequestSpec.h"

namespace VaRest::Http::Testing::NativeOwnership
{
/** Captured as an actual shared owner, so an unexpected retained payload never references expired stack counters. */
struct FProbeState
{
	int32 AnchorCalls = 0;
	int32 CaptureDestructors = 0;
	int32 UserCalls = 0;
	bool bWrongThread = false;
};

inline bool Anchor(FNativeOperationReservation& Claim, const TSharedRef<FProbeState, ESPMode::ThreadSafe>& Probes)
{
	auto Input = Claim.ForkInputAnchor();
	if (!Input.IsValid())
		return false;
	Claim.SetReleaseAnchor([Input = MoveTemp(Input), Probes](bool Discarded) mutable {
		Probes->bWrongThread |= !IsInGameThread();
		++Probes->AnchorCalls;
		Input.Finish(Discarded);
		Input.Reset();
	});
	return true;
}

/**
 * Owns ONLY this fixture's producer, exclusive claim, leases, exact store and input inventory. Every return goes through
 * its GT destructor while all fields/probes still live. It never pumps the global GT queue or fabricates normal terminal
 * or queue/fence completion. The optional native client reservation is never committed/published by these fixtures.
 */
struct FFixture final
{
	explicit FFixture(bool bControlOwnedStore = false)
		: Control(FNativeLifetimeControl::Create())
	{
		if (bControlOwnedStore)
		{
			Scope = FNativeScopeLifetime::Open({}, 0.0);
			FName Error;
			if (Scope && Control->ClientSlotsOnGT().TryReserve(Scope.ToSharedRef(), Client, Error))
			{
				const auto Registration = Client.InitialRegistration();
				Gate = Registration.Gate;
				Store = Registration.Operations;
			}
		}
		else
		{
			Gate = MakeShared<FClientAdmissionGate, ESPMode::ThreadSafe>();
			StandaloneStore = MakeUnique<FNativeOperationStore>(Control, Gate.ToSharedRef());
			Store = StandaloneStore.Get();
		}
	}

	~FFixture()
	{
		check(IsInGameThread());
		if (Producer.IsValid())
			Producer.Wait(); // only this producer's body; remaining captures must be native/inert, not another payload user
		Claim.Abort();
		Children.Reset();
		DeliveryWorker.Reset();
		Completion.Reset();
		Handoff.Reset();
		Work.Release();
		if (Sender.IsValid())
		{
			if (Sender.IsUnpublishedSenderFor(Control.operator->()))
				Sender.PublishReady();
			Sender.Finish(true); // real fixture abandonment, not a successful public completion
			Sender.Reset();
		}
		if (Gate)
			Gate->ClosePermanently();
		if (Store)
			Store->SealOnGT();     // Control owns only its slot stores, not the standalone primitive store
		Control->StopInputsOnGT(); // truthful private native stop; this Control never owns an external operation
		if (Store)
			Store->SweepOnGT(FNativeOperationStore::Capacity);
		for (uint32 Pass = 0; Pass < 16 && Control->NumPendingInputs() != 0; ++Pass)
			Control->CollectInputsOnGT();
		Client.Abort(); // unpublished: no client/queue/probe registration was ever installed by this reservation
		if (Scope)
			Scope->Close(0.0);
		if (Control->NumPendingInputs() == 0 && Control->ClientSlotsOnGT().NumOccupied() == 0)
			Control->DropQuiescentExitHoldForTests(); // all fixture producers ended; no later adoption or active borrower
	}

	FTestRig Rig;
	TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> Control;
	TSharedPtr<FNativeScopeLifetime, ESPMode::ThreadSafe> Scope;
	FNativeClientReservation Client;
	TSharedPtr<FClientAdmissionGate, ESPMode::ThreadSafe> Gate;
	TUniquePtr<FNativeOperationStore> StandaloneStore;
	FNativeOperationStore* Store = nullptr;
	TSharedRef<FProbeState, ESPMode::ThreadSafe> Probes = MakeShared<FProbeState, ESPMode::ThreadSafe>();
	FNativeInputBorrow Sender;
	FClientWorkLease Work;
	FNativeOperationReservation Claim;
	FNativeLogicalTailLease Handoff, Completion, DeliveryWorker;
	TArray<FNativeLogicalTailLease> Children;
	TFuture<void> Producer;
};
} // namespace VaRest::Http::Testing::NativeOwnership

#endif
