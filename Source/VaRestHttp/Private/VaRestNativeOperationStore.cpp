// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeOperationStore.h"
#include "VaRestExchangeState.h"
#include "VaRestManagedCompletion.h"
#include "VaRestManagedOperation.h"

namespace VaRest::Http
{
FNativeLogicalTailLease::~FNativeLogicalTailLease()
{
	Reset();
}
FNativeLogicalTailLease::FNativeLogicalTailLease(FNativeLogicalTailLease&& Other) noexcept
	: Ref(MoveTemp(Other.Ref))
{
	Other.Ref = {};
}
FNativeLogicalTailLease& FNativeLogicalTailLease::operator=(FNativeLogicalTailLease&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Ref = MoveTemp(Other.Ref);
		Other.Ref = {};
	}
	return *this;
}
FManagedOperationPayload* FNativeLogicalTailLease::ManagedPayloadOnGT() const
{
	check(IsInGameThread() && IsHeld());
	check(Ref.Store->Matches(Ref));
	auto* Cell = Ref.Store->FindPublished(Ref.Cell);
	check(Cell); // this held authority prevents reclamation of the exact generation throughout the caller's use
	return Cell ? Cell->ManagedPayload : nullptr;
}
void FNativeLogicalTailLease::Reset()
{
	FNativeOperationRef Releasing = MoveTemp(Ref);
	Ref = {};
	if (Releasing.IsValid())
		Releasing.Store->SettleLogical(Releasing.Cell, Releasing.Control->IsStopped());
}

FNativeDispatchSettlementLease::~FNativeDispatchSettlementLease()
{
	Reset();
}
FNativeDispatchSettlementLease::FNativeDispatchSettlementLease(FNativeDispatchSettlementLease&& Other) noexcept
	: Ref(MoveTemp(Other.Ref))
{
	Other.Ref = {};
}
FNativeDispatchSettlementLease& FNativeDispatchSettlementLease::operator=(FNativeDispatchSettlementLease&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Ref = MoveTemp(Other.Ref);
		Other.Ref = {};
	}
	return *this;
}
void FNativeDispatchSettlementLease::Reset()
{
	FNativeOperationRef Releasing = MoveTemp(Ref);
	Ref = {};
	if (Releasing.IsValid())
		Releasing.Store->SettlePhysical(Releasing.Cell, Releasing.Control->IsStopped());
}

FNativeOperationReservation::~FNativeOperationReservation()
{
	Abort();
}
FNativeOperationReservation::FNativeOperationReservation(FNativeOperationReservation&& Other) noexcept
	: Ref(MoveTemp(Other.Ref))
	, Sender(MoveTemp(Other.Sender))
{
	Other.Ref = {};
}
FNativeOperationReservation& FNativeOperationReservation::operator=(FNativeOperationReservation&& Other) noexcept
{
	if (this != &Other)
	{
		Abort();
		Ref = MoveTemp(Other.Ref);
		Sender = MoveTemp(Other.Sender);
		Other.Ref = {};
	}
	return *this;
}
void FNativeOperationReservation::SetReleaseAnchor(TUniqueFunction<void(bool)>&& Anchor)
{
	check(IsValid() && Anchor);
	auto& Cell = Ref.Store->OwnClaim(Ref.Cell);
	check(!Cell.ReleaseAnchor);
	Cell.ReleaseAnchor = MoveTemp(Anchor);
}
FManagedOperationPayload* FNativeOperationReservation::CreateManagedPayload()
{
	check(IsValid() && !Ref.Store->OwnClaim(Ref.Cell).ReleaseAnchor);
	auto Input = ForkInputAnchor();
	if (!Input.IsValid())
		return nullptr;
	TUniquePtr<FManagedOperationPayload> Owned(new FManagedOperationPayload(Ref, MoveTemp(Input)));
	auto* Payload = Owned.Get();
	SetManagedPayload(MoveTemp(Owned));
	return Payload;
}
void FNativeOperationReservation::SetManagedPayload(TUniquePtr<FManagedOperationPayload>&& Payload)
{
	check(IsValid() && Payload && Payload->MatchesOperation(Ref));
	auto& Cell = Ref.Store->OwnClaim(Ref.Cell);
	check(!Cell.ReleaseAnchor && !Cell.ManagedPayload);
	Cell.ManagedPayload = Payload.Get();
	Cell.ReleaseAnchor = [Owned = MoveTemp(Payload)](bool bDiscarded) mutable {
		Owned->DestroyOnGT(bDiscarded);
		Owned.Reset();
	};
}
FNativeLogicalTailLease FNativeOperationReservation::Publish()
{
	check(IsValid());
	check(Ref.Store->OwnClaim(Ref.Cell).InitialTails == 1);
	return PublishInitial(nullptr, nullptr);
}
FNativeLogicalTailLease FNativeOperationReservation::Publish(FNativeLogicalTailLease& OutCompletion, FNativeLogicalTailLease& OutDeliveryWorker)
{
	check(IsValid() && !OutCompletion.IsHeld() && !OutDeliveryWorker.IsHeld());
	if (&OutCompletion == &OutDeliveryWorker)
		return {}; // no Ready/publication/move: the caller may retry distinct outputs or abort the still-owned claim
	const uint32 InitialTails = Ref.Store->OwnClaim(Ref.Cell).InitialTails;
	check(InitialTails == 2 || InitialTails == 3);
	return PublishInitial(&OutCompletion, &OutDeliveryWorker);
}
FNativeLogicalTailLease FNativeOperationReservation::PublishAdmitted(FNativeLogicalTailLease& OutCompletion, FNativeLogicalTailLease& OutDeliveryWorker)
{
	check(IsValid() && !OutCompletion.IsHeld() && !OutDeliveryWorker.IsHeld());
	if (&OutCompletion == &OutDeliveryWorker)
		return {};
	const auto& Cell = Ref.Store->OwnClaim(Ref.Cell);
	check(Cell.ReleaseAnchor && (Cell.InitialTails == 2 || Cell.InitialTails == 3));
	const uint32 InitialTails = Cell.InitialTails;
	// No count/cell/child is acquired here. A live closed client still owes its admitted request the canceled GT path.
	// A stopped sender receives only its EXISTING native authorities, which it drops without posting Enter.
	Sender.PublishReady();
	Ref.Store->PublishClaim(Ref.Cell);
	Sender.Reset();
	OutCompletion = FNativeLogicalTailLease(Ref);
	if (InitialTails == 3)
		OutDeliveryWorker = FNativeLogicalTailLease(Ref);
	FNativeLogicalTailLease Handoff(MoveTemp(Ref));
	Ref = {};
	return Handoff;
}
FNativeLogicalTailLease FNativeOperationReservation::PublishManaged()
{
	check(IsValid());
	const auto& Cell = Ref.Store->OwnClaim(Ref.Cell);
	auto* Payload = Cell.ManagedPayload;
	check(Payload && Payload->Exchange && Cell.ReleaseAnchor && (Cell.InitialTails == 2 || Cell.InitialTails == 3));
	check(!Payload->ParkedNativeCompletion && !Payload->ParkedCompletion.IsHeld() && !Payload->ParkedDeliveryWorker.IsHeld());
	// No callout/fallible return follows installation until publication. These are the already-reserved initial tails,
	// not new children. The immutable weak Ex route is complete BEFORE GTK can inspect this genuine published row.
	Payload->ParkedNativeCompletion = FManagedCompletionAccess::CreateForClaim(*Payload, FNativeLogicalTailLease(Ref));
	if (Cell.InitialTails == 3)
		Payload->ParkedDeliveryWorker = FNativeLogicalTailLease(Ref);
	Internals(*Payload->Exchange).NativeCompletion = Payload->ParkedNativeCompletion;
	Sender.PublishReady();
	Ref.Store->PublishClaim(Ref.Cell);
	Sender.Reset();
	FNativeLogicalTailLease Handoff(MoveTemp(Ref));
	Ref = {};
	return Handoff;
}
FNativeLogicalTailLease FNativeOperationReservation::PublishInitial(FNativeLogicalTailLease* OutCompletion, FNativeLogicalTailLease* OutDeliveryWorker)
{
	const auto& Cell = Ref.Store->OwnClaim(Ref.Cell);
	check(Cell.ReleaseAnchor);
	const uint32 InitialTails = Cell.InitialTails;
	if (!Sender.PublishReady() || Ref.Store->bSealed.load(std::memory_order_acquire))
	{
		Abort();
		return {};
	}
	Ref.Store->PublishClaim(Ref.Cell);
	Sender.Reset();
	if (OutCompletion)
		*OutCompletion = FNativeLogicalTailLease(Ref);
	if (OutDeliveryWorker && InitialTails == 3)
		*OutDeliveryWorker = FNativeLogicalTailLease(Ref);
	FNativeLogicalTailLease Initial(MoveTemp(Ref));
	Ref = {};
	return Initial;
}
void FNativeOperationReservation::Abort()
{
	FNativeOperationRef Releasing = MoveTemp(Ref);
	Ref = {};
	Sender.Reset(); // unpublished input becomes Ready/discarded BEFORE a GT reader can reclaim its cell anchor
	if (Releasing.IsValid())
		Releasing.Store->AbortClaim(Releasing.Cell, Releasing.Control->IsStopped());
}

FNativeOperationStore::FNativeOperationStore(TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> InOwner,
	TSharedRef<FClientAdmissionGate, ESPMode::ThreadSafe> InGate)
	: Owner(InOwner)
	, Gate(InGate)
{
	check(IsInGameThread());
	check(!Gate->IsClosed() && Gate->NumOwnedOperations() == 0);
	Cells = MakeUnique<FCell[]>(Capacity);
}
FNativeOperationStore::~FNativeOperationStore()
{
	check(NumOccupied() == 0);
}
bool FNativeOperationStore::Matches(const FNativeOperationRef& Ref) const
{
	return Ref.Store == this && Ref.Control.IsValid() && Ref.Control == Owner.Pin();
}
bool FNativeOperationStore::TryClaim(FClientWorkLease& Work, FNativeInputBorrow& Sender, FNativeOperationReservation& Out, uint32 InitialTails)
{
	if (InitialTails == 0 || InitialTails > 3 || !Work.IsHeld() || !Work.BelongsTo(Gate.Get()) || !Sender.IsValid() || Out.IsValid() || bSealed.load(std::memory_order_acquire))
		return false;
	auto Kept = Owner.Pin();
	if (!Kept || Kept->IsStopped() || !Sender.IsUnpublishedSenderFor(Kept.Get()))
		return false;
	const uint32 Start = NextClaim.fetch_add(1, std::memory_order_relaxed) % Capacity;
	for (uint32 Offset = 0; Offset < Capacity; ++Offset)
	{
		const uint32 Index = (Start + Offset) % Capacity;
		FCell& Cell = Cells[Index];
		uint64 Before = Cell.State.load(std::memory_order_acquire);
		if ((Before & 3) != uint64(ECellState::Free))
			continue;
		const uint64 Generation = Before >> 2;
		if (!Cell.State.compare_exchange_strong(Before, Tag(Generation, ECellState::Claimed), std::memory_order_acq_rel, std::memory_order_acquire))
			continue;
		Occupied.fetch_add(1, std::memory_order_acq_rel);
		Cell.Settlement.emplace(MoveTemp(Work), InitialTails);
		Cell.Authorities.store(InitialTails, std::memory_order_relaxed);
		Cell.InitialTails = InitialTails;
		Cell.bAborted = false;
		Out.Ref = {MoveTemp(Kept), this, {static_cast<uint16>(Index), Generation}};
		Out.Sender = MoveTemp(Sender);
		return true;
	}
	return false;
}

FNativeOperationStore::FCell& FNativeOperationStore::OwnClaim(FNativeOperationHandle Handle) const
{
	check(Handle.Index < Capacity && Handle.Generation != 0 && Handle.Generation <= MaxGeneration);
	FCell& Cell = Cells[Handle.Index];
	check(Cell.State.load(std::memory_order_acquire) == Tag(Handle.Generation, ECellState::Claimed));
	return Cell;
}
FNativeOperationStore::FCell* FNativeOperationStore::FindPublished(FNativeOperationHandle Handle) const
{
	if (!Handle.IsValid() || Handle.Generation > MaxGeneration || Handle.Index >= Capacity)
		return nullptr;
	FCell& Cell = Cells[Handle.Index];
	return Cell.State.load(std::memory_order_acquire) == Tag(Handle.Generation, ECellState::Published) ? &Cell : nullptr;
}
void FNativeOperationStore::PublishClaim(FNativeOperationHandle Handle)
{
	FCell& Cell = OwnClaim(Handle);
	Cell.State.store(Tag(Handle.Generation, ECellState::Published), std::memory_order_release);
}
void FNativeOperationStore::AbortClaim(FNativeOperationHandle Handle, bool Stopped)
{
	FCell& Cell = OwnClaim(Handle);
	const uint32 InitialTails = Cell.InitialTails;
	Cell.bAborted = true;
	if (Stopped)
		Cell.Settlement->MarkDiscardedAtStop(); // Claimed has never admitted/exposed a GT child
	PublishClaim(Handle);
	for (uint32 Index = 0; Index < InitialTails; ++Index)
		SettleLogical(Handle, Stopped); // last iteration may permit GT reclamation; no cell access follows
}

FNativeLogicalTailLease FNativeOperationStore::TryAddLogicalTailOnGT(const FNativeOperationRef& Ref)
{
	check(IsInGameThread());
	if (!Matches(Ref) || bSealed.load(std::memory_order_acquire))
		return {};
	FCell* Cell = FindPublished(Ref.Cell);
	if (!Cell || !Cell->Settlement->TryAddLogicalTail())
		return {};
	Cell->Authorities.fetch_add(1, std::memory_order_acq_rel);
	return FNativeLogicalTailLease(Ref);
}
FNativeDispatchSettlementLease FNativeOperationStore::TryAddDispatchOnGT(const FNativeOperationRef& Ref)
{
	check(IsInGameThread());
	if (!Matches(Ref) || bSealed.load(std::memory_order_acquire))
		return {};
	FCell* Cell = FindPublished(Ref.Cell);
	if (!Cell || !Cell->Settlement->TryAddDispatchSettlement())
		return {};
	Cell->Authorities.fetch_add(1, std::memory_order_acq_rel);
	return FNativeDispatchSettlementLease(Ref);
}
bool FNativeOperationStore::TakeInitialCompletionOnGT(const FNativeOperationRef& Ref,
	FNativeLogicalTailLease& Completion, FNativeLogicalTailLease& DeliveryWorker)
{
	check(IsInGameThread() && !Completion.IsHeld() && !DeliveryWorker.IsHeld());
	if (&Completion == &DeliveryWorker || !Matches(Ref))
		return false;
	auto* Cell = FindPublished(Ref.Cell);
	if (!Cell || !Cell->ManagedPayload)
		return false;
	auto& Payload = *Cell->ManagedPayload;
	if (Payload.bCompletionSelected || !Payload.ParkedCompletion.IsHeld())
		return false;
	// GT is the sole reclaim owner. No callout occurs while finding/transferring this EXISTING authority; the parked
	// initial lease protects the row until it is moved into the caller. This works even when all64 tails are occupied.
	Payload.bCompletionSelected = true;
	Completion = MoveTemp(Payload.ParkedCompletion);
	DeliveryWorker = MoveTemp(Payload.ParkedDeliveryWorker);
	return true;
}
bool FNativeOperationStore::TakeManagedCompletionOnGT(const FNativeOperationRef& Ref,
	TSharedPtr<FManagedCompletionAccess, ESPMode::ThreadSafe>& Completion, FNativeLogicalTailLease& DeliveryWorker)
{
	check(IsInGameThread() && !Completion && !DeliveryWorker.IsHeld());
	auto* Cell = Matches(Ref) ? FindPublished(Ref.Cell) : nullptr;
	if (!Cell || !Cell->ManagedPayload)
		return false;
	auto& Payload = *Cell->ManagedPayload;
	if (Payload.bCompletionSelected || !Payload.ParkedNativeCompletion)
		return false;
	Payload.bCompletionSelected = true;
	Completion = MoveTemp(Payload.ParkedNativeCompletion); // SAME shared native initial authority, including any already-posted cancel
	DeliveryWorker = MoveTemp(Payload.ParkedDeliveryWorker);
	return true;
}
bool FNativeOperationStore::CloseChildrenOnGT(const FNativeOperationRef& Ref)
{
	check(IsInGameThread());
	FCell* Cell = Matches(Ref) ? FindPublished(Ref.Cell) : nullptr;
	if (!Cell)
		return false;
	Cell->Settlement->CloseChildAdmission();
	return true;
}
bool FNativeOperationStore::MarkTerminalOnGT(const FNativeOperationRef& Ref)
{
	check(IsInGameThread());
	FCell* Cell = Matches(Ref) ? FindPublished(Ref.Cell) : nullptr;
	if (!Cell)
		return false;
	Cell->Settlement->CloseChildAdmission();
	Cell->Settlement->MarkLogicalTerminalSettled();
	return true;
}
void FNativeOperationStore::SettleLogical(FNativeOperationHandle Handle, bool Stopped)
{
	FCell* Cell = FindPublished(Handle); // caller owns a real authority until the LAST cell access below
	check(Cell);
	if (!Cell)
		return;
	if (Stopped && bSealed.load(std::memory_order_acquire))
		Cell->Settlement->MarkDiscardedAtStop();
	Cell->Settlement->SettleLogicalTail();
	const uint64 Before = Cell->Authorities.fetch_sub(1, std::memory_order_release);
	check(Before != 0); // no cell access after fetch_sub; GT may now reclaim/reuse it
}
void FNativeOperationStore::SettlePhysical(FNativeOperationHandle Handle, bool Stopped)
{
	FCell* Cell = FindPublished(Handle);
	check(Cell);
	if (!Cell)
		return;
	if (Stopped && bSealed.load(std::memory_order_acquire))
		Cell->Settlement->MarkDiscardedAtStop();
	Cell->Settlement->SettleDispatch();
	const uint64 Before = Cell->Authorities.fetch_sub(1, std::memory_order_release);
	check(Before != 0); // last cell access, including the release-claim winner's entire method
}
void FNativeOperationStore::SealOnGT()
{
	check(IsInGameThread());
	bSealed.store(true, std::memory_order_release);
}
bool FNativeOperationStore::ResetForNextClientOnGT(TSharedRef<FClientAdmissionGate, ESPMode::ThreadSafe> NewGate)
{
	check(IsInGameThread());
	auto Kept = Owner.Pin();
	if (NumOccupied() != 0 || !Gate->IsClosed() || Gate->NumOwnedOperations() != 0 || !Kept || Kept->IsStopped() || NewGate->IsClosed() || NewGate->NumOwnedOperations() != 0)
		return false;
	Gate = MoveTemp(NewGate);
	NextClaim.store(0, std::memory_order_relaxed);
	SweepCursor = 0;
	bSealed.store(false, std::memory_order_release);
	return true;
}
FNativeCellSweepResult FNativeOperationStore::SweepOnGT(uint32 Budget)
{
	check(IsInGameThread());
	FNativeCellSweepResult Result;
	if (bSweeping)
		return Result;
	auto Kept = Owner.Pin(); // native Control must own this store allocation through all token authorities
	check(Kept.IsValid());
	bSweeping = true;
	const uint32 Limit = FMath::Min(Budget, Capacity);
	for (uint32 Step = 0; Step < Limit; ++Step)
	{
		FCell& Cell = Cells[SweepCursor];
		SweepCursor = (SweepCursor + 1) % Capacity;
		++Result.Visited;
		uint64 Before = Cell.State.load(std::memory_order_acquire);
		if ((Before & 3) != uint64(ECellState::Published))
			continue; // in particular, GT never reads a Claimed producer's payload
		if (bSealed.load(std::memory_order_acquire) && Kept->IsStopped())
			Cell.Settlement->MarkDiscardedAtStop();
		else if (Cell.bAborted)
		{
			Cell.Settlement->CloseChildAdmission();
			Cell.Settlement->MarkLogicalTerminalSettled();
		}
		if (Cell.ManagedPayload)
			Cell.ManagedPayload->ServiceReadyOnGT(Kept->IsStopped());
		if (Cell.Authorities.load(std::memory_order_acquire) != 0 || !Cell.Settlement->WasWorkReleaseClaimed())
			continue;
		const uint64 Generation = Before >> 2;
		if (!Cell.State.compare_exchange_strong(Before, Tag(Generation, ECellState::Reclaiming), std::memory_order_acq_rel, std::memory_order_acquire))
			continue;
		const bool Discarded = Cell.bAborted || Cell.Settlement->WasDiscardedAtStop();
		Cell.ManagedPayload = nullptr; // clear the servicing pointer before any affine destructor can reenter
		TUniqueFunction<void(bool)> Anchor = MoveTemp(Cell.ReleaseAnchor);
		if (Anchor)
			Anchor(Discarded);
		Anchor = nullptr; // native handler/config/exchange owners must die on GT BEFORE the slot becomes Free
		Cell.Settlement.reset();
		Cell.InitialTails = 0;
		Cell.bAborted = false;
		Occupied.fetch_sub(1, std::memory_order_release);
		// No external callout remains. Publish Free last; a racing claim cannot transiently double-count this cell.
		Cell.State.store(Generation == MaxGeneration ? Tag(Generation, ECellState::Reclaiming) : Tag(Generation + 1, ECellState::Free), std::memory_order_release);
		++Result.Reclaimed;
	}
	bSweeping = false;
	return Result;
}
#if WITH_DEV_AUTOMATION_TESTS
void FNativeOperationStore::SeedFreeGenerationsForTests(uint64 Generation)
{
	check(IsInGameThread() && NumOccupied() == 0 && Gate->NumOwnedOperations() == 0);
	check(Generation != 0 && Generation <= MaxGeneration);
	for (uint32 Index = 0; Index < Capacity; ++Index)
	{
		FCell& Cell = Cells[Index];
		check((Cell.State.load(std::memory_order_acquire) & 3) == uint64(ECellState::Free));
		check(!Cell.Settlement.has_value() && !Cell.ReleaseAnchor);
		Cell.State.store(Tag(Generation, ECellState::Free), std::memory_order_release);
	}
}
#endif
} // namespace VaRest::Http
