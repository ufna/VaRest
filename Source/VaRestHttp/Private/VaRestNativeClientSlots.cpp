// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeClientSlots.h"
#include "VaRestClientState.h"
#include "VaRestManagedRefreshDelivery.h"
#include "VaRestNativeScope.h"
#include "VaRestOfflineInternal.h"

namespace VaRest::Http
{
FNativeQueueBindingReservation::~FNativeQueueBindingReservation()
{
	Abort();
}
FNativeQueueBindingReservation::FNativeQueueBindingReservation(FNativeQueueBindingReservation&& Other) noexcept
	: Slots(Other.Slots)
	, Control(MoveTemp(Other.Control))
	, ClientIndex(Other.ClientIndex)
	, ClientGeneration(Other.ClientGeneration)
	, Generation(Other.Generation)
	, DrainIndex(Other.DrainIndex)
	, bResumePending(Other.bResumePending)
{
	Other.Slots = nullptr;
}
FNativeQueueBindingReservation& FNativeQueueBindingReservation::operator=(FNativeQueueBindingReservation&& Other) noexcept
{
	if (this != &Other)
	{
		Abort();
		Slots = Other.Slots;
		Control = MoveTemp(Other.Control);
		ClientIndex = Other.ClientIndex;
		ClientGeneration = Other.ClientGeneration;
		Generation = Other.Generation;
		DrainIndex = Other.DrainIndex;
		bResumePending = Other.bResumePending;
		Other.Slots = nullptr;
	}
	return *this;
}
bool FNativeQueueBindingReservation::Commit(TSharedRef<FOfflineQueue, ESPMode::ThreadSafe> Queue,
	TSharedRef<FQueueBinding, ESPMode::ThreadSafe> Binding)
{
	check(IsInGameThread());
	if (!Slots || !Slots->CommitQueueBinding(*this, MoveTemp(Queue), MoveTemp(Binding)))
		return false;
	Slots = nullptr;
	Control.Reset();
	return true;
}
bool FNativeQueueBindingReservation::ParkForCurrentTarget()
{
	check(IsInGameThread());
	if (!Slots || !Slots->ParkQueueBinding(*this))
		return false;
	Slots = nullptr;
	Control.Reset();
	return true;
}
void FNativeQueueBindingReservation::Abort()
{
	if (!Slots)
		return;
	check(IsInGameThread());
	const auto Kept = Control;
	FNativeClientSlots* Releasing = Slots;
	Slots = nullptr;
	Releasing->AbortQueueBinding(*this);
	Control.Reset(); // exact allocation stays owned until AFTER registry mutation/destruction callouts return
}

FNativeClientReservation::~FNativeClientReservation()
{
	Abort();
}
FNativeClientReservation::FNativeClientReservation(FNativeClientReservation&& Other) noexcept
	: Slots(Other.Slots)
	, Registration(MoveTemp(Other.Registration))
{
	Other.Slots = nullptr;
	Other.Registration = {};
}
FNativeClientReservation& FNativeClientReservation::operator=(FNativeClientReservation&& Other) noexcept
{
	if (this != &Other)
	{
		Abort();
		Slots = Other.Slots;
		Registration = MoveTemp(Other.Registration);
		Other.Slots = nullptr;
		Other.Registration = {};
	}
	return *this;
}
bool FNativeClientReservation::CommitClient(TSharedRef<FClientState, ESPMode::ThreadSafe> Client)
{
	check(IsInGameThread());
	if (!Slots || !Slots->Commit(Registration, MoveTemp(Client)))
		return false;
	Slots = nullptr;
	Registration = {};
	return true;
}
void FNativeClientReservation::Abort()
{
	if (!Slots)
		return;
	check(IsInGameThread());
	FNativeClientSlots* Releasing = Slots;
	Slots = nullptr;
	FNativeClientRegistration Kept = MoveTemp(Registration);
	Registration = {};
	Releasing->Abandon(Kept); // Kept captures Control until this registry method has returned
}

FNativeClientSlots::FNativeClientSlots(TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> InOwner)
	: Owner(InOwner)
{
	check(IsInGameThread());
	Slots = MakeUnique<FSlot[]>(Capacity);
}
FNativeClientSlots::~FNativeClientSlots()
{
	// An empty captured Control can die off GT; no affine scope/client/anchor survives this point.
	check(Occupied == 0);
}
bool FNativeClientSlots::TryReserve(TSharedRef<FNativeScopeLifetime, ESPMode::ThreadSafe> Scope,
	FNativeClientReservation& Out, FName& Error)
{
	check(IsInGameThread());
	Error = NAME_None;
	auto Kept = Owner.Pin();
	if (!Kept || bStopping || Kept->IsStopped() || !Scope->AcceptsClientsOnGT() || Out.IsValid())
	{
		Error = TEXT("LifecycleClosed");
		return false;
	}
	const uint32 InScope = Scope->NumOwnedClientsOnGT();
	if (Occupied >= Capacity || InScope >= MaxPerScope)
	{
		Error = TEXT("LifecycleCapacity");
		return false;
	}
	for (uint32 Offset = 0; Offset < Capacity; ++Offset)
	{
		const uint32 Index = (NextReservation + Offset) % Capacity;
		FSlot& Slot = Slots[Index];
		if (Slot.Phase != EPhase::Free)
			continue;
		if (Slot.RefreshDelivery && !Slot.RefreshDelivery->IsQuiescentOnGT())
			continue;
		auto Gate = MakeShared<FClientAdmissionGate, ESPMode::ThreadSafe>();
		if (!Scope->TryAddClientOnGT(Kept.ToSharedRef()))
		{
			Error = TEXT("LifecycleClosed");
			return false;
		}
		if (Slot.Operations)
		{
			if (!Slot.Operations->ResetForNextClientOnGT(Gate))
			{
				Scope->ReleaseClientOnGT(!Kept->IsStopped());
				continue;
			}
		}
		else
			Slot.Operations = MakeUnique<FNativeOperationStore>(Kept.ToSharedRef(), Gate);
		if (Slot.RefreshDelivery)
		{
			if (!Slot.RefreshDelivery->ResetForNextClientOnGT(Gate))
			{
				Slot.Phase = EPhase::Exhausted;
				Scope->ReleaseClientOnGT(!Kept->IsStopped());
				continue;
			} // fail closed rather than recycle partially reset storage
		}
		else
			Slot.RefreshDelivery = MakeUnique<FManagedRefreshDelivery>(Kept.ToSharedRef(), Gate);
		Slot.Gate = Gate;
		Slot.Scope = Scope;
		Slot.bCloseRequested = false;
		Slot.bNonOperationQuiescent = false;
		Slot.bLimiterPumpPending = false;
		Slot.Phase = EPhase::Reserved;
		++Occupied;
		NextReservation = (Index + 1) % Capacity;
		Out.Slots = this;
		Out.Registration = {MoveTemp(Kept), MoveTemp(Gate), Slot.Operations.Get(), {static_cast<uint16>(Index), Slot.Generation}};
		return true;
	}
	Error = TEXT("LifecycleCapacity"); // exhausted nonwrapping generations also refuse before client creation
	return false;
}
FNativeClientSlots::FSlot* FNativeClientSlots::Find(const FNativeClientRegistration& Registration) const
{
	if (!Registration.IsValid() || Registration.Control != Owner.Pin() || Registration.Client.Index >= Capacity)
		return nullptr;
	FSlot& Slot = Slots[Registration.Client.Index];
	if (Slot.Generation != Registration.Client.Generation || Slot.Phase == EPhase::Free || Slot.Phase == EPhase::Exhausted ||
		Slot.Gate != Registration.Gate || Slot.Operations.Get() != Registration.Operations)
		return nullptr;
	return &Slot;
}
bool FNativeClientSlots::Commit(const FNativeClientRegistration& Registration, TSharedRef<FClientState, ESPMode::ThreadSafe> Client)
{
	check(IsInGameThread());
	FSlot* Slot = Find(Registration);
	if (!Slot || Slot->Phase != EPhase::Reserved || Client->GetScopeId() != Slot->Scope->GetId() ||
		bStopping || Registration.Control->IsStopped() || Slot->Gate->IsClosed() || !Slot->Scope->AcceptsClientsOnGT())
		return false;
	Slot->Client = Client;
	Slot->Phase = Slot->bCloseRequested ? EPhase::Closing : EPhase::Live;
	return true;
}
void FNativeClientSlots::Close(FSlot& Slot)
{
	Slot.bLimiterPumpPending = false;
	Slot.Gate->ClosePermanently();
	Slot.Operations->SealOnGT();
	Slot.bCloseRequested = true;
	if (Slot.Phase != EPhase::Reserved)
		Slot.Phase = EPhase::Closing;
}
void FNativeClientSlots::Abandon(const FNativeClientRegistration& Registration)
{
	check(IsInGameThread());
	FSlot* Slot = Find(Registration);
	check(Slot && Slot->Phase == EPhase::Reserved);
	if (!Slot)
		return;
	Close(*Slot);
	Slot->Phase = EPhase::Closing;
	TryReleaseQuiescent(Registration); // no registration/queue side effects were published by this reservation
}
bool FNativeClientSlots::BeginRetirement(const FNativeClientRegistration& Registration)
{
	check(IsInGameThread());
	FSlot* Slot = Find(Registration);
	if (!Slot)
		return false;
	Close(*Slot);
	return true;
}
bool FNativeClientSlots::ReserveQueueBinding(const FNativeClientRegistration& Registration,
	FNativeQueueBindingReservation& Out, FName& Error, bool bForSwitch)
{
	check(IsInGameThread());
	Error = NAME_None;
	FSlot* Slot = Find(Registration);
	if (!Slot || Out.IsValid() || bStopping || Registration.Control->IsStopped() || Slot->Gate->IsClosed() || !Slot->Scope->AcceptsClientsOnGT())
	{
		Error = TEXT("LifecycleClosed");
		return false;
	}
	if (bForSwitch)
		for (const auto& Row : Slot->Drains)
			if (Row.Phase != EDrainPhase::Empty && Row.Phase != EDrainPhase::Active && Row.Phase != EDrainPhase::Exhausted)
			{
				Error = TEXT("LifecycleBusy");
				return false;
			} // exactly one switch, including a paused complete B target
	for (uint8 Pass = 0; Pass < 2; ++Pass)
		for (uint8 Index = 0; Index < 2; ++Index)
		{
			auto& Row = Slot->Drains[Index];
			const bool bPending = !bForSwitch && Row.Phase == EDrainPhase::Pending;
			if (Pass == 0 ? !bPending : Row.Phase != EDrainPhase::Empty)
				continue;
			check(Row.Native.Outstanding.load(std::memory_order_acquire) == 0 && !Row.Queue && !Row.Binding);
			const uint64 Previous = Row.Native.Generation.load(std::memory_order_relaxed);
			if (!bPending && Previous == MAX_uint64)
			{
				Row.Phase = EDrainPhase::Exhausted;
				continue;
			}
			const uint64 Generation = bPending ? Previous : Previous + 1;
			Row.Native.Result.store(EQueueFenceResult::Pending, std::memory_order_relaxed);
			Row.Native.ClientGeneration.store(Slot->Generation, std::memory_order_relaxed);
			Row.Native.Generation.store(Generation, std::memory_order_release);
			Row.bFlushAttempted = false;
			Row.bFlushAdmitted = false;
			Row.bDrainAcknowledged = false;
			Row.Flush = EQueueFenceResult::Pending;
			Row.Through = 0;
			Row.Phase = EDrainPhase::Reserved;
			Out.Slots = this;
			Out.Control = Registration.Control;
			Out.ClientIndex = Registration.Client.Index;
			Out.ClientGeneration = Slot->Generation;
			Out.Generation = Generation;
			Out.DrainIndex = Index;
			Out.bResumePending = bPending;
			return true;
		}
	Error = TEXT("LifecycleBusy");
	return false; // no third row or append-only switch history
}
bool FNativeClientSlots::CommitQueueBinding(const FNativeQueueBindingReservation& Reservation,
	TSharedRef<FOfflineQueue, ESPMode::ThreadSafe> Queue, TSharedRef<FQueueBinding, ESPMode::ThreadSafe> Binding)
{
	check(IsInGameThread());
	if (Reservation.Control != Owner.Pin() || Reservation.ClientIndex >= Capacity || Reservation.DrainIndex >= 2)
		return false;
	auto& Slot = Slots[Reservation.ClientIndex];
	auto& Row = Slot.Drains[Reservation.DrainIndex];
	if (bStopping || Reservation.Control->IsStopped() || Slot.Generation != Reservation.ClientGeneration ||
		Slot.Phase == EPhase::Free || Slot.Phase == EPhase::Exhausted || Slot.Gate->IsClosed() ||
		Row.Phase != EDrainPhase::Reserved || Row.Native.Generation.load(std::memory_order_acquire) != Reservation.Generation)
		return false;
	Row.Queue = MoveTemp(Queue);
	Row.Binding = MoveTemp(Binding);
	Row.Phase = EDrainPhase::Active;
	return true;
}
bool FNativeClientSlots::ParkQueueBinding(const FNativeQueueBindingReservation& Reservation)
{
	check(IsInGameThread());
	if (Reservation.Control != Owner.Pin() || Reservation.ClientIndex >= Capacity || Reservation.DrainIndex >= 2)
		return false;
	auto& Slot = Slots[Reservation.ClientIndex];
	auto& Row = Slot.Drains[Reservation.DrainIndex];
	if (bStopping || Reservation.Control->IsStopped() || Slot.Gate->IsClosed() ||
		Slot.Generation != Reservation.ClientGeneration || Row.Phase != EDrainPhase::Reserved ||
		Row.Native.Generation.load(std::memory_order_acquire) != Reservation.Generation)
		return false;
	Row.Phase = EDrainPhase::Pending;
	return true;
}
void FNativeClientSlots::DiscardPendingQueueTarget(const FNativeClientRegistration& Registration)
{
	check(IsInGameThread());
	const auto Kept = Registration.Control;
	if (auto* Slot = Find(Registration))
		for (auto& Row : Slot->Drains)
			if (Row.Phase == EDrainPhase::Pending)
				Row.Phase = Row.Native.Generation.load(std::memory_order_relaxed) == MAX_uint64 ? EDrainPhase::Exhausted : EDrainPhase::Empty;
}
void FNativeClientSlots::AbortQueueBinding(const FNativeQueueBindingReservation& Reservation)
{
	check(IsInGameThread());
	check(Reservation.Control == Owner.Pin() && Reservation.ClientIndex < Capacity && Reservation.DrainIndex < 2);
	auto& Slot = Slots[Reservation.ClientIndex];
	auto& Row = Slot.Drains[Reservation.DrainIndex];
	check(Slot.Generation == Reservation.ClientGeneration && Row.Phase == EDrainPhase::Reserved &&
		  Row.Native.Generation.load(std::memory_order_acquire) == Reservation.Generation && Row.Native.Outstanding.load(std::memory_order_acquire) == 0);
	Row.Phase = Reservation.bResumePending && !bStopping && !Reservation.Control->IsStopped() && !Slot.Gate->IsClosed()
					? EDrainPhase::Pending
					: (Reservation.Generation == MAX_uint64 ? EDrainPhase::Exhausted : EDrainPhase::Empty);
	TryReclaim(Slot);
}
void FNativeClientSlots::BeginQueueDrain(FQueueDrain& Row, bool bRevoke)
{
	if (Row.Phase == EDrainPhase::Pending)
	{
		// A paused target owns no queue/IO/callback; retirement can discard its fixed native reservation directly.
		Row.Phase = Row.Native.Generation.load(std::memory_order_relaxed) == MAX_uint64 ? EDrainPhase::Exhausted : EDrainPhase::Empty;
		return;
	}
	if (Row.Phase != EDrainPhase::Active)
		return;
	Row.Phase = EDrainPhase::Detaching; // no sweep/reuse until the actual detach call stack and watermark selection return
	const auto Queue = Row.Queue;
	const auto Binding = Row.Binding;
	Queue->AdmitOwnedDrainOnGT(); // before removal can expose an otherwise idle last-member namespace to a reentrant Join
	if (bRevoke)
		Binding->RevokeOnGT();
	else
		Binding->CloseCaptureAdmission(); // ordinary complete legacy snapshots remain authoritative
	Queue->Leave(Binding, Owner.Pin());
	Row.Through = Queue->AdmissionWatermarkOnGT();
	Row.Phase = EDrainPhase::Draining;
}
void FNativeClientSlots::DetachQueueBinding(const FNativeClientRegistration& Registration,
	const TSharedPtr<FQueueBinding, ESPMode::ThreadSafe>& Binding, bool bRevoke)
{
	check(IsInGameThread());
	const auto Kept = Registration.Control;
	FSlot* Slot = Find(Registration);
	if (!Slot || !Binding)
		return;
	for (auto& Row : Slot->Drains)
		if (Row.Binding == Binding)
		{
			BeginQueueDrain(Row, bRevoke);
			return;
		}
}
void FNativeClientSlots::DetachQueuesForRetirement(const FNativeClientRegistration& Registration)
{
	check(IsInGameThread());
	const auto Kept = Registration.Control;
	FSlot* Slot = Find(Registration);
	if (!Slot)
		return;
	for (auto& Row : Slot->Drains)
		BeginQueueDrain(Row);
}
void FNativeClientSlots::DetachAllQueuesForStop()
{
	check(IsInGameThread() && bStopping);
	const auto Kept = Owner.Pin();
	for (uint32 Index = 0; Index < Capacity; ++Index)
		for (auto& Row : Slots[Index].Drains)
			BeginQueueDrain(Row);
}
void FNativeClientSlots::RequestLimiterPump(const FNativeClientRegistration& Registration)
{
	check(IsInGameThread());
	FSlot* Slot = Find(Registration);
	if (Slot && !bStopping && !Registration.Control->IsStopped() && !Slot->Gate->IsClosed())
		Slot->bLimiterPumpPending = true;
}
void FNativeClientSlots::RetireClosingClientsOnGT()
{
	check(IsInGameThread());
	const auto Kept = Owner.Pin();
	if (!Kept || Kept->IsStopped())
		return;
	for (uint32 Index = 0; Index < Capacity && !Kept->IsStopped(); ++Index)
	{
		auto& Slot = Slots[Index];
		if (Slot.Phase != EPhase::Closing)
			continue;
		const uint64 Generation=Slot.Generation;
		const auto Client=Slot.Client.Pin();
		if (Client)
			Client->BeginPermanentRetirement();
		if (Kept->IsStopped() || Slot.Generation!=Generation || Slot.Phase!=EPhase::Closing)
			continue;
		if (Slot.RefreshDelivery)
			Slot.RefreshDelivery->RetireOnGT(); // actual owned GT destruction, even after reentrant Stop
		if (!Kept->IsStopped() && Client && Slot.Generation==Generation && Slot.Phase==EPhase::Closing)
			Client->TryFinishPermanentRetirementOnGT(); // possible Closed sink/reuse; no later access to this slot
	}
}
FManagedRefreshDelivery* FNativeClientSlots::RefreshDeliveryOnGT(const FNativeClientRegistration& Registration) const
{
	check(IsInGameThread());
	const auto* Slot = Find(Registration);
	return Slot ? Slot->RefreshDelivery.Get() : nullptr;
}
void FNativeClientSlots::DropRefreshCallbacksAtStopOnGT()
{
	check(IsInGameThread() && bStopping);
	const auto Kept = Owner.Pin();
	for (uint32 Index = 0; Index < Capacity; ++Index)
		if (Slots[Index].Phase != EPhase::Free && Slots[Index].Phase != EPhase::Exhausted && Slots[Index].RefreshDelivery)
			Slots[Index].RefreshDelivery->RetireOnGT(); // only captured-allocation GT payload destruction; no normal service
}

void FNativeClientSlots::SweepQueueDrains(bool bAllowStoppingFlush)
{
	check(IsInGameThread());
	const auto Kept = Owner.Pin();
	if (!Kept || Kept->IsStopped())
		return;
	// Reentrant destruction uses the same scan guard as operation reclamation: no in-progress row can be recycled.
	if (bSweeping)
		return;
	TGuardValue<bool> Sweeping(bSweeping, true);
	for (uint32 Index = 0; Index < Capacity && !Kept->IsStopped(); ++Index)
	{
		auto& Slot = Slots[Index];
		if (Slot.Phase != EPhase::Free && Slot.Phase != EPhase::Exhausted && Slot.RefreshDelivery)
			Slot.RefreshDelivery->ServiceOnGT();
		if (Kept->IsStopped())
			return;
		for (auto& Row : Slot.Drains)
		{
			if (Row.Phase != EDrainPhase::Draining)
				continue;
			auto Queue = Row.Queue;
			auto Binding = Row.Binding;
			if (!Queue->MemberDrainReadyOnGT(Binding, Row.Through))
				continue;
			if (!Row.bFlushAttempted)
			{
				Row.bFlushAttempted = true;
				FNativeQueueFenceToken Token(Kept.ToSharedRef(), Row.Native,
					Row.Native.Generation.load(std::memory_order_acquire), Slot.Generation);
				Row.bFlushAdmitted = (!bStopping || bAllowStoppingFlush) && Queue->FlushOwned(Binding, Row.Through, MoveTemp(Token));
				// A refusal retains no waiter. Its local native token ends BEFORE any row can be reused.
			}
			if (Row.Native.Outstanding.load(std::memory_order_acquire) != 0)
				continue;
			Row.Flush = Row.bFlushAdmitted ? Row.Native.Result.load(std::memory_order_acquire) : EQueueFenceResult::NotAdmitted;
			if (Row.Flush == EQueueFenceResult::Pending)
				continue;
			if (Row.Flush == EQueueFenceResult::Flushed && Queue->HasStorageFailureOnGT())
				Row.Flush = EQueueFenceResult::Failed;
			if (!Queue->MemberDrainReadyOnGT(Binding, Row.Through))
				continue;
			if (!Row.bDrainAcknowledged)
			{
				Row.bDrainAcknowledged = true;
				Queue->AcknowledgeOwnedDrainOnGT();
			}
			if (!Queue->TryFenceNamespaceOnGT())
				continue;
			if (Kept->IsStopped())
				return;
			Row.Phase = EDrainPhase::Cleaning;
			Row.Binding.Reset();
			Row.Queue.Reset();
			Binding.Reset();
			Queue.Reset(); // actual affine destruction/callouts while Cleaning still bars reuse
			Row.Phase = Row.Native.Generation.load(std::memory_order_relaxed) == MAX_uint64 ? EDrainPhase::Exhausted : EDrainPhase::Empty;
			if (Kept->IsStopped())
				return;
		}
		if (!bStopping && !Kept->IsStopped() && Slot.Phase == EPhase::Live && !Slot.Gate->IsClosed())
		{
			const auto Client = Slot.Client.Pin();
			if (Client && Slot.bLimiterPumpPending)
			{
				Slot.bLimiterPumpPending = false; // before grants can reenter setters/retirement
				Client->GetLimiter().Pump();
			}
			if (Client && !bStopping && !Kept->IsStopped() && !Slot.Gate->IsClosed())
			{
				bool bActive = false, bEmpty = false;
				for (const auto& Row : Slot.Drains)
				{
					bActive |= Row.Phase == EDrainPhase::Active || Row.Phase == EDrainPhase::Reserved;
					bEmpty |= Row.Phase == EDrainPhase::Empty || Row.Phase == EDrainPhase::Pending;
				}
				const auto Current = Client->GetConfig();
				if (!bActive && bEmpty && Current->bQueueActivationRequested && !Current->QueueName.IsEmpty() &&
					(Current->QueueConfigError == TEXT("QueueBusy") || Current->QueueConfigError == TEXT("LifecycleBusy")))
					Client->RefreshQueueRegistration(); // one current validated target; no retired-target/task history
			}
		}
		if (Kept->IsStopped())
			return;
		TryReclaim(Slot);
	}
}
bool FNativeClientSlots::TryReleaseQuiescent(const FNativeClientRegistration& Registration)
{
	check(IsInGameThread());
	FSlot* Slot = Find(Registration);
	if (!Slot || Slot->Phase != EPhase::Closing)
		return false;
	Slot->bNonOperationQuiescent = true;
	return TryReclaim(*Slot);
}
bool FNativeClientSlots::TryReclaim(FSlot& Slot)
{
	if (Slot.Phase != EPhase::Closing || !Slot.bNonOperationQuiescent || !Slot.Gate->IsClosed() ||
		Slot.Gate->NumOwnedOperations() != 0 || Slot.Gate->NumOwnedInputs() != 0 || Slot.Operations->NumOccupied() != 0)
		return false;
	if (Slot.RefreshDelivery && !Slot.RefreshDelivery->IsQuiescentOnGT())
		return false;
	for (const auto& Row : Slot.Drains)
		if (Row.Phase != EDrainPhase::Empty && Row.Phase != EDrainPhase::Exhausted)
			return false;
	Slot.Client.Reset(); // weak only; external inert aliases do not retain a registry slot
	auto ReleasedScope = MoveTemp(Slot.Scope);
	Slot.bCloseRequested = false;
	Slot.bNonOperationQuiescent = false;
	check(Occupied != 0);
	--Occupied;
	if (Slot.Generation == MAX_uint64 || Slot.Drains[0].Phase == EDrainPhase::Exhausted || Slot.Drains[1].Phase == EDrainPhase::Exhausted)
		Slot.Phase = EPhase::Exhausted;
	else
	{
		++Slot.Generation;
		Slot.Phase = EPhase::Free;
	}
	const auto Kept = Owner.Pin();
	ReleasedScope->ReleaseClientOnGT(Kept && !Kept->IsStopped()); // all slot mutation precedes a possible Closed trace callout
	return true;
}
TSharedPtr<FClientState, ESPMode::ThreadSafe> FNativeClientSlots::PinClient(const FNativeClientRegistration& Registration) const
{
	check(IsInGameThread());
	const FSlot* Slot = Find(Registration);
	return Slot ? Slot->Client.Pin() : nullptr;
}
FNativeClientSweepResult FNativeClientSlots::SweepOperations(uint32 Budget)
{
	check(IsInGameThread());
	FNativeClientSweepResult Result;
	if (bSweeping)
		return Result;
	auto Kept = Owner.Pin();
	check(Kept.IsValid());
	bSweeping = true;
	uint32 Remaining = FMath::Min(Budget, uint32(256));
	for (uint32 Step = 0; Step < Capacity && Remaining != 0; ++Step)
	{
		FSlot& Slot = Slots[NextOperationClient];
		NextOperationClient = (NextOperationClient + 1) % Capacity;
		++Result.ClientsInspected;
		if (Slot.Operations && Slot.Operations->NumOccupied() != 0)
		{
			const auto Cells = Slot.Operations->SweepOnGT(Remaining);
			Result.OperationCellsInspected += Cells.Visited;
			Result.OperationCellsReclaimed += Cells.Reclaimed;
			Remaining -= Cells.Visited;
		}
		TryReclaim(Slot); // the exact-generation owner recorded its non-operation barrier; no fence is invented here
	}
	bSweeping = false;
	return Result;
}
void FNativeClientSlots::BeginStop()
{
	check(IsInGameThread());
	bStopping = true;
	for (uint32 Index = 0; Index < Capacity; ++Index)
	{
		FSlot& Slot = Slots[Index];
		if (Slot.Phase == EPhase::Reserved || Slot.Phase == EPhase::Live || Slot.Phase == EPhase::Closing)
			Close(Slot);
	}
}
#if WITH_DEV_AUTOMATION_TESTS
void FNativeClientSlots::SeedFreeGenerationsForTests(uint64 Generation)
{
	check(IsInGameThread() && Occupied == 0 && Generation != 0);
	for (uint32 Index = 0; Index < Capacity; ++Index)
	{
		check(Slots[Index].Phase == EPhase::Free);
		Slots[Index].Generation = Generation;
	}
}
#endif
} // namespace VaRest::Http
