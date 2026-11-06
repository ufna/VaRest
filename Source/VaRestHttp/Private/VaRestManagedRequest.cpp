// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Tasks/Task.h"
#include "UObject/StrongObjectPtr.h"
#include "VaRestClientState.h"
#include "VaRestManagedOperation.h"
#include "VaRestRequestSpec.h"
#include "VaRestQueueReplay.h"

namespace VaRest::Http
{
void FRequestSpecAccess::QueueManagedRefusal(FNativeInputBorrow&& Sender, FName Reason, const FLlmSenderTags& Tags)
{
	check(Sender.IsValid());
	auto& Spec = Sender.Get();
	const auto Control = Spec.State->GetManagedLifetime().Control;
	Spec.NativeRefusalReason = Reason;
	auto Notification = Sender.ForkBeforePublication();
	auto Worker = Spec.bOnHttpThread && Spec.Handler && !Spec.Handler->RequiresGameThreadDelivery() ? Sender.ForkBeforePublication() : FNativeInputBorrow();
	const bool bReadyLive = Sender.PublishReady();
	Sender.Finish(!bReadyLive);
	Sender.Reset(); // all remaining payload access belongs to existing borrows and the Control-owned whole node
	if (!bReadyLive || Control->IsStopped() || !Notification.IsValid())
		return; // no new task after observing Stop; native borrows drop, actual affine payload remains owned for GT
	UE::Tasks::Launch(TEXT("VaRest.ManagedRefusal"), [Held = MoveTemp(Notification), DeliveryWorker = MoveTemp(Worker), Control, Tags]() mutable {
		auto& Owned = Held.Get();
		if (Control->IsStopped() || FClientState::IsShuttingDown() || !Owned.Handler ||
			(Owned.bHasOwner && !Owned.Owner.IsValid()))
		{
			Owned.Handler.Reset(); // suppression does not delegate actual handler destruction to a worker/task destructor
			return;
		}
		Owned.NativeRefusalFinish = MakeUnique<FFinish>();
		auto& Failed = *Owned.NativeRefusalFinish;
		Failed.Error.Kind = EVaRestErrorKind::Config;
		Failed.Error.Tag = Owned.Tag;
		Failed.Error.Message = Owned.NativeRefusalReason == TEXT("ClientClosed")
			? TEXT("The managed client is closed; this request was not admitted")
			: TEXT("Managed request capacity is exhausted; this request was not admitted");
		if (Owned.Handler->GetCompletionPolicy()==EResultCompletionPolicy::LaterFrame)
		{
			TUniqueFunction<void()> Deliver=[Borrow=MoveTemp(Held),Control,Tags]() mutable {
				auto& Input=Borrow.Get();
				if (!Control->IsStopped() && !FClientState::IsShuttingDown() && Input.Handler &&
					(!Input.bHasOwner || Input.Owner.IsValid()))
				{
					VAREST_LLM_SCOPE_SENDER(Tags);
					Input.Handler->Finalize(*Input.NativeRefusalFinish);
					if (!Control->IsStopped() && !FClientState::IsShuttingDown() && (!Input.bHasOwner || Input.Owner.IsValid())) Input.Handler->Deliver(*Input.NativeRefusalFinish);
				}
				Input.Handler.Reset();Input.NativeRefusalFinish.Reset();
			};
			// The same preborrowed whole input is now owned by the reserved GTK frame, including suppression/drop.
			Owned.Handler->DeferCompletionOnGT(Deliver);
			return;
		}
		// No exchange or resolved URL exists. Do not consult current configuration or expose an unadmitted raw path.
		if (!Owned.bOnHttpThread || Owned.Handler->RequiresGameThreadDelivery())
		{
			{
				VAREST_LLM_SCOPE_SENDER(Tags);
				Owned.Handler->Deliver(Failed);
			}
			Owned.Handler.Reset();
			Owned.NativeRefusalFinish.Reset();
			return;
		}
		check(DeliveryWorker.IsValid()); // preborrowed before Ready/Finish, never a late input acquisition
		TStrongObjectPtr<const UObject> Pin;
		if (Owned.bHasOwner)
			Pin.Reset(Owned.Owner.Get());
		auto Work = UE::Tasks::Launch(TEXT("VaRest.ManagedRefusalWorker"),
			[Borrow = MoveTemp(DeliveryWorker), Control, Tags, Kept = MoveTemp(Pin)]() mutable {
				auto& Input = Borrow.Get();
				if (!Control->IsStopped() && !FClientState::IsShuttingDown())
				{
					VAREST_LLM_SCOPE_SENDER(Tags);
					Input.Handler->Deliver(*Input.NativeRefusalFinish);
				}
				Kept.Reset();
				Borrow.Reset(); // actual Handler/Finish stay in the whole GTK-owned node, never this worker's captures
			});
		// This handback is created HERE on GT with the already-owned input borrow, before GT can enter Stop.
		UE::Tasks::Launch(TEXT("VaRest.ManagedRefusalCleanup"), [Borrow = MoveTemp(Held)]() mutable {
			auto& Input = Borrow.Get();
			Input.Handler.Reset();
			Input.NativeRefusalFinish.Reset();
			Borrow.Reset(); // last input access; the owned collector may now destroy residual historical config/body/State
		}, UE::Tasks::Prerequisites(Work), UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri); }, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
}

FVaRestRequestHandle FRequestSpecAccess::SendManaged(TUniquePtr<FRequestSpec>&& Spec, const char* SourceFile, uint32 SourceLine)
{
	check(Spec && Spec->State && Spec->State->GetManagedLifetime().IsValid());
	FLlmSenderTags Tags;
	Tags.Capture();
	LLM_SCOPE_BYTAG(VaRest);
	const auto& Registration = Spec->State->GetManagedLifetime();
	const auto Control = Registration.Control; // native allocation owner only; never a sender-local affine config snapshot
	auto Sender = Control->AdoptInput(Spec);   // caller is null BEFORE any consuming mutation/current-config read
	FClientWorkLease Work;
	if (!Registration.Gate->TryAcquire(Work))
	{
		QueueManagedRefusal(MoveTemp(Sender), Registration.Gate->IsClosed() ? TEXT("ClientClosed") : TEXT("ClientCapacity"), Tags);
		return {};
	}
	const uint32 InitialTails = Sender.Get().bOnHttpThread ? 3 : 2;
	FNativeOperationReservation Claim;
	if (!Registration.Operations->TryClaim(Work, Sender, Claim, InitialTails))
	{
		Work.Release(); // tentative count rolls back; no cell/exchange/request identity was published
		QueueManagedRefusal(MoveTemp(Sender), Registration.Gate->IsClosed() ? TEXT("ClientClosed") : TEXT("OperationCellCapacity"), Tags);
		return {};
	}
	auto* Payload = Claim.CreateManagedPayload(); // unique actual owner installed while Claimed BEFORE affine moves
	if (!Payload)
		return {}; // Stop barred the input anchor; Claim abort is native-only and leaves whole input owned for GT
	auto& Input = Claim.Input();
	Payload->Exchange = MakeShared<FVaRestExchange, ESPMode::ThreadSafe>();
	auto& Ex = Internals(*Payload->Exchange);
	Payload->SendConfig = Input.State->SnapshotForSend(Ex.QueueCapture, Input.bQueueWhenOffline);
	Ex.Managed = Payload->Reference();
	Ex.RequestId = Input.PresetRequestId != 0 ? Input.PresetRequestId : FVaRestTrace::NewRequestId();
	Ex.ClientId = Input.State->GetClientId();
	Ex.Config = Payload->SendConfig;
	InitializeExchange(Input, Ex, Tags, SourceFile, SourceLine);
	FVaRestRequestHandle Handle(Payload->Exchange.ToSharedRef());
	if (Ex.QueueReplay) Ex.QueueReplay->BindManagedBeforePublication(Ex.Managed, Handle);
	auto Handoff = Claim.PublishManaged();
	check(Handoff.IsHeld());
	if (Control->IsStopped())
		return Handle; // all EXISTING authorities settle native discarded; no late Enter/notification/service lookup
	if (IsInGameThread())
		EnterManaged(MoveTemp(Handoff), true);
	else
		UE::Tasks::Launch(TEXT("VaRest.ManagedEnter"), [Entering = MoveTemp(Handoff)]() mutable { EnterManaged(MoveTemp(Entering), false); }, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	// No payload/registration/reference dereference after publishing Enter: GT may already have completed/reclaimed it.
	return Handle;
}

void FRequestSpecAccess::EnterManaged(FNativeLogicalTailLease&& Handoff, FNativeLogicalTailLease&& Completion,
	FNativeLogicalTailLease&& DeliveryWorker, bool bSentOnGT)
{
	check(IsInGameThread() && Handoff.IsHeld());
	auto* Payload = Handoff.ManagedPayloadOnGT();
	check(Payload);
	Payload->ParkCompletionOnGT(MoveTemp(Completion), MoveTemp(DeliveryWorker));
	EnterManaged(MoveTemp(Handoff), bSentOnGT);
}

void FRequestSpecAccess::EnterManaged(FNativeLogicalTailLease&& Handoff, bool bSentOnGT)
{
	check(IsInGameThread() && Handoff.IsHeld());
	const auto Control = Handoff.Reference().Control;
	auto* Payload = Handoff.ManagedPayloadOnGT();
	check(Payload);
	if (Control->IsStopped())
		return;
	const auto Entering = Payload->Exchange.ToSharedRef();
	auto& Ex = Internals(*Entering);
	Ex.bSentOnGameThread = bSentOnGT;
	if (Ex.State->GetManagedLifetime().Gate->IsClosed())
	{
		Ex.CancelReason = EVaRestCancelReason::User;
		Ex.bCancelRequested.store(true, std::memory_order_relaxed);
		Ex.PersistenceAbort->store(true, std::memory_order_relaxed);
	}
	Ex.State->Enter(Entering); // actual initial authority spans the whole active Enter stack and its GTK locals
}
} // namespace VaRest::Http
