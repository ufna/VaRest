// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestManagedOperation.h"
#include "VaRestClientState.h"
#include "VaRestManagedCompletion.h"
#include "VaRestManagedDispatch.h"
#include "VaRestManagedOutcome.h"
#include "VaRestQueueReplay.h"

namespace VaRest::Http
{
FManagedOperationPayload::FManagedOperationPayload(FNativeOperationRef InRef, FNativeInputBorrow&& InInput)
	: Ref(MoveTemp(InRef))
	, Input(MoveTemp(InInput))
{
	check(Ref.IsValid() && Input.IsValid());
}

FManagedOperationPayload::~FManagedOperationPayload()
{
	check(IsInGameThread() && bDestroyed);
	check(!Input.IsValid() && !ParkedCompletion.IsHeld() && !ParkedDeliveryWorker.IsHeld());
	check(!ParkedNativeCompletion);
	check(!SendConfig && !Exchange);
	check(!Dispatches);
	check(!Outcomes);
	check(!Completion);
	check(!Ref.IsValid());
}

bool FManagedOperationPayload::MatchesOperation(const FNativeOperationRef& Other) const
{
	return Ref.IsValid() && Ref.Control == Other.Control && Ref.Store == Other.Store &&
		   Ref.Cell.Index == Other.Cell.Index && Ref.Cell.Generation == Other.Cell.Generation;
}

FManagedDispatchSlots& FManagedOperationPayload::DispatchesOnGT()
{
	check(IsInGameThread() && !bDestroyed);
	if (!Dispatches)
		Dispatches.Reset(new FManagedDispatchSlots(Ref)); // only an operation reaching dispatch needs its fixed32 table
	return *Dispatches;
}

FManagedOutcomeSlots& FManagedOperationPayload::OutcomesOnGT()
{
	check(IsInGameThread() && !bDestroyed);
	if (!Outcomes)
		Outcomes.Reset(new FManagedOutcomeSlots(Ref));
	return *Outcomes;
}

FManagedCompletionPayload& FManagedOperationPayload::CompletionOnGT()
{
	check(IsInGameThread() && !bDestroyed && Exchange.IsValid());
	if (!Completion)
		Completion.Reset(new FManagedCompletionPayload(Ref, Exchange.Get()));
	return *Completion;
}

void FManagedOperationPayload::ParkCompletionOnGT(FNativeLogicalTailLease&& CompletionLease, FNativeLogicalTailLease&& DeliveryWorker)
{
	check(IsInGameThread() && !bDestroyed && !bCompletionSelected && !ParkedCompletion.IsHeld() && !ParkedNativeCompletion);
	check(&CompletionLease != &DeliveryWorker);
	check(CompletionLease.IsHeld() && MatchesOperation(CompletionLease.Reference()));
	check(!DeliveryWorker.IsHeld() || MatchesOperation(DeliveryWorker.Reference()));
	const bool bStopped = CompletionLease.Reference().Control->IsStopped();
	ParkedCompletion = MoveTemp(CompletionLease);
	ParkedDeliveryWorker = MoveTemp(DeliveryWorker);
	if (bStopped)
		ServiceReadyOnGT(true); // already-owned GT entry only; no late task or normal completion is manufactured
}

bool FManagedOperationPayload::TakeCompletionOnGT(FNativeLogicalTailLease& OutCompletion, FNativeLogicalTailLease& OutDeliveryWorker)
{
	check(IsInGameThread() && !bDestroyed && !OutCompletion.IsHeld() && !OutDeliveryWorker.IsHeld());
	if (&OutCompletion == &OutDeliveryWorker)
		return false; // preserve both actual parked reservations and the one-shot selector
	if (bCompletionSelected || !ParkedCompletion.IsHeld())
		return false;
	if (ParkedCompletion.Reference().Control->IsStopped())
	{
		ServiceReadyOnGT(true);
		return false;
	}
	bCompletionSelected = true;
	OutCompletion = MoveTemp(ParkedCompletion);
	OutDeliveryWorker = MoveTemp(ParkedDeliveryWorker);
	return true;
}

void FManagedOperationPayload::ServiceReadyOnGT(bool bStopped)
{
	check(IsInGameThread() && !bDestroyed);
	const auto NativeCompletion = ParkedNativeCompletion; // full service method pin to the SAME already-counted obligation
	if (NativeCompletion && !bStopped && !Ref.Control->IsStopped())
		NativeCompletion->ServicePendingCancelOnGT();
	if (Dispatches)
		Dispatches->ServiceReadyOnGT(bStopped);
	if (Outcomes)
		Outcomes->ServiceReadyOnGT();
	if (Completion)
		Completion->ServiceReadyOnGT(bStopped || Ref.Control->IsStopped());
	if ((bStopped || Ref.Control->IsStopped()) && Exchange)
		Internals(*Exchange).FileCleanup.Reset(); // only still-parked file permission; an actual worker already took its own
	if ((!bStopped && !Ref.Control->IsStopped()) || bCompletionSelected)
		return;
	bCompletionSelected = true;
	// Only STILL-PARKED reservations belong to this cleanup. A taken completion protects its actual task/call stack.
	ParkedDeliveryWorker.Reset();
	ParkedCompletion.Reset();
	ParkedNativeCompletion.Reset(); // posted/paused/active native references, if any, remain real unsettled authorities
}

void FManagedOperationPayload::DestroyOnGT(bool bDiscarded)
{
	check(IsInGameThread() && !bDestroyed);
	check(!ParkedCompletion.IsHeld() && !ParkedDeliveryWorker.IsHeld());
	check(!ParkedNativeCompletion);
	bDestroyed = true;
	// Destructors may reenter Stop. The outer store holds Control and has already claimed this cell Reclaiming.
	Dispatches.Reset();
	Outcomes.Reset();
	if (Completion)
		Completion->DestroyOnGT(bDiscarded);
	Completion.Reset();
	TSharedPtr<FQueueReplayOperation, ESPMode::ThreadSafe> Replay;
	if (Exchange)
	{
		auto& Ex = Internals(*Exchange);
		Replay = MoveTemp(Ex.QueueReplay);
		if (bDiscarded)
		{
			Ex.bCancelRequested = true;
			Ex.PersistenceAbort->store(true, std::memory_order_relaxed);
			Ex.CancelReason = EVaRestCancelReason::Shutdown;
		}
		if (Ex.State && !Ex.bFinished)
			Ex.State->DropManagedCompletion(Exchange.ToSharedRef());
		Ex.bDone = true; // late owned GTK handle tasks recheck this BEFORE looking at the detached State
		// A custom stage may retain Exchange itself. Strip ACTUAL affine owners here while GTK still owns reclamation;
		// a later native/external last Exchange reference must not publish a new destructor handback after Stop.
		Ex.Interrupt = nullptr;
		Ex.ManagedTerminalObserver = nullptr;
		Ex.OnQueued = nullptr;
		Ex.Handler.Reset();
		Ex.TransportCall.Reset();
		Ex.Body = FVaRestRequestBody();
		Ex.Envelope.Reset();
		Ex.EnvelopeOverride.Reset();
		Ex.PersistenceGuard.Reset();
		Ex.UnionRedactor.Reset();
		Ex.UnionBuiltFrom.Reset();
		Ex.AppliedRedactor.Reset();
		Ex.QueueCapture.Reset();
		Ex.Queue.Reset();
		Ex.Config.Reset();
		Ex.State.Reset();
	}
	Exchange.Reset();
	SendConfig.Reset();
	Input.Finish(bDiscarded);
	Input.Reset(); // last whole-input access; the next owned collector may now delete its residual A/config/captures
	if (Replay) Replay->ManagedAnchorSettledOnGT(Ref, bDiscarded);
	Ref = {};
}
} // namespace VaRest::Http
